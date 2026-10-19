#pragma once

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

// Z-DON dev hook YATAIDON_FIXED_STEP_MS=<ms>: deterministic capture. Every rendered frame advances
// the game clock by exactly <ms> from a fixed start, so two runs of the same exe render the same
// frames (shoot them with YATAIDON_DUMP_FRAMES="<dir>;<f1,f2,...>"):
//   - get_current_ms() / g_frame_ms return the frame's clock (constant within a frame); there is
//     no frame limiter, vsync or vblank pacing wait;
//   - the chart follows that clock instead of the audio position, and the audio queries the game
//     makes (is_sound_playing, get_sound_time_played and the music stream ones) answer from a
//     virtual clock started by play/seek on the same frame clock (the real audio still plays);
//   - random seeds (C++ and Lua) are fixed;
//   - background work whose result shows on screen (library scans, song select statistics, text
//     builds, preview and song loads, video frames) is waited for where it starts / is needed;
//   - YATAIDON_JUMP_SCREEN and YATAIDON_FAKE_KEYS take frame numbers instead of milliseconds.
// Unset (the default) nothing changes.
namespace fixed_step {
    inline double step_ms() {
        static const double v = [] {
            const char* e = std::getenv("YATAIDON_FIXED_STEP_MS");
            const double ms = e ? std::atof(e) : 0.0;
            return ms > 0.0 ? ms : 0.0;
        }();
        return v;
    }
    inline bool on() {
        return step_ms() > 0.0;
    }

    constexpr double kStartMs = 10000000.0; // clock of frame 0
    inline std::atomic<double> clock_ms{kStartMs};
    inline std::atomic<long long> frame{-1}; // the current frame's index (-1 before the first)

    // at the start of every rendered frame
    inline void begin_frame() {
        const long long f = frame.load(std::memory_order_relaxed) + 1;
        frame.store(f, std::memory_order_relaxed);
        clock_ms.store(kStartMs + (double)f * step_ms(), std::memory_order_relaxed);
    }

    // fixed mode: run a just started worker to completion now
    inline void join(std::thread& t) {
        if (on() && t.joinable()) {
            t.join();
        }
    }
    template<typename Future>
    inline void wait(Future& f) {
        if (on() && f.valid()) {
            f.wait();
        }
    }

    // a fixed seed in fixed mode, else a random one
    unsigned int seed();

    // the virtual audio clock (fixed mode only); `music` keeps sounds and music streams apart
    void audio_play(const std::string& name, bool music);
    void audio_stop(const std::string& name, bool music);
    void audio_seek(const std::string& name, bool music, float seconds);
    void audio_set_loop(const std::string& name, bool loop);
    void audio_set_pitch(const std::string& name, float pitch);
    bool audio_playing(const std::string& name, bool music, float length);
    float audio_time(const std::string& name, bool music, float length);
} // namespace fixed_step
