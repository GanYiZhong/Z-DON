#include "fixed_step.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <random>
#include <unordered_map>

namespace fixed_step {
    unsigned int seed() {
        return on() ? 20261019u : std::random_device{}();
    }

    namespace {
        struct Voice {
            bool playing = false;
            double start_ms = 0.0; // frame clock when it (re)started at `offset`
            double offset = 0.0;   // seconds
        };
        struct Params {
            bool loop = false;
            float pitch = 1.0f;
        };
        std::mutex g_mutex;
        std::unordered_map<std::string, Voice> g_voices;  // "s:" sounds, "m:" music streams
        std::unordered_map<std::string, Params> g_params; // sounds only

        std::string key(const std::string& name, bool music) {
            return (music ? "m:" : "s:") + name;
        }
        double now_ms() {
            return clock_ms.load(std::memory_order_relaxed);
        }
        // position in seconds; sets `ended` when a non-looping voice ran past its length
        double position(const Voice& v, const Params& p, double length, bool& ended) {
            double t = v.offset + (now_ms() - v.start_ms) / 1000.0 * (double)p.pitch;
            ended = false;
            if (length > 0.0 && t >= length) {
                if (p.loop) {
                    t = std::fmod(t, length);
                } else {
                    t = length;
                    ended = true;
                }
            }
            return t;
        }
    } // namespace

    void audio_play(const std::string& name, bool music) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_voices[key(name, music)] = Voice{true, now_ms(), 0.0};
    }

    void audio_stop(const std::string& name, bool music) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_voices.erase(key(name, music));
    }

    void audio_seek(const std::string& name, bool music, float seconds) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_voices.find(key(name, music));
        if (it != g_voices.end()) {
            it->second.offset = std::max(0.0, (double)seconds);
            it->second.start_ms = now_ms();
        }
    }

    void audio_set_loop(const std::string& name, bool loop) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_params[name].loop = loop;
    }

    void audio_set_pitch(const std::string& name, float pitch) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_params[name].pitch = pitch;
    }

    bool audio_playing(const std::string& name, bool music, float length) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_voices.find(key(name, music));
        if (it == g_voices.end() || !it->second.playing) {
            return false;
        }
        const Params p = music ? Params{} : g_params[name];
        bool ended = false;
        position(it->second, p, length, ended);
        return !ended;
    }

    float audio_time(const std::string& name, bool music, float length) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_voices.find(key(name, music));
        if (it == g_voices.end()) {
            return 0.0f;
        }
        const Params p = music ? Params{} : g_params[name];
        bool ended = false;
        return (float)position(it->second, p, length, ended);
    }
} // namespace fixed_step
