#pragma once

#include <optional>
#include <string_view>
#include <variant>
#ifdef SUPPORT_FUMEN
#include "../optional/fumen.h"
#endif
#include "../global_data.h"
#include "osu.h"
#include "tja.h"

// Exposes the same public interface as TJAParser so existing code
// can be updated by substituting SongParser for TJAParser.
class SongParser {
public:
    TJAMetadata metadata;
    TJAEXData ex_data;
    fs::path file_path;
    int start_delay = 0;
    PlayerNum player_num = PlayerNum::ALL;

    SongParser() = default;
    SongParser(const fs::path& path, int start_delay = 0, PlayerNum player_num = PlayerNum::ALL);
    void get_metadata() {}
    std::string get_difficulty_name();

    std::tuple<NoteList, std::deque<NoteList>, std::deque<NoteList>, std::deque<NoteList>> notes_to_position(int diff);

    std::string get_song_hash();
    std::string get_diff_hash(int difficulty);

    // Z-DON: the song select's boxes are built from a record of the metadata (box_record, stored in
    // scores.db) instead of reading every chart of a folder. A parser made from one holds metadata,
    // ex_data and file_path only; the chart is read (as SongParser(path) would) the first time
    // something needs more than that: notes, hashes, impl. Only TJA and osu charts have records.
    static std::optional<SongParser> from_box_record(const fs::path& path, std::string_view record);
    // everything from_box_record restores, or "" for a chart kind without records
    std::string box_record();
    // false: made from a record and not read since (impl is not the chart's yet)
    bool parsed() const { return !lazy_; }

#ifdef SUPPORT_FUMEN
    std::variant<TJAParser, OsuParser, FumenParser> impl;
#else
    std::variant<TJAParser, OsuParser> impl;
#endif

private:
    void sync();
    // reads the chart into impl if this parser was made from a record; false if that failed
    bool ensure_parsed();
    bool lazy_ = false;
    int kind_ = -1;               // 0 TJA, 1 osu, -1 other (no record)
    std::string difficulty_name_; // an osu record's, while lazy
    fs::path source_path_;        // what a lazy parser reads
};
