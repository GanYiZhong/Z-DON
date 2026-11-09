#include "song_info.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "../../libs/global_data.h"
#include "../enums.h"
#include "../song_select/file_navigator/color_utils.h"
#include "zdon_ai.h"

static const Color GENRE_PLATE_TEMPLATE_COLOR{60, 103, 0, 255};

static float skin_outline(const SkinInfo& s) {
    return s.outline >= 0 ? s.outline : 5.0f;
}

SongInfo::SongInfo(const std::string& song_name, const std::string& subtitle, bool show_subtitle, int genre,
                   int song_num, int song_total, const std::string& genre_label)
    : song_name(song_name), genre(genre >= 0 && genre < 9 ? genre : 0) {

    // Z-DON: the skin's AI stage may set its own title size (zdon_ai_song_info.font_size)
    int title_font = tex.get_skin_config(SC::GAME_SONG_INFO)->font_size;
    if (zdon_ai_stage()) {
        if (auto it = tex.skin_config_by_name.find("zdon_ai_song_info");
            it != tex.skin_config_by_name.end() && it->second.font_size > 0) {
            title_font = it->second.font_size;
        }
    }
    song_title = std::make_unique<OutlinedText>(song_name, title_font, Color::WHITE, Color::BLACK, false,
                                                skin_outline(*tex.get_skin_config(SC::GAME_SONG_INFO)));
    if (show_subtitle && !subtitle.empty()) {
        song_subtitle =
            std::make_unique<OutlinedText>(subtitle, tex.get_skin_config(SC::GAME_SONG_INFO_SUBTITLE)->font_size,
                                           Color::WHITE, Color::BLACK, false, 5);
    }
    const SkinInfo* plate_cfg = tex.get_skin_config(SC::SONG_NUM_GAME);
    this->song_num = std::make_unique<SongNum>(song_num, plate_cfg ? plate_cfg->outline : -1.0f);
    if (song_total > 0 && tex.get_skin_config(SC::SONG_NUM_MAX_GAME)) {
        song_max = std::make_unique<SongNum>(song_total, SC::SONG_NUM_MAX_GAME, "song_num_max_game");
    }
    fade = dynamic_cast<FadeAnimation*>(tex.get_animation(3));
    if (!fade) {
        throw std::runtime_error("SongInfo: animation 3 has an unexpected type");
    }

    t_genre = tex.get_texture("song_info/genre");
    t_song_num_plate =
        tex.has_texture("song_info/song_num_plate") ? tex.get_texture("song_info/song_num_plate") : nullptr;

    int font_size = 18 * tex.screen_scale;
    genre_text = std::make_unique<OutlinedText>(genre_label, font_size, Color::WHITE, Color::BLANK, false, 3, 1);
    // Z-DON: the AI stage's genre pill has black-rimmed lettering, a little smaller (arcade capture MBAZD6xA1D8)
    if (zdon_ai_stage()) {
        if (auto it = tex.skin_config_by_name.find("zdon_ai_song_info");
            it != tex.skin_config_by_name.end() && it->second.height > 0.0f) {
            genre_text = std::make_unique<OutlinedText>(genre_label, static_cast<int>(16 * tex.screen_scale),
                                                        Color::WHITE, Color::BLACK, false, 2, 1);
        }
    }

    GenreIndex genre_bucket = this->genre < 8 ? static_cast<GenreIndex>(this->genre + 1) : GenreIndex::DEFAULT;
    auto color_it = DEFAULT_COLORS.find(genre_bucket);
    Color target = (color_it != DEFAULT_COLORS.end() && color_it->second[1].has_value()) ? color_it->second[1].value()
                                                                                         : Color{101, 0, 82, 255};
    // Z-DON: zdon_ai_song_info "height": 1 = the AI stage's genre pill takes the genre's bright colour (the cabinet's
    // orange Namco Original pill) instead of the dark one
    if (zdon_ai_stage()) {
        if (auto it = tex.skin_config_by_name.find("zdon_ai_song_info");
            it != tex.skin_config_by_name.end() && it->second.height > 0.0f && color_it != DEFAULT_COLORS.end() &&
            color_it->second[0].has_value()) {
            target = color_it->second[0].value();
        }
    }

    // Z-DON: the title stays up; only the plate under it swaps the genre and the song number
    if (auto it = tex.skin_config_by_name.find("zdon_song_title_always"); it != tex.skin_config_by_name.end()) {
        keep_title = it->second.x != 0.0f;
    }

    genre_shader = load_shader("shader/dummy.vs", "shader/colortransform.fs");
    genre_shader_loaded = genre_shader.id != 0;
    if (genre_shader_loaded) {
        float src[3] = {GENRE_PLATE_TEMPLATE_COLOR.r / 255.0f, GENRE_PLATE_TEMPLATE_COLOR.g / 255.0f,
                        GENRE_PLATE_TEMPLATE_COLOR.b / 255.0f};
        float tgt[3] = {target.r / 255.0f, target.g / 255.0f, target.b / 255.0f};
        set_shader_value(genre_shader, get_shader_location(genre_shader, "sourceColor"), src, shader_uniform::VEC3);
        set_shader_value(genre_shader, get_shader_location(genre_shader, "targetColor"), tgt, shader_uniform::VEC3);
    }
}

