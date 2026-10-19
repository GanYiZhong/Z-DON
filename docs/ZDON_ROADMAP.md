# Z-DON roadmap

狀態：2026-10-19。上游同步到 `bde4201`。所有項目都用確定性證據驗證（見「驗證工具」），數字是當時這台機器量的。

## 已完成

### 載入
| 項目 | 開關（環境變數） | 結果 |
|---|---|---|
| 跨畫面預讀，8 個同時讀，新畫面開始載入就取消 | `YATAIDON_NO_READAHEAD=1` 關、`YATAIDON_READAHEAD_THREADS=<n>` | 冷硬碟選曲貼圖 1.4 s → 0.35 s（標題停 5 s） |
| 8 秒以上音效先解前 2 秒，其餘背景解碼 | — | 選曲 bgm 載入 211 → 23 ms |
| 曲庫只走訪一次（LibraryIndex）、快取驗證用列表的大小/時間 | `YATAIDON_LIBRARY_DUMP=<file>` 匯出指紋 | 背景資料夾掃描 1.7–2.0 s → 0.23 s |
| 曲庫走訪和 load_skin 並行 | — | 啟動到曲庫掃描完 0.93–1.13 → 0.46–0.51 s |
| 開資料夾用快取的 box record 建 SongBox，需要時才讀譜面 | `YATAIDON_BOX_RECORD_VERIFY=1`、`_SELFTEST=1` | — |
| 選曲/段位選擇貼圖跨遊戲/結算常駐（VRAM 預算） | `YATAIDON_RETAIN_MB=<n>`，0 = 關 | 結算回選曲貼圖 234 → 32 ms |
| 每畫面貼圖打包快取 `cache/texpack/`、zlib-ng PNG 解碼 | `YATAIDON_TEXPACK=0`、`_TEXPACK_MB`、`_TEXPACK_VERIFY=1`、`YATAIDON_FASTPNG=0` | 冷硬碟選曲貼圖 755 → 417 ms；解碼 1.75× |
| 3D 角色零件跨畫面重用 | — | 每個 Don 約 30 → 7 ms |
| 選定歌曲就預讀歌曲資料夾、背景影片在背景開 | — | — |
| 響度快取合成單一檔案、段位曲音符數存進 scores.db | — | — |

### 執行時
| 項目 | 開關 | 結果 |
|---|---|---|
| 輸入執行緒改用高解析度計時器（原本空轉） | `YATAIDON_INPUT_SLEEP=plain` 還原、`YATAIDON_INPUT_STATS=1` | 省一整顆核心；輪詢約每 1 ms |
| Windows Release `-O3`＋LTO | `-DZDON_LTO=OFF` 開發用 | 每幀 CPU 約 −14% |
| 無鎖音訊 callback、串流解碼移到自己的執行緒、SoundId 打擊音 | `YATAIDON_AUDIO_STATS=1`、`_CAPTURE`、`_SELFTEST` | callback 最壞 10.6 ms → 0.3 ms，不再等鎖 |
| 字型：共用不可變 glyph 快取、增量 atlas | `YATAIDON_TEXT_STATS=1` | 每段文字 0.64 → 0.11 ms，最壞 5–6 → 0.6 ms |
| Lua 分代 GC、方法快取事件化、跳過提示去重 | — | 連打譜每 2 s 最慢一幀 5.4 → 2.6 ms |
| 音符繪製精簡、已停住的歌曲框不更新、繪圖延後切換混合模式＋3 個批次緩衝 | `ZDON_EAGER_BLEND=1` 還原、`YATAIDON_DRAW_STATS=1` | 每幀 flush 10.7–12.9 → 9.7–10.0 |
| vblank 對齊限幀（vsync 關、不限 fps 時），**預設開** | `ZDON_VBLANK_PACING=0` 關、`=<n>`、`ZDON_PACING_LOG=<csv>` | 步進 >1 ms 偏差 30% → <1%；代價：165 Hz 上約 170 fps，判定粒度約 6 ms |
| 共用工作池取代臨時執行緒、玩家共用 SongParser、貼圖查名不建字串 | — | — |
| TJA 解析加速（指令表、無 regex、view） | — | notes_to_position 1.0–1.1 → 0.64–0.70 s（全曲庫） |
| 全 src 精簡 | — | −2450 行 |

