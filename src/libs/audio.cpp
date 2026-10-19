#include "audio.h"

#include "audio_dev.h"
#include "fixed_step.h"
#include "jobs.h"
#include "loudness.h"
#include "profiler.h"
#include "spdlog/spdlog.h"
#include "test_mode.h"
#ifdef YATAIDON_PLATFORM_IOS
#include "../platform/platform_ios.h"
#endif
#ifdef _WIN32
#include "../platform/platform_windows.h"
#endif

// Global iOS audio stream for platform access
#ifdef YATAIDON_PLATFORM_IOS
SDL_AudioStream* g_ios_audio_stream = nullptr;
#endif
#ifdef SUPPORT_FUMEN
#include "optional/nub.h"
#include "optional/nus3bank.h"
#endif
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "filesystem.h"

#ifdef SUPPORT_FUMEN
static bool is_bank_file(const fs::path& p) {
    auto ext = p.extension();
    return ext == ".nus3bank" || ext == ".nub";
}
static bool decode_bank(const fs::path& p, gen4::DecodedAudio& out) {
    return p.extension() == ".nub" ? gen3::decode_nub(p, out) : gen4::decode_nus3bank(p, out);
}
#endif
#include <algorithm>
#if defined(__ANDROID__) || defined(YATAIDON_PLATFORM_IOS)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

// Decode any audio file to float32 interleaved via FFmpeg.
// Returns false on failure; on success fills frames/sample_rate/channels/data (caller frees).
static bool ffmpeg_decode_float(const char* path, float** out_data, sf_count_t* out_frames, unsigned int* out_rate,
                                unsigned int* out_channels) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) {
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    int audio_idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = (int)i;
            break;
        }
    }
    if (audio_idx < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    auto* par = fmt->streams[audio_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return false;
    }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(dec, par) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }

    SwrContext* swr = nullptr;
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int out_ch = 2;
    if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, dec->sample_rate, &dec->ch_layout, dec->sample_fmt,
                            dec->sample_rate, 0, nullptr) < 0 ||
        !swr) {
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }
    if (swr_init(swr) < 0) {
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }

    std::vector<float> pcm;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    if (!pkt || !frm) {
        av_packet_free(&pkt);
        av_frame_free(&frm);
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }

    auto drain = [&]() {
        while (true) {
            int r = avcodec_receive_frame(dec, frm);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                break;
            }
            if (r < 0) {
                break;
            }
            int64_t n = swr_get_out_samples(swr, frm->nb_samples);
            std::vector<float> tmp(n * out_ch);
            uint8_t* op = (uint8_t*)tmp.data();
            int got = swr_convert(swr, &op, (int)n, (const uint8_t**)frm->extended_data, frm->nb_samples);
            if (got > 0) {
                pcm.insert(pcm.end(), tmp.begin(), tmp.begin() + got * out_ch);
            } else if (got < 0) {
                spdlog::error("ffmpeg_decode_float: swr_convert failed for {}", path);
            }
            av_frame_unref(frm);
        }
    };

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audio_idx) {
            if (avcodec_send_packet(dec, pkt) < 0) {
                spdlog::error("ffmpeg_decode_float: avcodec_send_packet failed for {}", path);
            } else {
                drain();
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dec, nullptr);
    drain();

    av_packet_free(&pkt);
    av_frame_free(&frm);
    swr_free(&swr);

    if (pcm.empty()) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }

    *out_rate = (unsigned int)dec->sample_rate;
    *out_channels = (unsigned int)out_ch;
    *out_frames = (sf_count_t)(pcm.size() / out_ch);
    *out_data = new float[pcm.size()];
    std::copy(pcm.begin(), pcm.end(), *out_data);

    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return true;
}
#endif // Android/iOS FFmpeg audio fallback

static sf_count_t vf_get_filelen(void* user_data) {
    auto* vf = static_cast<VirtualFile*>(user_data);
    return static_cast<sf_count_t>(vf->data->size());
}

static sf_count_t vf_seek(sf_count_t offset, int whence, void* user_data) {
    auto* vf = static_cast<VirtualFile*>(user_data);
    sf_count_t size = static_cast<sf_count_t>(vf->data->size());

    switch (whence) {
    case SEEK_SET:
        vf->pos = offset;
        break;
    case SEEK_CUR:
        vf->pos += offset;
        break;
    case SEEK_END:
        vf->pos = size + offset;
        break;
    default:
        return -1;
    }

    vf->pos = std::max<sf_count_t>(vf->pos, 0);
    vf->pos = std::min(vf->pos, size);
    return vf->pos;
}

static sf_count_t vf_read(void* ptr, sf_count_t count, void* user_data) {
    auto* vf = static_cast<VirtualFile*>(user_data);
    sf_count_t remaining = static_cast<sf_count_t>(vf->data->size()) - vf->pos;
    sf_count_t to_read = std::min(count, remaining);
    if (to_read <= 0) {
        return 0;
    }
    std::memcpy(ptr, vf->data->data() + vf->pos, static_cast<std::size_t>(to_read));
    vf->pos += to_read;
    return to_read;
}

static sf_count_t vf_write(const void*, sf_count_t, void*) {
    return 0; // read-only
}

static sf_count_t vf_tell(void* user_data) {
    return static_cast<VirtualFile*>(user_data)->pos;
}

AudioEngine::AudioEngine() {
    activate_cells = std::make_unique<ActivateCell[]>(kActivateCap);
    for (size_t i = 0; i < kActivateCap; i++) {
        activate_cells[i].seq.store(i, std::memory_order_relaxed);
    }
    active_voices.reserve(kSlotChunk * kMaxSlotChunks); // the mixer never allocates
}

#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
struct RtApiEntry {
    RtAudio::Api api;
    const char* label;
};

// RtAudio backend for a config device_type (1-8), or null for the SDL3/PortAudio types.
static const RtApiEntry* rt_api_for_type(int device_type) {
    static const RtApiEntry kApis[] = {
        {RtAudio::LINUX_ALSA, "ALSA"},       {RtAudio::LINUX_PULSE, "PulseAudio"}, {RtAudio::UNIX_JACK, "JACK"},
        {RtAudio::LINUX_OSS, "OSS"},         {RtAudio::MACOSX_CORE, "CoreAudio"},  {RtAudio::WINDOWS_DS, "DirectSound"},
        {RtAudio::WINDOWS_WASAPI, "WASAPI"}, {RtAudio::WINDOWS_ASIO, "ASIO"},
    };
    return device_type >= 1 && device_type <= 8 ? &kApis[device_type - 1] : nullptr;
}

static void rtaudio_error_log(RtAudioErrorType type, const std::string& errorText) {
    if (type == RTAUDIO_WARNING) {
        spdlog::warn("RtAudio: {}", errorText);
    } else {
        spdlog::error("RtAudio: {}", errorText);
    }
}
#endif

