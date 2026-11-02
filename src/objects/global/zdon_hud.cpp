#include "zdon_hud.h"

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_map>

#include "../../libs/gfx.h"
#include "../../libs/gfx/texture.h"
#include "../../libs/global_data.h"
#include "../../libs/time.h"

namespace zhud {

    namespace {

        using Langs = std::map<std::string, std::string>;
        const std::map<std::string, Langs>& builtin() {
            static const std::map<std::string, Langs> t = {
                {"bpm", {{"ja", "BPM {0}"}, {"en", "BPM {0}"}, {"zh", "BPM {0}"}, {"ko", "BPM {0}"}}},
                {"branch", {{"ja", "分岐：{0}"}, {"en", "Branch: {0}"}, {"zh", "分歧：{0}"}, {"ko", "분기: {0}"}}},
                {"branch_auto", {{"ja", "自動"}, {"en", "Auto"}, {"zh", "自动"}, {"ko", "자동"}}},
                {"branch_normal", {{"ja", "普通"}, {"en", "Normal"}, {"zh", "普通"}, {"ko", "보통"}}},
                {"branch_expert", {{"ja", "玄人"}, {"en", "Advanced"}, {"zh", "玄人"}, {"ko", "현인"}}},
                {"branch_master", {{"ja", "達人"}, {"en", "Master"}, {"zh", "达人"}, {"ko", "달인"}}},
                {"practice_keys",
                 {{"ja", "数字キー：小節へ　F6／左右ドン同時：分岐　カッ連打：早送り"},
                  {"en", "Number keys: bar   F6 / both dons: branch   quick kats: move faster"},
                  {"zh", "数字键：跳到小节　F6／左右咚同时：分歧　连敲咔：快转"},
                  {"ko", "숫자 키: 마디　F6/양쪽 쿵: 분기　딱 연타: 빨리 이동"}}},
                {"practice_keys_drum",
                 {{"ja", "左右ドン同時：分岐　カッ連打：早送り"},
                  {"en", "Both dons: branch   quick kats: move faster"},
                  {"zh", "左右咚同时：分歧　连敲咔：快转"},
                  {"ko", "양쪽 쿵: 분기　딱 연타: 빨리 이동"}}},
                {"bar_pos",
                 {{"ja", "小節 {0}／{1}"}, {"en", "Bar {0}/{1}"}, {"zh", "小节 {0}／{1}"}, {"ko", "마디 {0}/{1}"}}},
                {"bar_input", {{"ja", "小節 {0}"}, {"en", "Bar {0}"}, {"zh", "小节 {0}"}, {"ko", "마디 {0}"}}},
                {"bar_input_hint",
                 {{"ja", "Enter：移動　Esc：やめる（1〜{0}）"},
                  {"en", "Enter: jump   Esc: cancel (1-{0})"},
                  {"zh", "Enter：跳转　Esc：取消（1～{0}）"},
                  {"ko", "Enter: 이동　Esc: 취소 (1~{0})"}}},
                {"bar_missing",
                 {{"ja", "{0}小節目はありません"},
                  {"en", "There is no bar {0}"},
                  {"zh", "没有第{0}小节"},
                  {"ko", "{0}번째 마디가 없습니다"}}},
                {"loop_to_end",
                 {{"ja", "{0}小節目から最後までくり返し"},
                  {"en", "Looping from bar {0} to the end"},
                  {"zh", "从第{0}小节到曲末循环"},
                  {"ko", "{0}마디부터 끝까지 반복"}}},
                {"hist_title", {{"ja", "判定分布"}, {"en", "Hit timing"}, {"zh", "判定分布"}, {"ko", "판정 분포"}}},
                {"hist_stats",
                 {{"ja", "平均 {0} ms　標準偏差 {1} ms"},
                  {"en", "Mean {0} ms   SD {1} ms"},
                  {"zh", "平均 {0} ms　标准差 {1} ms"},
                  {"ko", "평균 {0} ms　표준편차 {1} ms"}}},
                {"hist_counts",
                 {{"ja", "早 {0}　遅 {1}"},
                  {"en", "Early {0}   Late {1}"},
                  {"zh", "早 {0}　晚 {1}"},
                  {"ko", "빠름 {0}　느림 {1}"}}},
                {"hist_hint",
                 {{"ja", "カッ：判定分布"},
                  {"en", "Kat: hit timing"},
                  {"zh", "咔：判定分布"},
                  {"ko", "딱: 판정 분포"}}},
                {"ghost", {{"ja", "自己ベスト"}, {"en", "PB pace"}, {"zh", "自己最佳"}, {"ko", "자기 최고"}}},
                {"ghost_diff",
                 {{"ja", "自己ベスト {0}"}, {"en", "PB {0}"}, {"zh", "自己最佳 {0}"}, {"ko", "자기 최고 {0}"}}},
                {"fade",
                 {{"ja", "ヒドゥン／サドゥン"}, {"en", "Hidden / Sudden"}, {"zh", "渐隐／突现"}, {"ko", "히든/서든"}}},
                {"fade_hidden",
                 {{"ja", "ヒドゥン {0}%"}, {"en", "Hidden {0}%"}, {"zh", "渐隐 {0}%"}, {"ko", "히든 {0}%"}}},
                {"fade_sudden",
                 {{"ja", "サドゥン {0}%"}, {"en", "Sudden {0}%"}, {"zh", "突现 {0}%"}, {"ko", "서든 {0}%"}}},
                {"equal_speed", {{"ja", "等速"}, {"en", "Constant speed"}, {"zh", "等速"}, {"ko", "등속"}}},
                {"unsaved",
                 {{"ja", "この成績は保存されません（{0}）"},
                  {"en", "This score is not saved ({0})"},
                  {"zh", "本次成绩不保存（{0}）"},
                  {"ko", "이번 기록은 저장되지 않습니다 ({0})"}}},
                {"list_sep", {{"ja", "・"}, {"en", ", "}, {"zh", "、"}, {"ko", ", "}}},
                {"reason_auto", {{"ja", "オート"}, {"en", "Auto"}, {"zh", "自动演奏"}, {"ko", "오토"}}},
                {"reason_replay", {{"ja", "リプレイ"}, {"en", "Replay"}, {"zh", "回放"}, {"ko", "리플레이"}}},
                {"reason_equal_speed", {{"ja", "等速"}, {"en", "Constant speed"}, {"zh", "等速"}, {"ko", "등속"}}},
                {"reason_skip", {{"ja", "スキップ"}, {"en", "Skip"}, {"zh", "跳过"}, {"ko", "스킵"}}},
                {"reason_record", {{"ja", "録画"}, {"en", "Recording"}, {"zh", "录像"}, {"ko", "녹화"}}},
                {"rec_mode", {{"ja", "録画モード"}, {"en", "Recording mode"}, {"zh", "录像模式"}, {"ko", "녹화 모드"}}},
                {"rec_choose",
                 {{"ja", "どちらで録画する？"},
                  {"en", "Record how?"},
                  {"zh", "要怎么录？"},
                  {"ko", "어떻게 녹화할까요?"}}},
                {"rec_auto", {{"ja", "オート"}, {"en", "Auto play"}, {"zh", "自动演奏"}, {"ko", "오토"}}},
                {"rec_nohit", {{"ja", "たたかない"}, {"en", "No hits"}, {"zh", "不敲击"}, {"ko", "치지 않음"}}},
                {"rec_courses",
                 {{"ja", "いっしょに録画するむずかしさ（1つ1レーン）"},
                  {"en", "Courses to record together (one lane each)"},
                  {"zh", "一起录的难度（每个一条轨道）"},
                  {"ko", "함께 녹화할 난이도 (난이도마다 1레인)"}}},
                {"rec_keys_choose",
                 {{"ja", "左右カッ：えらぶ　ドン：録画／むずかしさの切りかえ　もどる：やめる"},
                  {"en", "Kat L/R: move   Don: record / tick a course   Back: cancel"},
                  {"zh", "左右咔：选择　咚：开始录像／勾选难度　返回：取消"},
                  {"ko", "좌우 딱: 선택　쿵: 녹화 / 난이도 선택　뒤로: 취소"}}},
                {"rec_running",
                 {{"ja", "録画中… {0}%"}, {"en", "Recording… {0}%"}, {"zh", "录像中… {0}%"}, {"ko", "녹화 중… {0}%"}}},
                {"rec_running_keys",
                 {{"ja", "カッ：うらで続ける（ほかの曲を予約できる）　もどる：すべて中止"},
                  {"en", "Kat: keep going in the background (queue more songs)   Back: stop all"},
                  {"zh", "咔：后台继续（可再排其他歌）　返回：全部中止"},
                  {"ko", "딱: 백그라운드로 계속 (다른 곡 예약 가능)　뒤로: 모두 중지"}}},
                {"rec_keys_queue",
                 {{"ja", "左右カッ：えらぶ　ドン：予約に追加／むずかしさの切りかえ　もどる：やめる"},
                  {"en", "Kat L/R: move   Don: add to the queue / tick a course   Back: cancel"},
                  {"zh", "左右咔：选择　咚：加入队列／勾选难度　返回：取消"},
                  {"ko", "좌우 딱: 선택　쿵: 대기열에 추가 / 난이도 선택　뒤로: 취소"}}},
                {"rec_eta",
                 {{"ja", "のこり 約{0}"}, {"en", "About {0} left"}, {"zh", "剩余约 {0}"}, {"ko", "남은 시간 약 {0}"}}},
                {"rec_queue_n",
                 {{"ja", "予約 {0}曲"}, {"en", "{0} queued"}, {"zh", "队列 {0} 首"}, {"ko", "대기 {0}곡"}}},
                {"rec_queued",
                 {{"ja", "予約しました（{0}番目）"},
                  {"en", "Queued (#{0})"},
                  {"zh", "已加入队列（第 {0} 个）"},
                  {"ko", "대기열에 추가 ({0}번째)"}}},
                {"rec_bg",
                 {{"ja", "録画中 {0}%  {1}"},
                  {"en", "Recording {0}%  {1}"},
                  {"zh", "录像中 {0}%  {1}"},
                  {"ko", "녹화 중 {0}%  {1}"}}},
                {"rec_bg_keys",
                 {{"ja", "もどる：録画の進みぐあい"},
                  {"en", "Back: recording progress"},
                  {"zh", "返回：查看录像进度"},
                  {"ko", "뒤로: 녹화 진행 상황"}}},
                {"rec_batch_done",
                 {{"ja", "録画おわり：{0}本（失敗 {1}本）"},
                  {"en", "Recordings done: {0} saved ({1} failed)"},
                  {"zh", "录像全部完成：{0} 个（失败 {1} 个）"},
                  {"ko", "녹화 완료: {0}개 (실패 {1}개)"}}},
                {"rec_done", {{"ja", "録画しました"}, {"en", "Saved"}, {"zh", "录像完成"}, {"ko", "녹화 완료"}}},
                {"rec_done_keys",
                 {{"ja", "ドン：フォルダを開く　カッ：もどる"},
                  {"en", "Don: show in folder   Kat: back"},
                  {"zh", "咚：打开文件夹　咔：返回"},
                  {"ko", "쿵: 폴더 열기　딱: 돌아가기"}}},
                {"rec_failed",
                 {{"ja", "録画できませんでした（record.log）"},
                  {"en", "Recording failed (see record.log)"},
                  {"zh", "录像失败（详见 record.log）"},
                  {"ko", "녹화 실패 (record.log 참고)"}}},
                {"rec_failed_keys",
                 {{"ja", "たたいて もどる"}, {"en", "Hit to go back"}, {"zh", "敲击返回"}, {"ko", "쳐서 돌아가기"}}},
                {"search_genre",
                 {{"ja", "ジャンル：{0}"}, {"en", "Genre: {0}"}, {"zh", "类型：{0}"}, {"ko", "장르: {0}"}}},
                {"search_course",
                 {{"ja", "コース：{0}"}, {"en", "Course: {0}"}, {"zh", "难度：{0}"}, {"ko", "코스: {0}"}}},
                {"search_level", {{"ja", "★{0}"}, {"en", "★{0}"}, {"zh", "★{0}"}, {"ko", "★{0}"}}},
                {"search_all", {{"ja", "すべて"}, {"en", "All"}, {"zh", "全部"}, {"ko", "전체"}}},
                {"search_count", {{"ja", "{0}曲"}, {"en", "{0} songs"}, {"zh", "{0}首"}, {"ko", "{0}곡"}}},
                {"search_none",
                 {{"ja", "見つかりません"}, {"en", "No matches"}, {"zh", "没有符合的歌曲"}, {"ko", "결과 없음"}}},
                {"search_keys",
                 {{"ja", "↑↓／ホイール：選ぶ　Enter／クリック：その曲へ　Tab：ジャンル　F3：難易度　PgUp/"
                         "PgDn：★（項目クリックでも切替）"},
                  {"en", "Up/Down/wheel: pick   Enter/click: go   Tab: genre   F3: course   PgUp/PgDn: level (or click "
                         "a filter)"},
                  {"zh", "↑↓／滚轮：选择　Enter／点击：跳到该曲　Tab：类型　F3：难度　PgUp/PgDn：星级（也可点筛选项）"},
                  {"ko", "↑↓/휠: 선택　Enter/클릭: 이동　Tab: 장르　F3: 난이도　PgUp/PgDn: ★ (필터 클릭도 가능)"}}},
                {"course_0", {{"ja", "かんたん"}, {"en", "Easy"}, {"zh", "简单"}, {"ko", "쉬움"}}},
                {"course_1", {{"ja", "ふつう"}, {"en", "Normal"}, {"zh", "普通"}, {"ko", "보통"}}},
                {"course_2", {{"ja", "むずかしい"}, {"en", "Hard"}, {"zh", "困难"}, {"ko", "어려움"}}},
                {"course_3", {{"ja", "おに"}, {"en", "Extreme"}, {"zh", "魔王"}, {"ko", "오니"}}},
                {"course_4", {{"ja", "おに(裏)"}, {"en", "Extreme (Ura)"}, {"zh", "魔王(里)"}, {"ko", "오니(뒷면)"}}},
            };
            return t;
        }

