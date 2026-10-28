#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "animation.h"
#include "color_utils.h"
#include "ray.h" // IWYU pragma: keep
#include "skin_config.h"
#include "texture_lazy.h"

namespace fs = std::filesystem;

enum class Mirror : uint8_t {
    NONE,
    HORIZONTAL,
    VERTICAL
};

inline Mirror mirror_from_string(const std::string& s) {
    if (s == "horizontal") {
        return Mirror::HORIZONTAL;
    }
    if (s == "vertical") {
        return Mirror::VERTICAL;
    }
    return Mirror::NONE;
}

struct DrawTextureParams {
    Color color = Color::WHITE;
    int frame = 0;
    float scale = 1.0f;
    bool center = false;
    Mirror mirror = Mirror::NONE;
    float x = 0, y = 0, x2 = 0, y2 = 0;
    Vec2 origin = {0, 0};
    float rotation = 0;
    double fade = 1.1f;
    int index = 0;
    std::optional<Rect> src = std::nullopt;
    std::optional<int> blend = std::nullopt;
};

struct SkinInfo {
    float x;
    float y;
    int font_size;
    float width;
    float height;
    std::map<std::string, std::string> text;
    float outline = -1.0f;

    SkinInfo(float x = 0, float y = 0, int font_size = 0, float width = 0, float height = 0,
             const std::map<std::string, std::string>& text = {}, float outline = -1.0f)
        : x(x), y(y), font_size(font_size), width(width), height(height), text(text), outline(outline) {}
};

struct Chara3DConfig {
    float scale = 650.0f;
    float rot_x = 181.25f;
    float rot_y = 27.5f;
    float rot_z = 0.0f;
};

struct TextureObject {
    std::string name;
    int width;
    int height;
    std::vector<int> x;
    std::vector<int> y;
    std::vector<int> x2;
    std::vector<int> y2;
    std::optional<std::vector<Rect>> crop_data;

    float opaque_right = 0.0f;
    // Z-DON texture density (screen.texture_density): the image's pixels per unit of width / height /
    // x / y / x2 / y2, which are logical. Only the source rect is scaled by it (crop_data is in image pixels).
    int density = 1;
    TextureObject(const std::string& name, int width, int height)
        : name(name), width(width), height(height), x{0}, y{0}, x2{width}, y2{height} {}
    virtual ~TextureObject() = default;

    float scissor_right() const {
        if (opaque_right > 0.0f) {
            return opaque_right;
        }
        return x2.empty() ? 0.0f : static_cast<float>(x2[0]);
    }

    // The frame's GL texture, uploaded first if it still waits for its first use (texture_lazy.h):
    // everything that hands the texture to GL goes through here.
    const Texture* frame_texture(int frame) const {
        const Texture* t = frame_texture_raw(frame);
        if (t && t->id == 0) {
            texture_lazy::upload(t);
        }
        return t;
    }

    // The frame's texture as it is (id 0 while the upload is pending; width / height / format are set):
    // for bookkeeping that must not upload it.
    virtual const Texture* frame_texture_raw(int frame) const { return nullptr; }

    virtual int frame_count() const { return 1; }
};

struct SingleTexture : public TextureObject {
    std::shared_ptr<Texture> texture;

    SingleTexture(const std::string& name, std::shared_ptr<Texture> tex)
        : TextureObject(name, tex->width, tex->height), texture(std::move(tex)) {
        texture_lazy::set_wrap(texture.get(), ray::TEXTURE_WRAP_CLAMP);
    }

    const Texture* frame_texture_raw(int) const override { return texture.get(); }
};

struct FramedTexture : public TextureObject {
    std::vector<std::shared_ptr<Texture>> textures;

    FramedTexture(const std::string& name, std::vector<std::shared_ptr<Texture>> texs)
        : TextureObject(name, texs.empty() ? 0 : texs[0]->width, texs.empty() ? 0 : texs[0]->height),
          textures(std::move(texs)) {
        for (auto& tex : textures) {
            texture_lazy::set_wrap(tex.get(), ray::TEXTURE_WRAP_CLAMP);
        }
    }

    int frame_count() const override { return static_cast<int>(textures.size()); }

    const Texture* frame_texture_raw(int frame) const override {
        if (frame < 0 || frame >= static_cast<int>(textures.size())) {
            throw std::runtime_error("Frame " + std::to_string(frame) + " not available in framed texture " + name);
        }
        return textures[frame].get();
    }
};

