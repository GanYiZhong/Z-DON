#pragma once

namespace ray {
    extern "C" {
#include <raylib.h>
    }
} // namespace ray

#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
#include "sdl_compat.h"
#endif
#include <string>

#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS) || defined(__EMSCRIPTEN__)
// GLES builds load the shaders under shader/es/
inline std::string repath_shader(const char* p) {
    if (!p) {
        return "";
    }
    std::string s(p);
    const std::string prefix = "shader/";
    const std::string es_prefix = "shader/es/";
    if (s.compare(0, es_prefix.size(), es_prefix) == 0) {
        return s;
    }
    if (s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix) {
        s.insert(prefix.size(), "es/");
    }
    return s;
}
#endif
inline void draw_rectangle(int x, int y, int w, int h, ray::Color color) {
    ray::DrawRectangle(x, y, w, h, color);
}

inline void draw_rectangle(ray::Rectangle rec, ray::Color color) {
    ray::DrawRectangleRec(rec, color);
}

inline ray::Shader load_shader(const char* vs_path, const char* fs_path) {
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
    auto read_asset = [](const std::string& path) -> std::string {
        if (path.empty()) {
            return "";
        }
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "r");
        if (!io) {
            return "";
        }
        Sint64 size = SDL_GetIOSize(io);
        if (size <= 0) {
            SDL_CloseIO(io);
            return "";
        }
        std::string buf(static_cast<size_t>(size), '\0');
        size_t got = SDL_ReadIO(io, buf.data(), static_cast<size_t>(size));
        SDL_CloseIO(io);
        if (got != static_cast<size_t>(size)) {
            return "";
        }
        return buf;
    };

    std::string vs_repath = repath_shader(vs_path);
    std::string fs_repath = repath_shader(fs_path);
    std::string vs_src = vs_path ? read_asset(vs_repath) : std::string{};
    std::string fs_src = fs_path ? read_asset(fs_repath) : std::string{};
    return ray::LoadShaderFromMemory(vs_src.empty() ? nullptr : vs_src.c_str(),
                                     fs_src.empty() ? nullptr : fs_src.c_str());
#endif
#ifdef __EMSCRIPTEN__
    std::string vs_repath = repath_shader(vs_path);
    std::string fs_repath = repath_shader(fs_path);
    return ray::LoadShader(vs_path ? vs_repath.c_str() : nullptr, fs_path ? fs_repath.c_str() : nullptr);
#endif
    return ray::LoadShader(vs_path, fs_path);
}
