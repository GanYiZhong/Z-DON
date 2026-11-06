#include "text.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>
#include <vector>

#include "../../platform/platform.h"
#include "../asset_cache.h"
#include "../file/filesystem.h"
#include "../file/script.h"
#include "../fixed_step.h"
#include "../global_data.h"
#include "../raylib_ns.h" // Z-DON: the incremental glyph atlas and image compositing call raylib directly
#include "../text_stats.h"
#include "han_fold_table.h"

#if defined(_WIN32)
#include "../../platform/platform_windows.h"
#elif defined(YATAIDON_HAVE_ICONV)
#include <iconv.h>
#endif

// load_font_data's glyph images are the caller's
static void unload_glyphs(std::vector<GlyphInfo>& glyphs) {
    for (GlyphInfo& g : glyphs) {
        unload_image(g.image);
    }
    glyphs.clear();
}

static void image_draw_image_pro(Image* dst, const Image& src, Rect src_rec, Rect dst_rec, Vec2 origin, float rotation,
                                 Color tint) {
    ray::ImageDrawImagePro(reinterpret_cast<ray::Image*>(dst), std::bit_cast<ray::Image>(src), src_rec, dst_rec, origin,
                           rotation, tint);
}

// merge 0ed42ec (text.cpp): Z-DON's font cache (release_all_fonts, sentinel texture, per-text glyph refs) is the
// base; upstream's generation counter and outlined-text texture cache are ported onto it, upstream's
// atlas-free deep_copy_font is not (Z-DON's copy_font already hands out only the text's glyphs)
static std::atomic<uint64_t> next_font_generation{1};

struct CachedOutlinedText {
    Texture texture;
    float width, height, x_offset, y_offset;
    int top_extra;
    ~CachedOutlinedText() { unload_texture(texture); }
};

static AssetCache<CachedOutlinedText>& outlined_text_cache() {
    static AssetCache<CachedOutlinedText> cache(asset_cache_budget(48u << 20));
    return cache;
}

FontManager::FontManager() : generation(next_font_generation.fetch_add(1, std::memory_order_relaxed)) {}

uint64_t FontManager::cache_generation() const {
    std::scoped_lock lock(font_mutex);
    return generation;
}

void FontManager::init(const fs::path& font_path, const fs::path& fallback_path) {
    std::scoped_lock lock(font_mutex);
    generation = next_font_generation.fetch_add(1, std::memory_order_relaxed);
    generation = next_font_generation.fetch_add(1, std::memory_order_relaxed);
    if (!exists(font_path)) {
        throw std::runtime_error("Failed to load font: " + font_path.string());
    }
    this->font_path = font_path;
    release_all_fonts();
    auto read_font = [](const fs::path& p, std::vector<unsigned char>& out) {
        out = load_file_bytes(p.string().c_str());
        if (out.empty()) {
            throw std::runtime_error("Failed to read font data: " + p.string());
        }
    };
    read_font(font_path, font_data);
    fallback_data.clear();
    if (!fallback_path.empty() && exists(fallback_path)) {
        read_font(fallback_path, fallback_data);
    }

    if (sentinel_texture.id == 0) {
        Image px = gen_image_color(1, 1, Color::BLANK);
        sentinel_texture = load_texture_from_image(px);
        unload_image(px);
    }
}

static std::unordered_map<std::string, SkinInfo> text_entries;

static double json_member(const rapidjson::Value& o, const char* key, double fallback) {
    return o.HasMember(key) && o[key].IsNumber() ? o[key].GetDouble() : fallback;
}

static void read_text_entries(const fs::path& file, float scale) {
    if (!fs::exists(file)) {
        return;
    }
    try {
        auto doc = read_json_file(file);
        if (!doc.IsObject()) {
            return;
        }
        for (auto& m : doc.GetObject()) {
            const auto& v = m.value;
            if (!v.IsObject() || !v.HasMember("text") || !v["text"].IsObject()) {
                continue;
            }
            std::map<std::string, std::string> labels;
            for (auto& t : v["text"].GetObject()) {
                if (t.value.IsString()) {
                    labels[t.name.GetString()] = t.value.GetString();
                }
            }
            text_entries[m.name.GetString()] = SkinInfo(static_cast<float>(json_member(v, "x", 0.0)) * scale,
                                                        static_cast<float>(json_member(v, "y", 0.0)) * scale,
                                                        static_cast<int>(json_member(v, "font_size", 0) * scale),
                                                        static_cast<float>(json_member(v, "width", 0.0)) * scale,
                                                        static_cast<float>(json_member(v, "height", 0.0)) * scale,
                                                        labels, static_cast<float>(json_member(v, "outline", -1.0)));
        }
    } catch (const std::exception& e) {
        spdlog::warn("text_config {}: {}", file.string(), e.what());
    }
}

void FontManager::load_text_config(const fs::path& skin_root) {
    text_entries.clear();
    std::vector<fs::path> roots = skin_ancestor_roots(); // nearest first
    roots.insert(roots.begin(), skin_root);
    for (size_t i = roots.size(); i-- > 0;) { // farthest first so nearer skins override
        float scale = 1.0f;
        if (i > 0) {
            float aw = 1280.0f;
            try {
                auto cfg = read_json_file(roots[i] / "Graphics" / "skin_config.json");
                if (cfg.HasMember("screen") && cfg["screen"].HasMember("width") && cfg["screen"]["width"].IsNumber()) {
                    aw = cfg["screen"]["width"].GetFloat();
                }
            } catch (const std::exception&) {
            }
            scale = static_cast<float>(tex.screen_width) / aw;
        }
        read_text_entries(roots[i] / "Graphics" / "skin_config.json", scale);
        read_text_entries(roots[i] / "text_config.json", scale);
    }
}

const SkinInfo* FontManager::get_text_info(const std::string& key) {
    auto it = text_entries.find(key);
    return it == text_entries.end() ? nullptr : &it->second;
}

std::string FontManager::get_text_label(const std::string& key) {
    auto it = text_entries.find(key);
    if (it == text_entries.end()) {
        spdlog::warn("text_config: missing key '{}'", key);
        return "";
    }
    return localized_text(it->second.text, global_data.config->general.language,
                          LocalizedTextFallback::ENGLISH_OR_FIRST);
}