std::vector<std::string> enumerate_audio_devices(int device_type) {
    std::vector<std::string> names;

#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
    if (const RtApiEntry* entry = rt_api_for_type(device_type)) {
        try {
            RtAudio rt(entry->api, rtaudio_error_log);
            for (unsigned int id : rt.getDeviceIds()) {
                RtAudio::DeviceInfo info = rt.getDeviceInfo(id);
                if (info.outputChannels > 0 && !info.name.empty()) {
                    names.push_back(info.name);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("Failed to enumerate RtAudio devices: {}", e.what());
        }
        return names;
    }
#endif

#ifdef _WIN32
    switch (device_type) {
    case 9:
        return win32_enumerate_portaudio_devices(paWDMKS);
    case 10:
        return win32_enumerate_portaudio_devices(paMME);
    default:
        break;
    }
#endif

    // Default (SDL3) backend and any unrecognized type: enumerate SDL3 devices.
    const bool init_here = !(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO);
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (devices) {
        for (int i = 0; i < count; i++) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            if (name && *name) {
                names.emplace_back(name);
            }
        }
        SDL_free(devices);
    }
    if (init_here) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    return names;
}

AudioEngine::~AudioEngine() {
    close_audio_device();
    stop_decoder();
    reclaim(true);
    delete mixer_streams.exchange(nullptr);
    for (auto& chunk : slot_chunks) {
        delete[] chunk.exchange(nullptr);
    }
}

// Splits one source frame into left/right: mono by pan, stereo attenuated on the far side.
static inline void pan_frame(const float* src, unsigned int channels, float pan, float& left, float& right) {
    if (channels == 1) {
        left = src[0] * (1.0f - pan);
        right = src[0] * pan;
    } else {
        left = src[0];
        right = src[1];
        if (pan < 0.5f) {
            right *= (pan * 2.0f);
        } else if (pan > 0.5f) {
            left *= ((1.0f - pan) * 2.0f);
        }
    }
}

void AudioEngine::mix(float* out, unsigned int framesPerBuffer, AudioEngine* engine) {
    PROFILE_SCOPE();

    const unsigned long buffer_size = framesPerBuffer * 2;
    std::memset(out, 0, buffer_size * sizeof(float));

    if (!engine) {
        return;
    }

    const bool stats = audio_dev::stats_enabled();
    const int64_t t_start = stats ? audio_dev::now_ns() : 0;

    // No lock: what is freed elsewhere waits until this callback has left (see retire)
    engine->mix_epoch.fetch_add(1, std::memory_order_seq_cst);
    engine->mix_sounds(out, framesPerBuffer);
    engine->mix_streams(out, framesPerBuffer);
    engine->mix_epoch.fetch_add(1, std::memory_order_seq_cst);

    audio_dev::capture(out, framesPerBuffer);
    const float master_vol = engine->master_volume.load(std::memory_order_relaxed);
    for (unsigned long i = 0; i < buffer_size; i++) {
        out[i] = std::clamp(out[i] * master_vol, -1.0f, 1.0f);
    }
    if (stats) {
        audio_dev::record_callback(audio_dev::now_ns() - t_start, 0);
    }
}

void AudioEngine::mix_sounds(float* out, unsigned int framesPerBuffer) {
    auto add_voice = [this](SoundId id) {
        SoundSlot& s = *slot_at(id);
        if (!s.in_active) {
            s.in_active = true;
            active_voices.push_back(id); // capacity reserved for every possible slot
        }
    };
    // voices play_sound started since the last callback
    for (;;) {
        const size_t pos = activate_tail.load(std::memory_order_relaxed);
        ActivateCell& cell = activate_cells[pos % kActivateCap];
        if (cell.seq.load(std::memory_order_acquire) != pos + 1) {
            break;
        }
        const SoundId id = cell.id;
        cell.seq.store(pos + kActivateCap, std::memory_order_release);
        activate_tail.store(pos + 1, std::memory_order_relaxed);
        add_voice(id);
    }
    if (activate_rescan.exchange(false, std::memory_order_acquire)) {
        const uint32_t n = slot_count.load(std::memory_order_acquire);
        for (SoundId id = 0; id < n; id++) {
            if (slot_at(id)->is_playing.load(std::memory_order_relaxed)) {
                add_voice(id);
            }
        }
    }

    for (size_t v = 0; v < active_voices.size();) {
        SoundSlot& snd = *slot_at(active_voices[v]);
        const sound* pcm =
            snd.is_playing.load(std::memory_order_acquire) ? snd.pcm.load(std::memory_order_seq_cst) : nullptr;
        if (!pcm) {
            snd.is_playing.store(false, std::memory_order_relaxed); // unloaded while playing
            snd.in_active = false;
            active_voices[v] = active_voices.back();
            active_voices.pop_back();
            continue;
        }

        const float volume = snd.volume.load(std::memory_order_relaxed) *
                             snd.playback_gain.load(std::memory_order_relaxed) * pcm->normalization_gain;

        const float pan = snd.pan.load(std::memory_order_relaxed);
        unsigned int frame = snd.current_frame.load(std::memory_order_relaxed);

        const float pitch = snd.pitch.load(std::memory_order_relaxed);
        const float* data_ptr = pcm->data;
        const unsigned int channels = pcm->channels;

        unsigned long frames_to_process = framesPerBuffer;
        unsigned long output_index = 0;
        bool still_playing = true;
        // Track the position as integer frame + fractional remainder. The
        // fraction must survive across mix() calls, otherwise non-integer
        // pitches degrade toward trunc(pitch) as the buffer size shrinks
        // (at buffer_size=1, pitch 0.9 never advances and 1.5 plays at 1.0).
        double frame_f = (double)frame + snd.frame_frac.load(std::memory_order_relaxed);

        unsigned int end_frame = pcm->frame_count;
        bool decoding = false; // the tail isn't decoded yet: past end_frame is silence for now, not the end
        if (pcm->decode_job) {
            decoding = !pcm->decode_job->done.load(std::memory_order_acquire);
            end_frame = pcm->decode_job->frames_ready.load(std::memory_order_acquire);
        }

        while (frames_to_process > 0 && still_playing) {
            unsigned long src_frame = (unsigned long)frame_f;
            if (src_frame >= end_frame) {
                if (decoding) {
                    break;
                }
                if (snd.loop.load(std::memory_order_relaxed) && end_frame > 0) {
                    frame_f = 0.0;
                    continue;
                } else {
                    still_playing = false;
                    break;
                }
            }

            unsigned long dst_index = output_index * 2;
            float left, right;
            pan_frame(data_ptr + src_frame * channels, channels, pan, left, right);

            out[dst_index] += left * volume;
            out[dst_index + 1] += right * volume;

            frame_f += pitch;
            output_index++;
            frames_to_process--;
        }
        frame = (unsigned int)frame_f;

        // a store_sound/unload that replaced the PCM meanwhile has reset the position already
        if (snd.pcm.load(std::memory_order_relaxed) == pcm) {
            snd.frame_frac.store((float)(frame_f - (double)frame), std::memory_order_relaxed);
            snd.current_frame.store(frame, std::memory_order_relaxed);
        }
        if (!still_playing) {
            snd.is_playing.store(false, std::memory_order_release);
            snd.in_active = false;
            active_voices[v] = active_voices.back();
            active_voices.pop_back();
            continue;
        }
        v++;
    }
}

// Mixer side of a stream's seek: drop what the ring holds from before it.
static void apply_stream_flush(music& mus) {
    const uint32_t rgen = mus.ring_gen.load(std::memory_order_acquire);
    if (rgen == mus.mixer_gen) {
        return;
    }
    const unsigned long long fp = mus.flush_pos.load(std::memory_order_acquire);
    if (mus.read_pos.load(std::memory_order_relaxed) < fp) {
        mus.read_pos.store(fp, std::memory_order_release);
    }
    mus.mixer_gen = rgen;
    mus.current_frame.store(mus.cmd_out_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void AudioEngine::mix_streams(float* out, unsigned int framesPerBuffer) {
    const StreamList* list = mixer_streams.load(std::memory_order_seq_cst);
    if (!list) {
        return;
    }
    for (music* mp : list->streams) {
        music& mus = *mp;
        const uint32_t gen = mus.cmd_gen.load(std::memory_order_acquire);
        const unsigned int channels = (unsigned int)mus.file_info.channels;

        if (mus.pcm_data) {
            // fully decoded: read in place; a seek/stop/play takes effect here
            if (gen != mus.mixer_gen) {
                mus.mixer_gen = gen;
                mus.current_frame.store(mus.cmd_out_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            if (!mus.is_playing.load(std::memory_order_acquire)) {
                continue;
            }
            const float volume = mus.volume.load(std::memory_order_relaxed) * mus.normalization_gain;
            const float pan = mus.pan.load(std::memory_order_relaxed);
            const unsigned long long total = (unsigned long long)std::max<sf_count_t>(mus.pcm_total_frames, 0);
            unsigned long long pos = mus.current_frame.load(std::memory_order_relaxed);
            unsigned long frames_to_process = framesPerBuffer;
            unsigned long output_index = 0;
            while (frames_to_process > 0) {
                if (pos >= total) {
                    if (mus.loop && total > 0) {
                        pos = 0;
                        continue;
                    }
                    mus.is_playing.store(false, std::memory_order_release);
                    break;
                }
                const unsigned long n = (unsigned long)std::min<unsigned long long>(frames_to_process, total - pos);
                for (unsigned long i = 0; i < n; i++) {
                    float left, right;
                    pan_frame(mus.pcm_data + (pos + i) * channels, channels, pan, left, right);
                    out[(output_index + i) * 2] += left * volume;
                    out[(output_index + i) * 2 + 1] += right * volume;
                }
                pos += n;
                output_index += n;
                frames_to_process -= n;
            }
            mus.current_frame.store(pos, std::memory_order_relaxed);
            continue;
        }

        apply_stream_flush(mus);
        if (gen != mus.ring_gen.load(std::memory_order_acquire)) {
            continue; // a seek/stop the decoder hasn't carried out yet: hold the position, play nothing
        }
        if (!mus.is_playing.load(std::memory_order_acquire)) {
            continue;
        }

        const float volume = mus.volume.load(std::memory_order_relaxed) * mus.normalization_gain;
        const float pan = mus.pan.load(std::memory_order_relaxed);
        const unsigned long long rp = mus.read_pos.load(std::memory_order_relaxed);
        const unsigned long long avail = mus.write_pos.load(std::memory_order_acquire) - rp;
        const unsigned long n = (unsigned long)std::min<unsigned long long>(framesPerBuffer, avail);
        for (unsigned long i = 0; i < n; i++) {
            const float* src = mus.ring.get() + ((rp + i) % mus.ring_frames) * channels;
            float left, right;
            pan_frame(src, channels, pan, left, right);
            out[i * 2] += left * volume;
            out[i * 2 + 1] += right * volume;
        }
        mus.read_pos.store(rp + n, std::memory_order_release);
        mus.current_frame.fetch_add(n, std::memory_order_relaxed);

        if (n < framesPerBuffer) {
            if (mus.eof.load(std::memory_order_acquire) && mus.write_pos.load(std::memory_order_acquire) == rp + n &&
                mus.cmd_gen.load(std::memory_order_acquire) == gen) {
                mus.is_playing.store(false, std::memory_order_release); // the end of the file
            } else {
                audio_dev::record_stream_underrun(); // the decoder fell behind: silence, position held
            }
        }
    }
}

void AudioEngine::sdl_audio_callback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                     int /*total_amount*/) {
    AudioEngine* engine = static_cast<AudioEngine*>(userdata);
    if (!engine || additional_amount <= 0) {
        return;
    }

    const int bytes_per_frame = static_cast<int>(sizeof(float)) * 2; // f32 stereo, matches the spec we opened with
    unsigned int frames = static_cast<unsigned int>(additional_amount) / bytes_per_frame;
    if (frames == 0) {
        return;
    }

    size_t needed_floats = static_cast<size_t>(frames) * 2;
    if (engine->sdl_scratch_buffer.size() < needed_floats) {
        engine->sdl_scratch_buffer.resize(needed_floats);
    }

    mix(engine->sdl_scratch_buffer.data(), frames, engine);

    SDL_PutAudioStreamData(stream, engine->sdl_scratch_buffer.data(), static_cast<int>(needed_floats * sizeof(float)));
}

#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
int AudioEngine::rt_audio_callback(void* outputBuffer, void* /*inputBuffer*/, unsigned int framesPerBuffer,
                                   double /*streamTime*/, unsigned int status, void* userData) {
    AudioEngine* engine = static_cast<AudioEngine*>(userData);
    if (status & RTAUDIO_OUTPUT_UNDERFLOW) {
        audio_dev::record_underflow();
    }
    float* out = static_cast<float*>(outputBuffer);
    const unsigned int total_channels = engine->rt_total_channels;
    std::memset(out, 0, static_cast<size_t>(framesPerBuffer) * total_channels * sizeof(float));

    if (total_channels == 2 && engine->channel_offsets.size() == 1 && engine->channel_offsets[0] == 0) {
        mix(out, framesPerBuffer, engine);
        return 0;
    }

    size_t needed_floats = static_cast<size_t>(framesPerBuffer) * 2;
    if (engine->sdl_scratch_buffer.size() < needed_floats) {
        engine->sdl_scratch_buffer.resize(needed_floats);
    }
    mix(engine->sdl_scratch_buffer.data(), framesPerBuffer, engine);

    for (unsigned int f = 0; f < framesPerBuffer; f++) {
        const float l = engine->sdl_scratch_buffer[f * 2];
        const float r = engine->sdl_scratch_buffer[f * 2 + 1];
        for (int offset : engine->channel_offsets) {
            out[f * total_channels + offset] = l;
            out[f * total_channels + offset + 1] = r;
        }
    }
    return 0;
}
#endif

#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
bool AudioEngine::init_rtaudio_device(RtAudio::Api api, const char* label) {
    rt_audio = new RtAudio(api, rtaudio_error_log);

    if (rt_audio->getDeviceCount() == 0) {
        spdlog::error("No audio devices found for RtAudio backend: {}", label);
        delete rt_audio;
        rt_audio = nullptr;
        return false;
    }

    std::erase_if(channel_offsets, [](int off) { return off < 0; });
    std::ranges::sort(channel_offsets);
    channel_offsets.erase(std::ranges::unique(channel_offsets).begin(), channel_offsets.end());
    if (channel_offsets.empty()) {
        channel_offsets.push_back(0);
    }

    int max_offset = 0;
    for (int off : channel_offsets) {
        max_offset = std::max(max_offset, off);
    }
    rt_total_channels = static_cast<unsigned int>(max_offset) + 2;

    RtAudio::StreamParameters params;
    params.deviceId = rt_audio->getDefaultOutputDevice();
    params.nChannels = rt_total_channels;
    params.firstChannel = 0;

    if (!device_name.empty()) {
        bool found = false;
        for (unsigned int id : rt_audio->getDeviceIds()) {
            RtAudio::DeviceInfo info = rt_audio->getDeviceInfo(id);
            if (info.outputChannels > 0 && info.name == device_name) {
                params.deviceId = id;
                found = true;
                break;
            }
        }
        if (!found) {
            spdlog::warn("Requested audio device '{}' not found; using default", device_name);
        }
    }

    unsigned int bufferFrames = static_cast<unsigned int>(buffer_size);

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_MINIMIZE_LATENCY | RTAUDIO_SCHEDULE_REALTIME;
    options.priority = 99;

    RtAudioErrorType err =
        rt_audio->openStream(&params, nullptr, RTAUDIO_FLOAT32, static_cast<unsigned int>(target_sample_rate),
                             &bufferFrames, &AudioEngine::rt_audio_callback, this, &options);
    if (err != RTAUDIO_NO_ERROR) {
        spdlog::error("Failed to open {} stream: {}", label, rt_audio->getErrorText());
        delete rt_audio;
        rt_audio = nullptr;
        return false;
    }

    err = rt_audio->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        spdlog::error("Failed to start {} stream: {}", label, rt_audio->getErrorText());
        rt_audio->closeStream();
        delete rt_audio;
        rt_audio = nullptr;
        return false;
    }

    is_ready = true;

    auto dev_info = rt_audio->getDeviceInfo(params.deviceId);
    spdlog::info("Audio Device initialized successfully");
    spdlog::info("    > Backend:       RtAudio | {}", label);
    spdlog::info("    > Device:        {}", dev_info.name);
    spdlog::info("    > Format:        Float32");
    std::string offsets_str;
    for (int off : channel_offsets) {
        offsets_str += (offsets_str.empty() ? "" : ", ") + std::to_string(off);
    }
    spdlog::info("    > Channels:      {} total, stereo duplicated at offsets [{}]", rt_total_channels, offsets_str);
    spdlog::info("    > Sample rate:   {} Hz", rt_audio->getStreamSampleRate());
    spdlog::info("    > Buffer size:   {} frames (actual)", bufferFrames);
    return true;
}
#endif

bool AudioEngine::init_sdl3_device() {
#ifdef YATAIDON_PLATFORM_IOS
    ios_request_audio_buffer();
#endif
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);

    char frames_str[16];
    std::snprintf(frames_str, sizeof(frames_str), "%lu", buffer_size);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_str);

    if (!sdl_audio_subsystem_initialized) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            spdlog::error("Failed to init SDL audio subsystem: {}", SDL_GetError());
            return false;
        }
        sdl_audio_subsystem_initialized = true;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = static_cast<int>(target_sample_rate);

    auto quit_sdl_audio_subsystem = [this]() {
        if (sdl_audio_subsystem_initialized) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_audio_subsystem_initialized = false;
        }
    };

    SDL_AudioDeviceID device_id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!device_name.empty()) {
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
        if (devices) {
            for (int i = 0; i < count; i++) {
                const char* name = SDL_GetAudioDeviceName(devices[i]);
                if (name && device_name == name) {
                    device_id = devices[i];
                    break;
                }
            }
            SDL_free(devices);
        }
        if (device_id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
            spdlog::warn("Requested audio device '{}' not found; using default", device_name);
        }
    }

    sdl_stream = SDL_OpenAudioDeviceStream(device_id, &spec, AudioEngine::sdl_audio_callback, this);
    if (!sdl_stream) {
        spdlog::error("Failed to open SDL audio device stream: {}", SDL_GetError());
        quit_sdl_audio_subsystem();
        return false;
    }

