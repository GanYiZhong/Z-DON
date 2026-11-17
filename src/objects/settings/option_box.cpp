#include "option_box.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <utility>

#include "../../libs/gfx/camera_utils.h"
#include "../../libs/gfx/qr_texture.h"
#include "../../libs/input/ime_ui.h"
#include "../../libs/input/input.h"
#include "../../libs/math/animation.h"
#include "../song_select/file_navigator/text_layout.h"

static constexpr int OPTION_FONT_SIZE_BASE = 30;
static constexpr float DESC_X_BASE = 450.0f;
static constexpr float DESC_Y_BASE = 270.0f;
static constexpr float DESC_FONT_SIZE_BASE = 25.0f;

// These are laid out for the 1280x720 base canvas; scale with the skin so
// higher-resolution skins don't drift out of alignment with the box art.
static int option_font_size() {
    return static_cast<int>(OPTION_FONT_SIZE_BASE * tex.screen_scale);
}

static std::unique_ptr<FadeAnimation> make_flicker() {
    auto fa = std::make_unique<FadeAnimation>(400.0, 0.0, true, false, 1.0, 0.0, std::nullopt, std::nullopt, 0.0);
    fa->start();
    return fa;
}

BaseOptionBox::BaseOptionBox(const std::string& name, const std::string& description, const std::string& path)
    // merge 0ed42ec: the text stays unwrapped; Z-DON's draw_base wraps it at the skin's settings_description
    // width (upstream's word_wrap here would wrap it twice)
    : description_text(std::move(description)), config_ref(get_config_ref(path)),
      name_text(std::make_unique<OutlinedText>(name, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4)),
      t_overlay(tex.get_texture("background/overlay")), t_title(tex.get_texture("background/title")),
      t_title_highlight(tex.get_texture("background/title_highlight")),
      t_button_on(tex.get_texture("option/button_on")), t_button_off(tex.get_texture("option/button_off")) {}

void BaseOptionBox::draw_base() const {
    tex.draw_texture(t_overlay, {.scale = 0.70f});
    if (is_highlighted) {
        tex.draw_texture(t_title_highlight);
    } else {
        tex.draw_texture(t_title);
    }
    float text_x = t_title->x[0] + (t_title->x2[0] / 2.0f) - (name_text->width / 2.0f);
    float text_y = t_title->y[0] + (t_title->y2[0] / 8.0f);
    // Z-DON: skin_config "settings_option_name" x (, y): the option name's centre (SimpleStyle: its header box is part
    // of the background art, not the title image the name is otherwise centred on)
    if (const auto it = tex.skin_config_by_name.find("settings_option_name");
        it != tex.skin_config_by_name.end() && it->second.x > 0) {
        text_x = it->second.x - (name_text->width / 2.0f);
        if (it->second.y > 0) { // and y: its vertical centre
            text_y = it->second.y - (name_text->height / 2.0f);
        }
    }
    name_text->draw({.x = text_x, .y = text_y});

    // Z-DON: skin_config "settings_description" (x, y, font_size, width) overrides the hard-coded
    // position / size (a pixel font needs its own grid size); the text wraps at the width (default: to
    // the screen's right edge less the left margin's 1/10), between words or between CJK characters
    float desc_font_size = DESC_FONT_SIZE_BASE * tex.screen_scale;
    float desc_x = DESC_X_BASE * tex.screen_scale;
    float desc_y = DESC_Y_BASE * tex.screen_scale;
    float max_w = 0.0f;
    if (const SkinInfo* info = font_manager.get_text_info("settings_description")) {
        if (info->font_size > 0) {
            desc_font_size = static_cast<float>(info->font_size);
        }
        if (info->x > 0) {
            desc_x = info->x;
        }
        if (info->y > 0) {
            desc_y = info->y;
        }
        max_w = info->width;
    }
    if (max_w <= 0) {
        max_w = static_cast<float>(tex.screen_width) - desc_x * 1.1f;
    }
    Font font = font_manager.get_font(description_text, static_cast<int>(desc_font_size));
    if (desc_lines_for != description_text || desc_lines_size != desc_font_size || desc_lines_w != max_w) {
        desc_lines = wrap_description(font, description_text, desc_font_size, max_w);
        desc_lines_for = description_text;
        desc_lines_size = desc_font_size;
        desc_lines_w = max_w;
    }
    const float line_h = std::round(desc_font_size * 1.25f);
    for (size_t i = 0; i < desc_lines.size(); i++) {
        draw_text_ex(font, desc_lines[i].c_str(), {desc_x, desc_y + line_h * static_cast<float>(i)}, desc_font_size, 1,
                     Color::BLACK);
    }
}