void FontManager::unload() {
    std::scoped_lock lock(font_mutex);
    generation = next_font_generation.fetch_add(1, std::memory_order_relaxed);
    release_all_fonts();
    if (sentinel_texture.id != 0) {
        unload_texture(sentinel_texture);
        sentinel_texture = {};
    }
}

struct CachedGlyph {
    // value/offsets/advance as LoadFontData gave them; the image is GRAY_ALPHA (255, coverage), byte for
    // byte what LoadFontEx-style atlas crops hold (Image{} when the bitmap is empty)
    GlyphInfo info{};
    float rec_width = 0.0f; // the rasterized bitmap's size (Font::recs width/height)
    float rec_height = 0.0f;

    CachedGlyph() = default;
    CachedGlyph(const CachedGlyph&) = delete;
    CachedGlyph& operator=(const CachedGlyph&) = delete;
    ~CachedGlyph() {
        if (info.image.data) {
            unload_image(info.image);
        }
    }
};

static constexpr int GLYPH_PADDING = 4; // FONT_TTF_DEFAULT_CHARS_PADDING, same as LoadFontEx

// `raw` is a LoadFontData glyph (GRAYSCALE bitmap); it is left untouched.
static GlyphRef make_glyph(const GlyphInfo& raw, int value) {
    auto g = std::make_shared<CachedGlyph>();
    g->info = raw;
    g->info.value = value;
    g->info.image = {};
    const int w = raw.image.width, h = raw.image.height;
    g->rec_width = static_cast<float>(w);
    g->rec_height = static_cast<float>(h);
    if (w > 0 && h > 0) {
        auto* px = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(w) * h * 2));
        const auto* src = static_cast<const unsigned char*>(raw.image.data);
        for (int i = 0; i < w * h; i++) {
            px[i * 2] = 255;
            px[i * 2 + 1] = src ? src[i] : 0;
        }
        g->info.image = Image{px, w, h, 1, pixel_format::GRAY_ALPHA};
    }
    return g;
}

static void add_glyph(std::vector<GlyphRef>& cache, std::unordered_map<int, size_t>& index, GlyphRef g) {
    index.emplace(g->info.value, cache.size()); // first one wins, like GetGlyphIndex
    cache.push_back(std::move(g));
}

void FontManager::evict_lru(int keep_size) {
    while (fonts.size() > MAX_SIZED_FONTS) {
        auto victim = fonts.end();
        for (auto it = fonts.begin(); it != fonts.end(); ++it) {
            if (it->first == keep_size) {
                continue;
            }
            if (victim == fonts.end() || it->second.last_used < victim->second.last_used) {
                victim = it;
            }
        }
        if (victim == fonts.end()) {
            break;
        }
        spdlog::debug("font: evicting {}px ({} codepoints), {} resident", victim->first,
                      victim->second.codepoints.size(), fonts.size());
        release_atlas(victim->second);
        fonts.erase(victim); // glyphs an OutlinedText still holds stay alive through its GlyphRefs
    }
}

// Rasterize only the codepoints not yet in the cache (stb_truetype via LoadFontData).
bool FontManager::rasterize_new(SizedFont& entry, int font_size, const std::vector<int>& cps) {
    if (cps.empty()) {
        return true;
    }
    std::vector<GlyphInfo> g = load_font_data(font_data, font_size, cps);
    std::unordered_set<int> got;
    for (const GlyphInfo& gi : g) {
        add_glyph(entry.cache, entry.index, make_glyph(gi, gi.value));
        got.insert(gi.value);
    }
    unload_glyphs(g); // the cache holds converted copies

    // Z-DON: an overlay font (screen.font_base) lacks most glyphs: the base font has them
    if (!fallback_data.empty()) {
        std::vector<int> rest;
        for (int cp : cps) {
            if (!got.contains(cp)) {
                rest.push_back(cp);
            }
        }
        if (!rest.empty()) {
            std::vector<GlyphInfo> fg = load_font_data(fallback_data, font_size, rest);
            for (const GlyphInfo& gi : fg) {
                add_glyph(entry.cache, entry.index, make_glyph(gi, gi.value));
                got.insert(gi.value);
            }
            unload_glyphs(fg);
        }
    }

    std::vector<int> missing, alts;
    for (int cp : cps) {
        if (got.contains(cp)) {
            continue;
        }
        int alt = static_cast<int>(han_fold_codepoint(static_cast<uint32_t>(cp)));
        if (alt != cp) {
            missing.push_back(cp);
            alts.push_back(alt);
        }
    }
    if (missing.empty()) {
        return true;
    }
    std::vector<GlyphInfo> ag = load_font_data(font_data, font_size, alts);
    std::vector<bool> assigned(alts.size(), false);
    for (auto& i : ag) {
        for (size_t k = 0; k < alts.size(); k++) {
            if (assigned[k] || alts[k] != i.value) {
                continue;
            }
            add_glyph(entry.cache, entry.index, make_glyph(i, missing[k])); // drawn as the original
            assigned[k] = true;
        }
    }
    unload_glyphs(ag);
    // Any alt LoadFontData silently dropped never got assigned; retry those too.
    for (size_t k = 0; k < alts.size(); k++) {
        if (!assigned[k]) {
            entry.codepoints.erase(missing[k]);
        }
    }
    return true;
}

void FontManager::reset_atlas(SizedFont& entry, int width, int height) {
    if (entry.atlas_image.data) {
        unload_image(entry.atlas_image);
    }
    // blank texels are (255, 0) like GenImageFontAtlas's, so bilinear filtering at glyph edges blends
    // toward the same color
    auto* px = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(width) * height * 2));
    for (size_t i = 0; i < static_cast<size_t>(width) * height; i++) {
        px[i * 2] = 255;
        px[i * 2 + 1] = 0;
    }
    entry.atlas_image = Image{px, width, height, 1, pixel_format::GRAY_ALPHA};
    entry.atlas_glyphs.clear();
    entry.atlas_recs.clear();
    entry.pen_x = entry.pen_y = entry.row_h = 0;
}

