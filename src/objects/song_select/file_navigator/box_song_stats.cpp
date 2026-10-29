// Z-DON: SongBox::chart_stats, the note count / length / BPM range of one course for the skin's
// difficulty select (Lua: box:chart_stats(diff)). The chart is parsed on the first request for that course
// and the numbers are kept on the box.
#include <algorithm>
#include <spdlog/spdlog.h>

#include "box_song.h"

const SongBox::ChartStats& SongBox::chart_stats(int difficulty) {
    static const ChartStats empty{};
    if (difficulty < 0 || difficulty >= (int)chart_stats_cache.size()) {
        return empty;
    }
    std::optional<ChartStats>& slot = chart_stats_cache[difficulty];
    if (slot.has_value()) {
        return slot.value();
    }
    ChartStats st;
    try {
        auto [notes, branch_m, branch_e, branch_n] = parser.notes_to_position(difficulty);
        bool any = false;
        auto take = [&](const std::deque<Note>& list) {
            for (const Note& note : list) {
                if (note.type == NoteType::BARLINE) {
                    continue;
                }
                if (note.type >= NoteType::DON && note.type <= NoteType::KAT_L) {
                    st.notes++;
                }
                st.length_ms = std::max(st.length_ms, note.hit_ms);
                if (note.bpm > 0) {
                    st.bpm_min = any ? std::min(st.bpm_min, note.bpm) : note.bpm;
                    st.bpm_max = any ? std::max(st.bpm_max, note.bpm) : note.bpm;
                    any = true;
                }
            }
        };
        take(notes.notes);
        for (const NoteList& section : branch_m) {
            take(section.notes);
        }
    } catch (const std::exception& e) {
        spdlog::warn("SongBox::chart_stats: failed to parse course {}: {}", difficulty, e.what());
    }
    slot = st;
    return slot.value();
}