#ifdef YATAIDON_PLATFORM_IOS
    g_ios_audio_stream = sdl_stream;
#endif

    if (!SDL_ResumeAudioStreamDevice(sdl_stream)) {
        spdlog::error("Failed to start SDL audio stream: {}", SDL_GetError());
        SDL_DestroyAudioStream(sdl_stream);
        sdl_stream = nullptr;
        quit_sdl_audio_subsystem();
        return false;
    }

    is_ready = true;

    SDL_AudioSpec actual_spec{};
    int actual_frames = 0;
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sdl_stream), &actual_spec, &actual_frames);

    spdlog::info("Audio Device initialized successfully");
    spdlog::info("    > Backend:       SDL3 | {}", SDL_GetCurrentAudioDriver());
    spdlog::info("    > Format:        Float32");
    spdlog::info("    > Channels:      {} (requested 2)", actual_spec.channels);
    spdlog::info("    > Sample rate:   {} Hz (requested {} Hz)", actual_spec.freq, (int)target_sample_rate);
    spdlog::info("    > Buffer size:   {} frames (device-reported)", actual_frames);
    return true;
}

bool AudioEngine::init_audio_device(const fs::path& sounds_path, const AudioConfig& audio_config,
                                    const VolumeConfig& volume_presets) {
    if (is_ready) {
        close_audio_device();
    }

    this->sounds_path = sounds_path;
    this->target_sample_rate = audio_config.sample_rate <= 0 ? 44100.0 : audio_config.sample_rate;
    this->buffer_size = audio_config.buffer_size;
    this->device_name = audio_config.device;
    this->channel_offsets = audio_config.asio_channel.empty() ? std::vector<int>{0} : audio_config.asio_channel;
    this->volume_presets = volume_presets;

    loudness_scanner = std::make_unique<song_loudness::Scanner>("cache/loudness");

    this->is_ready = false;
    // Dev hook: YATAIDON_TEST=1 (automated test runs) plays everything at 1% volume
    this->master_volume = zdon_test_mode() ? 0.01f : 1.0f;
    // the backends' callbacks reuse this for multi-channel / SDL output: no allocation there
    sdl_scratch_buffer.resize(std::max<size_t>(sdl_scratch_buffer.size(), 16384));
    start_decoder();
    try {
        bool ok = false;
        bool chosen = false;
#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
        if (const RtApiEntry* entry = rt_api_for_type(audio_config.device_type)) {
            ok = init_rtaudio_device(entry->api, entry->label);
            chosen = true;
        }
#endif
#ifdef _WIN32
        if (!chosen) {
            switch (audio_config.device_type) {
            case 9:
                ok = win32_init_portaudio_wdmks(target_sample_rate, buffer_size, device_name);
                chosen = true;
                break;
            case 10:
                ok = win32_init_portaudio_mme(target_sample_rate, buffer_size, device_name);
                chosen = true;
                break;
            default:
                break;
            }
        }
#endif
        if (!chosen) {
            ok = init_sdl3_device();
        }
        if (ok) {
            audio_dev::start(target_sample_rate);
            if (!audio_dev::selftest_spec().empty()) {
                run_selftest(audio_dev::selftest_spec());
            }
        }
        return ok;
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize audio device: {}", e.what());
        return false;
    }
}

void AudioEngine::close_audio_device() {
    loudness_scanner.reset();
    audio_dev::stop();

    if (!is_ready) {
        return;
    }
    try {
        unload_all_sounds();
        unload_all_music();

        if (sdl_stream) {
            SDL_DestroyAudioStream(sdl_stream); // also closes the underlying device
            sdl_stream = nullptr;
        }
        if (sdl_audio_subsystem_initialized) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_audio_subsystem_initialized = false;
        }
#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
        if (rt_audio != nullptr) {
            if (rt_audio->isStreamRunning()) {
                rt_audio->stopStream();
            }
            if (rt_audio->isStreamOpen()) {
                rt_audio->closeStream();
            }
            delete rt_audio;
            rt_audio = nullptr;
        }
#endif
#ifdef _WIN32
        win32_close_portaudio();
#endif
        is_ready = false;

        spdlog::info("Audio device closed");
    } catch (const std::exception& e) {
        spdlog::error("Error closing audio device: {}", e.what());
    }
    // no callback runs any more: free what is still waiting for one to finish
    stop_decoder();
    reclaim(true);
}

bool AudioEngine::is_audio_device_ready() const {
    return is_ready;
}

std::string AudioEngine::path_to_string(const fs::path& path) const {
#ifdef _WIN32
    return win32_path_to_string(path);
#else
    return path.string();
#endif
}

// A decoded buffer handed over as raw float PCM, resampled to the device rate
// if it does not already match. Returns null on failure; frames and rate are
// updated in place.
static float* adopt_decoded_pcm(std::vector<float>& samples, int channels, unsigned int& frames, unsigned int& rate,
                                double target_rate, int converter = SRC_SINC_FASTEST) {
    if ((double)rate == target_rate) {
        float* data = new float[samples.size()];
        std::memcpy(data, samples.data(), samples.size() * sizeof(float));
        return data;
    }

    double ratio = target_rate / (double)rate;
    long out_frames = (long)(frames * ratio) + 1;
    float* resampled = new float[(size_t)out_frames * channels];
    SRC_DATA sd{};
    sd.data_in = samples.data();
    sd.input_frames = frames;
    sd.data_out = resampled;
    sd.output_frames = out_frames;
    sd.src_ratio = ratio;
    sd.end_of_input = 1;
    int err = src_simple(&sd, converter, channels);
    if (err) {
        delete[] resampled;
        spdlog::error("Resampling a decoded stream failed: {}", src_strerror(err));
        return nullptr;
    }
    frames = (unsigned int)sd.output_frames_gen;
    rate = (unsigned int)target_rate;
    return resampled;
}

