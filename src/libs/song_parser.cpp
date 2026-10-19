#include "song_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <system_error>

namespace {
    std::string lower_extension(const fs::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return ext;
    }

    // box_record encoding: native-endian fixed-width numbers, strings as a u32 length and their code
    // units, maps in their order. Only read back by the build that wrote it (the format number).
    constexpr uint32_t BOX_RECORD_FORMAT = 1;

    struct RecordWriter {
        std::string out;
        template<class T>
        void num(T v) {
            out.append(reinterpret_cast<const char*>(&v), sizeof(T));
        }
        template<class S>
        void str(const S& s) {
            num<uint32_t>(static_cast<uint32_t>(s.size()));
            out.append(reinterpret_cast<const char*>(s.data()), s.size() * sizeof(typename S::value_type));
        }
        void path(const fs::path& p) { str(p.native()); }
        void ints(const std::vector<int>& v) {
            num<uint32_t>(static_cast<uint32_t>(v.size()));
            for (int x : v) {
                num<int32_t>(x);
            }
        }
        void text_map(const std::map<std::string, std::string>& m) {
            num<uint32_t>(static_cast<uint32_t>(m.size()));
            for (const auto& [k, v] : m) {
                str(k);
                str(v);
            }
        }
    };

    struct RecordReader {
        std::string_view in;
        bool ok = true;
        template<class T>
        T num() {
            T v{};
            if (in.size() < sizeof(T)) {
                ok = false;
                return v;
            }
            std::memcpy(&v, in.data(), sizeof(T));
            in.remove_prefix(sizeof(T));
            return v;
        }
        template<class S>
        S str() {
            const uint32_t n = num<uint32_t>();
            S s;
            const size_t bytes = size_t(n) * sizeof(typename S::value_type);
            if (!ok || in.size() < bytes) {
                ok = false;
                return s;
            }
            s.resize(n);
            std::memcpy(s.data(), in.data(), bytes);
            in.remove_prefix(bytes);
            return s;
        }
        fs::path path() { return fs::path(str<fs::path::string_type>()); }
        std::vector<int> ints() {
            const uint32_t n = num<uint32_t>();
            std::vector<int> v;
            for (uint32_t i = 0; ok && i < n; i++) {
                v.push_back(num<int32_t>());
            }
            return v;
        }
        std::map<std::string, std::string> text_map() {
            const uint32_t n = num<uint32_t>();
            std::map<std::string, std::string> m;
            for (uint32_t i = 0; ok && i < n; i++) {
                std::string k = str<std::string>();
                std::string v = str<std::string>();
                m.emplace(std::move(k), std::move(v));
            }
            return m;
        }
    };
} // namespace

SongParser::SongParser(const fs::path& path, int start_delay, PlayerNum player_num) {
    // A gen 4 song is a folder of chart files rather than a single file.
    std::string ext = lower_extension(path);
#ifdef SUPPORT_FUMEN
    std::error_code dir_ec;
    const bool is_dir = fs::is_directory(path, dir_ec);
    if (dir_ec) {
        throw fs::filesystem_error("SongParser: cannot stat chart path", path, dir_ec);
    }
    if (is_dir) {
        impl = FumenParser(path, start_delay);
    } else if (ext == ".osu") {
        impl = OsuParser(path);
    } else if (ext == ".bin") {
        impl = FumenParser(path, start_delay);
    } else if (ext == ".tja") {
        impl = TJAParser(path, start_delay, static_cast<int>(player_num));
    } else {
        spdlog::warn("SongParser: unrecognized chart extension '{}' for {} -- treating as TJA", ext, path.string());
        impl = TJAParser(path, start_delay, static_cast<int>(player_num));
    }
#else
    if (ext == ".osu") {
        impl = OsuParser(path);
    } else {
        if (ext != ".tja") {
            spdlog::warn("SongParser: unrecognized chart extension '{}' for {} -- treating as TJA", ext, path.string());
        }
        impl = TJAParser(path, start_delay, static_cast<int>(player_num));
    }
#endif
    sync();
}

void SongParser::sync() {
    std::visit(
        [this](auto& p) {
            metadata = p.metadata;
            ex_data = p.ex_data;
            file_path = p.file_path;
        },
        impl);
    kind_ = std::holds_alternative<TJAParser>(impl) ? 0 : std::holds_alternative<OsuParser>(impl) ? 1 : -1;
}

bool SongParser::ensure_parsed() {
    if (!lazy_) {
        return true;
    }
    try {
        // the record's metadata stays: the box keeps showing what it was built with
        SongParser full(source_path_);
        impl = std::move(full.impl);
        lazy_ = false;
        difficulty_name_.clear();
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("SongParser: cannot read {}: {}", source_path_.string(), e.what());
        return false;
    }
}

