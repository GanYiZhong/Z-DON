// Z-DON: the support-level simplification upstream had before the arcade's (Yonokid, 790f0fe3), kept so a
// recording can show it next to the new one (--record-tracks <course>@<level>y).
#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <vector>

#include "tja.h"

static constexpr auto REMOVED_NOTE = static_cast<NoteType>(-1);

static bool is_roll_or_balloon(const Note& note) {
    return note.type == NoteType::ROLL_HEAD || note.type == NoteType::ROLL_HEAD_L ||
           note.type == NoteType::BALLOON_HEAD || note.type == NoteType::KUSUDAMA;
}

static void make_single_color(std::deque<Note>& notes, int start, int end) {
    int don_count = 0;
    int kat_count = 0;
    const int stop = std::min(end, static_cast<int>(notes.size()));
    for (int i = start; i < stop; ++i) {
        don_count += notes[i].type == NoteType::DON || notes[i].type == NoteType::DON_L;
        kat_count += notes[i].type == NoteType::KAT || notes[i].type == NoteType::KAT_L;
    }
    const bool don = don_count >= kat_count;
    for (int i = start; i < stop; ++i) {
        if (notes[i].type == NoteType::DON || notes[i].type == NoteType::KAT) {
            notes[i].type = don ? NoteType::DON : NoteType::KAT;
        } else if (notes[i].type == NoteType::DON_L || notes[i].type == NoteType::KAT_L) {
            notes[i].type = don ? NoteType::DON_L : NoteType::KAT_L;
        }
    }
}

static std::vector<int> find_sixteenth_2plus2(const std::deque<Note>& notes) {
    std::vector<int> patterns;
    for (int i = 0; i < static_cast<int>(notes.size()) - 3; ++i) {
        if (is_roll_or_balloon(notes[i]) || is_roll_or_balloon(notes[i + 1]) || is_roll_or_balloon(notes[i + 2]) ||
            is_roll_or_balloon(notes[i + 3])) {
            continue;
        }
        if (get_note_interval_type(notes[i + 1].hit_ms - notes[i].hit_ms, notes[i].bpm) != Interval::SIXTEENTH ||
            get_note_interval_type(notes[i + 3].hit_ms - notes[i + 2].hit_ms, notes[i + 2].bpm) !=
                Interval::SIXTEENTH) {
            continue;
        }
        const double sixteenth = get_ms_per_measure(notes[i].bpm, 4.0) / 4.0 / 16.0;
        if (std::abs(notes[i + 2].hit_ms - notes[i + 1].hit_ms - sixteenth * 2) >= 20.0) {
            continue;
        }
        if (i + 4 >= static_cast<int>(notes.size())) {
            patterns.push_back(i);
        } else if (!is_roll_or_balloon(notes[i + 4])) {
            const double after = notes[i + 4].hit_ms - notes[i + 3].hit_ms;
            if (after >= sixteenth * 1.5 || get_note_interval_type(after, notes[i + 3].bpm) != Interval::SIXTEENTH) {
                patterns.push_back(i);
            }
        }
    }
    return patterns;
}

// Every other note of 1/32 streams; two of every three notes of 1/24 streams (levels 10-12).
static void thin_fastest_streams(std::deque<Note>& notes) {
    for (const auto& [start, length] : find_streams(notes, Interval::THIRTYSECOND)) {
        for (int i = start + 1; i < start + length; i += 2) {
            notes[i].type = REMOVED_NOTE;
        }
    }
    for (const auto& [start, length] : find_streams(notes, Interval::TWENTYFOURTH)) {
        for (int i = start + 1; i < start + length - 1; i += 3) {
            notes[i].type = REMOVED_NOTE;
            notes[i + 1].type = REMOVED_NOTE;
        }
    }
}

// Long 1/16 streams become repeating single-color triplets with the next two notes dropped (levels 11-12).
static void sixteenths_to_triplets(std::deque<Note>& notes, int start, int length) {
    const int end = start + length;
    int i = start;
    while (i < end) {
        const int triplet_end = std::min(i + 3, end);
        if (triplet_end - i >= 2) {
            make_single_color(notes, i, triplet_end);
        }
        i += 3;
        if (i < end) {
            notes[i].type = REMOVED_NOTE;
            i += 2;
        }
        if (i < end) {
            notes[i].type = REMOVED_NOTE;
            i += 1;
        }
    }
}

