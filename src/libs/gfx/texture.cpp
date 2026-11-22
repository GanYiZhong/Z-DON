#include "texture.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <spdlog/spdlog.h>
#include <string_view>
#include <thread>

#include "../file/filesystem.h"
#include "../file/script.h"
#include "../global_data.h"
#include "../jobs.h"
#include "../png_fast.h"
#include "../profiler.h"
#include "../texture_bc7.h"
#include "../texture_ondemand.h"
#include "../texture_pack.h"
#include "../texture_retain.h"
#include "../zdon_os.h"
#if !defined(_WIN32) && defined(ZDON_POSIX_LIST_DIR)
#include <sys/stat.h>
#endif
#include "text.h"

#ifdef _WIN32
#include "../../platform/platform_windows.h"
#include "../raylib_ns.h"
#endif

namespace {

    // Z-DON: what a chart can show, from its .tja text: any #GOGOSTART, any balloon (7) or kusudama (9) in a note
    // line (a line with no ':' that is not a command, comments cut). All courses of the file count; anything that
    // is not a readable .tja counts as having everything. Kept per file and write time.
    struct ChartFeatures {
        bool gogo = true, balloon = true, kusudama = true;
    };
    ChartFeatures chart_features(const fs::path& chart) {
        static std::mutex mutex;
        static std::map<fs::path, std::pair<fs::file_time_type, ChartFeatures>> cache;
        std::error_code ec;
        const auto mtime = fs::last_write_time(chart, ec);
        if (ec || chart.extension() != ".tja") {
            return {};
        }
        {
            std::scoped_lock lock(mutex);
            if (auto it = cache.find(chart); it != cache.end() && it->second.first == mtime) {
                return it->second.second;
            }
        }
        std::ifstream in(chart, std::ios::binary);
        if (!in) {
            return {};
        }
        ChartFeatures f{false, false, false};
        std::string line;
        while (std::getline(in, line)) {
            if (const size_t c = line.find("//"); c != std::string::npos) {
                line.resize(c);
            }
            const size_t start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos) {
                continue;
            }
            if (line[start] == '#') {
                f.gogo = f.gogo || line.compare(start, 10, "#GOGOSTART") == 0;
                continue;
            }
            if (line.find(':') != std::string::npos) {
                continue; // a header (TITLE:, BALLOON:, ...)
            }
            f.balloon = f.balloon || line.find('7') != std::string::npos;
            f.kusudama = f.kusudama || line.find('9') != std::string::npos;
        }
        std::scoped_lock lock(mutex);
        cache[chart] = {mtime, f};
        return f;
    }

    // Z-DON: the game folders a play mode never draws, by the screen showing (global_data.current_screen):
    // AI battle (GAME_2P, AI_GAME), dan (GAME_DAN), the tutorial's (TUTORIAL, INPUT_CALI) and enso battle's
    // (PyTaikoGreen: 13 textures, 26 MB, all over the 1 MB a game texture may wait for its first draw). Another mode,
    // or ZDON_DEFER_MODE_FOLDERS=0: all of them load as before. practice always loads: every mode's pause menu draws
    // its panel (left out, opening the menu loaded it: 46-386 ms).
    std::unordered_set<std::string> deferred_game_subsets(const std::string& screen, const std::string& mode) {
        static const bool on = [] {
            const char* v = std::getenv("ZDON_DEFER_MODE_FOLDERS");
            return !(v && *v == '0');
        }();
        static const std::unordered_set<std::string> modes = {"GAME",    "GAME_2P",  "GAME_DAN",  "GAME_PRACTICE",
                                                              "AI_GAME", "TUTORIAL", "INPUT_CALI"};
        if (!on || screen != "game" || !modes.contains(mode)) {
            return {};
        }
        std::unordered_set<std::string> out{"enso_battle"}; // enso battle (BATTLE_GAME) is not one of the modes
        if (mode != "GAME_2P" && mode != "AI_GAME") {
            out.insert({"ai_battle", "gauge_ai"});
        }
        if (mode != "GAME_DAN") {
            out.insert({"dan_info", "gauge_dan", "ss_dan"});
        }
        if (mode != "TUTORIAL" && mode != "INPUT_CALI") {
            out.insert("tutorial");
        }
        // Z-DON: and, for one song (not dan, tutorial or calibration), what its chart never shows: the gogo time
        // art (Nijiiro 18 MB), the kusudama (18 MB) and the balloon (6 MB) are only drawn once one starts. A
        // folder left out loads when first asked for (load_deferred), so a wrong guess only costs that load.
        if (mode == "GAME" || mode == "GAME_2P" || mode == "GAME_PRACTICE" || mode == "AI_GAME") {
            ChartFeatures any{false, false, false};
            bool songs = false;
            for (const SessionData& sd : global_data.session_data) {
                if (sd.selected_song.empty()) {
                    continue;
                }
                songs = true;
                const ChartFeatures f = chart_features(sd.selected_song);
                any.gogo = any.gogo || f.gogo;
                any.balloon = any.balloon || f.balloon;
                any.kusudama = any.kusudama || f.kusudama;
            }
            if (songs && !any.gogo) {
                out.insert("gogo_time");
            }
            if (songs && !any.kusudama) {
                out.insert("kusudama");
            }
            if (songs && !any.balloon && !any.kusudama) {
                out.insert("balloon");
            }
        }
        return out;
    }

    // the <subset> of a file under <skin>/Graphics/<screen>/<subset>/..., "" when it isn't one
    std::string graphics_subset(const fs::path& file, const std::string& screen) {
        auto it = file.begin();
        for (; it != file.end(); ++it) {
            if (*it == "Graphics") {
                break;
            }
        }
        if (it == file.end() || ++it == file.end() || *it != screen || ++it == file.end()) {
            return {};
        }
        return it->string();
    }

    inline double json_number(const Value& v, double fallback = 0.0) {
        if (v.IsInt()) {
            return static_cast<double>(v.GetInt());
        }
        if (v.IsInt64()) {
            return static_cast<double>(v.GetInt64());
        }
        if (v.IsUint()) {
            return static_cast<double>(v.GetUint());
        }
        if (v.IsUint64()) {
            return static_cast<double>(v.GetUint64());
        }
        if (v.IsNumber()) {
            return v.GetDouble();
        }
        return fallback;
    }

    inline double json_member(const Value& o, const char* key, double fallback) {
        return o.HasMember(key) ? json_number(o[key], fallback) : fallback;
    }

    // "crop": [[x, y, w, h], ...]; malformed entries are logged and skipped
    std::vector<Rect> parse_crops(const Value& crop_array, const std::string& tex_name) {
        std::vector<Rect> crops;
        for (SizeType j = 0; j < crop_array.Size(); j++) {
            const Value& crop = crop_array[j];
            if (!crop.IsArray() || crop.Size() < 4) {
                spdlog::error("Invalid crop entry {} for texture {}", j, tex_name);
                continue;
            }
            crops.emplace_back(crop[0].GetFloat(), crop[1].GetFloat(), crop[2].GetFloat(), crop[3].GetFloat());
        }
        return crops;
    }

    // Z-DON: a big RGBA image cut into "crop" frames (Nijiiro gogo_time/explosion: 2352x3810, 30 frames, 34 MB,
    // no BC7: its alpha gradients miss the quality bar) is mostly empty: each frame keeps only the rect around its
    // non-zero pixels, 2 pixels wider on every side (so the kept rect's edge pixels are zero, like everything
    // the frame leaves out: a sample at or past the edge reads zeros either way), packed in rows into a smaller
    // image that replaces `img`. Only images of at least ZDON_TRIM_ATLAS_MB (default 8; 0 = off) whose frames
    // are all inside the image, when the packed one is at most 3/4 of it. nullopt: img unchanged.
    std::optional<std::vector<TextureObject::CropTrim>> trim_crop_atlas(Image& img, const Value& mapping,
                                                                        const std::string& name) {
        static const size_t min_bytes = [] {
            const char* v = std::getenv("ZDON_TRIM_ATLAS_MB");
            return static_cast<size_t>(v && *v ? std::max(0L, std::atol(v)) : 8L) << 20;
        }();
        if (min_bytes == 0 || !img.data || img.format != pixel_format::RGBA8 || img.mipmaps != 1 ||
            static_cast<size_t>(img.width) * img.height * 4 < min_bytes) {
            return std::nullopt;
        }
        const Value* first = mapping.IsArray() ? (mapping.Size() > 0 ? &mapping[0] : nullptr) : &mapping;
        if (!first || !first->IsObject() || !first->HasMember("crop") || !(*first)["crop"].IsArray()) {
            return std::nullopt;
        }
        const std::vector<Rect> crops = parse_crops((*first)["crop"], name);
        if (crops.empty()) {
            return std::nullopt;
        }
        const int W = img.width, H = img.height;
        const auto* px = static_cast<const uint32_t*>(img.data);
        constexpr int kMargin = 2;
        struct Keep {
            int x0, y0, x1, y1; // in the image, x1/y1 exclusive; x1 == x0: empty
        };
        std::vector<Keep> keep;
        keep.reserve(crops.size());
        for (const Rect& c : crops) {
            const int cx = static_cast<int>(c.x), cy = static_cast<int>(c.y);
            const int cw = static_cast<int>(c.width), ch = static_cast<int>(c.height);
            if (static_cast<float>(cx) != c.x || static_cast<float>(cy) != c.y || static_cast<float>(cw) != c.width ||
                static_cast<float>(ch) != c.height || cx < 0 || cy < 0 || cw <= 1 || ch <= 1 || cx + cw > W ||
                cy + ch > H) {
                return std::nullopt;
            }
            int x0 = cx + cw, y0 = cy + ch, x1 = cx, y1 = cy;
            for (int y = cy; y < cy + ch; ++y) {
                const uint32_t* row = px + static_cast<size_t>(y) * W;
                for (int x = cx; x < cx + cw; ++x) {
                    if (row[x] != 0) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x + 1);
                        y0 = std::min(y0, y);
                        y1 = std::max(y1, y + 1);
                    }
                }
            }
            if (x1 <= x0) {
                keep.push_back({cx, cy, cx, cy});
                continue;
            }
            keep.push_back({std::max(cx, x0 - kMargin), std::max(cy, y0 - kMargin), std::min(cx + cw, x1 + kMargin),
                            std::min(cy + ch, y1 + kMargin)});
        }
        // rows of rects, tallest first, 1 empty pixel between them, as wide as the image
        std::vector<size_t> order(keep.size());
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::ranges::stable_sort(order,
                                 [&](size_t l, size_t r) { return keep[l].y1 - keep[l].y0 > keep[r].y1 - keep[r].y0; });
        std::vector<std::pair<int, int>> at(keep.size(), {0, 0});
        int pen_x = 0, pen_y = 0, row_h = 0;
        for (const size_t i : order) {
            const int w = keep[i].x1 - keep[i].x0, h = keep[i].y1 - keep[i].y0;
            if (w <= 0) {
                continue;
            }
            if (pen_x > 0 && pen_x + w > W) {
                pen_x = 0;
                pen_y += row_h + 1;
                row_h = 0;
            }
            at[i] = {pen_x, pen_y};
            pen_x += w + 1;
            row_h = std::max(row_h, h);
        }
        const int new_h = std::max(1, pen_y + row_h);
        if (static_cast<size_t>(new_h) * 4 > static_cast<size_t>(H) * 3) {
            return std::nullopt;
        }
        auto* out = static_cast<uint32_t*>(std::calloc(static_cast<size_t>(W) * new_h, 4));
        if (!out) {
            return std::nullopt;
        }
        std::vector<TextureObject::CropTrim> trims(keep.size());
        for (size_t i = 0; i < keep.size(); ++i) {
            const Keep& k = keep[i];
            const int w = k.x1 - k.x0, h = k.y1 - k.y0;
            if (w <= 0) {
                continue;
            }
            for (int y = 0; y < h; ++y) {
                std::memcpy(out + static_cast<size_t>(at[i].second + y) * W + at[i].first,
                            px + static_cast<size_t>(k.y0 + y) * W + k.x0, static_cast<size_t>(w) * 4);
            }
            trims[i] = {static_cast<float>(k.x0) - crops[i].x,
                        static_cast<float>(k.y0) - crops[i].y,
                        static_cast<float>(w),
                        static_cast<float>(h),
                        static_cast<float>(at[i].first),
                        static_cast<float>(at[i].second)};
        }
        spdlog::info("Texture {}: {} crop frames packed without their empty margins, {}x{} -> {}x{} ({:.1f} -> "
                     "{:.1f} MB)",
                     name, crops.size(), W, H, W, new_h, static_cast<double>(W) * H * 4 / 1048576.0,
                     static_cast<double>(W) * new_h * 4 / 1048576.0);
        unload_image(img);
        img = Image{out, W, new_h, 1, pixel_format::RGBA8};
        return trims;
    }

    void apply_frame_order(TextureObject* tex_obj, const Value& mapping) {
        if (!mapping.HasMember("frame_order") || !mapping["frame_order"].IsArray()) {
            return;
        }
        auto* framed = dynamic_cast<FramedTexture*>(tex_obj);
        if (!framed) {
            return;
        }
        std::vector<std::shared_ptr<Texture>> reordered;
        for (SizeType j = 0; j < mapping["frame_order"].Size(); j++) {
            int idx = static_cast<int>(json_number(mapping["frame_order"][j]));
            if (idx < 0 || idx >= static_cast<int>(framed->textures.size())) {
                spdlog::error("Invalid frame_order index {} for texture {}", idx, tex_obj->name);
                continue;
            }
            reordered.push_back(framed->textures[idx]);
        }
        framed->textures = std::move(reordered);
    }

    // Copies src (restarting it if src is running) into dst and returns the copy
    BaseAnimation* store_animation_copy(std::vector<std::unique_ptr<BaseAnimation>>& dst, const BaseAnimation& src) {
        auto new_anim = src.copy();
        if (src.isStarted()) {
            new_anim->start();
        }
        dst.push_back(std::move(new_anim));
        return dst.back().get();
    }

} // namespace

