// Z-DON recording mode (zdon_record.h export_*): the game records a chart by running itself with --record as a
// hidden child process. Kept apart from zdon_record.cpp: windows.h and raylib's names do not mix.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "zdon_record.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace zdon_record {
    namespace {
        struct Job {
            ExportState state = ExportState::None;
            fs::path out, status;
            double progress = 0.0;
#ifdef _WIN32
            HANDLE process = nullptr;
            HANDLE job = nullptr; // kill-on-close: the child's ffmpeg goes with it, also if the game quits
#else
            pid_t pid = -1;
#endif
        };
        Job j;

        fs::path utf8_path(const std::string& v) {
            return fs::path(std::u8string(reinterpret_cast<const char8_t*>(v.data()), v.size()));
        }

        // a file name from a song title: no characters Windows refuses, at most 60 characters
        std::string file_safe(const std::string& title) {
            std::string out;
            size_t chars = 0;
            for (size_t i = 0; i < title.size() && chars < 60;) {
                const unsigned char c = title[i];
                const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
                if (n == 1 && (c < 0x20 || std::string_view("<>:\"/\\|?*").find((char)c) != std::string_view::npos)) {
                    out += '_';
                } else {
                    out.append(title, i, n);
                }
                i += n;
                chars++;
            }
            while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
                out.pop_back();
            }
            return out.empty() ? "song" : out;
        }

        fs::path videos_dir() {
#ifdef _WIN32
            const char* home = std::getenv("USERPROFILE");
#else
            const char* home = std::getenv("HOME");
#endif
            return (home && *home ? utf8_path(home) / "Videos" : fs::current_path()) / "YataiDON";
        }

        void remove_outputs() {
            std::error_code ec;
            for (const char* ext : {".video.tmp.mp4", ".audio.tmp.wav"}) {
                fs::path p = j.out;
                fs::remove(p.replace_extension(ext), ec);
            }
            fs::remove(j.out, ec);
        }

        double read_progress() {
#ifdef _WIN32
            FILE* f = _wfopen(j.status.wstring().c_str(), L"rb");
#else
            FILE* f = std::fopen(j.status.c_str(), "rb");
#endif
            double v = j.progress;
            if (f) {
                if (std::fscanf(f, "%lf", &v) != 1) {
                    v = j.progress;
                }
                std::fclose(f);
            }
            return std::max(j.progress, std::min(v, 1.0));
        }
    } // namespace

    fs::path export_path(const std::string& title, int difficulty, bool autoplay) {
        static const char* courses[] = {"Easy", "Normal", "Hard", "Oni", "Ura"};
        const std::string course =
            difficulty >= 0 && difficulty <= 4 ? courses[difficulty] : std::to_string(difficulty);
        const fs::path dir = videos_dir();
        const std::string base = file_safe(title) + "_" + course + "_" + (autoplay ? "auto" : "nohit");
        fs::path p = dir / utf8_path(base + ".mp4");
        std::error_code ec;
        for (int n = 2; fs::exists(p, ec) && n < 1000; n++) {
            p = dir / utf8_path(base + " (" + std::to_string(n) + ").mp4");
        }
        return p;
    }

    bool export_start(const fs::path& song, int difficulty, bool autoplay, const fs::path& out) {
        if (j.state == ExportState::Running) {
            return false;
        }
        j = {};
        j.out = out;
        j.status = fs::absolute("cache") / "record" / "status.txt";
        std::error_code ec;
        fs::create_directories(j.status.parent_path(), ec);
        fs::remove(j.status, ec);
        fs::create_directories(out.parent_path(), ec);
#ifdef _WIN32
        std::wstring exe(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
            if (n < exe.size()) {
                exe.resize(n);
                break;
            }
            exe.resize(exe.size() * 2);
        }
        const auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
        std::wstring cmd = q(exe) + L" " + q(song.wstring()) + L" " + std::to_wstring(difficulty) +
                           (autoplay ? L" --auto" : L"") + L" --record " + q(out.wstring()) + L" --record-status " +
                           q(j.status.wstring());
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const std::wstring cwd = fs::path(exe).parent_path().wstring();
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                            nullptr, cwd.c_str(), &si, &pi)) {
            return false;
        }
        j.job = CreateJobObjectW(nullptr, nullptr);
        if (j.job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
            lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j.job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
            AssignProcessToJobObject(j.job, pi.hProcess);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        j.process = pi.hProcess;
#else
        const std::string exe = fs::read_symlink("/proc/self/exe", ec).string();
        std::vector<std::string> args = {exe, song.string(), std::to_string(difficulty)};
        if (autoplay) {
            args.emplace_back("--auto");
        }
        args.insert(args.end(), {"--record", out.string(), "--record-status", j.status.string()});
        std::vector<char*> argv;
        for (std::string& a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        if (posix_spawn(&j.pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
            j.pid = -1;
            return false;
        }
#endif
        j.state = ExportState::Running;
        return true;
    }

    ExportStatus export_poll() {
        if (j.state == ExportState::Running) {
            j.progress = read_progress();
            bool exited = false;
            bool ok = false;
#ifdef _WIN32
            if (WaitForSingleObject(j.process, 0) == WAIT_OBJECT_0) {
                DWORD code = 1;
                GetExitCodeProcess(j.process, &code);
                CloseHandle(j.process);
                j.process = nullptr;
                if (j.job) {
                    CloseHandle(j.job);
                    j.job = nullptr;
                }
                exited = true;
                ok = code == 0;
            }
#else
            int st = 0;
            if (waitpid(j.pid, &st, WNOHANG) == j.pid) {
                exited = true;
                ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
                j.pid = -1;
            }
#endif
            if (exited) {
                std::error_code ec;
                ok = ok && fs::exists(j.out, ec);
                j.state = ok ? ExportState::Done : ExportState::Failed;
                j.progress = ok ? 1.0 : j.progress;
            }
        }
        return {j.state, j.progress, j.out};
    }

    void export_cancel() {
        if (j.state != ExportState::Running) {
            return;
        }
#ifdef _WIN32
        if (j.job) {
            TerminateJobObject(j.job, 1); // the child and its ffmpeg
            CloseHandle(j.job);
            j.job = nullptr;
        } else {
            TerminateProcess(j.process, 1);
        }
        WaitForSingleObject(j.process, 5000);
        CloseHandle(j.process);
        j.process = nullptr;
        Sleep(200); // ffmpeg's handles on the parts
#else
        kill(j.pid, SIGTERM);
        int st = 0;
        waitpid(j.pid, &st, 0);
        j.pid = -1;
#endif
        remove_outputs();
        j.state = ExportState::None;
    }

    void export_forget() {
        if (j.state != ExportState::Running) {
            j = {};
        }
    }

    void show_in_folder(const fs::path& file) {
#ifdef _WIN32
        std::wstring cmd = L"explorer.exe /select,\"" + file.wstring() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
#else
        pid_t pid = -1;
        std::string dir = file.parent_path().string();
        char* argv[] = {const_cast<char*>("xdg-open"), dir.data(), nullptr};
        posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ);
#endif
    }
} // namespace zdon_record