### 修掉的 bug（多半上游也有）
- 打開「歌曲資料夾還有子資料夾」的類別會當機（scan_song_tree 懸空參考）。
- 同 hash 譜面對應的檔案隨執行緒競賽而變；同名歌曲（例如段位用的 A Cruel Angel's Thesis 新/舊音源）每次啟動選到不同檔案。
- 命令列/捷徑開不了含假名/韓文的譜面路徑（argv 是 ANSI 字碼頁）。
- 練習模式結束遊戲必當（PracticeTimer 在 Lua 關閉後才釋放）。
- `TimelineObject::end_time`、`Player::last_drumroll_color_time` 未初始化。
- 自動演奏/回放打段位也會記錄段位成績並升級名牌。
- skin 更新器每次開機都下載整份清單（Green 約 1.15 MB）並重寫雜湊快取。

### 開發工具
- `YATAIDON_TEST=1`：音量 1%、最小化（隱藏視窗時不最小化）、忽略真實輸入、不寫歌單。
- `YATAIDON_FIXED_STEP_MS=<ms>`＋`YATAIDON_DUMP_FRAMES=<dir>;<frames>`：確定性畫面（固定時間步、等背景載入、無網路），同一個 exe 跑出逐位元組相同的截圖。
- `YATAIDON_JUMP_SCREEN`、`YATAIDON_FAKE_KEYS`、`YATAIDON_FORCE_AUTO`、`YATAIDON_DUMP_SCREEN`、`YATAIDON_DUMP_CHARA`、`YATAIDON_FRAME_STATS=1`。
- `tools/zdon_syntax_check.py`（單檔 -fsyntax-only）、`tools/build_pgo.sh`（PGO，量不到增益，預設不用）、預編譯標頭 `ZDON_PCH`。

## 合併上游 bde4201 後的已知畫面差異（都是上游自己的改動）
- 練習模式：f2e1174 起會播前奏（以前第一幀就跳過約 1 s 前奏）。純上游 f2e1174 前/後與 Z-DON 合併前/後各自 19/19 相同。
- 練習模式按空白鍵不會暫停（上游就沒有 pause_key 處理，只有 2P 咚/咔），未改。

## 驗證工具（scratchpad，非 repo）
- 確定性截圖套件：title / ss / dense / roll / hbs / dan / entry / settings / p2 / practice / danplay / movie，共 349 張，用 RGB 位元組比對。**不要用 Pillow 的 RGBA getbbox（只看 alpha）。**
- TJA 對拍：連結舊/新 tja.cpp，解析全曲庫 3043 首比對每個欄位。
- 曲庫指紋：`YATAIDON_LIBRARY_DUMP`。

## 評估後不做
- 單一執行緒依序讀檔、Lua bytecode 快取（冷硬碟反而慢）。
- 啟動時先用存下的目錄樹（暖快取省 0；有效版本會讓曲數/皇冠先顯示舊值）。
- 執行期貼圖 atlas（每幀只有 60–110 次 draw call）。
- PGO 交付（量不到增益）。
- 紋理快取存解碼後像素（目前瓶頸是 GPU 上傳，不是解碼）。
- 輸入時間戳、音訊時鐘插值：會改變判定方式（使用者選擇不做）。

## 還可以做的
- 貼圖用到時才上傳 GPU / PBO 背景上傳（第一次進畫面約 150 ms 的上傳）：需要共享 GL context 或分幀上傳（可能先看到貼圖空白），收益小、風險高，暫不做。
- `PlaybackClock` 與 vblank 限幀更深的整合。
- 譜面音符改成不用 deque（約 90 處，收益小）。