// ---- deferred reclamation ----
// mix() bumps mix_epoch on entry and exit (odd = inside). Whatever a callback can reach is unlinked
// first (a seq_cst exchange) and then retired with the epoch seen afterwards: a callback that was
// already running may still hold it, one that starts later can't, so it is freed once the epoch
// shows that running callback has left.
void AudioEngine::retire(std::function<void()> free) {
    const uint64_t e = mix_epoch.load(std::memory_order_seq_cst);
    {
        std::scoped_lock g(retire_lock);
        retired.push_back(Retired{(e & 1) ? e + 1 : e, std::move(free)});
    }
    reclaim();
}

void AudioEngine::reclaim(bool force) {
    std::vector<std::function<void()>> ready;
    {
        std::scoped_lock g(retire_lock);
        if (retired.empty()) {
            return;
        }
        const uint64_t now = mix_epoch.load(std::memory_order_seq_cst);
        for (size_t i = 0; i < retired.size();) {
            if (force || now >= retired[i].safe_epoch) {
                ready.push_back(std::move(retired[i].free));
                retired[i] = std::move(retired.back());
                retired.pop_back();
            } else {
                i++;
            }
        }
    }
    if (ready.empty()) {
        return;
    }
    // the main-thread readers of a slot's PCM hold names_lock (shared): wait out any that may
    // have loaded the pointer before it was unlinked
    {
        std::unique_lock<std::shared_mutex> grace(names_lock);
    }
    for (auto& f : ready) {
        f();
    }
}

SoundId AudioEngine::find_sound(const std::string& name) const {
    std::shared_lock<std::shared_mutex> g(names_lock);
    auto it = sound_ids.find(name);
    return it == sound_ids.end() ? kNoSound : it->second;
}

SoundId AudioEngine::sound_id(const std::string& name) {
    {
        std::shared_lock<std::shared_mutex> g(names_lock);
        auto it = sound_ids.find(name);
        if (it != sound_ids.end()) {
            return it->second;
        }
    }
    std::unique_lock<std::shared_mutex> g(names_lock);
    auto it = sound_ids.find(name);
    if (it != sound_ids.end()) {
        return it->second;
    }
    const uint32_t id = slot_count.load(std::memory_order_relaxed);
    if (id >= kSlotChunk * kMaxSlotChunks) {
        static std::once_flag warned;
        std::call_once(warned, [] { spdlog::error("Too many sound names; further ones are ignored"); });
        return kNoSound;
    }
    if (id % kSlotChunk == 0) {
        slot_chunks[id / kSlotChunk].store(new SoundSlot[kSlotChunk], std::memory_order_release);
    }
    slot_count.store(id + 1, std::memory_order_release);
    sound_ids.emplace(name, id);
    return id;
}

// Runs fn(slot, pcm) for a loaded sound, holding names_lock (shared) so the PCM stays alive;
// false if there is no such sound.
template<class Fn>
bool AudioEngine::with_sound(const std::string& name, Fn&& fn) const {
    std::shared_lock<std::shared_mutex> g(names_lock);
    auto it = sound_ids.find(name);
    if (it == sound_ids.end()) {
        return false;
    }
    SoundSlot& s = *slot_at(it->second);
    sound* pcm = s.pcm.load(std::memory_order_acquire);
    if (!pcm) {
        return false;
    }
    fn(s, *pcm);
    return true;
}

void AudioEngine::store_sound(const std::string& name, const sound& snd) {
    const SoundId id = sound_id(name);
    if (id == kNoSound) {
        sound lost = snd;
        free_sound(lost);
        return;
    }
    SoundSlot& s = *slot_at(id);
    s.is_playing.store(false, std::memory_order_relaxed);
    sound* fresh = new sound(snd);
    s.current_frame.store(snd.current_frame, std::memory_order_relaxed);
    s.frame_frac.store(snd.frame_frac, std::memory_order_relaxed);
    s.loop.store(snd.loop, std::memory_order_relaxed);
    s.volume.store(snd.volume, std::memory_order_relaxed);
    s.playback_gain.store(snd.playback_gain, std::memory_order_relaxed);
    s.pan.store(snd.pan, std::memory_order_relaxed);
    s.pitch.store(snd.pitch, std::memory_order_relaxed);
    sound* old = s.pcm.exchange(fresh, std::memory_order_seq_cst);
    if (old) {
        retire([old] {
            free_sound(*old);
            delete old;
        });
    }
}

void AudioEngine::free_sound(sound& snd) {
    if (!stop_decode_job(snd)) {
        delete[] snd.data;
    }
    snd.data = nullptr;
    if (snd.resampler) {
        src_delete(snd.resampler);
        snd.resampler = nullptr;
    }
    delete[] snd.resample_buffer;
    snd.resample_buffer = nullptr;
}

bool AudioEngine::stop_decode_job(sound& snd) {
    if (!snd.decode_job) {
        return false;
    }
    std::shared_ptr<SoundDecodeJob> job = std::move(snd.decode_job);
    std::scoped_lock lock(job->state);
    job->cancel.store(true, std::memory_order_relaxed);
    if (job->decoder_exited) {
        return false; // nothing writes into the buffer any more: the caller frees it
    }
    job->orphaned = snd.data; // the decoder frees it when it stops (after its current chunk)
    return true;
}

// Z-DON: a long sound (a screen's bgm: Nijiiro's song select bgm.ogg took ~210 ms warm, ~450 ms
// from a cold disk to decode whole) decodes its first seconds here and the rest on a background
// thread, into the same buffer; it can start playing at once. Takes ownership of file.
std::string AudioEngine::load_sound_progressive(SNDFILE* file, const SF_INFO& info, const fs::path& file_path,
                                                const std::string& name) {
    const unsigned int channels = static_cast<unsigned int>(info.channels);
    const bool resample = static_cast<double>(info.samplerate) != target_sample_rate;
    const double ratio = resample ? target_sample_rate / static_cast<double>(info.samplerate) : 1.0;
    const size_t expected = static_cast<size_t>(static_cast<double>(info.frames) * ratio);
    const size_t capacity = expected + 64; // frames; room for the resampler's rounding

    struct Decoder {
        SNDFILE* file = nullptr;
        SRC_STATE* src = nullptr;
        float* out = nullptr;
        size_t capacity = 0;
        size_t written = 0;
        unsigned int channels = 0;
        double ratio = 1.0;
        bool eof = false;
        std::vector<float> in;

        // decodes up to frames source frames; false once the whole file is in
        bool step(sf_count_t frames) {
            if (eof) {
                return false;
            }
            if (!src) {
                const sf_count_t room = static_cast<sf_count_t>(capacity - written);
                const sf_count_t n = sf_readf_float(file, out + written * channels, std::min(frames, room));
                written += static_cast<size_t>(std::max<sf_count_t>(n, 0));
                eof = n < std::min(frames, room) || written >= capacity;
                return !eof;
            }
            in.resize(static_cast<size_t>(frames) * channels);
            const sf_count_t n = sf_readf_float(file, in.data(), frames);
            eof = n < frames;
            SRC_DATA d{};
            d.data_in = in.data();
            d.input_frames = static_cast<long>(std::max<sf_count_t>(n, 0));
            d.src_ratio = ratio;
            d.end_of_input = eof ? 1 : 0;
            do {
                d.data_out = out + written * channels;
                d.output_frames = static_cast<long>(capacity - written);
                if (d.output_frames <= 0 || src_process(src, &d) != 0) {
                    eof = true;
                    return false;
                }
                written += static_cast<size_t>(d.output_frames_gen);
                if (d.input_frames_used == 0 && d.output_frames_gen == 0) {
                    break;
                }
                d.data_in += d.input_frames_used * channels;
                d.input_frames -= d.input_frames_used;
                // at the end, keep draining the resampler until it has nothing left
            } while (d.input_frames > 0 || (eof && d.output_frames_gen > 0));
            return !eof;
        }
    };

    auto dec = std::make_shared<Decoder>();
    dec->file = file;
    dec->capacity = capacity;
    dec->channels = channels;
    dec->ratio = ratio;
    if (resample) {
        int err = 0;
        dec->src = src_new(SRC_SINC_FASTEST, static_cast<int>(channels), &err);
        if (!dec->src) {
            spdlog::error("Failed to create resampler for {}: {}", file_path.string(), src_strerror(err));
            sf_close(file);
            return "";
        }
    }
    dec->out = new float[capacity * channels]; // only the frames_ready part is ever read

    // the first seconds now, so play_sound right after this has something to play
    const sf_count_t head = static_cast<sf_count_t>(info.samplerate) * kProgressiveHeadSeconds;
    const bool more = dec->step(head);

    auto job = std::make_shared<SoundDecodeJob>();
    job->frames_ready.store(static_cast<unsigned int>(dec->written), std::memory_order_release);

    sound snd{};
    snd.data = dec->out;
    snd.frame_count = static_cast<unsigned int>(more ? expected : dec->written);
    snd.sample_rate = resample ? static_cast<unsigned int>(target_sample_rate) : info.samplerate;
    snd.channels = channels;
    snd.decode_job = job;
    snd.normalization_gain = loaded_sound_gain(file_path, name);

    auto finish = [](Decoder& d) {
        sf_close(d.file);
        if (d.src) {
            src_delete(d.src);
        }
    };
    if (!more) {
        job->done.store(true, std::memory_order_release);
        finish(*dec);
        store_sound(name, snd);
        return name;
    }
    store_sound(name, snd);

    std::thread([dec, job, finish, path = file_path.string()]() {
        const sf_count_t chunk = 1 << 15; // ~0.7 s at 44.1 kHz
        // writes only past frames_ready, which nothing else reads; an unload while this runs
        // leaves the buffer to this thread (SoundDecodeJob::orphaned)
        while (!job->cancel.load(std::memory_order_relaxed)) {
            const bool more = dec->step(chunk);
            job->frames_ready.store(static_cast<unsigned int>(dec->written), std::memory_order_release);
            if (!more) {
                job->done.store(true, std::memory_order_release);
                spdlog::debug("Decoded the rest of {} ({} frames)", path, dec->written);
                break;
            }
        }
        finish(*dec);
        float* orphaned = nullptr;
        {
            std::scoped_lock lock(job->state);
            job->decoder_exited = true;
            orphaned = job->orphaned;
        }
        delete[] orphaned;
    }).detach();

    spdlog::debug("Loading sound in the background: {} ({} frames ready of ~{})", name, dec->written, expected);
    return name;
}

