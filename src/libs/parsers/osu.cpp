#include "osu.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>

#include "../math/md5.h"

std::vector<std::string> OsuParser::read_file_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return lines;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// Calls fn(line) for each line inside [section], up to the next section header
template<class F>
static void for_each_section_line(const std::vector<std::string>& lines, const std::string& section, F&& fn) {
    const std::string header = "[" + section + "]";
    bool in_section = false;
    for (const auto& line : lines) {
        if (line == header) {
            in_section = true;
            continue;
        }
        if (!line.empty() && line[0] == '[') {
            in_section = false;
            continue;
        }
        if (in_section) {
            fn(line);
        }
    }
}

// Same, for the sections of numbers: only lines that start with a digit or sign
template<class F>
static void for_each_data_line(const std::vector<std::string>& lines, const std::string& section, F&& fn) {
    for_each_section_line(lines, section, [&](const std::string& line) {
        if (line.empty() || line[0] == '/' || line[0] == '\r') {
            return;
        }
        if (!std::isdigit((unsigned char)line[0]) && line[0] != '-' && line[0] != '+') {
            return;
        }
        fn(line);
    });
}

std::map<std::string, std::string> OsuParser::read_section_dict(const std::vector<std::string>& lines,
                                                                const std::string& section) const {
    std::map<std::string, std::string> result;
    for_each_section_line(lines, section, [&](const std::string& line) {
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            return;
        }
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // trim whitespace
        auto ltrim = [](std::string& s) { s.erase(0, s.find_first_not_of(" \t")); };
        auto rtrim = [](std::string& s) {
            auto p = s.find_last_not_of(" \t\r\n");
            if (p != std::string::npos) {
                s.erase(p + 1);
            }
        };
        ltrim(key);
        rtrim(key);
        ltrim(val);
        rtrim(val);
        result[key] = val;
    });
    return result;
}

std::vector<std::vector<double>> OsuParser::read_section_list(const std::vector<std::string>& lines,
                                                              const std::string& section) const {
    static const std::regex num_re(R"([-+]?\d*\.?\d+)");
    std::vector<std::vector<double>> result;
    for_each_data_line(lines, section, [&](const std::string& line) {
        std::vector<double> nums;
        auto begin = std::sregex_iterator(line.begin(), line.end(), num_re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            try {
                nums.push_back(std::stod(it->str()));
            } catch (const std::out_of_range&) {
                nums.push_back(0.0);
            }
        }
        if (!nums.empty()) {
            result.push_back(nums);
        }
    });
    return result;
}

std::vector<std::vector<std::string>> OsuParser::read_hitobjects_fields(const std::vector<std::string>& lines) const {
    std::vector<std::vector<std::string>> result;
    for_each_data_line(lines, "HitObjects", [&](const std::string& line) {
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }
        if (!fields.empty()) {
            result.push_back(fields);
        }
    });
    return result;
}

double OsuParser::get_scroll_multiplier(double ms) const {
    double base_scroll = (slider_multiplier >= 1.37 && slider_multiplier <= 1.47) ? 1.0 : slider_multiplier / 1.40;
    double current_scroll = 1.0;
    for (const auto& tp : timing_points) {
        if (tp[0] > ms) {
            break;
        }
        if (tp[1] < 0) {
            current_scroll = -100.0 / tp[1];
        } else {
            current_scroll = 1.0; // uninherited point resets SV
        }
    }
    return current_scroll * base_scroll;
}

double OsuParser::get_bpm_at(double ms) const {
    double bpm = 120.0;
    for (const auto& tp : timing_points) {
        if (tp[0] > ms) {
            break;
        }
        if (tp[1] > 0 && tp[1] < 60000) {
            bpm = 60000.0 / tp[1];
        }
    }
    return bpm;
}

