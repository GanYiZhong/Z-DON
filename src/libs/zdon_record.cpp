#include "zdon_record.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <print>
#include <spdlog/spdlog.h>
#include <thread>
#include <utility>
#include <vector>

#include "audio/audio.h"
#include "file/config.h"
#include "gfx.h" // IWYU pragma: keep
#include "gfx/sdl_compat.h"
#include "global_data.h"
#include "parsers/song_parser.h"
#include "raylib_ns.h"

// raylib.h (inside namespace ray) already defined RL_MATRIX_TYPE, so rlgl.h declares its functions with this
using ray::Matrix;
#include <rlgl.h>

#ifdef _WIN32
#define ZDON_GLAPI __stdcall
#else
#define ZDON_GLAPI
#endif

namespace fs = std::filesystem;

namespace zdon_record {
    namespace {
        // The window's frame is read into one of kPbo pixel buffers and taken out kPbo - 1 frames later, when
        // the GPU has long finished it (a plain glReadPixels waited for the frame: 6 ms at 1080p); a writer
        // thread feeds ffmpeg (a blocking 8 MB pipe write per frame: 3.5 ms) from a few queued frames.
        constexpr int kPbo = 3;
        constexpr size_t kQueue = 4;

        struct Gl {
            using GenBuffers = void(ZDON_GLAPI*)(int, unsigned int*);
            using DeleteBuffers = void(ZDON_GLAPI*)(int, const unsigned int*);
            using BindBuffer = void(ZDON_GLAPI*)(unsigned int, unsigned int);
            using BufferData = void(ZDON_GLAPI*)(unsigned int, std::ptrdiff_t, const void*, unsigned int);
            using ReadPixels = void(ZDON_GLAPI*)(int, int, int, int, unsigned int, unsigned int, void*);
            using MapBufferRange = void*(ZDON_GLAPI*)(unsigned int, std::ptrdiff_t, std::ptrdiff_t, unsigned int);
            using UnmapBuffer = unsigned char(ZDON_GLAPI*)(unsigned int);
            using BindFramebuffer = void(ZDON_GLAPI*)(unsigned int, unsigned int);
            using BlitFramebuffer = void(ZDON_GLAPI*)(int, int, int, int, int, int, int, int, unsigned int,
                                                      unsigned int);
            static constexpr unsigned int PIXEL_PACK_BUFFER = 0x88EB, STREAM_READ = 0x88E1, RGBA = 0x1908,
                                          UNSIGNED_BYTE = 0x1401, MAP_READ_BIT = 0x0001, READ_FRAMEBUFFER = 0x8CA8,
                                          DRAW_FRAMEBUFFER = 0x8CA9, COLOR_BUFFER_BIT = 0x4000, NEAREST = 0x2600,
                                          LINEAR = 0x2601;
            GenBuffers gen = nullptr;
            DeleteBuffers del = nullptr;
            BindBuffer bind = nullptr;
            BufferData data = nullptr;
            ReadPixels read = nullptr;
            MapBufferRange map = nullptr;
            UnmapBuffer unmap = nullptr;
            BindFramebuffer bind_fb = nullptr;
            BlitFramebuffer blit = nullptr;

            bool load() {
                gen = reinterpret_cast<GenBuffers>(SDL_GL_GetProcAddress("glGenBuffers"));
                del = reinterpret_cast<DeleteBuffers>(SDL_GL_GetProcAddress("glDeleteBuffers"));
                bind = reinterpret_cast<BindBuffer>(SDL_GL_GetProcAddress("glBindBuffer"));
                data = reinterpret_cast<BufferData>(SDL_GL_GetProcAddress("glBufferData"));
                read = reinterpret_cast<ReadPixels>(SDL_GL_GetProcAddress("glReadPixels"));
                map = reinterpret_cast<MapBufferRange>(SDL_GL_GetProcAddress("glMapBufferRange"));
                unmap = reinterpret_cast<UnmapBuffer>(SDL_GL_GetProcAddress("glUnmapBuffer"));
                bind_fb = reinterpret_cast<BindFramebuffer>(SDL_GL_GetProcAddress("glBindFramebuffer"));
                blit = reinterpret_cast<BlitFramebuffer>(SDL_GL_GetProcAddress("glBlitFramebuffer"));
                return gen && del && bind && data && read && map && unmap;
            }
        };

        // The frame as NV12 (BT.709, limited range), made on the GPU: ffmpeg gets 1.5 bytes a pixel instead of
        // 4 and converts nothing (the pipe and its RGBA conversion held the recording back). The target is
        // W/4 x 1.5H RGBA: each texel packs four luma bytes (rows 0..H-1, top of the picture first) or two
        // U,V pairs (rows H.., one per two picture rows), so the read-back bytes are NV12 as they are.
        constexpr const char* kNv12Shader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform ivec2 srcSize;
out vec4 finalColor;
vec3 px(int x, int y) { return texelFetch(texture0, ivec2(x, srcSize.y - 1 - y), 0).rgb; }
float luma(vec3 c) { return (16.0 + 219.0 * dot(c, vec3(0.2126, 0.7152, 0.0722))) / 255.0; }
vec2 chroma(vec3 c) {
    return (128.0 + 224.0 * vec2(dot(c, vec3(-0.1146, -0.3854, 0.5)), dot(c, vec3(0.5, -0.4542, -0.0458)))) / 255.0;
}
void main() {
    ivec2 o = ivec2(gl_FragCoord.xy);
    int x = o.x * 4;
    if (o.y < srcSize.y) {
        finalColor = vec4(luma(px(x, o.y)), luma(px(x + 1, o.y)), luma(px(x + 2, o.y)), luma(px(x + 3, o.y)));
    } else {
        int y = (o.y - srcSize.y) * 2;
        vec3 a = (px(x, y) + px(x + 1, y) + px(x, y + 1) + px(x + 1, y + 1)) * 0.25;
        vec3 b = (px(x + 2, y) + px(x + 3, y) + px(x + 2, y + 1) + px(x + 3, y + 1)) * 0.25;
        finalColor = vec4(chroma(a), chroma(b));
    }
}
)";

        struct State {
            bool active = false;
            fs::path out;
            int fps = 60;
            double result_s = 6.0;

            FILE* video = nullptr; // ffmpeg's stdin
            fs::path video_tmp, audio_tmp;
            FILE* wav = nullptr;
            uint64_t wav_bytes = 0;
            double rate = 44100.0;
            std::vector<float> mix;

            Gl gl;
            bool nv12 = false;      // kNv12Shader (else RGBA, flipped by ffmpeg)
            RenderTarget copy_rt{}; // the window's frame, sampled by the shader
            RenderTarget nv12_rt{}; // W/4 x 1.5H
            ray::Shader shader{};
            size_t frame_bytes = 0;
            std::array<unsigned int, kPbo> pbo{};
            std::deque<int> in_flight; // pbo indices read and not taken out yet, oldest first