// Opens an audio file for reading (wide-char path first on Windows); zeroes `info` first. Null on failure.
static SNDFILE* open_sound_file(const fs::path& file_path, SF_INFO& info) {
    std::memset(&info, 0, sizeof(SF_INFO));

    SNDFILE* file = nullptr;
#ifdef _WIN32
    file = sf_wchar_open(file_path.wstring().c_str(), SFM_READ, &info);
#endif

    if (!file) {
#ifdef _WIN32
        std::string path_str = win32_path_to_string(file_path);
#else
        std::string path_str = file_path.string();
#endif
        file = sf_open(path_str.c_str(), SFM_READ, &info);
    }
    return file;
}

void AudioEngine::queue_song_loudness(const fs::path& file_path, bool priority) {
    if (loudness_scanner) {
        loudness_scanner->enqueue(file_path, priority);
    }
}

float AudioEngine::loaded_sound_gain(const fs::path& file_path, const std::string& name) {
    if (name != "song") {
        return song_loudness::skin_gain();
    }
    return loaded_song_gain(file_path);
}

float AudioEngine::loaded_song_gain(const fs::path& file_path) {
    return loudness_scanner ? loudness_scanner->gain_for(file_path) : song_loudness::fallback_gain();
}

std::string AudioEngine::load_sound(const fs::path& file_path, const std::string& name) {
    try {
        if (name == "song") {
            queue_song_loudness(file_path, true);
        }
#ifdef SUPPORT_FUMEN
        if (is_bank_file(file_path)) {
            gen4::DecodedAudio decoded;
            if (!decode_bank(file_path, decoded)) {
                return "";
            }

            unsigned int frames = (unsigned int)decoded.frame_count();
            unsigned int rate = (unsigned int)decoded.sample_rate;
            float* data = adopt_decoded_pcm(decoded.samples, decoded.channels, frames, rate, target_sample_rate);
            if (!data) {
                return "";
            }

            sound snd{};
            snd.data = data;
            snd.frame_count = frames;
            snd.sample_rate = rate;
            snd.channels = decoded.channels;
            snd.normalization_gain = loaded_sound_gain(file_path, name);
            store_sound(name, snd);
            spdlog::info("Loaded sound (G.719): {} ({} frames, {} Hz, {} ch)", name, frames, rate, snd.channels);
            return name;
        }
#endif

        SF_INFO file_info;
        SNDFILE* file = open_sound_file(file_path, file_info);

        if (!file) {
#if defined(__ANDROID__) || defined(YATAIDON_PLATFORM_IOS)
            float* ff_data = nullptr;
            sf_count_t ff_frames = 0;
            unsigned int ff_rate = 0, ff_ch = 0;
            std::string path_str2 = path_to_string(file_path);
            if (!ffmpeg_decode_float(path_str2.c_str(), &ff_data, &ff_frames, &ff_rate, &ff_ch)) {
                spdlog::error("Failed to open sound file: {} - {} (ffmpeg fallback also failed)", file_path.string(),
                              sf_strerror(NULL));
                return "";
            }
            sound snd{};
            snd.data = ff_data;
            snd.frame_count = ff_frames;
            snd.sample_rate = ff_rate;
            snd.channels = ff_ch;
            if ((double)ff_rate != target_sample_rate) {
                double ratio = target_sample_rate / (double)ff_rate;
                long out_frames = (long)(ff_frames * ratio) + 1;
                float* rs = new float[out_frames * ff_ch];
                SRC_DATA sd;
                sd.data_in = ff_data;
                sd.input_frames = ff_frames;
                sd.data_out = rs;
                sd.output_frames = out_frames;
                sd.src_ratio = ratio;
                sd.end_of_input = 1;
                int err = src_simple(&sd, SRC_SINC_FASTEST, (int)ff_ch);
                if (err) {
                    delete[] ff_data;
                    delete[] rs;
                    return "";
                }
                delete[] ff_data;
                snd.data = rs;
                snd.frame_count = sd.output_frames_gen;
                snd.sample_rate = (unsigned int)target_sample_rate;
            }
            snd.normalization_gain = loaded_sound_gain(file_path, name);
            store_sound(name, snd);
            spdlog::debug("Loaded sound (ffmpeg): {} ({} frames, {} Hz, {} ch)", name, snd.frame_count, snd.sample_rate,
                          snd.channels);
            return name;
#else
            spdlog::error("Failed to open sound file: {} - {}", file_path.string(), sf_strerror(NULL));
            return "";
#endif
        }

        const sf_count_t total_frames_sf = file_info.frames;
        const unsigned int channels = static_cast<unsigned int>(file_info.channels);
        if (total_frames_sf <= 0 || channels == 0) {
            sf_close(file);
            return "";
        }
        unsigned int total_frames = static_cast<unsigned int>(total_frames_sf);
        if (total_frames_sf > static_cast<sf_count_t>(file_info.samplerate) * kProgressiveMinSeconds) {
            return load_sound_progressive(file, file_info, file_path, name);
        }
        float* data = new float[static_cast<size_t>(total_frames_sf) * channels];

        sf_count_t frames_read = sf_readf_float(file, data, total_frames);
        sf_close(file);

        if (frames_read != total_frames) {
            spdlog::warn("Read {} frames, expected {} for sound: {}", frames_read, total_frames, file_path.string());
        }

        sound snd{};
        snd.data = data;
        snd.frame_count = frames_read;
        snd.sample_rate = file_info.samplerate;
        snd.channels = channels;

        if (snd.sample_rate != target_sample_rate) {
            double ratio = target_sample_rate / (double)snd.sample_rate;
            long output_frames = (long)(frames_read * ratio) + 1;
            float* resampled_data = new float[output_frames * channels];

            SRC_DATA src_data;
            src_data.data_in = data;
            src_data.input_frames = frames_read;
            src_data.data_out = resampled_data;
            src_data.output_frames = output_frames;
            src_data.src_ratio = ratio;
            src_data.end_of_input = 1;

            int error = src_simple(&src_data, SRC_SINC_FASTEST, channels);
            if (error) {
                spdlog::error("Failed to resample sound: {} - {}", file_path.string(), src_strerror(error));
                delete[] data;
                delete[] resampled_data;
                return "";
            }

            delete[] data;
            snd.data = resampled_data;
            snd.frame_count = src_data.output_frames_gen;
            snd.sample_rate = target_sample_rate;
        }

        snd.normalization_gain = loaded_sound_gain(file_path, name);

        store_sound(name, snd);

        spdlog::debug("Loaded sound: {} ({} frames, {} Hz, {} channels)", name, snd.frame_count, snd.sample_rate,
                      snd.channels);

        return name;

    } catch (const std::exception& e) {
        spdlog::error("Error loading sound {}: {}", file_path.string(), e.what());
        return "";
    }
}

void AudioEngine::load_screen_sounds(const std::string& screen_name) {
    std::vector<std::pair<fs::path, std::string>> queue;
    std::unordered_set<std::string> claimed;

    auto want = [&](const fs::path& file, const std::string& name) {
        if (!claimed.insert(name).second) {
            return;
        }
        if (!fs::exists(file)) {
            claimed.erase(name);
            return;
        }
        queue.emplace_back(file, name);
    };

    auto scan = [&](const fs::path& dir) {
        if (!fs::exists(dir)) {
            return;
        }
        try {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_directory()) {
                    for (const auto& file : fs::directory_iterator(entry.path())) {
                        want(file.path(), entry.path().stem().string() + "_" + file.path().stem().string());
                    }
                } else if (entry.is_regular_file()) {
                    want(entry.path(), entry.path().stem().string());
                }
            }
        } catch (const fs::filesystem_error& e) {
            spdlog::error("load_screen_sounds: error scanning {}: {}", dir.string(), e.what());
        }
    };

    // the ancestors' Sounds (parent, its parent, ...), nearest first: the first file claims a name
    std::vector<fs::path> ancestor_sounds;
    for (const fs::path& root : skin_ancestor_roots()) {
        ancestor_sounds.push_back(root / "Sounds");
    }

    fs::path path = sounds_path / screen_name;
    // A screen without its own sound folder still gets don/kat and the
    // global folder - those are screen-independent. Returning here also
    // took those away, leaving such screens (input_test, loading) silent.
    bool has_screen_sounds = fs::exists(path);
    for (const fs::path& dir : ancestor_sounds) {
        has_screen_sounds = has_screen_sounds || fs::exists(dir / screen_name);
    }
    if (!has_screen_sounds) {
        spdlog::warn("Sounds for screen {} not found", screen_name);
    }

    want(sounds_path / "don.wav", "don");
    for (const fs::path& dir : ancestor_sounds) {
        want(dir / "don.wav", "don");
    }
    want(sounds_path / "ka.wav", "kat");
    for (const fs::path& dir : ancestor_sounds) {
        want(dir / "ka.wav", "kat");
    }

    if (has_screen_sounds) {
        scan(path);
        for (const fs::path& dir : ancestor_sounds) {
            scan(dir / screen_name);
        }
    }

    scan(sounds_path / "global");
    for (const fs::path& dir : ancestor_sounds) {
        scan(dir / "global");
    }

    unsigned hw = std::thread::hardware_concurrency();
    size_t workers = std::min<size_t>(queue.size(), hw ? hw : 4);
    if (workers <= 1) {
        for (const auto& [file, name] : queue) {
            load_sound(file, name);
        }
    } else {
        jobs::parallel_for(
            queue.size(), jobs::Priority::Critical,
            [this, &queue](size_t i) { load_sound(queue[i].first, queue[i].second); }, workers);
    }
}

void AudioEngine::unload_sound(const std::string& name) {
    if (fixed_step::on()) {
        fixed_step::audio_stop(name, false);
    }
    const SoundId id = find_sound(name);
    SoundSlot* s = id == kNoSound ? nullptr : slot_at(id);
    if (!s || !s->pcm.load(std::memory_order_acquire)) {
        spdlog::warn("Sound {} not found", name);
        return;
    }
    s->is_playing.store(false, std::memory_order_relaxed);
    sound* old = s->pcm.exchange(nullptr, std::memory_order_seq_cst);
    if (old) {
        retire([old] {
            free_sound(*old);
            delete old;
        });
    }
}

