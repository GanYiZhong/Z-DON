# Z-DON roadmap

狀態：2026-10-27。上游同步到 `ef867fb`。所有項目都用確定性證據驗證（見「驗證工具」），數字是當時這台機器量的。

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
| 第一次畫到的貼圖提前上傳（warm-up）：每個「等待上傳的貼圖第一次被畫到」都記成（建立它的畫面, 檔案），存 `cache/warm_upload.txt`（每畫面最多 512 筆、依第一次畫到的順序、換畫面時有變才寫）；畫面載入後，它持有、還在等待上傳、有解碼圖的已記錄貼圖，在每幀 present 之後（限幀本來要睡的時間）照順序上傳，每次 1 MB 一段列（glTexSubImage2D），每幀到 `ZDON_WARM_UPLOAD_MS` 用完為止；貼圖在全部列傳完前 id 維持 0，所以在那之前被畫到就當場補完剩下的列（跟以前一樣同步），畫面逐位元相同。每張配置完先畫一次到 1x1 render texture、完成時再畫一次：NVIDIA 驅動在貼圖第一次被畫到時才整張搬進顯存，不先「碰」的話成本只是從上傳搬到第一次畫它那幀的 batch flush（見量測備註）。換畫面時沒傳完的丟掉（之後照舊 evict），**預設開** | `ZDON_WARM_UPLOAD_MS=<ms>`（預設 1.0，0 = 關、也不記）；`ZDON_LAZY_UPLOAD_LOG=1`（開發）每張第一次畫到的上傳、每次 warm tick 各記一行 | Nijiiro 第一次進選曲（有記錄後）：進難度選擇那幀 12.5–16.6 → 2.2–2.8 ms（3 對 3，同一 exe 開/關），第一次畫到的上傳（載入那幀之後）37 張 15–18 ms → 0；整趟第一次選曲 >10 ms 的幀 2 → 0。warm-up 本身：88 張 84 MB 在 52 幀做完，每幀 p50 1.1、最長 2.1–2.4 ms（都在 present 之後）。載入那幀那 10 張（第一幀就畫，7–10 ms）不變。結算畫面也受惠（43/32 張先傳）。回選曲（retention＋prefill）本來就 0 張第一次上傳，數字不變（textures 60–68 ms、initialized 151–166 ms）。Green 選曲本來就小（第一次進選曲 27 張 5 ms，全在載入那幀）。確定性套件：目前的 config（Green）與改成 Nijiiro 各 121/121 與 HEAD 相同（drum1024 基準拍的時候語言是 zh，現在是 ja，HEAD 自己對它就是 0/121）|
| 學習 Lua 臨時載入的貼圖資料夾：`resolve_lua_texture` 臨時 load_folder 時記下（目前畫面 → 資料夾），存在 `cache/ondemand/<skin>.txt`（每畫面最多 64 筆、有變才寫、換畫面時寫）；下次該畫面載入時跟自己的資料夾一起平行解碼（走 texpack、lazy upload），物件先放暫存區、不放進 `textures`，等 Lua 要時才裝上（時機和覆蓋行為都跟以前一樣），換畫面一起丟。資料夾不存在就從檔案刪掉。每次臨時載入都記一行 info（含耗時），**預設開** | `ZDON_PREFETCH_ONDEMAND=0` 關（不記也不預載） | 選曲進段位轉場那幀 32.9–34.7 → 4.1–5.0 ms（dan_select/global 臨時載入 32.0–33.7 → 1.5–1.8 ms，其餘是第一次畫到的上傳約 2.5 ms）；選曲貼圖載入 147–156 → 157–165 ms（多解一張 1080p）、進選曲那幀 463–540 → 470–493 ms（雜訊內）；確定性套件空檔/有檔各 121/121 相同 |
| 每畫面貼圖打包快取 `cache/texpack/`、zlib-ng PNG 解碼 | `YATAIDON_TEXPACK=0`、`_TEXPACK_MB`、`_TEXPACK_VERIFY=1`、`YATAIDON_FASTPNG=0` | 冷硬碟選曲貼圖 755 → 417 ms；解碼 1.75× |
| 下一個畫面在背景先準備（predecode）：預期會去的畫面的貼圖從它的 texture pack 用 4 個 Background 工作解碼（上限 512 MB），遊戲畫面（載入時上傳的）另外在主執行緒每幀 present 之後用 1 MB 分段先上傳（每幀最多 1 ms，warm-up 的作法）；它的音效也先解（長的 bgm 只解 load_sound 回傳前那 2 秒）。載入時在原本要解碼/上傳的地方拿現成的，每個檔的大小/時間跟載入自己的目錄列表比過、用同一個解碼器，結果相同；載入別的畫面就丟掉，畫面開始後沒用到的釋放。觸發點：選曲打開難度選擇（遊戲；退出難度選擇就取消）、最後一個音符後 1 秒（結算；重來 F1 取消）、標題畫面開始（選曲貼圖，經過 entry 畫面也保留）、entry／結算畫面開始（選曲音效）、段位結算（段位選擇音效）、段位選擇打開確認框（遊戲；取消就丟），**預設開** | `ZDON_PREDECODE=0` 關、`=<n>` 工作數；`ZDON_PREDECODE_MB`；`ZDON_PREUPLOAD_MS`（0 = 不先上傳）；`ZDON_PREDECODE_VERIFY=1`（開發）每個拿現成的音效再解一次比對 | Nijiiro 同一 exe 開/關：選曲→遊戲載入幀 754/630 → 717/454 ms（貼圖 357/348 → 201/197 ms、音效 64/66 → 27/26 ms；剩下的大宗是背景 Lua，每次隨機選一套，兩邊 217–487 ms）；遊戲→結算 159/144 → 83/72 ms；結算→選曲 147/145 → 104/180 ms（音效 65 → 18–25 ms；180 那次貼圖 90 ms，其他次 57–66，單次）；標題→選曲 484 → 432 ms（貼圖 170 → 97 ms）。段位選擇→段位遊戲（Green、固定步進）貼圖 125 → 58 ms、畫面開始 292 → 221 ms。確定性套件 121/121、p2 39/39、danplay 92/92 相同 |
| 3D 角色零件跨畫面重用 | — | 每個 Don 約 30 → 7 ms |
| 選定歌曲就預讀歌曲資料夾、背景影片在背景開 | — | — |
| 段位曲音符數存進 scores.db | — | — |

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
| rlgl 每次批次 flush 少做的 GL 呼叫：只有讀 normal 的 shader 才上傳 normals（預設 2D shader 不讀）；VAO 從上傳到 draw 一路綁著（以前上傳完先解綁再重綁），flush 結束時照舊解綁（`cmake/patches/raylib-lazy-blend.patch`） | — | dense auto 不限 fps、同樣 10 次 flush/幀：GL 驅動執行緒每幀 0.640–0.687 → 0.589–0.630 ms（A 5 次、B 6 次，約 −7%）；180 Hz 一幀一刷新時差異在雜訊內（0.88–0.93 vs 0.87–0.91 ms）；確定性套件 121/121 相同 |
| vblank 對齊限幀（vsync 關、不限 fps 時），**預設開** | `ZDON_VBLANK_PACING=0` 關、`=<n>`、`ZDON_PACING_LOG=<csv>` | 步進 >1 ms 偏差 30% → <1%；代價：165 Hz 上約 170 fps，判定粒度約 6 ms |
| 限幀等待的空轉依實測計時器遲到量調整：高解析度計時器在這台機器遲到 0.03–0.6 ms（p50 0.32，0.5 ms tick），遲到 0.5 ms 內本來就取格點時間，所以只空轉 p99 遲到 − 0.5 + 0.1 ms（0.05–0.5 ms，每 600 次重估，實測 0.18–0.22 ms） | `ZDON_PACING_LOG` 多 `w,<要求喚醒>,<實際喚醒>,<空轉 ms>` | 空轉 40 → 8 ms/s（4.0% → 0.8% 單核）；步進 >1 ms 偏差 0.39/0.50% → 0.44/0.41%，去除卡頓 std 0.046/0.067 → 0.073/0.065 ms；確定性套件 121/121 相同 |
| 影片以 YUV 平面上傳（Y 全尺寸、U/V 色度尺寸的 R8 貼圖），shader 轉 RGB；只支援 8-bit planar YUV（420/422/444/440/411/410 與 yuvj），其他照舊 swscale RGB24。矩陣：標 BT.709/BT.2020 才用，其餘 BT.601（舊路徑全用 601）；範圍：yuvj 或標 full 才全範圍。shader 重現舊畫法（每個影片像素用涵蓋它的色度樣本＝swscale 的最近鄰色度、轉換後夾值，再雙線性、GL_REPEAT 邊緣），**預設開** | `ZDON_VIDEO_YUV=0` 回 RGB；`ZDON_VIDEO_YUV_CHECK=1`（開發）每 30 幀把 shader 結果讀回和 swscale 比 | 對 swscale 每通道最大差 2–3（601 limited）、2–3（709）、1–2（yuvj full）、1（444），差 >2 的像素 ≤0.23%；確定性截圖 movie（720p→1080p）對舊路徑最大 3、平均 0.41，title 最大 3；165 fps 同幀數下：解碼執行緒 12.2/11.1 → 7.8/7.1%、主執行緒 15.9/15.1 → 15.0/13.0%、GL 驅動 15.2/14.0 → 12.5/12.4%、行程 48.9/45.9 → 40.4/36.7%，幀工作 avg 0.87/0.82 → 0.77/0.72 ms、p99 2.3/2.2 → 1.58/1.53 ms |
| 共用工作池取代臨時執行緒、玩家共用 SongParser、貼圖查名不建字串 | — | — |
| TJA 解析加速（指令表、無 regex、view） | — | notes_to_position 1.0–1.1 → 0.64–0.70 s（全曲庫） |
| 全 src 精簡 | — | −2450 行 |