bool save_texture_position(const TextureObject& obj, int index) {
    if (obj.json_path.empty() || index < 0 || index >= std::ssize(obj.x)) {
        return false;
    }
    try {
        rapidjson::Document doc = read_json_file(obj.json_path);
        if (!doc.IsObject() || !doc.HasMember(obj.json_key.c_str())) {
            return false;
        }
        Value& entry = doc[obj.json_key.c_str()];
        Value* mapping = &entry;
        if (entry.IsArray()) {
            if (index >= static_cast<int>(entry.Size())) {
                return false;
            }
            mapping = &entry[index];
        }
        if (!mapping->IsObject()) {
            return false;
        }

        auto& alloc = doc.GetAllocator();
        const int values[4] = {obj.x[index], obj.y[index], obj.x2[index], obj.y2[index]};
        const char* keys[4] = {"x", "y", "x2", "y2"};
        // x2/y2 default to the image size, so only write them when they now differ from what the json yields.
        const double defaults[4] = {0.0, 0.0, static_cast<double>(obj.width), static_cast<double>(obj.height)};
        for (int i = 0; i < 4; i++) {
            const int loaded = static_cast<int>(json_member(*mapping, keys[i], defaults[i]) * obj.json_scale);
            if (values[i] == loaded) {
                continue;
            }
            const double raw = values[i] / static_cast<double>(obj.json_scale);
            Value v;
            if (raw == std::round(raw)) {
                v.SetInt(static_cast<int>(raw));
            } else {
                v.SetDouble(raw);
            }
            if (mapping->HasMember(keys[i])) {
                (*mapping)[keys[i]] = v;
            } else {
                mapping->AddMember(Value(keys[i], alloc), v, alloc);
            }
        }
        return write_json_file(obj.json_path, doc);
    } catch (const std::exception& e) {
        spdlog::error("Failed to save {}: {}", obj.json_path.string(), e.what());
        return false;
    }
}

std::string TextureWrapper::skin_text(const std::string& key, const std::string& language,
                                      const std::string& fallback) const {
    const SkinInfo* info = font_manager.get_text_info(key);
    if (!info) {
        return fallback;
    }
    auto t = info->text.find(language);
    return t == info->text.end() ? fallback : t->second;
}

void TextureWrapper::init(const fs::path& skin_path) {
    graphics_path = skin_path;
    texture_retain::clear(); // a new or reloaded skin: nothing loaded before may be reused

    if (!fs::exists(graphics_path)) {
        throw std::runtime_error("The skin path provided is not a valid path");
    }

    // Every entry below is only ever inserted/overwritten from what the new
    // skin's (and its parent's) json actually specifies. A key the previous
    // skin set but this one doesn't mention would otherwise survive the
    // switch with the old skin's value.
    skin_info.clear();
    skin_config_by_name.clear();
    options.clear();
    chara_3d_config = Chara3DConfig{};
    chara_positions.clear();
    nameplate_positions.clear();

    auto skin_config_file = read_json_file(graphics_path / "skin_config.json");

    // Derive screen dimensions from child config first so screen_scale is known.
    {
        float w = skin_config_file.HasMember("screen") && skin_config_file["screen"].HasMember("width")
                      ? skin_config_file["screen"]["width"].GetFloat()
                      : 1280.0f;
        float h = skin_config_file.HasMember("screen") && skin_config_file["screen"].HasMember("height")
                      ? skin_config_file["screen"]["height"].GetFloat()
                      : 720.0f;
        screen_width = static_cast<int>(w);
        screen_height = static_cast<int>(h);
        screen_scale = w / 1280.0f;
        texture_density = 1;
        if (skin_config_file.HasMember("screen") && skin_config_file["screen"].HasMember("texture_density") &&
            skin_config_file["screen"]["texture_density"].IsInt()) {
            texture_density = std::clamp(skin_config_file["screen"]["texture_density"].GetInt(), 1, 4);
        }
        font_base.clear();
        if (skin_config_file.HasMember("screen") && skin_config_file["screen"].HasMember("font_base") &&
            skin_config_file["screen"]["font_base"].IsString()) {
            font_base = skin_config_file["screen"]["font_base"].GetString();
        }
    }

    auto load_entry = [this](const std::string& name, const Value& v, float scale) {
        if (!v.IsObject()) {
            return;
        }
        float x = static_cast<float>(json_member(v, "x", 0.0)) * scale;
        float y = static_cast<float>(json_member(v, "y", 0.0)) * scale;
        int font_size = static_cast<int>(json_member(v, "font_size", 0) * scale);
        float width = static_cast<float>(json_member(v, "width", 0.0)) * scale;
        float height = static_cast<float>(json_member(v, "height", 0.0)) * scale;

        std::map<std::string, std::string> text_map;
        if (v.HasMember("text") && v["text"].IsObject()) {
            for (auto& t : v["text"].GetObject()) {
                text_map[t.name.GetString()] = t.value.GetString();
            }
        }

        float outline = static_cast<float>(json_member(v, "outline", -1.0));

        SkinInfo info(x, y, font_size, width, height, text_map, outline);

        skin_config_by_name[name] = info;

        auto sc_it = skin_config_map.find(name);
        if (sc_it != skin_config_map.end()) {
            set_skin_config(sc_it->second, std::move(info));
        }
    };

    // "nameplate": an [x, y] per place the nameplate is drawn, same scaling.
    // "chara_3d": the model's scale and rotation, plus a [x, y] per place it is drawn ("entry",
    // "song_select_1p", ...). Positions are scaled like the other skin_config values.
    auto load_chara = [this](const Value& cfg, float scale) {
        if (cfg.HasMember("nameplate") && cfg["nameplate"].IsObject()) {
            for (auto& m : cfg["nameplate"].GetObject()) {
                if (m.value.IsArray() && m.value.Size() >= 2 && m.value[0].IsNumber() && m.value[1].IsNumber()) {
                    nameplate_positions[m.name.GetString()] = {m.value[0].GetFloat() * scale,
                                                               m.value[1].GetFloat() * scale};
                }
            }
        }
        if (!cfg.HasMember("chara_3d") || !cfg["chara_3d"].IsObject()) {
            return;
        }
        for (auto& m : cfg["chara_3d"].GetObject()) {
            std::string name = m.name.GetString();
            if (m.value.IsNumber()) {
                struct CharaField {
                    std::string_view name;
                    float Chara3DConfig::* member;
                };

                static constexpr CharaField fields[] = {
                    {"scale", &Chara3DConfig::scale},
                    {"rot_x", &Chara3DConfig::rot_x},
                    {"rot_y", &Chara3DConfig::rot_y},
                    {"rot_z", &Chara3DConfig::rot_z},
                };

                const auto field = std::ranges::find(fields, name, &CharaField::name);
                if (field != std::end(fields)) {
                    chara_3d_config.*(field->member) = m.value.GetFloat();
                }
            } else if (m.value.IsArray() && m.value.Size() >= 2 && m.value[0].IsNumber() && m.value[1].IsNumber()) {
                chara_positions[name] = {m.value[0].GetFloat() * scale, m.value[1].GetFloat() * scale};
            }
        }
    };

    // Ancestors (parent, its parent, ...), each scaled by this skin's width over its own
    // (screen.width, default 1280). Their skin_config loads farthest first, so nearer values
    // override, and the child's below overrides them all.
    ancestor_graphics_paths.clear();
    ancestor_scales.clear();
    {
        std::vector<fs::path> chain = resolve_skin_chain(graphics_path);
        for (size_t i = 1; i < chain.size(); i++) {
            float aw = 1280.0f;
            try {
                auto cfg = read_json_file(chain[i] / "skin_config.json");
                if (cfg.HasMember("screen") && cfg["screen"].HasMember("width") && cfg["screen"]["width"].IsNumber()) {
                    aw = cfg["screen"]["width"].GetFloat();
                }
            } catch (const std::exception& e) {
                spdlog::warn("skin ancestor {}: {}", chain[i].string(), e.what());
            }
            ancestor_graphics_paths.push_back(chain[i]);
            ancestor_scales.push_back(static_cast<float>(screen_width) / aw);
        }
    }
    parent_graphics_path = ancestor_graphics_paths.empty() ? graphics_path : ancestor_graphics_paths.front();
    for (size_t i = ancestor_graphics_paths.size(); i-- > 0;) {
        auto ancestor_config = read_json_file(ancestor_graphics_paths[i] / "skin_config.json");
        for (auto& m : ancestor_config.GetObject()) {
            load_entry(m.name.GetString(), m.value, ancestor_scales[i]);
        }
        load_chara(ancestor_config, ancestor_scales[i]);
    }

    // Load child skin_config ??overrides parent defaults.
    for (auto& m : skin_config_file.GetObject()) {
        load_entry(m.name.GetString(), m.value, 1.0f);
    }
    load_chara(skin_config_file, 1.0f);

    // a skin whose chain defines no value for a required key reads as zeroes
    for (unsigned int k = 0; k < static_cast<unsigned int>(SC::OPTIONAL_KEYS); k++) {
        set_skin_config(static_cast<SC>(k), SkinInfo(), false);
    }

    if (skin_config_file.HasMember("screen") && skin_config_file["screen"].HasMember("options")) {
        const Value& opts = skin_config_file["screen"]["options"];
        if (opts.IsObject()) {
            for (auto& opt : opts.GetObject()) {
                if (opt.value.IsBool()) {
                    auto it = screen_options_map.find(opt.name.GetString());
                    if (it != screen_options_map.end()) {
                        options[it->second] = opt.value.GetBool();
                    }
                }
            }
        }
    }
}

static Image decode_evicted(const fs::path& source);
void TextureWrapper::unload_textures() {
    if (TextureObject::zdon_tex_use_stats) {
        // uploaded (id != 0) and never handed to GL since the load, by folder
        std::map<std::string, std::pair<int, double>> unused;
        int used_n = 0;
        double used_mb = 0;
        std::unordered_set<const Texture*> seen;
        for (const auto& [name, obj] : textures) {
            for (int f = 0; f < obj->frame_count(); ++f) {
                const Texture* t = obj->frame_texture_raw(f);
                if (!t || t->id == 0 || !seen.insert(t).second) {
                    continue;
                }
                const double mb = static_cast<double>(image_data_size(t->width, t->height, t->format)) / 1048576.0;
                if (TextureObject::zdon_tex_used().contains(t)) {
                    ++used_n;
                    used_mb += mb;
                    continue;
                }
                const size_t slash = name.rfind('/');
                auto& u = unused[slash == std::string::npos ? std::string("-") : name.substr(0, slash)];
                ++u.first;
                u.second += mb;
            }
        }
        int n = 0;
        double mb = 0;
        std::vector<std::pair<double, std::string>> top;
        for (const auto& [folder, u] : unused) {
            n += u.first;
            mb += u.second;
            top.emplace_back(u.second, fmt::format("{} {} ({:.1f} MB)", folder, u.first, u.second));
        }
        std::ranges::sort(top, std::greater<>());
        spdlog::info("[tex_use] {}: drawn {} ({:.1f} MB), uploaded but never drawn {} ({:.1f} MB)",
                     global_data.current_screen, used_n, used_mb, n, mb);
        for (size_t i = 0; i < top.size() && i < 25; ++i) {
            spdlog::info("[tex_use]   {}", top[i].second);
        }
        TextureObject::zdon_tex_used().clear();
    }
    lua_texture_lookup.clear();
    texture_fallbacks.clear();
    language_preferred.clear();
    textures.clear();
    loaded_subsets.clear();
    animations.clear();
    copied_animations.clear();
    screen_animations.clear();
    subset_loaded_ids.clear();
    tex_id_refcount.clear();
    debug_draw_log.clear();
    debug_draw_log_prev.clear();
    screen_loaded.clear();
    ondemand_stash.clear();
    deferred_folders.clear();
    texture_ondemand::flush(); // what the screen that ends loaded on demand
    // the images of textures never drawn: the next load of a screen that holds them decodes them again
    texture_lazy::set_decoder(decode_evicted);
    texture_lazy::evict_all();
}

BaseAnimation* TextureWrapper::get_animation(const int id, bool is_copy) {
    auto anim_it = animations.find(id);
    if (anim_it == animations.end()) {
        throw std::runtime_error("Unable to find animation: " + std::to_string(id));
    }

    if (is_copy) {
        // Note: Returning raw pointer from unique_ptr requires careful management
        return store_animation_copy(copied_animations, *anim_it->second);
    }

    return anim_it->second.get();
}

BaseAnimation* TextureWrapper::get_animation(const int id, const std::string& screen_name) {
    if (!screen_animations.contains(screen_name)) {
        Document anim_config;
        if (read_screen_animations(screen_name, anim_config)) {
            AnimationParser parser;
            screen_animations[screen_name] = parser.parse_animations(anim_config);
        } else {
            spdlog::warn("No animation.json found for screen: {}", screen_name);
        }
    }

    auto screen_it = screen_animations.find(screen_name);
    if (screen_it == screen_animations.end()) {
        throw std::runtime_error("No animations available for screen: " + screen_name);
    }
    auto& anim_map = screen_it->second;
    auto it = anim_map.find(id);
    if (it == anim_map.end()) {
        throw std::runtime_error("Unable to find animation " + std::to_string(id) + " in screen: " + screen_name);
    }

    return store_animation_copy(copied_animations, *it->second);
}