void AudioEngine::unload_all_sounds() {
    std::vector<std::string> names;
    {
        std::shared_lock<std::shared_mutex> g(names_lock);
        for (const auto& [name, id] : sound_ids) {
            if (slot_at(id)->pcm.load(std::memory_order_acquire)) {
                names.push_back(name);
            }
        }
    }
    for (const auto& name : names) {
        unload_sound(name);
    }

    spdlog::info("All sounds unloaded");
}

static float preset_volume(const VolumeConfig& presets, VolumePreset preset) {
    switch (preset) {
    case VolumePreset::SOUND:
        return presets.sound;
    case VolumePreset::MUSIC:
        return presets.music;
    case VolumePreset::VOICE:
        return presets.voice;
    case VolumePreset::HITSOUND:
        return presets.hitsound;
    case VolumePreset::ATTRACT_MODE:
        return presets.attract_mode;
    default:
        return 1.0f;
    }
}

uint64_t AudioEngine::sound_play_count(const std::string& name) {
    const SoundId id = find_sound(name);
    return id == kNoSound ? 0 : slot_at(id)->play_count.load(std::memory_order_relaxed);
}

void AudioEngine::play_sound(const std::string& name, VolumePreset volume_preset, float gain) {
    if (fixed_step::on() && has_sound(name)) {
        fixed_step::audio_play(name, false);
    }
    play_sound(sound_id(name), volume_preset, gain);
}

void AudioEngine::play_sound(SoundId id, VolumePreset volume_preset, float gain) {
    if (id == kNoSound) {
        return;
    }
    SoundSlot& snd = *slot_at(id);
    snd.play_count.fetch_add(1, std::memory_order_relaxed);
    if (!snd.pcm.load(std::memory_order_acquire)) {
        return;
    }
    if (volume_preset != VolumePreset::NONE) {
        snd.volume.store(preset_volume(volume_presets, volume_preset), std::memory_order_relaxed);
    }
    snd.playback_gain.store(std::max(gain, 0.0f), std::memory_order_relaxed);
    snd.current_frame.store(0, std::memory_order_relaxed);
    snd.frame_frac.store(0.0f, std::memory_order_relaxed);
    snd.is_playing.store(true, std::memory_order_release);
    activate(id);
}

// Vyukov's bounded MPMC queue (one consumer: the mixer). A full queue makes the mixer rescan.
void AudioEngine::activate(SoundId id) {
    size_t pos = activate_head.load(std::memory_order_relaxed);
    for (;;) {
        ActivateCell& cell = activate_cells[pos % kActivateCap];
        const size_t seq = cell.seq.load(std::memory_order_acquire);
        const intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            if (activate_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.id = id;
                cell.seq.store(pos + 1, std::memory_order_release);
                return;
            }
        } else if (dif < 0) {
            activate_rescan.store(true, std::memory_order_release);
            return;
        } else {
            pos = activate_head.load(std::memory_order_relaxed);
        }
    }
}

bool AudioEngine::has_sound(const std::string& name) {
    return with_sound(name, [](SoundSlot&, const sound&) {});
}

void AudioEngine::stop_sound(const std::string& name) {
    if (fixed_step::on()) {
        fixed_step::audio_stop(name, false);
    }
    const bool found = with_sound(name, [](SoundSlot& snd, const sound&) {
        snd.is_playing.store(false, std::memory_order_relaxed);
        snd.current_frame.store(0, std::memory_order_relaxed);
        snd.frame_frac.store(0.0f, std::memory_order_relaxed);
    });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
}

void AudioEngine::set_sound_loop(const std::string& name, bool loop) {
    if (fixed_step::on()) {
        fixed_step::audio_set_loop(name, loop);
    }
    with_sound(name, [loop](SoundSlot& snd, const sound&) { snd.loop.store(loop, std::memory_order_relaxed); });
}

bool AudioEngine::is_sound_playing(const std::string& name) {
    if (fixed_step::on()) {
        return has_sound(name) && fixed_step::audio_playing(name, false, get_sound_time_length(name));
    }
    bool playing = false;
    const bool found = with_sound(
        name, [&playing](SoundSlot& snd, const sound&) { playing = snd.is_playing.load(std::memory_order_relaxed); });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
    return playing;
}