## 2026-10-29 貼圖密度（texture_density，skin 出 2x 圖、4K 繪製）
- skin_config `screen.texture_density`（整數 1–4，預設 1）。d > 1 時：**只有這個 skin 自己的** PNG 與它的 texture.json（x、y、x2、y2、crop）是實體像素，載入時乘 1/d，`TextureObject::width/height`、`get_texture_info` 都是邏輯值，source rect 照樣取整張圖（`TextureObject::density`；呼叫端給的 src 視為邏輯值）；祖先 skin 的貼圖照舊縮放；skin_config、animation.json 維持邏輯座標。
- 每幀畫進 (寬·d)x(高·d) 的 render target（`src/libs/density.*`、ray.h 的 `zdon_density`）：`get_render_width/height` 回報 target 大小，所以 Camera2D（zoom d）、scissor、Chara3D、影片檢查等全部自動是實體大小；巢狀 `end_texture_mode` 回到 target；最後等比例貼進視窗。滑鼠座標換算到 target、IME 區域換回視窗；F12 截圖與 `YATAIDON_DUMP_*` 讀 target（1920x1080、d=2 → 3840x2160）。OutlinedText 以 font_size·d 點陣化、外框 ·d，畫在邏輯大小。
- 驗證：d=1 時 Nijiiro＋Green 六個情境 68 張與改動前逐位元相同；ProbeD2（Nijiiro 子 skin、d=2、無自己的圖）各畫面縮回 1080p 與 Nijiiro 平均差 0.4–2.3/255；自己的 2x 測試圖逐像素 1:1 出現在 4K 畫面（差 0）。演奏中 CPU 端每幀 p50 0.80 → 2.23 ms、private +150 MB（4K target＋2x 字型）。
- 限制：debug menu、IME 文字仍是 raylib 預設字型放大；影片照原解析度放大；只覆寫祖先貼圖位置（無 PNG）的 texture.json 項目座標也算實體，crop 不支援。

