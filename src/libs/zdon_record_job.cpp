// Z-DON recording mode (zdon_record.h export_*): the game records a chart by running itself with --record as a
// hidden child process. Kept apart from zdon_record.cpp: windows.h and raylib's names do not mix.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "zdon_record.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/stat.h>
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
                if (n == 1 && (c < 0x20 || std::string_view("<>:\"/\\|?*").contains((char)c))) {
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

        // this game's exe
        fs::path exe_path() {
#ifdef _WIN32
            std::wstring exe(MAX_PATH, L'\0');
            for (;;) {
                const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
                if (n < exe.size()) {
                    exe.resize(n);
                    return exe;
                }
                exe.resize(exe.size() * 2);
            }
#else
            std::error_code ec;
            return fs::read_symlink("/proc/self/exe", ec);
#endif
        }

        // Records next to the exe
        fs::path records_dir() {
            const fs::path exe = exe_path();
            return (exe.empty() ? fs::current_path() : exe.parent_path()) / "Records";
        }

        // the output and what its recording leaves next to it: <stem>.video.tmp.mp4 / .audio.tmp.wav / .parts.txt,
        // and the parts' and courses' <stem>.part<k>.* / .track<k>.*
        void remove_outputs() {
            std::error_code ec;
            const std::u8string stem = j.out.stem().u8string() + u8".";
            std::vector<fs::path> doomed = {j.out};
            for (const fs::directory_entry& e : fs::directory_iterator(j.out.parent_path(), ec)) {
                const std::u8string name = e.path().filename().u8string();
                if (!name.starts_with(stem)) {
                    continue;
                }
                const std::u8string rest = name.substr(stem.size());
                if (rest.starts_with(u8"part") || rest.starts_with(u8"track") || rest == u8"video.tmp.mp4" ||
                    rest == u8"audio.tmp.wav") {
                    doomed.push_back(e.path());
                }
            }
            // a killed part may hold its files for a moment after the job counts it gone: retry for up to 3 s
            for (int attempt = 0; attempt < 30 && !doomed.empty(); attempt++) {
                if (attempt > 0) {
#ifdef _WIN32
                    Sleep(100);
#else
                    usleep(100000);
#endif
                }
                std::erase_if(doomed, [&](const fs::path& p) {
                    fs::remove(p, ec);
                    return !fs::exists(p, ec);
                });
            }
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

    fs::path export_path(const std::string& title, const std::vector<int>& courses, bool autoplay) {
        static const char* names[] = {"Easy", "Normal", "Hard", "Oni", "Ura"};
        std::string course;
        for (const int c : courses) {
            course += (course.empty() ? "" : "+") + (c >= 0 && c <= 4 ? std::string(names[c]) : std::to_string(c));
        }
        const fs::path dir = records_dir();
        const std::string base = file_safe(title) + "_" + course + "_" + (autoplay ? "auto" : "nohit");
        fs::path p = dir / utf8_path(base + ".mp4");
        std::error_code ec;
        for (int n = 2; fs::exists(p, ec) && n < 1000; n++) {
            p = dir / utf8_path(base + " (" + std::to_string(n) + ").mp4");
        }
        return p;
    }

    bool export_start(const fs::path& song, const std::vector<int>& courses, bool autoplay, const fs::path& out) {
        if (j.state == ExportState::Running || courses.empty()) {
            return false;
        }
        const int difficulty = courses[0];
        std::string tracks; // several lanes: --record-tracks (zdon_record.h)
        for (const int c : courses) {
            tracks += (tracks.empty() ? "" : ",") + std::to_string(c);
        }
        j = {};
        j.out = out;
        j.status = fs::absolute("cache") / "record" / "status.txt";
        std::error_code ec;
        fs::create_directories(j.status.parent_path(), ec);
        fs::remove(j.status, ec);
        fs::create_directories(out.parent_path(), ec);
#ifdef _WIN32
        const std::wstring exe = exe_path().wstring();
        const auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
        std::wstring cmd = q(exe) + L" " + q(song.wstring()) + L" " + std::to_wstring(difficulty) +
                           (autoplay ? L" --auto" : L"") + L" --record " + q(out.wstring()) + L" --record-status " +
                           q(j.status.wstring());
        if (courses.size() > 1) {
            cmd += L" --record-tracks " + fs::path(tracks).wstring();
        }
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
        const std::string exe = exe_path().string();
        std::vector<std::string> args = {exe, song.string(), std::to_string(difficulty)};
        if (autoplay) {
            args.emplace_back("--auto");
        }
        args.insert(args.end(), {"--record", out.string(), "--record-status", j.status.string()});
        if (courses.size() > 1) {
            args.insert(args.end(), {"--record-tracks", tracks});
        }
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
            TerminateJobObject(j.job, 1); // the child, its parts and their ffmpegs
            CloseHandle(j.job);
            j.job = nullptr;
        } else {
            TerminateProcess(j.process, 1);
        }
        WaitForSingleObject(j.process, 5000);
        CloseHandle(j.process);
        j.process = nullptr;
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

    bool run_children(const std::vector<std::vector<std::string>>& args, const std::function<bool()>& tick) {
#ifdef _WIN32
        const std::wstring exe = exe_path().wstring();
        const std::wstring cwd = exe_path().parent_path().wstring();
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
            lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
        }
        std::vector<HANDLE> procs;
        bool ok = true;
        for (const std::vector<std::string>& a : args) {
            std::wstring cmd = L"\"" + exe + L"\"";
            for (const std::string& s : a) {
                cmd += L" \"" + utf8_path(s).wstring() + L"\"";
            }
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                nullptr, cwd.c_str(), &si, &pi)) {
                ok = false;
                break;
            }
            if (job) {
                AssignProcessToJobObject(job, pi.hProcess);
            }
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            procs.push_back(pi.hProcess);
        }
        while (ok) {
            const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(procs.size()), procs.data(), TRUE, 250);
            if (!tick()) {
                ok = false; // the children are stopped below
                break;
            }
            if (r != WAIT_TIMEOUT) {
                break;
            }
        }
        for (HANDLE p : procs) {
            DWORD code = 1;
            if (!ok) {
                TerminateProcess(p, 1);
            }
            WaitForSingleObject(p, INFINITE);
            GetExitCodeProcess(p, &code);
            ok = ok && code == 0;
            CloseHandle(p);
        }
        if (job) {
            CloseHandle(job);
        }
        return ok;