std::optional<SongParser> SongParser::from_box_record(const fs::path& path, std::string_view record) {
    RecordReader r{record};
    if (r.num<uint32_t>() != BOX_RECORD_FORMAT) {
        return std::nullopt;
    }
    SongParser p;
    p.kind_ = r.num<int8_t>();
    if (p.kind_ != 0 && p.kind_ != 1) {
        return std::nullopt;
    }
    p.file_path = r.path();
    TJAMetadata& m = p.metadata;
    m.title = r.text_map();
    m.subtitle = r.text_map();
    m.subtitle_full_display = r.num<uint8_t>() != 0;
    m.genre = r.str<std::string>();
    m.wave = r.path();
    m.demostart = r.num<double>();
    m.offset = r.num<double>();
    m.bpm = r.num<double>();
    m.bgmovie = r.path();
    m.movieoffset = r.num<double>();
    m.preimage = r.path();
    m.scene_preset = r.str<std::string>();
    const uint32_t courses = r.num<uint32_t>();
    for (uint32_t i = 0; r.ok && i < courses; i++) {
        const int key = r.num<int32_t>();
        CourseData c;
        c.level = r.num<double>();
        c.balloon = r.ints();
        c.scoreinit = r.ints();
        c.scorediff = r.num<double>();
        c.is_branching = r.num<uint8_t>() != 0;
        m.course_data.emplace(key, std::move(c));
    }
    p.ex_data.new_audio = r.num<uint8_t>() != 0;
    p.ex_data.old_audio = r.num<uint8_t>() != 0;
    p.ex_data.limited_time = r.num<uint8_t>() != 0;
    p.difficulty_name_ = r.str<std::string>();
    if (!r.ok || !r.in.empty()) {
        return std::nullopt;
    }
    p.lazy_ = true;
    p.source_path_ = path;
    return p;
}

std::string SongParser::box_record() {
    if (kind_ != 0 && kind_ != 1) {
        return {};
    }
    RecordWriter w;
    w.num<uint32_t>(BOX_RECORD_FORMAT);
    w.num<int8_t>(static_cast<int8_t>(kind_));
    w.path(file_path);
    const TJAMetadata& m = metadata;
    w.text_map(m.title);
    w.text_map(m.subtitle);
    w.num<uint8_t>(m.subtitle_full_display);
    w.str(m.genre);
    w.path(m.wave);
    w.num<double>(m.demostart);
    w.num<double>(m.offset);
    w.num<double>(m.bpm);
    w.path(m.bgmovie);
    w.num<double>(m.movieoffset);
    w.path(m.preimage);
    w.str(m.scene_preset);
    w.num<uint32_t>(static_cast<uint32_t>(m.course_data.size()));
    for (const auto& [key, c] : m.course_data) {
        w.num<int32_t>(key);
        w.num<double>(c.level);
        w.ints(c.balloon);
        w.ints(c.scoreinit);
        w.num<double>(c.scorediff);
        w.num<uint8_t>(c.is_branching);
    }
    w.num<uint8_t>(ex_data.new_audio);
    w.num<uint8_t>(ex_data.old_audio);
    w.num<uint8_t>(ex_data.limited_time);
    // what SongBoxOsu shows (a TJA's name depends on its last notes_to_position, which no box runs)
    w.str(kind_ == 1 ? get_difficulty_name() : std::string());
    return std::move(w.out);
}

std::string SongParser::get_difficulty_name() {
    if (lazy_ && kind_ == 1) {
        return difficulty_name_;
    }
    if (!ensure_parsed()) {
        return "";
    }
    return std::visit([](auto& p) { return p.get_difficulty_name(); }, impl);
}

std::tuple<NoteList, std::deque<NoteList>, std::deque<NoteList>, std::deque<NoteList>>
SongParser::notes_to_position(int diff) {
    if (!ensure_parsed()) {
        throw std::runtime_error("SongParser: cannot read " + source_path_.string());
    }
    return std::visit([diff](auto& p) { return p.notes_to_position(diff); }, impl);
}

std::string SongParser::get_song_hash() {
    if (!ensure_parsed()) {
        return "";
    }
    return std::visit([](auto& p) { return p.get_song_hash(); }, impl);
}

std::string SongParser::get_diff_hash(int difficulty) {
    if (!ensure_parsed()) {
        return "";
    }
    return std::visit([difficulty](auto& p) { return p.get_diff_hash(difficulty); }, impl);
}