## 2026-10-27 效能優化移植到其他平台（`src/libs/zdon_os.*`）
- 背景執行緒：`zdon_os::background_thread()` 取代只有 Windows 的呼叫。Linux nice +10＋I/O best-effort 7（不用 idle class：遊戲有時會等這些讀取）；Android 只 nice +10（ioprio_set 不一定在 app 的 seccomp 白名單，被擋會直接殺 app）；macOS/iOS utility QoS＋`IOPOL_THROTTLE`。job pool 的可逆切換 `job_priority_background()`：macOS/iOS utility QoS 再切回原本的；Linux/Android 只有 RLIMIT_NICE（或 root）允許調回去時才 nice +10（一般使用者降了就調不回來，卡在低優先權的 worker 會拖到遊戲自己的工作），否則不動。WSL 實測：背景執行緒 ni=10 / best-effort 7。
- 限幀與輸入執行緒的精準 sleep：macOS/iOS 用 `mach_wait_until`（iOS 本來就沒有限幀，只有輸入執行緒）；Linux 照舊。
- 曲庫走訪／貼圖資料夾列表：Linux/Android/macOS/iOS 也走跟 Windows 一樣的快速路徑（一般資料夾不 canonical、只有連結才解析；譜面檔帶列表的大小/時間，載入畫面不用每首再 stat 兩次）。`<dirent.h>` 在這個專案的 include 路徑會拿到 raylib 的 Windows 版 shim，所以用 `fs::directory_iterator`（libstdc++/libc++ 都帶 readdir 的 d_type）。時間存成 FILETIME 單位（跟 Windows 同）；Linux/macOS 既有的 texture pack 會重建一次。WSL 對拍：3043 首（ext4 複本、D: 的 drvfs、合成的連結/迴圈/fumen 樹）路徑與順序和舊的可攜版逐筆相同、每個 stamp 的大小/時間正確；暖快取 ext4 舊走訪＋載入 stat 87 ms → 64 ms，drvfs 49–51 s → 30–32 s。
- 預讀：macOS/iOS 用 `F_RDADVISE`（以前整個檔案讀一遍），Linux 照舊 `posix_fadvise`；都移到 `zdon_os::prefetch_file()`。
- Android/iOS：實體記憶體 ≥ 5.5 GB（「6 GB」手機）才預設開 texture pack、lazy upload、predecode、常駐貼圖，上限較小（predecode 128 MB／2 個工作、lazy 256 MB、pack 快取 512 MB、常駐 256 MB）；環境變數照舊可開關。
- Linux 原本就建不起來（Z-DON 自己的 readahead 命名空間撞到 glibc 的 `readahead()`、navigator 在 `_WIN32` 外用了 `_wcsicmp`），一起修掉；WSL Ubuntu 24.04＋g++ 14 建置成功，xvfb 隱藏視窗跑標題、選曲兩次（第二次曲庫 3043 首全從快取、選曲 texture pack 360/360 命中）。
- 確定性套件（Windows，Green）121/121 與 76be414 相同。
- **沒編譯過**：macOS、iOS、Android 的程式（這台沒有 SDK）。
- 沒做：macOS/Linux 的 vblank 限幀（CVDisplayLink/CADisplayLink、DRM vblank 都沒辦法在這裡跑，計時程式不能沒測就上）；依 GPU 速度調 `ZDON_PREUPLOAD_MS`（預算本來就是時間，慢的 GPU 每幀自然傳得少，要調的是幀的餘裕，跟 vblank 限幀綁在一起）；Linux/macOS 的 LibraryIndex（名字是 wstring、navigator 的快速路徑是 Windows 專用）。

## 2026-10-27 預先準備下一個畫面（predecode）：量測備註
- 量法：Nijiiro（暫時改 config、關 skin 更新器，跑完逐位元組還原）、隱藏視窗、`ZDON_FRAME_SPIKE_MS=0.001`；選曲 → 1-2-3 Koi easy auto → 結算 → 選曲兩輪（scratchpad loop.sh）。同一個 exe 用 `ZDON_PREDECODE=0` 對照。
- 遊戲載入幀的組成（關）：貼圖 350–375 ms（845 個檔、406 MB，其中上傳約 165 ms、解碼約 15 ms——解碼本來就跟上傳重疊）、音效 63–70 ms（123 個）、背景 Lua 207–640 ms（每次隨機選一套背景/舞者，量不出來也無法預先知道）、讀譜 0.3–0.5 ms、建 Player 11 ms。只先解碼：貼圖 357/350 ms（幾乎沒變）；加上先上傳：193–201 ms。剩下約 190 ms 是 texture.json／目錄列表兩次（規劃 65 ms、load_folder 再讀一次 json 27 ms）、ImagePrefetch 等待與物件建立，沒動。
- 先上傳：846 張 407 MB 在 168–184 幀做完，總共 194–214 ms，每幀最長 1.5–2.2 ms（在 present 之後、限幀本來要睡的時間）；難度選擇期間每幀 work 沒變（>8.3 ms 0 次）。遊戲中每幀 work：同一 exe 先上傳/不先上傳兩對 p50 0.73/0.52、0.66/0.65 ms——背景隨機加上機器有人在用，差異在雜訊內；loop 兩趟也有 p50 0.7 → 1.2 ms 的一次，沒有重現成穩定差異。
- 結尾（最後 12 s，結算預解在跑）：p50 0.98 → 1.43 / 0.91 → 1.06 ms、p99 1.87 → 2.99 / 1.80 → 2.34 ms，沒有多出 >8.3 ms 的幀。
- 標題：一次 loop 在標題影片播放中有 0.5 s 的 14–33 ms 幀（overlay/update），短情境重跑 4 次（4 個/1 個工作）都沒有（>8.3 ms 0 次），判斷是環境（同時段使用者在用電腦），沒有再追。
- 預覽音（選曲停下時預解附近 2–4 首）沒做：`ZDON_PREVIEW_LOG=1` 量 1-2-3 Koi，盒子打開後 357–367 ms 開始播（350 ms 是故意等開啟動畫），開檔後 5.8–11.3 ms（一兩幀）就有聲音，開檔在主執行緒 1.5–1.7 ms。最多省那幾 ms。
- 段位曲間：下一首讀譜 0.4–0.5 ms；試過最後一個音符後在背景先解下一首音訊的前 2 秒（約 3 ms），換曲那幀（固定步進）沒有：13.5/15.0 vs 14.1/12.3 ms，撤掉；換曲現在記一行各部分時間。
- 演奏中 51 ms draw、57 ms input 那兩幀（10-26 loop_f_on）：都在 2.3 s 內的一串（另有 12 幀 input 5–13 ms），其他次在隨機時間也有同型的串（draw 14–150 ms），從不在同一首的同一處、第二首也沒有。input 段是 SDL_PumpEvents（視窗訊息），draw 是 GL flush 卡住；那時測試視窗其實是顯示中的全螢幕視窗（SDL 忽略隱藏旗標，見 15bbba6），使用者在桌面上的動作會打到它。視窗真正隱藏後 5 趟 loop（10 首）演奏中 input 段最多 1.8 ms、沒有成串的卡頓，draw 段最多 9.2 ms（一次；其餘 ≤6 ms，含載入後第一幀）。剩下的是兩邊都有的 10–15 ms 幀：歌曲開始前約 1.5 s 那幀（各段加起來不到 1 ms，時間不在任何記錄的段裡）、歌曲結束存分數那幀（update 13 ms），沒追。
- 段位選擇→段位遊戲、danplay/p2 畫面比對：用確定性套件的情境（固定步進 20 ms，Green）跑同一 exe 開/關。

