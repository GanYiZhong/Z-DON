# Z-DON roadmap

狀態：2026-10-21。上游同步到 `5e1b04d`。所有項目都用確定性證據驗證（見「驗證工具」），數字是當時這台機器量的。

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
| 貼圖用到時才上傳 GPU（第一次畫到的那一幀同步上傳，不會空白；演奏中會畫到的 global/game/background 照舊載入時上傳），**預設開** | `ZDON_LAZY_UPLOAD=0` 關、`ZDON_LAZY_UPLOAD_MB=<n>`（等待中的解碼圖 RAM 上限，預設 768，超過就改回載入時上傳） | 選曲貼圖載入 238–250 → 152–157 ms，進選曲那幀 536–566 → 455–465 ms；捲動測試 525 張只上傳 39 張（456 → 85 MB）；段位選擇 92–97 → 64–66 ms；結算 178 → 144 ms（＋第一幀 draw 約 8 ms）；開難度選擇 35.6–38.7 → 33.8–34.0 ms（沒變差） |
| 換畫面時釋放沒畫過的貼圖的解碼圖（evict）：貼圖仍是等待上傳、記住來源檔；下次載入持有它的畫面時跟該畫面的貼圖一起解碼回來（走 texpack、預讀順序），在那之前被畫到就當場解碼（記 log），**預設開** | `ZDON_LAZY_EVICT=0` 關、`ZDON_LAZY_KEEP_MB=<n>`（保留最小的 n MB，預設 0） | 選曲 → 演奏 → 結算 → 選曲兩輪：演奏中 working set 754–827 → 383–417 MB、private 1623–1762 → 1204–1290 MB；結算 ws 776–837 → 412–446 MB。代價：回選曲貼圖載入 55–67 → 167–186 ms（重解 478 張 387 MB 約 89 ms），畫面開始 150–165 → 260–287 ms；`KEEP_MB=128` 時 135–140 ms、演奏 private 1412–1478 MB。確定性套件 121/121 相同 |
| 結算畫面背景先解回被釋放的貼圖（prefill）：結算/段位結算畫面載入完，就把下一個最可能的畫面（結算 → 選曲、段位結算 → 段位選擇）被 evict 的解碼圖，用 4 個 Background 工作（低於一般優先權、不佔最後一個 worker）解回來，**走該畫面的 texpack**（一次循序讀上次存的檔、讀檔執行緒用背景 I/O 優先權；散檔是幾百個小檔讀取）；解好的先放在暫存區，下一次載入畫面時才交給貼圖（中間若再 evict 會又被丟）：是目標畫面就拿走已解好的、其餘照舊在載入時解；不是目標畫面就全部丟掉。離開時還在解的那張由工作自己丟掉，不等它。上限＝`ZDON_LAZY_UPLOAD_MB` 剩下的空間，**預設開** | `ZDON_LAZY_PREFILL=0` 關、`=<n>` 用 n 個工作（預設 4、上限 16）；`ZDON_LAZY_PREFILL_SLOW_MS=<ms>`（開發）每張先等 ms | 選曲 → 演奏 → 結算 → 選曲兩輪（Nijiiro、auto）：回選曲貼圖載入 192–207 → 61–62 ms、畫面開始 289–309 → 154–156 ms（回到 evict 前的 150–165）；484 張 397 MB 在結算開始後 216–252 ms 解完（1 個工作、散檔 589 ms）。結算畫面 working set 394–435 → 815–855 MB、private 結束時 652/788 → 1073/1119 MB（多的就是那 397 MB），演奏中不變（ws 363–409 vs 379–406、private 1240–1368 vs 1236–1324 MB）。結算畫面主執行緒：p99 1.1–2.8 vs 1.3–2.5 ms，>10 ms 的幀（第一幀 155–180 ms 兩邊都有）關 51/22/16 ms、開 13 ms，沒變差。提早離開（fast_transitions、1 個工作、SLOW_MS=10）：105/484 張好了，其餘 378 張載入時解 89 ms；解到一半關遊戲正常結束。確定性套件 121/121 與 HEAD 相同 |
| 學習 Lua 臨時載入的貼圖資料夾：`resolve_lua_texture` 臨時 load_folder 時記下（目前畫面 → 資料夾），存在 `cache/ondemand/<skin>.txt`（每畫面最多 64 筆、有變才寫、換畫面時寫）；下次該畫面載入時跟自己的資料夾一起平行解碼（走 texpack、lazy upload），物件先放暫存區、不放進 `textures`，等 Lua 要時才裝上（時機和覆蓋行為都跟以前一樣），換畫面一起丟。資料夾不存在就從檔案刪掉。每次臨時載入都記一行 info（含耗時），**預設開** | `ZDON_PREFETCH_ONDEMAND=0` 關（不記也不預載） | 選曲進段位轉場那幀 32.9–34.7 → 4.1–5.0 ms（dan_select/global 臨時載入 32.0–33.7 → 1.5–1.8 ms，其餘是第一次畫到的上傳約 2.5 ms）；選曲貼圖載入 147–156 → 157–165 ms（多解一張 1080p）、進選曲那幀 463–540 → 470–493 ms（雜訊內）；確定性套件空檔/有檔各 121/121 相同 |
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
| 打擊音複音（咚/咔各 6 個 voice 環、共用 PCM、最舊的被搶），**預設開** | `ZDON_HIT_POLYPHONY=<n>`（1 = 舊的重播行為，上限 24）；`YATAIDON_AUDIO_STATS` 多報 max_voices/poly_plays/overlapped/stolen | dense 譜 auto：每 2 s 約 20 次重疊、最多 12 個 voice 同時響；selftest 8 連打對模型誤差 1e-7 |
| 跨畫面常駐音效：打擊音、氣球/薬玉破裂、don/ka、Sounds/global（名稱＋路徑相同就不重解碼） | `ZDON_RESIDENT_SOUND_MB=<n>`（預設 64，0 = 關）；切 skin 時丟掉 | 結算/選曲各直接重用 7 個（1.9 MB 常駐） |
| 音量類別即時生效（mixer 每次 callback 乘上類別音量，設定畫面確認即套用） | — | 音量不變時數值與以前相同；selftest 播放中改音量 → 正好 ×0.5、再回 ×1.0（誤差 0） |
| 換畫面時 Lua 完整 GC（debug log 記時間與 heap） | `ZDON_LUA_GC_STRESS=1`：每幀都跑（測試用） | 確定性套件 121/121 相同 |
| 字型：共用不可變 glyph 快取、增量 atlas | `YATAIDON_TEXT_STATS=1` | 每段文字 0.64 → 0.11 ms，最壞 5–6 → 0.6 ms |
| Lua 分代 GC、方法快取事件化、跳過提示去重 | — | 連打譜每 2 s 最慢一幀 5.4 → 2.6 ms |
| 音符繪製精簡、已停住的歌曲框不更新、繪圖延後切換混合模式＋3 個批次緩衝 | `ZDON_EAGER_BLEND=1` 還原、`YATAIDON_DRAW_STATS=1` | 每幀 flush 10.7–12.9 → 9.7–10.0 |
| vblank 對齊限幀（vsync 關、不限 fps 時），**預設開** | `ZDON_VBLANK_PACING=0` 關、`=<n>`、`ZDON_PACING_LOG=<csv>` | 步進 >1 ms 偏差 30% → <1%；代價：165 Hz 上約 170 fps，判定粒度約 6 ms |
| 限幀等待的空轉依實測計時器遲到量調整：高解析度計時器在這台機器遲到 0.03–0.6 ms（p50 0.32，0.5 ms tick），遲到 0.5 ms 內本來就取格點時間，所以只空轉 p99 遲到 − 0.5 + 0.1 ms（0.05–0.5 ms，每 600 次重估，實測 0.18–0.22 ms） | `ZDON_PACING_LOG` 多 `w,<要求喚醒>,<實際喚醒>,<空轉 ms>` | 空轉 40 → 8 ms/s（4.0% → 0.8% 單核）；步進 >1 ms 偏差 0.39/0.50% → 0.44/0.41%，去除卡頓 std 0.046/0.067 → 0.073/0.065 ms；確定性套件 121/121 相同 |
| 影片以 YUV 平面上傳（Y 全尺寸、U/V 色度尺寸的 R8 貼圖），shader 轉 RGB；只支援 8-bit planar YUV（420/422/444/440/411/410 與 yuvj），其他照舊 swscale RGB24。矩陣：標 BT.709/BT.2020 才用，其餘 BT.601（舊路徑全用 601）；範圍：yuvj 或標 full 才全範圍。shader 重現舊畫法（每個影片像素用涵蓋它的色度樣本＝swscale 的最近鄰色度、轉換後夾值，再雙線性、GL_REPEAT 邊緣），**預設開** | `ZDON_VIDEO_YUV=0` 回 RGB；`ZDON_VIDEO_YUV_CHECK=1`（開發）每 30 幀把 shader 結果讀回和 swscale 比 | 對 swscale 每通道最大差 2–3（601 limited）、2–3（709）、1–2（yuvj full）、1（444），差 >2 的像素 ≤0.23%；確定性截圖 movie（720p→1080p）對舊路徑最大 3、平均 0.41，title 最大 3；165 fps 同幀數下：解碼執行緒 12.2/11.1 → 7.8/7.1%、主執行緒 15.9/15.1 → 15.0/13.0%、GL 驅動 15.2/14.0 → 12.5/12.4%、行程 48.9/45.9 → 40.4/36.7%，幀工作 avg 0.87/0.82 → 0.77/0.72 ms、p99 2.3/2.2 → 1.58/1.53 ms |
| 共用工作池取代臨時執行緒、玩家共用 SongParser、貼圖查名不建字串 | — | — |
| TJA 解析加速（指令表、無 regex、view） | — | notes_to_position 1.0–1.1 → 0.64–0.70 s（全曲庫） |
| 全 src 精簡 | — | −2450 行 |

