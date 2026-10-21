#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

class SkinUpdater {
public:
    struct Status {
        bool running = false;     // a pass is currently in progress
        bool finished = false;    // a pass has completed (true even when disabled)
        bool any_updated = false; // the current pass identified >= 1 file to update
        std::string current_skin; // skin being processed ("" between skins)
        std::string current_file; // file being downloaded ("" while scanning)
        int files_updated = 0;    // running total of files written this pass
        int files_done = 0;       // files of current_skin downloaded (or failed) so far
        int files_total = 0;      // files of current_skin to download; 0 while scanning
    };

    void start();

    bool finished();

    bool applying_updates();

    bool updated_this_pass();

    Status snapshot();

    void shutdown();

private:
    void scan_skins();
    friend class SkinUpdatePass; // the pass's tasks report through the members below
    void set_current_skin(const std::string& name);
    void set_current_file(const std::string& skin, const std::string& file);
    void note_file_updated();
    void set_progress(const std::string& skin, int done, int total);

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> any_updated_{false};

    std::mutex status_mutex_;
    std::string current_skin_;
    std::string current_file_;
    int files_updated_ = 0;
    std::unordered_map<std::string, std::pair<int, int>> progress_; // skin -> files done, to download

    std::thread thread_;
};

extern SkinUpdater skin_updater;