void TextureWrapper::read_tex_obj_data(const Value& tex_mapping, TextureObject* tex_obj, float scale, int density,
                                       float default_size_scale) {
    if (default_size_scale < 0.0f) {
        default_size_scale = scale;
    }
    // Z-DON texture density: `"density": n` on the entry (or its first entry) overrides the skin's
    // texture_density for this one texture: its image is n x logical (1 = logical size, for art that is
    // blocky at the skin density, half or less the memory). Drawn point-filtered when n < the skin's.
    int img_density = density;
    const Value* first = tex_mapping.IsArray() ? (tex_mapping.Size() > 0 ? &tex_mapping[0] : nullptr) : &tex_mapping;
    if (density > 1 && first != nullptr && first->IsObject() && first->HasMember("density") &&
        (*first)["density"].IsInt()) {
        img_density = std::clamp((*first)["density"].GetInt(), 1, density);
        default_size_scale = 1.0f / static_cast<float>(img_density);
    }
    if (tex_mapping.IsArray()) {
        // Check if crop data exists in the first mapping (index 0)
        bool has_crop_in_first = tex_mapping.Size() > 0 && tex_mapping[0].IsObject() &&
                                 tex_mapping[0].HasMember("crop") && tex_mapping[0]["crop"].IsArray() &&
                                 !tex_mapping[0]["crop"].Empty();

        std::vector<Rect> crops;
        if (has_crop_in_first) {
            crops = parse_crops(tex_mapping[0]["crop"], tex_obj->name);
            if (crops.empty()) {
                has_crop_in_first = false;
            } else {
                tex_obj->crop_data = crops;
                tex_obj->width = static_cast<int>(crops[0].width);
                tex_obj->height = static_cast<int>(crops[0].height);
            }
        }

        tex_obj->x.resize(tex_mapping.Size());
        tex_obj->y.resize(tex_mapping.Size());
        tex_obj->x2.resize(tex_mapping.Size());
        tex_obj->y2.resize(tex_mapping.Size());

        for (SizeType i = 0; i < tex_mapping.Size(); i++) {
            const Value& mapping = tex_mapping[i];

            int x = static_cast<int>(json_member(mapping, "x", 0) * scale);
            int y = static_cast<int>(json_member(mapping, "y", 0) * scale);
            int x2 = mapping.HasMember("x2")
                         ? static_cast<int>(json_member(mapping, "x2", tex_obj->width) * scale)
                         : static_cast<int>(static_cast<double>(tex_obj->width) * default_size_scale);
            int y2 = mapping.HasMember("y2")
                         ? static_cast<int>(json_member(mapping, "y2", tex_obj->height) * scale)
                         : static_cast<int>(static_cast<double>(tex_obj->height) * default_size_scale);

            tex_obj->x[i] = x;
            tex_obj->y[i] = y;
            tex_obj->x2[i] = x2;
            tex_obj->y2[i] = y2;

            apply_frame_order(tex_obj, mapping);

            // Apply crop dimensions to all indices if crop exists in first mapping
            if (has_crop_in_first) {
                tex_obj->x2[i] = static_cast<int>(crops[0].width * scale);
                tex_obj->y2[i] = static_cast<int>(crops[0].height * scale);
            }
        }
    } else if (tex_mapping.IsObject()) {
        if (tex_mapping.HasMember("crop") && tex_mapping["crop"].IsArray() && !tex_mapping["crop"].Empty()) {
            std::vector<Rect> crops = parse_crops(tex_mapping["crop"], tex_obj->name);
            if (!crops.empty()) {
                tex_obj->crop_data = crops;
                tex_obj->width = static_cast<int>(crops[0].width);
                tex_obj->height = static_cast<int>(crops[0].height);
            }
        }

        tex_obj->x = {static_cast<int>(json_member(tex_mapping, "x", 0) * scale)};
        tex_obj->y = {static_cast<int>(json_member(tex_mapping, "y", 0) * scale)};
        tex_obj->x2 = {tex_mapping.HasMember("x2")
                           ? static_cast<int>(json_member(tex_mapping, "x2", tex_obj->width) * scale)
                           : static_cast<int>(static_cast<double>(tex_obj->width) * default_size_scale)};
        tex_obj->y2 = {tex_mapping.HasMember("y2")
                           ? static_cast<int>(json_member(tex_mapping, "y2", tex_obj->height) * scale)
                           : static_cast<int>(static_cast<double>(tex_obj->height) * default_size_scale)};

        apply_frame_order(tex_obj, tex_mapping);
    }

    // Z-DON texture density: the numbers above are physical (scale is 1/density), the size becomes logical
    // too; the source rect keeps sampling the whole image (draw_texture)
    if (img_density > 1) {
        tex_obj->density = img_density;
        tex_obj->width /= img_density;
        tex_obj->height /= img_density;
    }

    int filter = img_density < density ? texture_filter::POINT : texture_filter::BILINEAR;
    for (int i = 0; i < tex_obj->frame_count(); i++) {
        texture_lazy::set_filter(tex_obj->frame_texture_raw(i), filter);
    }
}

bool TextureWrapper::read_screen_animations(const std::string& screen_name, Document& out) {
    fs::path own = graphics_path / screen_name / "animation.json";
    if (fs::exists(own)) {
        out = read_json_file(own);
        return true;
    }
    for (size_t i = 0; i < ancestor_graphics_paths.size(); i++) {
        fs::path file = ancestor_graphics_paths[i] / screen_name / "animation.json";
        if (!fs::exists(file)) {
            continue;
        }
        out = read_json_file(file);
        const float scale = ancestor_scales[i];
        auto scale_number = [scale](Value& v) {
            if (v.IsInt()) {
                v.SetInt(static_cast<int>(json_number(v) * scale));
            } else if (v.IsDouble()) {
                v.SetDouble(v.GetDouble() * scale);
            }
        };
        if (out.IsArray()) {
            for (SizeType k = 0; k < out.Size(); k++) {
                Value& anim = out[k];
                if (anim.HasMember("total_distance") && !anim["total_distance"].IsObject()) {
                    scale_number(anim["total_distance"]);
                }
                if (anim.HasMember("start_position") && !anim["start_position"].IsObject()) {
                    scale_number(anim["start_position"]);
                }
                if (anim.HasMember("waypoints") && anim["waypoints"].IsArray()) {
                    for (auto& wp : anim["waypoints"].GetArray()) {
                        if (wp.IsObject() && wp.HasMember("value")) {
                            scale_number(wp["value"]);
                        }
                    }
                }
            }
        }
        return true;
    }
    return false;
}

void TextureWrapper::load_animations(const std::string& screen_name) {
    PROFILE_SCOPE();
    PROFILE_ZONE_TEXT(screen_name);
    Document anim_config;
    if (read_screen_animations(screen_name, anim_config)) {
        AnimationParser parser;
        animations = parser.parse_animations(anim_config);
        spdlog::info("Animations loaded for screen: {}", screen_name);
    }
}

std::unordered_map<std::string, std::weak_ptr<TextureObject>>& tex_object_cache() {
    static std::unordered_map<std::string, std::weak_ptr<TextureObject>> cache;
    return cache;
}

std::shared_ptr<Texture> shared_texture(Texture tex) {
    return std::shared_ptr<Texture>(new Texture(tex), [](Texture* p) {
        if (p->id != 0) {
            unload_texture(*p);
        }
        delete p;
    });
}

namespace {

    // LoadImageFromMemory, through png_fast for PNGs (the same pixels, faster)
    Image image_from_memory(const fs::path& path, const uint8_t* data, size_t size) {
        const std::string ext = path.extension().string();
        if ((ext == ".png" || ext == ".PNG") && png_fast::enabled()) {
            Image image{};
            if (png_fast::decode(data, size, image)) {
                return image;
            }
        }
        return load_image_from_memory(std::span<const unsigned char>(data, size));
    }

    // ray::LoadImage(path), taking the file's bytes from the screen's texture pack when it holds them
    // (and handing the pack the bytes it is missing)
    Image load_image_unchecked(const fs::path& path, const std::string& key, texture_pack::Session* pack) {
        if (pack) {
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (pack->get(key, data, size)) {
                Image image = image_from_memory(path, data, size);
                if (image.data) {
                    return image;
                }
                pack->reject(key);
            }
            if (pack->wants(key)) {
                std::vector<uint8_t> bytes;
                if (texture_pack::read_file(path, bytes)) {
                    Image image = image_from_memory(path, bytes.data(), bytes.size());
                    if (image.data) {
                        pack->put(key, std::move(bytes));
                    }
                    return image;
                }
            }
        }
        if (png_fast::enabled()) {
            std::vector<uint8_t> bytes;
            if (texture_pack::read_file(path, bytes)) {
                return image_from_memory(path, bytes.data(), bytes.size());
            }
        }
        return ::load_image(path.string().c_str());
    }

    // Dev hook: YATAIDON_TEXPACK_VERIFY=1 decodes every image a second time with ray::LoadImage from
    // the skin's file and counts the ones whose pixels differ from what the pack / png_fast gave.
    struct VerifyCounts {
        std::atomic<size_t> checked{0};
        std::atomic<size_t> mismatched{0};
    } verify_counts;

    bool verify_enabled() {
        static const bool on = [] {
            const char* v = std::getenv("YATAIDON_TEXPACK_VERIFY");
            return v && v[0] == '1';
        }();
        return on;
    }

    Image load_image(const fs::path& path, const std::string& key, texture_pack::Session* pack) {
        // Z-DON: the BC7 blocks of a file compressed before, without reading and decoding it
        const texture_bc7::FileStamp st =
            texture_bc7::enabled() && !verify_enabled() ? texture_bc7::stamp(path) : texture_bc7::FileStamp{};
        if (Image cached{}; texture_bc7::cached_image(path, st, cached)) {
            return cached;
        }
        Image image = load_image_unchecked(path, key, pack);
        if (verify_enabled()) {
            Image ref = ::load_image(path.string().c_str());
            const bool same = ref.width == image.width && ref.height == image.height && ref.format == image.format &&
                              (ref.data == nullptr) == (image.data == nullptr) &&
                              (!ref.data || std::memcmp(ref.data, image.data,
                                                        image_data_size(ref.width, ref.height, ref.format)) == 0);
            ++verify_counts.checked;
            if (!same) {
                ++verify_counts.mismatched;
                spdlog::error("Texture pack verify: {} differs from LoadImage", path.string());
            }
            if (ref.data) {
                unload_image(ref);
            }
        }
        texture_bc7::compress(image, path, st); // Z-DON: BC7 when the cache has its blocks
        return image;
    }

} // namespace

void decode_images_parallel(const std::vector<fs::path>& files, std::vector<Image>& out) {
    out.assign(files.size(), Image{});
    if (files.empty()) {
        return;
    }

    unsigned hw = std::thread::hardware_concurrency();
    size_t workers = std::min<size_t>(files.size(), hw ? hw : 4);
    if (workers <= 1) {
        for (size_t i = 0; i < files.size(); ++i) {
            out[i] = load_image(files[i], {}, nullptr);
        }
        return;
    }

    jobs::parallel_for(
        files.size(), jobs::Priority::Critical,
        [&files, &out](size_t i) {
            try {
                out[i] = load_image(files[i], {}, nullptr);
            } catch (const std::exception& e) {
                spdlog::error("Failed to decode image {}: {}", files[i].string(), e.what());
                out[i] = Image{};
            }
        },
        workers);
}

// A screen's texture folders, each listed once. Loading a screen asked the file system about
// every texture.json entry separately (is it a frame folder, is its PNG there) in the child skin
// and again in each ancestor, a few thousand calls for a 1080p skin -- about a third of the time
// song select took to open. Names match case-insensitively on Windows, as the file system does.
class DirIndex {
public:
    struct Entry {
        fs::path path;
        bool is_dir;
        uint64_t size = 0;  // files: as listed (texture packs check it)
        uint64_t mtime = 0; // last write, in the file system's own units
    };

    size_t listed() const { return listings.size(); } // folders listed so far (the load log)
    double list_ms = 0;                               // time spent listing them