## 2026-10-21 資源用量四項：量測備註
- 限幀空轉：直接寫 console 小程式量 `CreateWaitableTimerExW(HIGH_RESOLUTION)`，遲到 p50 0.33、p99 0.61 ms；提高執行緒優先權或 `NtSetTimerResolution(0.5 ms)` 都一樣（看起來是 0.5 ms 的 tick），所以縮到 0.1 ms 的固定空轉會讓 2.4% 的喚醒超過 0.5 ms、步進 std 0.069 → 0.14 ms；改成依 p99 調整。原本估的 8% 單核實際是 4%（timer 平均已遲到 0.32 ms，只剩一半要空轉）。主執行緒 CPU 量起來雜訊太大（同版本 22–31%），所以用 log 直接算空轉時間。
- 未畫過的貼圖：只是「不保留在 retention 裡」只省 83 MB——約 300 MB 的物件（box/background 107 MB、yellow_box/s_crown_* 93 MB、diff_select/difficulty_back 34 MB…）同時被 Lua 腳本拿著，物件不會死；所以改成釋放影像、物件留著、下次載入補回。`KEEP_MB` 先留小圖：留 126 MB（457 張小圖）只把重解從 89 ms 降到 67–71 ms，大圖才是時間大宗，所以預設全放。進選曲時的重解是 CPU 解碼量（約 2.6 GB/s 輸出）決定的，要再省得在結算畫面背景先解，沒做。
- 影片：雙線性色度（含 left siting）在 720p 測試片的色塊邊緣和 swscale 差到 255（平均約 2），720p→1080p 畫面上差到 100；最近鄰色度直接取樣也只在原尺寸吻合，放大時色度邊緣比舊的銳利（差到 100）。最後在 shader 裡手動做「四個像素各自轉 RGB 再雙線性」，才和舊路徑放大後也只差 3。
- 影片 CPU 要固定幀數比：預設的 vblank 限幀在 YUV 版因為幀工作變短、p99.5 變小，每個刷新多塞了輸入幀（165 → 266–330 fps），每秒 CPU 反而變高（每幀 CPU 2.9–3.1 → 1.8–2.1 ms）。上表用 `ZDON_VBLANK_PACING=1`。
- 標 BT.709 的影片舊路徑用 601 解，新路徑照標記，所以畫面會變（測試片 max 109、平均 5）；未標記的照舊 601。
- 外洩：標題畫面 attract 循環 6 分鐘（19 支影片，含 1080p 420 與 444），working set 242–310 MB、private 356–427 MB 來回、GPU dedicated 142–145 MB，沒有成長。