OsuParser::OsuParser(const fs::path& path) : file_path(path) {
    auto lines = read_file_lines(path);
    if (lines.empty()) {
        spdlog::warn("OsuParser: empty or unreadable file: {}", path.string());
        return;
    }

    auto general = read_section_dict(lines, "General");
    auto osu_meta = read_section_dict(lines, "Metadata");
    auto difficulty = read_section_dict(lines, "Difficulty");
    auto tp_data = read_section_list(lines, "TimingPoints");
    hit_objects_data = read_hitobjects_fields(lines);

    // Slider multiplier
    if (difficulty.contains("SliderMultiplier")) {
        try {
            double sm = std::stod(difficulty["SliderMultiplier"]);
            if (sm > 0.0) {
                slider_multiplier = sm;
            } else {
                spdlog::warn("OsuParser: invalid SliderMultiplier '{}', using default", difficulty["SliderMultiplier"]);
            }
        } catch (const std::exception& e) {
            spdlog::warn("OsuParser: failed to parse SliderMultiplier '{}': {}", difficulty["SliderMultiplier"],
                         e.what());
        }
    }

    // Timing points: store {time_ms, beat_length, meter, effects}
    for (const auto& row : tp_data) {
        if (row.size() >= 2) {
            double meter = row.size() > 2 && row[2] > 0 ? row[2] : 4.0;
            double effects = row.size() > 7 ? row[7] : 0.0;
            timing_points.push_back({row[0], row[1], meter, effects});
        }
    }
    // stable_sort + tie-breaker: an inherited (green) and uninherited (red) point
    // often share the same offset, and get_scroll_multiplier()/get_bpm_at() depend
    // on the uninherited (positive beatLength) one being processed first.
    std::ranges::stable_sort(timing_points, [](const std::array<double, 4>& a, const std::array<double, 4>& b) {
        if (a[0] != b[0]) {
            return a[0] < b[0];
        }
        return a[1] > b[1]; // uninherited (positive beatLength) first
    });

    // Metadata
    if (osu_meta.contains("Title")) {
        metadata.title["en"] = osu_meta["Title"];
    }
    if (osu_meta.contains("Artist")) {
        metadata.subtitle["en"] = osu_meta["Artist"];
    }
    if (osu_meta.contains("TitleUnicode")) {
        metadata.title["ja"] = osu_meta["TitleUnicode"];
    }
    if (osu_meta.contains("ArtistUnicode")) {
        metadata.subtitle["ja"] = osu_meta["ArtistUnicode"];
    }
    if (osu_meta.contains("Version")) {
        difficulty_name = osu_meta["Version"];
    }

    if (general.contains("AudioFilename")) {
        metadata.wave = path.parent_path() / general["AudioFilename"];
    }
    if (general.contains("PreviewTime")) {
        try {
            metadata.demostart = std::stod(general["PreviewTime"]) / 1000.0;
        } catch (const std::exception& e) {
            spdlog::warn("OsuParser: invalid PreviewTime '{}': {}", general["PreviewTime"], e.what());
        }
    }

    metadata.offset = -30.0 / 1000.0;

    // Background/video from Events section
    {
        // Look for video or background: "Video,offset,"filename"" or "0,0,"filename""
        static const std::regex bg_re(R"re(^\s*(?:Video|\d+),\d+,"([^"]+)")re");
        static constexpr std::string_view video_extensions[] = {".mp4", ".avi", ".mkv", ".mov", ".flv"};
        static constexpr std::string_view image_extensions[] = {".jpg", ".jpeg", ".png", ".bmp"};
        bool in_events = false;
        for (const auto& el : lines) {
            if (el == "[Events]") {
                in_events = true;
                continue;
            }
            if (!el.empty() && el[0] == '[') {
                if (in_events) {
                    break;
                }
                continue;
            }
            if (!in_events) {
                continue;
            }
            std::smatch m;
            if (std::regex_search(el, m, bg_re)) {
                fs::path candidate = path.parent_path() / m[1].str();
                std::string ext = candidate.extension().string();
                std::ranges::transform(ext, ext.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (std::ranges::contains(video_extensions, ext)) {
                    metadata.bgmovie = candidate;
                    break;
                }

                if (std::ranges::contains(image_extensions, ext)) {
                    metadata.preimage = candidate;
                    break;
                }
            }
        }
    }

    // BPM from first uninherited timing point
    double first_beat_length = 500.0; // default 120 BPM
    for (const auto& tp : timing_points) {
        if (tp[1] > 0 && tp[1] < 60000) {
            first_beat_length = tp[1];
            break;
        }
    }
    metadata.bpm = 60000.0 / first_beat_length;

    // One course entry (difficulty 0 = Easy slot, used for all osu songs)
    metadata.course_data[0] = CourseData{};
}

static Note make_note(NoteType type, double hit_ms, double bpm, double scroll, int index, int moji) {
    Note note;
    note.type = type;
    note.hit_ms = hit_ms;
    note.bpm = bpm;
    note.scroll_x = scroll;
    note.index = index;
    note.moji = moji;
    return note;
}

NoteList& OsuParser::get_notes() {
    if (notes_built) {
        return cached_notes;
    }

    double first_bpm = metadata.bpm;
    double first_beat_length = (first_bpm > 0) ? 60000.0 / first_bpm : 500.0;

    int counter = 0;

    for (const auto& line : hit_objects_data) {
        if (line.size() < 5) {
            continue;
        }

        double note_time;
        int obj_type;
        int hit_sound;
        try {
            note_time = std::stod(line[2]);
            obj_type = std::stoi(line[3]);
            hit_sound = std::stoi(line[4]);
        } catch (const std::exception& e) {
            spdlog::warn("OsuParser: skipping malformed hit object line: {}", e.what());
            continue;
        }
        double scroll = get_scroll_multiplier(note_time);
        double bpm_here = get_bpm_at(note_time);

        bool is_circle = (obj_type & 1) != 0;  // type bit 0
        bool is_slider = (obj_type & 2) != 0;  // type bit 1
        bool is_spinner = (obj_type & 8) != 0; // type bit 3

        if (is_circle) {
            Note note = make_note(NoteType::BARLINE, note_time, bpm_here, scroll, counter++, 1);

            bool kat = (hit_sound & (2 | 8)) != 0; // whistle or clap bit -> kat
            bool big = (hit_sound & 4) != 0;       // finish bit -> big don/kat
            if (big && kat) {
                note.type = NoteType::KAT_L;
                note.moji = 6;
            } else if (big) {
                note.type = NoteType::DON_L;
                note.moji = 5;
            } else if (kat) {
                note.type = NoteType::KAT;
                note.moji = 4;
            } else {
                note.type = NoteType::DON;
            }

            cached_notes.notes.push_back(note);

        } else if (is_slider) {
            // Drumroll: compute duration (field 7 is the slider "length")
            double slider_len = 0.0;
            if (line.size() > 7) {
                try {
                    slider_len = std::stod(line[7]);
                } catch (const std::exception&) {
                }
            }
            // Field 6 is the slide (repeat) count.
            int slides = 1;
            if (line.size() > 6) {
                try {
                    slides = std::stoi(line[6]);
                } catch (const std::exception&) {
                }
            }
            slides = std::max(slides, 1);

            double beat_len_at = first_beat_length;
            // Effective slider velocity multiplier from inherited (green) timing
            // points; an uninherited (red) point resets it back to 1.0.
            double sv = 1.0;
            for (const auto& tp : timing_points) {
                if (tp[0] > note_time) {
                    break;
                }
                if (tp[1] > 0) {
                    beat_len_at = tp[1];
                }
                sv = tp[1] < 0 ? (-100.0 / tp[1]) : 1.0;
            }
            double slider_time = slider_len / (slider_multiplier * 100.0 * sv) * beat_len_at * slides;

            bool big = (hit_sound & 4) != 0; // finish bit -> big roll head
            NoteType head_type = big ? NoteType::ROLL_HEAD_L : NoteType::ROLL_HEAD;

            Note head = make_note(head_type, note_time, bpm_here, scroll, counter++, big ? 8 : 7);
            head.color = 255;

            Note tail = make_note(NoteType::TAIL, note_time + slider_time, bpm_here, scroll, counter++, 10);

            cached_notes.notes.push_back(head);
            cached_notes.notes.push_back(tail);

        } else if (is_spinner) {
            // Balloon (field 5 is the spinner "endTime")
            double end_time = note_time + 1000.0;
            if (line.size() > 5) {
                try {
                    end_time = std::stod(line[5]);
                } catch (const std::exception&) {
                }
            }

            Note head = make_note(NoteType::BALLOON_HEAD, note_time, bpm_here, scroll, counter++, 10);
            head.count = 20;

            Note tail = make_note(NoteType::TAIL, end_time, bpm_here, scroll, counter++, 9);

            cached_notes.notes.push_back(head);
            cached_notes.notes.push_back(tail);
        }
    }

    double last_ms = 0.0;
    for (const Note& n : cached_notes.notes) {
        last_ms = std::max(last_ms, n.hit_ms);
    }
    std::vector<const std::array<double, 4>*> red_points;
    for (const auto& tp : timing_points) {
        if (tp[1] > 0 && tp[1] < 60000) {
            red_points.push_back(&tp);
        }
    }
    std::deque<Note> barlines;
    for (size_t i = 0; i < red_points.size(); ++i) {
        const auto& tp = *red_points[i];
        double section_end = i + 1 < red_points.size() ? (*red_points[i + 1])[0] : last_ms + 1.0;
        double measure_ms = tp[1] * tp[2];
        bool omit_first = (static_cast<int>(tp[3]) & 8) != 0;

        for (double t = tp[0]; t < section_end - 1.0; t += measure_ms) {
            if (omit_first && t == tp[0]) {
                continue;
            }
            Note bar;
            bar.type = NoteType::BARLINE;
            bar.hit_ms = t;
            bar.bpm = get_bpm_at(t);
            bar.scroll_x = get_scroll_multiplier(t);
            bar.scroll_y = 0.0;
            bar.display = true;
            bar.index = counter++;
            barlines.push_back(bar);
        }
    }
    cached_notes.notes.insert(cached_notes.notes.end(), barlines.begin(), barlines.end());

    std::ranges::stable_sort(cached_notes.notes, [](const Note& a, const Note& b) {
        if (a.hit_ms != b.hit_ms) {
            return a.hit_ms < b.hit_ms;
        }
        return a.type == NoteType::BARLINE && b.type != NoteType::BARLINE;
    });

    // Build timeline from uninherited timing points (BPM changes)
    for (const auto& tp : timing_points) {
        if (tp[1] > 0 && tp[1] < 60000) {
            TimelineObject tl;
            tl.start_time = tp[0];
            tl.end_time = tp[0];
            tl.bpm = 60000.0 / tp[1];
            cached_notes.timeline.push_back(tl);
        }
    }

    modifier_moji(cached_notes);
    notes_built = true;
    return cached_notes;
}

std::tuple<NoteList, std::deque<NoteList>, std::deque<NoteList>, std::deque<NoteList>>
OsuParser::notes_to_position(int /*diff*/) {
    return {get_notes(), {}, {}, {}};
}

std::string OsuParser::get_diff_hash(int /*difficulty*/) {
    const NoteList& notes = get_notes();
    if (notes.notes.empty()) {
        return "";
    }
    return hash_notes(notes.notes);
}

std::string OsuParser::get_song_hash() {
    return get_diff_hash(0);
}