            // writer thread: frames to pipe, oldest first, and spent buffers to reuse
            std::thread writer;
            std::mutex m;
            std::condition_variable cv;
            std::deque<std::vector<unsigned char>> queue;
            std::vector<std::vector<unsigned char>> spare;
            bool closing = false;
            bool pipe_failed = false;
            bool stopped = false; // record_picture gave up: the video ends early

            long long frames = 0;
            long long result_frames = 0; // frames on the result screen so far
            bool played = false;         // a game screen has shown
            int width = 0, height = 0;
            std::chrono::steady_clock::time_point started, frame_end;
            double next_log_s = 10.0;
            double ms_game = 0, ms_read = 0, ms_queue = 0, ms_audio = 0; // totals, for the progress log

            fs::path status;                   // --record-status
            double song_pos = 0, song_end = 0; // note_song

            int jobs = kDefaultJobs;               // --record-jobs
            int part = -1;                         // --record-part
            long long range_first = 0;             // --record-range: this part's first frame
            long long range_end = -1;              //   and the frame after its last (-1: the natural end)
            long long recorded = 0;                // frames this process encoded
            std::vector<int> tracks;               // --record-tracks: one difficulty per lane
            std::vector<std::string> track_tokens; // as given (a stacked recording hands them on)
            std::vector<int> track_subdiff;        // the support level of each lane (0: none)
            std::vector<bool> track_yono;          // upstream's earlier simplification
            double sync_end_ms = 0; // --record-sync-end: the song ends no earlier (a lane of a composite)
            bool no_music = false;  // --record-no-music: the hit sounds only (a lane of a composite)
            int band_lanes = 0;     // --record-band: only this lane's band, scaled for a stack of that many
            bool backdrop = false;  // --record-backdrop: and the picture at a fifth of the size below it
            std::string fifo;       // --record-pipe: the raw frames go there (the stacking ffmpeg reads them)

            int enc_w = 0, enc_h = 0; // the encoded frame (the window's, or the band's)
            bool via_copy = false;    // the frame is put together in copy_rt (NV12 or a band)
        };
        State s;

        // a stacked recording's frame, from the courses' window size and the number of lanes
        struct BandLayout {
            int src_y = 0, src_h = 0; // the band in the window: from the gauge above the lane to the lane's bottom
            int out_w = 0, out_h = 0; // the band in the stack (scaled down when the lanes do not fit the height)
            int bg_w = 0, bg_h = 0;   // the backdrop's source: the picture at a fifth of the size
        };
        BandLayout band_layout(int w, int h, int lanes) {
            const auto even = [](double v) { return 2 * static_cast<int>(std::lround(v / 2.0)); };
            const auto four = [](double v) { return 4 * std::max(1, static_cast<int>(std::lround(v / 4.0))); };
            BandLayout b;
            b.src_y = even(h * 136.0 / 720.0); // the lane is 184..360 at 720p
            b.src_h = even(h * 224.0 / 720.0);
            const double scale = std::min(1.0, static_cast<double>(h) / (std::max(1, lanes) * b.src_h));
            b.out_w = four(w * scale);
            b.out_h = std::max(2, even(b.src_h * scale));
            b.bg_w = four(w / 5.0);
            b.bg_h = even(h / 5.0);
            return b;
        }

        fs::path utf8_path(const std::string& v) {
            return fs::path(std::u8string(reinterpret_cast<const char8_t*>(v.data()), v.size()));
        }

        std::string utf8(const fs::path& p) {
            const std::u8string u = p.u8string();
            return {reinterpret_cast<const char*>(u.data()), u.size()};
        }

        std::string ffmpeg() {
            const char* v = std::getenv("ZDON_FFMPEG");
            return v && *v ? v : "ffmpeg";
        }

        std::string quoted(const fs::path& p) {
            return "\"" + utf8(p) + "\"";
        }

#ifdef _WIN32
        // cmd.exe drops the outer quotes of a command line that starts with one: wrap it once more
        std::wstring wide_command(const std::string& cmd) {
            return utf8_path("\"" + cmd + "\"").wstring();
        }
        int run(const std::string& cmd) {
            return _wsystem(wide_command(cmd).c_str());
        }
        FILE* open_pipe(const std::string& cmd) {
            return _wpopen(wide_command(cmd).c_str(), L"wb");
        }
        int close_pipe(FILE* f) {
            return _pclose(f);
        }
        void set_env(const char* name, const std::string& value) {
            _putenv_s(name, value.c_str());
        }
        const char* kNull = "NUL";
#else
        int run(const std::string& cmd) {
            return std::system(cmd.c_str());
        }
        FILE* open_pipe(const std::string& cmd) {
            return popen(cmd.c_str(), "w");
        }
        int close_pipe(FILE* f) {
            return pclose(f);
        }
        void set_env(const char* name, const std::string& value) {
            setenv(name, value.c_str(), 1);
        }
        const char* kNull = "/dev/null";
#endif

        // h264_nvenc when this machine can use it (a GPU and driver, not only the encoder in the build); it takes
        // the RGBA frames as they are and converts them on the GPU (ffmpeg's own conversion was the bottleneck)
        std::string video_codec() {
            const std::string probe = "\"" + ffmpeg() +
                                      "\" -hide_banner -loglevel error -f lavfi -i color=s=256x256:d=0.1 " +
                                      "-frames:v 1 -c:v h264_nvenc -f null - >" + kNull + " 2>&1";
            if (run(probe) == 0) {
                return "-c:v h264_nvenc -preset p5 -rc vbr -cq 19 -b:v 0";
            }
            return "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p";
        }

        // the AAC encoder for the mux: Windows' (aac_mf) when this ffmpeg has it, else ffmpeg's with the fast coder.
        // ffmpeg's default (twoloop) took 11.6 s for 3 minutes of sound, most of a stacked recording's mux; fast
        // 4.1 s, aac_mf 2.2 s (and the closest to the WAV: 47 dB SNR against twoloop's 42)
        std::string audio_codec() {
            static const std::string codec = [] {
                const std::string probe = "\"" + ffmpeg() +
                                          "\" -hide_banner -loglevel error -f lavfi -i sine=d=0.2 -c:a aac_mf "
                                          "-b:a 320k -f null - >" +
                                          kNull + " 2>&1";
                return run(probe) == 0 ? std::string("-c:a aac_mf -b:a 320k")
                                       : std::string("-c:a aac -aac_coder fast -b:a 320k");
            }();
            return codec;
        }

