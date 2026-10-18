#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// Z-DON dev hooks for the mixer (all off unless the env var is set):
//   YATAIDON_AUDIO_STATS=1        every 2 s logs the mix callback's duration (avg/p99/max, us), the longest
//                                 time it waited for a lock and the underflows the backend reported.
//   YATAIDON_AUDIO_CAPTURE=<wav>  writes the mixer's output, before the master volume (YATAIDON_TEST
//                                 plays at 1%) and the final clamp, to a 32-bit float stereo WAV.
//   YATAIDON_AUDIO_SELFTEST=<wav>;<long sound>;<stream file>  after the device opens, pauses it and mixes a
//                                 fixed script of play/seek/stop/pitch/pan/volume calls offline into <wav>
//                                 (state queries logged to <wav>.txt), then resumes the device.
namespace audio_dev {
    bool stats_enabled();
    bool capture_enabled();

    // starts the stats reporter / capture writer threads (once); rate is the mixer's rate
    void start(double sample_rate);
    void stop();

    // from the mix callback
    void record_callback(int64_t mix_ns, int64_t lock_wait_ns);
    void record_underflow();
    void record_stream_underrun();
    void capture(const float* stereo, unsigned int frames);

    inline int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // "<wav>;<long sound>;<stream file>" or empty
    const std::string& selftest_spec();
} // namespace audio_dev