        std::string replace_args(std::string s, const std::string& a0, const std::string& a1) {
            for (auto [tok, val] : {std::pair<const char*, const std::string*>{"{0}", &a0}, {"{1}", &a1}}) {
                for (size_t p = s.find(tok); p != std::string::npos; p = s.find(tok, p + val->size())) {
                    s.replace(p, 3, *val);
                }
            }
            return s;
        }

        std::unordered_map<std::string, std::unique_ptr<OutlinedText>> cache;
        std::string toast_text;
        double toast_at = -1e9;

    } // namespace

    std::string text(const std::string& key) {
        return text(key, "", "");
    }

    std::string text(const std::string& key, const std::string& a0, const std::string& a1) {
        const std::string& lang = global_data.config->general.language;
        std::string s = tex.skin_text("zdon_" + key, lang);
        if (s.empty()) {
            auto it = builtin().find(key);
            if (it == builtin().end()) {
                return key;
            }
            auto l = it->second.find(lang);
            s = l != it->second.end() ? l->second : it->second.at("en");
        }
        return replace_args(s, a0, a1);
    }

    OutlinedText& label(const std::string& s, int size, Color color) {
        const std::string key = s + '\x1f' + std::to_string(size) + '\x1f' + std::to_string(color.r) + ',' +
                                std::to_string(color.g) + ',' + std::to_string(color.b);
        auto it = cache.find(key);
        if (it == cache.end()) {
            if (cache.size() > 512) {
                cache.clear();
            }
            const int px = std::max(1, static_cast<int>(size * tex.screen_scale));
            it = cache
                     .emplace(key, std::make_unique<OutlinedText>(s, px, color, Color::BLACK, false,
                                                                  3.0f * tex.screen_scale))
                     .first;
        }
        return *it->second;
    }