    // Z-DON: lists the folders not listed yet on the job pool, all at once (one by one on the main thread, a
    // skin's 100-200 texture folders took 20-40 ms of a screen load)
    void prefetch(const std::vector<fs::path>& dirs) {
        std::vector<fs::path> todo;
        std::unordered_set<fs::path::string_type> seen;
        for (const fs::path& d : dirs) {
            fs::path::string_type k = key(d);
            if (!listings.contains(k) && seen.insert(std::move(k)).second) {
                todo.push_back(d);
            }
        }
        if (todo.empty()) {
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Listing> got(todo.size());
        jobs::parallel_for(todo.size(), jobs::Priority::Critical, [&](size_t i) { fill(got[i], todo[i]); });
        for (size_t i = 0; i < todo.size(); ++i) {
            listings.try_emplace(key(todo[i]), std::move(got[i]));
        }
        list_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // the folder's entries, or nullptr when it cannot be listed
    const std::vector<Entry>* list(const fs::path& dir) {
        const Listing& l = get(dir);
        return l.ok ? &l.entries : nullptr;
    }
    bool exists(const fs::path& dir, const fs::path& name) { return find(dir, name) != nullptr; }
    bool is_dir(const fs::path& dir, const fs::path& name) {
        const Entry* e = find(dir, name);
        return e && e->is_dir;
    }
    bool is_file(const fs::path& dir, const fs::path& name) {
        const Entry* e = find(dir, name);
        return e && !e->is_dir;
    }
    // the listing's entry for a file path (its folder is listed if it wasn't yet)
    const Entry* file_entry(const fs::path& file) {
        const Entry* e = find(file.parent_path(), file.filename());
        return e && !e->is_dir ? e : nullptr;
    }
    // Like file_entry without listing anything: -1 when the folder hasn't been listed, 0 when the
    // file isn't there, 1 (and e set) when it is.
    int peek_file(const fs::path& file, const Entry*& e) const {
        auto it = listings.find(key(file.parent_path()));
        if (it == listings.end() || !it->second.ok) {
            return -1;
        }
        auto f = it->second.by_name.find(key(file.filename()));
        if (f == it->second.by_name.end() || it->second.entries[f->second].is_dir) {
            return 0;
        }
        e = &it->second.entries[f->second];
        return 1;
    }

private:
    struct Listing {
        bool ok = false;
        std::vector<Entry> entries;
        std::unordered_map<fs::path::string_type, size_t> by_name;
    };
    std::unordered_map<fs::path::string_type, Listing> listings;

    static fs::path::string_type key(const fs::path& p) {
        fs::path::string_type k = p.native();
#ifdef _WIN32
        for (auto& c : k) {
            c = c == L'\\' ? L'/' : static_cast<wchar_t>(towlower(c)); // one key per folder, however spelled
        }
#endif
        return k;
    }

    // lists dir into l (any thread: touches nothing but l)
    static void fill(Listing& l, const fs::path& dir) {
#ifdef _WIN32
        std::vector<Win32DirEntry> raw;
        l.ok = win32_list_dir(dir, raw);
        for (Win32DirEntry& e : raw) {
            l.entries.push_back({dir / e.name, e.is_dir, e.size, e.ft_write});
        }
#elif defined(ZDON_POSIX_LIST_DIR)
        // readdir's types instead of a stat per entry (two more per file for the texture packs)
        std::vector<zdon_os::DirEntry> raw;
        l.ok = zdon_os::list_dir(dir, raw, texture_pack::enabled() ? +[](std::string_view) { return true; } : nullptr);
        for (zdon_os::DirEntry& e : raw) {
            l.entries.push_back({dir / e.name, e.is_dir, e.size, e.ft_write});
        }
#else
        std::error_code ec;
        fs::directory_iterator iter(dir, ec);
        l.ok = !ec;
        const bool want_stat = texture_pack::enabled();
        for (; !ec && iter != fs::directory_iterator(); iter.increment(ec)) {
            std::error_code type_ec;
            Entry e{iter->path(), iter->is_directory(type_ec)};
            if (want_stat && !e.is_dir) {
                e.size = static_cast<uint64_t>(iter->file_size(type_ec));
                e.mtime = static_cast<uint64_t>(iter->last_write_time(type_ec).time_since_epoch().count());
            }
            l.entries.push_back(std::move(e));
        }
#endif
        for (size_t i = 0; i < l.entries.size(); ++i) {
            l.by_name.emplace(key(l.entries[i].path.filename()), i);
        }
    }

    const Listing& get(const fs::path& dir) {
        auto [it, inserted] = listings.try_emplace(key(dir));
        Listing& l = it->second;
        if (!inserted) {
            return l;
        }
        const auto t0 = std::chrono::steady_clock::now();
        fill(l, dir);
        list_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return l;
    }

    const Entry* find(const fs::path& dir, const fs::path& name) {
        const Listing& l = get(dir);
        auto it = l.by_name.find(key(name));
        return it == l.by_name.end() ? nullptr : &l.entries[it->second];
    }
};

std::vector<fs::path> sorted_frames(DirIndex& dirs, const fs::path& dir) {
    std::vector<fs::path> frames;
    if (const auto* entries = dirs.list(dir)) {
        for (const DirIndex::Entry& entry : *entries) {
            if (!entry.is_dir) {
                frames.push_back(entry.path);
            }
        }
    }
    std::ranges::sort(frames, [](const fs::path& a, const fs::path& b) {
        const std::string as = a.stem().string(), bs = b.stem().string();
        int an = 0, bn = 0;
        auto ar = std::from_chars(as.data(), as.data() + as.size(), an);
        auto br = std::from_chars(bs.data(), bs.data() + bs.size(), bn);
        bool a_ok = ar.ec == std::errc() && ar.ptr == as.data() + as.size();
        bool b_ok = br.ec == std::errc() && br.ptr == bs.data() + bs.size();
        if (a_ok && b_ok) {
            return an < bn;
        }
        if (a_ok != b_ok) {
            return a_ok;
        }
        return as < bs;
    });
    return frames;
}

std::unordered_set<std::string> overridden_names(DirIndex& dirs, const fs::path& child_folder) {
    std::unordered_set<std::string> names;
    try {
        auto cfg = read_json_file(child_folder / "texture.json");
        for (auto& m : cfg.GetObject()) {
            std::string n = m.name.GetString();
            if (dirs.is_dir(child_folder, n) || dirs.exists(child_folder, n + ".png")) {
                names.insert(n);
            }
        }
    } catch (const std::exception&) {
        // Unreadable child config - fall back to loading everything from the parent.
        names.clear();
    }
    return names;
}

// For each ancestor (nearest first), the texture names a nearer skin already provides for
// <screen>/<subset>; child_has_folder tells whether the skin itself has that folder.
std::vector<std::unordered_set<std::string>> subset_skips(DirIndex& dirs, const fs::path& own_dir,
                                                          const std::vector<fs::path>& ancestor_dirs,
                                                          bool& child_has_folder) {
    child_has_folder = ancestor_dirs.empty() || dirs.is_file(own_dir, "texture.json");
    std::vector<std::unordered_set<std::string>> skips(ancestor_dirs.size());
    std::unordered_set<std::string> claimed;
    if (child_has_folder && !ancestor_dirs.empty()) {
        claimed = overridden_names(dirs, own_dir);
    }
    for (size_t i = 0; i < ancestor_dirs.size(); i++) {
        skips[i] = claimed;
        if (dirs.is_file(ancestor_dirs[i], "texture.json")) {
            for (const std::string& n : overridden_names(dirs, ancestor_dirs[i])) {
                claimed.insert(n);
            }
        }
    }
    return skips;
}

// Z-DON: images a predecode (TextureWrapper::start_predecode) decoded before their screen started to load,
// by image key, with the file's size and time from the pack index. A screen load first checks them against
// its own folder listing (a changed or vanished file is decoded again as usual), then takes them where it
// would decode the file; the ones left when the screen has started are freed.
namespace {
    struct Predecoded {
        struct Image {
            ::Image image{};
            texture_pack::FileStat stat;
            bool checked = false;   // matched the load's listing (image prefetch workers take only these)
            unsigned int gl_id = 0; // uploaded ahead (predecode_tick): the texture of exactly these pixels
        };
        struct Uploaded {
            unsigned int id = 0;
            int width = 0, height = 0, format = 0;
        };
        std::mutex mutex;
        std::unordered_map<std::string, Image> images;
        // the uploaded textures of images handed to the load, by key, until load_folder makes their texture
        std::unordered_map<std::string, Uploaded> uploaded;
        std::string owner;
        size_t given = 0, given_bytes = 0, given_gl = 0;

        static void unload_gl(unsigned int id) {
            if (id != 0) {
                Texture t{};
                t.id = id;
                unload_texture(t);
            }
        }
        // main thread only (it frees GL textures)
        void clear_locked() {
            for (auto& [key, im] : images) {
                texture_retain::free_image_async(im.image);
                unload_gl(im.gl_id);
            }
            images.clear();
            for (auto& [key, up] : uploaded) {
                unload_gl(up.id);
            }
            uploaded.clear();
        }
        // hands an image out (mutex held): its uploaded texture waits for take_gl
        void give_locked(const std::string& key, Image& im) {
            given_bytes += image_data_size(im.image.width, im.image.height, im.image.format);
            ++given;
            if (im.gl_id != 0) {
                uploaded[key] = {im.gl_id, im.image.width, im.image.height, im.image.format};
                im.gl_id = 0;
            }
        }
        // the texture uploaded ahead for key's image, when it matches img (main thread)
        bool take_gl(const std::string& key, const ::Image& img, Texture& out) {
            std::scoped_lock lock(mutex);
            auto it = uploaded.find(key);
            if (it == uploaded.end()) {
                return false;
            }
            const Uploaded up = it->second;
            uploaded.erase(it);
            if (up.width != img.width || up.height != img.height || up.format != img.format || img.mipmaps != 1) {
                unload_gl(up.id);
                return false;
            }
            out = Texture{};
            out.id = up.id;
            out.width = up.width;
            out.height = up.height;
            out.mipmaps = 1;
            out.format = up.format;
            ++given_gl;
            return true;
        }
        // a worker's take: only an image checked against the listing
        bool take_checked(const std::string& key, ::Image& out) {
            std::scoped_lock lock(mutex);
            auto it = images.find(key);
            if (it == images.end() || !it->second.checked) {
                return false;
            }
            out = it->second.image;
            give_locked(key, it->second);
            images.erase(it);
            return true;
        }
    };
    Predecoded& predecoded() {
        static auto* p = new Predecoded(); // never destroyed: jobs may outlive static teardown
        return *p;
    }
} // namespace

class ImagePrefetch {
public:
    ImagePrefetch(const std::vector<std::vector<fs::path>>& groups, const std::unordered_set<std::string>& border_keys,
                  size_t budget_bytes, texture_pack::Session* pack_session)
        : budget(budget_bytes), pack(pack_session) {
        for (size_t g = 0; g < groups.size(); ++g) {
            for (const fs::path& p : groups[g]) {
                std::string key = image_key(p);
                if (index.contains(key)) {
                    continue;
                }
                index.emplace(key, items.size());
                items.push_back(Item{p, key, g});
                items.back().want_border = border_keys.contains(key);
            }
        }
        unsigned hw = std::thread::hardware_concurrency();
        size_t workers = std::min<size_t>(items.size(), std::clamp<unsigned>(hw ? hw - 1 : 3, 1, 16));
        for (size_t w = 0; w < workers; ++w) {
            pool.emplace_back([this] { work(); });
        }
    }

    ~ImagePrefetch() {
        {
            std::scoped_lock lock(mutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : pool) {
            t.join();
        }
        for (Item& it : items) {
            if (it.state == State::Done) {
                texture_retain::free_image_async(it.image);
            }
        }
    }

    // The decoded image for path when it's part of this prefetch (waiting for a worker that
    // has started it, decoding it here if none has); false when it isn't.
    bool take(const fs::path& path, Image& image, Rect& border, bool& has_border) {
        std::unique_lock<std::mutex> lock(mutex);
        auto found = index.find(image_key(path));
        if (found == index.end()) {
            return false;
        }
        Item& it = items[found->second];
        cv.wait(lock, [&] { return it.state != State::Decoding; });
        if (it.state == State::Pending) {
            it.state = State::Taken;
            lock.unlock();
            has_border = decode(it, image, border);
            return true;
        }
        if (it.state != State::Done) {
            return false;
        }
        image = it.image;
        border = it.border;
        has_border = it.has_border;
        it.image = Image{};
        it.state = State::Taken;
        held -= it.bytes;
        cv.notify_all();
        return true;
    }

    // Folder group g has been loaded: whatever it (and earlier groups) left untaken goes.
    void finish_group(size_t g) {
        std::scoped_lock lock(mutex);
        finished_groups = std::max(finished_groups, g + 1);
        for (Item& it : items) {
            if (it.group > g) {
                continue;
            }
            if (it.state == State::Pending) {
                it.state = State::Dropped;
            } else if (it.state == State::Done) {
                texture_retain::free_image_async(it.image);
                it.image = Image{};
                held -= it.bytes;
                it.state = State::Dropped;
            }
        }
        cv.notify_all();
    }

    static std::string image_key(const fs::path& p) { return p.lexically_normal().generic_string(); }

private:
    enum class State {
        Pending,
        Decoding,
        Done,
        Taken,
        Dropped
    };
    struct Item {
        fs::path path;
        std::string key;
        size_t group;
        State state = State::Pending;
        Image image{};
        Rect border{};
        bool want_border = false;
        bool has_border = false;
        size_t bytes = 0;
    };

    // true when border was computed
    bool decode(const Item& it, Image& image, Rect& border) {
        try {
            if (!predecoded().take_checked(it.key, image)) {
                image = load_image(it.path, it.key, pack);
            }
        } catch (const std::exception& e) {
            spdlog::error("Failed to decode image {}: {}", it.path.string(), e.what());
            image = Image{};
        }
        border = Rect{};
        if (!it.want_border || !image.data) {
            return false;
        }
        border = get_image_alpha_border(image, 0.0f);
        return true;
    }

    void work() {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            cv.wait(lock, [&] { return stop || held < budget; });
            while (next < items.size() && items[next].state != State::Pending) {
                ++next;
            }
            if (stop || next >= items.size()) {
                return;
            }
            Item& it = items[next++];
            it.state = State::Decoding;
            lock.unlock();
            Image image;
            Rect border;
            const bool has_border = decode(it, image, border);
            lock.lock();
            if (finished_groups > it.group) { // its folder was loaded while this one decoded
                if (image.data) {
                    unload_image(image);
                }
                it.state = State::Dropped;
                continue;
            }
            it.image = image;
            it.border = border;
            it.has_border = has_border;
            it.bytes = image.data ? image_data_size(image.width, image.height, image.format) : 0;
            it.state = State::Done;
            held += it.bytes;
            cv.notify_all();
        }
    }

    std::vector<Item> items;
    std::unordered_map<std::string, size_t> index;
    std::vector<std::thread> pool;
    std::mutex mutex;
    std::condition_variable cv;
    size_t next = 0;
    size_t held = 0;
    size_t finished_groups = 0; // groups below this are done with
    const size_t budget;
    texture_pack::Session* const pack; // may be null
    bool stop = false;
};

void TextureWrapper::load_folder(const std::string& screen_name, const std::string& subset) {
    lua_texture_lookup.clear();
    texture_fallbacks.clear();
    language_preferred.clear();
    // Subset leaf name is the key prefix used in textures[] (e.g. "notes_nijiiro" from "game/notes_nijiiro")
    const std::string subset_key = fs::path(subset).filename().string();
    const std::string dedup_key = screen_name + "/" + subset_key;

    if (loaded_subsets.contains(dedup_key)) {
        return;
    }

    int loaded_count = 0;
    std::unordered_set<std::string> ids_this_call;
    // Z-DON: on-demand prefetch (texture_ondemand.h): make the objects missing from the caches, keep
    // them in *stash, and leave `textures` and the folder bookkeeping alone
    std::vector<std::shared_ptr<TextureObject>>* const stash_to = stash;
    DirIndex local_dirs;
    DirIndex& dirs = dir_index ? *dir_index : local_dirs;

    // A texture.json entry whose PNG(s) still have to be read off disk.
    struct PendingTex {
        std::string id;
        std::string name;
        const Value* mapping;
        std::string cache_key;
        size_t first_file;
        size_t file_count;
        bool framed;
    };

    auto load_from_path = [&](const fs::path& folder, float tex_scale, const std::unordered_set<std::string>* skip,
                              int density) {
        fs::path tex_json = folder / "texture.json";
        if (!dirs.is_file(folder, "texture.json")) {
            return;
        }

        try {
            const auto tex_config_doc = read_json_shared(tex_json);
            const rapidjson::Document& tex_config = *tex_config_doc;
            auto& cache = tex_object_cache();

            std::vector<PendingTex> pending;
            std::vector<fs::path> files;
            std::unordered_map<std::string, size_t> file_index;
            auto push_file = [&](const fs::path& path) -> size_t {
                std::string key = path.string();
                auto it = file_index.find(key);
                if (it != file_index.end()) {
                    return it->second;
                }
                size_t idx = files.size();
                files.push_back(path);
                file_index.emplace(std::move(key), idx);
                return idx;
            };

            for (auto& m : tex_config.GetObject()) {
                std::string tex_name = m.name.GetString();

                if (skip && skip->contains(tex_name)) {
                    continue;
                }

                std::string tex_id = std::format("{}/{}", subset_key, tex_name);

                std::string cache_key = (folder / tex_name).string();
                if (stash_to) {
                    auto cit = cache.find(cache_key);
                    if (texture_retain::contains(cache_key) || (cit != cache.end() && !cit->second.expired())) {
                        continue; // a load finds it there
                    }
                } else if (auto kept = texture_retain::reuse(cache_key)) {
                    textures[tex_id] = kept;
                    ids_this_call.insert(tex_id);
                    ++loaded_count;
                    continue;
                }
                auto cit = cache.find(cache_key);
                if (cit != cache.end()) {
                    if (auto shared = cit->second.lock()) {
                        textures[tex_id] = shared;
                        ids_this_call.insert(tex_id);
                        ++loaded_count;
                        continue;
                    }
                    cache.erase(cit);
                }

                fs::path tex_dir = folder / tex_name;
                fs::path tex_file = folder / (tex_name + ".png");
                // "file" lets several keys share one atlas PNG, distinguished by "crop".
                bool file_override = m.value.IsObject() && m.value.HasMember("file") && m.value["file"].IsString();
                if (file_override) {
                    tex_file = folder / m.value["file"].GetString();
                }

                if (!file_override && dirs.is_dir(folder, tex_name)) {
                    auto frames = sorted_frames(dirs, tex_dir);
                    size_t first = files.size();
                    for (const auto& frame : frames) {
                        push_file(frame);
                    }
                    pending.push_back({tex_id, tex_name, &m.value, cache_key, first, frames.size(), true});
                } else if (dirs.exists(tex_file.parent_path(), tex_file.filename())) {
                    size_t first = push_file(tex_file);
                    pending.push_back({tex_id, tex_name, &m.value, cache_key, first, size_t{1}, false});
                } else if (!stash_to) {
                    auto existing = textures.find(tex_id);
                    if (existing != textures.end()) {
                        // Z-DON texture density: this skin's numbers are physical, the ancestor's size is not
                        read_tex_obj_data(m.value, existing->second.get(), tex_scale, 1,
                                          density > 1 ? 1.0f : tex_scale);
                        existing->second->json_path = tex_json;
                        existing->second->json_key = tex_name;
                        existing->second->json_scale = tex_scale;
                        ids_this_call.insert(tex_id);
                        ++loaded_count;
                    } else {
                        spdlog::error("Texture {} was not found in {}", tex_name, folder.string());
                    }
                }
            }

            // images load_screen_textures already decoded, the rest here
            std::vector<Image> images(files.size());
            std::vector<Rect> borders(files.size());
            std::vector<char> has_border(files.size(), 0);
            std::vector<fs::path> rest;
            std::vector<size_t> rest_index;
            std::vector<char> lazy_owned(files.size(), 0); // images now owned by a pending texture
            for (size_t i = 0; i < files.size(); ++i) {
                bool got_border = false;
                if (image_prefetch && image_prefetch->take(files[i], images[i], borders[i], got_border)) {
                    has_border[i] = got_border;
                } else {
                    rest.push_back(files[i]);
                    rest_index.push_back(i);
                }
            }
            // Z-DON: predecoded images (a folder a script loads while its screen starts)
            if (!rest.empty()) {
                Predecoded& pre = predecoded();
                std::scoped_lock lock(pre.mutex);
                if (!pre.images.empty()) {
                    size_t kept = 0;
                    for (size_t k = 0; k < rest.size(); ++k) {
                        auto pit = pre.images.find(ImagePrefetch::image_key(rest[k]));
                        const DirIndex::Entry* e = pit == pre.images.end() ? nullptr : dirs.file_entry(rest[k]);
                        if (e && e->size == pit->second.stat.size && e->mtime == pit->second.stat.mtime) {
                            images[rest_index[k]] = pit->second.image;
                            pre.give_locked(pit->first, pit->second);
                            pre.images.erase(pit);
                            continue;
                        }
                        rest[kept] = rest[k];
                        rest_index[kept] = rest_index[k];
                        ++kept;
                    }
                    rest.resize(kept);
                    rest_index.resize(kept);
                }
            }
            std::vector<Image> decoded;
            decode_images_parallel(rest, decoded);
            for (size_t k = 0; k < rest.size(); ++k) {
                images[rest_index[k]] = decoded[k];
            }

            // the listing's size and time of a file (texture_lazy keeps them for a prefill from the pack)
            auto file_size = [&](size_t fi) {
                const DirIndex::Entry* e = dirs.file_entry(files[fi]);
                return e ? e->size : uint64_t{0};
            };
            auto file_mtime = [&](size_t fi) {
                const DirIndex::Entry* e = dirs.file_entry(files[fi]);
                return e ? e->mtime : uint64_t{0};
            };
            try {
                // GPU textures indexed by file, so keys that share one atlas PNG
                // (via "file" + "crop") upload it once and share the texture.
                std::vector<std::shared_ptr<Texture>> gpu(files.size());
                std::vector<int> file_uses(files.size(), 0);
                for (const auto& p : pending) {
                    for (size_t i = 0; i < p.file_count; ++i) {
                        ++file_uses[p.first_file + i];
                    }
                }
                for (const auto& p : pending) {
                    bool ok = true;
                    // Z-DON: trim_crop_atlas; uploaded here (not left for a first draw: a refill would read the
                    // whole file again)
                    std::optional<std::vector<TextureObject::CropTrim>> trim;
                    if (!p.framed && p.file_count == 1 && density == 1 && file_uses[p.first_file] == 1 &&
                        !gpu[p.first_file]) {
                        trim = trim_crop_atlas(images[p.first_file], *p.mapping, p.name);
                        if (trim) {
                            Texture t = load_texture_from_image(images[p.first_file]);
                            if (is_texture_valid(t)) {
                                texture_lazy::count_eager(screen_name, t);
                                gpu[p.first_file] = shared_texture(t);
                            } else {
                                trim.reset();
                            }
                        }
                    }
                    std::vector<std::shared_ptr<Texture>> texs;
                    texs.reserve(p.file_count);
                    for (size_t i = 0; i < p.file_count; ++i) {
                        size_t fi = p.first_file + i;
                        std::shared_ptr<Texture> tex = gpu[fi];
                        if (!tex) {
                            const Image& img = images[fi];
                            if (!img.data) {
                                spdlog::error("Failed to decode image for texture {}: Frame {}", p.name, i);
                                ok = false;
                            } else if ((tex = texture_lazy::make(img, screen_name, files[fi], screen_loaded,
                                                                 file_size(fi), file_mtime(fi)))) {
                                gpu[fi] = tex;
                                lazy_owned[fi] = 1;
                            } else {
                                Texture t{};
                                // Z-DON: uploaded ahead from the same pixels (start_predecode), else now
                                if (!predecoded().take_gl(ImagePrefetch::image_key(files[fi]), img, t)) {
                                    t = load_texture_from_image(img);
                                }
                                if (!is_texture_valid(t)) {
                                    spdlog::error("Failed to load texture {}: Frame {}", p.name, i);
                                    ok = false;
                                } else {
                                    texture_lazy::count_eager(screen_name, t);
                                    tex = shared_texture(t);
                                    gpu[fi] = tex;
                                }
                            }
                        }
                        texs.push_back(std::move(tex));
                    }
                    if (!ok) {
                        continue;
                    }

                    std::shared_ptr<TextureObject> obj;
                    if (p.framed) {
                        obj = std::make_shared<FramedTexture>(p.id, std::move(texs));
                    } else {
                        obj = std::make_shared<SingleTexture>(p.id, texs[0]);
                    }

                    read_tex_obj_data(*p.mapping, obj.get(), tex_scale, density);
                    if (trim && obj->crop_data && obj->crop_data->size() == trim->size()) {
                        obj->crop_trim = std::move(trim);
                    }
                    obj->json_path = tex_json;
                    obj->json_key = p.name;
                    obj->json_scale = tex_scale;

                    if (!p.framed && !obj->crop_data.has_value() && p.mapping->IsObject() &&
                        !p.mapping->HasMember("x2")) {
                        const Image& img = images[p.first_file];
                        if (img.data) {
                            const Rect border =
                                has_border[p.first_file] ? borders[p.first_file] : get_image_alpha_border(img, 0.0f);
                            if (border.width > 0.0f) {
                                obj->opaque_right = (border.x + border.width) * tex_scale;
                            }
                        }
                    }

                    texture_retain::add(screen_name, p.cache_key, obj);
                    cache[p.cache_key] = obj;
                    if (stash_to) {
                        stash_to->push_back(std::move(obj));
                        continue;
                    }
                    textures[p.id] = obj;
                    ids_this_call.insert(p.id);
                    ++loaded_count;
                }
            } catch (...) {
                for (size_t i = 0; i < images.size(); ++i) {
                    if (!lazy_owned[i]) {
                        texture_retain::free_image_async(images[i]);
                    }
                }
                throw;
            }

            for (size_t i = 0; i < images.size(); ++i) {
                if (!lazy_owned[i]) {
                    texture_retain::free_image_async(images[i]);
                }
            }

            spdlog::debug("Textures loaded from folder: {}", folder.string());

        } catch (const std::exception& e) {
            spdlog::error("Failed to load textures from folder {}: {}", folder.string(), e.what());
        }
    };

    // Farthest ancestor first, child last (later loads overwrite), each ancestor skipping
    // the names already provided by the child or a closer ancestor.
    const fs::path own_dir = graphics_path / screen_name / subset;
    std::vector<fs::path> ancestor_dirs;
    ancestor_dirs.reserve(ancestor_graphics_paths.size());
    for (const fs::path& a : ancestor_graphics_paths) {
        ancestor_dirs.push_back(a / screen_name / subset);
    }
    bool child_has_folder = false;
    const std::vector<std::unordered_set<std::string>> skips =
        subset_skips(dirs, own_dir, ancestor_dirs, child_has_folder);
    bool any_folder = dirs.is_file(own_dir, "texture.json");
    for (size_t i = ancestor_graphics_paths.size(); i-- > 0;) {
        const fs::path dir = ancestor_graphics_paths[i] / screen_name / subset;
        if (dirs.is_file(dir, "texture.json")) {
            any_folder = true;
            load_from_path(dir, ancestor_scales[i], &skips[i], 1);
        }
    }
    if (child_has_folder) {
        // Z-DON texture density: this skin's own texture.json numbers are physical pixels
        load_from_path(own_dir, 1.0f / static_cast<float>(texture_density), nullptr, texture_density);
    }

    if (stash_to) {
        return;
    }
    if (loaded_count == 0) {
        if (!any_folder) {
            // no skin in the chain has the folder at all: a script probing an optional art set (Green's
            // fc_animation asks for game/ending_donderful and falls back to ending_anim), not a broken skin
            spdlog::debug("No textures loaded for {}/{}: no skin in the chain has the folder", screen_name, subset);
        } else {
            spdlog::error("No textures loaded for {}/{}", screen_name, subset);
        }
    } else {
        loaded_subsets.insert(dedup_key);
        subset_loaded_ids[dedup_key] = ids_this_call;
        for (const std::string& id : ids_this_call) {
            ++tex_id_refcount[id];
        }
    }
}

void TextureWrapper::unload_folder(const std::string& screen_name, const std::string& subset) {
    lua_texture_lookup.clear();
    texture_fallbacks.clear();
    language_preferred.clear();
    const std::string subset_key = fs::path(subset).filename().string();
    const std::string dedup_key = screen_name + "/" + subset_key;

    if (!loaded_subsets.contains(dedup_key)) {
        return;
    }

    // A subset name can be shared by several screens (they resolve to the same
    // "subset/name" keys), so only drop a texture once every
    // screen/subset that loaded it has also unloaded it.
    auto& refcount = tex_id_refcount;
    auto ids_it = subset_loaded_ids.find(dedup_key);
    if (ids_it != subset_loaded_ids.end()) {
        for (const std::string& id : ids_it->second) {
            auto rc_it = refcount.find(id);
            if (rc_it == refcount.end()) {
                continue;
            }
            if (--rc_it->second <= 0) {
                textures.erase(id);
                refcount.erase(rc_it);
            }
        }
        subset_loaded_ids.erase(ids_it);
    }

    loaded_subsets.erase(dedup_key);
    spdlog::info("Textures unloaded for folder: {}/{}", screen_name, subset);
}

static Image decode_evicted(const fs::path& source) {
    try {
        return load_image(source, {}, nullptr);
    } catch (const std::exception& e) {
        spdlog::error("Failed to decode image {}: {}", source.string(), e.what());
        return Image{};
    }
}

// Z-DON: gives the evicted textures the screen now holds (its texture map and on-demand stash) their images
// back: from the prefetch when load_screen_textures planned them, else decoded here in parallel
void TextureWrapper::refill_evicted(ImagePrefetch& prefetch, const std::vector<fs::path>& planned) {
    std::map<fs::path, std::vector<const Texture*>> want; // by file
    size_t count = 0;
    std::unordered_set<const Texture*> seen;
    auto collect = [&](const std::shared_ptr<TextureObject>& obj) {
        for (int i = 0; obj && i < obj->frame_count(); ++i) {
            const Texture* t = obj->frame_texture_raw(i);
            if (texture_lazy::evicted(t) && seen.insert(t).second) {
                want[texture_lazy::source(t)].push_back(t);
                ++count;
            }
        }
    };
    for (const auto& [id, obj] : textures) {
        collect(obj);
    }
    for (const auto& obj : ondemand_stash) {
        collect(obj);
    }
    if (want.empty()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    auto give = [](const std::vector<const Texture*>& texs, const Image& img) {
        for (size_t i = 0; i < texs.size(); ++i) {
            texture_lazy::refill(texs[i], i + 1 < texs.size() && img.data ? image_copy(img) : img);
        }
    };
    // in the prefetch's order, so its workers stay ahead of the takes
    size_t from_prefetch = 0;
    for (const fs::path& path : planned) {
        auto it = want.find(path);
        Image img{};
        Rect border{};
        bool has_border = false;
        if (it != want.end() && prefetch.take(path, img, border, has_border)) {
            give(it->second, img);
            from_prefetch += it->second.size();
            want.erase(it);
        }
    }
    std::vector<fs::path> rest;
    rest.reserve(want.size());
    for (const auto& [path, texs] : want) {
        rest.push_back(path);
    }
    std::vector<Image> decoded;
    decode_images_parallel(rest, decoded);
    for (size_t i = 0; i < rest.size(); ++i) {
        give(want[rest[i]], decoded[i]);
    }
    spdlog::info("Lazy texture upload: {} images freed at the last screen change decoded again for {} ({} prefetched, "
                 "{:.1f} ms)",
                 count, screen_loaded, from_prefetch,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

// Z-DON: prefill (texture_lazy.h). On a result screen, the evicted images of the screen likely to come next
// are decoded on background jobs, from that screen's texture pack (one sequential read of the files the
// last load of it stored, at low I/O priority; the loose PNGs would be a few hundred small reads) when
// it holds them, else from the skin. They stay here until a screen loads: the target takes the ones that
// are done, the rest decodes with its load as before; any other screen drops them.
namespace {
    struct Prefill {
        std::string owner;
        std::string skin;
        std::vector<texture_lazy::EvictedFile> files; // whole: set by the first job (under mutex)
        size_t bytes = 0;
        unsigned jobs = 0;
        // Z-DON predecode (start_predecode): every file of owner's texture pack, not only the evicted ones,
        // up to cap bytes of images; a load of a screen in `via` leaves it running for owner
        bool whole = false;
        size_t cap = 0;
        std::atomic<size_t> held{0};
        std::vector<std::string> via;
        std::string mode;                         // whole: the play mode it is for (next_game_mode)
        std::unordered_set<std::string> deferred; // whole: owner's folders that mode leaves out
        // whole, for a screen whose textures wait for their first draw: the files an earlier load drew. A BC7 one
        // not among them is let go of right after the load (texture_lazy::log_screen), so it is not read ahead
        std::optional<std::unordered_set<std::string>> warm;
        size_t wait_max = SIZE_MAX; // with warm: the largest such image that waits (the game screen's rule)
        // whole, for a screen whose textures upload at load (the game screen): the decoded images are also
        // uploaded ahead, on the main thread after the frame's present (predecode_tick); main thread only
        bool upload = false;
        std::vector<texture_lazy::BandUpload> up;
        std::vector<char> up_state; // 0 to do, 1 uploaded, 2 left for the load
        size_t up_pos = 0, up_count = 0, up_bytes = 0, up_frames = 0;
        double up_ms = 0, up_max_ms = 0;
        std::unique_ptr<texture_pack::Session> pack; // set by the first job before the others start
        std::atomic<size_t> next{0};
        std::atomic<bool> taken{false};
        std::chrono::steady_clock::time_point start;
        std::mutex mutex; // guards the rest
        std::vector<Image> images;
        size_t done = 0;
        double finished_ms = 0; // since start, when the last one was decoded
    };

    std::shared_ptr<Prefill>& prefill_slot() {
        static auto* p = new std::shared_ptr<Prefill>(); // never destroyed: jobs may outlive static teardown
        return *p;
    }

    size_t drop_prefill(struct Prefill& pf);

    // the screen a player most likely goes to from screen_name
    const char* prefill_target(const std::string& screen_name) {
        if (screen_name == "result") {
            return "song_select";
        }
        if (screen_name == "dan_result") {
            return "dan_select";
        }
        return nullptr;
    }

    // Z-DON: whether the file is still the one the pack copied (a prefill / predecode reads the pack with the pack's
    // own stats, so a file changed since would come back as its old copy -- and the BC7 cache would then keep those
    // pixels under the new file's stamp). Same units as the folder listings. Unknown counts as changed.
    bool file_unchanged(const fs::path& path, uint64_t size, uint64_t mtime) {
        uint64_t now_size = 0;
        uint64_t now_mtime = 0;
#ifdef _WIN32
        if (!win32_file_stat(path, now_size, now_mtime)) {
            return false;
        }
#elif defined(ZDON_POSIX_LIST_DIR)
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) {
            return false;
        }
#if defined(__APPLE__)
        const struct timespec& t = st.st_mtimespec;
#else
        const struct timespec& t = st.st_mtim;
#endif
        constexpr int64_t kUnixEpochIn1601 = 11644473600; // as zdon_os::list_dir
        now_size = static_cast<uint64_t>(st.st_size);
        now_mtime = static_cast<uint64_t>((static_cast<int64_t>(t.tv_sec) + kUnixEpochIn1601) * 10000000 +
                                          static_cast<int64_t>(t.tv_nsec) / 100);
#else
        std::error_code ec;
        now_size = static_cast<uint64_t>(fs::file_size(path, ec));
        if (ec) {
            return false;
        }
        now_mtime = static_cast<uint64_t>(fs::last_write_time(path, ec).time_since_epoch().count());
        if (ec) {
            return false;
        }
#endif
        return now_size == size && now_mtime == mtime;
    }

    void prefill_work(const std::shared_ptr<Prefill>& pf) {
        // dev hook: ZDON_LAZY_PREFILL_SLOW_MS=<ms> waits that long before each image, so a test can leave the
        // result screen while the prefill still runs
        static const long slow_ms = [] {
            const char* v = std::getenv("ZDON_LAZY_PREFILL_SLOW_MS");
            return v && *v ? std::strtol(v, nullptr, 10) : 0L;
        }();
        while (!pf->taken.load(std::memory_order_acquire)) {
            const size_t i = pf->next.fetch_add(1);
            if (i >= pf->files.size()) {
                return;
            }
            if (slow_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
            }
            if (pf->whole && pf->held.load(std::memory_order_relaxed) >= pf->cap) {
                return; // the RAM cap: the rest decodes with the load
            }
            const fs::path& path = pf->files[i].source;
            Image img{};
            if (!file_unchanged(path, pf->files[i].size, pf->files[i].mtime)) {
                // changed since it was listed / packed: left for the screen's own load, which reads the file
                std::scoped_lock lock(pf->mutex);
                if (!pf->taken.load(std::memory_order_relaxed) && ++pf->done == pf->files.size()) {
                    pf->finished_ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pf->start).count();
                }
                continue;
            }
            try {
                img = load_image(path, ImagePrefetch::image_key(path), pf->pack.get());
            } catch (const std::exception& e) {
                spdlog::error("Lazy texture prefill: failed to decode {}: {}", path.string(), e.what());
            }
            if (img.data) {
                pf->held.fetch_add(image_data_size(img.width, img.height, img.format), std::memory_order_relaxed);
            }
            std::scoped_lock lock(pf->mutex);
            if (pf->taken.load(std::memory_order_relaxed)) {
                if (img.data) {
                    unload_image(img); // the screen loaded meanwhile and decodes this one itself
                }
                return;
            }
            pf->images[i] = img;
            if (++pf->done == pf->files.size()) {
                pf->finished_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pf->start).count();
            }
        }
    }

    void start_prefill(const std::string& screen_name, const std::string& skin) {
        const char* target = prefill_target(screen_name);
        const unsigned jobs_n = texture_lazy::prefill_jobs();
        if (!target || jobs_n == 0) {
            return;
        }
        auto pf = std::make_shared<Prefill>();
        pf->owner = target;
        pf->skin = skin;
        pf->jobs = jobs_n;
        // within the RAM cap for images waiting to be uploaded; the rest decodes at the load as before
        const size_t room = texture_lazy::room_bytes();
        for (texture_lazy::EvictedFile& f : texture_lazy::evicted_files(target)) {
            if (pf->bytes + f.bytes <= room) {
                pf->bytes += f.bytes;
                pf->files.push_back(std::move(f));
            }
        }
        if (pf->files.empty()) {
            return;
        }
        pf->images.assign(pf->files.size(), Image{});
        pf->start = std::chrono::steady_clock::now();
        if (prefill_slot()) {
            drop_prefill(*prefill_slot());
        }
        prefill_slot() = pf;
        spdlog::info("Lazy texture prefill for {}: {} images ({} MB) on {} background jobs", pf->owner,
                     pf->files.size(), pf->bytes >> 20, jobs_n);
        // the first job opens the pack (reading its index is file I/O), then starts the others
        jobs::submit(jobs::Priority::Background, [pf] {
            if (texture_pack::enabled() && !pf->taken.load(std::memory_order_acquire)) {
                std::vector<texture_pack::Session::Planned> planned;
                for (const texture_lazy::EvictedFile& f : pf->files) {
                    if (f.size > 0) {
                        planned.push_back({ImagePrefetch::image_key(f.source), {f.size, f.mtime}});
                    }
                }
                try {
                    // never finished: a prefill only reads the pack, the screen's own loads rewrite it
                    pf->pack =
                        std::make_unique<texture_pack::Session>(texture_pack::pack_path(pf->skin, pf->owner), pf->owner,
                                                                std::move(planned), nullptr, nullptr, true);
                } catch (const std::exception& e) {
                    spdlog::warn("Lazy texture prefill: texture pack for {} not used: {}", pf->owner, e.what());
                }
            }
            for (unsigned j = 1; j < pf->jobs; ++j) {
                jobs::submit(jobs::Priority::Background, [pf] { prefill_work(pf); });
            }
            prefill_work(pf);
        });
    }

    // ZDON_PREDECODE: 0 = off, else the number of background jobs (default 4); ZDON_PREDECODE_MB: the cap
    unsigned predecode_jobs() {
        static const unsigned n = [] {
            const char* v = std::getenv("ZDON_PREDECODE");
            const long k = v && *v ? std::strtol(v, nullptr, 10) : zdon_os::mobile_cap_mb(4, 2);
            return k > 0 ? static_cast<unsigned>(std::min(k, 16L)) : 0U;
        }();
        return n;
    }
    size_t predecode_cap() {
        static const size_t cap = [] {
            const char* v = std::getenv("ZDON_PREDECODE_MB");
            const long mb = v && *v ? std::strtol(v, nullptr, 10) : zdon_os::mobile_cap_mb(512, 128);
            return mb > 0 ? static_cast<size_t>(mb) << 20 : size_t{0};
        }();
        return cap;
    }

    // stops a prefill / predecode and frees what it decoded (jobs still decoding drop theirs)
    size_t drop_prefill(Prefill& pf) {
        for (texture_lazy::BandUpload& st : pf.up) {
            Predecoded::unload_gl(st.id);
            st.id = 0;
        }
        std::scoped_lock lock(pf.mutex);
        pf.taken.store(true, std::memory_order_release);
        for (Image& img : pf.images) {
            if (img.data) {
                texture_retain::free_image_async(img);
                img = Image{};
            }
        }
        return pf.done;
    }

    // a screen starts loading while a predecode runs: its target takes the images, any other screen drops them
    void take_predecode(const std::shared_ptr<Prefill>& pf, const std::string& screen_name) {
        std::vector<std::pair<std::string, Predecoded::Image>> got;
        size_t done = 0, total = 0;
        {
            std::scoped_lock lock(pf->mutex);
            pf->taken.store(true, std::memory_order_release);
            for (size_t i = 0; i < pf->images.size(); ++i) {
                unsigned int gl = 0;
                if (i < pf->up.size()) {
                    if (i < pf->up_state.size() && pf->up_state[i] == 1) {
                        gl = pf->up[i].id;
                    } else {
                        Predecoded::unload_gl(pf->up[i].id); // only some rows in: uploaded whole at the load
                    }
                    pf->up[i].id = 0;
                }
                if (pf->images[i].data) {
                    got.emplace_back(
                        ImagePrefetch::image_key(pf->files[i].source),
                        Predecoded::Image{pf->images[i], {pf->files[i].size, pf->files[i].mtime}, false, gl});
                    pf->images[i] = Image{};
                } else {
                    Predecoded::unload_gl(gl);
                }
            }
            done = pf->done;
            total = pf->files.size();
        }
        if (pf->up_frames > 0) {
            spdlog::info("Texture predecode for {}: {} textures uploaded ahead ({} MB) in {} frames, {:.1f} ms in all, "
                         "longest {:.2f} ms",
                         pf->owner, pf->up_count, pf->up_bytes >> 20, pf->up_frames, pf->up_ms, pf->up_max_ms);
        }
        const double since =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pf->start).count();
        if (screen_name != pf->owner) {
            for (auto& [key, im] : got) {
                texture_retain::free_image_async(im.image);
                Predecoded::unload_gl(im.gl_id);
            }
            spdlog::info("Texture predecode for {}: {} loads instead, {} decoded images dropped", pf->owner,
                         screen_name, done);
            return;
        }
        // textures of the screen whose images were evicted (texture_lazy) take theirs as from a prefill
        std::unordered_map<std::string, fs::path> evicted;
        for (const fs::path& src : texture_lazy::evicted_sources(pf->owner)) {
            evicted.emplace(ImagePrefetch::image_key(src), src);
        }
        std::vector<std::pair<fs::path, Image>> refill;
        size_t bytes = 0;
        Predecoded& pre = predecoded();
        {
            std::scoped_lock lock(pre.mutex);
            pre.clear_locked();
            pre.owner = pf->owner;
            pre.given = pre.given_bytes = 0;
            for (auto& [key, im] : got) {
                bytes += image_data_size(im.image.width, im.image.height, im.image.format);
                if (auto it = evicted.find(key); it != evicted.end()) {
                    refill.emplace_back(it->second, im.image);
                    Predecoded::unload_gl(im.gl_id); // a lazy texture: uploaded on first use
                } else {
                    pre.images.insert_or_assign(key, im);
                }
            }
        }
        const size_t refilled = refill.empty() ? 0 : texture_lazy::refill_from(pf->owner, refill);
        spdlog::info("Texture predecode for {}: {} of {} images ready after {:.0f} ms ({} MB{}), {} evicted textures "
                     "refilled",
                     pf->owner, got.size(), total, since, bytes >> 20,
                     done < total ? ", the rest decodes with the load" : "", refilled);
    }

    // a screen starts loading: screen_name takes what is done if it is the target, otherwise it is dropped
    void take_prefill(const std::string& screen_name) {
        if (prefill_slot() && prefill_slot()->whole && screen_name != prefill_slot()->owner &&
            std::ranges::find(prefill_slot()->via, screen_name) != prefill_slot()->via.end()) {
            return; // a screen on the way to the predecode's target: it keeps running
        }
        std::shared_ptr<Prefill> pf = std::move(prefill_slot());
        prefill_slot().reset();
        if (!pf) {
            return;
        }
        if (pf->whole) {
            take_predecode(pf, screen_name);
            return;
        }
        std::vector<std::pair<fs::path, Image>> got;
        size_t done = 0;
        double finished_ms = 0;
        {
            std::scoped_lock lock(pf->mutex);
            pf->taken.store(true, std::memory_order_release);
            for (size_t i = 0; i < pf->files.size(); ++i) {
                if (pf->images[i].data) {
                    got.emplace_back(pf->files[i].source, pf->images[i]);
                    pf->images[i] = Image{};
                }
            }
            done = pf->done;
            finished_ms = pf->finished_ms;
        }
        const double since =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pf->start).count();
        if (screen_name != pf->owner) {
            for (auto& [path, img] : got) {
                texture_retain::free_image_async(img);
            }
            spdlog::info("Lazy texture prefill for {}: {} loads instead, {} decoded images dropped", pf->owner,
                         screen_name, done);
            return;
        }
        const size_t n = texture_lazy::refill_from(pf->owner, got);
        if (done == pf->files.size()) {
            spdlog::info("Lazy texture prefill for {}: all {} images ready ({} textures; decoded in {:.0f} ms, {:.0f} "
                         "ms ago)",
                         pf->owner, done, n, finished_ms, since - finished_ms);
        } else {
            spdlog::info("Lazy texture prefill for {}: {} of {} images ready after {:.0f} ms ({} textures), the rest "
                         "decodes with the load",
                         pf->owner, done, pf->files.size(), since, n);
        }
    }
} // namespace

