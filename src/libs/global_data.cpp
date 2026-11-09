#include "global_data.h"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "../objects/game/zdon_ai.h"
#include "../objects/global/chara_3d.h"
#include "../objects/global/debug_menu.h"
#include "../objects/song_select/file_navigator/navigator.h"
#include "../platform/platform.h"
#include "audio/audio.h"
#include "file/filesystem.h"
#include "file/script.h"
#include "gfx/text.h"
#include "gfx/texture.h"
#include "texture_retain.h"

GlobalData global_data;

namespace {
    std::optional<std::pair<int, int>> queued_window_resize;
}

void queue_window_resize(int width, int height) {
    queued_window_resize = std::make_pair(width, height);
}

bool apply_queued_window_resize() {
#if !defined(YATAIDON_PLATFORM_IOS)
    if (!(platform_window_flags(0) & window_flag::RESIZABLE)) {
        queued_window_resize.reset(); // fixed display (console)
        return false;
    }
    if (!queued_window_resize) {
        return false;
    }
    const int width = queued_window_resize->first;
    const int height = queued_window_resize->second;
    queued_window_resize.reset();

    const bool was_fullscreen = is_window_fullscreen();
    if (was_fullscreen) {
        toggle_fullscreen();
    }
    set_window_size(width, height);
    if (was_fullscreen) {
        toggle_fullscreen();
    }
    return true;
#else
    queued_window_resize.reset();
    return false;
#endif
}

void load_skin() {
    if (!global_data.config) {
        spdlog::error("load_skin() called before config was initialized");
        return;
    }
    unload_skin();
    try {
        ensure_skin_extracted(global_data.config->paths.skin.string());
        fs::path root_skin_path = fs::path("Skins") / global_data.config->paths.skin;
        set_skin_graphics_path(root_skin_path / "Graphics");

        // Z-DON: zh is Traditional Chinese (upstream's zh is Simplified)
        static const std::unordered_map<std::string, std::string> font_family = {
            {"zh", "tw"},    {"ko", "kr"},    {"ja", "jp"},    {"zh_tw", "tw"},
            {"zh-tw", "tw"}, {"zh_cn", "cn"}, {"zh-cn", "cn"},
        };
        const std::string& lang = global_data.config->general.language;
        std::vector<std::string> font_candidates{"Fonts/font_" + lang + ".ttf"};
        if (auto family = font_family.find(lang); family != font_family.end()) {
            font_candidates.push_back("Fonts/font_" + family->second + ".ttf");
        }
        font_candidates.emplace_back("Fonts/font.ttf");
        // Z-DON: the skin's own fonts first (every candidate), then the parent skins': a skin that ships
        // only font.ttf for a language is not overridden by a parent's font_<lang>.ttf
        fs::path font_path;
        for (const std::string& candidate : font_candidates) {
            font_path = root_skin_path / candidate;
            if (fs::exists(font_path)) {
                break;
            }
        }
        if (!fs::exists(font_path)) {
            for (const std::string& candidate : font_candidates) {
                font_path = resolve_skin_path(candidate);
                if (fs::exists(font_path)) {
                    break;
                }
            }
        }
        if (!fs::exists(font_path)) {
            spdlog::error("No skin font found (tried font_{}.ttf and font.ttf) in {}", lang, root_skin_path.string());
            throw std::runtime_error("load_skin: no usable font found in " + root_skin_path.string());
        }

        tex.init(root_skin_path / "Graphics");
        // Resize is deferred so it never happens mid-frame; see queue_window_resize.
        queue_window_resize(tex.screen_width, tex.screen_height);

        global_tex.init(root_skin_path / "Graphics");
        global_tex.load_screen_textures("global");
        script_manager.init(root_skin_path / "Scripts");
        // Z-DON: screen.font_base: a language font of the skin is an overlay on its base font
        fs::path font_fallback;
        if (!tex.font_base.empty() && font_path.filename() != fs::path(tex.font_base)) {
            font_fallback = root_skin_path / "Fonts" / tex.font_base;
        }
        font_manager.init(font_path, font_fallback);
        font_manager.load_text_config(root_skin_path);
        audio.init_audio_device(root_skin_path / "Sounds", global_data.config->audio, global_data.config->volume);
        debug_menu.load_fonts();
        chara3d_prewarm(); // Z-DON
    } catch (...) {
        spdlog::error("load_skin() failed; rolling back to unloaded state");
        unload_skin();
        throw;
    }
}

// Cached shaders/textures must be released while the GL context is still alive.
#include "asset_cache.h"
void unload_skin() {
    clear_asset_caches();
    navigator.reset_for_skin_reload();
    debug_menu.clear_selection();
    debug_menu.unload_fonts();
    tex.unload_textures();
    global_tex.unload_textures();
    texture_retain::clear(); // before the GL context goes (shutdown) or the skin changes
    zdon_ai_vs_chara_release();
    chara3d_clear_part_pool();
    // the practice session's timer is a Lua object: release it while its Lua state is alive (it outlives
    // the screens; a quit from practice mode freed it in the static destructors, after lua_close)
    global_data.practice_timer.reset();
    script_manager.shutdown();
    font_manager.unload();
    unload_named_fonts();
    audio.unload_all_sounds();
    audio.unload_all_music();
    audio.close_audio_device();
}

void reset_session() {
    global_data.session_data[static_cast<int>(PlayerNum::P1)] = SessionData();
    global_data.session_data[static_cast<int>(PlayerNum::P2)] = SessionData();
}

std::string guest_id(int slot) {
    return "guest:" + std::to_string(slot + 1);
}

bool is_guest_id(const std::string& player_id) {
    return player_id.starts_with("guest:");
}

bool guests_only(bool two_player) {
    const auto& network = global_data.config->network;
    return is_guest_id(network.access_code_1) && (!two_player || is_guest_id(network.access_code_2));
}

std::string get_player_id(PlayerNum player_num) {
    if (!global_data.config) {
        spdlog::error("get_player_id() called before config was initialized");
        return "";
    }
    if (player_num != PlayerNum::P1 && player_num != PlayerNum::P2) {
        spdlog::error("get_player_id() called with invalid player_num: {}", static_cast<int>(player_num));
        return "";
    }
    return (player_num == global_data.first_login_player) ? global_data.config->network.access_code_1
                                                          : global_data.config->network.access_code_2;
}
