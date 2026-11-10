#include "zdon_update.h"

#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <vector>

#include "../fixed_step.h"
#include "../global_data.h"
#include "../math/sha256.h"

#if defined(YATAIDON_HAS_HTTP)
#include <cpr/cpr.h>
#endif

#ifdef _WIN32
#include "../../platform/platform_windows.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef CloseWindow
#undef ShowCursor
#undef DrawText
#undef LoadImage
#undef PlaySound
#undef GetObject
#endif

namespace fs = std::filesystem;

namespace zdon_update {

    namespace {
        std::mutex mutex;
        Status current;
        std::thread worker;
        std::atomic<bool> started{false};

        std::string trim(std::string s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
                s.pop_back();
            }
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
                i++;
            }
            return s.substr(i);
        }

        std::string source_url() {
            std::string url = global_data.config->network.zdon_update_url;
            if (url.empty()) {
                url = server_base();
                if (!url.empty()) {
                    url += "/update/windows";
                }
            }
            while (!url.empty() && url.back() == '/') {
                url.pop_back();
            }
            return url;
        }

        // the folder the game runs from, where zdon_update.ps1 installs
        fs::path install_dir() {
#ifdef _WIN32
            wchar_t buf[MAX_PATH * 4];
            const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            if (n > 0 && n < std::size(buf)) {
                return fs::path(std::wstring(buf, n)).parent_path();
            }
#endif
            std::error_code ec;
            return fs::current_path(ec);
        }

        std::string file_sha256(const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return "";
            }
            crypto::Sha256 hasher;
            std::vector<char> buf(1 << 20);
            while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
                hasher.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(in.gcount()));
            }
            return crypto::to_hex(hasher.finalize());
        }

#if defined(YATAIDON_HAS_HTTP)
        void check(std::string url) {
#ifdef _WIN32
            win32_background_mode_thread();
#endif
            cpr::Response version = cpr::Get(cpr::Url{url + "/version.txt"}, cpr::Timeout{8000});
            if (version.status_code != 200) {
                spdlog::info("Z-DON update: {} not reachable (HTTP {})", url, version.status_code);
                return;
            }
            const std::string remote = trim(version.text);
            const fs::path dir = install_dir();
            std::string local;
            if (std::ifstream f(dir / ".zdon_version"); f) {
                std::getline(f, local);
                local = trim(local);
            }
            if (!remote.empty() && remote == local) {
                spdlog::info("Z-DON update: up to date ({})", local);
                return;
            }
            cpr::Response manifest = cpr::Get(cpr::Url{url + "/checksums.sha256"}, cpr::Timeout{15000});
            if (manifest.status_code != 200 || manifest.text.empty() || manifest.text.back() != '\n') {
                spdlog::warn("Z-DON update: no complete checksums.sha256 at {} (HTTP {})", url, manifest.status_code);
                return;
            }
            // a different version.txt alone isn't an update (an install the updater never touched has none):
            // count the listed files that differ here
            int differ = 0;
            std::istringstream lines(manifest.text);
            std::string line;
            while (std::getline(lines, line)) {
                line = trim(line);
                const size_t sp = line.find_first_of(" \t");
                if (sp == std::string::npos || sp != 64) {
                    continue;
                }
                std::string rel = trim(line.substr(sp));
                if (!rel.empty() && rel.front() == '*') {
                    rel.erase(0, 1);
                }
                const fs::path local_file = dir / fs::path(std::u8string(rel.begin(), rel.end()));
                std::string want = line.substr(0, 64);
                for (char& c : want) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (file_sha256(local_file) != want) {
                    differ++;
                }
            }
            std::lock_guard lock(mutex);
            current.version = remote;
            current.files = differ;
            current.available = differ > 0;
            spdlog::info("Z-DON update: server {} / installed {}: {} file(s) differ", remote,
                         local.empty() ? "(unknown)" : local, differ);
        }
#endif
    } // namespace

    void start_check() {
        if (started.exchange(true) || !global_data.config || fixed_step::on() ||
            !global_data.config->network.zdon_update_check) {
            return;
        }
        const std::string url = source_url();
        if (url.empty()) {
            return;
        }
#if defined(YATAIDON_HAS_HTTP)
        worker = std::thread([url] {
            try {
                check(url);
            } catch (const std::exception& e) {
                spdlog::warn("Z-DON update check failed: {}", e.what());
            }
        });
#endif
    }

    Status status() {
        std::lock_guard lock(mutex);
        return current;
    }

    bool launch_updater() {
#ifdef _WIN32
        const fs::path dir = install_dir();
        const fs::path script = dir / "zdon_update.ps1";
        std::error_code ec;
#if defined(YATAIDON_HAS_HTTP)
        // an install the updater never reached has no script yet: the release carries it
        for (const char* name : {"zdon_update.ps1", "zdon_update.bat"}) {
            if (fs::exists(dir / name, ec) || source_url().empty()) {
                continue;
            }
            cpr::Response r = cpr::Get(cpr::Url{source_url() + "/files/" + name}, cpr::Timeout{8000});
            if (r.status_code == 200 && !r.text.empty()) {
                std::ofstream(dir / name, std::ios::binary) << r.text;
                spdlog::info("Z-DON update: fetched {} from the server", name);
            }
        }
#endif
        if (!fs::exists(script, ec)) {
            spdlog::warn("Z-DON update: {} is missing", script.string());
            return false;
        }
        std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() +
                           L"\" -WaitPid " + std::to_wstring(GetCurrentProcessId()) + L" -Restart -PauseOnError";
        // the source this check used (a compiled-in server isn't in config.toml for the script to find)
        if (const std::string url = source_url(); !url.empty()) {
            cmd += L" -Url \"" + fs::path(std::u8string(url.begin(), url.end())).wstring() + L"\"";
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const std::wstring cwd = dir.wstring();
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, cwd.c_str(), &si,
                            &pi)) {
            spdlog::warn("Z-DON update: could not start the updater (error {})", GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        spdlog::info("Z-DON update: updater started; closing the game");
        return true;
#else
        return false;
#endif
    }

    std::string server_base() {
        std::string url = global_data.config ? global_data.config->network.zdon_server_url : "";
#if defined(NETWORK_URL)
        if (url.empty()) {
            url = NETWORK_URL;
        }
#endif
        while (!url.empty() && url.back() == '/') {
            url.pop_back();
        }
        return url;
    }

    std::optional<std::string> http_get(const std::string& url, int timeout_ms, int* status) {
#if defined(YATAIDON_HAS_HTTP)
        cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Timeout{timeout_ms});
        if (status) {
            *status = static_cast<int>(r.status_code);
        }
        if (r.status_code == 200 && r.error.code == cpr::ErrorCode::OK) {
            return std::move(r.text);
        }
#else
        if (status) {
            *status = 0;
        }
#endif
        return std::nullopt;
    }

    void shutdown() {
        if (worker.joinable()) {
            worker.join();
        }
    }

} // namespace zdon_update