### 功能
| 項目 | 開關 | 結果 |
|---|---|---|
| 一般演奏（單人 GameScreen、2P）的暫停選單：Esc（back_key）或空白鍵（pause_key）打開；歌曲時鐘、音樂、背景影片、音符都停住（Lua 背景、Player 都不 update）。左右框（或方向鍵）移動「繼續演奏／從頭演奏／選其他歌曲」，咚（或 Enter）確定，再按 Esc／空白鍵＝繼續。繼續前倒數 3-2-1（1.8 s），倒數中按 Esc／空白鍵再暫停；暫停與倒數期間按的鍵在恢復時全部丟掉（clear_input_buffers），所以不會有免費判定；F1 照舊重來。外觀沿用 skin 練習選單的 practice/menu_panel、menu_button(_selected) 貼圖（沒有就引擎畫純色面板），文字先找 skin 的 pause_menu_*，再找練習選單的 practice_menu_restart / practice_menu_another_song，最後內建 en/ja/zh/ko；不改任何 skin。只在開場轉場結束後、歌曲結束前、沒跳過時可暫停，其他時候 Esc 照舊直接離開。視窗失焦也會打開（測試模式不會，只在「有焦點 → 失焦」那一刻）。段位（game_dan）維持原樣（考試規則下暫停有疑慮）；練習模式、輸入校正不受影響。歌曲結束時 log 一行最終判定數 | `config.toml` `[general] pause_menu = true`（預設）；`false` = Esc 直接離開、2P 空白鍵照舊單純暫停 | dense 譜 auto：暫停兩次（4.80 s、2.80 s）時鐘停在 17761.8 / 32969.0 ms 原值恢復；結果 good 999 ok 0 bad 0 drumroll 64，與沒暫停的三次（drumroll 63/64/64、其中一次 ok 1，auto 本身就會差一下）相同。混音輸出錄音：暫停期間除了選單音效全靜音，恢復後與沒暫停的錄音對齊，延遲差＝暫停時間 +11 ms、+7 ms（相關係數 0.57–0.85）。手動（非 auto）暫停＋倒數期間按 15 下咚：good 0 ok 0 bad 999，與沒按相同；恢復後按的照常判定。重來、選其他歌曲、`pause_menu = false` 都驗過。確定性套件 121/121 相同 |

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
- `YATAIDON_JUMP_SCREEN`、`YATAIDON_FAKE_KEYS`、`YATAIDON_FORCE_AUTO`、`YATAIDON_DUMP_SCREEN`、`YATAIDON_DUMP_CHARA`。
- `YATAIDON_FRAME_STATS=1`：每 2 s 一行 `[frame]`：幀工作時間 avg/p50/p99/max、幀間隔（開始到開始）超過 2.08 / 4.17 / 8.33 ms 的次數（480 / 240 / 120 Hz 的一格）、程序 CPU（單核 %，Windows）、working set 與 private MB。每 2 s 才查一次系統（GetProcessTimes＋GetProcessMemoryInfo），預設關。
- `ZDON_FRAME_SPIKE_MS=<ms>`：每個超過門檻的幀記一行（update/draw 拆開），present（EndDrawing＋swap）超過門檻也另記；不需要開 FRAME_STATS。
- `tools/zdon_syntax_check.py`（單檔 -fsyntax-only）、`tools/build_pgo.sh`（PGO，量不到增益，預設不用）、預編譯標頭 `ZDON_PCH`。