std::string next_game_mode(const std::string& showing) {
    if (showing == "SONG_SELECT_2P") {
        return "GAME_2P";
    }
    if (showing == "DAN_SELECT") {
        return "GAME_DAN";
    }
    if (showing == "PRACTICE_SELECT") {
        return "GAME_PRACTICE";
    }
    if (showing == "AI_SELECT") {
        return "AI_GAME";
    }
    if (showing == "BATTLE_SELECT") {
        return "BATTLE_GAME";
    }
    return "GAME";
}

void TextureWrapper::start_predecode(const std::string& target, std::vector<std::string> via) {
    const unsigned jobs_n = predecode_jobs();
    if (jobs_n == 0 || predecode_cap() == 0 || !texture_pack::enabled() || target == screen_loaded) {
        return;
    }
    std::shared_ptr<Prefill>& slot = prefill_slot();
    if (slot && slot->whole && slot->owner == target && !slot->taken.load(std::memory_order_acquire)) {
        slot->via = std::move(via); // already under way
        return;
    }
    if (slot) {
        const size_t n = drop_prefill(*slot);
        spdlog::info("Texture predecode for {}: replaced by {}, {} decoded images dropped", slot->owner, target, n);
        slot.reset();
    }
    auto pf = std::make_shared<Prefill>();
    pf->owner = target;
    pf->skin = skin_root().filename().string();
    pf->jobs = jobs_n;
    pf->whole = true;
    pf->cap = predecode_cap();
    pf->via = std::move(via);
    pf->upload = target == "game"; // the screens whose textures upload at load (texture_lazy: gameplay)
    pf->mode = next_game_mode(global_data.current_screen);
    pf->deferred = deferred_game_subsets(target, pf->mode);
    static const bool read_cold = [] {
        const char* v = std::getenv("ZDON_PREDECODE_COLD");
        return v && *v == '1';
    }();
    // the game screen uploads at load, except what no earlier load drew and is small enough to wait
    // (texture_lazy gameplay_lazy): those wait for their first draw too, and a BC7 one lets its image go
    pf->wait_max = pf->upload ? texture_lazy::gameplay_lazy_max_bytes() : SIZE_MAX;
    if (!read_cold && texture_bc7::enabled() && pf->wait_max > 0) {
        pf->warm = texture_lazy::warm_sources(target);
    }
    pf->start = std::chrono::steady_clock::now();
    slot = pf;
    // the first job reads the pack's index (file I/O), opens the pack, then starts the others
    jobs::submit(jobs::Priority::Background, [pf] {
        if (pf->taken.load(std::memory_order_acquire)) {
            return;
        }
        const fs::path pack_file = texture_pack::pack_path(pf->skin, pf->owner);
        std::vector<texture_pack::Session::Planned> planned;
        try {
            texture_pack::Session index(pack_file, pf->owner, {}, nullptr, nullptr, true);
            planned = index.listed();
        } catch (const std::exception& e) {
            spdlog::warn("Texture predecode for {}: texture pack not read: {}", pf->owner, e.what());
        }
        if (planned.empty()) {
            spdlog::info("Texture predecode for {}: no texture pack yet", pf->owner);
            return;
        }
        std::vector<texture_lazy::EvictedFile> files;
        files.reserve(planned.size());
        size_t skipped = 0;
        size_t cold = 0;
        for (const texture_pack::Session::Planned& p : planned) {
            fs::path file(p.key);
            if (!pf->deferred.empty() && pf->deferred.contains(graphics_subset(file, pf->owner))) {
                ++skipped; // a folder the coming play mode leaves out (deferred_game_subsets)
                continue;
            }
            uint32_t w = 0, h = 0;
            if (pf->warm && !pf->warm->contains(file.generic_string()) &&
                texture_bc7::has_file(ImagePrefetch::image_key(file), p.stat.size, &w, &h) &&
                image_data_size(static_cast<int>(w), static_cast<int>(h), pixel_format::COMPRESSED_BPTC_RGBA) <=
                    pf->wait_max) {
                ++cold; // never drawn before and BC7: the load lets its image go (Nijiiro song select: ~900, 165 MB)
                continue;
            }
            files.push_back({std::move(file), p.stat.size, p.stat.mtime, 0});
        }
        if (skipped > 0) {
            spdlog::info("Texture predecode for {}: {} files of folders {} leaves out not decoded", pf->owner, skipped,
                         pf->mode);
        }
        if (cold > 0) {
            spdlog::info("Texture predecode for {}: {} BC7 files no earlier load drew not read ahead", pf->owner, cold);
        }
        try {
            // never finished: a predecode only reads the pack, the screen's own loads rewrite it
            pf->pack = std::make_unique<texture_pack::Session>(pack_file, pf->owner, std::move(planned), nullptr,
                                                               nullptr, true);
        } catch (const std::exception& e) {
            spdlog::warn("Texture predecode for {}: texture pack not used: {}", pf->owner, e.what());
        }
        {
            std::scoped_lock lock(pf->mutex);
            if (pf->taken.load(std::memory_order_relaxed)) {
                return;
            }
            pf->images.assign(files.size(), Image{});
            pf->files = std::move(files);
        }
        spdlog::info("Texture predecode for {}: {} files of its texture pack on {} background jobs (cap {} MB)",
                     pf->owner, pf->files.size(), pf->jobs, pf->cap >> 20);
        for (unsigned j = 1; j < pf->jobs; ++j) {
            jobs::submit(jobs::Priority::Background, [pf] { prefill_work(pf); });
        }
        prefill_work(pf);
    });
}