## 2026-10-26 結算 → 選曲預解、第一次畫到的上傳：量測備註
- 量法：Nijiiro（暫時改 config 的 skin、關 skin 更新器，跑完逐位元組還原）、隱藏視窗、`ZDON_FRAME_SPIKE_MS=0.001` 記每一幀；選曲 → 1-2-3 Koi easy auto → 結算 → 選曲兩輪。Green 用的假按鍵在即時模式會漏（有一兩輪沒選到歌），所以完整流程用 Nijiiro；skin 更新器開著時選曲不吃按鍵（更新中）。
- 結算畫面背景預解（prefill，2026-10-21 已做）今天重量，同一個 HEAD exe：`ZDON_LAZY_PREFILL=0` 回選曲 textures 188–190 ms、initialized 281–282 ms、載入幀 272–278 ms（載入時重解 478 張 100 ms）；預設開 62 ms、149–154 ms、144–147 ms（484 張早在結算開始後約 190 ms 解完）。進選曲只剩上傳——而 lazy upload 下回選曲那幾幀第一次上傳是 0 張（畫過的貼圖 retention 留著），所以沒有新的東西要做。
- 「選曲 39 張約 30 ms」是整趟第一次選曲（含捲動、難度選擇）的第一次上傳總和，不是前 30 幀：前 30 幀只有載入幀 10 張（7–13 ms）和第 9 幀 3 張（2–2.5 ms，那幀 2.9–3.5 ms 沒超過一個 180 Hz 刷新）；真正掉幀的是打開難度選擇那幀（12 張 9–12 ms，整幀 11.5–18.6 ms）。
- 第一版 warm-up 只做分段上傳：難度選擇那幀還是 11–14.8 ms，時間跑到 `overlay`（EndMode2D 的 batch flush）7–11 ms——NVIDIA 對 glTexImage2D(NULL)＋glTexSubImage2D 的貼圖，第一次被畫到時才整張搬進顯存。改成整張 LoadTextureFromImage 提前傳：那幀 2.0–4.3 ms，但 warm tick 最長 10–13 ms（一次 12 MB）。只在傳完時畫一次到 1x1：那幀 2.1–2.3 ms，但搬顯存的成本落在 tick（最長 8–15 ms）。**配置完先畫（空的就搬進顯存），之後的分段直接進顯存**：那幀 2.2–2.8 ms，tick 最長 2.1–2.4 ms、總共 47–60 ms（比整張上傳的 88 ms 還少）。
- 代價：記錄裡的貼圖這次不一定會畫到（例如別的類別背景），照樣先傳：記錄累積到 99 張時一趟傳 88 張 84 MB 顯存（retention 預算本來就把等待中的貼圖算成已上傳，記帳不變）；傳完的貼圖不再是等待上傳，換畫面時不會被 evict，回選曲要預解的少了（484 → 433 張），RAM 反而少。
- spike 記的 work 只到 present 為止，warm tick 在 present 之後，所以 tick 的時間另外記（`ahead-of-use uploads ran in N frames, longest X ms`）。

## 2026-10-21 資源用量四項：量測備註
- 限幀空轉：直接寫 console 小程式量 `CreateWaitableTimerExW(HIGH_RESOLUTION)`，遲到 p50 0.33、p99 0.61 ms；提高執行緒優先權或 `NtSetTimerResolution(0.5 ms)` 都一樣（看起來是 0.5 ms 的 tick），所以縮到 0.1 ms 的固定空轉會讓 2.4% 的喚醒超過 0.5 ms、步進 std 0.069 → 0.14 ms；改成依 p99 調整。原本估的 8% 單核實際是 4%（timer 平均已遲到 0.32 ms，只剩一半要空轉）。主執行緒 CPU 量起來雜訊太大（同版本 22–31%），所以用 log 直接算空轉時間。
- 未畫過的貼圖：只是「不保留在 retention 裡」只省 83 MB——約 300 MB 的物件（box/background 107 MB、yellow_box/s_crown_* 93 MB、diff_select/difficulty_back 34 MB…）同時被 Lua 腳本拿著，物件不會死；所以改成釋放影像、物件留著、下次載入補回。`KEEP_MB` 先留小圖：留 126 MB（457 張小圖）只把重解從 89 ms 降到 67–71 ms，大圖才是時間大宗，所以預設全放。進選曲時的重解是 CPU 解碼量（約 2.6 GB/s 輸出）決定的，要再省得在結算畫面背景先解——已做（prefill，1b6483d，見上表與「2026-10-26」）。
- 影片：雙線性色度（含 left siting）在 720p 測試片的色塊邊緣和 swscale 差到 255（平均約 2），720p→1080p 畫面上差到 100；最近鄰色度直接取樣也只在原尺寸吻合，放大時色度邊緣比舊的銳利（差到 100）。最後在 shader 裡手動做「四個像素各自轉 RGB 再雙線性」，才和舊路徑放大後也只差 3。
- 影片 CPU 要固定幀數比：預設的 vblank 限幀在 YUV 版因為幀工作變短、p99.5 變小，每個刷新多塞了輸入幀（165 → 266–330 fps），每秒 CPU 反而變高（每幀 CPU 2.9–3.1 → 1.8–2.1 ms）。上表用 `ZDON_VBLANK_PACING=1`。
- 標 BT.709 的影片舊路徑用 601 解，新路徑照標記，所以畫面會變（測試片 max 109、平均 5）；未標記的照舊 601。
- 外洩：標題畫面 attract 循環 6 分鐘（19 支影片，含 1080p 420 與 444），working set 242–310 MB、private 356–427 MB 來回、GPU dedicated 142–145 MB，沒有成長。

