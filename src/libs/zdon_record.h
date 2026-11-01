#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Z-DON: renders a chart straight to a video file, faster than real time, instead of recording a play.
//
//   YataiDON.exe <song.tja> [difficulty] --record <out.mp4> [--auto] [--record-fps <n>] [--record-result <s>]
//                [--record-status <file>]
//
// Without --auto nothing hits (every note passes: a "no input" video). The run is a test-style one
// (YATAIDON_TEST: hidden window, real input ignored, the player's lists untouched) on the fixed-step clock
// (YATAIDON_FIXED_STEP_MS = 1000 / fps): every frame is drawn, read back and piped to ffmpeg, and the
// mixer is driven from the main thread (the audio device is paused) for exactly one frame of audio, so the
// sound is in step with the picture. The window is the skin's screen size (no fullscreen); audio_offset
// and visual_offset are 0 (they compensate a live device). Nothing is saved: no score, no config.toml.
// The recording runs until the result screen has shown for --record-result seconds (default 6), then the
// audio is muxed in (AAC) and the game exits; it logs to record.log, not latest.log. ffmpeg comes from
// PATH, or ZDON_FFMPEG; the video is h264_nvenc when it works, else libx264. --record-status: the progress
// ("<0..1>") is written there every few frames (the recording mode reads it).
//
// The recording mode (scenes/song_select_record.h) runs such a recording as a child process of the game
// (export_*, zdon_record_job.cpp).
namespace zdon_record {
    // main(), first thing: with --record in argv sets up the test-style fixed-step run (environment)
    void prepare(int argc, char** argv);
    bool active();

    // check_args: true when argv[i] is a --record option (i is moved past its value)
    bool consume_arg(const std::string& arg, int& i, int argc);

    // after load_skin (the audio device is open): overrides the config in memory, pauses the device and
    // starts the encoder; false when it cannot record (logged; the game should exit)
    bool begin();

    // the game screen, every frame of a recording: where the song is (chart ms) and where it ends
    void note_song(double position_ms, double end_ms);

    // ---- parallel recording (--record-jobs N, default kDefaultJobs) ----
    // The recording is split by frame into N parts recorded at once by child processes (--record-part k
    // --record-range <first> <end>): each runs the game from the start on the fixed clock, without presenting,
    // reading back or encoding until its first frame (it still draws: skins keep state in draw, e.g. the max
    // combo, and mixes the audio, so the state at the seam is the one a single run has), records its frames
    // into <out>.part<k>.mp4 / .wav, and stops at its end frame (the last part at the recording's natural end).
    // The process that was asked for the recording only coordinates: it estimates the length from the chart's
    // last note, starts the parts, sums their progress and joins them (concat demuxer, copied video; the WAV
    // parts appended) before the AAC mux. 3 parts: more do not help (GPU / NVENC bound).
    constexpr int kDefaultJobs = 3;
    // after check_args and the log setup, before the window: true when this process coordinates parts
    bool coordinating();
    // runs the parts and joins them; the process exits with !result
    bool coordinate(int argc, char** argv);
    // run_frame: true while this part runs up to its first frame (the frame is drawn, not presented)
    bool skip_present();
    int part(); // this process's part (-1: not a part)

    // every frame, after the frame is complete in the window and before it is presented: the picture and
    // one frame of audio. true when the recording is done.
    bool frame(const std::string& screen);

    // after the main loop: closes the encoder and muxes the audio into the output file; false when the
    // recording failed (the exit code says so)
    bool end();

    // ---- the recording mode: a recording in a child process ----
    enum class ExportState {
        None,
        Running,
        Done,
        Failed
    };
    struct ExportStatus {
        ExportState state = ExportState::None;
        double progress = 0.0; // 0..1
        std::filesystem::path out;
    };
    // where a recording of this song goes: Records/<title>_<course>_<auto|nohit>[ (n)].mp4 next to the exe
    std::filesystem::path export_path(const std::string& title, int difficulty, bool autoplay);
    // starts recording song / difficulty into out (false: the child could not start)
    bool export_start(const std::filesystem::path& song, int difficulty, bool autoplay,
                      const std::filesystem::path& out);
    ExportStatus export_poll();
    void export_cancel(); // stops the child and removes what it wrote
    void export_forget(); // after Done / Failed: back to None
    void show_in_folder(const std::filesystem::path& file);

    // zdon_record_job.cpp: runs this exe once per argument list (UTF-8), hidden, all at once, in a kill-on-close
    // job; calls tick about four times a second until every one has exited. true when all exited with 0.
    bool run_children(const std::vector<std::vector<std::string>>& args, const std::function<void()>& tick);
} // namespace zdon_record
