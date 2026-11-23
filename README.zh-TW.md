# Z-DON

[English](README.md) | 繁體中文

<img src="docs/logo.png">

**Z-DON** 是 [YataiDON](https://github.com/Yonokid/YataiDON) 的分支：一套用 C++ 寫的 TJA 播放器／太鼓模擬器。
YataiDON 本身由它的貢獻者撰寫；**Z-DON 在它之上的修改完全由 AI 開發**，目標是極致的效能，以及上游沒有的新功能與線上對戰。

- 官網：<https://zdon.taiko.ac>
- Windows 版下載：[Z-DON-windows.zip](https://github.com/GanYiZhong/Z-DON/releases/latest/download/Z-DON-windows.zip)（rolling release，永遠是最新版；Linux、macOS、Android 版在同一個 [release](https://github.com/GanYiZhong/Z-DON/releases/tag/rolling)）
- 瀏覽器直接玩（網頁版）：<https://zdon.taiko.ac/play/>
- 授權：GPL-3.0（見 [LICENSE](LICENSE)），源自 YataiDON

## 目錄

- [與上游的關係](#與上游的關係)
- [Z-DON 與 YataiDON 一覽](#z-don-與-yataidon-一覽)
- [Z-DON 加了什麼](#z-don-加了什麼)：[效能](#效能)、[畫面與 skin](#畫面與-skin)、[遊玩](#遊玩與演奏選項)、[練習](#練習)、
  [選曲](#選曲)、[結算](#結算)、[AI 對戰](#ai-對戰)、[錄影](#錄影)、[譜面與輸入裝置](#譜面與輸入裝置)、[線上](#線上)、[網頁版](#網頁版)
- [與上游做法不同的地方](#與上游做法不同的地方)
- [修掉的 bug](#修掉的-bug)．[系統需求](#系統需求)．[安裝](#安裝)．[設定](#設定)．[從原始碼建置](#從原始碼建置)．[操作](#操作)

## 與上游的關係

- 持續合併上游 master（目前同步到 `dd478af2`，2026-11-23），上游的功能（多平台、Lua skin、3D 角色、AI 對戰、演奏對戰、觸控、線上抓歌、
  商店、活動、段位、教學…）都在。
- **相容**：YataiDON／ESE 的 skin、`scores.db`、`config.toml` 都能直接用。Z-DON 自己的資料放在另外的表
  （`zdon_player_options`、`zdon_ghost`、`zdon_ai_sections`、`zdon_ai_section_best`、`zdon_outfits`）與 `zdon_*`
  設定項，換回上游不會掉任何東西。
- 新功能不是預設關閉，就是可以關掉（`config.toml` 的設定或環境變數），回到上游行為；見[設定](#設定)。
- 執行檔名稱沿用上游的 `YataiDON.exe`，方便合併。
- 完整的變更紀錄、每項量測與開關在 [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md)。

## Z-DON 與 YataiDON 一覽

| | YataiDON | Z-DON |
|---|---|---|
| 冷硬碟進選曲的貼圖載入 | 約 1.4 s | 約 0.35 s |
| 演奏中記憶體（private／working set） | 約 680／480 MB | 約 455／305 MB |
| skin 貼圖的顯存 | 未壓縮 RGBA | BC7 壓縮快取（SimpleStyle 選曲 538 → 266 MB） |
| 輸入執行緒 | 空轉一整顆核心 | 高解析度計時器，約 1 ms 輪詢 |
| 限幀 | sleep | 對齊 vblank、不撕裂 |
| Windows 下載大小 | 約 91 MB | 約 41 MB（只解碼的 FFmpeg、strip 過的執行檔） |
| 更新 | 重新下載 | 遊戲內標題畫面按 F9，逐檔差量更新 |
| 演奏中暫停 | 上游的暫停選單 | Z-DON 暫停選單，繼續前 3-2-1 倒數，失焦自動暫停 |
| 練習 | 基本 | A–B 循環、小節跳轉、固定分歧、每一下的偏差、節拍器 |
| 演奏選項 | 上游那些 | 再加等速、隱藏／突現、自己最佳的影子、失誤就結束、手電筒 |
| 線上 | 上游的線上功能 | 再加自架伺服器、線上對戰（房間／自動配對）、挑戰、網站 |
| 錄成影片 | — | 指定譜面與難度直接輸出 MP4，多軌同框 |
| 3D 視角 | — | 可選（`zdon_lane3d`） |
| 譜面 | TJA、osu（上游 parser） | 再加 TJA `COMPAT:`、OpenTaiko 音符字母、`#LEVELHOLD`、osu!taiko 直接放進曲庫 |
| 輸入裝置 | 鍵盤、手把 | 再加 MIDI 鼓（學習對應）、USIO 機台 I/O（實驗性） |

數字都是在開發機上量的，細節見 roadmap。

## Z-DON 加了什麼

### 效能

每一項都用確定性測試（固定時間步，121–349 張截圖逐位元比對）確認畫面不變，並在實機量過。

**載入**

- **跨畫面預讀**：下一個畫面要用的檔案平行讀進系統快取（讓硬碟自己重排請求），換到別的畫面就取消。冷硬碟進選曲的貼圖：1.4 s → 0.35 s。
- **貼圖打包快取**（`cache/texpack/`）：每個畫面的圖從一個包讀，不再讀一堆小檔；PNG 用 zlib-ng 解碼（快 1.75 倍）。
  BC7 快取已經有的檔不再放進包裡。
- **BC7 貼圖快取**（`cache/bc7/`，zstd）：skin 的圖壓一次、過品質門檻（每個像素差 ≤ 4/255 或 PSNR ≥ 45 dB，不過就維持
  RGBA），之後載入不必解 PNG。需要 OpenGL 4.2／BPTC。
- **貼圖用到才上傳 GPU**：第一次畫到才上傳；以前畫過的在每幀之後以 1 MB 為單位先傳（`cache/warm_upload.txt`），第一幀不會卡。
  沒畫到的圖換畫面時釋放，需要時在背景補回來。
- **背景先準備下一個畫面（predecode）**：打開難度選擇就準備演奏畫面、最後一個音符準備結算、標題準備選曲…。
  選曲 → 演奏 754 → 454–717 ms；演奏 → 結算 159 → 83 ms。
- **曲庫只走訪一次**（LibraryIndex），而且跟載入 skin 同時進行；歌曲框用快取的紀錄，需要時才讀譜面。資料夾掃描 1.7–2.0 s → 0.23 s。
- **音效**：長音效先解前 2 秒、其餘在背景解（選曲 BGM 211 → 23 ms）；選單與結算音效第一次用到才載入；連段語音只載入
  有在玩的那一邊、譜面打得到的連段數。
- **只載入這一局要用的**：AI 對戰、段位、教學、演奏對戰的資料夾只在各自的模式載入；`gogo_time`、`kusudama`、`balloon`
  等到譜面真的有 GOGO、薬玉、氣球才載。
- **跨畫面保留**：選曲與段位選擇的貼圖在演奏與結算期間留著（結算 → 選曲貼圖 234 → 32 ms）；打擊音與共用音效常駐。
- **3D 咚**：動畫在背景先解析（啟動 → entry 就緒 765–829 → 386–393 ms），角色部件跨畫面重用。
- **TJA parser 加速**：命令表取代 regex、用 string view；整個曲庫 1.0–1.1 s → 0.64–0.70 s。

**執行時**

- **輸入執行緒改用高解析度計時器**：不再空轉一整顆 CPU 核心，仍約每 1 ms 輪詢。
- **對齊 vblank、不撕裂的限幀**，開 vsync 也一樣（開 vsync 時主執行緒 95.6% → 20.8% 一顆核心）。
- **無鎖音訊 callback**：串流在自己的執行緒解碼；最慢的一次 callback 10.6 → 0.3 ms。
- **打擊音複音**：咚、咔各 6 聲，快速連打不會互相切掉。
- **影片以 YUV 平面上傳**、由 shader 轉色（解碼執行緒 12.2 → 7.8%）；BT.709 影片改用正確的色彩矩陣。
- **文字**：共用的 glyph 快取＋漸進式 atlas（每段文字 0.64 → 0.11 ms）。
- **Lua**：分代 GC、換畫面時完整 GC、method 快取；連打譜最慢一幀 5.4 → 2.6 ms。
- **繪圖**：精簡音符繪製、延後切換 blend、減少 batch flush、raylib 補丁跳過用不到的法線（GL 驅動時間約 −7%）。
- **卡頓後的自動演奏**：卡頓那幾幀跳過的音符與連打會補打（AI 那條也是）。
- **建置**：Windows Release 用 `-O3`＋LTO（每幀 CPU 約 −14%）與 Windows segment heap。

**記憶體**

- 音效改 16-bit PCM（差異最多 −99 dBFS）、BC7 圖不留在 RAM、載入後馬上會被丟掉的貼圖根本不讀、大張圖集去掉空白邊
  （Nijiiro explosion 顯存 34 → 18 MB）、字型用記憶體對映、譜面 parser 結構縮小。
- 演奏中合計：private 約 680 → 455 MB（−33%），working set 約 480 → 305 MB（−37%）。標題畫面 committed 531 → 364 MB。

**下載大小**

- 只解碼的 FFmpeg（DLL 150 → 24 MB）、strip 過的執行檔（33.8 → 23.2 MB；符號留在伺服器，當機紀錄印模組＋位址）、
  大的更新檔用 gzip（執行檔更新 23.3 → 9.7 MB）。Windows zip 91 → 41 MB。
- Linux、macOS 版也用靜態連結、只解碼的 FFmpeg（Linux 178 → 57 MB）。

### 畫面與 skin

- **貼圖密度**：skin 可以出 2×–4× 的圖（`screen.texture_density`，或單張貼圖的 `"density"`），畫面以那個解析度繪製，4K 也清晰。
- **BC7 壓縮貼圖**（`[video] zdon_texture_compression`，預設開）。
- **2D 服裝**：沒有 3D 角色的 skin 也能換裝（`skin_config` 的 `zdon_outfits`），在 entry 畫面選。
- **引擎畫的面板**（練習資訊、判定分布、影子、判定條、「新紀錄」）任何 skin 都能用，內建日／英／中／韓文字，skin 可以改字與位置。
- **給 skin 的 Lua API**：最後一下、每一下的偏差、歌曲進度、最佳分數、最近成績、譜面統計、練習循環、AI 對戰狀態、
  投幣數、可點擊區域、畫面去飽和、加算混合…。
- **字型與語言**：`screen.font_base`（各語言字型疊在基底字型上）、跟隨系統的 AUTO 語言、繁體中文用 skin 的 `font_tw`、
  `<名稱>_<語言>` 的貼圖變體（`language_variants`）。
- **skin 繼承鏈**：缺父 skin 時停在那裡而不是失敗；skin 自己的字型優先於父 skin。
- **附帶 skin**：Windows zip 附 YataiDONSimpleStyle 與 SimpleStyleDark；它們的父 skin（Nijiiro、PyTaikoGreen）第一次執行時
  從一個壓縮檔下載（約 3¾ 分鐘，原本約 11 分鐘）。
- **音符間距**照街機（流速相當於 720p 時軌道寬 82 px）。
- **錯誤更清楚**：log 會寫出 GPU；OpenGL 低於 3.3 時跳訊息，不再是黑畫面。
- **合成式無邊框全螢幕**（Windows，`zdon_composited_fullscreen`，預設關）。

### 遊玩與演奏選項

- **暫停選單**（`pause_menu`，預設開）：演奏中 Esc 或空白鍵；繼續、重來、選別首，繼續前 3-2-1 倒數；視窗失焦也會暫停；
  暫停中按的鼓會被忽略。暫停過的一局不存成績（跟上游一樣）。有 `pause_menu.lua` 的 skin 自己畫。
- **演奏選項**（每位玩家各自存在 Z-DON 自己的表）：
  - **等速**：所有音符照譜面基本 BPM，忽略 BPM／SCROLL 變化（不存成績）。
  - **隱藏／突現**：25／50／75%。
  - **自己最佳的影子**：演奏中顯示「PB ±N」。
  - **失誤就結束**：第一個不可就結束，或第一個可就結束（不存成績）。
  - **手電筒**。
- **演奏輔助**（設定裡，全部預設關）：判定圈下的判定條、目標分數、結算的「新紀錄 +N」、結算判定面板預設打開、
  咔音符上的白叉（色盲輔助）、每首記住難度、隨機／未通過歌曲的按鍵。
- **打擊時序選項**（預設關）：由輸入執行緒播打擊音、以按下的時間判定、提前一小節決定分歧。
- **3D 視角**（`zdon_lane3d`，預設關）：演奏畫面以 3D 呈現，硬幣狀音符、光影；拖曳／滾輪／中鍵移動鏡頭，四種預設。
  一般演奏、2P、わいわい、段位、練習都能用。
- **只用鼓操作選單**：設定畫面只靠鼓就能改（按鍵擷取、防重複綁定、「恢復預設」列、按住連續）。
- **選單滑鼠操作**：左鍵確定、右鍵返回、滾輪移動。
- **遊戲內輸入法**：選字窗畫在遊戲裡（全螢幕時系統的看不到）。
- **投幣模式**（`zdon_coin_mode`，預設關），機台式的 credit。

### 練習

參考 PaulSimulator 的特訓模式，只用鼓就能操作。

- **A–B 區段循環**（`zdon_practice_loop`，預設開）：循環外的音符變灰、2 秒前導、循環結束音樂淡出、列出最近幾次與最好的一次。
- **暫停時**：輸入小節號按 Enter 跳轉、F6（或兩邊咚）固定分歧、咔一次跳 4／8 小節、M 跳到最後一個失誤、音符上方顯示每一下的偏差。
- BPM／分歧／小節資訊列、可選的節拍器、3D 視角。

### 選曲

- **搜尋面板**（F2）：即時搜尋，類型（Tab）、難度（F3）、星級（PgUp／PgDn）篩選，支援滑鼠；Enter 跳到該曲。
- **難度資料夾只列未通過的譜面**（`zdon_sort_filter`，預設開；關掉就是上游的排序）。
- **最近成績**：每張譜面最近 10 筆。
- 開始演奏時才存選項（選曲計時結束不會用到舊的選項）；清單還在捲動時按咚，會等它停下來。

### 結算

- **判定分布直方圖**（按咔）：平均、標準差、早／晚數，以及歌曲每四分之一段的平均。
- **顯示不存成績的原因**（自動演奏、重播、等速、失誤就結束、錄影）。

### AI 對戰

Z-DON 有自己照街機規則做的 AI 對戰，和上游的並存。

- entry 畫面「AI 對戰」→ AI 選曲 → 與 AI 的 2P 演奏。
- AI 依星級與 1–5 的狀態調整，會跟著你的偏差；歌曲在小節線切成數段，每段由贏的一方拿分，平手算玩家贏。
- 每段的最佳王冠與紀錄會保存，有段位、獎勵、音效提示、詳細結算與全段總結。
- 用哪一套：skin 有上游的 `ai_support_menu` Lua 就用上游的 AI 對戰，否則用 Z-DON 的。

### 錄影

- 指定譜面與難度直接輸出 MP4：主選單的「錄影模式」（一次選多個難度、背景佇列顯示剩餘時間），或 `tools/record_video.bat`
  （把 `.tja` 拖上去），或命令列：`YataiDON.exe <譜面.tja> [難度] --record out.mp4 [--auto] [--record-fps 60]`。
- 多軌同框（`--record-tracks 4,3,2,1,0`：1P＋2P，或最多 5 條軌疊在模糊背景上）、片段（`--record-clip`）、每軌音量。
- 速度：GPU 轉色＋三段平行（239 秒的影片約 45 秒錄完；5 軌 179 秒 36.5 秒）。需要 PATH 裡有 `ffmpeg`（或 `ZDON_FFMPEG`）。
  取消的錄影不會留下暫存檔。

### 譜面與輸入裝置

- **TJA**：`COMPAT:` jiro1／jiro2、OpenTaiko 音符字母（A／B 大咚／大咔、D 氣球）、`#LEVELHOLD`、分數分歧條件、各分歧的氣球數、
  P1／P2 譜面各自在自己的 `#END` 結束、STYLE 區段各有自己的 BALLOON／SCOREINIT／SCOREDIFF、命令比對整個名稱
  （以前 `#NEXTSONG` 被當成 `#N`，弄壞 49 首）。不支援：炸彈、SCOREMODE、LIFE、EXAM、`#NEXTSONG`（段位用 `dan.json`）。
- **osu!taiko** 譜面可以直接放進曲庫：難度從難度名稱判斷（否則看 OD）、等級依音符密度、轉盤變氣球、kiai 變 GOGO。
- **MIDI 鼓**：設定裡的 MIDI 分頁，選裝置、學習對應（還沒在真的 MIDI 鼓上測過）。
- **USIO 機台 I/O**（`[usio] enabled`，預設關；實驗性，沒在真機台測過）：鼓感應器門檻、投幣與服務 credit、測試面板。
  不附 `bnusio.dll`。見 [docs/ZDON_USIO.md](docs/ZDON_USIO.md)。
- 輸入測試畫面顯示輸入執行緒的輪詢率。
- Windows 上含假名或韓文的譜面路徑可以從命令列開。

### 線上

- **自架伺服器**（[`server/`](server/README.md)，只用 Python 標準函式庫）：遊戲線上 API（簽章請求）、更新頻道、skin 發佈、網站、
  管理指令（`publish`、`publish-skin`、`publish-web`…）。設定 `[network] zdon_server_url`（空白 = 編進版本的伺服器，或離線）。
- **差量更新**：遊戲在背景檢查（`zdon_update_check`）；標題畫面顯示新版本，按 F9 更新並重開。逐檔比對 SHA-256、以 gzip 下載；
  不碰設定、成績、快取、歌曲、錄影。Windows、Linux、macOS；Android 從自己的頻道更新。
- **skin**：設了 Z-DON 伺服器時從它更新。
- **線上對戰**（主選單，需要帳號）：
  - 4 位數房間代碼或自動配對；三戰兩勝，輸的人選下一首。
  - 客人跟著房主的歌，自己選難度與選項；客人沒有的歌會自動傳過去（最多 85 MB，檢查雜湊）。
  - 打擊同時經 WebSocket 與 UDP 直連（打洞）送出；雙方對時後同時開始（誤差 3 ms 內）。
  - 聊天（Y）、投降、檢舉（R）、信用分（中途離開扣分；分數低就不能自動配對，再低就不能線上）。
- **挑戰**：用別人錄下的演奏打同一張譜。
- **網站**（zdon.taiko.ac）：下載、歌曲與譜面頁、Rating 排行榜、難易度表（資料來自 YataiDON-Syakai）、玩家頁（名牌、王冠統計、
  Best 50、段位）、我的頁面（稱號、名牌、角色外觀、隱私、成績管理與 CSV 匯出）。用 TaikoForge 登入。
- **GitHub rolling release**：Windows、Linux、macOS、Android 一起發佈。

### 網頁版

用 Emscripten 編成 WebAssembly，在瀏覽器裡玩（<https://zdon.taiko.ac/play/>），手機有觸控鼓，音訊走 AudioWorklet 降低延遲。
在另一個 repo 開發；那裡找到的跨平台修正會回到 Z-DON。

## 與上游做法不同的地方

Z-DON 沒有採用、或換成自己版本的上游程式：

- **判定**：不用上游的 JudgeCore／GaugeCore 重構（e56c73e），Player 與魂量表維持 Z-DON 版。
- **歌曲資訊**：不用上游的 Lua `song_info.lua`，維持 C++ 版。
- **暫停選單**：用 Z-DON 的取代上游的（有 `pause_menu.lua` 的 skin 照樣由它畫），並保留上游拿掉的空白鍵暫停。
- **AI 對戰**：兩套並存，由 skin 決定（見 [AI 對戰](#ai-對戰)）。
- **連打計數**：用上游的 Lua 版，但 skin 有 `bubble_unfold` 或在 Z-DON 的 AI 舞台時由 Z-DON 的 C++ 畫。
- **音符流速**：用 Z-DON 的街機間距，不用上游的 `scroll_distance()`。
- **更新**：用 Z-DON 自己的差量更新與伺服器；不附上游指向 YataiDON release 的 `update.ps1`。
- **繪圖**：Z-DON 的繪圖與效能工作建立在 OpenGL（raylib）路線上，不是上游的 bgfx 路線。
- **navigation loader**：沒有編進來，所以沒有上游わいわい與 AI 段位 10 的指定資料夾。
- 來自上游本身的已知畫面差異：鏡像貼圖差 0.5 px、練習模式會播前奏、上游拿掉 Lua 加算混合後部分光暈／煙火效果改變
  （Z-DON 的 skin 可以用 `blend="add"` 再打開）。

試過後放棄的（量過，沒效果或變差）：單一執行緒依序讀檔、Lua bytecode 快取、PGO 發佈、執行時貼圖圖集、編譯過的譜面快取、
輸入時間戳／音訊時鐘內插（會改變判定）、歌曲改串流而不整首解碼。

## 修掉的 bug

多半上游也有：

- 打開歌曲資料夾裡還有子資料夾的分類會當機。
- 同雜湊或同名的歌曲會隨機開到其中一個檔。
- 含假名／韓文的譜面路徑從命令列開不了。
- 練習模式後結束遊戲會當機。
- 時間軸／連打顏色欄位未初始化。
- 段位裡的自動演奏或重播會存成績、升級名牌。
- 30／60 fps 卡頓後自動演奏會吞掉打擊音。
- `#NEXTSONG`／`#NOTATIONOFF` 被當成 `#N`。
- 暫停選單吃到自動演奏留下的鼓輸入。
- AI 選曲用到已釋放的動畫而當機。
- 測試用的隱藏視窗被顯示出來（SDL 忽略了隱藏旗標）。

## 系統需求

- **Windows**：Windows 10 以上（主要開發與測試平台）；OpenGL 3.3，BC7 貼圖需要 4.2
- **Linux／macOS／Android**：CI 會連同 Z-DON 的改動一起建置，但測試比 Windows 少
- **iOS／PS5／Switch**：上游的移植；Z-DON 的改動盡量跨平台，但這裡沒有建置
- **瀏覽器**：支援 WebAssembly 與 AudioWorklet 的新版 Chrome／Edge／Firefox

## 安裝

1. 下載 [Z-DON-windows.zip](https://github.com/GanYiZhong/Z-DON/releases/latest/download/Z-DON-windows.zip)（或到 <https://zdon.taiko.ac>）並解壓縮。
   Z-DON 是 rolling release：沒有版本號，每次發布新版都會覆蓋 [`rolling`](https://github.com/GanYiZhong/Z-DON/releases/tag/rolling) 這個 release。
2. 執行 `YataiDON.exe`。第一次執行時選要下載的 skin。
3. 把 TJA（或 osu!taiko）歌曲放進 `Songs`，或在設定裡指定歌曲資料夾。之後的更新在標題畫面按 F9 就好。

延遲高的話，把設定裡的 `device_type` 改成 `WDM-KS`、`WASAPI` 或 `ASIO`。

## 設定

Z-DON 的 `config.toml` 設定（都可以不寫，沒寫就是預設值）：

| 區段 | 設定 | 預設 | 作用 |
|---|---|---|---|
| `[general]` | `pause_menu` | `true` | 演奏中的暫停選單 |
| `[general]` | `zdon_language_auto` | 新設定檔為 `true` | 跟隨系統語言 |
| `[general]` | `zdon_hit_timing`／`zdon_hit_timing_show` | `true`／`"half"` | 每一下的偏差顯示（`half`、`ok`、`all`） |
| `[general]` | `zdon_song_progress` | `true` | 給 skin 的歌曲進度 |
| `[general]` | `zdon_sort_filter` | `true` | 難度資料夾只列未通過的譜面 |
| `[general]` | `zdon_practice_loop` | `true` | 練習模式的 A–B 循環 |
| `[general]` | `zdon_practice_metronome` | `false` | 練習模式的節拍器 |
| `[general]` | `zdon_lane3d`／`zdon_lane3d_preset` | `false`／`0` | 3D 視角與鏡頭預設 |
| `[general]` | `zdon_hit_error_bar` | `false` | 判定圈下的判定條 |
| `[general]` | `zdon_target_score` | `0` | 目標分數（0 = 關） |
| `[general]` | `zdon_new_best_chip` | `false` | 結算的「新紀錄 +N」 |
| `[general]` | `zdon_hist_auto_open` | `false` | 結算判定面板預設打開 |
| `[general]` | `zdon_ka_mark` | `false` | 咔音符上的白叉 |
| `[general]` | `zdon_remember_difficulty` | `false` | 每首記住難度 |
| `[general]` | `zdon_early_hitsound`／`zdon_press_time_judge`／`zdon_branch_by_measure` | `false` | 打擊時序選項 |
| `[general]` | `zdon_coin_mode`／`zdon_coin_credits` | `false`／`0` | 投幣模式 |
| `[keys]` | `pause_key` | `"space"` | 第二個暫停鍵 |
| `[keys]` | `random_song_key`／`uncleared_song_key` | 未綁定 | 跳到隨機／未通過的歌 |
| `[video]` | `zdon_texture_compression` | `true` | BC7 貼圖快取 |
| `[video]` | `zdon_composited_fullscreen` | `false` | 合成式無邊框全螢幕（Windows） |
| `[network]` | `zdon_server_url`／`zdon_server_key` | `""` | Z-DON 伺服器（空白 = 內建或離線） |
| `[network]` | `zdon_update_check`／`zdon_update_url` | `true`／`""` | 更新檢查與頻道 |
| `[usio]` | `enabled`、`dll`、`threshold`、`thresholds`、`holdoff_ms`、`poll_us` | `false`、`"bnusio.dll"`、`2000`、—、`25`、`1000` | USIO 機台 I/O |

效能相關的功能沒有設定項；若某一項出問題，可以用環境變數關掉，例如 `ZDON_BC7=0`、`ZDON_LAZY_UPLOAD=0`、`ZDON_PREDECODE=0`、
`ZDON_VBLANK_PACING=0`、`ZDON_VIDEO_YUV=0`、`ZDON_PCM16=0`、`YATAIDON_TEXPACK=0`、`YATAIDON_NO_READAHEAD=1`、
`ZDON_HIT_POLYPHONY=1`、`ZDON_NO_MOUSE_MENUS=1`。完整清單見 roadmap。

## 從原始碼建置

### Windows（MSYS2 MinGW64）

先照上游的 [Windows (MSYS2/MinGW64)](<https://github.com/Yonokid/YataiDON/wiki/Windows-(MSYS2-MingW64)>) 裝好相依套件，然後：

```bash
export PATH=/c/msys64/mingw64/bin:$PATH   # 少了這行 gcc 會靜默失敗
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 16
```

輸出在 `build/bin`。

| CMake 選項 | 預設 | 說明 |
|---|---|---|
| `ZDON_LTO` | `ON` | Windows Release 的連結期最佳化（完整連結要幾分鐘；開發時可關，量效能和發佈前要開） |
| `ZDON_PCH` | `ON` | 預編譯標頭 |
| `ZDON_PGO` | `OFF` | Profile-guided optimization（`GEN`／`USE`，見 `tools/build_pgo.sh`）；量不到增益，發佈版不用 |
| `ZDON_FFMPEG_DIR` | — | 改用 `tools/build_ffmpeg_min.sh` 建的只解碼 FFmpeg |
| `ZDON_NO_MANIFEST` | `OFF` | 不放 Windows manifest（segment heap） |
| `YATAIDON_PROFILER` | `OFF` | 編入 [Tracy](https://github.com/wolfpld/tracy) v0.14.1 客戶端 |
| `YATAIDON_USE_SDL2` | `OFF` | 改用 SDL2 |

其他平台見上游 wiki：[Linux](https://github.com/Yonokid/YataiDON/wiki/Linux)、
[macOS](https://github.com/Yonokid/YataiDON/wiki/Mac-OS)、[Android](https://github.com/Yonokid/YataiDON/wiki/Android)、
[iOS](platform/ios/README.md)、[PS5](platform/ps5/README.md)、[Switch](platform/switch/README.md)。

### 格式與檢查

`src/` 的改動要用 clang-format 23.1.3（與上游 CI 相同）：

```bash
find src -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +
```

clang-tidy：設定好 `build/` 之後

```bash
run-clang-tidy -quiet -j 4 -p build "^$(pwd)/src/"
```

### 開發用的環境變數

- `YATAIDON_TEST=1`：靜音、隱藏視窗、忽略真實輸入，用來跑自動測試。
- `YATAIDON_FIXED_STEP_MS`＋`YATAIDON_DUMP_FRAMES`：固定時間步，輸出可逐位元比對的截圖。
- `YATAIDON_FAKE_KEYS`、`YATAIDON_FAKE_MOUSE`、`YATAIDON_JUMP_SCREEN`、`YATAIDON_FORCE_AUTO`：假輸入、直接跳到某畫面、強制自動演奏。
- `YATAIDON_FRAME_STATS=1`：每 2 秒記一行幀時間、CPU 與記憶體；`ZDON_FRAME_SPIKE_MS=<ms>` 記下超過門檻的幀；
  `ZDON_LUA_PROFILE=1` 取樣 Lua；`ZDON_TEX_USE_STATS=1` 列出從沒畫到的貼圖。
- 命令列測試參數：`--auto`、`--2p`、`--dan`、`--ai`。

[`tools/`](tools/) 裡有：錄影、Lua 語法檢查、skin 打包、最小 FFmpeg 建置、伺服器金鑰產生、Android 簽章金鑰。

## 操作

- 演奏中 **Esc**／**空白鍵**：暫停選單；**F1**：重來
- 任何畫面 **Esc**：返回
- 選曲：**空白鍵** 加入最愛、**左右 Ctrl** 一次跳 7 首、**F2** 搜尋、**F5** 重新整理
- 搜尋面板：**Tab** 類型、**F3** 難度、**PgUp/PgDn** 星級、**Enter** 跳到該曲
- 練習（暫停中）：數字＋**Enter** 跳到小節、**F6** 固定分歧、**M** 最後一個失誤
- 結算：**咔** 打開判定分布
- 線上對戰的選單：**Y** 聊天、**R** 檢舉
- 標題畫面 **F9**：更新到最新版
- 選單：左鍵確定、右鍵返回、滾輪移動；3D 視角裡拖曳／滾輪／中鍵移動鏡頭
- 鼓的按鍵可以在遊戲設定或 `config.toml` 裡改

## 回報問題

Z-DON 的問題請開在這個 repo 的 [Issues](https://github.com/GanYiZhong/Z-DON/issues)，不要回報到上游；
上游 YataiDON 本身的問題請到 [YataiDON 的 Issues](https://github.com/Yonokid/YataiDON/issues)。

## 致謝

Z-DON 建立在 [YataiDON](https://github.com/Yonokid/YataiDON) 與它所有貢獻者的成果上：
IID、mc08、QBaraki、KabanFriends、DragonRatTiger、Komi、sumandrew、WallK、dpkgluci、Ape、chuchy（osu parser）、
churrochef（vertical scroll）、cainan（3D don chan）、RyutoSetsujin、Nyannurs、fehpizza（logo）、Radix、Thonktheta、
SomehowScarlet、SunnyDeez。網站的難易度表資料來自 [YataiDON-Syakai](https://github.com/Yonokid/YataiDON-Syakai)。
練習模式參考 PaulSimulator 的特訓模式；部分打擊時序的想法來自 GuminTaiko，TJA／osu!taiko 的擴充參考 OpenTaiko、taiko-web 與 osu!。

使用的函式庫：[raylib](https://www.raylib.com/)、[SDL3](https://github.com/libsdl-org/SDL)、[SQLite](https://www.sqlite.org/)、
[RapidJSON](https://github.com/Tencent/rapidjson)、[toml++](https://github.com/marzer/tomlplusplus)、[spdlog](https://github.com/gabime/spdlog)、
[Lua](https://github.com/marovira/lua)、[sol2](https://github.com/ThePhD/sol2)、[cpptrace](https://github.com/jeremy-rifkin/cpptrace)、
[libsndfile](https://github.com/libsndfile/libsndfile)、[libsamplerate](https://github.com/libsndfile/libsamplerate)、
[FFmpeg](https://ffmpeg.org/)、[PortAudio](http://www.portaudio.com/)、[miniz](https://github.com/richgel999/miniz)、
[zlib-ng](https://github.com/zlib-ng/zlib-ng)、[bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo)、[zstd](https://github.com/facebook/zstd)、
[cgltf](https://github.com/jkuatz/cgltf)、[curl](https://curl.se/)。