SongInfo::SongInfo(SongInfo&& other) noexcept
    : song_name(std::move(other.song_name)), genre(other.genre), fade(other.fade),
      song_title(std::move(other.song_title)), song_subtitle(std::move(other.song_subtitle)),
      genre_text(std::move(other.genre_text)), song_num(std::move(other.song_num)), song_max(std::move(other.song_max)),
      t_genre(other.t_genre), t_song_num_plate(other.t_song_num_plate), genre_shader(other.genre_shader),
      genre_shader_loaded(other.genre_shader_loaded), keep_title(other.keep_title) {
    other.genre_shader_loaded = false;
}

SongInfo& SongInfo::operator=(SongInfo&& other) noexcept {
    if (this != &other) {
        if (genre_shader_loaded) {
            unload_shader(genre_shader);
        }
        song_name = std::move(other.song_name);
        genre = other.genre;
        fade = other.fade;
        start_ms = other.start_ms;
        now_ms = other.now_ms;
        song_title = std::move(other.song_title);
        song_subtitle = std::move(other.song_subtitle);
        genre_text = std::move(other.genre_text);
        song_num = std::move(other.song_num);
        song_max = std::move(other.song_max);
        t_genre = other.t_genre;
        t_song_num_plate = other.t_song_num_plate;
        genre_shader = other.genre_shader;
        genre_shader_loaded = other.genre_shader_loaded;
        keep_title = other.keep_title;
        other.genre_shader_loaded = false;
    }
    return *this;
}

SongInfo::~SongInfo() {
    if (genre_shader_loaded) {
        unload_shader(genre_shader);
    }
}

void SongInfo::update(double current_ms) {
    if (start_ms < 0.0) {
        start_ms = current_ms;
    }
    now_ms = current_ms;
    if (fade) {
        fade->update(current_ms);
    }
}