### 功能
| 項目 | 開關 | 結果 |
|---|---|---|
| 一般演奏（單人 GameScreen、2P）的暫停選單：Esc（back_key）或空白鍵（pause_key）打開；歌曲時鐘、音樂、背景影片、音符都停住（Lua 背景、Player 都不 update）。左右框（或方向鍵）移動「繼續演奏／從頭演奏／選其他歌曲」，咚（或 Enter）確定，再按 Esc／空白鍵＝繼續。繼續前倒數 3-2-1（1.8 s），倒數中按 Esc／空白鍵再暫停；暫停與倒數期間按的鍵在恢復時全部丟掉（clear_input_buffers），所以不會有免費判定；F1 照舊重來。外觀沿用 skin 練習選單的 practice/menu_panel、menu_button(_selected) 貼圖（沒有就引擎畫純色面板），文字先找 skin 的 pause_menu_*，再找練習選單的 practice_menu_restart / practice_menu_another_song，最後內建 en/ja/zh/ko；不改任何 skin。只在開場轉場結束後、歌曲結束前、沒跳過時可暫停，其他時候 Esc 照舊直接離開。視窗失焦也會打開（測試模式不會，只在「有焦點 → 失焦」那一刻）。段位（game_dan）維持原樣（考試規則下暫停有疑慮）；練習模式、輸入校正不受影響。歌曲結束時 log 一行最終判定數 | `config.toml` `[general] pause_menu = true`（預設）；`false` = Esc 直接離開、2P 空白鍵照舊單純暫停 | dense 譜 auto：暫停兩次（4.80 s、2.80 s）時鐘停在 17761.8 / 32969.0 ms 原值恢復；結果 good 999 ok 0 bad 0 drumroll 64，與沒暫停的三次（drumroll 63/64/64、其中一次 ok 1，auto 本身就會差一下）相同。混音輸出錄音：暫停期間除了選單音效全靜音，恢復後與沒暫停的錄音對齊，延遲差＝暫停時間 +11 ms、+7 ms（相關係數 0.57–0.85）。手動（非 auto）暫停＋倒數期間按 15 下咚：good 0 ok 0 bad 999，與沒按相同；恢復後按的照常判定。重來、選其他歌曲、`pause_menu = false` 都驗過。確定性套件 121/121 相同 |
| 只用太鼓（4 個輸入、框移動／咚決定，不用同按或長按手勢）也能操作（2026-10-24）：①練習模式「另一位玩家」的暫停／選單／速度／標記結束只認與遊玩者同角色按鍵不同的綁定（出廠 `[gamepad_2p]`＝`[gamepad_1p]`，原本每一下都觸發）。②設定：抓鍵／抓手把按鈕後 200 ms 內丟掉輸入，被抓的那一下不再重新進入抓鍵或移動游標；1P 鼓的四個角色不能綁到別的角色已用的鍵（值框顯示「已被占用」1.5 s、原值不變）；按鍵設定、手把 1P/2P 每箱最後多一列「恢復默认」（咚→否/是，框選、咚確定，值同 config.cpp 預設）；音量限 0–100%；音頻偏移左右框 −1/+1 ms，校準獨立成下一列；數值項按住單邊框重複（400 ms 後 100/50/20 ms）。③畫面上的太鼓字元選擇器（32b5f5d）使用者 10-26 要求取消，已撤回；設定文字欄「咚離開時寫回舊值」也一起撤回（沒有選擇器時，咚離開會把打鼓的 F/D/J/K 一起存進去，反而更糟；鍵盤打完按 Enter 本來就會存）。新開發掛鉤 `YATAIDON_JUMP_SCREEN=settings`、`YATAIDON_FAKE_KEYS_RAW=1`（假鍵也進每幀鍵盤佇列，測抓鍵用） | — | 假鍵截圖：抓 D 後游標不動、抓 J 被拒、恢復預設把 A 改回 D；音頻偏移 +3／−2、音量 25 下停在 100%。真鼓、手把未測。確定性套件見 commit |

### 修掉的 bug（多半上游也有）
- 打開「歌曲資料夾還有子資料夾」的類別會當機（scan_song_tree 懸空參考）。
- 同 hash 譜面對應的檔案隨執行緒競賽而變；同名歌曲（例如段位用的 A Cruel Angel's Thesis 新/舊音源）每次啟動選到不同檔案。
- 命令列/捷徑開不了含假名/韓文的譜面路徑（argv 是 ANSI 字碼頁）。
- 練習模式結束遊戲必當（PracticeTimer 在 Lua 關閉後才釋放）。
- `TimelineObject::end_time`、`Player::last_drumroll_color_time` 未初始化。
- 自動演奏/回放打段位也會記錄段位成績並升級名牌。

### 開發工具
- `YATAIDON_TEST=1`：音量 1%、視窗建立時就隱藏（不會出現在畫面上、不會搶前景；config 要全螢幕/無邊框時改成「無邊框、螢幕大小」的隱藏視窗，畫面與全螢幕相同）、忽略真實輸入、不寫歌單。raylib 的 SDL 平台原本忽略 `FLAG_WINDOW_HIDDEN`（`cmake/patches/raylib-sdl-hidden-window.patch`），所以以前的「隱藏視窗」（DUMP_SCREEN/DUMP_FRAMES）其實都是顯示、取得焦點的全螢幕視窗。2026-10-27 驗證：確定性套件 121/121 與 bd16149（顯示的視窗）相同；套件跑完全程每 20 ms 查 GetForegroundWindow／IsWindowVisible，遊戲視窗從沒顯示、從沒在前景。
- `YATAIDON_FIXED_STEP_MS=<ms>`＋`YATAIDON_DUMP_FRAMES=<dir>;<frames>`：確定性畫面（固定時間步、等背景載入、無網路），同一個 exe 跑出逐位元組相同的截圖。
- `YATAIDON_JUMP_SCREEN`、`YATAIDON_FAKE_KEYS`、`YATAIDON_FORCE_AUTO`、`YATAIDON_DUMP_SCREEN`、`YATAIDON_DUMP_CHARA`。
- `YATAIDON_FRAME_STATS=1`：每 2 s 一行 `[frame]`：幀工作時間 avg/p50/p99/max、幀間隔（開始到開始）超過 2.08 / 4.17 / 8.33 ms 的次數（480 / 240 / 120 Hz 的一格）、程序 CPU（單核 %，Windows）、working set 與 private MB。每 2 s 才查一次系統（GetProcessTimes＋GetProcessMemoryInfo），預設關。
- `ZDON_FRAME_SPIKE_MS=<ms>`：每個超過門檻的幀記一行（update/draw 拆開），present（EndDrawing＋swap）超過門檻也另記；不需要開 FRAME_STATS。
- `ZDON_PREVIEW_LOG=1`：選曲預覽音開檔多久、開檔後多久開始播。
- `tools/zdon_syntax_check.py`（單檔 -fsyntax-only）、`tools/build_pgo.sh`（PGO，量不到增益，預設不用）、預編譯標頭 `ZDON_PCH`。

