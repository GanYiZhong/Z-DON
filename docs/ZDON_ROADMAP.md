# Z-DON roadmap

狀態：2026-10-19。上游同步到 `5e1b04d`。所有項目都用確定性證據驗證（見「驗證工具」），數字是當時這台機器量的。

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
- `ZDON_FRAME_SPIKE_MS=<ms>`：每個超過門檻的幀記一行（update/draw 拆開），present（EndDrawing＋swap）超過門檻也另記；不需要開 FRAME_STATS。
- `tools/zdon_syntax_check.py`（單檔 -fsyntax-only）、`tools/build_pgo.sh`（PGO，量不到增益，預設不用）、預編譯標頭 `ZDON_PCH`。

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
- 上游 dc27b85 把一般演奏的暫停鍵（pause_key）註解掉，Z-DON 照上游（演奏中按空白鍵不再暫停）。

## 2026-10-20 貼圖用到時才上傳：量測備註
- 原本以為「進選曲後下一幀卡約 377 ms 是驅動在補上傳」，量下來不是：用 glFinish 夾每一段，GL 佇列在 swap 前就空了，卡住的是 `SwapScreenBuffer` 本身；而且在 loading 畫面、完全沒載貼圖時也會發生，固定在啟動後約 0.5–1 s（JUMP song_select;10 剛好落在那時）。改成啟動 5 s 後才進選曲，兩種模式下一幀都不卡。
- 演奏中第一次畫到才上傳會掉幀（實測 2352x3810 的 game 貼圖 24 ms、背景 gogo 階段 11 ms），所以 global/game/background 與演奏中載入的資料夾維持載入時上傳；dense 譜 auto 全程開/關都沒有新的演奏中上傳。
- 代價：沒畫到的解碼圖留在 RAM（選曲常駐那批約 370 MB 會一直留著，算在 768 MB 上限內）；常駐 VRAM 預算把等待中的貼圖當作已上傳計算。

## 2026-10-20 臨時載入資料夾預載：量測備註
- 實測（Nijiiro）記到的：選曲 → global/indicator、global/nameplate、global/timer、dan_select/global；演奏 → global/nameplate、global/result_transition、global/rainbow_transition、loading/skip_badge；結算、段位選擇 → global/*。
- 只有 dan_select/global 真的省到：global/* 的物件 global_tex 早就做好（共用 tex_object_cache），那 1.5 ms 是列資料夾＋讀 texture.json，預載前後都一樣。要再省得讓臨時 load_folder 共用畫面載入時的目錄列表。
- 不能直接把預載的資料夾放進 `textures`：dan_select/global 經 Green 繼承了 footer/arrow，會蓋掉選曲自己的 global/footer、global/arrow（今天是轉場那一刻才蓋），所以改成暫存區、等 Lua 要時才裝。
- 預載的物件照原本 load_folder 的規則進 texture_retain（記在資料夾所屬畫面，例如 dan_select），之後進段位選擇照樣重用；沒用到就在換畫面時跟著丟（常駐的那份留在 retain 預算內）。

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
