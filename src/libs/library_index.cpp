#include "library_index.h"

namespace {
    std::mutex published_mutex;
    std::shared_ptr<const LibraryIndex> published;
} // namespace

// One spelling per folder: the walk joins names onto the configured root as written, the navigator
// onto the same root without its trailing separator. lexically_normal would also do, but costs more
// than the rest of a lookup.
std::wstring LibraryIndex::key(const fs::path& dir) {
    const std::wstring in = dir.wstring();
    std::wstring k;
    k.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        const wchar_t c = in[i] == L'/' ? L'\\' : in[i];
        if (c == L'\\' && i > 1 && !k.empty() && k.back() == L'\\') {
            continue; // a repeated separator (not the leading pair of a UNC path)
        }
        k += c;
    }
    while (k.size() > 1 && k.back() == L'\\') {
        k.pop_back();
    }
    return k;
}

const LibraryIndex::Dir* LibraryIndex::find(const fs::path& dir) const {
    auto it = by_path_.find(key(dir));
    return it != by_path_.end() ? &dirs_[it->second] : nullptr;
}

int32_t LibraryIndex::add_dir(const fs::path& path, std::vector<Entry> entries) {
    const int32_t id = static_cast<int32_t>(dirs_.size());
    dirs_.push_back(Dir{std::move(entries)});
    by_path_[key(path)] = id;
    return id;
}

bool LibraryIndex::song_list_written(const fs::path& dir) const {
    std::lock_guard<std::mutex> lock(written_mutex_);
    return !written_.empty() && written_.count(key(dir)) > 0;
}

void LibraryIndex::note_song_list_written(const fs::path& dir) const {
    std::lock_guard<std::mutex> lock(written_mutex_);
    written_.insert(key(dir));
}

std::shared_ptr<const LibraryIndex> library_index() {
    std::lock_guard<std::mutex> lock(published_mutex);
    return published;
}

void publish_library_index(std::shared_ptr<const LibraryIndex> index) {
    std::lock_guard<std::mutex> lock(published_mutex);
    published = std::move(index);
}