void TextureWrapper::cancel_predecode(const std::string& target) {
    std::shared_ptr<Prefill>& slot = prefill_slot();
    if (!slot || !slot->whole || slot->owner != target) {
        return;
    }
    const size_t n = drop_prefill(*slot);
    spdlog::info("Texture predecode for {}: cancelled, {} decoded images dropped", target, n);
    slot.reset();
}

void TextureWrapper::end_predecode() {
    Predecoded& pre = predecoded();
    std::scoped_lock lock(pre.mutex);
    if (pre.owner.empty()) {
        return;
    }
    size_t left_bytes = 0;
    for (const auto& [key, im] : pre.images) {
        left_bytes += image_data_size(im.image.width, im.image.height, im.image.format);
    }
    spdlog::info("Texture predecode for {}: {} images used instead of decoding ({} MB; {} textures uploaded ahead), {} "
                 "not needed ({} MB)",
                 pre.owner, pre.given, pre.given_bytes >> 20, pre.given_gl, pre.images.size(), left_bytes >> 20);
    pre.clear_locked();
    pre.owner.clear();
    pre.given_gl = 0;
}

void TextureWrapper::predecode_tick() {
    std::shared_ptr<Prefill>& slot = prefill_slot();
    if (!slot || !slot->upload || slot->taken.load(std::memory_order_acquire)) {
        return;
    }
    Prefill& pf = *slot;
    // ZDON_PREUPLOAD_MS: the frame time it may take after the present (default 1.0, 0 = no upload ahead)
    static const double budget = [] {
        const char* v = std::getenv("ZDON_PREUPLOAD_MS");
        return v && *v ? std::max(0.0, std::atof(v)) : 1.0;
    }();
    if (budget <= 0) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    bool worked = false;
    while (elapsed() < budget) {
        Image img{};
        {
            std::scoped_lock lock(pf.mutex);
            if (pf.up.size() != pf.images.size()) {
                pf.up.resize(pf.images.size());
                pf.up_state.resize(pf.images.size(), 0);
            }
            if (pf.up_pos >= pf.images.size()) {
                break;
            }
            img = pf.images[pf.up_pos]; // freed only on this thread (drop / take)
        }
        if (!img.data) {
            break; // not decoded yet (they come in order): next frame
        }
        worked = true;
        texture_lazy::BandUpload& st = pf.up[pf.up_pos];
        if (texture_lazy::upload_band(img, st)) {
            pf.up_state[pf.up_pos] = 1;
            ++pf.up_count;
            pf.up_bytes += image_data_size(img.width, img.height, img.format);
            ++pf.up_pos;
        } else if (st.id == 0) {
            pf.up_state[pf.up_pos] = 2; // not bandable: uploaded at the load as before
            ++pf.up_pos;
        }
    }
    if (worked) {
        const double ms = elapsed();
        ++pf.up_frames;
        pf.up_ms += ms;
        pf.up_max_ms = std::max(pf.up_max_ms, ms);
    }
}