void SongInfo::draw() {
    if (!fade) {
        return;
    }
    // Z-DON: skin_config "zdon_ai_song_info" {x, y} moves the whole block in the skin's AI battle stage (the
    // cabinet's AI battle shows the title and the genre / song number plate at the bottom right)
    // "width": 1 = the skin draws its own genre pill and shows the title and the song number in one spot (Green):
    // the engine leaves the pill out and the one fades out before the other fades in. Without it (Nijiiro) the
    // title stays and only the plate under it swaps the genre and the song number, as the cabinet does
    float dx = 0.0f;
    float dy = 0.0f;
    bool on_stage = false;
    bool skin_pill = false;
    if (zdon_ai_stage()) {
        if (auto it = tex.skin_config_by_name.find("zdon_ai_song_info"); it != tex.skin_config_by_name.end()) {
            dx = it->second.x;
            dy = it->second.y;
            on_stage = true;
            skin_pill = it->second.width > 0.0f;
        }
    }
    // Z-DON: skin_config "zdon_ai_song_plate" {x, y} moves the genre / song number plate on the AI stage on its own
    // (the cabinet's plate sits lower under the title than the title's own block offset gives)
    float pdx = dx;
    float pdy = dy;
    if (on_stage) {
        if (auto it = tex.skin_config_by_name.find("zdon_ai_song_plate"); it != tex.skin_config_by_name.end()) {
            pdx += it->second.x;
            pdy += it->second.y;
        }
    }
    auto f = static_cast<float>(fade->attribute);
    // skin_config zdon_green_strict (Green): the cabinet's AI battle swaps the title and the song number every 2 s
    // from the start of the song (direct feed Tl7g8XXXvAM: title 0-2 s, counter 2-4 s, 0.37 s cross-fade), instead
    // of the normal game's 3.3 s hold
    if (const auto strict = tex.skin_config_by_name.find("zdon_green_strict");
        on_stage && skin_pill && strict != tex.skin_config_by_name.end() && strict->second.x != 0.0f) {
        const double p = std::fmod(now_ms - start_ms, 4000.0);
        const double h = 183.0; // half of the cross-fade
        if (now_ms - start_ms < h) {
            f = 0.0f;
        } else if (p < 2000.0 - h) {
            f = p < h ? 1.0f - static_cast<float>((p + h) / (2 * h)) : 0.0f;
        } else if (p < 2000.0 + h) {
            f = static_cast<float>((p - (2000.0 - h)) / (2 * h));
        } else if (p < 4000.0 - h) {
            f = 1.0f;
        } else {
            f = 1.0f - static_cast<float>((p - (4000.0 - h)) / (2 * h));
        }
    }
    float a_num = f;
    float a_title = 1 - f;
    float a_genre = 1 - f;
    if (skin_pill) {
        a_title = std::clamp(1.0f - 2.0f * f, 0.0f, 1.0f);
        a_num = std::clamp(2.0f * f - 1.0f, 0.0f, 1.0f);
    } else if (on_stage || keep_title) {
        a_title = 1.0f;
    }
    zdon_ai_set_title_alpha(a_title);
    float text_x = tex.get_skin_config(SC::GAME_SONG_INFO)->x + dx;
    float text_y = tex.get_skin_config(SC::GAME_SONG_INFO)->y + dy - song_title->height / 2.0f;

    // Optional skin key game_song_info_max_width: a longer title is squeezed horizontally to fit.
    float title_w = song_title->width;
    float title_x2 = 0.0f;
    if (const SkinInfo* m = tex.get_skin_config(SC::GAME_SONG_INFO_MAX_WIDTH);
        m && m->width > 0 && title_w > m->width) {
        title_x2 = m->width - title_w;
        title_w = m->width;
    }

    float title_x = text_x - title_w;
    if (const SkinInfo* c = tex.get_skin_config(SC::GAME_SONG_INFO_CENTER); c && c->width > 0 && title_w <= c->width) {
        title_x = c->x + dx - title_w / 2.0f;
    }

    auto draw_genre = [&](float y_offset) {
        if (!(genre_text && t_genre) || skin_pill) { // the skin's AI stage draws its own pill
            return;
        }
        if (genre_shader_loaded) {
            begin_shader_mode(genre_shader);
        }
        tex.draw_texture(t_genre, {.x = pdx, .y = y_offset + pdy, .fade = a_genre});
        if (genre_shader_loaded) {
            end_shader_mode();
        }
        genre_text->draw({.x = t_genre->x[0] + pdx + t_genre->width / 2.0f - genre_text->width / 2.0f,
                          .y = t_genre->y[0] + y_offset + pdy + t_genre->height / 2.0f - genre_text->height / 2.0f,
                          .fade = a_genre});
    };

    if (const SkinInfo* plate = tex.get_skin_config(SC::SONG_NUM_GAME)) {
        song_title->draw({.x = title_x, .y = text_y, .x2 = title_x2, .fade = a_title});
        draw_genre(0.0f);
        if (t_song_num_plate) {
            tex.draw_texture(t_song_num_plate, {.x = pdx, .y = pdy, .fade = a_num});
        }
        song_num->draw(plate->x + pdx - song_num->width / 2.0f, plate->y + pdy - song_num->height / 2.0f, a_num);
        if (const SkinInfo* m = tex.get_skin_config(SC::SONG_NUM_MAX_GAME); song_max && m) {
            song_max->draw(m->x + pdx - song_max->width / 2.0f, m->y + pdy - song_max->height / 2.0f, a_num);
        }
        return;
    }

    song_num->draw(text_x - song_num->width, text_y, a_num);

    song_title->draw({.x = title_x, .y = text_y, .x2 = title_x2, .fade = a_title});

    if (song_subtitle) {
        float sub_y = tex.get_skin_config(SC::GAME_SONG_INFO_SUBTITLE)->y - song_subtitle->height / 2.0f;
        song_subtitle->draw({.x = text_x - song_subtitle->width, .y = sub_y, .fade = a_title});
    }

    draw_genre(song_subtitle ? song_subtitle->height : 0.0f);
}

static void fill_song_num(std::string& fmt, int value) {
    size_t pos = fmt.find("{0}");
    if (pos != std::string::npos) {
        fmt.replace(pos, 3, std::to_string(value));
    }
}

SongNum::SongNum(int song_num, float outline_override) {
    static const SkinInfo default_info{};
    const SkinInfo* cfg_ptr = font_manager.get_text_info("song_num");
    const SkinInfo& cfg = cfg_ptr ? *cfg_ptr : default_info;

    std::string song_format = cfg.text.empty() ? "{0}" : font_manager.get_text_label("song_num");
    fill_song_num(song_format, song_num);
    Color outline_color;
    if (global_data.config->limits.song_limit > 0 && global_data.config->limits.song_limit == song_num) {
        outline_color = Color::RED;
    } else {
        outline_color = Color::BLACK;
    }
    text = std::make_unique<OutlinedText>(song_format, cfg.font_size, Color::WHITE, outline_color, false,
                                          outline_override >= 0 ? outline_override : skin_outline(cfg));
    width = text->width;
    height = text->height;
}

SongNum::SongNum(int value, SC key, const std::string& config_key) {
    const SkinInfo* cfg = tex.get_skin_config(key);
    if (!cfg) {
        width = 0.0f;
        height = 0.0f;
        return;
    }
    const SkinInfo* text_cfg = font_manager.get_text_info(config_key);
    std::string fmt = text_cfg && !text_cfg->text.empty() ? font_manager.get_text_label(config_key) : "{0}";
    fill_song_num(fmt, value);
    text = std::make_unique<OutlinedText>(fmt, cfg->font_size, Color::WHITE, Color::BLACK, false, skin_outline(*cfg));
    width = text->width;
    height = text->height;
}

void SongNum::draw(float x, float y, float fade) {
    if (!text) {
        return;
    }
    text->draw({.x = x, .y = y, .fade = fade});
}
