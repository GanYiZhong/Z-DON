#pragma once

#include <filesystem>
#include <string>

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
} // namespace zdon_record