## 2026-10-22 GL 驅動執行緒與輸入卡頓：量測備註
- 量法：dense 譜 auto、隱藏視窗、視窗模式 vsync 關不限 fps（＝vblank 限幀＋不撕裂），skin 更新器關；`thread_cpu.py` 取第 12–27 s 每個執行緒的 CPU，換算成每幀 ms。同一張譜每次啟動隨機選到的背景不同，每幀 flush 會是 10 或 16（整場固定），比較時只拿 flush 數相同的。
- 預設（不撕裂、一刷新一幀，180 Hz）：主執行緒 14–22%、nvoglv64 驅動執行緒 15–18%（每幀約 0.85–1.0 ms）。每幀 10–16 次 flush、65–90 次 draw call、約 12.7 次混合模式切換、約 5 次 3D mesh draw。
- 不撕裂的 vsync swap 沒有在驅動裡空轉：同樣一刷新一幀，vsync swap（`ZDON_VBLANK_PACING=1`）驅動 15.8–16.8% vs 不 vsync（再加 `ZDON_PACING_TEARFREE=0`）13.0–15.5%，差約 0.1 ms/幀。`TEARFREE=0` 自動模式一刷新兩幀（360 fps）：主 23%、驅動 20%。
- **設定 `vsync = true`（不限 fps、限幀關）時主執行緒吃滿一核（97.6%）**：每幀 5.4 ms 的「work」全部在 screen draw 裡（第一次 flush 時驅動等前一幀翻頁，忙等），驅動執行緒 15%。不撕裂模式（限幀睡到 vblank 前才畫）沒有這個問題。要不要讓 vsync=true 也走限幀，留給使用者決定（會改變 vsync 模式的出幀時機，沒做）。
- NVIDIA「執行緒最佳化」是驅動設定（自動）：有一次不限 fps 的量測整場沒有驅動執行緒（主 0.97 ms/幀、總 CPU 最低），其餘有（主 0.8–0.9＋驅動 0.6–0.7 ms/幀）。只回報，沒改系統設定。
- 輸入卡頓：自動測試（隱藏視窗、沒有真實輸入）演奏中 45 s 沒有 >1 ms 的 input 段；可重現的都在視窗建立後約 2 s 內與選曲剛開時：`SDL_PumpEvents` 裡 Windows 文字服務（TSF）的 `WM_TIMER`（TimerProc 在 MSCTF.dll）每次約 5 ms CPU，加上 `SystemUserAdapterWindowClass`／`MSCTFIME UI` 視窗的訊息，第一幀 pump 45–55 ms、之後 5–15 ms。主執行緒在建立視窗前 `ImmDisableIME` 就全部消失（第一幀 4 ms、之後 ≤4 ms 且沒有訊息），但會讓搜尋框不能用輸入法打中日文，沒做。對自己的視窗 PostMessage 按鍵（每秒 15 下）：約 2% 的按鍵讓 pump 多 1–3 ms（多半在等，CPU 只 0.2–0.5 Mcycles），有無 IME 都一樣。使用者實際遊玩看到的 8–11 ms 沒有在自動測試重現，最可能也是 TSF（同樣的訊息型態），要在真實視窗用診斷版確認。
- 限制每幀處理的訊息數不會有幫助：卡住的是單一則 WM_TIMER 的處理。
- 演奏中關 TSF（`ITfThreadMgrEx::Deactivate`，離開時 `ActivateEx`，38420fb）已撤回（b3078ae）：使用者實測打完歌回選曲，搜尋框注音打不了中文（新的 activation 接不回 IMM32 那條）。增益也只有直接開譜時視窗剛建立那一下（pump 75→25 ms），演奏穩定後本來就沒有 TSF 訊息。ImmAssociateContextEx(NULL)、AssociateFocus(NULL) 都是空操作（SDL 停止文字輸入時已斷開），SuspendKeystrokeHandling、ImmDisableTextFrameService 無效。
- 輸入法選字框自己畫（`src/libs/ime_ui.cpp`）：全螢幕時 OS 的組字/選字視窗被遊戲視窗蓋住；設 `SDL_HINT_IME_IMPLEMENTED_UI=composition,candidates`，由 SDL 事件畫組字（底線＋游標）與候選清單（選中反白、換頁時顯示頁碼），錨在搜尋框/設定欄位下方，並 `SDL_SetTextInputArea`；另修 raylib 只收一次提交的第一個字（整詞提交只剩首字）。開發鉤子 `YATAIDON_FAKE_IME=<f1>,<f2>`。

## 2026-10-22 vsync 開也用 vblank 限幀
- vsync 開（config `vsync = true`、沒有 target_fps）時，NVIDIA GL 驅動在下一幀第一次 flush 時忙等翻頁：主執行緒吃 95.6% 單核、每幀 work 5.46 ms（180 Hz 一個刷新 5.56 ms）。
- 改成 vsync 開也啟用限幀：每次刷新一幀（同一般 vsync），先睡到刷新前才開始畫、swap 用驅動 vsync（不撕裂）。`ZDON_VBLANK_PACING=0` 回舊行為。
- 量測（dense auto、180 Hz、隱藏視窗）：主執行緒 95.6% → 20.8%、程序 113% → 36%；180 fps 不變；幀步偏差 >1 ms 0.04%、40 s 錯過刷新 2 次。確定性套件 121/121。

## 2026-10-21 vblank 限幀改成不撕裂（ZDON_PACING_TEARFREE）
- 使用者回報：三種視窗模式都看到一條橫線切過音符。原因：驅動直接翻頁（independent flip／NVIDIA GL 走 DXGI）時，vsync 關的每次 swap 都會撕裂，撕裂線落在 swap 當下的掃描行；用 ZDON_PACING_LOG 換算，舊版限幀的 swap 集中在畫面高度 10–70%（音符軌道）。原設計假設 DWM 合成（只顯示每次刷新最新的一幀），不成立。
- 改法（預設開，`ZDON_PACING_TEARFREE=0` 回舊行為）：限幀啟用時打開驅動 vsync；每次刷新只有要顯示的那一幀 swap，額外的（輸入）幀照跑輸入/update/判定/繪圖但不 swap；work 量到 swap 之前為止（vsync 等待不算 work，不然 lead 會一直長）。
- 量測（dense auto、180 Hz、隱藏視窗、45 s）：幀步偏差 >1 ms 11.9% → 5.1%；錯過刷新 2 → 11 次；work p99 2.06 → 3.13 ms。確定性套件 121/121。實際是否還撕裂、順不順要使用者開視窗確認。