// decoded-but-not-yet-uploaded images load_screen_textures may hold ahead of load_folder
constexpr size_t kPrefetchBudgetBytes = size_t{128} << 20;

void TextureWrapper::load_screen_textures(const std::string& screen_name) {
    PROFILE_SCOPE();
    PROFILE_ZONE_TEXT(screen_name);
    using clock = std::chrono::steady_clock;
    const auto t_start = clock::now();
    screen_loaded = screen_name;
    take_prefill(screen_name); // Z-DON: before the evicted files are planned
    const std::vector<fs::path> roots = all_graphics_roots();
    DirIndex dirs;
    dir_index = &dirs;
    struct ResetDirs {
        DirIndex*& p;
        ~ResetDirs() { p = nullptr; }
    } reset_dirs{dir_index};
    std::vector<fs::path> screen_dirs;
    for (const fs::path& root : roots) {
        if (dirs.is_dir(root, screen_name)) {
            screen_dirs.push_back(root / screen_name);
        }
    }

    if (screen_dirs.empty()) {
        spdlog::warn("Textures for Screen {} do not exist", screen_name);
        return;
    }

    load_animations(screen_name);
    const auto t_anim = clock::now();

    // Subsets of the child and every ancestor, each once
    deferred_folders.clear();
    const std::unordered_set<std::string> deferred = deferred_game_subsets(screen_name, global_data.current_screen);
    std::vector<std::string> subsets;
    std::unordered_set<std::string> seen;
    for (const fs::path& dir : screen_dirs) {
        const auto* entries = dirs.list(dir);
        if (!entries) {
            continue;
        }
        for (const DirIndex::Entry& entry : *entries) {
            if (!entry.is_dir) {
                continue;
            }
            std::string name = entry.path.filename().string();
            if (!seen.insert(name).second || loaded_subsets.contains(std::format("{}/{}", screen_name, name))) {
                continue;
            }
            if (deferred.contains(name)) {
                deferred_folders.emplace_back(screen_name, std::move(name));
                continue;
            }
            subsets.push_back(std::move(name));
        }
    }

    // The files load_folder will read for each subset, in the order it reads them (farthest
    // ancestor first, child last), one group per subset: names a nearer skin overrides and
    // textures still loaded (texture_retain) are left out.
    std::vector<std::vector<fs::path>> groups(subsets.size());
    std::unordered_set<std::string> border_keys;
    auto& cache = tex_object_cache();
    auto plan_folder = [&](const fs::path& dir, const std::unordered_set<std::string>* skip,
                           std::vector<fs::path>& out) {
        if (!dirs.is_file(dir, "texture.json")) {
            return;
        }
        try {
            const auto cfg = read_json_shared(dir / "texture.json");
            for (const auto& m : cfg->GetObject()) {
                const std::string name = m.name.GetString();
                if (skip && skip->contains(name)) {
                    continue;
                }
                const std::string cache_key = (dir / name).string();
                if (texture_retain::contains(cache_key)) {
                    continue;
                }
                if (auto cit = cache.find(cache_key); cit != cache.end() && !cit->second.expired()) {
                    continue;
                }
                const bool file_override =
                    m.value.IsObject() && m.value.HasMember("file") && m.value["file"].IsString();
                const fs::path tex_file =
                    dir / (file_override ? std::string(m.value["file"].GetString()) : name + ".png");
                if (!file_override && dirs.is_dir(dir, name)) {
                    for (const fs::path& frame : sorted_frames(dirs, dir / name)) {
                        out.push_back(frame);
                    }
                    continue;
                }
                if (!dirs.exists(tex_file.parent_path(), tex_file.filename())) {
                    continue;
                }
                out.push_back(tex_file);
                if (m.value.IsObject() && !m.value.HasMember("x2") && !m.value.HasMember("crop")) {
                    border_keys.insert(ImagePrefetch::image_key(tex_file));
                }
            }
        } catch (const std::exception&) {
        }
    };
    // false when no skin in the chain has <folder_screen>/<subset>/texture.json
    auto plan_subset = [&](const std::string& folder_screen, const std::string& subset, std::vector<fs::path>& out) {
        const fs::path own_dir = graphics_path / folder_screen / subset;
        std::vector<fs::path> ancestor_dirs;
        bool any = dirs.is_file(own_dir, "texture.json");
        for (const fs::path& a : ancestor_graphics_paths) {
            ancestor_dirs.push_back(a / folder_screen / subset);
            any = any || dirs.is_file(ancestor_dirs.back(), "texture.json");
        }
        bool child_has_folder = false;
        const auto skips = subset_skips(dirs, own_dir, ancestor_dirs, child_has_folder);
        for (size_t i = ancestor_dirs.size(); i-- > 0;) {
            plan_folder(ancestor_dirs[i], &skips[i], out);
        }
        if (child_has_folder) {
            plan_folder(own_dir, nullptr, out);
        }
        return any;
    };
    // the subset folders of every skin in the chain, then their frame folders, listed in parallel first
    {
        std::vector<fs::path> level;
        for (const fs::path& root : roots) {
            for (const std::string& subset : subsets) {
                if (dirs.is_dir(root / screen_name, subset)) {
                    level.push_back(root / screen_name / subset);
                }
            }
        }
        dirs.prefetch(level);
        std::vector<fs::path> frames;
        for (const fs::path& dir : level) {
            if (const auto* entries = dirs.list(dir)) {
                for (const DirIndex::Entry& e : *entries) {
                    if (e.is_dir) {
                        frames.push_back(e.path);
                    }
                }
            }
        }
        dirs.prefetch(frames);
    }
    for (size_t g = 0; g < subsets.size(); ++g) {
        plan_subset(screen_name, subsets[g], groups[g]);
    }

    // Z-DON: the folders a Lua script loaded on demand the last times this screen showed
    // (texture_ondemand.h). They are decoded with the screen's own folders and their texture objects
    // made (lazy upload: pending until first drawn), but only kept in ondemand_stash: `textures` is
    // left as it was, so the script's tex.get_texture still loads the folder when it asks for it, at
    // the same moment as before (it may replace textures the screen already has), now from the stash.
    std::vector<texture_ondemand::Folder> extra;
    const std::string skin_name = skin_root().filename().string();
    for (const texture_ondemand::Folder& f : texture_ondemand::folders(skin_name, screen_name)) {
        if ((f.screen == screen_name && seen.contains(fs::path(f.subset).filename().string())) ||
            has_folder(f.screen, f.subset)) {
            continue;
        }
        std::vector<fs::path> files;
        if (!plan_subset(f.screen, f.subset, files)) {
            spdlog::info("On-demand prefetch for {}: {}/{} no longer exists, dropped", screen_name, f.screen, f.subset);
            texture_ondemand::forget(skin_name, screen_name, f);
            continue;
        }
        groups.push_back(std::move(files));
        extra.push_back(f);
    }

    // Z-DON: images of textures this screen made that were freed at a screen change (texture_lazy evict_all),
    // decoded with the rest; the textures that get them are found after the folders are loaded
    std::vector<fs::path> refill_files = texture_lazy::evicted_sources(screen_name);
    std::ranges::sort(refill_files);
    refill_files.erase(std::ranges::unique(refill_files).begin(), refill_files.end());
    const size_t refill_group = groups.size();
    groups.push_back(refill_files);

    // Z-DON: predecoded images (start_predecode), checked against this load's listing: a file changed or gone
    // since is decoded again as usual; one in a folder not listed yet waits for the load_folder that lists it
    std::unordered_set<std::string> predecoded_keys;
    {
        Predecoded& pre = predecoded();
        std::scoped_lock lock(pre.mutex);
        for (auto it = pre.images.begin(); it != pre.images.end();) {
            const DirIndex::Entry* e = nullptr;
            const int r = dirs.peek_file(fs::path(it->first), e);
            if (r == 0 || (r == 1 && (e->size != it->second.stat.size || e->mtime != it->second.stat.mtime))) {
                texture_retain::free_image_async(it->second.image);
                Predecoded::unload_gl(it->second.gl_id);
                it = pre.images.erase(it);
                continue;
            }
            it->second.checked = r == 1;
            if (r == 1) {
                predecoded_keys.insert(it->first);
            }
            ++it;
        }
    }

    const auto t_plan = clock::now();
    // the screen's texture pack: the planned files' bytes in one file, checked against the listings
    std::unique_ptr<texture_pack::Session> pack;
    if (texture_pack::enabled()) {
        std::vector<texture_pack::Session::Planned> planned;
        for (const auto& group : groups) {
            for (const fs::path& p : group) {
                if (predecoded_keys.contains(ImagePrefetch::image_key(p))) {
                    continue; // not read again (kept in the pack as a current entry)
                }
                if (const DirIndex::Entry* e = dirs.file_entry(p)) {
                    planned.push_back({ImagePrefetch::image_key(p), {e->size, e->mtime}});
                }
            }
        }
        auto peek = [](void* ctx, const std::string& key, texture_pack::FileStat& out) -> int {
            const DirIndex::Entry* e = nullptr;
            const int r = static_cast<DirIndex*>(ctx)->peek_file(fs::path(key), e);
            if (r == 1) {
                out = {e->size, e->mtime};
            }
            return r;
        };
        try {
            pack = std::make_unique<texture_pack::Session>(texture_pack::pack_path(skin_root().filename(), screen_name),
                                                           screen_name, std::move(planned), peek, &dirs);
        } catch (const std::exception& e) {
            spdlog::warn("Texture pack for {} not used: {}", screen_name, e.what());
        }
    }

    {
        ImagePrefetch prefetch(groups, border_keys, kPrefetchBudgetBytes, pack.get());
        image_prefetch = &prefetch;
        struct Reset {
            ImagePrefetch*& p;
            ~Reset() { p = nullptr; }
        } reset{image_prefetch};
        for (size_t g = 0; g < subsets.size(); ++g) {
            load_folder(screen_name, subsets[g]);
            prefetch.finish_group(g);
        }
        stash = &ondemand_stash;
        struct ResetStash {
            std::vector<std::shared_ptr<TextureObject>>*& p;
            ~ResetStash() { p = nullptr; }
        } reset_stash{stash};
        for (size_t e = 0; e < extra.size(); ++e) {
            load_folder(extra[e].screen, extra[e].subset);
            prefetch.finish_group(subsets.size() + e);
        }
        refill_evicted(prefetch, refill_files);
        prefetch.finish_group(refill_group);
    }
    if (pack) {
        pack->finish();
    }
    if (verify_enabled()) {
        spdlog::info("Texture pack verify for {}: {} images checked, {} differ", screen_name,
                     verify_counts.checked.exchange(0), verify_counts.mismatched.exchange(0));
    }

    if (!extra.empty()) {
        std::string list;
        for (const texture_ondemand::Folder& f : extra) {
            list += (list.empty() ? "" : ", ") + f.screen + "/" + f.subset;
        }
        spdlog::info("On-demand prefetch for {}: {} folder(s): {}", screen_name, extra.size(), list);
    }
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    spdlog::info("Screen textures loaded for: {} ({:.1f} ms: animations {:.1f}, listing and plan {:.1f}, "
                 "{} folders listed in {:.1f} ms)",
                 screen_name, ms(t_start, clock::now()), ms(t_start, t_anim), ms(t_anim, t_plan), dirs.listed(),
                 dirs.list_ms);
    texture_retain::log_screen(screen_name);
    texture_lazy::log_screen(screen_name);
    start_prefill(screen_name, skin_root().filename().string());
}