// Shelf-pack the cached glyphs the atlas doesn't hold yet and upload only the rows they touched; the
// texture is recreated only when the atlas had to grow. Each glyph sits in a cell with GLYPH_PADDING
// blank texels on every side, which DrawTextCodepoint samples along with it.
void FontManager::update_atlas(SizedFont& entry, int font_size) {
    const size_t n = entry.cache.size();
    if (n == 0 || (entry.atlas_texture.id != 0 && entry.atlas_glyphs.size() == n)) {
        return;
    }
    text_stats::Scope stat(text_stats::ATLAS);
    constexpr int MAX_DIM = 8192;
    constexpr int pad = GLYPH_PADDING;
    bool recreate = entry.atlas_texture.id == 0;
    if (!entry.atlas_image.data) {
        int w = 256;
        while (w < 16 * (font_size + 2 * pad) && w < 4096) {
            w *= 2;
        }
        reset_atlas(entry, w, std::max(64, w / 4));
        recreate = true;
    }
    int dirty_y0 = entry.pen_y;
    while (entry.atlas_glyphs.size() < n) {
        const CachedGlyph& g = *entry.cache[entry.atlas_glyphs.size()];
        const int gw = static_cast<int>(g.rec_width), gh = static_cast<int>(g.rec_height);
        const int cell_w = gw + 2 * pad, cell_h = std::max(gh, font_size) + 2 * pad;
        const int W = entry.atlas_image.width, H = entry.atlas_image.height;
        if (entry.pen_x + cell_w > W && entry.pen_x > 0) {
            entry.pen_x = 0;
            entry.pen_y += entry.row_h;
            entry.row_h = 0;
        }
        if (cell_w > W || entry.pen_y + cell_h > H) {
            if (cell_w <= W && H * 2 <= MAX_DIM) { // taller: the packed rows stay where they are
                Image old = entry.atlas_image;
                entry.atlas_image = {};
                std::vector<GlyphInfo> glyphs = std::move(entry.atlas_glyphs);
                std::vector<Rect> recs = std::move(entry.atlas_recs);
                const int pen_x = entry.pen_x, pen_y = entry.pen_y, row_h = entry.row_h;
                reset_atlas(entry, W, H * 2);
                memcpy(entry.atlas_image.data, old.data, static_cast<size_t>(W) * H * 2);
                unload_image(old);
                entry.atlas_glyphs = std::move(glyphs);
                entry.atlas_recs = std::move(recs);
                entry.pen_x = pen_x;
                entry.pen_y = pen_y;
                entry.row_h = row_h;
            } else if (W * 2 <= MAX_DIM) { // wider: repack everything
                reset_atlas(entry, W * 2, H);
            } else {
                spdlog::warn("font: {}px atlas full at {}x{}, {} glyphs left out", font_size, W, H,
                             n - entry.atlas_glyphs.size());
                break;
            }
            recreate = true;
            continue;
        }
        const int x = entry.pen_x + pad, y = entry.pen_y + pad;
        if (g.info.image.data) {
            auto* dst = static_cast<unsigned char*>(entry.atlas_image.data);
            const auto* src = static_cast<const unsigned char*>(g.info.image.data);
            for (int row = 0; row < gh; row++) {
                memcpy(dst + (static_cast<size_t>(y + row) * W + x) * 2, src + static_cast<size_t>(row) * gw * 2,
                       static_cast<size_t>(gw) * 2);
            }
        }
        GlyphInfo info = g.info; // the image aliases the cached glyph (cache[i] keeps it alive)
        entry.atlas_glyphs.push_back(info);
        entry.atlas_recs.emplace_back(static_cast<float>(x), static_cast<float>(y), g.rec_width, g.rec_height);
        entry.pen_x += cell_w;
        entry.row_h = std::max(entry.row_h, cell_h);
    }

    const int W = entry.atlas_image.width, H = entry.atlas_image.height;
    if (recreate) {
        if (entry.atlas_texture.id != 0) {
            unload_texture(entry.atlas_texture);
        }
        entry.atlas_texture = load_texture_from_image(entry.atlas_image);
        set_texture_filter(entry.atlas_texture, texture_filter::BILINEAR);
    } else {
        const int y1 = std::min(H, entry.pen_y + entry.row_h);
        if (y1 > dirty_y0) {
            ray::UpdateTextureRec(entry.atlas_texture,
                                  ray::Rectangle{0, static_cast<float>(dirty_y0), static_cast<float>(W),
                                                 static_cast<float>(y1 - dirty_y0)},
                                  static_cast<const unsigned char*>(entry.atlas_image.data) +
                                      static_cast<size_t>(dirty_y0) * W * 2);
        }
    }

    Font f{};
    f.baseSize = font_size;
    f.glyphCount = static_cast<int>(entry.atlas_glyphs.size());
    f.glyphPadding = pad;
    f.texture = entry.atlas_texture;
    f.recs = entry.atlas_recs.data();
    f.glyphs = entry.atlas_glyphs.data();
    entry.font = f;
}

void FontManager::release_atlas(SizedFont& entry) {
    if (entry.atlas_texture.id != 0) {
        unload_texture(entry.atlas_texture);
    }
    if (entry.atlas_image.data) {
        unload_image(entry.atlas_image);
    }
    entry.atlas_texture = {};
    entry.atlas_image = {};
    entry.atlas_glyphs.clear();
    entry.atlas_recs.clear();
    entry.font = {};
    entry.pen_x = entry.pen_y = entry.row_h = 0;
}

void FontManager::release_all_fonts() {
    for (auto& [size, entry] : fonts) {
        release_atlas(entry);
    }
    fonts.clear();
}

bool FontManager::register_codepoints(SizedFont& entry, int font_size, const std::string& text) {
    std::vector<int> fresh;
    if (entry.codepoints.empty()) {
        // "A" is what OutlinedText measures line height with; keep it (and '?') always present.
        for (int cp : {'A', '?', ' '}) {
            entry.codepoints.insert(cp);
            fresh.push_back(cp);
        }
    }
    const char* ptr = text.c_str();
    while (*ptr) {
        int cp_size = 0;
        int codepoint = get_codepoint_next(ptr, &cp_size);
        if (cp_size <= 0) {
            break;
        }
        if (codepoint > 0 && entry.codepoints.insert(codepoint).second) {
            fresh.push_back(codepoint);
        }
        ptr += cp_size;
    }
    if (fresh.empty()) {
        return false;
    }
    if (!rasterize_new(entry, font_size, fresh)) {
        for (int cp : fresh) {
            entry.codepoints.erase(cp); // allow a later retry
        }
        spdlog::warn("font: failed to rasterize {} codepoints at {}px", fresh.size(), font_size);
        return false;
    }
    return true;
}