## wip-all（YataiDON 本機分支）稽核，2026-10-19
- wip-all HEAD 對它最後合併的上游 479816a 只剩 24 個檔案的差異（+1227/−72），逐項對照 Z-DON；另外 172 個本機 commit 全部分類。
- 低吻合（<70%）的 123 個：84 個已在上游或 Z-DON（形式不同）、25 個在 wip-all 內被撤回/取代、11 個是撤回的 skin 文字標籤、**skin 用得到但 Z-DON 缺的 = 0**。
- 補搬過 dbc9e69（skin 更新器在 checksums 清單過期時照樣寫入完整下載，8ac153d）；合併上游 ef867fb 時改用上游的 skin 更新器，這項跟著拿掉（上游版遇到下載內容和清單不符時保留本機檔）。不搬：YATAIDON_SYNC_LOG（開發用）。
- 完整分類表在 scratchpad arch/impl_wip_port.md。

## 合併上游 bde4201 後的已知畫面差異（都是上游自己的改動）
- 練習模式：f2e1174 起會播前奏（以前第一幀就跳過約 1 s 前奏）。純上游 f2e1174 前/後與 Z-DON 合併前/後各自 19/19 相同。
- 練習模式按空白鍵不會暫停（上游就沒有 pause_key 處理，只有 2P 咚/咔），未改。

## 合併上游 PR #326–#333（2026-10-23，使用者指示：所有 PR 都收進 Z-DON）
- 已有、no-op 合併：#326 整個名稱比對指令／譜面中的 NAME:value 行／P1・P2 各自的 #END、#333 TimelineObject 預設值、#327 STYLE 各段的 BALLOON/SCOREINIT/SCOREDIFF（全保留 Z-DON 的指令桶比對與無 regex 寫法）。
- 新收：#332 `COURSE:Ura` 也找得到 Edit 譜面的音符；#331 find_chart 掃到該 course 結束，Single 在前的 `#START P1/P2` 2P 也找得到；#328 BALLOONNOR/EXP/MAS 只填自己分歧的清單、不再接到／取代 `balloon`（取代上面 ef867fb 時「沒有自己清單的分歧用上游語意的 balloon」那條）；#330 skin 更新器暫存檔＋rename、連線重用、k / n 進度。
- 驗證：全曲庫 3043 首＋合成譜（P0/P1/P2）音符比對：P0 只有合成 Ura 譜，和 3 首實際歌曲（Bataille Decisive、41 no Sorezore no Mirai、Pan vs Gohan Daikessen）的 course `balloon` 欄位變（每顆氣球次數不變）；P1/P2 變動的 122／223 個 course 全都是「Single 之後還有 #START P1/P2」的譜。確定性套件 121/121 與 merge1023 相同。

## 合併上游 ef867fb（#277 響度、onboarding＋skin 更新器、#329 資料表重構、clang-tidy #325、SDL2 相容、#323/#324）
- 使用者決定三個衝突區都用上游版：
  - 響度：用上游 #277 的 `loudness.*`（每首歌一個 `cache/loudness/*.loudness` 檔）；Z-DON 的單一 store 檔（462741a）拿掉，舊的 `cache/loudness/loudness_store.txt` 不再讀寫（留在磁碟上，上游格式的每首檔一直都在）。API 一樣，mixer／漸進解碼／常駐音效照舊乘 `normalization_gain`。
  - skin 更新器：用上游 30da305 之後的 `skin_updater.*`（`download()`／`in_progress()`／`failed`、每個 skin 16 個下載執行緒）。Z-DON 的三方規則保留本機修改、`.zdon-local-backup`、暫存檔＋rename、8 執行緒並行、k / n 進度、`ZDON_SKIN_UPDATE_KEEP_LOCAL`／`_THREADS`／`_ONLY`、ETag 清單快取（`cache/skin_hashes/*.manifest`、`*.base` 不再使用）、過期清單照寫（8ac153d）全部拿掉。上游 PR #330（暫存檔＋rename、連線重用、k / n 進度）2026-10-23 依使用者指示（所有 PR 都收進 Z-DON）合併進來，見「合併上游 PR #326–#333」。
  - TJA：上游的數值欄位表（BPM/OFFSET/DEMOSTART/MOVIEOFFSET）、COURSE 別名表、BALLOON 規則表照收，LEVEL 用上游寫法；Z-DON 的 `float_field_or_zero` 拿掉。保留：整個名稱比對指令、譜面中間的標頭行、P1/P2 各自的 #END、STYLE 各段的 BALLOON/SCOREINIT/SCOREDIFF、OpenTaiko 字母、指令表／無 regex。每個分歧自己的 BALLOONNOR/EXP/MAS 接在上游的規則表後面（同一份解析結果也存一份到該分歧）；沒有自己清單的分歧改用上游語意的 `balloon`（NOR/EXP 接在後面、MAS 取代），以前是只看 BALLOON。全曲庫 3043 首（P0/P1/P2）音符與每顆氣球次數和合併前 Z-DON 相同，只有 3 首的 course `balloon` 欄位變了；合成譜 pr_c（BALLOON＋NOR＋MAS、沒有 EXP）的 EXP 分歧氣球 22 → 8（＝純上游）。
- 其他衝突照 Z-DON 版、上游的新寫法跟著套（`find_rtaudio_backend`、`get_preset_volume`、`draw_rectangle`、`yd_key_scancode`、Lua 矩形不再重複縮放 a5de3b3、default member init）。新的 clang-tidy 檢查（default-member-init、nullptr、override、else-after-return、redundant-lambda-parameter-list、performance-*）套到 Z-DON 自己的程式；navigator.cpp 的 `const Work item = work[i]` 是故意複製（scan_dir 會 append），加 NOLINT。
- 確定性套件 121/121 與合併前（upd1022）相同；AUDIO_SELFTEST 輸出 wav 和合併前逐位元組相同。

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
- 選曲時預解附近歌曲的預覽音（2026-10-27 量：開檔後一兩幀就有聲音，最多省幾 ms）；段位下一首音訊先解（量不到差異）；預先建 Player／讀譜（讀譜 0.4 ms、Player 11 ms 多半是主執行緒的物件）；遊戲背景那套圖（skin 用 math.random 選，無法預先知道）。

