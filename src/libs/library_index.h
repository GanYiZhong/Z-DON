#pragma once

#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// Every folder the loading screen's library walk (get_song_files) listed, kept so the song select
// can answer its folder questions -- chart counts, box.def / .osu / child folder checks, the dojo's
// course files -- from memory instead of walking the library again with a stat per entry.
//
// Built once per start (Windows; elsewhere nothing is published) and read-only afterwards, except
// for the song_list.txt files written since (note_song_list_written). A folder is only here when the
// walk listed it under the path the navigator asks about: folders inside a gen3/gen4 data root, behind
// a link to a folder the walk had already entered, or that could not be listed are missing (Entry::dir
// is -1), and a question that needs one gets no answer, so the caller reads the disk as before.
// Folders added to the library after the walk are not seen (charts there were not in the song
// cache either: the library needs a restart for them).
class LibraryIndex {
public:
    struct Entry {
        std::wstring name;
        bool is_dir = false;
        bool is_reparse = false; // a link: is_dir describes the link, not its target
        int32_t dir = -1;        // the folder's listing in this index, -1 = not listed by the walk
    };
    struct Dir {
        std::vector<Entry> entries; // in listing order
    };

    // the listing of `dir` (as the walk spelled it, separators and a trailing one aside)
    const Dir* find(const fs::path& dir) const;
    const Dir* child(const Entry& entry) const { return entry.dir >= 0 ? &dirs_[entry.dir] : nullptr; }

    // true if a song_list.txt was written into `dir` after the walk (it is not in the listing)
    bool song_list_written(const fs::path& dir) const;
    void note_song_list_written(const fs::path& dir) const;

    // building, by the walk
    int32_t add_dir(const fs::path& path, std::vector<Entry> entries);
    void set_child(int32_t dir, size_t entry, int32_t child) { dirs_[dir].entries[entry].dir = child; }
    std::vector<Entry>& entries_of(int32_t dir) { return dirs_[dir].entries; }

private:
    static std::wstring key(const fs::path& dir);

    std::vector<Dir> dirs_;
    std::unordered_map<std::wstring, int32_t> by_path_;
    mutable std::mutex written_mutex_;
    mutable std::unordered_set<std::wstring> written_;
};

// the index of the last walk the loading screen published, or nullptr
std::shared_ptr<const LibraryIndex> library_index();
void publish_library_index(std::shared_ptr<const LibraryIndex> index);

// `name` has extension `ext` (".tja"), as fs::path(name).extension() == ext decides
inline bool name_has_ext(const std::wstring& name, const wchar_t* ext) {
    const size_t n = std::wcslen(ext);
    return name.size() > n && name.compare(name.size() - n, n, ext) == 0;
}