void FontManager::register_text(const std::string& text, int font_size) {
    text_stats::Scope stat(text_stats::REGISTER);
    std::scoped_lock lock(font_mutex);
    font_size = std::max(font_size, 1);
    register_codepoints(fonts[font_size], font_size, text);
}

FontManager::SizedFont& FontManager::acquire(const std::string& text, int font_size) {
    SizedFont& entry = fonts[font_size];
    register_codepoints(entry, font_size, text);
    entry.last_used = ++use_clock;
    evict_lru(font_size); // never evicts `font_size` itself, so `entry` stays valid
    return entry;
}

Font FontManager::get_font(const std::string& text, int font_size) {
    std::scoped_lock lock(font_mutex);
    font_size = std::max(font_size, 1);
    SizedFont& entry = acquire(text, font_size);
    update_atlas(entry, font_size);
    return entry.font;
}

// The glyphs `text` uses, in an order that makes GetGlyphIndex pick the same glyph it would in the
// size's full glyph set: slot 0 first (the fallback when there is no '?'), then '?', then the rest.
// Glyph values in the cache are unique, so a codepoint maps to the same glyph either way.
Font FontManager::copy_font(const std::string& text, int font_size, GlyphSubset& storage) {
    std::scoped_lock lock(font_mutex);
    font_size = std::max(font_size, 1);
    SizedFont& entry = acquire(text, font_size);
    storage.glyphs.clear();
    storage.recs.clear();
    storage.refs.clear();
    if (!entry.cache.empty()) {
        std::vector<size_t> slots;
        std::unordered_set<size_t> seen;
        auto add_slot = [&](size_t slot) {
            if (seen.insert(slot).second) {
                slots.push_back(slot);
            }
        };
        auto add_cp = [&](int cp) {
            auto it = entry.index.find(cp);
            if (it != entry.index.end()) {
                add_slot(it->second);
            }
        };
        add_slot(0);
        for (int cp : {'?', 'A', ' '}) {
            add_cp(cp);
        }
        const char* ptr = text.c_str();
        while (*ptr) {
            int cp_size = 0;
            int codepoint = get_codepoint_next(ptr, &cp_size);
            if (cp_size <= 0) {
                break;
            }
            add_cp(codepoint);
            ptr += cp_size;
        }
        storage.glyphs.reserve(slots.size());
        storage.recs.reserve(slots.size());
        storage.refs.reserve(slots.size());
        for (size_t slot : slots) {
            const GlyphRef& g = entry.cache[slot];
            storage.glyphs.push_back(g->info);
            storage.recs.emplace_back(0, 0, g->rec_width, g->rec_height);
            storage.refs.push_back(g);
        }
    }
    Font f{};
    if (!storage.glyphs.empty()) {
        f.baseSize = font_size;
        f.glyphCount = static_cast<int>(storage.glyphs.size());
        f.glyphPadding = GLYPH_PADDING;
        f.glyphs = storage.glyphs.data();
        f.recs = storage.recs.data();
    }
    f.texture = sentinel_texture;
    return f;
}

static Color clear_of(Color c) {
    return Color{c.r, c.g, c.b, 0};
}

static void blit_over(Image* dst, const Image& src, int x0, int y0) {
    if (!dst->data || !src.data) {
        return;
    }
    if (dst->format != pixel_format::RGBA8 || src.format != pixel_format::RGBA8) {
        Rect sr = {0, 0, static_cast<float>(src.width), static_cast<float>(src.height)};
        Rect dr = {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(src.width),
                   static_cast<float>(src.height)};
        image_draw_image_pro(dst, src, sr, dr, {0, 0}, 0.0f, Color::WHITE);
        return;
    }
    unsigned char* d = static_cast<unsigned char*>(dst->data);
    const unsigned char* sp = static_cast<const unsigned char*>(src.data);
    for (int y = 0; y < src.height; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= dst->height) {
            continue;
        }
        for (int x = 0; x < src.width; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= dst->width) {
                continue;
            }
            const unsigned char* s = sp + (y * src.width + x) * 4;
            unsigned char* o = d + (dy * dst->width + dx) * 4;
            const int sa = s[3];
            if (sa == 0) {
                continue;
            }
            if (sa == 255) {
                o[0] = s[0];
                o[1] = s[1];
                o[2] = s[2];
                o[3] = 255;
                continue;
            }
            const int da = o[3];
            const int oa = sa * 255 + da * (255 - sa); // out alpha * 255
            for (int c = 0; c < 3; c++) {
                int v = (s[c] * sa * 255 + o[c] * da * (255 - sa) + oa / 2) / (oa ? oa : 1);
                o[c] = static_cast<unsigned char>(v > 255 ? 255 : v);
            }
            o[3] = static_cast<unsigned char>((oa + 127) / 255);
        }
    }
}

static int text_margin(float font_size) {
    return static_cast<int>(std::ceil(font_size * 0.25f));
}

static Image text_image(const Font& font, const char* text, float font_size, float spacing, int margin) {
    Vec2 sz = measure_text_ex(font, text, font_size, spacing);
    Image im = gen_image_color(static_cast<int>(sz.x), static_cast<int>(sz.y) + margin * 2, Color::BLANK);
    int x = 0;
    for (const char* p = text; *p;) {
        int n = 0;
        int cp = get_codepoint_next(p, &n);
        if (n <= 0) {
            break;
        }
        p += n;
        int gi = get_glyph_index(font, cp);
        const GlyphInfo& g = font.glyphs[gi];
        if (cp != ' ' && cp != '\t') {
            Rect sr = {0, 0, static_cast<float>(g.image.width), static_cast<float>(g.image.height)};
            Rect dr = {static_cast<float>(x + g.offsetX), static_cast<float>(margin + g.offsetY), font.recs[gi].width,
                       font.recs[gi].height};
            image_draw_image_pro(&im, g.image, sr, dr, {0, 0}, 0.0f, Color::WHITE);
        }
        x += (g.advanceX == 0 ? static_cast<int>(font.recs[gi].width + spacing)
                              : g.advanceX + static_cast<int>(spacing));
    }
    return im;
}