#else
        const std::string exe = exe_path().string();
        std::vector<pid_t> pids;
        bool ok = true;
        for (const std::vector<std::string>& a : args) {
            std::vector<std::string> all = {exe};
            all.insert(all.end(), a.begin(), a.end());
            std::vector<char*> argv;
            for (std::string& s : all) {
                argv.push_back(s.data());
            }
            argv.push_back(nullptr);
            pid_t pid = -1;
            if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
                ok = false;
                break;
            }
            pids.push_back(pid);
        }
        size_t left = pids.size();
        std::vector<bool> done(pids.size(), false);
        while (ok && left > 0) {
            for (size_t i = 0; i < pids.size(); i++) {
                int st = 0;
                if (!done[i] && waitpid(pids[i], &st, WNOHANG) == pids[i]) {
                    done[i] = true;
                    left--;
                    ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
                }
            }
            if (!tick()) {
                ok = false;
                break;
            }
            usleep(250000);
        }
        for (size_t i = 0; i < pids.size(); i++) {
            if (!done[i]) {
                kill(pids[i], SIGTERM);
                int st = 0;
                waitpid(pids[i], &st, 0);
            }
        }
        return ok;
#endif
    }

    std::string fifo_name(int lane) {
#ifdef _WIN32
        return R"(\\.\pipe\zdon-record-)" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(lane);
#else
        const fs::path dir = fs::absolute("cache") / "record";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return (dir / ("lane-" + std::to_string(getpid()) + "-" + std::to_string(lane) + ".fifo")).string();
#endif
    }

    FILE* fifo_writer(const std::string& name, const std::function<void()>& created) {
#ifdef _WIN32
        HANDLE h = CreateNamedPipeW(utf8_path(name).wstring().c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT,
                                    1, 1 << 22, 0, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return nullptr;
        }
        created();
        if (!ConnectNamedPipe(h, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(h);
            return nullptr;
        }
        const int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
        if (fd < 0) {
            CloseHandle(h);
            return nullptr;
        }
        FILE* f = _fdopen(fd, "wb");
        if (!f) {
            _close(fd);
        }
        return f;
#else
        ::unlink(name.c_str());
        if (mkfifo(name.c_str(), 0600) != 0) {
            return nullptr;
        }
        created();
        return std::fopen(name.c_str(), "wb"); // waits for the reader
#endif
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