static void simplify_notes(std::deque<Note>& notes, int level) {
    if (level == 6) {
        for (const auto& [start, length] : find_streams(notes, Interval::EIGHTH)) {
            if (length == 3) {
                notes[start + 1].type = REMOVED_NOTE;
            } else if (length > 3) {
                make_single_color(notes, start, start + length);
            }
        }
    } else if (level == 7) {
        for (const auto& [start, length] : find_streams(notes, Interval::EIGHTH)) {
            if (length == 5) {
                notes[start + 3].type = REMOVED_NOTE;
            } else if (length >= 7) {
                for (int i = start + 3; i < start + length; i += 4) {
                    notes[i].type = REMOVED_NOTE;
                }
            }
        }
    } else if (level == 8) {
        for (const auto& [start, length] : find_streams(notes, Interval::SIXTEENTH)) {
            if (length == 3) {
                notes[start + 1].type = REMOVED_NOTE;
            } else if (length == 5) {
                // 3+1 when it starts on don, 2+2 when it starts on kat
                const bool don = notes[start].type == NoteType::DON || notes[start].type == NoteType::DON_L;
                notes[start + (don ? 3 : 2)].type = REMOVED_NOTE;
            }
        }
    } else if (level == 10) {
        thin_fastest_streams(notes);
        for (const auto& [start, length] : find_streams(notes, Interval::SIXTEENTH)) {
            if (length == 2 && notes[start].type != NoteType::TAIL) {
                notes[start].type = notes[start + 1].type;
            } else if (length == 3) {
                notes[start + 1].type = REMOVED_NOTE;
            } else if (length == 4 || length == 5) {
                notes[start + 3].type = REMOVED_NOTE;
                make_single_color(notes, start, start + length);
            } else if (length > 5) {
                notes[start + 3].type = REMOVED_NOTE;
                for (int i = start + 5; i < start + length; i += 2) {
                    notes[i].type = REMOVED_NOTE;
                }
            }
        }
        for (const int start : find_sixteenth_2plus2(notes)) {
            notes[start + 2].type = REMOVED_NOTE;
        }
    } else if (level == 11) {
        thin_fastest_streams(notes);
        for (const auto& [start, length] : find_streams(notes, Interval::TWELFTH)) {
            for (int i = start + 1; i < start + length - 1; i += 3) {
                notes[i].type = REMOVED_NOTE;
            }
        }
        for (const auto& [start, length] : find_streams(notes, Interval::SIXTEENTH)) {
            if (length == 2 && notes[start].type != NoteType::TAIL) {
                notes[start].type = notes[start + 1].type;
            } else if (length == 3) {
                notes[start + 1].type = REMOVED_NOTE;
            } else if (length == 4 || length == 5) {
                notes[start + 3].type = REMOVED_NOTE;
                make_single_color(notes, start, start + length);
            } else if (length > 5) {
                sixteenths_to_triplets(notes, start, length);
            }
        }
    } else if (level == 12) {
        thin_fastest_streams(notes);
        for (const auto& [start, length] : find_streams(notes, Interval::TWELFTH)) {
            if (length <= 4) {
                make_single_color(notes, start, start + length);
            } else {
                for (int i = start + 1; i < start + length - 1; i += 3) {
                    notes[i].type = REMOVED_NOTE;
                }
            }
        }
        for (const auto& [start, length] : find_streams(notes, Interval::SIXTEENTH)) {
            if (length == 3) {
                make_single_color(notes, start, start + length);
            } else if (length == 4 || length == 5) {
                notes[start + 3].type = REMOVED_NOTE;
                make_single_color(notes, start, start + length);
            } else if (length > 5) {
                sixteenths_to_triplets(notes, start, length);
            }
        }
    }
}

void zdon_modifier_difficulty_yono(NoteList& notes, int level) {
    if (level < 6 || level == 9 || level > 12) {
        return;
    }
    std::deque<Note> chart;
    std::ranges::copy_if(notes.notes, std::back_inserter(chart),
                         [](const Note& note) { return note.type != NoteType::BARLINE; });
    simplify_notes(chart, level);

    auto simplified = chart.begin();
    for (Note& note : notes.notes) {
        if (note.type != NoteType::BARLINE) {
            note.type = (simplified++)->type;
        }
    }
    std::erase_if(notes.notes, [](const Note& note) { return note.type == REMOVED_NOTE; });
}