struct DrawLogEntry {
    std::string name;
    Rect rect;
    TextureObject* tex_obj = nullptr;
    int index = 0;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float scale = 1.0f;
    bool center = false;
    Vec2 origin = {0, 0};
    float rotation = 0.0f;
    bool from_lua = false;
    std::string lua_source;
    int lua_line = 0;
    std::string lua_function;
    int lua_defined_line = 0;
};

// Z-DON: hash for string-keyed maps that are also searched with string_views / literals (no temporary
// std::string). Same hash values as std::hash<std::string>, so the maps iterate in the same order.
struct TexNameHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

inline bool debug_log_draws = false;
inline std::vector<DrawLogEntry> debug_draw_log;
inline std::vector<DrawLogEntry> debug_draw_log_prev;

class TextureWrapper {
private:
    std::unordered_map<int, std::unique_ptr<BaseAnimation>> animations;
    std::vector<std::unique_ptr<BaseAnimation>> copied_animations;
    std::unordered_map<std::string, std::unordered_map<int, std::unique_ptr<BaseAnimation>>> screen_animations;
    fs::path graphics_path;
    fs::path parent_graphics_path;
    std::vector<fs::path> ancestor_graphics_paths;
    std::vector<float> ancestor_scales;
    std::unordered_set<std::string> loaded_subsets;
    std::unordered_map<std::string, int> tex_id_refcount;
    std::unordered_map<std::string, std::unordered_set<std::string>> subset_loaded_ids;
    // set while load_screen_textures runs: the screen's PNGs, decoded ahead of load_folder
    class ImagePrefetch* image_prefetch = nullptr;
    // set while load_screen_textures runs: the skin folders' listings, read once for the screen
    class DirIndex* dir_index = nullptr;
    // Z-DON: the screen load_screen_textures loaded last (texture_ondemand.h); empty after unload_textures
    std::string screen_loaded;
    // Z-DON: texture objects of the folders loaded ahead for on-demand use (load_screen_textures), and
    // where load_folder puts them while it makes them; dropped with unload_textures
    std::vector<std::shared_ptr<TextureObject>> ondemand_stash;
    std::vector<std::shared_ptr<TextureObject>>* stash = nullptr;
    // Z-DON: the images texture_lazy::evict_all freed for the textures this screen now holds
    void refill_evicted(class ImagePrefetch& prefetch, const std::vector<fs::path>& planned);

    bool read_screen_animations(const std::string& screen_name, rapidjson::Document& out);

public:
    std::unordered_map<std::string, std::shared_ptr<TextureObject>, TexNameHash, std::equal_to<>> textures;
    // Z-DON: get_texture / has_texture results for names that are not in `textures` themselves (a language
    // fallback or a miss), so a miss no longer builds the variant names and logs a warning on every call.
    // Emptied with lua_texture_lookup, and when the language changes.
    struct TextureFallback {
        std::shared_ptr<TextureObject> found; // nullptr: no variant exists
        bool warned = false;
    };
    std::unordered_map<std::string, TextureFallback, TexNameHash, std::equal_to<>> texture_fallbacks;
    std::string texture_fallbacks_lang;
    // Lua tex.get_texture(path) results by the exact path string (nullptr = not found): skins call
    // it every frame. Emptied whenever `textures` changes, and when the language changes.
    std::unordered_map<std::string, std::shared_ptr<TextureObject>> lua_texture_lookup;
    std::string lua_texture_lookup_lang;
    // indexed by SC (read every frame, per note): set_skin_config / get_skin_config
    std::vector<std::optional<SkinInfo>> skin_info;
    std::unordered_map<std::string, SkinInfo> skin_config_by_name;
    std::unordered_map<SCO, bool> options;
    Chara3DConfig chara_3d_config;
    std::map<std::string, std::pair<float, float>, std::less<>> chara_positions;
    std::map<std::string, std::pair<float, float>, std::less<>> nameplate_positions;

    std::pair<float, float> chara_xy(std::string_view name, std::string_view fallback = "") const {
        return lookup_xy(chara_positions, name, fallback);
    }

    std::pair<float, float> nameplate_xy(std::string_view name, std::string_view fallback = "") const {
        return lookup_xy(nameplate_positions, name, fallback);
    }
    static std::pair<float, float> lookup_xy(const std::map<std::string, std::pair<float, float>, std::less<>>& m,
                                             std::string_view name, std::string_view fallback) {
        auto it = m.find(name);
        if (it == m.end()) {
            it = m.find(fallback);
        }
        return it == m.end() ? std::pair<float, float>{0.0f, 0.0f} : it->second;
    }
    fs::path font_path;
    int screen_width{1280};
    int screen_height{720};
    float screen_scale{1.0};
    // Z-DON: skin_config screen.texture_density (default 1). With d > 1 this skin's own PNGs and their
    // texture.json numbers are in physical pixels (d per logical unit); the frame renders at d x the screen
    // size (libs/density.h). skin_config and animation.json stay logical.
    int texture_density{1};
    float draw_offset_x = 0.0f;
    float draw_offset_y = 0.0f;