void AudioEngine::set_sound_volume(const std::string& name, float volume) {
    const bool found = with_sound(name, [volume](SoundSlot& snd, const sound&) {
        snd.volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
}

void AudioEngine::set_sound_pan(const std::string& name, float pan) {
    const bool found = with_sound(name, [pan](SoundSlot& snd, const sound&) {
        snd.pan.store(std::clamp(pan, 0.0f, 1.0f), std::memory_order_relaxed);
    });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
}

void AudioEngine::set_sound_pitch(const std::string& name, float pitch) {
    if (fixed_step::on()) {
        fixed_step::audio_set_pitch(name, std::isfinite(pitch) ? std::clamp(pitch, 0.01f, 16.0f) : 1.0f);
    }
    const bool found = with_sound(name, [pitch](SoundSlot& snd, const sound&) {
        const float p = std::isfinite(pitch) ? pitch : 1.0f;
        snd.pitch.store(std::clamp(p, 0.01f, 16.0f), std::memory_order_relaxed);
    });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
}

float AudioEngine::get_sound_time_played(const std::string& name) const {
    if (fixed_step::on()) {
        return fixed_step::audio_time(name, false, get_sound_time_length(name));
    }
    float seconds = 0.0f;
    const bool found = with_sound(name, [&](SoundSlot& snd, const sound&) {
        unsigned int frame = snd.current_frame.load(std::memory_order_relaxed);
        seconds = static_cast<float>(frame) / static_cast<float>(target_sample_rate);
    });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
    return seconds;
}

float AudioEngine::get_sound_time_length(const std::string& name) const {
    float seconds = 0.0f;
    const bool found = with_sound(name, [&](SoundSlot&, const sound& pcm) {
        seconds = static_cast<float>(pcm.frame_count) / static_cast<float>(target_sample_rate);
    });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
    return seconds;
}

void AudioEngine::seek_sound(const std::string& name, float position) {
    if (fixed_step::on()) {
        fixed_step::audio_seek(name, false, position);
    }
    const bool found = with_sound(name, [&](SoundSlot& snd, const sound& pcm) {
        unsigned int frame =
            static_cast<unsigned int>(std::max(position, 0.0f) * static_cast<float>(target_sample_rate));
        snd.current_frame.store(std::min(frame, pcm.frame_count), std::memory_order_relaxed);
        snd.frame_frac.store(0.0f, std::memory_order_relaxed);
    });
    if (!found) {
        spdlog::warn("Sound {} not found", name);
    }
}

bool AudioEngine::prepare_nus3bank_pcm(const fs::path& file_path, PreparedPCM& out, bool quick_resample) {
#ifdef SUPPORT_FUMEN
    gen4::DecodedAudio decoded;
    if (!decode_bank(file_path, decoded)) {
        return false;
    }

    unsigned int frames = (unsigned int)decoded.frame_count();
    unsigned int rate = (unsigned int)decoded.sample_rate;
    float* data = adopt_decoded_pcm(decoded.samples, decoded.channels, frames, rate, target_sample_rate,
                                    quick_resample ? SRC_LINEAR : SRC_SINC_FASTEST);
    if (!data) {
        return false;
    }

    out.data.reset(data);
    out.frames = frames;
    out.rate = rate;
    out.channels = decoded.channels;
    out.preview_ms = decoded.preview_ms;
    out.source_path = file_path.string();
    return true;
#else
    (void)file_path;
    (void)out;
    (void)quick_resample;
    return false;
#endif
}

music::~music() {
    if (file_handle) {
        sf_close(file_handle);
    }
    if (resampler) {
        src_delete(resampler);
    }
    delete[] pcm_data;
    delete[] stream_buffer;
    delete[] resample_buffer;
}

// Frames one decoder read can produce at most (the resampler's budget, as the mixer used to have it).
static unsigned long long stream_max_out(const music& mus, double target_rate) {
    if (!mus.resampler) {
        return mus.buffer_size;
    }
    return (unsigned long long)(mus.buffer_size * (target_rate / (double)mus.file_info.samplerate)) + 256;
}

// Sets up the decode ring of a file-backed stream (~370 ms at 44.1 kHz).
static void alloc_stream_ring(music& mus, double target_rate) {
    mus.ring_frames = std::max<unsigned long long>(16384, stream_max_out(mus, target_rate) * 2);
    mus.ring = std::make_unique<float[]>(mus.ring_frames * (unsigned long long)mus.file_info.channels);
}

std::shared_ptr<music> AudioEngine::find_music(const std::string& name) const {
    std::scoped_lock g(music_lock);
    auto it = music_streams.find(name);
    return it == music_streams.end() ? nullptr : it->second;
}

// Under music_lock: the callback's stream list follows music_streams. Returns the old list, to retire.
AudioEngine::StreamList* AudioEngine::publish_streams() {
    auto* list = new StreamList();
    list->streams.reserve(music_streams.size());
    for (auto& [name, mus] : music_streams) {
        list->streams.push_back(mus.get());
    }
    return mixer_streams.exchange(list, std::memory_order_seq_cst);
}

// Replaces any music stream stored under name with mus.
void AudioEngine::install_music(const std::string& name, std::shared_ptr<music> mus) {
    unload_music_stream(name);
    StreamList* old_list = nullptr;
    std::shared_ptr<music> replaced;
    {
        std::scoped_lock g(music_lock);
        auto& entry = music_streams[name];
        replaced = std::move(entry); // only if another thread installed the same name meanwhile
        entry = mus;
        old_list = publish_streams();
    }
    if (mus->file_handle) {
        start_decoder();
        {
            std::scoped_lock g(decoder_mutex);
            decoder_streams.push_back(mus);
            if (replaced) {
                std::erase(decoder_streams, replaced);
            }
            decoder_kick = true;
        }
        decoder_cv.notify_one();
    }
    retire([old_list, replaced]() mutable {
        delete old_list;
        replaced.reset();
    });
}

void AudioEngine::unload_music_stream(const std::string& name) {
    if (fixed_step::on()) {
        fixed_step::audio_stop(name, true);
    }
    std::shared_ptr<music> old;
    StreamList* old_list = nullptr;
    {
        std::scoped_lock g(music_lock);
        auto it = music_streams.find(name);
        if (it == music_streams.end()) {
            spdlog::warn("Music stream {} not found", name);
            return;
        }
        old = std::move(it->second);
        music_streams.erase(it);
        old_list = publish_streams();
    }
    old->is_playing.store(false, std::memory_order_relaxed);
    if (old->file_handle) {
        std::scoped_lock g(decoder_mutex);
        std::erase(decoder_streams, old);
    }
    // the decoder may still be inside a read of it: it holds its own reference until done
    retire([old_list, old]() mutable {
        delete old_list;
        old.reset();
    });
    spdlog::debug("Unloaded music stream: {}", name);
}

void AudioEngine::unload_all_music() {
    std::vector<std::string> names;
    {
        std::scoped_lock g(music_lock);
        for (const auto& entry : music_streams) {
            names.push_back(entry.first);
        }
    }
    for (const auto& name : names) {
        unload_music_stream(name);
    }

    spdlog::info("All music streams unloaded");
}

// ---- the stream decoder thread: file reads, decoding and resampling, off the callback ----
void AudioEngine::start_decoder() {
    std::scoped_lock g(decoder_mutex);
    if (decoder_running) {
        return;
    }
    decoder_running = true;
    decoder_thread = std::thread([this] { decoder_main(); });
}

void AudioEngine::stop_decoder() {
    {
        std::scoped_lock g(decoder_mutex);
        if (!decoder_running) {
            return;
        }
        decoder_running = false;
    }
    decoder_cv.notify_one();
    if (decoder_thread.joinable()) {
        decoder_thread.join();
    }
}

void AudioEngine::kick_decoder() {
    {
        std::scoped_lock g(decoder_mutex);
        decoder_kick = true;
    }
    decoder_cv.notify_one();
}

// One read of a file-backed stream into its ring; false when there was no room or nothing more to read.
bool AudioEngine::decode_into_ring(music& mus, double target_rate) {
    const unsigned int channels = (unsigned int)mus.file_info.channels;
    const unsigned long long wp = mus.write_pos.load(std::memory_order_relaxed);
    const unsigned long long used = wp - mus.read_pos.load(std::memory_order_acquire);
    if (mus.decoder_eof || mus.ring_frames - used < stream_max_out(mus, target_rate)) {
        return false;
    }

    auto end_of_stream = [&mus]() {
        mus.decoder_eof = true;
        mus.eof.store(true, std::memory_order_release);
        return false;
    };

    sf_count_t frames_read = sf_readf_float(mus.file_handle, mus.stream_buffer, mus.buffer_size);
    if (frames_read == 0) {
        if (!mus.loop) {
            return end_of_stream();
        }
        sf_seek(mus.file_handle, 0, SEEK_SET);
        frames_read = sf_readf_float(mus.file_handle, mus.stream_buffer, mus.buffer_size);
        if (mus.resampler) {
            src_reset(mus.resampler);
        }
        if (frames_read == 0) {
            // Empty/broken stream even after seeking to the start -- stop
            return end_of_stream();
        }
    }

    const float* produced = mus.stream_buffer;
    unsigned long long frames = (unsigned long long)frames_read;
    if (mus.resampler) {
        double ratio = target_rate / (double)mus.file_info.samplerate;
        const long output_budget = (long)(frames_read * ratio) + 256;

        sf_count_t input_consumed = 0;
        long output_generated = 0;

        while (input_consumed < frames_read && output_generated < output_budget) {
            SRC_DATA src_data;
            src_data.data_in = mus.stream_buffer + input_consumed * channels;
            src_data.input_frames = frames_read - input_consumed;
            src_data.data_out = mus.resample_buffer + output_generated * channels;
            src_data.output_frames = output_budget - output_generated;
            src_data.src_ratio = ratio;
            src_data.end_of_input = 0;

            int error = src_process(mus.resampler, &src_data);
            if (error) {
                spdlog::error("Resampling error for music stream {}: {}", mus.file_path, src_strerror(error));
                return end_of_stream();
            }

            input_consumed += src_data.input_frames_used;
            output_generated += src_data.output_frames_gen;

            if (src_data.input_frames_used == 0 && src_data.output_frames_gen == 0) {
                spdlog::error("Resampler stalled for music stream {}", mus.file_path);
                return end_of_stream();
            }
        }
        produced = mus.resample_buffer;
        frames = (unsigned long long)output_generated;
    }

    for (unsigned long long done = 0; done < frames;) {
        const unsigned long long at = (wp + done) % mus.ring_frames;
        const unsigned long long n = std::min(frames - done, mus.ring_frames - at);
        std::memcpy(mus.ring.get() + at * channels, produced + done * channels, n * channels * sizeof(float));
        done += n;
    }
    mus.write_pos.store(wp + frames, std::memory_order_release);
    return true;
}

void AudioEngine::decoder_main() {
    std::vector<std::shared_ptr<music>> local;
    std::unique_lock<std::mutex> lk(decoder_mutex);
    while (decoder_running) {
        local = decoder_streams;
        decoder_kick = false;
        lk.unlock();

        bool busy = false;
        for (auto& mp : local) {
            music& mus = *mp;
            const uint32_t gen = mus.cmd_gen.load(std::memory_order_acquire);
            if (gen != mus.decoder_gen) { // a seek or stop
                sf_seek(mus.file_handle, mus.cmd_source_frame.load(std::memory_order_relaxed), SEEK_SET);
                if (mus.resampler) {
                    src_reset(mus.resampler);
                }
                mus.decoder_gen = gen;
                mus.decoder_eof = false;
                mus.eof.store(false, std::memory_order_relaxed);
                mus.flush_pos.store(mus.write_pos.load(std::memory_order_relaxed), std::memory_order_relaxed);
                mus.ring_gen.store(gen, std::memory_order_release);
                busy = true;
            }
            // a few reads, then the next stream; come back at once while there is room
            for (int k = 0; k < 4 && mus.cmd_gen.load(std::memory_order_relaxed) == gen; k++) {
                if (!decode_into_ring(mus, target_sample_rate)) {
                    break;
                }
                busy = true;
            }
        }
        local.clear(); // an unloaded stream may be destroyed here, off the main thread
        reclaim();

        lk.lock();
        if (!busy && decoder_running && !decoder_kick) {
            decoder_cv.wait_for(lk, std::chrono::milliseconds(5));
        }
    }
}

std::string AudioEngine::load_music_stream_prepared(PreparedPCM&& pcm, const std::string& name) {
    if (!pcm.data || pcm.frames == 0 || pcm.channels <= 0) {
        return "";
    }

    auto mus = std::make_shared<music>();
    mus->file_info.channels = pcm.channels;
    mus->file_info.samplerate = (int)pcm.rate;
    mus->file_path = pcm.source_path;
    mus->normalization_gain = name == "preview" ? loaded_song_gain(pcm.source_path) : 1.0f;
    mus->pcm_data = pcm.data.release();
    mus->pcm_total_frames = pcm.frames;
    install_music(name, mus);
    spdlog::info("Loaded music stream (G.719): {} ({} frames, {} Hz, {} ch)", name, pcm.frames, pcm.rate, pcm.channels);
    return name;
}

std::string AudioEngine::load_music_stream(const fs::path& file_path, const std::string& name) {
    try {
#ifdef SUPPORT_FUMEN
        if (is_bank_file(file_path)) {
            PreparedPCM pcm;
            if (!prepare_nus3bank_pcm(file_path, pcm)) {
                return "";
            }
            return load_music_stream_prepared(std::move(pcm), name);
        }
#endif

        SF_INFO file_info;
        SNDFILE* file = open_sound_file(file_path, file_info);

        if (!file) {
#if defined(__ANDROID__) || defined(YATAIDON_PLATFORM_IOS)
            float* ff_data = nullptr;
            sf_count_t ff_frames = 0;
            unsigned int ff_rate = 0, ff_ch = 0;
            std::string path_str2 = path_to_string(file_path);
            if (!ffmpeg_decode_float(path_str2.c_str(), &ff_data, &ff_frames, &ff_rate, &ff_ch)) {
                spdlog::error("Failed to open music file: {} - {} (ffmpeg fallback also failed)", file_path.string(),
                              sf_strerror(NULL));
                return "";
            }

            if ((double)ff_rate != target_sample_rate) {
                double ratio = target_sample_rate / (double)ff_rate;
                long out_frames = (long)(ff_frames * ratio) + 1;
                float* rs = new float[out_frames * ff_ch];
                SRC_DATA sd{};
                sd.data_in = ff_data;
                sd.input_frames = ff_frames;
                sd.data_out = rs;
                sd.output_frames = out_frames;
                sd.src_ratio = ratio;
                sd.end_of_input = 1;
                int err = src_simple(&sd, SRC_SINC_FASTEST, (int)ff_ch);
                if (err) {
                    delete[] ff_data;
                    delete[] rs;
                    return "";
                }
                delete[] ff_data;
                ff_data = rs;
                ff_frames = sd.output_frames_gen;
                ff_rate = (unsigned int)target_sample_rate;
            }

            auto mus = std::make_shared<music>();
            mus->file_info.channels = (int)ff_ch;
            mus->file_info.samplerate = (int)ff_rate;
            mus->file_path = file_path.string();
            mus->normalization_gain = name == "preview" ? loaded_song_gain(file_path) : 1.0f;
            mus->pcm_data = ff_data;
            mus->pcm_total_frames = ff_frames;
            install_music(name, mus);
            spdlog::debug("Loaded music stream (ffmpeg): {} ({} frames, {} Hz, {} ch)", name, ff_frames, ff_rate,
                          ff_ch);
            return name;
#else
            spdlog::error("Failed to open music file: {} - {}", file_path.string(), sf_strerror(NULL));
            return "";
#endif
        }

        auto mus = std::make_shared<music>();
        mus->file_handle = file;
        mus->file_info = file_info;
        mus->file_path = file_path.string();
        mus->normalization_gain = name == "preview" ? loaded_song_gain(file_path) : 1.0f;

        mus->buffer_size = 4096; // source frames per decoder read
        mus->stream_buffer = new float[mus->buffer_size * file_info.channels];

        if (file_info.samplerate != target_sample_rate) {
            int error;
            mus->resampler = src_new(SRC_SINC_FASTEST, file_info.channels, &error);
            if (!mus->resampler) {
                spdlog::error("Failed to create resampler for music stream {}: {}", name, src_strerror(error));
                return ""; // ~music closes the file
            }

            double ratio = target_sample_rate / (double)file_info.samplerate;
            unsigned int resample_buffer_size = (unsigned int)(mus->buffer_size * ratio) + 256;
            mus->resample_buffer = new float[resample_buffer_size * file_info.channels];

            spdlog::info("Music stream {} will be resampled from {} Hz to {} Hz", name, file_info.samplerate,
                         target_sample_rate);
        }
        alloc_stream_ring(*mus, target_sample_rate);

        install_music(name, mus);

        spdlog::debug("Loaded music stream: {} ({} frames, {} Hz, {} channels)", name, file_info.frames,
                      file_info.samplerate, file_info.channels);

        return name;

    } catch (const std::exception& e) {
        spdlog::error("Error loading music stream {}: {}", file_path.string(), e.what());
        return "";
    }
}

#ifndef __EMSCRIPTEN__
std::string AudioEngine::load_music_stream_memory(const av::AVAudioStream& audio_stream, const std::string& name) {
    try {
        std::vector<uint8_t> encoded = audio_stream.encoded_bytes();
        if (encoded.empty()) {
            spdlog::error("load_music_stream_memory: no audio data for stream '{}'", name);
            return "";
        }

        // the stream lives on the heap, so vio_cursor keeps the address sf_open_virtual stores
        auto mus = std::make_shared<music>();
        mus->memory_buffer = std::make_shared<std::vector<uint8_t>>(std::move(encoded));
        mus->file_path = "<memory:" + name + ">";
        mus->buffer_size = 4096;
        mus->vio_cursor = VirtualFile{mus->memory_buffer, 0};

        // a previous stream of this name goes even if this one fails to open
        if (is_music_stream_valid(name)) {
            unload_music_stream(name);
        }

        SF_VIRTUAL_IO vio{};
        vio.get_filelen = vf_get_filelen;
        vio.seek = vf_seek;
        vio.read = vf_read;
        vio.write = vf_write;
        vio.tell = vf_tell;

        SF_INFO file_info{};

        mus->file_handle = sf_open_virtual(&vio, SFM_READ, &file_info, &mus->vio_cursor);
        if (!mus->file_handle) {
            spdlog::error("load_music_stream_memory: sf_open_virtual failed for '{}': {}", name, sf_strerror(nullptr));
            return "";
        }

        mus->file_info = file_info;
        mus->stream_buffer = new float[mus->buffer_size * file_info.channels];

        if (file_info.samplerate != target_sample_rate) {
            int error;
            mus->resampler = src_new(SRC_SINC_FASTEST, file_info.channels, &error);
            if (!mus->resampler) {
                spdlog::error("Failed to create resampler for memory stream '{}': {}", name, src_strerror(error));
                return "";
            }
            double ratio = target_sample_rate / (double)file_info.samplerate;
            unsigned int resample_buf_size = (unsigned int)(mus->buffer_size * ratio) + 256;
            mus->resample_buffer = new float[resample_buf_size * file_info.channels];
            spdlog::info("Memory music stream '{}' will be resampled from {} Hz to {} Hz", name, file_info.samplerate,
                         target_sample_rate);
        }
        alloc_stream_ring(*mus, target_sample_rate);

        install_music(name, mus);

        spdlog::debug("Loaded memory music stream: '{}' ({} frames, {} Hz, {} channels)", name, file_info.frames,
                      file_info.samplerate, file_info.channels);
        return name;

    } catch (const std::exception& e) {
        spdlog::error("Error loading memory music stream '{}': {}", name, e.what());
        return "";
    }
}
#endif // __EMSCRIPTEN__

void AudioEngine::play_music_stream(const std::string& name, VolumePreset volume_preset) {
    if (fixed_step::on() && is_music_stream_valid(name)) {
        fixed_step::audio_play(name, true);
    }
    std::shared_ptr<music> mus = find_music(name);
    if (!mus) {
        spdlog::warn("Sound {} not found", name);
        return;
    }
    if (volume_preset != VolumePreset::NONE) {
        mus->volume.store(preset_volume(volume_presets, volume_preset), std::memory_order_relaxed);
    }
    if (mus->pcm_data) {
        // the read position of a decoded stream is its current_frame: restart via the mixer
        mus->cmd_out_frame.store(0, std::memory_order_relaxed);
        mus->cmd_gen.fetch_add(1, std::memory_order_release);
    }
    mus->current_frame.store(0, std::memory_order_relaxed);
    mus->is_playing.store(true, std::memory_order_release);
}

float AudioEngine::get_music_time_length(const std::string& name) const {
    std::shared_ptr<music> mus = find_music(name);
    if (mus) {
        if (mus->pcm_data) {
            return static_cast<float>(mus->pcm_total_frames) / static_cast<float>(target_sample_rate);
        }
        // file_info.frames/samplerate are in the source file's own rate --
        // pcm_data above is already resampled to target_sample_rate, but
        // this file-backed stream is not.
        return static_cast<float>(mus->file_info.frames) / static_cast<float>(mus->file_info.samplerate);
    }
    spdlog::warn("Music stream {} not found", name);
    return 0.0f;
}

float AudioEngine::get_music_time_played(const std::string& name) const {
    if (fixed_step::on()) {
        return fixed_step::audio_time(name, true, get_music_time_length(name));
    }
    std::shared_ptr<music> mus = find_music(name);
    if (mus) {
        unsigned long long frame = mus->current_frame.load(std::memory_order_relaxed);
        return static_cast<float>(frame) / static_cast<float>(target_sample_rate);
    }
    spdlog::warn("Music stream {} not found", name);
    return 0.0f;
}

void AudioEngine::set_music_volume(const std::string& name, float volume) {
    std::shared_ptr<music> mus = find_music(name);
    if (mus) {
        mus->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    } else {
        spdlog::warn("Sound {} not found", name);
    }
}

bool AudioEngine::is_music_stream_valid(const std::string& name) const {
    return find_music(name) != nullptr;
}

bool AudioEngine::is_music_stream_playing(const std::string& name) const {
    if (fixed_step::on()) {
        return is_music_stream_valid(name) && fixed_step::audio_playing(name, true, get_music_time_length(name));
    }
    std::shared_ptr<music> mus = find_music(name);
    if (mus) {
        return mus->is_playing.load(std::memory_order_relaxed);
    }
    spdlog::warn("Sound {} not found", name);
    return false;
}

// Seek command: the decoder (file-backed) or the mixer (decoded) carries it out; until then the
// stream plays nothing and its position stays at out_frame.
static void command_seek(music& mus, long long source_frame, unsigned long long out_frame) {
    mus.cmd_source_frame.store(source_frame, std::memory_order_relaxed);
    mus.cmd_out_frame.store(out_frame, std::memory_order_relaxed);
    mus.current_frame.store(out_frame, std::memory_order_relaxed);
    mus.cmd_gen.fetch_add(1, std::memory_order_release);
}

void AudioEngine::stop_music_stream(const std::string& name) {
    if (fixed_step::on()) {
        fixed_step::audio_stop(name, true);
    }
    std::shared_ptr<music> mus = find_music(name);
    if (!mus) {
        spdlog::warn("Music stream {} not found", name);
        return;
    }
    mus->is_playing.store(false, std::memory_order_release);
    command_seek(*mus, 0, 0);
    if (mus->file_handle) {
        kick_decoder();
    }
}

void AudioEngine::seek_music_stream(const std::string& name, float position) {
    if (fixed_step::on()) {
        fixed_step::audio_seek(name, true, position);
    }
    std::shared_ptr<music> mus = find_music(name);
    if (!mus) {
        spdlog::warn("Music stream {} not found", name);
        return;
    }
    if (mus->file_handle) {
        sf_count_t frame_position = static_cast<sf_count_t>(position * mus->file_info.samplerate);

        frame_position = std::max<sf_count_t>(frame_position, 0);
        if (frame_position >= mus->file_info.frames) {
            frame_position = mus->file_info.frames - 1;
        }
        command_seek(
            *mus, frame_position,
            static_cast<unsigned long long>(std::max(position, 0.0f) * static_cast<float>(target_sample_rate)));
        kick_decoder();
    } else if (mus->pcm_data) {
        unsigned long long frame_pos =
            static_cast<unsigned long long>(std::max(position, 0.0f) * static_cast<float>(target_sample_rate));
        frame_pos = std::min(frame_pos, static_cast<unsigned long long>(mus->pcm_total_frames));
        command_seek(*mus, 0, frame_pos);
    }
}

void AudioEngine::dev_pause_device(bool pause) {
    if (sdl_stream) {
        if (pause) {
            SDL_PauseAudioStreamDevice(sdl_stream);
        } else {
            SDL_ResumeAudioStreamDevice(sdl_stream);
        }
    }
#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
    if (rt_audio && rt_audio->isStreamOpen()) {
        if (pause && rt_audio->isStreamRunning()) {
            rt_audio->stopStream();
        } else if (!pause && !rt_audio->isStreamRunning()) {
            rt_audio->startStream();
        }
    }
#endif
}

// With the device paused (the selftest mixes on this thread): until each file-backed stream has
// carried out its last command and filled its ring (or reached the end), so the next mix() has
// what the old synchronous reads would have produced.
void AudioEngine::dev_wait_streams() {
    std::vector<std::shared_ptr<music>> streams;
    {
        std::scoped_lock g(music_lock);
        for (auto& [name, mus] : music_streams) {
            if (mus->file_handle) {
                streams.push_back(mus);
            }
        }
    }
    for (auto& mp : streams) {
        music& mus = *mp;
        for (;;) {
            if (mus.ring_gen.load(std::memory_order_acquire) == mus.cmd_gen.load(std::memory_order_acquire)) {
                apply_stream_flush(mus);
                const unsigned long long used =
                    mus.write_pos.load(std::memory_order_acquire) - mus.read_pos.load(std::memory_order_relaxed);
                if (mus.eof.load(std::memory_order_acquire) ||
                    mus.ring_frames - used < stream_max_out(mus, target_sample_rate)) {
                    break;
                }
            }
            kick_decoder();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
}

void AudioEngine::dev_wait_decoded(const std::string& name) {
    for (;;) {
        std::shared_ptr<SoundDecodeJob> job;
        with_sound(name, [&job](SoundSlot&, const sound& pcm) { job = pcm.decode_job; });
        if (!job || job->done.load(std::memory_order_acquire)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

AudioEngine audio;