        void write_wav_header(FILE* f, double rate, uint64_t data_bytes) {
            auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            const uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(data_bytes, 0xFFFFFFF0u));
            std::fseek(f, 0, SEEK_SET);
            std::fwrite("RIFF", 1, 4, f);
            u32(36 + data);
            std::fwrite("WAVEfmt ", 1, 8, f);
            u32(16);
            u16(3); // IEEE float
            u16(2);
            u32(static_cast<uint32_t>(rate));
            u32(static_cast<uint32_t>(rate) * 2 * 4);
            u16(2 * 4);
            u16(32);
            std::fwrite("data", 1, 4, f);
            u32(data);
        }

        FILE* open_file(const fs::path& p) {
#ifdef _WIN32
            return _wfopen(p.wstring().c_str(), L"w+b");
#else
            return std::fopen(p.string().c_str(), "w+b");
#endif
        }

        void writer_main() {
            for (;;) {
                std::vector<unsigned char> buf;
                {
                    std::unique_lock lock(s.m);
                    s.cv.wait(lock, [] { return !s.queue.empty() || s.closing; });
                    if (s.queue.empty()) {
                        return;
                    }
                    buf = std::move(s.queue.front());
                    s.queue.pop_front();
                }
                s.cv.notify_all();
                const bool ok = !s.pipe_failed && std::fwrite(buf.data(), 1, buf.size(), s.video) == buf.size();
                std::scoped_lock lock(s.m);
                s.pipe_failed = s.pipe_failed || !ok;
                s.spare.push_back(std::move(buf));
            }
        }

        // the oldest pbo's frame into the writer's queue (waits while the queue is full)
        bool take_oldest() {
            const int i = s.in_flight.front();
            s.in_flight.pop_front();
            const size_t bytes = s.frame_bytes;
            std::vector<unsigned char> buf;
            {
                std::unique_lock lock(s.m);
                s.cv.wait(lock, [] { return s.queue.size() < kQueue || s.pipe_failed; });
                if (s.pipe_failed) {
                    return false;
                }
                if (!s.spare.empty()) {
                    buf = std::move(s.spare.back());
                    s.spare.pop_back();
                }
            }
            buf.resize(bytes);
            s.gl.bind(Gl::PIXEL_PACK_BUFFER, s.pbo[i]);
            const void* p = s.gl.map(Gl::PIXEL_PACK_BUFFER, 0, static_cast<std::ptrdiff_t>(bytes), Gl::MAP_READ_BIT);
            if (p) {
                std::memcpy(buf.data(), p, bytes);
                s.gl.unmap(Gl::PIXEL_PACK_BUFFER);
            }
            s.gl.bind(Gl::PIXEL_PACK_BUFFER, 0);
            if (!p) {
                spdlog::error("[record] could not map the read-back buffer");
                return false;
            }
            {
                std::scoped_lock lock(s.m);
                s.queue.push_back(std::move(buf));
            }
            s.cv.notify_all();
            return true;
        }

        bool start_video(int w, int h) {
            s.width = w;
            s.height = h;
            if (!s.gl.load()) {
                spdlog::error("[record] the GL driver has no pixel buffer objects");
                return false;
            }
            const bool blits = s.gl.bind_fb && s.gl.blit;
            s.enc_w = w;
            s.enc_h = h;
            if (s.band_lanes > 0) {
                if (!blits) {
                    spdlog::error("[record] the GL driver cannot blit: no band recording");
                    return false;
                }
                const BandLayout b = band_layout(w, h, s.band_lanes);
                s.enc_w = b.out_w;
                s.enc_h = b.out_h + (s.backdrop ? b.bg_h : 0);
            }
            const int ew = s.enc_w, eh = s.enc_h;
            const char* rgba_env = std::getenv("ZDON_RECORD_RGBA"); // A/B: the RGBA path
            s.nv12 = blits && ew % 4 == 0 && eh % 2 == 0 && !(rgba_env && *rgba_env == '1');
            if (s.nv12) {
                s.shader = ray::LoadShaderFromMemory(nullptr, kNv12Shader);
                if (s.shader.id == 0 || s.shader.id == ray::rlGetShaderIdDefault()) {
                    spdlog::warn("[record] the NV12 shader did not compile; recording RGBA");
                    s.nv12 = false;
                } else {
                    const int size[2] = {ew, eh};
                    ray::SetShaderValue(s.shader, ray::GetShaderLocation(s.shader, "srcSize"), size,
                                        ray::SHADER_UNIFORM_IVEC2);
                    s.nv12_rt = ray::LoadRenderTexture(ew / 4, eh * 3 / 2);
                }
            }
            s.via_copy = s.nv12 || s.band_lanes > 0;
            if (s.via_copy) {
                s.copy_rt = ray::LoadRenderTexture(ew, eh);
                ray::BeginTextureMode(s.copy_rt); // the backdrop's row right of the small picture stays black
                ray::ClearBackground(ray::Color{0, 0, 0, 255});
                ray::EndTextureMode();
            }
            s.frame_bytes = s.nv12 ? static_cast<size_t>(ew) * eh * 3 / 2 : static_cast<size_t>(ew) * eh * 4;
            s.gl.gen(kPbo, s.pbo.data());
            for (unsigned int b : s.pbo) {
                s.gl.bind(Gl::PIXEL_PACK_BUFFER, b);
                s.gl.data(Gl::PIXEL_PACK_BUFFER, static_cast<std::ptrdiff_t>(s.frame_bytes), nullptr, Gl::STREAM_READ);
            }
            s.gl.bind(Gl::PIXEL_PACK_BUFFER, 0);
            // a band: the window's size and the frame format, for the coordinator (once the pipe can be opened)
            const auto write_size = [&] {
                if (s.band_lanes > 0) {
                    fs::path size_file = s.out;
                    if (FILE* f = open_file(size_file.replace_extension(".size"))) {
                        std::println(f, "{} {} {}", w, h, s.nv12 ? "nv12" : "rgba");
                        std::fclose(f);
                    }
                }
            };
            if (!s.fifo.empty()) {
                s.video = fifo_writer(s.fifo, write_size);
                if (!s.video) {
                    spdlog::error("[record] cannot open the pipe {}", s.fifo);
                    return false;
                }
                s.writer = std::thread(writer_main);
                spdlog::info("[record] {}x{} {} into {}", ew, eh, s.nv12 ? "NV12" : "RGBA", s.fifo);
                return true;
            }
            write_size();
            const std::string size = std::to_string(ew) + "x" + std::to_string(eh);
            // NV12 rows are top first already; GL's RGBA rows come bottom first and ffmpeg flips them. The NV12
            // colour is declared on the input: declared on the output, ffmpeg converted every frame (69 fps)
            const std::string input =
                s.nv12 ? "-f rawvideo -pix_fmt nv12 -color_range tv -colorspace bt709 -color_primaries bt709 "
                         "-color_trc bt709 -s " +
                             size + " -framerate " + std::to_string(s.fps) + " -i - "
                       : "-f rawvideo -pix_fmt rgba -s " + size + " -framerate " + std::to_string(s.fps) +
                             " -i - -vf vflip ";
            const std::string cmd = "\"" + ffmpeg() + "\" -hide_banner -loglevel error -y " + input + video_codec() +
                                    " " + quoted(s.video_tmp);
            s.video = open_pipe(cmd);
            if (!s.video) {
                spdlog::error("[record] cannot start ffmpeg ({})", ffmpeg());
                return false;
            }
            s.writer = std::thread(writer_main);
            spdlog::info("[record] {}x{} {}{}", ew, eh, s.nv12 ? "NV12" : "RGBA",
                         s.band_lanes > 0 ? (s.backdrop ? " (band + backdrop)" : " (band)") : "");
            return true;
        }
        // the song is 95 % of the work, the result screen the rest
        void write_status(double progress) {
            if (s.status.empty()) {
                return;
            }
            if (FILE* f = open_file(s.status)) {
                std::println(f, "{:.4f}", progress);
                std::fclose(f);
            }
        }

        bool record_picture(); // this frame's picture into a pbo (false: stop the recording); below frame()
    } // namespace

    void note_song(double position_ms, double end_ms) {
        s.song_pos = position_ms;
        s.song_end = end_ms;
    }

    void prepare(int argc, char** argv) {
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            if (a == "--record" && i + 1 < argc) {
                s.active = true;
                s.out = fs::absolute(utf8_path(argv[++i]));
            } else if (a == "--record-fps" && i + 1 < argc) {
                s.fps = std::clamp(std::atoi(argv[++i]), 1, 240);
            } else if (a == "--record-result" && i + 1 < argc) {
                s.result_s = std::clamp(std::atof(argv[++i]), 0.0, 600.0);
            } else if (a == "--record-status" && i + 1 < argc) {
                s.status = fs::absolute(utf8_path(argv[++i]));
            } else if (a == "--record-jobs" && i + 1 < argc) {
                s.jobs = std::clamp(std::atoi(argv[++i]), 1, 16);
            } else if (a == "--record-part" && i + 1 < argc) {
                s.part = std::max(0, std::atoi(argv[++i]));
            } else if (a == "--record-range" && i + 2 < argc) {
                s.range_first = std::max(0LL, std::atoll(argv[++i]));
                s.range_end = std::atoll(argv[++i]);
            } else if (a == "--record-no-music") {
                s.no_music = true;
            } else if (a == "--record-backdrop") {
                s.backdrop = true;
            } else if (a == "--record-pipe" && i + 1 < argc) {
                s.fifo = argv[++i];
            } else if (a == "--record-band" && i + 1 < argc) {
                s.band_lanes = std::clamp(std::atoi(argv[++i]), 1, 5);
            } else if (a == "--record-sync-end" && i + 1 < argc) {
                s.sync_end_ms = std::max(0.0, std::atof(argv[++i]));
            } else if (a == "--record-tracks" && i + 1 < argc) {
                s.tracks.clear();
                s.track_tokens.clear();
                s.track_subdiff.clear();
                s.track_yono.clear();
                const std::string v = argv[++i];
                for (size_t p = 0; p <= v.size();) {
                    const size_t q = std::min(v.find(',', p), v.size());
                    if (q > p) {
                        // <course>[@<support level>[y]]: y = upstream's earlier simplification
                        const std::string t = v.substr(p, q - p);
                        const size_t at = t.find('@');
                        s.track_tokens.push_back(t);
                        s.tracks.push_back(std::atoi(t.substr(0, at).c_str()));
                        s.track_subdiff.push_back(at == std::string::npos ? 0 : std::atoi(t.c_str() + at + 1));
                        s.track_yono.push_back(at != std::string::npos && t.back() == 'y');
                    }
                    p = q + 1;
                }
            }
        }
        if (!s.active) {
            return;
        }
        // before anything reads them (fixed_step::step_ms and zdon_test_mode keep the first value)
        set_env("YATAIDON_TEST", "1");
        set_env("YATAIDON_FIXED_STEP_MS", std::to_string(1000.0 / s.fps));
        // dev hooks a parent test run (the recording mode) may hand down: they would drive this run instead
        for (const char* hook :
             {"YATAIDON_JUMP_SCREEN", "YATAIDON_FAKE_KEYS", "YATAIDON_FAKE_MOUSE", "YATAIDON_DUMP_FRAMES",
              "YATAIDON_DUMP_SCREEN", "YATAIDON_DUMP_CHARA", "YATAIDON_FORCE_AUTO", "ZDON_TEST_STALL"}) {
            set_env(hook, "");
        }
        zdon_config_read_only = true; // the overrides below stay in memory
    }

    bool active() {
        return s.active;
    }

    bool consume_arg(const std::string& arg, int& i, int argc) {
        if (arg == "--record-no-music" || arg == "--record-backdrop") {
            return true;
        }
        if (arg == "--record" || arg == "--record-fps" || arg == "--record-result" || arg == "--record-status" ||
            arg == "--record-jobs" || arg == "--record-part" || arg == "--record-tracks" ||
            arg == "--record-sync-end" || arg == "--record-band" || arg == "--record-pipe") {
            i = std::min(i + 1, argc - 1);
            return true;
        }
        if (arg == "--record-range") {
            i = std::min(i + 2, argc - 1);
            return true;
        }
        return false;
    }

    bool coordinating() {
        return s.active && s.part < 0 && (s.jobs > 1 || s.tracks.size() >= 3);
    }

    double sync_end_ms() {
        return s.active ? s.sync_end_ms : 0.0;
    }

    bool skip_present() {
        // the window is hidden and the frame is read back before the present: nothing to show
        static const bool present = [] {
            const char* v = std::getenv("ZDON_RECORD_PRESENT"); // A/B: present every frame
            return v && *v == '1';
        }();
        return s.active && (!present || (s.part >= 0 && s.frames < s.range_first));
    }

    int part() {
        return s.part;
    }

    const std::vector<int>& tracks() {
        return s.tracks;
    }

    int track_subdiff(size_t k) {
        return s.active && k < s.track_subdiff.size() ? s.track_subdiff[k] : 0;
    }

    bool track_subdiff_yono(size_t k) {
        return s.active && k < s.track_yono.size() && s.track_yono[k];
    }

    bool begin() {
        if (!s.active) {
            return true;
        }
        Config& c = *global_data.config;
        c.video.fullscreen = false; // the window keeps the skin's screen size: the video's size
        c.video.borderless = false;
        c.general.audio_offset = 0;
        c.general.visual_offset = 0;

        audio.dev_pause_device(true); // the frames below mix the audio, in step with the picture
        audio.dev_set_master_volume(1.0f);
        s.rate = audio.dev_sample_rate();
        if (s.no_music) { // the song, voices and screen sounds once in a stacked recording: the first lane has them
            VolumeConfig v = c.volume; // hitsound kept: hits, balloon and kusudama pops
            v.sound = v.music = v.voice = v.attract_mode = 0.0f;
            audio.set_volume_presets(v);
        }

        std::error_code ec;
        fs::create_directories(s.out.parent_path(), ec);
        s.video_tmp = s.out;
        s.audio_tmp = s.out;
        if (s.part >= 0) { // the coordinator joins the parts as they are
            s.audio_tmp.replace_extension(".wav");
        } else {
            s.video_tmp.replace_extension(".video.tmp.mp4");
            s.audio_tmp.replace_extension(".audio.tmp.wav");
        }
        s.wav = open_file(s.audio_tmp);
        if (!s.wav) {
            spdlog::error("[record] cannot write {}", utf8(s.audio_tmp));
            return false;
        }
        write_wav_header(s.wav, s.rate, 0);
        s.started = std::chrono::steady_clock::now();
        spdlog::info("[record] {} at {} fps, until {:.0f} s into the result screen", utf8(s.out), s.fps, s.result_s);
        if (s.part >= 0) {
            spdlog::info("[record] part {}: frames {} to {}", s.part, s.range_first,
                         s.range_end < 0 ? std::string("the end") : std::to_string(s.range_end));
        }
        return true;
    }

    bool frame(const std::string& screen) {
        if (!s.active) {
            return false;
        }
        using clk = std::chrono::steady_clock;
        const auto ms_since = [](clk::time_point t) {
            return std::chrono::duration<double, std::milli>(clk::now() - t).count();
        };
        // a part runs up to its first frame without its picture (nothing was drawn) and drops that audio
        const bool recording = s.frames >= s.range_first;
        if (recording && s.recorded > 0) {
            s.ms_game += std::chrono::duration<double, std::milli>(clk::now() - s.frame_end).count();
        }
        if (recording && !record_picture()) {
            s.stopped = true;
            return true;
        }

        // this frame's audio: the samples from frame f to frame f + 1 of the fixed clock
        const auto t2 = clk::now();
        const auto at = [](long long f) { return (long long)std::llround(static_cast<double>(f) * s.rate / s.fps); };
        const unsigned int n = static_cast<unsigned int>(at(s.frames + 1) - at(s.frames));
        s.mix.resize(static_cast<size_t>(n) * 2);
        audio.dev_mix_offline(s.mix.data(), n);
        if (recording) {
            std::fwrite(s.mix.data(), sizeof(float), s.mix.size(), s.wav);
            s.wav_bytes += s.mix.size() * sizeof(float);
            s.recorded++;
        }
        s.frames++;
        s.ms_audio += ms_since(t2);
        s.frame_end = clk::now();

        const double video_s = static_cast<double>(s.frames) / s.fps;
        if (video_s >= s.next_log_s) {
            s.next_log_s += 10.0;
            const double real = std::chrono::duration<double>(clk::now() - s.started).count();
            const double f = static_cast<double>(std::max(1LL, s.recorded));
            spdlog::info("[record] {:.0f} s ({:.0f} s recorded, {:.1f}x real time); per recorded frame ms: game "
                         "{:.2f}, read {:.2f}, encoder wait {:.2f}, audio {:.2f}",
                         video_s, s.recorded / static_cast<double>(s.fps), video_s / std::max(real, 1e-3),
                         s.ms_game / f, s.ms_read / f, s.ms_queue / f, s.ms_audio / f);
        }

        const bool game = screen.starts_with("GAME");
        const bool result = screen.contains("RESULT");
        s.played = s.played || game;
        if (s.frames % 15 == 0) {
            const double result_total = std::max(1.0, s.result_s * s.fps);
            write_status(s.part >= 0 ? static_cast<double>(s.recorded) // the coordinator sums the parts' frames
                         : result    ? 0.95 + 0.05 * std::min(1.0, static_cast<double>(s.result_frames) / result_total)
                         : s.song_end > 0 ? 0.95 * std::clamp(s.song_pos / s.song_end, 0.0, 1.0)
                                          : 0.0);
        }
        if (s.range_end >= 0 && s.frames >= s.range_end) {
            return true; // this part's last frame
        }
        if (result) {
            return ++s.result_frames >= (long long)std::llround(s.result_s * s.fps);
        }
        // a screen after the song that is not a result (a quit, a dan course's next stage is still GAME_DAN)
        return s.played && !game;
    }

    namespace {
        bool record_picture() {
            using clk = std::chrono::steady_clock;
            const auto ms_since = [](clk::time_point t) {
                return std::chrono::duration<double, std::milli>(clk::now() - t).count();
            };
            ray::rlDrawRenderBatchActive();
            const int w = ray::GetRenderWidth();
            const int h = ray::GetRenderHeight();
            if (!s.video && !start_video(w, h)) {
                return false;
            }
            if (w != s.width || h != s.height) {
                spdlog::error("[record] the window changed size ({}x{} -> {}x{}); stopping", s.width, s.height, w, h);
                return false;
            }

            // the picture: into a free pbo; the oldest one out to the writer
            if (static_cast<int>(s.in_flight.size()) == kPbo) {
                const auto tq = clk::now();
                if (!take_oldest()) {
                    spdlog::error("[record] ffmpeg stopped taking frames");
                    return false;
                }
                s.ms_queue += ms_since(tq);
            }
            const auto t1 = clk::now();
            const int ew = s.enc_w, eh = s.enc_h;
            int rw = ew, rh = eh;
            if (s.via_copy) {
                // the window's frame (or its band, and the small picture below it) into copy_rt; GL rows go up
                s.gl.bind_fb(Gl::READ_FRAMEBUFFER, 0);
                s.gl.bind_fb(Gl::DRAW_FRAMEBUFFER, s.copy_rt.id);
                if (s.band_lanes > 0) {
                    const BandLayout b = band_layout(w, h, s.band_lanes);
                    s.gl.blit(0, h - b.src_y - b.src_h, w, h - b.src_y, 0, eh - b.out_h, ew, eh, Gl::COLOR_BUFFER_BIT,
                              Gl::LINEAR);
                    if (s.backdrop) {
                        s.gl.blit(0, 0, w, h, 0, 0, b.bg_w, b.bg_h, Gl::COLOR_BUFFER_BIT, Gl::LINEAR);
                    }
                } else {
                    s.gl.blit(0, 0, w, h, 0, 0, w, h, Gl::COLOR_BUFFER_BIT, Gl::NEAREST);
                }
                s.gl.bind_fb(Gl::DRAW_FRAMEBUFFER, 0);
                s.gl.bind_fb(Gl::READ_FRAMEBUFFER, s.copy_rt.id);
            }
            if (s.nv12) { // the shader packs copy_rt into nv12_rt
                s.gl.bind_fb(Gl::READ_FRAMEBUFFER, 0);
                rw = ew / 4;
                rh = eh * 3 / 2;
                ray::BeginTextureMode(s.nv12_rt);
                ray::rlDisableColorBlend(); // the alpha byte is data
                ray::BeginShaderMode(s.shader);
                ray::DrawTexturePro(s.copy_rt.texture, Rect{0, 0, static_cast<float>(ew), static_cast<float>(eh)},
                                    Rect{0, 0, static_cast<float>(rw), static_cast<float>(rh)}, Vec2{0, 0}, 0.0f,
                                    ray::Color{255, 255, 255, 255});
                ray::EndShaderMode();
                ray::rlEnableColorBlend();
                ray::EndTextureMode();
                s.gl.bind_fb(Gl::READ_FRAMEBUFFER, s.nv12_rt.id);
            }
            const int slot = static_cast<int>(s.frames % kPbo);
            s.gl.bind(Gl::PIXEL_PACK_BUFFER, s.pbo[slot]);
            s.gl.read(0, 0, rw, rh, Gl::RGBA, Gl::UNSIGNED_BYTE, nullptr);
            s.gl.bind(Gl::PIXEL_PACK_BUFFER, 0);
            if (s.via_copy) {
                s.gl.bind_fb(Gl::READ_FRAMEBUFFER, 0);
            }
            s.in_flight.push_back(slot);
            s.ms_read += ms_since(t1);
            return true;
        }
    } // namespace

    bool end() {
        if (!s.active) {
            return true;
        }
        if (s.wav) {
            write_wav_header(s.wav, s.rate, s.wav_bytes);
            std::fclose(s.wav);
            s.wav = nullptr;
        }
        if (!s.video) {
            if (s.part >= 0) { // the recording ended before this part's first frame: an empty part
                std::error_code ec;
                fs::remove(s.audio_tmp, ec);
                spdlog::info("[record] part {}: nothing to record (the recording ended at frame {})", s.part, s.frames);
                return true;
            }
            spdlog::error("[record] no video was recorded");
            return false;
        }
        while (!s.in_flight.empty() && take_oldest()) {
        }
        {
            std::scoped_lock lock(s.m);
            s.closing = true;
        }
        s.cv.notify_all();
        s.writer.join();
        s.gl.del(kPbo, s.pbo.data());
        const int vrc = s.fifo.empty() ? close_pipe(s.video) : std::fclose(s.video);
        s.video = nullptr;
        if (vrc != 0 || s.pipe_failed) {
            spdlog::error("[record] ffmpeg failed encoding the video ({}); the parts are {} and {}", vrc,
                          utf8(s.video_tmp), utf8(s.audio_tmp));
            return false;
        }
        if (s.stopped) { // the recording stopped early (logged there): a cut-off video is a failure
            return false;
        }
        if (s.part >= 0) {
            const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.started).count();
            spdlog::info("[record] part {} done: {} frames from {} in {:.1f} s", s.part, s.recorded, s.range_first,
                         real);
            return true;
        }
        const std::string mux = "\"" + ffmpeg() + "\" -hide_banner -loglevel error -y -i " + quoted(s.video_tmp) +
                                " -i " + quoted(s.audio_tmp) + " -c:v copy " + audio_codec() +
                                " -shortest -movflags +faststart " + quoted(s.out);
        if (const int rc = run(mux); rc != 0) {
            spdlog::error("[record] ffmpeg failed muxing ({}); the parts are {} and {}", rc, utf8(s.video_tmp),
                          utf8(s.audio_tmp));
            return false;
        }
        std::error_code ec;
        fs::remove(s.video_tmp, ec);
        fs::remove(s.audio_tmp, ec);
        const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.started).count();
        spdlog::info("[record] done: {} ({:.1f} s of video in {:.1f} s)", utf8(s.out),
                     static_cast<double>(s.frames) / s.fps, real);
        return true;
    }

    namespace {
        double read_number(const fs::path& file) {
#ifdef _WIN32
            FILE* f = _wfopen(file.wstring().c_str(), L"rb");
#else
            FILE* f = std::fopen(file.c_str(), "rb");
#endif
            double v = 0;
            if (f) {
                if (std::fscanf(f, "%lf", &v) != 1) {
                    v = 0;
                }
                std::fclose(f);
            }
            return v;
        }

        // appends the samples of a WAV this file wrote (44-byte header) to out; the bytes appended
        uint64_t append_wav_data(const fs::path& part, FILE* out) {
#ifdef _WIN32
            FILE* f = _wfopen(part.wstring().c_str(), L"rb");
#else
            FILE* f = std::fopen(part.c_str(), "rb");
#endif
            if (!f) {
                return 0;
            }
            std::fseek(f, 44, SEEK_SET);
            std::vector<char> buf(1 << 20);
            uint64_t total = 0;
            for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;) {
                std::fwrite(buf.data(), 1, n, out);
                total += n;
            }
            std::fclose(f);
            return total;
        }

        // a stacked recording's course's frame: the window's size and the frame format (<out>.size, written by the
        // course once its pipe exists); false while it is not there
        bool read_band_size(const fs::path& file, int& w, int& h, bool& nv12) {
#ifdef _WIN32
            FILE* f = _wfopen(file.wstring().c_str(), L"rb");
#else
            FILE* f = std::fopen(file.c_str(), "rb");
#endif
            if (!f) {
                return false;
            }
            char fmt[16] = {};
            const bool ok = std::fscanf(f, "%d %d %15s", &w, &h, fmt) == 3 && w > 0 && h > 0;
            std::fclose(f);
            nv12 = std::string(fmt) == "nv12";
            return ok;
        }

        // --record-tracks with 3 to 5 courses: each course is recorded on its own (a 1P game, all at once, every
        // song held until the longest course ends) as only its gauge and lane band, scaled on the GPU to its size
        // in the stack (--record-band), the first course with its picture at a fifth of the size below the band
        // (--record-backdrop). The courses write their raw frames into pipes (--record-pipe) that one ffmpeg reads
        // while they record: it stacks the bands top to bottom over the small picture, blurred and darkened, and
        // encodes once. The sound: the first course's (with the song) and the other courses' hit sounds
        // (everything else muted, --record-no-music), summed and limited, muxed in afterwards.
        // 5 courses of a 179 s 1080p chart: 36.5 s; 130 s when every course encoded its whole picture and ffmpeg
        // decoded, cropped and scaled them afterwards (and the hidden windows presented every frame).
        constexpr double kStackStartS = 90.0; // the courses open their pipes within this, or the recording fails
        bool compose_tracks(const std::vector<std::string>& base, double chart_end_ms, long long total,
                            std::chrono::steady_clock::time_point started) {
            const fs::path dir = s.out.parent_path();
            const std::string stem = utf8(s.out.stem());
            const fs::path status_dir = fs::absolute("cache") / "record";
            std::error_code ec;
            fs::create_directories(status_dir, ec);
            const size_t n = s.tracks.size();
            std::vector<std::vector<std::string>> args;
            std::vector<fs::path> outs, wavs, sizes, statuses;
            std::vector<std::string> pipes;
            for (size_t k = 0; k < n; k++) {
                // the course's --record path: only its WAV and .size are written next to it
                outs.push_back(dir / utf8_path(stem + ".track" + std::to_string(k) + ".mp4"));
                wavs.push_back(fs::path(outs.back()).replace_extension(".wav"));
                sizes.push_back(fs::path(outs.back()).replace_extension(".size"));
                statuses.push_back(status_dir / ("part" + std::to_string(k) + ".txt"));
                pipes.push_back(fifo_name(static_cast<int>(k)));
                fs::remove(statuses.back(), ec);
                fs::remove(sizes.back(), ec);
                std::vector<std::string> a = base;
                a.insert(a.end(), {"--record-tracks",
                                   s.track_tokens[k],
                                   "--record",
                                   utf8(outs.back()),
                                   "--record-part",
                                   std::to_string(k),
                                   "--record-range",
                                   "0",
                                   "-1",
                                   "--record-jobs",
                                   "1",
                                   "--record-fps",
                                   std::to_string(s.fps),
                                   "--record-result",
                                   "0",
                                   "--record-status",
                                   utf8(statuses.back()),
                                   "--record-sync-end",
                                   std::to_string(chart_end_ms),
                                   "--record-band",
                                   std::to_string(n),
                                   "--record-pipe",
                                   pipes.back()});
                a.emplace_back(k == 0 ? "--record-backdrop" : "--record-no-music");
                args.push_back(std::move(a));
            }
            const fs::path video_tmp = dir / utf8_path(stem + ".video.tmp.mp4");
            spdlog::info("[record] {}: {} courses", utf8(s.out), n);
            auto cleanup = [&] {
                for (size_t k = 0; k < n; k++) {
                    fs::remove(wavs[k], ec);
                    fs::remove(sizes[k], ec);
                    fs::remove(statuses[k], ec);
#ifndef _WIN32
                    fs::remove(pipes[k], ec);
#endif
                }
                fs::remove(video_tmp, ec);
            };

            // the stacking ffmpeg, once every course's pipe is there
            std::thread stacker;
            std::atomic<bool> stack_done{false};
            int stack_rc = -1;
            auto start_stacking = [&] -> bool {
                int w = 0, h = 0;
                std::vector<bool> nv12(n);
                for (size_t k = 0; k < n; k++) {
                    bool v = false;
                    if (!read_band_size(sizes[k], w, h, v)) {
                        return false;
                    }
                    nv12[k] = v;
                }
                const BandLayout b = band_layout(w, h, static_cast<int>(n));
                std::string cmd = "\"" + ffmpeg() + "\" -hide_banner -loglevel error -y";
                std::string fc, stack;
                for (size_t k = 0; k < n; k++) {
                    const std::string size =
                        std::to_string(b.out_w) + "x" + std::to_string(b.out_h + (k == 0 ? b.bg_h : 0));
                    // as the courses' own encoders took them (start_video): NV12 top first, GL's RGBA bottom first
                    cmd += nv12[k] ? " -f rawvideo -pix_fmt nv12 -color_range tv -colorspace bt709 -color_primaries "
                                     "bt709 -color_trc bt709 -s " +
                                         size
                                   : " -f rawvideo -pix_fmt rgba -s " + size;
                    cmd += " -framerate " + std::to_string(s.fps) + " -i \"" + pipes[k] + "\"";
                    const std::string in = "[" + std::to_string(k) + ":v]";
                    const std::string lane = "[l" + std::to_string(k) + "]";
                    // planar from here: boxblur took NV12's interleaved chroma for a half-width plane (a green half)
                    fc += in + (nv12[k] ? "format=yuv420p" : "vflip,format=yuv420p") + lane + ";";
                    if (k > 0) {
                        stack += lane;
                    }
                }
                // the first course's band, and its small picture blurred into the backdrop (a full-size blur was
                // 4x the whole job's time); the bands are already their size in the stack
                fc += "[l0]split=2[s0][bgsrc];[s0]crop=" + std::to_string(b.out_w) + ":" + std::to_string(b.out_h) +
                      ":0:0[c0];[bgsrc]crop=" + std::to_string(b.bg_w) + ":" + std::to_string(b.bg_h) +
                      ":0:" + std::to_string(b.out_h) + ",boxblur=6:1,eq=brightness=-0.2,scale=" + std::to_string(w) +
                      ":" + std::to_string(h) + ":flags=fast_bilinear[bg];[c0]" + stack +
                      "vstack=inputs=" + std::to_string(n) +
                      "[st];[bg][st]overlay=(W-w)/2:(H-h)/2:format=yuv420,format=yuv420p[v]";
                cmd += " -filter_complex \"" + fc + "\" -map \"[v]\" " + video_codec() + " -an " + quoted(video_tmp);
                stacker = std::thread([&, cmd] {
                    stack_rc = run(cmd);
                    stack_done = true;
                });
                spdlog::info("[record] stacking {}x{} lanes of {}x{}", w, h, b.out_w, b.out_h);
                return true;
            };

            const double frames = std::max(
                1.0, static_cast<double>(n) * static_cast<double>(total - static_cast<long long>(s.result_s * s.fps)));
            bool stacking = false;
            const bool ok = run_children(args, [&] {
                double done = 0;
                for (const fs::path& st : statuses) {
                    done += read_number(st);
                }
                write_status(std::min(0.97, 0.97 * done / frames));
                if (!stacking) {
                    stacking = start_stacking();
                    const double waited =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    if (!stacking && waited > kStackStartS) {
                        spdlog::error("[record] the courses did not open their pipes in {:.0f} s", kStackStartS);
                        return false;
                    }
                }
                return !(stack_done && stack_rc != 0); // the stacking failed: stop the courses
            });
            if (stacker.joinable()) {
                stacker.join(); // the courses closed their pipes (or were stopped): ffmpeg ends
            }
            if (!ok || stack_rc != 0) {
                if (!ok) {
                    spdlog::error("[record] a course failed (record_part<k>.log)");
                } else {
                    spdlog::error("[record] stacking the courses failed ({})", stack_rc);
                }
                cleanup();
                return false;
            }
            const auto stacked = std::chrono::steady_clock::now();

            // every lane's hits over the song (the other lanes have no music): summed, then limited
            std::string cmd = "\"" + ffmpeg() + "\" -hide_banner -loglevel error -y -i " + quoted(video_tmp);
            std::string mix;
            for (size_t k = 0; k < n; k++) {
                cmd += " -i " + quoted(wavs[k]);
                mix += "[" + std::to_string(k + 1) + ":a]";
            }
            cmd += " -filter_complex \"" + mix + "amix=inputs=" + std::to_string(n) +
                   ":normalize=0:duration=longest,alimiter=limit=0.95:level=0:latency=1[a]\" -map 0:v -map \"[a]\" "
                   "-c:v copy " +
                   audio_codec() + " -shortest -movflags +faststart " + quoted(s.out);
            const int rc = run(cmd);
            cleanup();
            if (rc != 0) {
                spdlog::error("[record] muxing the courses' sound failed ({})", rc);
                return false;
            }
            write_status(1.0);
            const auto now = std::chrono::steady_clock::now();
            spdlog::info("[record] done: {} ({} courses, {:.1f} s: {:.1f} s recording and stacking, {:.1f} s sound)",
                         utf8(s.out), n, std::chrono::duration<double>(now - started).count(),
                         std::chrono::duration<double>(stacked - started).count(),
                         std::chrono::duration<double>(now - stacked).count());
            return true;
        }
    } // namespace

    // the recording past the chart's last note besides the result screen: before the song and after it
    // (measured 13 s and 25 s on two charts: too long a guess only shortens the last part, too short a guess
    // lengthens it)
    constexpr double kOverheadS = 20.0;

    bool coordinate(int argc, char** argv) {
        const auto started = std::chrono::steady_clock::now();
        // the length: the song ends at its last note (Player::end_time), then the time before the song, after
        // its end and the result screen
        const SessionData& sd = global_data.session_data[static_cast<int>(PlayerNum::P1)];
        double chart_end_ms = 0;
        try {
            SongParser parser(sd.selected_song);
            std::vector<int> courses = s.tracks;
            if (courses.empty()) {
                courses.push_back(sd.selected_difficulty);
            }
            for (const int course : courses) { // the song lasts until its longest lane ends
                auto [notes, branch_m, branch_e, branch_n] = parser.notes_to_position(course);
                for (const Note& n : notes.notes) {
                    if (n.type != NoteType::BARLINE) {
                        chart_end_ms = std::max(chart_end_ms, n.hit_ms);
                    }
                }
                for (const auto* branch : {&branch_m, &branch_e, &branch_n}) {
                    for (const NoteList& section : *branch) {
                        for (const Note& n : section.notes) {
                            chart_end_ms = std::max(chart_end_ms, n.hit_ms);
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("[record] {}", e.what());
        }
        int jobs = s.jobs;
        if (chart_end_ms <= 0) {
            spdlog::warn("[record] the chart's length is unknown: one part");
            jobs = 1;
        }
        const long long total = std::llround((chart_end_ms / 1000.0 + kOverheadS + s.result_s) * s.fps);
        spdlog::info("[record] the chart ends at {:.1f} s: about {:.1f} s of video", chart_end_ms / 1000.0,
                     total / static_cast<double>(s.fps));

        // the parts' command lines: this one without the --record options, then each part's
        std::vector<std::string> base;
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            if (consume_arg(a, i, argc)) {
                continue; // past the option's values
            }
            base.push_back(a);
        }
        if (s.tracks.size() >= 3) {
            return compose_tracks(base, chart_end_ms, total, started);
        }
        if (!s.tracks.empty()) {
            std::string v;
            for (const std::string& t : s.track_tokens) {
                v += (v.empty() ? "" : ",") + t;
            }
            base.insert(base.end(), {"--record-tracks", v});
        }
        const fs::path dir = s.out.parent_path();
        const std::string stem = utf8(s.out.stem());
        const fs::path status_dir = fs::absolute("cache") / "record";
        std::error_code ec;
        fs::create_directories(status_dir, ec);
        std::vector<std::vector<std::string>> args;
        std::vector<fs::path> videos, wavs, statuses;
        for (int k = 0; k < jobs; k++) {
            const long long first = total * k / jobs;
            const long long last = k + 1 < jobs ? total * (k + 1) / jobs : -1;
            videos.push_back(dir / utf8_path(stem + ".part" + std::to_string(k) + ".mp4"));
            wavs.push_back(dir / utf8_path(stem + ".part" + std::to_string(k) + ".wav"));
            statuses.push_back(status_dir / ("part" + std::to_string(k) + ".txt"));
            fs::remove(statuses.back(), ec);
            std::vector<std::string> a = base;
            a.insert(a.end(), {"--record", utf8(videos.back()), "--record-part", std::to_string(k), "--record-range",
                               std::to_string(first), std::to_string(last), "--record-jobs", "1", "--record-fps",
                               std::to_string(s.fps), "--record-result", std::to_string(s.result_s), "--record-status",
                               utf8(statuses.back())});
            args.push_back(std::move(a));
        }
        spdlog::info("[record] {} in {} parts", utf8(s.out), jobs);

        double next_log = 10.0;
        const bool ok = run_children(args, [&] {
            double done = 0;
            for (const fs::path& st : statuses) {
                done += read_number(st);
            }
            write_status(std::min(0.99, done / std::max(1LL, total)));
            const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (real >= next_log) {
                next_log += 10.0;
                spdlog::info("[record] {:.0f} s recorded in {:.0f} s", done / s.fps, real);
            }
            return true;
        });
        auto cleanup = [&] {
            for (size_t k = 0; k < videos.size(); k++) {
                fs::remove(videos[k], ec);
                fs::remove(wavs[k], ec);
                fs::remove(statuses[k], ec);
            }
        };
        if (!ok) {
            spdlog::error("[record] a part failed (record_part<k>.log)");
            cleanup();
            return false;
        }

        // join: the parts' videos as they are, their samples into one WAV, then the AAC mux
        const fs::path list = dir / utf8_path(stem + ".parts.txt");
        const fs::path wav = dir / utf8_path(stem + ".audio.tmp.wav");
        FILE* lf = open_file(list);
        FILE* wf = open_file(wav);
        if (!lf || !wf) {
            spdlog::error("[record] cannot write next to {}", utf8(s.out));
            cleanup();
            return false;
        }
        // the parts mixed at the device rate of the config (this process opened no device)
        const double rate = global_data.config->audio.sample_rate > 0 ? global_data.config->audio.sample_rate : 44100.0;
        write_wav_header(wf, rate, 0);
        uint64_t wav_bytes = 0;
        int parts = 0;
        for (size_t k = 0; k < videos.size(); k++) {
            if (!fs::exists(videos[k], ec) || fs::file_size(videos[k], ec) == 0) {
                continue; // an empty part: the recording ended before it
            }
            std::string p = utf8(fs::path(videos[k]).generic_u8string());
            std::string escaped;
            for (char c : p) {
                escaped += c == '\'' ? std::string("'\\''") : std::string(1, c);
            }
            std::println(lf, "file '{}'", escaped);
            wav_bytes += append_wav_data(wavs[k], wf);
            parts++;
        }
        std::fclose(lf);
        write_wav_header(wf, rate, wav_bytes);
        std::fclose(wf);
        const std::string mux = "\"" + ffmpeg() + "\" -hide_banner -loglevel error -y -f concat -safe 0 -i " +
                                quoted(list) + " -i " + quoted(wav) + " -c:v copy " + audio_codec() +
                                " -shortest -movflags +faststart " + quoted(s.out);
        const int rc = parts > 0 ? run(mux) : -1;
        cleanup();
        fs::remove(list, ec);
        fs::remove(wav, ec);
        if (rc != 0) {
            spdlog::error("[record] joining the parts failed ({})", rc);
            return false;
        }
        write_status(1.0);
        const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        spdlog::info("[record] done: {} ({} parts, {:.1f} s)", utf8(s.out), parts, real);
        return true;
    }
} // namespace zdon_record