std::vector<std::string> TextureWrapper::language_variants(std::string_view name) const {
    std::vector<std::string> out{std::string(name)};
    if (!global_data.config) {
        return out;
    }
    const std::string& lang = global_data.config->general.language;
    const std::string suffix = "_" + lang;
    if (name.size() <= suffix.size() || !name.ends_with(suffix)) {
        return out;
    }
    const std::string base(name.substr(0, name.size() - suffix.size()));
    for (const char* fb : {"ja", "en"}) { // the cabinet's own order: untranslated rows draw from the Japanese set
        if (lang != fb) {
            out.push_back(base + "_" + fb);
        }
    }
    out.push_back(base); // art with no language variants at all
    return out;
}

std::shared_ptr<TextureObject> TextureWrapper::preferred_variant(std::string_view name) {
    auto opt = options.find(SCO::LANGUAGE_VARIANTS);
    if (opt == options.end() || !opt->second || !global_data.config) {
        return nullptr;
    }
    texture_fallback(name); // resets the caches when the language changed
    if (auto it = language_preferred.find(name); it != language_preferred.end()) {
        return it->second;
    }
    const std::string& lang = global_data.config->general.language;
    std::shared_ptr<TextureObject> found;
    const std::string suffix = "_" + lang;
    if (!name.ends_with(suffix)) {
        std::string variant = std::string(name) + suffix;
        if (auto it = textures.find(variant); it != textures.end()) {
            found = it->second;
        }
    }
    return language_preferred.emplace(std::string(name), std::move(found)).first->second;
}

TextureObject* TextureWrapper::get_texture(std::string_view name) {
    if (auto pref = preferred_variant(name)) {
        return pref.get();
    }
    auto it = textures.find(name);
    if (it != textures.end()) {
        return it->second.get();
    }
    return get_texture_shared(name).get();
}

TextureWrapper::TextureFallback& TextureWrapper::texture_fallback(std::string_view name) {
    static const std::string no_lang;
    const std::string& lang = global_data.config ? global_data.config->general.language : no_lang;
    if (texture_fallbacks_lang != lang) {
        texture_fallbacks.clear();
        language_preferred.clear();
        texture_fallbacks_lang = lang;
    }
    if (auto it = texture_fallbacks.find(name); it != texture_fallbacks.end()) {
        return it->second;
    }
    TextureFallback fb;
    for (const auto& v : language_variants(name)) {
        auto it = textures.find(v);
        if (it != textures.end()) {
            fb.found = it->second;
            break;
        }
    }
    return texture_fallbacks.emplace(std::string(name), std::move(fb)).first->second;
}

std::shared_ptr<TextureObject> TextureWrapper::get_texture_shared(std::string_view name) {
    if (auto pref = preferred_variant(name)) {
        return pref;
    }
    if (auto hit = textures.find(name); hit != textures.end()) {
        return hit->second;
    }
    TextureFallback& fb = texture_fallback(name);
    if (fb.found) {
        return fb.found;
    }
    if (load_deferred(name)) {
        return get_texture_shared(name);
    }
    if (!fb.warned) { // once per name until the textures change (it used to log on every call)
        fb.warned = true;
        spdlog::warn("Texture not found: {}", name);
    }
    auto it = textures.find("kidou/warning");
    return it != textures.end() ? it->second : nullptr;
}

bool TextureWrapper::has_texture(std::string_view name) {
    if (textures.contains(name)) {
        return true;
    }
    if (texture_fallback(name).found != nullptr) {
        return true;
    }
    return load_deferred(name) && has_texture(name);
}

bool TextureWrapper::load_deferred(std::string_view name) {
    const size_t slash = name.find('/');
    if (slash == std::string_view::npos || deferred_folders.empty()) {
        return false;
    }
    const std::string_view subset = name.substr(0, slash);
    auto it = std::ranges::find_if(deferred_folders, [&](const auto& f) { return f.second == subset; });
    if (it == deferred_folders.end()) {
        return false;
    }
    const auto [screen, sub] = *it;
    deferred_folders.erase(it);
    const auto t0 = std::chrono::steady_clock::now();
    load_folder(screen, sub);
    spdlog::info("Texture folder {}/{} left out of {} loaded when first asked for ({:.1f} ms)", screen, sub,
                 global_data.current_screen,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

void TextureWrapper::draw_texture(TextureObject* tex_obj, const DrawTextureParams& params) {
    if (!tex_obj) {
        return;
    }

    const float mirror_x = (params.mirror == Mirror::HORIZONTAL) ? -1.0f : 1.0f;
    const float mirror_y = (params.mirror == Mirror::VERTICAL) ? -1.0f : 1.0f;

    const Color final_color = (params.fade != 1.1f) ? fade(params.color, params.fade) : params.color;

    Rect source_rect;
    const TextureObject::CropTrim* trim = nullptr; // Z-DON: trim_crop_atlas
    if (tex_obj->crop_trim.has_value()) {
        if (params.src.has_value()) { // a caller's rect of the whole image: not in the packed one
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("draw_texture: {} is packed without its empty margins, a source rect is not drawn",
                             tex_obj->name);
            }
            return;
        }
        if (params.frame < 0 || static_cast<size_t>(params.frame) >= tex_obj->crop_trim->size()) {
            return;
        }
        trim = &(*tex_obj->crop_trim)[params.frame];
        if (trim->w <= 0) {
            return; // an empty frame
        }
    }
    if (params.src.has_value()) {
        source_rect = params.src.value();
        if (tex_obj->density != 1) { // a caller's src is logical, like the texture's width / height
            const auto d = static_cast<float>(tex_obj->density);
            source_rect = Rect{source_rect.x * d, source_rect.y * d, source_rect.width * d, source_rect.height * d};
        }
    } else if (tex_obj->crop_data.has_value()) {
        try {
            source_rect = tex_obj->crop_data->at(params.frame);
            source_rect.width -= 1.0f;
            source_rect.height -= 1.0f;
            source_rect.x += 0.5f;
            source_rect.y += 0.5f;
            source_rect.width *= mirror_x;
            source_rect.height *= mirror_y;
        } catch (const std::out_of_range& e) {
            spdlog::error("Frame index out of range for texture {}", tex_obj->name);
            spdlog::error("Frame index: {}, Number of frames: {}", params.frame, tex_obj->crop_data->size());
            throw;
        }
    } else {
        float width = static_cast<float>(tex_obj->width);
        float height = static_cast<float>(tex_obj->height);
        if (tex_obj->density != 1) { // the whole image (width / height are logical, rounded down)
            if (const Texture* raw = tex_obj->frame_texture_raw(params.frame)) {
                width = static_cast<float>(raw->width);
                height = static_cast<float>(raw->height);
            }
        }
        source_rect = Rect{0, 0, width * mirror_x, height * mirror_y};
    }

    if (params.index < 0 || static_cast<size_t>(params.index) >= tex_obj->x.size()) {
        spdlog::error("Draw index {} out of range for texture {}", params.index, tex_obj->name);
        return;
    }

    // Calculate destination rectangle with reduced redundant calculations
    const float base_x = tex_obj->x[params.index];
    const float base_y = tex_obj->y[params.index];

    Rect dest_rect;
    if (params.center) {
        const float x2v = tex_obj->x2[params.index];
        const float y2v = tex_obj->y2[params.index];
        const float half_width = x2v * 0.5f;
        const float half_height = y2v * 0.5f;
        const float scaled_half_width = (x2v * params.scale) * 0.5f;
        const float scaled_half_height = (y2v * params.scale) * 0.5f;

        dest_rect = Rect{base_x + draw_offset_x + half_width - scaled_half_width + params.x,
                         base_y + draw_offset_y + half_height - scaled_half_height + params.y,
                         x2v * params.scale + params.x2, y2v * params.scale + params.y2};
    } else {
        dest_rect = Rect{base_x + draw_offset_x + params.x, base_y + draw_offset_y + params.y,
                         tex_obj->x2[params.index] * params.scale + params.x2,
                         tex_obj->y2[params.index] * params.scale + params.y2};
    }

    if (debug_log_draws) {
        debug_draw_log.push_back({.name = tex_obj->name,
                                  .rect = dest_rect,
                                  .tex_obj = tex_obj,
                                  .index = params.index,
                                  .offset_x = params.x,
                                  .offset_y = params.y,
                                  .scale = params.scale,
                                  .center = params.center,
                                  .origin = params.origin,
                                  .rotation = params.rotation});
        log_lua_site(debug_draw_log.back(), script_lua_state());
    }

    Vec2 origin = params.origin;
    if (trim) {
        // The crop drew source pixel u = c0 + 0.5 + (c - 1) * t / D at t along a destination side D (mirrored:
        // from the far end). Draw the part of the destination whose u falls in the kept rect, with the packed
        // texture's pixels there, rotated about the same point.
        const Rect& c = tex_obj->crop_data->at(params.frame);
        const auto axis = [](float c0, float cn, float k0, float kn, float p0, float dn, bool mirrored, float& t_lo,
                             float& t_hi, float& s0, float& sn) {
            const float per = (cn - 1.0f) / dn; // source pixels per destination unit
            const float first = c0 + 0.5f, last = c0 + cn - 0.5f;
            const float a = c0 + k0, b = a + kn; // the kept rect
            const auto t_of = [&](float u) { return mirrored ? (last - u) / per : (u - first) / per; };
            const auto u_of = [&](float t) { return mirrored ? last - t * per : first + t * per; };
            t_lo = std::max(0.0f, mirrored ? t_of(b) : t_of(a));
            t_hi = std::min(dn, mirrored ? t_of(a) : t_of(b));
            const float u_lo = u_of(t_lo), u_hi = u_of(t_hi);
            s0 = p0 + (std::min(u_lo, u_hi) - a);
            sn = (mirrored ? -1.0f : 1.0f) * std::abs(u_hi - u_lo);
        };
        if (dest_rect.width <= 0.0f || dest_rect.height <= 0.0f || c.width <= 1.0f || c.height <= 1.0f) {
            return;
        }
        float tx0, tx1, ty0, ty1;
        axis(c.x, c.width, trim->ox, trim->w, trim->px, dest_rect.width, mirror_x < 0, tx0, tx1, source_rect.x,
             source_rect.width);
        axis(c.y, c.height, trim->oy, trim->h, trim->py, dest_rect.height, mirror_y < 0, ty0, ty1, source_rect.y,
             source_rect.height);
        if (tx1 <= tx0 || ty1 <= ty0) {
            return;
        }
        dest_rect.width = tx1 - tx0;
        dest_rect.height = ty1 - ty0;
        origin = Vec2{origin.x - tx0, origin.y - ty0};
    }
    const Texture* frame_tex = tex_obj->frame_texture(params.frame);
    if (frame_tex) {
        if (params.blend.has_value()) {
            begin_blend_mode(params.blend.value());
        }
        draw_texture_pro(*frame_tex, source_rect, dest_rect, origin, params.rotation, final_color);
        if (params.blend.has_value()) {
            end_blend_mode();
        }
    }
}

TextureWrapper tex;

TextureWrapper global_tex;
