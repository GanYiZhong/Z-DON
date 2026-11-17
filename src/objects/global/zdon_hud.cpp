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
                {"branch_auto", {{"ja", "自動"}, {"en", "Auto"}, {"zh", "自動"}, {"ko", "자동"}}},
                {"branch_normal", {{"ja", "普通"}, {"en", "Normal"}, {"zh", "普通"}, {"ko", "보통"}}},
                {"branch_expert", {{"ja", "玄人"}, {"en", "Advanced"}, {"zh", "玄人"}, {"ko", "현인"}}},
                {"branch_master", {{"ja", "達人"}, {"en", "Master"}, {"zh", "達人"}, {"ko", "달인"}}},
                {"practice_keys",
                 {{"ja", "数字キー：小節へ　M：最後のミスへ　F6／左右ドン同時：分岐"},
                  {"en", "Number keys: bar   M: last miss   F6 / both dons: branch"},
                  {"zh", "數字鍵：跳到小節　M：最後一個失誤　F6／左右咚同時：分歧"},
                  {"ko", "숫자 키: 마디　M: 마지막 미스　F6/양쪽 쿵: 분기"}}},
                {"practice_keys_drum",
                 {{"ja", "左右ドン同時：分岐"},
                  {"en", "Both dons: branch"},
                  {"zh", "左右咚同時：分歧"},
                  {"ko", "양쪽 쿵: 분기"}}},
                {"bar_pos",
                 {{"ja", "小節 {0}／{1}"}, {"en", "Bar {0}/{1}"}, {"zh", "小節 {0}／{1}"}, {"ko", "마디 {0}/{1}"}}},
                {"bar_input", {{"ja", "小節 {0}"}, {"en", "Bar {0}"}, {"zh", "小節 {0}"}, {"ko", "마디 {0}"}}},
                {"bar_input_hint",
                 {{"ja", "Enter：移動　Esc：やめる（1〜{0}）"},
                  {"en", "Enter: jump   Esc: cancel (1-{0})"},
                  {"zh", "Enter：跳轉　Esc：取消（1～{0}）"},
                  {"ko", "Enter: 이동　Esc: 취소 (1~{0})"}}},
                {"bar_missing",
                 {{"ja", "{0}小節目はありません"},
                  {"en", "There is no bar {0}"},
                  {"zh", "沒有第{0}小節"},
                  {"ko", "{0}번째 마디가 없습니다"}}},
                {"miss_bar",
                 {{"ja", "最後のミス：{0}小節目"},
                  {"en", "Last miss: bar {0}"},
                  {"zh", "最後一個失誤：第{0}小節"},
                  {"ko", "마지막 미스: {0}마디"}}},
                {"no_miss",
                 {{"ja", "ミスはありません"}, {"en", "No misses"}, {"zh", "沒有失誤"}, {"ko", "미스가 없습니다"}}},
                {"loop_pass",
                 {{"ja", "{0}回目　良{1} 可{2} 不可{3}"},
                  {"en", "Pass {0}   Good {1} OK {2} Bad {3}"},
                  {"zh", "第{0}輪　良{1} 可{2} 不可{3}"},
                  {"ko", "{0}회차　양{1} 가{2} 불가{3}"}}},
                {"loop_best",
                 {{"ja", "ベスト：{0}回目　良{1} 可{2} 不可{3}"},
                  {"en", "Best: pass {0}   Good {1} OK {2} Bad {3}"},
                  {"zh", "最佳：第{0}輪　良{1} 可{2} 不可{3}"},
                  {"ko", "최고: {0}회차　양{1} 가{2} 불가{3}"}}},
                {"loop_to_end",
                 {{"ja", "{0}小節目から最後までくり返し"},
                  {"en", "Looping from bar {0} to the end"},
                  {"zh", "從第{0}小節到曲末循環"},
                  {"ko", "{0}마디부터 끝까지 반복"}}},
                {"hist_title", {{"ja", "判定分布"}, {"en", "Hit timing"}, {"zh", "判定分佈"}, {"ko", "판정 분포"}}},
                {"hist_stats",
                 {{"ja", "平均 {0} ms　標準偏差 {1} ms"},
                  {"en", "Mean {0} ms   SD {1} ms"},
                  {"zh", "平均 {0} ms　標準差 {1} ms"},
                  {"ko", "평균 {0} ms　표준편차 {1} ms"}}},
                {"hist_counts",
                 {{"ja", "早 {0}　遅 {1}"},
                  {"en", "Early {0}   Late {1}"},
                  {"zh", "早 {0}　晚 {1}"},
                  {"ko", "빠름 {0}　느림 {1}"}}},
                {"hist_trend",
                 {{"ja", "前半→後半（ms）：{0}"},
                  {"en", "Start → end (ms): {0}"},
                  {"zh", "開頭→結尾（ms）：{0}"},
                  {"ko", "처음→끝 (ms): {0}"}}},
                {"hist_hint",
                 {{"ja", "カッ：判定分布"},
                  {"en", "Kat: hit timing"},
                  {"zh", "咔：判定分佈"},
                  {"ko", "딱: 판정 분포"}}},
                {"ghost", {{"ja", "自己ベスト"}, {"en", "PB pace"}, {"zh", "自己最佳"}, {"ko", "자기 최고"}}},
                {"ghost_diff",
                 {{"ja", "自己ベスト {0}"}, {"en", "PB {0}"}, {"zh", "自己最佳 {0}"}, {"ko", "자기 최고 {0}"}}},
                {"fade",
                 {{"ja", "ヒドゥン／サドゥン"}, {"en", "Hidden / Sudden"}, {"zh", "漸隱／突現"}, {"ko", "히든/서든"}}},
                {"fade_hidden",
                 {{"ja", "ヒドゥン {0}%"}, {"en", "Hidden {0}%"}, {"zh", "漸隱 {0}%"}, {"ko", "히든 {0}%"}}},
                {"fade_sudden",
                 {{"ja", "サドゥン {0}%"}, {"en", "Sudden {0}%"}, {"zh", "突現 {0}%"}, {"ko", "서든 {0}%"}}},
                {"equal_speed", {{"ja", "等速"}, {"en", "Constant speed"}, {"zh", "等速"}, {"ko", "등속"}}},
                {"flashlight",
                 {{"ja", "フラッシュライト"}, {"en", "Flashlight"}, {"zh", "手電筒"}, {"ko", "플래시라이트"}}},
                {"sudden_death",
                 {{"ja", "ミスで終了"}, {"en", "Sudden death"}, {"zh", "失誤就結束"}, {"ko", "실수하면 종료"}}},
                {"sudden_death_bad", {{"ja", "不可で終了"}, {"en", "On BAD"}, {"zh", "出不可"}, {"ko", "에구에서"}}},
                {"sudden_death_ok", {{"ja", "可で終了"}, {"en", "On OK"}, {"zh", "出可"}, {"ko", "좋다에서"}}},
                {"online_mode",
                 {{"ja", "オンライン対戦"}, {"en", "Online Battle"}, {"zh", "線上對戰"}, {"ko", "온라인 대전"}}},
                {"online_loading",
                 {{"ja", "記録を読み込み中…"},
                  {"en", "Loading plays…"},
                  {"zh", "讀取紀錄中…"},
                  {"ko", "기록 불러오는 중…"}}},
                {"online_none",
                 {{"ja", "この譜面に挑戦できる記録はまだありません"},
                  {"en", "Nobody to challenge on this chart yet"},
                  {"zh", "這張譜面還沒有可以挑戰的紀錄"},
                  {"ko", "이 보면에 도전할 기록이 아직 없습니다"}}},
                {"online_offline",
                 {{"ja", "サーバーに接続できません"},
                  {"en", "Can't reach the server"},
                  {"zh", "連不上伺服器"},
                  {"ko", "서버에 연결할 수 없습니다"}}},
                {"online_fetching",
                 {{"ja", "対戦の準備中…"},
                  {"en", "Getting the battle ready…"},
                  {"zh", "準備對戰中…"},
                  {"ko", "대전 준비 중…"}}},
                {"online_failed",
                 {{"ja", "記録を読み込めませんでした"},
                  {"en", "Couldn't load that play"},
                  {"zh", "無法讀取這筆紀錄"},
                  {"ko", "기록을 불러올 수 없습니다"}}},
                {"online_list_title",
                 {{"ja", "対戦相手を選ぶ　{0}（{1}）"},
                  {"en", "Pick an opponent: {0} ({1})"},
                  {"zh", "選擇對手　{0}（{1}）"},
                  {"ko", "상대 선택　{0} ({1})"}}},
                {"online_list_hint",
                 {{"ja", "カッ：選ぶ　ドン：挑戦　ESC：戻る"},
                  {"en", "Ka: choose   Don: challenge   ESC: back"},
                  {"zh", "咔：選擇　咚：挑戰　ESC：返回"},
                  {"ko", "딱: 선택　쿵: 도전　ESC: 돌아가기"}}},
                {"online_menu_challenge",
                 {{"ja", "記録に挑戦"}, {"en", "Challenge a play"}, {"zh", "挑戰紀錄"}, {"ko", "기록에 도전"}}},
                {"online_menu_quick",
                 {{"ja", "クイックマッチ"}, {"en", "Quick match"}, {"zh", "自動配對"}, {"ko", "빠른 매칭"}}},
                {"online_menu_create",
                 {{"ja", "ルームを作る"}, {"en", "Create a room"}, {"zh", "開房間"}, {"ko", "방 만들기"}}},
                {"online_menu_join",
                 {{"ja", "ルームに入る"}, {"en", "Join a room"}, {"zh", "加入房間"}, {"ko", "방 참가"}}},
                {"online_menu_hint",
                 {{"ja", "カッ：選ぶ　ドン：決定　ESC：戻る"},
                  {"en", "Ka: choose   Don: OK   ESC: back"},
                  {"zh", "咔：選擇　咚：決定　ESC：返回"},
                  {"ko", "딱: 선택　쿵: 결정　ESC: 돌아가기"}}},
                {"online_join_title",
                 {{"ja", "ルーム番号を入力"},
                  {"en", "Enter the room code"},
                  {"zh", "輸入房間代碼"},
                  {"ko", "방 번호 입력"}}},
                {"online_join_hint",
                 {{"ja", "数字キー：入力　Enter／ドン：参加　ESC：戻る"},
                  {"en", "Number keys: type   Enter / Don: join   ESC: back"},
                  {"zh", "數字鍵：輸入　Enter／咚：加入　ESC：返回"},
                  {"ko", "숫자 키: 입력　Enter/쿵: 참가　ESC: 돌아가기"}}},
                {"online_connecting",
                 {{"ja", "接続中…"}, {"en", "Connecting…"}, {"zh", "連線中…"}, {"ko", "연결 중…"}}},
                {"online_room_title", {{"ja", "ルーム {0}"}, {"en", "Room {0}"}, {"zh", "房間 {0}"}, {"ko", "방 {0}"}}},
                {"online_room_tag",
                 {{"ja", "ルーム {0}　相手：{1}"},
                  {"en", "Room {0}   Opponent: {1}"},
                  {"zh", "房間 {0}　對手：{1}"},
                  {"ko", "방 {0}　상대: {1}"}}},
                {"online_wait_opp",
                 {{"ja", "対戦相手を待っています…"},
                  {"en", "Waiting for an opponent…"},
                  {"zh", "等待對手加入…"},
                  {"ko", "상대를 기다리는 중…"}}},
                {"online_wait_guest",
                 {{"ja", "{0} の準備を待っています…"},
                  {"en", "Waiting for {0} to get ready…"},
                  {"zh", "等待 {0} 準備…"},
                  {"ko", "{0} 님의 준비를 기다리는 중…"}}},
                {"online_wait_start",
                 {{"ja", "まもなく開始…"}, {"en", "Starting soon…"}, {"zh", "即將開始…"}, {"ko", "곧 시작…"}}},
                {"online_pick_song",
                 {{"ja", "曲を選んでください"}, {"en", "Pick a song"}, {"zh", "請選歌"}, {"ko", "곡을 선택하세요"}}},
                {"online_wait_host",
                 {{"ja", "{0} が曲を選んでいます…"},
                  {"en", "{0} is picking a song…"},
                  {"zh", "{0} 正在選歌…"},
                  {"ko", "{0} 님이 곡을 고르는 중…"}}},
                {"online_room_hint",
                 {{"ja", "Y：チャット　R：通報　ESC：ルームを出る"},
                  {"en", "Y: chat   R: report   ESC: leave the room"},
                  {"zh", "Y：聊天　R：檢舉　ESC：離開房間"},
                  {"ko", "Y: 채팅　R: 신고　ESC: 방 나가기"}}},
                {"online_match_score",
                 {{"ja", "スコア {0}（2本先取）"},
                  {"en", "Match {0} (first to two wins)"},
                  {"zh", "比數 {0}（先贏兩首獲勝）"},
                  {"ko", "스코어 {0} (2승 먼저 승리)"}}},
                {"online_match_won",
                 {{"ja", "あなたの勝ち！ {0}"},
                  {"en", "You won the match! {0}"},
                  {"zh", "你贏得這場比賽！ {0}"},
                  {"ko", "매치 승리! {0}"}}},
                {"online_match_lost",
                 {{"ja", "{0} の勝ち {1}"},
                  {"en", "{0} won the match {1}"},
                  {"zh", "{0} 贏得這場比賽 {1}"},
                  {"ko", "{0} 님의 매치 승리 {1}"}}},
                {"online_credit", {{"ja", "信用 {0}"}, {"en", "Credit {0}"}, {"zh", "信用分 {0}"}, {"ko", "신용 {0}"}}},
                {"online_low_credit",
                 {{"ja", "信用が {0} です（{1} 以上が必要）"},
                  {"en", "Your credit is {0} ({1} needed)"},
                  {"zh", "你的信用分是 {0}，需要 {1} 分以上"},
                  {"ko", "신용이 {0}입니다 ({1} 이상 필요)"}}},
                {"online_report_prompt",
                 {{"ja", "{0} を通報（理由）："},
                  {"en", "Report {0} (reason): "},
                  {"zh", "檢舉 {0}（事由）："},
                  {"ko", "{0} 신고 (사유): "}}},
                {"online_report_sent",
                 {{"ja", "通報しました。管理者が確認します"},
                  {"en", "Reported: an admin will look at it"},
                  {"zh", "已送出檢舉，管理員確認後處理"},
                  {"ko", "신고했습니다. 관리자가 확인합니다"}}},
                {"online_report_dup",
                 {{"ja", "このプレイヤーは今日すでに通報しました"},
                  {"en", "You already reported this player today"},
                  {"zh", "今天已經檢舉過這位玩家了"},
                  {"ko", "오늘 이미 이 플레이어를 신고했습니다"}}},
                {"online_report_reason",
                 {{"ja", "理由を書いてください"},
                  {"en", "Write a reason"},
                  {"zh", "請寫下檢舉事由"},
                  {"ko", "사유를 적어 주세요"}}},
                {"online_report_none",
                 {{"ja", "通報できる相手がいません"},
                  {"en", "No opponent to report"},
                  {"zh", "沒有可以檢舉的對手"},
                  {"ko", "신고할 상대가 없습니다"}}},
                {"online_chat_prompt", {{"ja", "チャット："}, {"en", "Say: "}, {"zh", "聊天："}, {"ko", "채팅: "}}},
                {"online_chat_too_fast",
                 {{"ja", "メッセージが早すぎます"},
                  {"en", "Too fast: wait a moment"},
                  {"zh", "訊息太快了，稍等一下"},
                  {"ko", "너무 빠릅니다. 잠시 기다리세요"}}},
                {"online_guest_title",
                 {{"ja", "{0} が選んだ曲"}, {"en", "{0} picked"}, {"zh", "{0} 選的歌"}, {"ko", "{0} 님이 고른 곡"}}},
                {"online_guest_hint",
                 {{"ja", "カッ：難易度　ドン：準備OK　ESC：ルームを出る"},
                  {"en", "Ka: course   Don: ready   ESC: leave"},
                  {"zh", "咔：難度　咚：準備好了　ESC：離開房間"},
                  {"ko", "딱: 난이도　쿵: 준비 완료　ESC: 나가기"}}},
                {"online_notice_hint", {{"ja", "ドン：OK"}, {"en", "Don: OK"}, {"zh", "咚：確定"}, {"ko", "쿵: 확인"}}},
                {"online_room_gone",
                 {{"ja", "ルームがなくなりました"},
                  {"en", "The room is gone"},
                  {"zh", "房間已經不存在了"},
                  {"ko", "방이 사라졌습니다"}}},
                {"online_opp_gone",
                 {{"ja", "{0} が退出しました"},
                  {"en", "{0} left"},
                  {"zh", "{0} 離開了"},
                  {"ko", "{0} 님이 나갔습니다"}}},
                {"online_no_song",
                 {{"ja", "{0} にこの曲を送れませんでした。別の曲を選んでください"},
                  {"en", "{0} couldn't get that song. Pick another"},
                  {"zh", "{0} 無法取得這首歌，請換一首"},
                  {"ko", "{0} 님에게 이 곡을 보내지 못했습니다. 다른 곡을 고르세요"}}},
                {"online_guest_choose",
                 {{"ja", "{0} が「{1}」を選びました：難易度とオプションを選んでください（Y：チャット）"},
                  {"en", "{0} picked \"{1}\": choose your course and options (Y: chat)"},
                  {"zh", "{0} 選了「{1}」：請選難度和選項（Y：聊天）"},
                  {"ko", "{0} 님이 「{1}」을(를) 골랐습니다: 난이도와 옵션을 고르세요 (Y: 채팅)"}}},
                {"online_guest_wrong_song",
                 {{"ja", "「{0}」を選んでください"},
                  {"en", "Pick \"{0}\""},
                  {"zh", "請選「{0}」"},
                  {"ko", "「{0}」을(를) 고르세요"}}},
                {"online_transfer_send",
                 {{"ja", "{0} に曲を送っています…"},
                  {"en", "Sending the song to {0}…"},
                  {"zh", "正在把歌傳給 {0}…"},
                  {"ko", "{0} 님에게 곡을 보내는 중…"}}},
                {"online_transfer_wait",
                 {{"ja", "{0} から曲を受け取っています…"},
                  {"en", "Getting the song from {0}…"},
                  {"zh", "正在從 {0} 取得歌曲…"},
                  {"ko", "{0} 님에게서 곡을 받는 중…"}}},
                {"online_transfer_failed",
                 {{"ja", "曲を受け取れませんでした"},
                  {"en", "The song couldn't be sent"},
                  {"zh", "歌曲傳送失敗"},
                  {"ko", "곡을 받지 못했습니다"}}},
                {"online_transfer_bad",
                 {{"ja", "受け取った「{0}」が相手の譜面と一致しません"},
                  {"en", "The \"{0}\" received doesn't match the host's chart"},
                  {"zh", "收到的「{0}」和房主的譜面不一致"},
                  {"ko", "받은 「{0}」이(가) 방장의 보면과 다릅니다"}}},
                {"online_missing_song",
                 {{"ja", "「{0}」がこのライブラリにありません"},
                  {"en", "\"{0}\" isn't in your library"},
                  {"zh", "你的歌單裡沒有「{0}」"},
                  {"ko", "「{0}」이(가) 라이브러리에 없습니다"}}},
                {"online_need_account",
                 {{"ja", "ゲームのアカウントでログインしてください"},
                  {"en", "Log in with a game account first"},
                  {"zh", "請先用遊戲帳號登入"},
                  {"ko", "게임 계정으로 먼저 로그인하세요"}}},
                {"online_no_room",
                 {{"ja", "そのルームはありません"},
                  {"en", "No room with that code"},
                  {"zh", "沒有這個房間"},
                  {"ko", "그 방은 없습니다"}}},
                {"online_room_full",
                 {{"ja", "そのルームは満員です"},
                  {"en", "That room is full"},
                  {"zh", "房間已滿"},
                  {"ko", "방이 가득 찼습니다"}}},
                {"online_surrender_prompt",
                 {{"ja", "この曲を降参しますか？　ドン：降参　カッ：やめる"},
                  {"en", "Give this song up?   Don: surrender   Ka: no"},
                  {"zh", "投降這一首？　咚：投降　咔：取消"},
                  {"ko", "이 곡을 항복할까요?　쿵: 항복　딱: 취소"}}},
                {"online_you_surrendered",
                 {{"ja", "降参しました"}, {"en", "You surrendered"}, {"zh", "你投降了"}, {"ko", "항복했습니다"}}},
                {"online_opp_surrendered",
                 {{"ja", "相手が降参しました"},
                  {"en", "The opponent surrendered"},
                  {"zh", "對手投降了"},
                  {"ko", "상대가 항복했습니다"}}},
                {"online_opp_left",
                 {{"ja", "相手が退出しました"},
                  {"en", "The opponent left"},
                  {"zh", "對手已離線"},
                  {"ko", "상대가 나갔습니다"}}},
                {"online_opp_waiting",
                 {{"ja", "相手の開始を待っています…"},
                  {"en", "Waiting for the opponent to start…"},
                  {"zh", "等待對手開始…"},
                  {"ko", "상대의 시작을 기다리는 중…"}}},
                {"online_wait_finish",
                 {{"ja", "相手の終了を待っています…"},
                  {"en", "Waiting for the opponent to finish…"},
                  {"zh", "等待對手完成…"},
                  {"ko", "상대가 끝나기를 기다리는 중…"}}},
                {"online_win", {{"ja", "勝利！"}, {"en", "You win!"}, {"zh", "你贏了！"}, {"ko", "승리!"}}},
                {"online_lose", {{"ja", "敗北"}, {"en", "You lose"}, {"zh", "你輸了"}, {"ko", "패배"}}},
                {"online_draw", {{"ja", "引き分け"}, {"en", "Draw"}, {"zh", "平手"}, {"ko", "무승부"}}},
                {"online_vs", {{"ja", "相手：{0}"}, {"en", "Opponent: {0}"}, {"zh", "對手：{0}"}, {"ko", "상대: {0}"}}},
                {"update_available",
                 {{"ja", "新しいバージョンがあります　F9 で更新"},
                  {"en", "Update available — press F9"},
                  {"zh", "有新版本　按 F9 更新"},
                  {"ko", "새 버전이 있습니다　F9로 업데이트"}}},
                {"update_failed",
                 {{"ja", "アップデーターを起動できません"},
                  {"en", "Could not start the updater"},
                  {"zh", "無法啟動更新程式"},
                  {"ko", "업데이터를 시작할 수 없습니다"}}},
                {"sudden_death_out",
                 {{"ja", "ミス！演奏終了"},
                  {"en", "Missed! The song ends here"},
                  {"zh", "失誤！演奏結束"},
                  {"ko", "실수! 연주 종료"}}},
                {"reason_sudden_death",
                 {{"ja", "ミスで終了"}, {"en", "Sudden death"}, {"zh", "失誤結束"}, {"ko", "실수 종료"}}},
                {"new_best",
                 {{"ja", "自己ベスト更新　+{0}"},
                  {"en", "New best   +{0}"},
                  {"zh", "刷新最佳　+{0}"},
                  {"ko", "최고 기록 갱신　+{0}"}}},
                {"new_best_plain",
                 {{"ja", "自己ベスト更新"}, {"en", "New best"}, {"zh", "刷新最佳"}, {"ko", "최고 기록 갱신"}}},
                {"unsaved",
                 {{"ja", "この成績は保存されません（{0}）"},
                  {"en", "This score is not saved ({0})"},
                  {"zh", "本次成績不儲存（{0}）"},
                  {"ko", "이번 기록은 저장되지 않습니다 ({0})"}}},
                {"list_sep", {{"ja", "・"}, {"en", ", "}, {"zh", "、"}, {"ko", ", "}}},
                {"reason_auto", {{"ja", "オート"}, {"en", "Auto"}, {"zh", "自動演奏"}, {"ko", "오토"}}},
                {"reason_replay", {{"ja", "リプレイ"}, {"en", "Replay"}, {"zh", "回放"}, {"ko", "리플레이"}}},
                {"reason_equal_speed", {{"ja", "等速"}, {"en", "Constant speed"}, {"zh", "等速"}, {"ko", "등속"}}},
                {"reason_skip", {{"ja", "スキップ"}, {"en", "Skip"}, {"zh", "跳過"}, {"ko", "스킵"}}},
                {"reason_record", {{"ja", "録画"}, {"en", "Recording"}, {"zh", "錄影"}, {"ko", "녹화"}}},
                {"rec_mode", {{"ja", "録画モード"}, {"en", "Recording mode"}, {"zh", "錄影模式"}, {"ko", "녹화 모드"}}},
                {"rec_choose",
                 {{"ja", "どちらで録画する？"},
                  {"en", "Record how?"},
                  {"zh", "要怎麼錄？"},
                  {"ko", "어떻게 녹화할까요?"}}},
                {"rec_auto", {{"ja", "オート"}, {"en", "Auto play"}, {"zh", "自動演奏"}, {"ko", "오토"}}},
                {"rec_nohit", {{"ja", "たたかない"}, {"en", "No hits"}, {"zh", "不敲擊"}, {"ko", "치지 않음"}}},
                {"rec_courses",
                 {{"ja", "いっしょに録画するむずかしさ（1つ1レーン）"},
                  {"en", "Courses to record together (one lane each)"},
                  {"zh", "一起錄的難度（每個一條軌道）"},
                  {"ko", "함께 녹화할 난이도 (난이도마다 1레인)"}}},
                {"rec_keys_choose",
                 {{"ja", "左右カッ：えらぶ　ドン：録画／むずかしさの切りかえ　もどる：やめる"},
                  {"en", "Kat L/R: move   Don: record / tick a course   Back: cancel"},
                  {"zh", "左右咔：選擇　咚：開始錄影／勾選難度　返回：取消"},
                  {"ko", "좌우 딱: 선택　쿵: 녹화 / 난이도 선택　뒤로: 취소"}}},
                {"rec_running",
                 {{"ja", "録画中… {0}%"}, {"en", "Recording… {0}%"}, {"zh", "錄影中… {0}%"}, {"ko", "녹화 중… {0}%"}}},
                {"rec_running_keys",
                 {{"ja", "カッ：うらで続ける（ほかの曲を予約できる）　もどる：すべて中止"},
                  {"en", "Kat: keep going in the background (queue more songs)   Back: stop all"},
                  {"zh", "咔：背景繼續（可再排其他歌）　返回：全部中止"},
                  {"ko", "딱: 백그라운드로 계속 (다른 곡 예약 가능)　뒤로: 모두 중지"}}},
                {"rec_keys_queue",
                 {{"ja", "左右カッ：えらぶ　ドン：予約に追加／むずかしさの切りかえ　もどる：やめる"},
                  {"en", "Kat L/R: move   Don: add to the queue / tick a course   Back: cancel"},
                  {"zh", "左右咔：選擇　咚：加入佇列／勾選難度　返回：取消"},
                  {"ko", "좌우 딱: 선택　쿵: 대기열에 추가 / 난이도 선택　뒤로: 취소"}}},
                {"rec_eta",
                 {{"ja", "のこり 約{0}"}, {"en", "About {0} left"}, {"zh", "剩餘約 {0}"}, {"ko", "남은 시간 약 {0}"}}},
                {"rec_queue_n",
                 {{"ja", "予約 {0}曲"}, {"en", "{0} queued"}, {"zh", "佇列 {0} 首"}, {"ko", "대기 {0}곡"}}},
                {"rec_queued",
                 {{"ja", "予約しました（{0}番目）"},
                  {"en", "Queued (#{0})"},
                  {"zh", "已加入佇列（第 {0} 個）"},
                  {"ko", "대기열에 추가 ({0}번째)"}}},
                {"rec_bg",
                 {{"ja", "録画中 {0}%  {1}"},
                  {"en", "Recording {0}%  {1}"},
                  {"zh", "錄影中 {0}%  {1}"},
                  {"ko", "녹화 중 {0}%  {1}"}}},
                {"rec_bg_keys",
                 {{"ja", "もどる：録画の進みぐあい"},
                  {"en", "Back: recording progress"},
                  {"zh", "返回：檢視錄影進度"},
                  {"ko", "뒤로: 녹화 진행 상황"}}},
                {"rec_batch_done",
                 {{"ja", "録画おわり：{0}本（失敗 {1}本）"},
                  {"en", "Recordings done: {0} saved ({1} failed)"},
                  {"zh", "錄影全部完成：{0} 個（失敗 {1} 個）"},
                  {"ko", "녹화 완료: {0}개 (실패 {1}개)"}}},
                {"rec_done", {{"ja", "録画しました"}, {"en", "Saved"}, {"zh", "錄影完成"}, {"ko", "녹화 완료"}}},
                {"rec_done_keys",
                 {{"ja", "ドン：フォルダを開く　カッ：もどる"},
                  {"en", "Don: show in folder   Kat: back"},
                  {"zh", "咚：開啟資料夾　咔：返回"},
                  {"ko", "쿵: 폴더 열기　딱: 돌아가기"}}},
                {"rec_failed",
                 {{"ja", "録画できませんでした（record.log）"},
                  {"en", "Recording failed (see record.log)"},
                  {"zh", "錄影失敗（詳見 record.log）"},
                  {"ko", "녹화 실패 (record.log 참고)"}}},
                {"rec_failed_keys",
                 {{"ja", "たたいて もどる"}, {"en", "Hit to go back"}, {"zh", "敲擊返回"}, {"ko", "쳐서 돌아가기"}}},
                {"search_genre",
                 {{"ja", "ジャンル：{0}"}, {"en", "Genre: {0}"}, {"zh", "類型：{0}"}, {"ko", "장르: {0}"}}},
                {"search_course",
                 {{"ja", "コース：{0}"}, {"en", "Course: {0}"}, {"zh", "難度：{0}"}, {"ko", "코스: {0}"}}},
                {"search_level", {{"ja", "★{0}"}, {"en", "★{0}"}, {"zh", "★{0}"}, {"ko", "★{0}"}}},
                {"search_all", {{"ja", "すべて"}, {"en", "All"}, {"zh", "全部"}, {"ko", "전체"}}},
                {"search_count", {{"ja", "{0}曲"}, {"en", "{0} songs"}, {"zh", "{0}首"}, {"ko", "{0}곡"}}},
                {"search_none",
                 {{"ja", "見つかりません"}, {"en", "No matches"}, {"zh", "沒有符合的歌曲"}, {"ko", "결과 없음"}}},
                {"search_keys",
                 {{"ja", "↑↓／ホイール：選ぶ　Enter／クリック：その曲へ　Tab：ジャンル　F3：難易度　PgUp/"
                         "PgDn：★（項目クリックでも切替）"},
                  {"en", "Up/Down/wheel: pick   Enter/click: go   Tab: genre   F3: course   PgUp/PgDn: level (or click "
                         "a filter)"},
                  {"zh", "↑↓／滾輪：選擇　Enter／點選：跳到該曲　Tab：類型　F3：難度　PgUp/PgDn：星級（也可點篩選項）"},
                  {"ko", "↑↓/휠: 선택　Enter/클릭: 이동　Tab: 장르　F3: 난이도　PgUp/PgDn: ★ (필터 클릭도 가능)"}}},
                {"course_0", {{"ja", "かんたん"}, {"en", "Easy"}, {"zh", "簡單"}, {"ko", "쉬움"}}},
                {"course_1", {{"ja", "ふつう"}, {"en", "Normal"}, {"zh", "普通"}, {"ko", "보통"}}},
                {"course_2", {{"ja", "むずかしい"}, {"en", "Hard"}, {"zh", "困難"}, {"ko", "어려움"}}},
                {"course_3", {{"ja", "おに"}, {"en", "Extreme"}, {"zh", "魔王"}, {"ko", "오니"}}},
                {"course_4", {{"ja", "おに(裏)"}, {"en", "Extreme (Ura)"}, {"zh", "魔王(裏)"}, {"ko", "오니(뒷면)"}}},
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

        // two generations: when the newer fills up it becomes the older one and the oldest goes, so a label handed
        // out earlier this frame (and kept until it is drawn) is never the one freed
        std::unordered_map<std::string, std::unique_ptr<OutlinedText>> cache;
        std::unordered_map<std::string, std::unique_ptr<OutlinedText>> cache_old;
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

    std::string text(const std::string& key, const std::string& a0, const std::string& a1, const std::string& a2,
                     const std::string& a3) {
        std::string s = text(key, a0, a1);
        for (const auto& [mark, value] : {std::pair<const char*, const std::string*>{"{2}", &a2}, {"{3}", &a3}}) {
            for (size_t at = s.find(mark); at != std::string::npos; at = s.find(mark, at + value->size())) {
                s.replace(at, 3, *value);
            }
        }
        return s;
    }

    OutlinedText& label(const std::string& s, int size, Color color) {
        const std::string key = s + '\x1f' + std::to_string(size) + '\x1f' + std::to_string(color.r) + ',' +
                                std::to_string(color.g) + ',' + std::to_string(color.b);
        auto it = cache.find(key);
        if (it == cache.end()) {
            if (auto old = cache_old.find(key); old != cache_old.end()) { // still in use: back to the newer one
                it = cache.emplace(key, std::move(old->second)).first;
                cache_old.erase(old);
                return *it->second;
            }
            // at most one flip a frame: a second would free labels handed out earlier in it
            static double last_flip_ms = -1.0;
            if (const double now = get_current_ms(); cache.size() > 512 && now != last_flip_ms) {
                last_flip_ms = now;
                cache_old = std::move(cache);
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
