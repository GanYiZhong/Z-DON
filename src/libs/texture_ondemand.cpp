#include "texture_ondemand.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <spdlog/spdlog.h>
#include <unordered_map>

namespace fs = std::filesystem;

namespace texture_ondemand {
    namespace {

        struct SkinRecords {
            bool dirty = false;
            std::map<std::string, std::vector<std::string>> by_screen; // screen -> "folder screen/subset"
        };

        std::unordered_map<std::string, SkinRecords>& skins() {
            static std::unordered_map<std::string, SkinRecords> s;
            return s;
        }

        fs::path file_path(const std::string& skin) {
            return fs::path("cache") / "ondemand" / (skin + ".txt");
        }

        std::string folder_key(const std::string& folder_screen, const std::string& subset) {
            return folder_screen + "/" + subset;
        }

        // the skin's records, read from its file the first time
        SkinRecords& records(const std::string& skin) {
            auto [it, inserted] = skins().try_emplace(skin);
            if (!inserted) {
                return it->second;
            }
            std::ifstream in(file_path(skin));
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const size_t tab = line.find('\t');
                if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
                    continue;
                }
                std::string folder = line.substr(tab + 1);
                const size_t slash = folder.find('/');
                if (slash == std::string::npos || slash == 0 || slash + 1 >= folder.size()) {
                    continue;
                }
                auto& list = it->second.by_screen[line.substr(0, tab)];
                if (list.size() < kMaxPerScreen && std::ranges::find(list, folder) == list.end()) {
                    list.push_back(std::move(folder));
                }
            }
            return it->second;
        }

    } // namespace

    bool enabled() {
        static const bool on = [] {
            const char* v = std::getenv("ZDON_PREFETCH_ONDEMAND");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    void record(const std::string& skin, const std::string& screen, const std::string& folder_screen,
                const std::string& subset) {
        if (!enabled() || skin.empty() || screen.empty()) {
            return;
        }
        SkinRecords& r = records(skin);
        auto& list = r.by_screen[screen];
        const std::string key = folder_key(folder_screen, subset);
        if (std::ranges::find(list, key) != list.end()) {
            return;
        }
        if (list.size() >= kMaxPerScreen) {
            list.erase(list.begin());
        }
        list.push_back(key);
        r.dirty = true;
    }

    std::vector<Folder> folders(const std::string& skin, const std::string& screen) {
        std::vector<Folder> out;
        if (!enabled() || skin.empty()) {
            return out;
        }
        SkinRecords& r = records(skin);
        auto it = r.by_screen.find(screen);
        if (it == r.by_screen.end()) {
            return out;
        }
        for (const std::string& key : it->second) {
            const size_t slash = key.find('/');
            out.push_back({key.substr(0, slash), key.substr(slash + 1)});
        }
        return out;
    }

    void forget(const std::string& skin, const std::string& screen, const Folder& folder) {
        SkinRecords& r = records(skin);
        auto it = r.by_screen.find(screen);
        if (it == r.by_screen.end()) {
            return;
        }
        auto& list = it->second;
        auto pos = std::ranges::find(list, folder_key(folder.screen, folder.subset));
        if (pos != list.end()) {
            list.erase(pos);
            r.dirty = true;
        }
    }

    void flush() {
        for (auto& [skin, r] : skins()) {
            if (!r.dirty) {
                continue;
            }
            r.dirty = false;
            const fs::path path = file_path(skin);
            fs::path tmp = path;
            tmp += ".tmp";
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                for (const auto& [screen, list] : r.by_screen) {
                    for (const std::string& key : list) {
                        out << screen << '\t' << key << '\n';
                    }
                }
                if (!out) {
                    spdlog::warn("On-demand texture prefetch list not written: {}", tmp.string());
                    continue;
                }
            }
            fs::rename(tmp, path, ec);
            if (ec) {
                spdlog::warn("On-demand texture prefetch list not written: {} ({})", path.string(), ec.message());
                fs::remove(tmp, ec);
            }
        }
    }

} // namespace texture_ondemand