## 2026-10-20 音訊四項驗證結果
- 確定性套件 121/121 與合併上游後（up_5e1b）相同。
- AUDIO_SELFTEST：舊腳本的部分與舊版逐位元相同；複音段對模型最大誤差 9.7e-8（重播模型差 0.82）；stop/unload 後全靜音；即時音量段誤差 0（長音效要用開頭就有聲音的檔，例如自產正弦波）。
- dense 譜 auto＋AUDIO_STATS：callback 平均約 10 µs、最壞 0.25 ms，沒有 underflow。

## 編譯譜面快取（分支 zdon-chartcache，不合併）
- 正確性：全曲庫 12116 個難度序列化/還原逐欄位相同，損毀/截斷/多執行緒都測過。
- 效能：每難度中位數 37.5 µs（解析）vs 35.4 µs（快取命中），42% 的難度命中反而較慢；光是把結果複製成 deque 就約 17 µs。快取檔整個曲庫 94 MB。收益感覺不到，不合併。

## 2026-10-30 給 skin 的遊玩資料（src/libs/zdon_lua.cpp）
- `tex.last_hit(pn)`：最近一擊的偏差 ms（+＝晚）、發生時間、良／可判定窗；`tex.hit_offsets(pn)`：整首每擊偏差（結算用）；開關 `[general] zdon_hit_timing`（預設開）。
- `tex.song_progress(pn)`：譜面時間、最後一顆音符時間、GOGO 段落（含達人分歧內的 GOGO）；開關 `[general] zdon_song_progress`（預設開）。
- `tex.best_score(pn)`：這次遊玩前存的最佳分（結算時才有值）；`SongBox:chart_stats(diff)`：音符數／長度／BPM 範圍，第一次要求時解析並存在 box 上。
- 自動演奏預設不記偏差；測試 UI 時設 `ZDON_RECORD_AUTO_OFFSETS=1`。
- `[general] zdon_hit_timing_show`（half／ok／all）：`tex.last_hit` 第 5 個回傳值＝快/慢門檻。`chart_stats` 改在 jobs 背景算（第一次回空值）。段位遊玩也更新 `zdon_song_pos_ms`。
- 逐張貼圖密度：texture.json 項目加 `"density": n` 覆寫 skin 的 texture_density（n 小於 skin 時用 point filter）。SimpleStyle 的生成器把「2×2 完全同色」的圖存成 1 倍（528 張）並用 `frame_order` 去掉重複動畫格（675 檔）：109 格逐像素相同；冷硬碟（G:）遊玩畫面貼圖 1.6–2.0 s → 1.0–1.3 s、global 0.56–0.74 s → 0.32 s，遊玩畫面 VRAM 1128 → 353 MB。被引擎以次像素位置畫的貼圖（lane cover）point filter 會出接縫，要排除。上游沒有這些函式，skin 要先檢查存在才呼叫（YataiDONSimpleStyle 就是這樣做）。

## 2026-10-31 判定建議、難度資料夾篩選、最近成績、區段循環
- 判定時機建議：`tex.offset_suggestion(pn)` → 建議改量（整首偏差中位數，≥50 擊且 |中位數|≥3 ms 才給）、目前 audio_offset、是否已套用。結算畫面按左/右咔一次就把 audio_offset 加上建議值並存 config（每局一次）。
- 難度資料夾（#COLLECTION:DIFFICULTY）`[general] zdon_sort_filter`（預設開）：只列未達成的譜面；關掉＝上游行為（未達成排前面）。
- 最近成績：`SongBox:recent_plays(diff)` → 最多 10 筆（舊→新）{score, crown, good, ok, bad, played_at}，讀 scores 表，refresh_scores 時清快取。
- 練習模式區段循環 `[general] zdon_practice_loop`（預設開）：從某個跳躍點繼續時，若後面還有跳躍點，就在 A→B 之間循環（到 B 記下這一輪良/可/不可、輪數 +1、跳回 A）；暫停即結束。`tex.practice_loop()` → active, a, b, passes, 上一輪良/可/不可。已用假按鍵隱藏測試驗證（小節 1–4 連跑 4 輪）。

## 2026-10-31 AI 對戰（src/objects/game/zdon_ai.*）
- 參考 CHN05 AiEnsoInput／AiManager 的設計（數值自訂，不搬資料表）：每顆音符從常態分布抽敲擊時間，超出不可窗或「失手」機率＝漏打；標準差由等級（1: 34 ms … 10: 16 ms）起算，隨玩家良/可的偏差往玩家的平均/標準差靠（最多 70%，64 筆滑動）；區段內 AI 領先 6 分以上變鬆（σ×1.5、失手×2）、落後變緊（σ×0.7、失手×0.5），差距 ≤1 回到持平；連打/氣球 10+等級 下/秒。
- 區段：開始第一幀依 don/ka 音符數平均切 3 段（≥200 顆切 5 段）；每段良 2 可 1，分高者勝；段數多者勝、同段數比總分。
- 入口：`[general] zdon_ai_battle`（預設關）＋`zdon_ai_level`（1–10）→ 1P 選曲進 GAME_2P，2P lane 由 AI 打（P2 鍵盤無效）；CLI `--ai`（加 `--auto` 1P 自動）。AI 不存分數、結算名牌/角色用 AI 專屬資料，結束回 1P 選曲。`ZDON_AI_SEED` 固定亂數（測試用）。
- Lua：`tex.ai_battle()` → table（level、state、section、count、live_p/live_ai、wins_p/wins_ai、total_*、finished、result、sections[]）。
- 上游的 Screens::AI_GAME / PlayerNum::AI 是空殼，沒用（skin 版面沿用 GAME_2P）。

## 還可以做的
- 載入時 texture.json 讀兩次（規劃一次、load_folder 一次，遊戲畫面約 27 ms）、目錄列表約 65 ms：可以在 predecode 時先做或在一次載入內共用，沒做。
- PBO／共用 context 上傳執行緒：第一次畫到的同步上傳已用 warm-up 搬到前幾幀的空檔（見上表），剩載入那幀就畫到的那幾張（Nijiiro 選曲 10 張 7–10 ms，在 480–560 ms 的載入幀裡）。要再省只能把它們跟解碼重疊（載入時上傳）或另開 GL context，收益是載入幀的 1–2%，沒做。
- `PlaybackClock` 與 vblank 限幀：已讓 synchronize 用實測的刷新週期（02f375d）；更深的整合會改變演奏時鐘，等於判定行為改動，不做。
- 譜面音符改成不用 deque：約 90 處參考/迭代器語意要改，每幀的迭代早已走 vector，收益小，不做。