static void draw_text_clean(Image* dst, const Font& font, const char* text, Vec2 pos, float font_size, float spacing,
                            Color tint) {
    const int m = text_margin(font_size);
    Image t = text_image(font, text, font_size, spacing, m);
    if (t.data && t.width > 0 && t.height > 0) {
        if (t.format != pixel_format::RGBA8) {
            image_format(&t, pixel_format::RGBA8);
        }
        unsigned char* px = static_cast<unsigned char*>(t.data);
        const int n = t.width * t.height;
        for (int i = 0; i < n; i++) {
            px[i * 4 + 0] = tint.r;
            px[i * 4 + 1] = tint.g;
            px[i * 4 + 2] = tint.b;
            px[i * 4 + 3] = static_cast<unsigned char>((px[i * 4 + 3] * static_cast<int>(tint.a)) / 255);
        }
        blit_over(dst, t, static_cast<int>(pos.x), static_cast<int>(pos.y) - m);
    }
    unload_image(t);
}

static void stamp_outline(Image* dst, const Font& font, const char* text, Vec2 pos, float font_size, float spacing,
                          Color outline_color, float radius) {
    const int m = text_margin(font_size);
    Image cov = text_image(font, text, font_size, spacing, m);
    if (!cov.data || cov.width <= 0 || cov.height <= 0) {
        unload_image(cov);
        return;
    }
    if (cov.format != pixel_format::RGBA8) {
        image_format(&cov, pixel_format::RGBA8);
    }
    const int r = static_cast<int>(std::ceil(radius + 0.5f));
    const int W = cov.width, H = cov.height;
    const int OW = W + 2 * r, OH = H + 2 * r;
    std::vector<unsigned char> a(W * H), out(OW * OH, 0);
    const unsigned char* cp = static_cast<const unsigned char*>(cov.data);
    for (int i = 0; i < W * H; i++) {
        a[i] = cp[i * 4 + 3];
    }
    // disk weights, 0..255, with the soft 1 px rim; rows carry their non-zero span
    const int D = 2 * r + 1;
    std::vector<unsigned char> wt(D * D, 0);
    std::vector<int> lo(D, D), hi(D, -1);
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            float d = std::sqrt(static_cast<float>(dx) * dx + static_cast<float>(dy) * dy);
            float w = radius + 0.5f - d;
            if (w <= 0.0f) {
                continue;
            }
            w = std::min(w, 1.0f);
            wt[(dy + r) * D + (dx + r)] = static_cast<unsigned char>(w * 255.0f + 0.5f);
            lo[dy + r] = std::min(lo[dy + r], dx + r);
            hi[dy + r] = std::max(hi[dy + r], dx + r);
        }
    }
    // H*W*D*D byte ops -- fine for HUD text
    for (int y = 0; y < H; y++) {
        const unsigned char* src = &a[y * W];
        for (int x = 0; x < W; x++) {
            const unsigned v = src[x];
            if (!v) {
                continue;
            }
            for (int dy = -r; dy <= r; dy++) {
                const int row = dy + r;
                if (hi[row] < 0) {
                    continue;
                }
                unsigned char* orow = &out[(y + dy + r) * OW + x];
                const unsigned char* wrow = &wt[row * D];
                for (int k = lo[row]; k <= hi[row]; k++) {
                    const unsigned char c = static_cast<unsigned char>((v * wrow[k] + 127) / 255);
                    unsigned char& o = orow[k]; // ox = x + (k - r) + r = x + k
                    o = std::max(o, c);
                }
            }
        }
    }
    Image o = gen_image_color(OW, OH, Color{outline_color.r, outline_color.g, outline_color.b, 0});
    unsigned char* op = static_cast<unsigned char*>(o.data);
    for (int i = 0; i < OW * OH; i++) {
        op[i * 4 + 3] = static_cast<unsigned char>((out[i] * static_cast<int>(outline_color.a)) / 255);
    }
    blit_over(dst, o, static_cast<int>(pos.x) - r, static_cast<int>(pos.y) - m - r);
    unload_image(o);
    unload_image(cov);
}

// Characters placed to the right of the preceding glyph at the same y,
// rather than stacked below it (terminal punctuation, etc.).
static bool is_beside_prev_char(const std::string& s) {
    static const std::unordered_set<std::string> beside_set = {
        ".",
        ",",
        "'",
        "\"",
        "\xe3\x80\x82", // 。
        "\xe3\x80\x81", // 、
    };
    return beside_set.contains(s);
}

// Extra downward draw offset for beside chars whose glyphs sit near the top
// of their cell (e.g. apostrophes), so they appear vertically centred.
static float beside_y_offset(const std::string& s, float char_height) {
    if (s == "'" || s == "\"") {
        return char_height * 0.7f;
    }
    return 0.0f;
}

static bool is_small_kana(const std::string& s) {
    static const std::unordered_set<std::string> sutegana = {
        // Small hiragana
        "ぁ",
        "ぃ",
        "ぅ",
        "ぇ",
        "ぉ",
        "っ",
        "ゃ",
        "ゅ",
        "ょ",
        "ゎ",
        "ゕ",
        "ゖ",
        // Small katakana
        "ァ",
        "ィ",
        "ゥ",
        "ェ",
        "ォ",
        "ッ",
        "ャ",
        "ュ",
        "ョ",
        "ヮ",
        "ヵ",
        "ヶ",
    };
    return sutegana.contains(s);
}

static bool needs_less_spacing_above(const std::string& s) {
    static const std::unordered_set<std::string> alphabet = {
        " ", "a", "c", "e", "g", "m", "n", "o", "p", "q", "r", "s", "u", "v", "w", "x", "y", "z",
    };
    return is_small_kana(s) || alphabet.contains(s);
}

// Consecutive runs of 2+ of these are drawn horizontally side-by-side.
static bool is_hgroup_char(const std::string& s) {
    static const std::unordered_set<std::string> hgroup_set = {
        "!",
        "?",
        "\xef\xbc\x81", // ！
        "\xef\xbc\x9f", // ？
        "\xe2\x80\xa0", // chaos time the dark
    };
    return hgroup_set.contains(s);
}

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0, pos;
    while ((pos = text.find('\n', start)) != std::string::npos) {
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    lines.push_back(text.substr(start));
    return lines;
}