    TextureWrapper() = default;

    void init(const fs::path& skin_path);

    const SkinInfo* get_skin_config(SC key) const {
        const size_t k = static_cast<size_t>(key);
        return k < skin_info.size() && skin_info[k] ? &*skin_info[k] : nullptr;
    }
    // overwrite = false keeps a value already set (std::map::try_emplace)
    void set_skin_config(SC key, SkinInfo info, bool overwrite = true) {
        const size_t k = static_cast<size_t>(key);
        if (k >= skin_info.size()) {
            skin_info.resize(k + 1);
        }
        if (overwrite || !skin_info[k]) {
            skin_info[k] = std::move(info);
        }
    }

    // text_config label for `key` in exactly `language` (see font_manager.get_text_label)
    std::string skin_text(const std::string& key, const std::string& language, const std::string& fallback = "") const;

    fs::path skin_root() const { return graphics_path.parent_path(); }
    // Graphics dirs of the skin and all its ancestors, nearest first
    std::vector<fs::path> all_graphics_roots() const {
        std::vector<fs::path> r{graphics_path};
        r.insert(r.end(), ancestor_graphics_paths.begin(), ancestor_graphics_paths.end());
        return r;
    }

    fs::path resolve_skin_path(const fs::path& relative_path) const {
        if (relative_path.is_absolute()) {
            return {};
        }
        for (const auto& part : relative_path) {
            if (part == "..") {
                return {};
            }
        }
        fs::path child = skin_root() / relative_path;
        if (fs::exists(child)) {
            return child;
        }
        for (const fs::path& ancestor : ancestor_graphics_paths) {
            fs::path p = ancestor.parent_path() / relative_path;
            if (fs::exists(p)) {
                return p;
            }
        }
        return child;
    }

    void unload_textures();

    bool has_animation(const int id) const { return animations.contains(id); }
    BaseAnimation* get_animation(const int id, bool is_copy = false);
    BaseAnimation* get_animation(const int id, const std::string& screen_name);

    // density > 1: a texture of this skin with density (texture_density); default_size_scale (< 0: scale) scales the
    // default x2 / y2 (the texture's own size)
    void read_tex_obj_data(const Value& tex_mapping, TextureObject* tex_obj, float scale = 1.0f, int density = 1,
                           float default_size_scale = -1.0f);

    void load_animations(const std::string& screen_name);

    void load_folder(const std::string& screen_name, const std::string& subset);

    void unload_folder(const std::string& screen_name, const std::string& subset);

    void load_screen_textures(const std::string& screen_name);

    // Z-DON predecode: the textures of the screen likely to come next, decoded from its texture pack on background
    // jobs (ZDON_PREDECODE=<jobs>, default 4, 0 = off; ZDON_PREDECODE_MB caps the images, default 512) while the
    // current screen idles. Its load takes the decoded images where it would decode the file (the file's size and
    // time checked against the load's own listing), uploads stay on the main thread; a load of any other screen
    // but those in `via` drops them. end_predecode frees what the screen did not need once it has started.
    void start_predecode(const std::string& target, std::vector<std::string> via = {});
    void cancel_predecode(const std::string& target);
    void end_predecode();
    // once per frame after its present: for the game screen, uploads predecoded images ahead (ZDON_PREUPLOAD_MS of
    // the frame, default 1.0, 0 = off), a band of rows at a time like the warm-up (texture_lazy.h); its load
    // takes those textures instead of uploading the same pixels again
    void predecode_tick();

    const std::string& loaded_screen() const { return screen_loaded; }
    bool has_folder(const std::string& screen_name, const std::string& subset) const {
        return loaded_subsets.contains(screen_name + "/" + fs::path(subset).filename().string());
    }

    TextureObject* get_texture(std::string_view name);

    std::shared_ptr<TextureObject> get_texture_shared(std::string_view name);
    std::vector<std::string> language_variants(std::string_view name) const;

    bool has_texture(std::string_view name);
    TextureFallback& texture_fallback(std::string_view name);

    void draw_texture(TextureObject* tex_obj, const DrawTextureParams& = {});
};

extern TextureWrapper tex;

extern TextureWrapper global_tex;
