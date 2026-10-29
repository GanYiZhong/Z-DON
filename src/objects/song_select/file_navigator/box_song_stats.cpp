// Z-DON: SongBox::chart_stats, the note count / length / BPM range of one course for the skin's
// difficulty select (Lua: box:chart_stats(diff)). The first request starts a worker job that parses the
// chart (on its own SongParser, so the box's parser is never touched off the main thread); until it is
// done the numbers are empty (notes 0: the skin draws nothing), then they are kept on the box.
#include <algorithm>
#include <spdlog/spdlog.h>

#include "../../../libs/jobs.h"
#include "box_song.h"

namespace {
    SongBox::ChartStats compute_stats(const fs::path& path, int difficulty) {
        SongBox::ChartStats st;
        try {
            SongParser parser(path);
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
            spdlog::warn("SongBox::chart_stats: failed to parse course {} of '{}': {}", difficulty, path.string(),
                         e.what());
        }
        return st;
    }
} // namespace

const SongBox::ChartStats& SongBox::chart_stats(int difficulty) {
    static const ChartStats empty{};
    if (difficulty < 0 || difficulty >= (int)chart_stats_cache.size()) {
        return empty;
    }
    std::optional<ChartStats>& slot = chart_stats_cache[difficulty];
    if (slot.has_value()) {
        return slot.value();
    }
    std::shared_ptr<ChartStatsJob>& job = chart_stats_jobs[difficulty];
    if (!job) {
        job = std::make_shared<ChartStatsJob>();
        jobs::submit(jobs::Priority::Normal, [job, chart = parser.file_path, difficulty] {
            job->stats = compute_stats(chart, difficulty);
            job->done.store(true, std::memory_order_release);
        });
        return empty;
    }
    if (!job->done.load(std::memory_order_acquire)) {
        return empty;
    }
    slot = job->stats;
    job.reset();
    return slot.value();
}
