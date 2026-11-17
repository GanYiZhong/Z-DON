#pragma once

#include <string>
#include <unordered_map>

#include "global_data.h"

// Z-DON: the difficulty last confirmed on each song, per player, for this session ([general]
// zdon_remember_difficulty): the song select's difficulty cursor reopens there instead of on the last difficulty
// played on any song. In memory only.
namespace zdon_diff_memory {

    inline std::unordered_map<std::string, int>& of(int player_index) {
        static std::unordered_map<std::string, int> by_player[4];
        return by_player[static_cast<size_t>(player_index) & 3U];
    }

    inline bool on() {
        return global_data.config && global_data.config->general.zdon_remember_difficulty;
    }

    inline void remember(int player_index, const std::string& song, int difficulty) {
        if (on() && !song.empty()) {
            of(player_index)[song] = difficulty;
        }
    }

    // the remembered difficulty, else `fallback`
    inline int recall(int player_index, const std::string& song, int fallback) {
        if (!on() || song.empty()) {
            return fallback;
        }
        const auto& m = of(player_index);
        const auto it = m.find(song);
        return it != m.end() ? it->second : fallback;
    }

} // namespace zdon_diff_memory