## 2026-10-21 vblank 限幀改成不撕裂（ZDON_PACING_TEARFREE）
- 使用者回報：三種視窗模式都看到一條橫線切過音符。原因：驅動直接翻頁（independent flip／NVIDIA GL 走 DXGI）時，vsync 關的每次 swap 都會撕裂，撕裂線落在 swap 當下的掃描行；用 ZDON_PACING_LOG 換算，舊版限幀的 swap 集中在畫面高度 10–70%（音符軌道）。原設計假設 DWM 合成（只顯示每次刷新最新的一幀），不成立。
- 改法（預設開，`ZDON_PACING_TEARFREE=0` 回舊行為）：限幀啟用時打開驅動 vsync；每次刷新只有要顯示的那一幀 swap，額外的（輸入）幀照跑輸入/update/判定/繪圖但不 swap；work 量到 swap 之前為止（vsync 等待不算 work，不然 lead 會一直長）。
- 量測（dense auto、180 Hz、隱藏視窗、45 s）：幀步偏差 >1 ms 11.9% → 5.1%；錯過刷新 2 → 11 次；work p99 2.06 → 3.13 ms。確定性套件 121/121。實際是否還撕裂、順不順要使用者開視窗確認。

## wip-all（YataiDON 本機分支）稽核，2026-10-19
- wip-all HEAD 對它最後合併的上游 479816a 只剩 24 個檔案的差異（+1227/−72），逐項對照 Z-DON；另外 172 個本機 commit 全部分類。
- 低吻合（<70%）的 123 個：84 個已在上游或 Z-DON（形式不同）、25 個在 wip-all 內被撤回/取代、11 個是撤回的 skin 文字標籤、**skin 用得到但 Z-DON 缺的 = 0**。
- 補搬 dbc9e69：skin 更新器在 checksums 清單過期時照樣寫入完整下載（8ac153d，用假伺服器驗證）。不搬：YATAIDON_SYNC_LOG（開發用）。
- 完整分類表在 scratchpad arch/impl_wip_port.md。

