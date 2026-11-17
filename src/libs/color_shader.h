#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "asset_cache.h"
#include "gfx.h"

// Up to 64 colours (one "byte" each) on every platform: compiling a shader per request is waste.
inline AssetCache<Shader>& color_shader_cache() {
    static AssetCache<Shader> cache(64);
    return cache;
}

inline std::shared_ptr<Shader> load_color_shader(Color color, Color from = {142, 212, 30, 255}) {
    // Z-DON: the last few pairs by a packed key first -- a skin recolours every frame (song_info.lua), and the
    // string key below costs a heap allocation and the cache's lock. Main thread only (GL), as before.
    struct Recent {
        uint64_t key = UINT64_MAX;
        std::shared_ptr<Shader> shader;
    };
    static std::array<Recent, 8> recent;
    static size_t recent_next = 0;
    const uint64_t packed = (static_cast<uint64_t>(from.r) << 40) | (static_cast<uint64_t>(from.g) << 32) |
                            (static_cast<uint64_t>(from.b) << 24) | (static_cast<uint64_t>(color.r) << 16) |
                            (static_cast<uint64_t>(color.g) << 8) | color.b;
    for (const Recent& r : recent) {
        if (r.key == packed) {
            return r.shader;
        }
    }
    auto remember = [&](std::shared_ptr<Shader> s) {
        recent[recent_next] = {packed, s};
        recent_next = (recent_next + 1) % recent.size();
        return s;
    };
    auto rgb = [](Color c) {
        return std::to_string((static_cast<unsigned>(c.r) << 16) | (static_cast<unsigned>(c.g) << 8) | c.b);
    };
    const std::string key = rgb(from) + ">" + rgb(color);

    if (auto shader = color_shader_cache().get(key)) {
        return remember(shader);
    }
    auto shader = std::shared_ptr<Shader>(new Shader(load_shader("shader/dummy.vs", "shader/colortransform.fs")),
                                          [](Shader* shader) {
                                              unload_shader(*shader);
                                              delete shader;
                                          });
    float source[3] = {from.r / 255.0f, from.g / 255.0f, from.b / 255.0f};
    float target[3] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f};
    set_shader_value(*shader, get_shader_location(*shader, "sourceColor"), source, shader_uniform::VEC3);
    set_shader_value(*shader, get_shader_location(*shader, "targetColor"), target, shader_uniform::VEC3);
    color_shader_cache().put(key, shader, 1);
    return remember(shader);
}
