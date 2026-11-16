#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../gfx.h"
#include "../jobs.h"
#include "color_utils.h"
#include "texture.h"

// One rasterized glyph, immutable once cached. Shared by its size's cache, that size's GPU atlas and
// every OutlinedText drawing with it, so evicting a size never frees a glyph a text still reads.
struct CachedGlyph;
using GlyphRef = std::shared_ptr<const CachedGlyph>;

// Storage behind the Font FontManager::copy_font returns: the glyph/rec arrays are owned here,
// the glyph images alias `refs`.
struct GlyphSubset {
    std::vector<GlyphInfo> glyphs;
    std::vector<Rect> recs;
    std::vector<GlyphRef> refs;
};

class FontManager {
private:
    fs::path font_path;
    uint64_t generation;

    struct SizedFont {
        std::unordered_set<int> codepoints;    // only what was asked for AT THIS SIZE
        std::vector<GlyphRef> cache;           // rasterized once per codepoint; append-only
        std::unordered_map<int, size_t> index; // codepoint -> slot in `cache`
        uint64_t last_used = 0;

        // GPU atlas for DrawTextEx (get_font only): built on first use, then extended in place
        Font font{};                         // aliases the three below
        std::vector<GlyphInfo> atlas_glyphs; // cache[0 .. atlas_glyphs.size()) are packed
        std::vector<Rect> atlas_recs;
        Texture atlas_texture{};
        Image atlas_image{}; // CPU copy (GRAY_ALPHA), kept to grow the texture
        int pen_x = 0, pen_y = 0, row_h = 0;
    };

    std::vector<unsigned char> font_data; // the .ttf bytes, read once
    // Z-DON: the base font's bytes when font_data is a language overlay (screen.font_base): glyphs the
    // overlay lacks come from here (same metrics: the skin builds both from one family)
    std::vector<unsigned char> fallback_data;

    bool rasterize_new(SizedFont& entry, int font_size, const std::vector<int>& cps);   // font_mutex held
    void update_atlas(SizedFont& entry, int font_size);                                 // font_mutex held
    void reset_atlas(SizedFont& entry, int width, int height);                          // font_mutex held
    void release_atlas(SizedFont& entry);                                               // font_mutex held
    void release_all_fonts();                                                           // font_mutex held
    bool register_codepoints(SizedFont& entry, int font_size, const std::string& text); // font_mutex held

    std::unordered_map<int, SizedFont> fonts;
    uint64_t use_clock = 0;

    Texture sentinel_texture{};

    mutable std::mutex font_mutex;

    static constexpr size_t MAX_SIZED_FONTS = 32;

    SizedFont& acquire(const std::string& text, int font_size); // font_mutex held
    void evict_lru(int keep_size);                              // font_mutex held

public:
    FontManager();
    void init(const fs::path& font_path, const fs::path& fallback_path = {});
    void unload();
    // For DrawTextEx: aliases the manager's atlas, valid until the next get_font()/copy_font() call.
    Font get_font(const std::string& text, int font_size);
    // For measuring and CPU compositing (the texture is a 1x1 placeholder): only the glyphs `text`
    // uses, backed by `storage` and valid as long as it is.
    Font copy_font(const std::string& text, int font_size, GlyphSubset& storage);
    void register_text(const std::string& text, int font_size);
    uint64_t cache_generation() const;

    void load_text_config(const fs::path& skin_root);

    std::string get_text_label(const std::string& key);

    const SkinInfo* get_text_info(const std::string& key);
    SkinInfo* get_text_info_mut(const std::string& key);
};

enum class LocalizedTextFallback {
    NONE,
    ENGLISH,
    ENGLISH_OR_FIRST,
    REQUIRED_ENGLISH,
};

inline std::string localized_text(const std::map<std::string, std::string>& values, const std::string& language,
                                  LocalizedTextFallback fallback) {
    if (auto it = values.find(language); it != values.end()) {
        return it->second;
    }

    if (fallback == LocalizedTextFallback::NONE) {
        return "";
    }
    if (fallback == LocalizedTextFallback::REQUIRED_ENGLISH) {
        return values.at("en");
    }

    if (auto it = values.find("en"); it != values.end()) {
        return it->second;
    }

    if (fallback == LocalizedTextFallback::ENGLISH_OR_FIRST && !values.empty()) {
        return values.begin()->second;
    }

    return "";
}

struct CachedOutlinedText;

// Fill color for the bytes [begin, end) of a text with its markup removed.
struct TextColorSpan {
    size_t begin;
    size_t end;
    Color color;
};

// OutlinedText reads <c=#RRGGBB>...</c> in its text as a fill color for that part (box.def titles use this).
// `text` without those tags, for code that compares or counts what is shown.
std::string strip_text_markup(const std::string& text);

class OutlinedText {
private:
    std::string text;
    int density = 1; // Z-DON texture density: font_size / outline_thickness are physical (x density)
    std::vector<TextColorSpan> color_spans;
    float font_size;
    float outline_thickness;
    float v_advance = 1.0f;
    std::string dbg_font; // dev draw log: "<font file or default>@<logical size>"

    Font worker_font;
    GlyphSubset worker_glyphs; // backs worker_font

    std::optional<Image> pending_image;
    mutable std::mutex pending_mutex;

    std::optional<Texture> texture;
    std::shared_ptr<CachedOutlinedText> cached_texture;
    std::string cache_key;

    jobs::Handle build_future; // Z-DON: a job on the shared pool (was a std::async thread per text)

    struct BuildData {
        Image img;
    };

    Color fill_at(size_t offset, Color base) const;

    BuildData build_horizontal_text(Color color, Color outline_color, float spacing);
    BuildData build_vertical_text(Color color, Color outline_color, float spacing);

public:
    float width = 0.0f;
    float height = 0.0f;
    float x_offset = 0.0f;
    float y_offset = 0.0f;
    int top_extra = 0;
    // Skin config entry this text takes its x/y offset from (empty for code-placed text).
    std::string position_key;

    OutlinedText(std::string text, int font_size, Color color, Color outline_color, bool is_vertical,
                 float outline_thickness = 5.0f, float spacing = 2.0f, float v_advance = 1.0f,
                 const std::string& font_name = "");

    ~OutlinedText();

    bool upload_pending();

    bool is_ready() const { return texture.has_value(); }

    const std::string& get_text() const { return text; }

    void finish();

    void draw(const DrawTextureParams& = {});
};

extern FontManager font_manager;

// Skin font by name ("Fonts/<name>[.ttf]"); empty or unloadable -> the default font_manager.
FontManager& named_font_manager(const std::string& name);
void unload_named_fonts();

// Writes the entry's current x/y back into the skin json it was loaded from.
bool save_text_position(const std::string& key);