## 合併上游 bde4201 後的已知畫面差異（都是上游自己的改動）
- 練習模式：f2e1174 起會播前奏（以前第一幀就跳過約 1 s 前奏）。純上游 f2e1174 前/後與 Z-DON 合併前/後各自 19/19 相同。
- 練習模式按空白鍵不會暫停（上游就沒有 pause_key 處理，只有 2P 咚/咔），未改。

## 合併上游 5e1b04d（方向鍵全畫面、replay 時間戳改 RapidJSON、行動版 TLS）
- 確定性套件 121/121 與合併前相同。
- 上游 dc27b85 把一般演奏的暫停鍵（pause_key）註解掉；Z-DON 之後改成暫停選單（見「功能」）。

## 2026-10-20 貼圖用到時才上傳：量測備註
- 原本以為「進選曲後下一幀卡約 377 ms 是驅動在補上傳」，量下來不是：用 glFinish 夾每一段，GL 佇列在 swap 前就空了，卡住的是 `SwapScreenBuffer` 本身；而且在 loading 畫面、完全沒載貼圖時也會發生，固定在啟動後約 0.5–1 s（JUMP song_select;10 剛好落在那時）。改成啟動 5 s 後才進選曲，兩種模式下一幀都不卡。
- 演奏中第一次畫到才上傳會掉幀（實測 2352x3810 的 game 貼圖 24 ms、背景 gogo 階段 11 ms），所以 global/game/background 與演奏中載入的資料夾維持載入時上傳；dense 譜 auto 全程開/關都沒有新的演奏中上傳。
- 代價：沒畫到的解碼圖留在 RAM（選曲常駐那批約 370 MB 會一直留著，算在 768 MB 上限內）；常駐 VRAM 預算把等待中的貼圖當作已上傳計算。