    void panel(float x, float y, float w, float h, float alpha, Color border) {
        const Rect r{x, y, w, h};
        draw_rectangle_rounded(r, 0.18f, 6, fade(Color{16, 18, 26, 255}, alpha));
        draw_rectangle_rounded_lines_ex(r, 0.18f, 6, 2.0f * tex.screen_scale, border);
    }

    void toast(const std::string& s) {
        toast_text = s;
        toast_at = get_current_ms();
    }

    void draw_toast() {
        const double age = get_current_ms() - toast_at;
        if (toast_text.empty() || age < 0 || age > 1600) {
            return;
        }
        const float s = tex.screen_scale;
        OutlinedText& t = label(toast_text, 30, Color::WHITE);
        const float a = age > 1300 ? static_cast<float>((1600 - age) / 300.0) : 1.0f;
        const float w = t.width + 48 * s;
        const float h = t.height + 20 * s;
        const float x = (tex.screen_width - w) / 2.0f;
        const float y = 120 * s;
        panel(x, y, w, h, 0.86f * a, fade(Color::WHITE, 0.6f * a));
        t.draw({.x = x + 24 * s, .y = y + 10 * s, .fade = a});
    }

    std::string course_name(int course) {
        return text("course_" + std::to_string(std::clamp(course, 0, 4)));
    }

} // namespace zhud