static bool in_rotate_set(const std::string& s) {
    static const std::unordered_set<std::string> rotate_set = {
        "-",
        "\xe2\x80\x90",
        "|",
        "/",
        "\\",
        "\xe3\x83\xbc",
        "\xef\xbd\x9e",
        "~",
        "\xef\xbc\x88",
        "\xef\xbc\x89",
        "(",
        ")",
        "\xe3\x80\x8c",
        "\xe3\x80\x8d",
        "[",
        "]",
        "\xef\xbc\xb3",
        "\xef\xbc\xb4",
        "\xe3\x80\x90",
        "\xe3\x80\x91",
        "\xe2\x80\xa6",
        "\xe2\x86\x92",
        ":",
        "\xef\xbc\x9a",
    };
    return rotate_set.contains(s);
}

OutlinedText::OutlinedText(std::string text, int logical_font_size, Color color, Color outline_color, bool is_vertical,
                           float outline_thickness, float logical_spacing, float v_advance,
                           const std::string& font_name)
    : text(std::move(text)), density(std::max(global_tex.texture_density, 1)),
      font_size(static_cast<float>(logical_font_size * density)),
      outline_thickness(outline_thickness * global_tex.screen_scale * static_cast<float>(density)),
      v_advance(v_advance),
      dbg_font((font_name.empty() ? std::string("default") : font_name) + "@" + std::to_string(logical_font_size)) {
    text_stats::Scope stat(text_stats::CTOR);
    // Z-DON texture density: the text is rasterised at the physical size (x density) and drawn at the logical one
    const int font_size = logical_font_size * density;
    const float spacing = logical_spacing * static_cast<float>(density);
    auto& manager = named_font_manager(font_name);
    // upstream: a text built before with the same font, size, colours and layout reuses its texture
    if (outlined_text_cache().enabled()) {
        const auto rgba = [](Color c) {
            return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) | (static_cast<uint32_t>(c.b) << 16) |
                   (static_cast<uint32_t>(c.a) << 24);
        };
        cache_key =
            std::format("{}/{}/{}/{}/{}/{}/{}/{}/{}/{}", manager.cache_generation(), density, font_size, rgba(color),
                        rgba(outline_color), is_vertical, this->outline_thickness, spacing, v_advance, this->text);
        if (auto cached = outlined_text_cache().get(cache_key)) {
            cached_texture = std::move(cached);
            texture = cached_texture->texture;
            width = cached_texture->width;
            height = cached_texture->height;
            x_offset = cached_texture->x_offset;
            y_offset = cached_texture->y_offset;
            top_extra = cached_texture->top_extra;
            return;
        }
    }
    worker_font = manager.copy_font(this->text, font_size, worker_glyphs);

    if (is_vertical) {
        float char_height = measure_text_ex(worker_font, "A", font_size, spacing).y;
        float max_char_width = 0.0f;
        int char_count = 0;
        const char* ptr = this->text.c_str();
        while (*ptr) {
            int cp_size = 0;
            get_codepoint_next(ptr, &cp_size);
            if (cp_size <= 0) {
                break;
            }
            std::string s(ptr, cp_size);
            float w = measure_text_ex(worker_font, s.c_str(), font_size, spacing).x;
            max_char_width = std::max(max_char_width, w);
            ++char_count;
            ptr += cp_size;
        }
        int pad = static_cast<int>(this->outline_thickness) + 2;
        width = max_char_width + pad * 2;
        height = char_height * (1.0f + (char_count - 1) * v_advance) + pad * 2;
    } else {
        float sp = (spacing < 0) ? 0.0f : spacing;
        int pad = static_cast<int>(this->outline_thickness) + 2;
        auto lines = split_lines(this->text);
        float line_h = measure_text_ex(worker_font, "A", font_size, sp).y;
        float line_advance = line_h + this->outline_thickness * 2;
        float max_w = 0.0f;
        for (const auto& line : lines) {
            float lw = measure_text_ex(worker_font, line.c_str(), font_size, sp).x;
            max_w = std::max(max_w, lw);
        }
        width = max_w + pad * 2;
        height = line_advance * static_cast<float>(lines.size() - 1) + line_h + pad * 2;
    }
    width /= static_cast<float>(density);
    height /= static_cast<float>(density);

#ifdef __EMSCRIPTEN__
    {
        auto data = is_vertical ? build_vertical_text(color, outline_color, spacing)
                                : build_horizontal_text(color, outline_color, spacing);
        pending_image = std::move(data.img);
    }
#else
    if (!platform_async_text_build()) { // upstream: platforms without a worker for it build inline
        auto data = is_vertical ? build_vertical_text(color, outline_color, spacing)
                                : build_horizontal_text(color, outline_color, spacing);
        pending_image = data.img;
        return;
    }
    auto build = is_vertical ? &OutlinedText::build_vertical_text : &OutlinedText::build_horizontal_text;
    build_future = jobs::submit(jobs::Priority::Normal, [this, build, color, outline_color, spacing] {
        auto data = (this->*build)(color, outline_color, spacing);
        std::scoped_lock lock(pending_mutex);
        pending_image = data.img;
    });
    fixed_step::wait(build_future);
#endif
}

OutlinedText::~OutlinedText() {
    if (build_future.valid()) {
        build_future.wait();
    }
    {
        std::scoped_lock lock(pending_mutex);
        if (pending_image.has_value()) {
            unload_image(pending_image.value());
            pending_image.reset();
        }
    }
    if (texture.has_value() && !cached_texture) {
        unload_texture(texture.value());
    }
}

OutlinedText::BuildData OutlinedText::build_horizontal_text(Color color, Color outline_color, float spacing) {
    float sp = (spacing < 0) ? 0.0f : spacing;
    auto lines = split_lines(text);
    float line_h = measure_text_ex(worker_font, "A", font_size, sp).y;
    float line_advance = line_h + outline_thickness * 2;
    float max_w = 0.0f;
    for (const auto& line : lines) {
        float lw = measure_text_ex(worker_font, line.c_str(), font_size, sp).x;
        max_w = std::max(max_w, lw);
    }

    int pad = static_cast<int>(outline_thickness) + 2;

    top_extra = std::max(0, text_margin(font_size) - pad);
    Image img = gen_image_color(static_cast<int>(max_w) + pad * 2,
                                static_cast<int>(line_advance * static_cast<float>(lines.size() - 1) + line_h) +
                                    pad * 2 + top_extra,
                                clear_of(outline_thickness > 0 ? outline_color : color));

    for (int li = 0; li < static_cast<int>(lines.size()); li++) {
        float y = pad + top_extra + li * line_advance;
        if (outline_thickness > 0) {
            stamp_outline(&img, worker_font, lines[li].c_str(), {static_cast<float>(pad), y}, font_size, sp,
                          outline_color, outline_thickness);
        }
        draw_text_clean(&img, worker_font, lines[li].c_str(), {static_cast<float>(pad), y}, font_size, sp, color);
    }

    return {img};
}