## 2026-10-20 臨時載入資料夾預載：量測備註
- 實測（Nijiiro）記到的：選曲 → global/indicator、global/nameplate、global/timer、dan_select/global；演奏 → global/nameplate、global/result_transition、global/rainbow_transition、loading/skip_badge；結算、段位選擇 → global/*。
- 只有 dan_select/global 真的省到：global/* 的物件 global_tex 早就做好（共用 tex_object_cache），那 1.5 ms 是列資料夾＋讀 texture.json，預載前後都一樣。要再省得讓臨時 load_folder 共用畫面載入時的目錄列表。
- 不能直接把預載的資料夾放進 `textures`：dan_select/global 經 Green 繼承了 footer/arrow，會蓋掉選曲自己的 global/footer、global/arrow（今天是轉場那一刻才蓋），所以改成暫存區、等 Lua 要時才裝。
- 預載的物件照原本 load_folder 的規則進 texture_retain（記在資料夾所屬畫面，例如 dan_select），之後進段位選擇照樣重用；沒用到就在換畫面時跟著丟（常駐的那份留在 retain 預算內）。

## 2026-10-21 TJA 相容性（分支 zdon-tja）
- 指令比對要整個名稱相同：以前 `#NEXTSONG`、`#NOTATIONOFF` 會被當成 `#N`（切到普通譜面分歧、時間倒回分歧起點），之後的音符全跑到分歧清單裡。曲庫 49 首受影響：48 首段位 TJA（COURSE:Dan，選曲本來就不顯示）和 promise.tja（Oni 主譜 311 → 585 個物件，第 77 小節後的音符回來了）。名稱後直接接數值（`#SCROLL0.5`）照舊。
- 譜面中間的 `NAME:值` 行（段位 TJA 在 `#NEXTSONG` 後的 `EXAM4:`）不再當成音符資料（以前每行冒出 4–5 個假音符）。
- `#NEXTSONG`：不支援（段位走 dan.json），每份譜面警告一次，各首歌接著讀成一張譜。其他沒註冊的指令（`#SENOTECHANGE`、`#LEVELHOLD`、`#DIRECTION`、`#BGAON`/`#BGAOFF`、`#SPLITLANE`、`#GAMETYPE`、`#OBJ*`…）照舊忽略。
- OpenTaiko 音符字母：`A`/`B`（雙人大音符）＝大咚/大咔，`D`（導火線連打）＝氣球（吃 BALLOON 次數，沒有爆炸懲罰）；`C`（炸彈）與 `E`–`I` 當空白（佔原本的格子），被略掉的音符後面那個結尾 `8`（前面沒有開著的連打/氣球時）一起略掉。不明字元每份譜面每個字元只警告一次（以前每個字元一行）。
- 驗證：全曲庫 3043 首新舊對拍，只有上述 49 首不同、其餘逐欄位相同；合成測試譜（字母、指令、分歧、格式錯誤）在 scratchpad tja_compat/synth。
- 沒做、留給使用者決定（會動到判定/分數/魂條或分歧）：炸彈/導火線爆炸、`SCOREMODE`、`LIFE`、`EXAM`、`#LEVELHOLD`、`BALLOONNOR/EXP/MAS` 分歧各自的氣球次數（現在 MAS 會整個蓋掉 NOR/EXP）、`STYLE:Double` 段的 BALLOON/SCOREINIT/SCOREDIFF 會蓋掉單人段的值（150 首裡 348 處 SCOREINIT）。

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

## 2026-10-20 音訊四項驗證結果
- 確定性套件 121/121 與合併上游後（up_5e1b）相同。
- AUDIO_SELFTEST：舊腳本的部分與舊版逐位元相同；複音段對模型最大誤差 9.7e-8（重播模型差 0.82）；stop/unload 後全靜音；即時音量段誤差 0（長音效要用開頭就有聲音的檔，例如自產正弦波）。
- dense 譜 auto＋AUDIO_STATS：callback 平均約 10 µs、最壞 0.25 ms，沒有 underflow。

## 編譯譜面快取（分支 zdon-chartcache，不合併）
- 正確性：全曲庫 12116 個難度序列化/還原逐欄位相同，損毀/截斷/多執行緒都測過。
- 效能：每難度中位數 37.5 µs（解析）vs 35.4 µs（快取命中），42% 的難度命中反而較慢；光是把結果複製成 deque 就約 17 µs。快取檔整個曲庫 94 MB。收益感覺不到，不合併。

## 還可以做的
- PBO／背景執行緒上傳：用到時才上傳已做（見上表），剩下第一次畫到時的同步上傳（選曲 39 張共約 30 ms，分散在多幀）。
- `PlaybackClock` 與 vblank 限幀：已讓 synchronize 用實測的刷新週期（02f375d）；更深的整合會改變演奏時鐘，等於判定行為改動，不做。
- 譜面音符改成不用 deque：約 90 處參考/迭代器語意要改，每幀的迭代早已走 vector，收益小，不做。