// Z-DON: greedy wrap to max_w: break at spaces, or before any character that starts a CJK run
std::vector<std::string> BaseOptionBox::wrap_description(const Font& font, const std::string& text, float size,
                                                         float max_w) {
    std::vector<std::string> lines;
    auto width = [&](const std::string& s) { return measure_text_ex(font, s.c_str(), size, 1).x; };
    if (width(text) <= max_w) {
        lines.push_back(text);
        return lines;
    }
    auto is_cjk = [](uint32_t cp) {
        return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp >= 0xFF00 && cp <= 0xFFEF);
    };
    // split into units: a word with its trailing space, or one CJK character
    std::vector<std::string> units;
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        uint32_t cp = c;
        if (len == 2) {
            cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
        } else if (len == 3) {
            cp = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        } else if (len == 4) {
            cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
        }
        const std::string ch = text.substr(i, len);
        i += len;
        if (is_cjk(cp)) {
            if (!cur.empty()) {
                units.push_back(cur);
                cur.clear();
            }
            units.push_back(ch);
        } else {
            cur += ch;
            if (ch == " ") {
                units.push_back(cur);
                cur.clear();
            }
        }
    }
    if (!cur.empty()) {
        units.push_back(cur);
    }
    std::string line;
    for (const std::string& u : units) {
        if (!line.empty() && width(line + u) > max_w) {
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            lines.push_back(line);
            line.clear();
            if (u == " ") {
                continue;
            }
        }
        line += u;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

void BaseOptionBox::draw_centered(OutlinedText& text, int index) const {
    // Z-DON: a value wider than its button (a long skin name) is scaled down to fit, a small margin each side
    const float box_w = static_cast<float>(t_button_on->x2[index]);
    const float max_w = box_w - 24.0f; // layout units, as box_w
    const float s = (text.width > max_w && max_w > 0.0f) ? max_w / text.width : 1.0f;
    float tx = t_button_on->x[index] + (box_w / 2.0f) - (text.width * s / 2.0f);
    float ty = t_button_on->y[index] + (t_button_on->y2[index] / 2.0f) - (text.height * s / 2.0f);
    text.draw({.scale = s, .x = tx, .y = ty});
}

void BaseOptionBox::draw_value_box(OutlinedText& text, const FadeAnimation& flicker) const {
    tex.draw_texture(t_button_off, {.index = 2});
    if (is_highlighted) {
        tex.draw_texture(t_button_on, {.fade = flicker.attribute, .index = 2});
    }
    draw_centered(text, 2);
}

// Z-DON built-in labels for the rows the code adds (Calibrate, Reset to defaults)
// and for the refused-keybind message. zh is Simplified like the skin templates.
std::string settings_builtin_text(const std::string& key) {
    static const std::map<std::string, std::map<std::string, std::string>> t = {
        {"calibrate_name",
         {{"en", "Audio Offset Calibration"},
          {"ja", "音ズレ調整"},
          {"zh", "音訊延遲校準"},
          {"ko", "오디오 오프셋 보정"}}},
        {"calibrate_desc",
         {{"en", "Hit don to open the calibration screen."},
          {"ja", "ドンで調整画面を開きます。"},
          {"zh", "敲咚開啟校準畫面。"},
          {"ko", "쿵으로 보정 화면을 엽니다."}}},
        {"calibrate_button", {{"en", "Calibrate"}, {"ja", "調整"}, {"zh", "校準"}, {"ko", "보정"}}},
        {"usio_box", {{"en", "USIO"}, {"ja", "USIO"}, {"zh", "USIO"}, {"ko", "USIO"}}},
        {"lane3d_box", {{"en", "3D View"}, {"ja", "3D表示"}, {"zh", "3D 視角"}, {"ko", "3D 시점"}}},
        {"lane3d_enabled", {{"en", "3D View"}, {"ja", "3D表示"}, {"zh", "3D 視角"}, {"ko", "3D 시점"}}},
        {"lane3d_enabled_desc",
         {{"en", "The play screen in layers you can turn: drag with the mouse to turn, wheel to zoom, middle click for "
                 "the next preset, right click to go back to flat."},
          {"ja",
           "演奏画面を重なった層として傾けて見ます。マウスでドラッグして回転、ホイールで拡大、中クリックで次の視点、右"
           "クリックで平面に戻ります。"},
          {"zh", "演奏畫面分成多層、可轉動視角：滑鼠拖曳旋轉、滾輪縮放、中鍵切換預設視角、右鍵回到平面。"},
          {"ko",
           "연주 화면을 겹친 층으로 기울여 봅니다. 마우스 드래그로 회전, 휠로 확대, 가운데 클릭으로 다음 시점, 오른쪽 "
           "클릭으로 평면으로 돌아갑니다."}}},
        {"lane3d_preset", {{"en", "Starting View"}, {"ja", "開始時の視点"}, {"zh", "開始視角"}, {"ko", "시작 시점"}}},
        {"lane3d_preset_desc",
         {{"en", "The view each song starts in (the mouse can still turn it)."},
          {"ja", "各曲の開始時の視点（マウスで変えられます）。"},
          {"zh", "每首歌開始時的視角（之後仍可用滑鼠轉動）。"},
          {"ko", "곡마다 시작할 때의 시점 (마우스로 바꿀 수 있습니다)."}}},
        {"play_aid_box", {{"en", "Play Aids"}, {"ja", "演奏補助"}, {"zh", "演奏輔助"}, {"ko", "연주 보조"}}},
        {"hit_error_bar",
         {{"en", "Hit Error Bar"}, {"ja", "タイミングバー"}, {"zh", "打擊誤差條"}, {"ko", "타이밍 바"}}},
        {"hit_error_bar_desc",
         {{"en", "A bar under the judge circle marking how early (left) or late (right) your last hits were."},
          {"ja", "判定枠の下に、直前の打撃が早い（左）か遅い（右）かを示すバーを表示します。"},
          {"zh", "在判定圈下方顯示一條橫條，標出最近幾下打得太早（左）或太晚（右）。"},
          {"ko", "판정 원 아래에 최근 타격이 빠른지(왼쪽) 느린지(오른쪽) 보여 주는 바를 표시합니다."}}},
        {"remember_difficulty",
         {{"en", "Remember Difficulty per Song"},
          {"ja", "曲ごとに難易度を記憶"},
          {"zh", "每首歌記住難度"},
          {"ko", "곡마다 난이도 기억"}}},
        {"remember_difficulty_desc",
         {{"en", "A song reopens on the difficulty you last played it on (until the game closes)."},
          {"ja", "曲を開くと、前回その曲で遊んだ難易度が選ばれます（ゲームを閉じるまで）。"},
          {"zh", "打開一首歌時，自動選回上次在這首歌玩的難度（直到關閉遊戲）。"},
          {"ko", "곡을 열면 그 곡에서 마지막으로 플레이한 난이도가 선택됩니다 (게임을 닫을 때까지)."}}},
        {"lane3d_on", {{"en", "On"}, {"ja", "オン"}, {"zh", "開啟"}, {"ko", "켜기"}}},
        {"lane3d_off", {{"en", "Off"}, {"ja", "オフ"}, {"zh", "關閉"}, {"ko", "끄기"}}},
        {"lane3d_flat", {{"en", "Flat"}, {"ja", "平面"}, {"zh", "平面"}, {"ko", "평면"}}},
        {"lane3d_tilt", {{"en", "Tilt"}, {"ja", "傾き"}, {"zh", "傾斜"}, {"ko", "기울임"}}},
        {"lane3d_highway", {{"en", "Highway"}, {"ja", "ハイウェイ"}, {"zh", "公路"}, {"ko", "하이웨이"}}},
        {"lane3d_angle", {{"en", "Angle"}, {"ja", "斜め"}, {"zh", "斜角"}, {"ko", "비스듬히"}}},
        {"lane3d_hint",
         {{"en", "3D view: drag to turn, wheel to zoom, middle click: presets, right click: flat"},
          {"ja", "3D表示：ドラッグで回転・ホイールで拡大・中クリックで視点・右クリックで平面"},
          {"zh", "3D 視角：拖曳旋轉、滾輪縮放、中鍵切換視角、右鍵回平面"},
          {"ko", "3D 시점: 드래그로 회전, 휠로 확대, 가운데 클릭 시점, 오른쪽 클릭 평면"}}},
        {"usio_test", {{"en", "USIO Test"}, {"ja", "USIOテスト"}, {"zh", "USIO 測試"}, {"ko", "USIO 테스트"}}},
        {"usio_test_desc",
         {{"en", "Hit don to see the board's drum sensor readings, switches and coin counters."},
          {"ja", "ドンで基板の太鼓センサー値、スイッチ、コインカウンターを表示します。"},
          {"zh", "敲咚查看基板的鼓感應器讀數、按鍵狀態與投幣計數。"},
          {"ko", "쿵으로 보드의 북 센서 값, 스위치, 코인 카운터를 봅니다."}}},
        {"usio_test_button", {{"en", "Open"}, {"ja", "開く"}, {"zh", "開啟"}, {"ko", "열기"}}},
        {"reset_name",
         {{"en", "Reset to Defaults"}, {"ja", "初期設定に戻す"}, {"zh", "恢復預設"}, {"ko", "기본값 복원"}}},
        {"reset_desc",
         {{"en", "Restores every binding in this box. Don, pick Yes with kat, don."},
          {"ja", "この項目のキー設定を初期値に戻します。ドン、カッで「はい」、ドン。"},
          {"zh", "將此項的所有按鍵恢復預設。咚，用咔選「是」，咚。"},
          {"ko", "이 항목의 키 설정을 기본값으로 되돌립니다. 쿵, 딱으로 예, 쿵."}}},
        {"no", {{"en", "No"}, {"ja", "いいえ"}, {"zh", "否"}, {"ko", "아니요"}}},
        {"yes", {{"en", "Yes"}, {"ja", "はい"}, {"zh", "是"}, {"ko", "예"}}},
        {"key_in_use", {{"en", "Already used"}, {"ja", "使用中"}, {"zh", "已被佔用"}, {"ko", "이미 사용 중"}}},
        {"midi_box", {{"en", "MIDI"}, {"ja", "MIDI"}, {"zh", "MIDI"}, {"ko", "MIDI"}}},
        {"midi_device", {{"en", "MIDI Device"}, {"ja", "MIDIデバイス"}, {"zh", "MIDI 裝置"}, {"ko", "MIDI 장치"}}},
        {"midi_device_desc",
         {{"en", "The MIDI drum to play with (electronic drum, MIDI pad). Off: MIDI is not used."},
          {"ja", "演奏に使うMIDI機器（電子ドラム、MIDIパッドなど）。オフ：MIDIを使わない。"},
          {"zh", "用來演奏的 MIDI 裝置（電子鼓、MIDI 打擊墊等）。關閉：不使用 MIDI。"},
          {"ko", "연주에 쓸 MIDI 기기 (전자 드럼, MIDI 패드 등). 끄기: MIDI를 쓰지 않음."}}},
        {"midi_off", {{"en", "Off"}, {"ja", "オフ"}, {"zh", "關閉"}, {"ko", "끄기"}}},
        {"midi_learn", {{"en", "Learn Drum"}, {"ja", "太鼓を覚える"}, {"zh", "學習對應"}, {"ko", "북 학습"}}},
        {"midi_learn_desc",
         {{"en", "Don, then hit Player 1's left kat, left don, right don and right kat on the MIDI drum."},
          {"ja", "ドンのあと、MIDIの太鼓で1Pの左カッ、左ドン、右ドン、右カッの順にたたいてください。"},
          {"zh", "按咚後，依序在 MIDI 鼓上敲 1P 的左咔、左咚、右咚、右咔。"},
          {"ko", "쿵을 누른 뒤 MIDI 북으로 1P의 왼쪽 딱, 왼쪽 쿵, 오른쪽 쿵, 오른쪽 딱 순서로 치세요."}}},
        {"midi_unset", {{"en", "Not set"}, {"ja", "未設定"}, {"zh", "未設定"}, {"ko", "미설정"}}},
        {"midi_mapped", {{"en", "{0} notes"}, {"ja", "{0}音"}, {"zh", "{0} 個音"}, {"ko", "{0}개 음"}}},
        {"midi_hit",
         {{"en", "Hit {0} ({1}/4)"},
          {"ja", "{0}をたたく ({1}/4)"},
          {"zh", "敲{0}（{1}/4）"},
          {"ko", "{0} 치기 ({1}/4)"}}},
        {"midi_left_kat", {{"en", "left kat"}, {"ja", "左カッ"}, {"zh", "左咔"}, {"ko", "왼쪽 딱"}}},
        {"midi_left_don", {{"en", "left don"}, {"ja", "左ドン"}, {"zh", "左咚"}, {"ko", "왼쪽 쿵"}}},
        {"midi_right_don", {{"en", "right don"}, {"ja", "右ドン"}, {"zh", "右咚"}, {"ko", "오른쪽 쿵"}}},
        {"midi_right_kat", {{"en", "right kat"}, {"ja", "右カッ"}, {"zh", "右咔"}, {"ko", "오른쪽 딱"}}},
        {"midi_done", {{"en", "Learned"}, {"ja", "設定しました"}, {"zh", "已設定"}, {"ko", "설정 완료"}}},
        {"midi_no_device",
         {{"en", "No MIDI device"}, {"ja", "MIDI機器なし"}, {"zh", "未連接 MIDI 裝置"}, {"ko", "MIDI 장치 없음"}}},
    };
    const auto& m = t.at(key);
    auto it = m.find(global_data.config->general.language);
    return it != m.end() ? it->second : m.at("en");
}

std::optional<std::vector<int>> default_binding(const std::string& path) {
    // Mirrors the defaults in get_config() (libs/config.cpp).
    static const std::map<std::string, std::vector<int>> d = {
        {"keys/exit_key", {SDL_SCANCODE_Q}},
        {"keys/fullscreen_key", {SDL_SCANCODE_F11}},
        {"keys/borderless_key", {SDL_SCANCODE_F10}},
        {"keys/pause_key", {SDL_SCANCODE_SPACE}},
        {"keys/back_key", {SDL_SCANCODE_ESCAPE}},
        {"keys/restart_key", {SDL_SCANCODE_F1}},
        {"keys_1p/left_kat", {SDL_SCANCODE_D}},
        {"keys_1p/left_don", {SDL_SCANCODE_F}},
        {"keys_1p/right_don", {SDL_SCANCODE_J}},
        {"keys_1p/right_kat", {SDL_SCANCODE_K}},
        {"keys_2p/left_kat", {SDL_SCANCODE_Z}},
        {"keys_2p/left_don", {SDL_SCANCODE_X}},
        {"keys_2p/right_don", {SDL_SCANCODE_C}},
        {"keys_2p/right_kat", {SDL_SCANCODE_V}},
        {"gamepad_1p/left_kat", {10}},
        {"gamepad_1p/left_don", {16}},
        {"gamepad_1p/right_don", {17}},
        {"gamepad_1p/right_kat", {12}},
        {"gamepad_2p/left_kat", {}},
        {"gamepad_2p/left_don", {}},
        {"gamepad_2p/right_don", {}},
        {"gamepad_2p/right_kat", {}},
    };
    auto it = d.find(path);
    if (it == d.end()) {
        return std::nullopt;
    }
    return it->second;
}

// Lock-out guard: a P1 drum role (keyboard or controller) may not take a key
// another P1 drum role already uses, or the menus (kat moves, don confirms)
// could become unreachable. Rebinding a role to its own key is fine.
static bool p1_drum_key_taken(const std::string& path, int key) {
    for (const char* prefix : {"keys_1p/", "gamepad_1p/"}) {
        if (!path.starts_with(prefix)) {
            continue;
        }
        for (const char* role : {"left_kat", "left_don", "right_don", "right_kat"}) {
            std::string other = std::string(prefix) + role;
            if (other == path) {
                continue;
            }
            std::vector<int> keys = get_config_ref(other).get_vec();
            if (std::ranges::find(keys, key) != keys.end()) {
                return true;
            }
        }
    }
    return false;
}

static std::vector<int> load_key_binding(const ConfigRef& ref) {
    if (ref.is_int()) {
        return {ref.get_int()};
    }
    return ref.get_vec();
}

static void save_key_binding(ConfigRef& ref, const std::vector<int>& value) {
    if (ref.is_int()) {
        ref.set_int(value.empty() ? 0 : value[0]);
    } else {
        ref.set_vec(value);
    }
}

BoolOptionBox::BoolOptionBox(const std::string& name, const std::string& description, const std::string& path,
                             const std::string& true_label, const std::string& false_label)
    : BaseOptionBox(name, description, path), value(config_ref.get_bool()),
      on_text(std::make_unique<OutlinedText>(true_label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4)),
      off_text(
          std::make_unique<OutlinedText>(false_label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4)) {}

void BoolOptionBox::confirm() {
    config_ref.set_bool(value);
}

// Nameplate options point at the active account, which AccountOptionBox can swap.
void BoolOptionBox::update(double /*current_time*/) {
    if (!is_highlighted) {
        value = config_ref.get_bool();
    }
}

void BoolOptionBox::move_left() {
    value = false;
}
void BoolOptionBox::move_right() {
    value = true;
}

void BoolOptionBox::draw() {
    draw_base();

    if (!value) {
        tex.draw_texture(t_button_on, {.index = 0});
    } else {
        tex.draw_texture(t_button_off, {.index = 0});
    }
    draw_centered(*off_text, 0);

    if (value) {
        tex.draw_texture(t_button_on, {.index = 1});
    } else {
        tex.draw_texture(t_button_off, {.index = 1});
    }
    draw_centered(*on_text, 1);
}

static std::string int_display(int v) {
    return std::to_string(v);
}

IntOptionBox::IntOptionBox(const std::string& name, const std::string& description, const std::string& path,
                           const std::map<std::string, std::string>& values)
    : BaseOptionBox(name, description, path), value(config_ref.get_int()), value_text(nullptr),
      flicker_fade(make_flicker()) {
    if (!values.empty()) {
        for (auto& [k, v] : values) {
            try {
                value_list.emplace_back(std::stoi(k), v);
            } catch (const std::exception&) {
                continue;
            }
        }

        std::ranges::sort(value_list, [](const auto& a, const auto& b) { return a.first < b.first; });

        if (!value_list.empty()) {
            bool matched = false;
            for (int i = 0; i < std::ssize(value_list); i++) {
                if (value_list[i].first == value) {
                    value_index = i;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                value_index = 0;
                value = value_list[0].first;
            }
        }
    }
    rebuild_text();
}

void IntOptionBox::rebuild_text() {
    std::string label = value_list.empty() ? int_display(value) : value_list[value_index].second;
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void IntOptionBox::confirm() {
    config_ref.set_int(value);
}

void IntOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    // The engine can adjust a value after it is set (e.g. a display rejecting a refresh rate).
    if (!is_highlighted && config_ref.get_int() != value) {
        value = config_ref.get_int();
        for (int i = 0; i < std::ssize(value_list); i++) {
            if (value_list[i].first == value) {
                value_index = i;
            }
        }
        rebuild_text();
    }
}

void IntOptionBox::move_left() {
    if (!value_list.empty()) {
        value_index = std::max(0, value_index - 1);
        value = value_list[value_index].first;
    } else {
        value--;
    }
    rebuild_text();
}

void IntOptionBox::move_right() {
    if (!value_list.empty()) {
        value_index = std::min(static_cast<int>(value_list.size()) - 1, value_index + 1);
        value = value_list[value_index].first;
    } else {
        value++;
    }
    rebuild_text();
}

void IntOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

StrOptionBox::StrOptionBox(const std::string& name, const std::string& description, const std::string& path,
                           const std::map<std::string, std::string>& values)
    : BaseOptionBox(name, description, path), value(config_ref.get_str()), input_string(value), value_text(nullptr),
      flicker_fade(make_flicker()) {
    if (!values.empty()) {
        for (auto& [k, v] : values) {
            value_list.emplace_back(k, v);
        }
        bool matched = false;
        for (int i = 0; i < std::ssize(value_list); i++) {
            if (value_list[i].first == value) {
                value_index = i;
                matched = true;
                break;
            }
        }
        if (!matched) {
            value_index = 0;
            value = value_list[0].first;
        }
    }
    rebuild_text();
}

void StrOptionBox::rebuild_text() {
    std::string label = value_list.empty() ? input_string : value_list[value_index].second;
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void StrOptionBox::confirm() {
    config_ref.set_str(value);
}

bool StrOptionBox::needs_text_input() const {
    return value_list.empty();
}

void StrOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    // Nameplate options point at the active account, which AccountOptionBox can swap.
    if (!is_highlighted && value_list.empty() && config_ref.get_str() != value) {
        value = input_string = config_ref.get_str();
        rebuild_text();
    }
    if (is_highlighted && value_list.empty()) {
        std::string before = input_string;
        TextEditAction action = poll_text_edit(input_string);
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
        if (action == TextEditAction::None && (is_l_don_pressed() || is_r_don_pressed())) {
            action = TextEditAction::Confirm;
        }
#endif
        if (action == TextEditAction::Confirm) {
            value = input_string;
            confirm();
            is_highlighted = false;
            set_keyboard_visible(false);
        }
        if (action == TextEditAction::Cancel) {
            input_string = value;
            is_highlighted = false;
            set_keyboard_visible(false);
        }
        if (input_string != before) {
            rebuild_text();
        }
    }
}

void StrOptionBox::move_left() {
    if (!value_list.empty()) {
        value_index = std::max(0, value_index - 1);
        value = value_list[value_index].first;
        rebuild_text();
    }
}

void StrOptionBox::move_right() {
    if (!value_list.empty()) {
        value_index = std::min(static_cast<int>(value_list.size()) - 1, value_index + 1);
        value = value_list[value_index].first;
        rebuild_text();
    }
}

void StrOptionBox::draw() {
    draw_base();
    if (is_highlighted && value_list.empty()) {
        ime_ui_set_field({static_cast<float>(t_button_on->x[2]), static_cast<float>(t_button_on->y[2]),
                          static_cast<float>(t_button_on->x2[2]), static_cast<float>(t_button_on->y2[2])});
    }

    draw_value_box(*value_text, *flicker_fade);
}

static constexpr float ACCOUNT_CHARA_X_BASE = 520.0f;
static constexpr float ACCOUNT_CHARA_Y_BASE = 565.0f;
static constexpr float ACCOUNT_PLATE_X_BASE = 705.0f;
static constexpr float ACCOUNT_PLATE_Y_BASE = 340.0f;
static constexpr float CODE_BOX_X_OFFSET_BASE = -94.0f; // from the value button
static constexpr float CODE_BOX_Y_OFFSET_BASE = 350.0f;
static constexpr float CODE_BOX_EXTRA_WIDTH_BASE = 188.0f; // fits a 20 digit code

static constexpr float QR_SIZE_BASE = 200.0f;
static constexpr float QR_GAP_BASE = 20.0f;
AccountOptionBox::~AccountOptionBox() {
    if (qr_texture) {
        unload_texture(*qr_texture);
    }
}

AccountOptionBox::AccountOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : BaseOptionBox(name, description, path), slot(path == "network/access_code_2" ? 1 : 0),
      value(config_ref.get_str()), accounts(scores_manager.get_player_ids()),
      masked_text(
          std::make_unique<OutlinedText>("********", option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4)),
      flicker_fade(make_flicker()) {
    auto current = std::ranges::find(accounts, value);
    if (current == accounts.end()) {
        current = accounts.insert(accounts.end(), value);
    }
    account_index = static_cast<int>(std::distance(accounts.begin(), current));
}

PlayerData& AccountOptionBox::slot_data() const {
    return slot == 0 ? scores_manager.player_1_data : scores_manager.player_2_data;
}

void AccountOptionBox::confirm() {
    if (is_highlighted || value == config_ref.get_str()) {
        return; // entering, or nothing picked
    }
    PlayerData& data = slot_data();
    scores_manager.save_player_data(data);
    config_ref.set_str(value);
    (slot == 0 ? scores_manager.player_1 : scores_manager.player_2) = value;
    data = scores_manager.get_player_data(value).value_or(PlayerData{});
    data.player_id = value;
    // A pick made during a card session outlives it; title and save_config would otherwise undo it.
    global_data.card_override[slot] = false;
}

// Both slots sharing an account would overwrite each other's data, so the other slot's account is skipped.
void AccountOptionBox::step(int direction) {
    const auto& network = global_data.config->network;
    const std::string& other = slot == 0 ? network.access_code_2 : network.access_code_1;
    for (int i = account_index + direction; i >= 0 && i < std::ssize(accounts); i += direction) {
        if (accounts[i] != other) {
            account_index = i;
            value = accounts[i];
            chara.reset();
            if (qr_texture) {
                unload_texture(*qr_texture);
                qr_texture.reset();
            }
            return;
        }
    }
}

void AccountOptionBox::move_left() {
    step(-1);
}

void AccountOptionBox::move_right() {
    step(1);
}

void AccountOptionBox::load_preview() {
    // The active account may hold nameplate edits that aren't saved until leaving settings.
    const PlayerData pd =
        value == slot_data().player_id ? slot_data() : scores_manager.get_player_data(value).value_or(PlayerData{});
    value_text =
        std::make_unique<OutlinedText>(pd.username, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
    code_text = std::make_unique<OutlinedText>(value, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
    nameplate =
        std::make_unique<Nameplate>(pd.username, pd.title, PlayerNum::ALL, pd.dan, pd.gold, pd.rainbow, pd.title_bg);
    chara = make_chara_from_player_data(&pd, slot == 1);
    chara->set_don_colors(pd.chara_color_1, pd.chara_color_2, pd.chara_color_3);
    chara->apply_face(pd.chara_face_index);
    chara->set_anim(AnimIndex::DON_ENTRY_LOOP);
}

void AccountOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (!drawn) {
        chara.reset();
    }
    drawn = false;
    if (chara) {
        chara->update(current_time);
        nameplate->update(current_time);
    }
}

void AccountOptionBox::draw() {
    draw_base();
    if (!chara) {
        load_preview();
    }
    drawn = true;
    const float s = tex.screen_scale;

    tex.draw_texture(t_button_off, {.index = 2});
    if (is_highlighted) {
        tex.draw_texture(t_button_on, {.fade = flicker_fade->attribute, .index = 2});
    }
    const float value_width = std::min(value_text->width, t_button_on->x2[2] * 0.9f);
    float tx = t_button_on->x[2] + (t_button_on->x2[2] / 2.0f) - (value_width / 2.0f);
    float ty = t_button_on->y[2] + (t_button_on->y2[2] / 2.0f) - (value_text->height / 2.0f);
    value_text->draw({.x = tx, .y = ty, .x2 = value_width - value_text->width});

    chara->draw(ACCOUNT_CHARA_X_BASE * s, ACCOUNT_CHARA_Y_BASE * s);
    nameplate->draw(ACCOUNT_PLATE_X_BASE * s, ACCOUNT_PLATE_Y_BASE * s);

    const float box_x = t_button_off->x[2] + (CODE_BOX_X_OFFSET_BASE * s);
    const float box_y = t_button_off->y[2] + (CODE_BOX_Y_OFFSET_BASE * s);
    const float box_w = t_button_off->x2[2] + (CODE_BOX_EXTRA_WIDTH_BASE * s);
    const float box_h = t_button_off->y2[2];
    tex.draw_texture(t_button_off, {.x = CODE_BOX_X_OFFSET_BASE * s,
                                    .y = CODE_BOX_Y_OFFSET_BASE * s,
                                    .x2 = CODE_BOX_EXTRA_WIDTH_BASE * s,
                                    .index = 2});
    const Vec2 mouse = get_screen_to_world_2d(mouse_position(), compute_camera2d(tex.screen_width, tex.screen_height));
    const bool hovered = mouse.x >= box_x && mouse.x < box_x + box_w && mouse.y >= box_y && mouse.y < box_y + box_h;
    OutlinedText& code = hovered ? *code_text : *masked_text;
    qr_visible = hovered && !value.empty();
    if (qr_visible) {
        if (!qr_texture) {
            qr_texture = make_qr_texture(access_code_login_url(value));
        }
        const float size = QR_SIZE_BASE * s;
        const float qx = std::min(box_x + box_w + (QR_GAP_BASE * s), tex.screen_width - size);
        qr_rect = {qx, box_y + (box_h / 2.0f) - (size / 2.0f), size, size};
    }
    code.draw({.x = box_x + (box_w / 2.0f) - (code.width / 2.0f), .y = box_y + (box_h / 2.0f) - (code.height / 2.0f)});
}

void AccountOptionBox::draw_overlay() {
    if (!qr_visible || !qr_texture) {
        return;
    }
    qr_visible = false;
    const float src = static_cast<float>(qr_texture->width);
    draw_texture_pro(*qr_texture, {0, 0, src, src}, qr_rect, {0, 0}, 0.0f, Color::WHITE);
}

KeybindOptionBox::KeybindOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : BaseOptionBox(name, description, path), path(path), value_text(nullptr), flicker_fade(make_flicker()) {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeybindOptionBox::rebuild_text() {
    std::string label;
    for (int i = 0; i < std::ssize(value); i++) {
        try {
            label += getKeyString(value[i]);
        } catch (...) {
            label += std::to_string(value[i]);
        }
        if (i + 1 < std::ssize(value)) {
            label += ", ";
        }
    }
    value_text = std::make_unique<OutlinedText>(label.empty() ? "none" : label, option_font_size(), Color::WHITE,
                                                Color::BLACK, false, 4, -4);
}

void KeybindOptionBox::confirm() {
    save_key_binding(config_ref, value);
}

void KeybindOptionBox::reload() {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeybindOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (message_until > 0.0 && current_time >= message_until) {
        message_until = 0.0;
        rebuild_text();
    }
    if (!is_highlighted) {
        capture_armed = false;
        return;
    }
    if (!capture_armed) {
        // Drop keys queued before the press that opened this box.
        while (key_pressed_next() > 0) {
        }
        capture_armed = true;
        return;
    }
    int key = key_pressed_next();
    if (key <= 0) {
        return;
    }
    is_highlighted = false;
    capture_done = true;
    if (p1_drum_key_taken(path, key)) {
        audio.play_sound("kat", VolumePreset::HITSOUND);
        value_text = std::make_unique<OutlinedText>(settings_builtin_text("key_in_use"), option_font_size(),
                                                    Color::WHITE, Color::BLACK, false, 4, -4);
        message_until = current_time + 1500.0;
        return;
    }
    value = {key};
    confirm();
    audio.play_sound("don", VolumePreset::HITSOUND);
    rebuild_text();
}

void KeybindOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

KeyBindControllerOptionBox::KeyBindControllerOptionBox(const std::string& name, const std::string& description,
                                                       const std::string& path)
    : BaseOptionBox(name, description, path), path(path), value_text(nullptr), flicker_fade(make_flicker()) {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeyBindControllerOptionBox::rebuild_text() {
    std::string label;
    for (int i = 0; i < std::ssize(value); i++) {
        label += std::to_string(value[i]);
        if (i + 1 < std::ssize(value)) {
            label += ", ";
        }
    }
    value_text = std::make_unique<OutlinedText>(label.empty() ? "none" : label, option_font_size(), Color::WHITE,
                                                Color::BLACK, false, 4, -4);
}

void KeyBindControllerOptionBox::confirm() {
    save_key_binding(config_ref, value);
}

void KeyBindControllerOptionBox::reload() {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeyBindControllerOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (message_until > 0.0 && current_time >= message_until) {
        message_until = 0.0;
        rebuild_text();
    }
    if (!is_highlighted) {
        capture_armed = false;
        return;
    }
    if (!capture_armed) {
        // Drop the press that opened this box, plus anything buffered
        // before it, so capture starts clean.
        take_gamepad_button_pressed();
        capture_armed = true;
        return;
    }
    {
        int btn = take_gamepad_button_pressed();
        if (btn >= 0) {
            is_highlighted = false;
            capture_done = true;
            if (p1_drum_key_taken(path, btn)) {
                audio.play_sound("kat", VolumePreset::HITSOUND);
                value_text = std::make_unique<OutlinedText>(settings_builtin_text("key_in_use"), option_font_size(),
                                                            Color::WHITE, Color::BLACK, false, 4, -4);
                message_until = current_time + 1500.0;
                return;
            }
            value = {btn};
            confirm();
            audio.play_sound("don", VolumePreset::HITSOUND);
            rebuild_text();
        }
    }
}

void KeyBindControllerOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

FloatOptionBox::FloatOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : BaseOptionBox(name, description, path), value(config_ref.get_float()), value_text(nullptr),
      flicker_fade(make_flicker()) {
    rebuild_text();
}

void FloatOptionBox::rebuild_text() {
    std::string label = std::to_string(static_cast<int>(value * 100)) + "%";
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void FloatOptionBox::confirm() {
    config_ref.set_float(value);
    audio.set_volume_presets(global_data.config->volume); // Z-DON: what is playing follows at once
}

void FloatOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

// Volumes: 0-100 %.
void FloatOptionBox::move_left() {
    value = std::clamp(std::round((value * 100.0f) - 1.0f), 0.0f, 100.0f) / 100.0f;
    rebuild_text();
}

void FloatOptionBox::move_right() {
    value = std::clamp(std::round((value * 100.0f) + 1.0f), 0.0f, 100.0f) / 100.0f;
    rebuild_text();
}

void FloatOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

AudioOffsetOptionBox::AudioOffsetOptionBox(const std::string& name, const std::string& description,
                                           const std::string& path)
    : BaseOptionBox(name, description, path), value(config_ref.get_int()), value_text(nullptr),
      flicker_fade(make_flicker()) {
    rebuild_text();
}

void AudioOffsetOptionBox::rebuild_text() {
    std::string label = (value >= 0 ? "+" : "") + std::to_string(value) + " ms";
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void AudioOffsetOptionBox::confirm() {
    if (is_highlighted) {
        return; // entering -- act on exit only
    }
    config_ref.set_int(value);
}

void AudioOffsetOptionBox::move_left() {
    value -= 1;
    rebuild_text();
}

void AudioOffsetOptionBox::move_right() {
    value += 1;
    rebuild_text();
}

void AudioOffsetOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void AudioOffsetOptionBox::draw() {
    draw_base();
    draw_value_box(*value_text, *flicker_fade);
}

CalibrateOptionBox::CalibrateOptionBox(const std::string& name, const std::string& description, const std::string& path,
                                       Screens calibrate_screen, const std::string& button_key)
    : BaseOptionBox(name, description, path), calibrate_screen(calibrate_screen),
      button_text(std::make_unique<OutlinedText>(settings_builtin_text(button_key), option_font_size(), Color::WHITE,
                                                 Color::BLACK, false, 4, -4)),
      flicker_fade(make_flicker()) {}

void CalibrateOptionBox::confirm() {
    // select_option() highlighted this row: go straight to calibration.
    if (is_highlighted) {
        is_highlighted = false;
        wants_screen_change = true;
    }
}

std::optional<Screens> CalibrateOptionBox::take_screen_change() {
    if (!wants_screen_change) {
        return std::nullopt;
    }
    wants_screen_change = false;
    return calibrate_screen;
}

void CalibrateOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void CalibrateOptionBox::draw() {
    draw_base();
    draw_value_box(*button_text, *flicker_fade);
}

ResetBindingsOptionBox::ResetBindingsOptionBox(const std::string& name, const std::string& description,
                                               std::vector<std::string> paths)
    : BaseOptionBox(name, description, paths.front()), paths(std::move(paths)),
      no_text(std::make_unique<OutlinedText>(settings_builtin_text("no"), option_font_size(), Color::WHITE,
                                             Color::BLACK, false, 4, -4)),
      yes_text(std::make_unique<OutlinedText>(settings_builtin_text("yes"), option_font_size(), Color::WHITE,
                                              Color::BLACK, false, 4, -4)),
      flicker_fade(make_flicker()) {}

void ResetBindingsOptionBox::confirm() {
    if (is_highlighted) {
        yes = false; // entering: the choice starts on No
        return;
    }
    if (!yes) {
        return;
    }
    yes = false;
    for (const std::string& p : paths) {
        if (auto def = default_binding(p)) {
            ConfigRef ref = get_config_ref(p);
            save_key_binding(ref, *def);
        }
    }
    reload_siblings = true;
}

void ResetBindingsOptionBox::move_left() {
    yes = false;
}

void ResetBindingsOptionBox::move_right() {
    yes = true;
}

void ResetBindingsOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void ResetBindingsOptionBox::draw() {
    draw_base();
    tex.draw_texture(t_button_off, {.index = 0});
    tex.draw_texture(t_button_off, {.index = 1});
    if (is_highlighted) {
        tex.draw_texture(t_button_on, {.fade = flicker_fade->attribute, .index = yes ? 1 : 0});
    }
    draw_centered(*no_text, 0);
    draw_centered(*yes_text, 1);
}

// Z-DON: MIDI set-up rows (the MIDI box SettingsScreen adds)
void MidiDeviceOptionBox::confirm() {
    StrOptionBox::confirm();
    if (!is_highlighted) {
        midi_select_device(global_data.config->midi.device);
    }
}

MidiLearnOptionBox::MidiLearnOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : BaseOptionBox(name, description, path), flicker_fade(make_flicker()) {
    set_text(idle_text());
}

MidiLearnOptionBox::~MidiLearnOptionBox() {
    if (step >= 0) {
        midi_set_learning(false);
    }
}

void MidiLearnOptionBox::set_text(const std::string& s) {
    if (value_text && s == shown) {
        return;
    }
    shown = s;
    value_text = std::make_unique<OutlinedText>(s, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

std::string MidiLearnOptionBox::idle_text() const {
    const MidiConfig& m = global_data.config->midi;
    const size_t n = std::min(m.notes.size(), m.buttons.size());
    if (n == 0) {
        return settings_builtin_text("midi_unset");
    }
    std::string s = settings_builtin_text("midi_mapped");
    if (const size_t at = s.find("{0}"); at != std::string::npos) {
        s.replace(at, 3, std::to_string(n));
    }
    return s;
}

void MidiLearnOptionBox::confirm() {
    if (is_highlighted) {
        ensure_midi_listening();
        step = 0;
        midi_set_learning(true);
    } else if (step >= 0) {
        step = -1;
        midi_set_learning(false);
        set_text(idle_text());
    }
}

// The note goes to the role's first controller button; a role without one gets a button no controller role and no
// other MIDI note uses (from 32 down, away from real pads' low numbers). One note per role: the note's and the
// button's earlier pairs are dropped.
void MidiLearnOptionBox::learn(int note) {
    Config& c = *global_data.config;
    const std::array<std::vector<int>*, 4> roles = {&c.gamepad_1p.left_kat, &c.gamepad_1p.left_don,
                                                    &c.gamepad_1p.right_don, &c.gamepad_1p.right_kat};
    std::vector<int>& role = *roles[step];
    int button = role.empty() ? 0 : role.front();
    if (button < 1 || button > 32) {
        auto used = [&](int b) {
            for (const GamepadConfig* g : {&c.gamepad_1p, &c.gamepad_2p}) {
                for (const std::vector<int>* v : {&g->left_kat, &g->left_don, &g->right_don, &g->right_kat}) {
                    if (std::ranges::contains(*v, b)) {
                        return true;
                    }
                }
            }
            return std::ranges::contains(c.midi.buttons, b);
        };
        button = 0;
        for (int b = 32; b >= 1; --b) {
            if (!used(b)) {
                button = b;
                break;
            }
        }
        if (button == 0) {
            return;
        }
        role.insert(role.begin(), button);
    }
    std::vector<int>& notes = c.midi.notes;
    std::vector<int>& buttons = c.midi.buttons;
    const size_t n = std::min(notes.size(), buttons.size());
    notes.resize(n);
    buttons.resize(n);
    for (size_t i = n; i-- > 0;) {
        if (notes[i] == note || buttons[i] == button) {
            notes.erase(notes.begin() + static_cast<std::ptrdiff_t>(i));
            buttons.erase(buttons.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    notes.push_back(note);
    buttons.push_back(button);
    midi_reload_mapping(c.midi);
    spdlog::info("MIDI learn: note {} -> controller button {} (step {})", note, button, step + 1);
}

void MidiLearnOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (!is_highlighted && step >= 0) { // left with the back key
        step = -1;
        midi_set_learning(false);
        set_text(idle_text());
    }
    if (message_until > 0.0 && current_time >= message_until) {
        message_until = 0.0;
        set_text(idle_text());
    }
    if (step < 0) {
        return;
    }
    if (const int note = midi_take_learned_note(); note >= 0) {
        learn(note);
        audio.play_sound("don", VolumePreset::HITSOUND);
        if (++step >= 4) {
            step = -1;
            midi_set_learning(false);
            is_highlighted = false;
            capture_done = true;
            set_text(settings_builtin_text("midi_done"));
            message_until = current_time + 1500.0;
            return;
        }
    }
    static constexpr std::array<const char*, 4> role_keys = {"midi_left_kat", "midi_left_don", "midi_right_don",
                                                             "midi_right_kat"};
    std::string s = settings_builtin_text("midi_hit");
    if (const size_t at = s.find("{0}"); at != std::string::npos) {
        s.replace(at, 3, settings_builtin_text(role_keys[step]));
    }
    if (const size_t at = s.find("{1}"); at != std::string::npos) {
        s.replace(at, 3, std::to_string(step + 1));
    }
    // the value box holds one short line: with no port open, say so instead of the prompt (learning still runs)
    set_text(midi_device_connected() ? s : settings_builtin_text("midi_no_device"));
}

void MidiLearnOptionBox::draw() {
    draw_base();
    draw_value_box(*value_text, *flicker_fade);
}