OutlinedText::BuildData OutlinedText::build_vertical_text(Color color, Color outline_color, float spacing) {
    float char_height = measure_text_ex(worker_font, "A", font_size, spacing).y;

    // Parse the text into items. Consecutive runs of 2+ hgroup chars (!, ?)
    // become a single item drawn horizontally; everything else is one item.
    struct TextItem {
        std::vector<std::string> chars;
        bool is_hgroup;
        float width; // total render width
    };

    std::vector<std::string> raw_chars;
    {
        const char* ptr = text.c_str();
        while (*ptr) {
            int cp_size = 0;
            get_codepoint_next(ptr, &cp_size);
            if (cp_size <= 0) {
                break;
            }
            raw_chars.emplace_back(ptr, cp_size);
            ptr += cp_size;
        }
    }

    std::vector<TextItem> items;
    for (int i = 0; i < static_cast<int>(raw_chars.size());) {
        if (is_hgroup_char(raw_chars[i])) {
            int j = i;
            while (j < static_cast<int>(raw_chars.size()) && is_hgroup_char(raw_chars[j])) {
                j++;
            }
            TextItem item;
            item.is_hgroup = (j - i >= 2);
            item.chars.assign(raw_chars.begin() + i, raw_chars.begin() + j);
            item.width = 0.0f;
            for (const auto& s : item.chars) {
                item.width += measure_text_ex(worker_font, s.c_str(), font_size, spacing).x;
            }
            items.push_back(std::move(item));
            i = j;
        } else {
            TextItem item;
            item.is_hgroup = false;
            item.chars = {raw_chars[i]};
            item.width = measure_text_ex(worker_font, raw_chars[i].c_str(), font_size, spacing).x;
            items.push_back(std::move(item));
            ++i;
        }
    }

    float max_item_width = 0.0f;
    for (const auto& item : items) {
        max_item_width = std::max(max_item_width, item.width);
    }

    int pad = static_cast<int>(outline_thickness) + 2;
    int img_w = static_cast<int>(max_item_width + pad * 2);

    // Pre-compute y position for each item.
    static constexpr float small_char_advance_factor = 0.8f;
    static constexpr float beside_advance_factor = 0.3f;
    std::vector<float> y_positions(items.size(), static_cast<float>(pad));
    float current_y = static_cast<float>(pad);
    for (int i = 1; i < static_cast<int>(items.size()); i++) {
        float advance;
        const auto& first = items[i].chars[0];
        if (items[i].is_hgroup) {
            advance = char_height;
        } else if (is_beside_prev_char(first)) {
            advance = char_height * beside_advance_factor;
        } else if (needs_less_spacing_above(first)) {
            advance = char_height * small_char_advance_factor;
        } else {
            advance = char_height;
        }
        current_y += advance * v_advance;
        y_positions[i] = current_y;
    }
    int img_h = items.empty() ? pad * 2 : static_cast<int>(y_positions.back() + char_height + pad);

    Image img = gen_image_color(img_w, img_h, clear_of(color));
    Image outline_img = gen_image_color(img_w, img_h, clear_of(outline_color));

    // Two passes: 0 = outlines onto outline_img, 1 = fills onto img.
    for (int pass = 0; pass < 2; pass++) {
        Image* target = (pass == 0) ? &outline_img : &img;

        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            const auto& item = items[i];
            float y = y_positions[i];

            if (item.is_hgroup) {
                // Chars drawn left-to-right, group centred in max_item_width.
                float cx = pad + (max_item_width - item.width) / 2.0f;
                for (const auto& s : item.chars) {
                    float cw = measure_text_ex(worker_font, s.c_str(), font_size, spacing).x;
                    if (pass == 0) {
                        if (outline_thickness > 0) {
                            stamp_outline(target, worker_font, s.c_str(), {cx, y}, font_size, spacing, outline_color,
                                          outline_thickness);
                        }
                    } else {
                        draw_text_clean(target, worker_font, s.c_str(), {cx, y}, font_size, spacing, color);
                    }
                    cx += cw;
                }
            } else {
                const auto& s = item.chars[0];
                float char_width = item.width;
                float x = is_beside_prev_char(s) ? pad + (max_item_width - char_width)
                                                 : pad + (max_item_width - char_width) / 2.0f;
                float draw_y = y + beside_y_offset(s, char_height);

                if (!is_beside_prev_char(s) && in_rotate_set(s)) {
                    int tmp_w = static_cast<int>(char_width) + pad * 2;
                    int tmp_h = static_cast<int>(char_height) + pad * 2;
                    Image tmp = gen_image_color(tmp_w, tmp_h, clear_of(outline_thickness > 0 ? outline_color : color));

                    if (pass == 0) {
                        if (outline_thickness > 0) {
                            stamp_outline(&tmp, worker_font, s.c_str(),
                                          {static_cast<float>(pad), static_cast<float>(pad)}, font_size, spacing,
                                          outline_color, outline_thickness);
                        }
                    } else {
                        draw_text_clean(&tmp, worker_font, s.c_str(),
                                        {static_cast<float>(pad), static_cast<float>(pad)}, font_size, spacing, color);
                    }
                    image_rotate_cw(&tmp);

                    float dx = x + (char_width - static_cast<float>(tmp.width)) / 2.0f;
                    float dy = draw_y + (char_height - static_cast<float>(tmp.height)) / 2.0f;
                    Rect dst = {dx, dy, static_cast<float>(tmp.width), static_cast<float>(tmp.height)};
                    blit_over(target, tmp, static_cast<int>(dst.x), static_cast<int>(dst.y));
                    unload_image(tmp);
                } else {
                    if (pass == 0) {
                        if (outline_thickness > 0) {
                            stamp_outline(target, worker_font, s.c_str(), {x, draw_y}, font_size, spacing,
                                          outline_color, outline_thickness);
                        }
                    } else {
                        draw_text_clean(target, worker_font, s.c_str(), {x, draw_y}, font_size, spacing, color);
                    }
                }
            }
        }

        if (pass == 0) {
            blit_over(&img, outline_img, 0, 0);
            unload_image(outline_img);
        }
    }

    return {img};
}

