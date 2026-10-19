#pragma once

#ifndef YATAIDON_PLATFORM_WIN32_H
#define YATAIDON_PLATFORM_WIN32_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Win32-specific platform services.

// Converts a UTF-8 string to a filesystem::path using Windows APIs.
// Handles both UTF-8 and ANSI codepages (CP932 for Japanese).
std::filesystem::path win32_path_from_utf8(const std::string& utf8_str);

// Converts a path string with a specific encoding to a filesystem::path.
// encoding: "utf-8", "utf-8-sig", "shift-jis", etc.
std::filesystem::path win32_path_from_encoded(const std::string& path_str, const std::string& encoding,
                                              const std::filesystem::path& base_path = {});

// The command-line arguments (argv[0] included) as UTF-8, from the wide command line; empty on failure.
// The CRT's argv is in the ANSI code page, which loses characters outside it (a Japanese path on a
// Chinese-locale system).
std::vector<std::string> win32_utf8_args();

// Gets the directory containing the current executable.
std::filesystem::path win32_get_executable_dir();

// Runs the calling thread below the game's threads for CPU time.
void win32_lower_thread_priority();
// background processing mode for the calling thread: lowest CPU, I/O and memory priority
void win32_background_mode_thread();

// Initializes Windows-specific crash handlers (SEH exception filter, dbghelp stack traces).
void win32_install_crash_handlers();

// Sleeps for about `us` microseconds on a high-resolution waitable timer (Windows 10 1803+).
// Returns false when that timer is unavailable; the caller then falls back to a normal sleep.
bool win32_precise_sleep_us(long long us);
// true while a window of this process is in the foreground
bool win32_app_has_focus();
// win32_is_key_down_native without its foreground check (the caller checked win32_app_has_focus)
bool win32_key_down_unchecked(int raylib_key);

// Checks if a key is currently down using GetAsyncKeyState.
// raylib_key: Raylib key code (KEY_A, KEY_SPACE, etc.)
// Returns true if the key is currently pressed.
bool win32_is_key_down_native(int raylib_key);

// Converts a filesystem::path to a string suitable for Windows APIs (CP932 for Japanese).
std::string win32_path_to_string(const std::filesystem::path& path);

// Shift-JIS (code page 932) text as UTF-8; the bytes unchanged when they do not convert.
std::string win32_utf8_from_sjis(std::string_view text);

// One entry of a directory listing, with the attributes the listing already carries.
struct Win32DirEntry {
    std::wstring name;
    bool is_dir = false;
    bool is_reparse = false; // symlink, junction or other reparse point
    // as the directory recorded them when the file was last closed (a file open for writing can be newer)
    uint64_t size = 0;
    uint64_t ft_write = 0; // FILETIME of the last write, 100 ns since 1601
};

// Lists `dir` (without "." and "..") in the order the file system returns it.
// Returns false if the directory cannot be opened.
bool win32_list_dir(const std::filesystem::path& dir, std::vector<Win32DirEntry>& out);

// The path `path` resolves to with every link followed, or an empty string if it cannot be opened.
std::wstring win32_final_path(const std::filesystem::path& path);

// Windows audio backend initialization functions.
// Returns true if initialization succeeded.
// device_name: output device to open (empty = host API default).
bool win32_init_portaudio_wdmks(double target_sample_rate, unsigned long buffer_size, const std::string& device_name);
bool win32_init_portaudio_mme(double target_sample_rate, unsigned long buffer_size, const std::string& device_name);
void win32_close_portaudio();

// Enumerates PortAudio output device names for the given host API type
// (a PaHostApiTypeId, e.g. paWDMKS or paMME).
std::vector<std::string> win32_enumerate_portaudio_devices(int host_api_type);

#endif // YATAIDON_PLATFORM_WIN32_H