bool OutlinedText::upload_pending() {
    std::optional<Image> img;
    {
        std::scoped_lock lock(pending_mutex);
        if (!pending_image.has_value()) {
            return false;
        }
        img = pending_image;
        pending_image.reset();
    }

    // the build task still reads worker_font; only release it once the task has finished
    if (build_future.valid()) {
        build_future.wait();
    }
    worker_font = {};
    worker_glyphs = {}; // drop this text's glyph references

    Texture tex{};
    {
        text_stats::Scope stat(text_stats::UPLOAD);
        tex = load_texture_from_image(*img);
        set_texture_filter(tex, texture_filter::BILINEAR);
    }
    unload_image(*img);

    texture = tex;
    width = static_cast<float>(tex.width) / static_cast<float>(density);
    height = (static_cast<float>(tex.height) - top_extra) / static_cast<float>(density);
    if (!cache_key.empty() && tex.id != 0) {
        cached_texture = std::make_shared<CachedOutlinedText>();
        cached_texture->texture = tex;
        cached_texture->width = width;
        cached_texture->height = height;
        cached_texture->x_offset = x_offset;
        cached_texture->y_offset = y_offset;
        cached_texture->top_extra = top_extra;
        outlined_text_cache().put(cache_key, cached_texture, static_cast<size_t>(tex.width) * tex.height * 4);
    }
    return true;
}

void OutlinedText::finish() {
    if (build_future.valid()) {
        build_future.get();
    }
    upload_pending();
}

void OutlinedText::draw(const DrawTextureParams& params) {
    if (!texture.has_value()) {
        upload_pending();
    }

    if (!texture.has_value()) {
        return;
    }

    // Z-DON texture density: the texture is physical (x density), a caller's src and everything drawn logical
    const auto d = static_cast<float>(density);
    Rect src = params.src.value_or(
        Rect{0, 0, static_cast<float>(texture->width) / d, static_cast<float>(texture->height) / d});
    float dx = params.x + x_offset;
    float dy = params.y + y_offset - top_extra / d;

    if (params.x2 == 0.0f && params.y2 == 0.0f) { // on whole (physical) pixels
        dx = roundf(dx * d) / d;
        dy = roundf(dy * d) / d;
    }

    // dst is sized off src (not the full texture) so a caller can pass a
    // shorter/narrower src to hard-crop the text instead of stretching it.
    float dst_w = src.width * params.scale + params.x2;
    float dst_h = src.height * params.scale + params.y2;
    Rect dst;
    if (params.center) {
        dst = {dx + (src.width - src.width * params.scale) * 0.5f, dy + (src.height - src.height * params.scale) * 0.5f,
               dst_w, dst_h};
    } else {
        dst = {dx, dy, dst_w, dst_h};
    }
    if (debug_log_draws) {
        std::string label = text.size() > 40 ? text.substr(0, 40) + "..." : text;
        debug_draw_log.push_back({"\"" + label + "\" {" + dbg_font + "}", dst});
        log_lua_site(debug_draw_log.back(), script_lua_state());
    }

    if (density != 1) {
        src = {src.x * d, src.y * d, src.width * d, src.height * d};
    }
    draw_texture_pro(*texture, src, dst, {0, 0}, 0.0f, fade(Color::WHITE, params.fade));
}

FontManager font_manager;

static std::unordered_map<std::string, std::unique_ptr<FontManager>> named_fonts;

FontManager& named_font_manager(const std::string& name) {
    if (name.empty()) {
        return font_manager;
    }
    auto it = named_fonts.find(name);
    if (it == named_fonts.end()) {
        std::unique_ptr<FontManager> fm;
        if (name.find_first_of("/\\") == std::string::npos) {
            fs::path p = resolve_skin_path("Fonts/" + (fs::path(name).has_extension() ? name : name + ".ttf"));
            try {
                fm = std::make_unique<FontManager>();
                fm->init(p);
                GlyphSubset probe;
                if (fm->copy_font("A?", 16, probe).glyphCount <= 0) {
                    throw std::runtime_error("no usable glyphs in " + p.string());
                }
            } catch (const std::exception& e) {
                spdlog::warn("font '{}' failed to load, using default: {}", name, e.what());
                fm.reset();
            }
        } else {
            spdlog::warn("invalid font name '{}', using default", name);
        }
        it = named_fonts.emplace(name, std::move(fm)).first; // null entry caches failure
    }
    return it->second ? *it->second : font_manager;
}

void unload_named_fonts() {
    for (auto& [n, fm] : named_fonts) {
        if (fm) {
            fm->unload();
        }
    }
    named_fonts.clear();
}

bool is_valid_utf8(std::string_view text) {
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            i++;
            continue;
        }
        int extra;
        unsigned int cp;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= n) {
            return false;
        }
        for (int k = 1; k <= extra; k++) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // overlong forms, surrogates and values past U+10FFFF are not UTF-8
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string utf8_from_utf8_or_sjis(std::string_view text) {
    if (is_valid_utf8(text)) {
        return std::string(text);
    }
#if defined(_WIN32)
    return win32_utf8_from_sjis(text);
#elif defined(YATAIDON_HAVE_ICONV)
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return std::string(text);
    }
    std::string out(text.size() * 3 + 4, '\0');
    char* in_ptr = const_cast<char*>(text.data());
    size_t in_left = text.size();
    char* out_ptr = out.data();
    size_t out_left = out.size();
    while (in_left > 0) {
        if (iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left) != static_cast<size_t>(-1)) {
            break;
        }
        // a byte CP932 does not map: keep it as '?' and go on
        if (out_left == 0) {
            break;
        }
        *out_ptr++ = '?';
        out_left--;
        in_ptr++;
        in_left--;
    }
    iconv_close(cd);
    out.resize(out.size() - out_left);
    return out;
#else
    return std::string(text);
#endif
}
