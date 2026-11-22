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

## 2026-11-22 記憶體：16-bit 音效、模式用不到的資料夾、global/背景與選曲貼圖

量測：Nijiiro、BC7 開（預設）、命令列開歌第 25 秒；private＝提交量，ws＝working set。

- 先查清楚記憶體去哪：上傳貼圖的位元組在 Windows（WDDM）會再算進行程的提交量約 1.25 倍（測試程式：256 MB RGBA 上傳 private +316–331 MB，glTexImage2D／glTexStorage2D／PBO 都一樣），所以只能減少上傳的量；演奏中 Lua 堆積只有約 3 MB；最大的自有項目是解碼後的音效（float）。
- **音效存 16-bit**（`ZDON_PCM16=0` 回 float）：整段解完的音效依峰值縮放存成 int16（峰值 >1 時照峰值，不會削波），8 秒以上邊播邊解的（歌曲、bgm）留 2 倍餘量；混音時把縮放併進音量。`YATAIDON_AUDIO_SELFTEST` 比對 float：播放狀態紀錄相同，樣本差最大 −99 dBFS、均方根 −118 dBFS。演奏中 private 674–688 → 582–603 MB、ws 473–489 → 368–395 MB。
- **演奏模式用不到的音效資料夾不載入**：跟貼圖的 deferred_game_subsets 一樣，教學以外不載 `tutorial`（Nijiiro 19 MB），演奏對戰以外不載 `enso_battle`（PyTaikoGreen 81 個檔 28 MB）；貼圖也多延後 `enso_battle`（13 張都大於 1 MB，以前一律載入時上傳 26 MB）。`ZDON_DEFER_MODE_FOLDERS=0` 全載。選曲預讀用 `next_game_mode`（補上 BATTLE_SELECT → BATTLE_GAME）。演奏音效 255 → 158 個檔，演奏貼圖載入時上傳 108 → 83 MB；private → 496–520 MB、ws → 313–322 MB。
- **global 與背景貼圖也等第一次畫到**：演奏貼圖那套規則（沒畫過、≤1 MB 才等）擴大到 global 和 background；BC7 圖等待時不留 RAM。global 載入時上傳 35 → 19 MB。private → 433–481 MB、ws → 293–322 MB。同一首歌 FRAME_STATS 每 2 s 最長一幀：舊 30–48 ms、新 12–14 ms，>4 ms 的視窗 7–9 vs 6–7。
- **其他畫面沒畫過的 BC7 圖也不留 RAM**：選曲 1246 張等待上傳的貼圖只有 18 張畫過（會提前上傳），其餘 BC7 的影像載入後就釋放、第一次畫到時從 BC7 快取讀回。選曲畫面 private 690–705 → 555–565 MB、ws 533–539 → 397–402 MB；捲動 12 s 最長一幀 4.7/86 → 5.4/7.7 ms。
- 合計（演奏中）：private 約 680 → 455 MB（−33%）、ws 約 480 → 305 MB（−37%）。
- 驗證：固定步進截圖（Nijiiro、PyTaikoGreen；演奏／暫停／練習／結算，BC7 關）44 張與改前逐像素相同；BC7 開的標題／選曲／演奏／暫停另比（見下）。
- 評估後不做：Nijiiro `gogo_time/explosion.png`（2352×3810，30 格，RGBA 34 MB，演奏中最大的一張）壓不過 BC7 門檻：bc7enc PSNR 44.2 dB（門檻 45）、最大誤差 162；basis_universal 的 bc7e 純 C++ 版（全部 8 種模式）各預設 44.2–44.8 dB、最大誤差 147–173，只算看得到的像素約 38 dB，不放寬。30 格沒有重複，各格裁掉透明邊只剩 42% 面積：改用上面的 trim_crop_atlas。
- **字型用檔案對映**（`zdon_os::FileBytes`：Windows MapViewOfFile、POSIX mmap；web、Android 資產、對映失敗時照舊讀進記憶體）：skin 字型（Nijiiro 26 + 4 MB）以前整個讀進 private，現在是檔案頁（mapped 14 → 41 MB）。開檔帶 FILE_SHARE_DELETE；遊戲執行中替換六個字型檔（os.replace，跟 skin 更新器的「寫暫存再改名」一樣）都成功；更新器改名失敗時另外會先把舊檔改名移開再放新檔（較舊 Windows 的保險）。標題／選曲／演奏／結算截圖（兩個 skin）46 張與改前逐像素相同。
- **預解碼的音效也存 16-bit**：選曲進難度選擇時預解碼的演奏音效 49.6 → 24.8 MB；`ZDON_PREDECODE_VERIFY=1` 152 個全部與重新解碼相同。
- **大張裁切圖集去掉空白邊**（`trim_crop_atlas`，`ZDON_TRIM_ATLAS_MB=<n>`，預設 8、0 = 關）：≥ 8 MB 的 RGBA 圖、texture.json 用 `crop` 切格、只被一個項目用時，每格只留非零像素外擴 2 像素的範圍（邊上的像素是 0，跟被丟掉的部分一樣），由高到矮成列打包；draw_texture 依原本 crop 的取樣對應只畫留下的那塊（鏡像、旋轉中心照算；呼叫端自帶 src 的不畫並記一次 log）。這種圖載入時就上傳（不走延後上傳／預先上傳，避免從原檔讀回時尺寸不合）。Nijiiro explosion 2352×3810 → 2352×1994（34.2 → 17.9 MB 顯存）。GOGO 煙火固定步進 41 幀：37 幀逐像素相同，4 幀最多 2/255（取樣座標的浮點差，最多約 8000 像素）；PyTaikoGreen 41 幀相同。
- **長音效第一次用到才載入**（`ZDON_LAZY_LONG_SOUNDS=0` 關）：畫面載入時，超過 8 秒、不常駐的音效只記下檔名，第一次播放／has_sound／設定音量等／取長度／跳轉時才載入（照舊先解前 2 秒、其餘背景解）；stop、is_playing、播放位置不會觸發載入。演奏畫面（screen "game"，所有演奏模式）照舊全部先載，避免歌曲中途解碼。原因：Nijiiro 的 parent 是 PyTaikoGreen，選曲會連 PyTaikoGreen `Sounds/song_select/` 底下 AI 對戰故事與導覽的音樂一起解（battle_story_*、ai_intro_*，約 85 MB int16），標題也會解 gacha_bgm（14 MB）。選曲 private 484 → 約 397 MB（同一版關掉比）；選曲 bgm 第一次播放時載入約 8 ms。錄影（含結算 bgm）開／關兩版音訊重疊部分逐樣本相同。
- 錄影偶發卡住（影片停寫、音訊幾乎不動，part 停在某一格）：開／關這項都各遇到一次，之後同一版連錄 7 次都正常，舊版也正常；看起來是編碼器那邊，不是這項。再遇到時可用 scratchpad 的 rechang.sh 做法（狀態檔 30 秒不動就 gdb 印所有執行緒）。
- 選曲 → 演奏（同一首直接開歌比）多約 140 MB：不是常駐貼圖（`YATAIDON_RETAIN_MB=0` 一樣）、不是預解碼（`ZDON_PREDECODE=0` 一樣）、也不是已釋放沒還的堆積（HeapCompact 沒變）；演奏中使用中的堆積 118 vs 63 MB，多的是曲庫（3022 首）與選曲導覽器的歌曲框（每首約 9–16 KB，打開的資料夾約 650 個），以及一首 4 分多鐘的歌本身 54 MB（int16）。導覽器保留是為了回選曲時不用重建，不動。歌曲也維持整首解進記憶體（int16，一般 2 分鐘約 25 MB）：演奏的時間基準是 `get_sound_time_played`，練習模式要變速與任意跳轉，改成 music stream 的 ring 邊播邊解要重做變速與同步，硬碟忙時（載入、冷硬碟）還可能斷音；省 25–55 MB（約 6–13%）不值得冒判定時間的風險，不做。開發工具 `ZDON_HEAP_HIST=1`（Windows）：每個畫面開始時依大小分組記錄堆積使用中的區塊與幾個區塊的開頭內容。
- **依譜面延後資料夾**：單曲模式（GAME／GAME_2P／GAME_PRACTICE／AI_GAME）載入演奏貼圖前讀選到的 .tja 原文（依檔案與修改時間快取）：沒有 #GOGOSTART 就延後 `gogo_time`（Nijiiro 18 MB），音符行沒有 9 就延後 `kusudama`（18 MB），沒有 7 也沒有 9 就延後 `balloon`（6 MB）；所有難度一起算，不是 .tja 的照舊全載，被要到時當場載入（load_deferred）。曲庫 3022 首：58% 沒有薬玉、14% 沒有氣球與薬玉、8% 沒有 GOGO。366Nichi：演奏載入時上傳 66 → 48 MB、private 423–457 → 412–419 MB；整首 27 張固定步進截圖與改前相同，沒有任何延後的資料夾被要求。
- 再查過、不動的：選曲時 8–16 KB 的區塊（每首歌一個歌曲框）停在選曲 13 s 與 34 s 進演奏時都是約 650 個，是打開的資料夾、不是洩漏；曲庫載入各段的使用中堆積：走訪後 57 MB、對快取後 82 MB（含暫時的快取表）、導覽器預載後 72 MB、SongLibrary 標題索引 +9 MB（搜尋／排序／難度資料夾的導覽要用，延後建立得留著同一份來源資料，省不到）；標題／選曲時 4.6 MB 的 animations.glb 原檔與 23 MB 的標題畫面大區塊，進演奏前都已釋放；SQLite 頁面快取上限 2 MB。

## 2026-11-21 演奏貼圖用到才上傳、記憶體

- 演奏畫面以前 984 張貼圖（Nijiiro）全部載入時上傳（歌曲中第一次畫到才傳會掉幀），但一整首歌只畫到約 170 張。現在「之前載入這個畫面時沒畫過」（cache/warm_upload.txt）且上傳量 ≤1 MB（BC7 1024×1024、RGBA 512×512；`ZDON_LAZY_GAME_MAX_KB`）的貼圖等第一次畫到才上傳；畫過的照舊載入時上傳，歌曲中不做提前上傳（量過：提前上傳在歌曲中一幀卡 196 ms）。其中 BC7 的影像載入完就不留在 RAM，第一次畫到時從 BC7 快取讀回（cached_image，不解 PNG）。`ZDON_LAZY_GAMEPLAY=0` 關。
- 量測（Nijiiro、BC7 開、歌曲第 25 秒、各 3 次）：載入時上傳的演奏貼圖 198 → 90 MB；private 813–829 → 661–686 MB、working set 479–550 → 454–465 MB（862 張 104 MB 不留 RAM）。第一次玩（清單空）100 s 內 61 張讀回、單張最多 0.9 ms，最忙的一幀 3.2 ms 是演奏第一幀（載入幀），其後開場 1–2 ms，沒有失敗。演奏畫面貼圖載入（命令列開歌、4 次）中位數約 144 → 117 ms、第一幀約 337 → 298 ms。
- 驗證：演奏／練習／暫停（Nijiiro、PyTaikoGreen；BC7 關與開；有無清單）截圖與改動前逐像素相同（BC7 開時要快取齊全：第一輪舊版有 4 張當場才編）。AI 對戰每次本來就不同，不比。
- Lua 剖析（`ZDON_LUA_PROFILE=1`，`src/libs/zdon_lua_profile.*`，預設關）：每 1000 條 VM 指令取樣正在跑的行、計數所有 C 函式呼叫，每 10 s 記一次前幾名。Nijiiro 演奏中一半以上的 Lua 取樣在街機背景動畫的 `packed_channel` 解碼（每秒 6 萬次呼叫）：動畫 60 Hz、畫面 180–240 Hz，同一個動畫幀每次畫都重解。Nijiiro skin（YataiDON/Skins/YataiDONNijiiro 3717599）`bg_anim`／`donbg/anim_player` 改成每個動畫幀解一次、留到幀號改變。固定步進 5 ms 同一背景：每幀解碼 488 → 138 次、draw_texture 不變（115／109），主執行緒每幀工作 p50 約 0.32 → 0.28 ms（4 次交替）；20 ms 與 5 ms 步進截圖（含連續幀）逐像素相同。只把這兩個檔套在已發佈的版本上 publish-skin（已發佈版本與 skin repo HEAD 差很多，不整個發）。
- GPU 計時（`ZDON_GPU_TIMER=1`，`src/libs/zdon_gpu_timer.*`，預設關）：每幀在開始畫、畫面繪製前後、present 前打 GL_TIMESTAMP，每 2 s 記平均。Nijiiro 演奏、不限幀（`ZDON_VBLANK_PACING=0`）：GPU 每幀繪製只有 0.04–0.06 ms，主執行緒 work 約 0.2 ms，而一幀從開始到下一幀開始 0.6–1.2 ms，其餘是 present（SwapBuffers／DWM）本身。不限幀的 FPS 上限在系統的畫面交換，不在 GPU 或遊戲邏輯；限幀（預設）時就是螢幕刷新率。
- 查過沒改的：主執行緒每幀工作演奏約 0.25 ms、選曲約 0.5 ms，exe 內一半以上是 skin 的 Lua（luaV_execute、表查詢），引擎的 draw 參數解析已是 lua_next 走訪；限幀等待的實際忙等平均 0.016 ms/幀；不限幀時主執行緒大多在核心等翻頁（win32u），FPS 卡在 GPU/present。計時器解析度（NtSetTimerResolution／timeBeginPeriod）對高解析度 waitable timer 的晚醒沒有影響（p50 0.29、p90 0.49 ms）。BC7 高品質編碼（uber 1–4）讓保留 RGBA 的圖過門檻：1519 張裡只多 1 張，不做。

## 2026-11-21 安裝包縮小：只解碼的 FFmpeg、發版 strip

- **FFmpeg**：`tools/build_ffmpeg_min.sh` 用 BtbN 同一個 FFmpeg commit（`3d16b774d7`，ABI 相同）在 MSYS2 編只解碼的版本：全部解碼器、demuxer、parser，只開 file/pipe 協定，沒有編碼器、muxer、濾鏡、裝置、網路；AV1 用靜態連結的 dav1d（跟 BtbN 一樣），zlib 也靜態。CMake `-DZDON_FFMPEG_DIR=<prefix>` 改用它（空的話照舊抓 BtbN）。五個 DLL 150 → 24 MB（avcodec 120.5 → 18.7、avformat 23.1 → 1.6）。
  - 驗證：36 個影音檔（skin 宣傳 mp4、歌曲 ogg、skin 音效，以及產生的 VP8/VP9/AV1/HEVC/H.264 4:4:4/MPEG-2/MPEG-4/WMV2/Theora、MP3/FLAC/Opus/AAC/WAV）逐幀解碼的雜湊：影像全部逐位元相同；Opus、WMA 浮點輸出有 76/144000 個樣本差在最低位（最大 1.5e-8，約 −156 dB，轉 16-bit 後相同）。解碼 CPU 時間相同或更少（少載入 120 MB DLL）。遊戲截圖（PyTaikoGreen、Nijiiro 的標題/選曲/演奏 34 張）對 BtbN 版逐像素相同。
  - 一次測試 Nijiiro 標題/選曲結束時少了「Game closed」、exit 1，之後新舊各重跑 15 次都沒再出現，看起來是偶發的關閉競態，不是這次改動造成。
- **exe 符號表**：YataiDON.exe 有 10.5 MB COFF 符號表。`server/zdon_server.py publish` 發版時 `strip --strip-unneeded`（33.8 → 23.2 MB），原本那份存到 `zdon_data/symbols/<版本>/`。當機紀錄多印每一幀的「模組＋位址」（cpptrace object_address，已扣掉重定位），strip 過的版本沒有函式名時用 `nm -C --numeric-sort` 對那份 exe 查。
- 安裝包（package.zip）91 → 41.3 MB（20261121-1831-e411d60a 實際發佈）。
- 更新下載（dfbaa01a）：`publish` 替每個 ≥64 KB、gzip 能省 10% 以上的更新檔多放 `files/<rel>.gz`（不在 manifest、不進 package.zip），`zdon_update.ps1`／`.sh` 先要 `.gz`、沒有就退回原檔（`-NoGzip`／`--no-gzip` 關）。Cloudflare 不壓縮 .exe/.dll，以前每次更新 YataiDON.exe 整個 23.3 MB，現在 9.7 MB；avcodec 18.7 → 8.1 MB。本機頻道測過兩支腳本的首次安裝、更新、-NoGzip，檔案逐位元相同、不留 .gz。舊版更新程式照舊抓原檔。
- Linux／macOS（ddcf6da9）：`tools/build_ffmpeg_min_posix.sh`（同一個 FFmpeg commit、全部解碼器、靜態、dav1d），workflow 依腳本 hash 快取，`YATAIDON_FFMPEG_PREFIX` 的靜態分支多連 prefix 的 pkg-config 列出的相依（zlib、dav1d、macOS framework）。Linux 套件 178 → 57 MB、macOS 54 → 42 MB（以前帶著發行版 FFmpeg 拉進來的 x265、codec2、SVT-AV1、aom、rsvg、jxl…）。workflow 多一個 `upload` 輸入（關＝只編譯，不動 rolling release）。
- 評估後不做：`-O2` 取代 `-O3`（stripped exe 23.3 → 21.6 MB，但每幀工作沒有比較快：O3 中位數約 0.28 ms、O2 約 0.31 ms）；package.zip 壓縮等級 9（41.35 → 41.13 MB、壓縮時間 3.2 → 8.8 s）；Android APK 改壓縮 .so（下載少約 20 MB，但手機上要多存一份解壓的 .so）。

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

## 合併上游 0ed42ec（2026-11-06：AI 對戰、わいわい、商店、總結算、活動、Switch、clang-tidy 修正）
- 45 個衝突檔，全在 src/；決定處在程式裡標 `merge 0ed42ec`，完整紀錄在 scratchpad zdon_merge_result_20261106.md。
- AI 對戰兩套並存：Z-DON 的改名 `ZdonAiSongSelectScreen`；YataiDON.cpp 的畫面工廠在 skin 有 `ai_support_menu` Lua 時建上游的 `AISongSelectScreen`（→ AI_GAME → AI_RESULT，要連線後端），否則建 Z-DON 的（→ GAME_2P / RESULT_2P）。Z-DON 的 skin 沒有那個腳本，走 Z-DON 的；上游 Green 98bf4532 實測會進上游的 AI 開場。`Game2PScreen::select_screen()` 的預設是 Z-DON 的規則。
- 選曲掃描：保留 Z-DON 的 walker、清單 stamp、LibraryIndex、原子游標；加上游的 song_list.txt／box.def 收集與掃描快照、`song_metadata_from`、`platform_file_stamp`、SongLibrary 填充。照上游寫法放在 worker 裡會讓 3008 首（全快取）的掃描慢約 500 ms（每首 `is_library_song_file` 在 scores_mutex 裡 stat 一次），改成用清單判斷＋背景 job 填 library／排序快照；之後 LTO 版 Song scan 中位數 306 ms（合併前 281 ms，暖機、6 次）。
- 上游的わいわい、AI rank 10 指定曲資料夾要上游的 navigation loader，Z-DON 的 navigator 沒建這兩種資料夾（Z-DON skin 也沒有這些腳本）。
- text.cpp 以 Z-DON 的字型快取為底，加上游的 generation 計數與描邊文字貼圖快取（key 含 texture density）。
- 特訓：小節跳轉改成到頭／尾停住（上游的跳轉鈕會變灰），保留 Z-DON 的連按步數。
- clang-tidy：只套在 Z-DON 的行；redundant-nested-if（括號壞掉）、redundant-member-init（`{}` 被拿掉＝未初始化）、use-string-view（回傳拼接字串）三種修正不收。
- 畫面比對（固定步長、同按鍵）：SimpleStyle 標題→結算 115/115 相同；Green 101/115，差異只在鏡像貼圖（右側煙火、結算右下花）＝上游 texture.cpp 拿掉鏡像時的 -0.5 裁切偏移；Nijiiro 差異同理（背景舞者鏡像，每幀 ≤0.14%）。特訓 3 組、錄影（8001 幀、長度相同）與合併前相同；AI 對戰 Green／Nijiiro 選曲→對戰→勝利→結算→段位頁都走完（AI 調子每次執行隨機，無法逐幀比）。

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
- ~~判定時機建議~~（2026-10-31 使用者要求移除：結算畫面不再提示、不改 audio_offset；`tex.offset_suggestion` 已拿掉）。
- 難度資料夾（#COLLECTION:DIFFICULTY）`[general] zdon_sort_filter`（預設開）：只列未達成的譜面；關掉＝上游行為（未達成排前面）。
- 最近成績：`SongBox:recent_plays(diff)` → 最多 10 筆（舊→新）{score, crown, good, ok, bad, played_at}，讀 scores 表，refresh_scores 時清快取。
- 練習模式區段循環 `[general] zdon_practice_loop`（預設開）：從某個跳躍點繼續時，若後面還有跳躍點，就在 A→B 之間循環（到 B 記下這一輪良/可/不可、輪數 +1、跳回 A）；暫停即結束。`tex.practice_loop()` → active, a, b, passes, 上一輪良/可/不可。已用假按鍵隱藏測試驗證（小節 1–4 連跑 4 輪）。

## 2026-10-31 AI 對戰（src/objects/game/zdon_ai.*）
- 參考 CHN05 AiEnsoInput／AiManager 的設計（數值自訂，不搬資料表）：每顆音符從常態分布抽敲擊時間，超出不可窗或「失手」機率＝漏打；標準差由等級（1: 34 ms … 10: 16 ms）起算，隨玩家良/可的偏差往玩家的平均/標準差靠（最多 70%，64 筆滑動）；區段內 AI 領先 6 分以上變鬆（σ×1.5、失手×2）、落後變緊（σ×0.7、失手×0.5），差距 ≤1 回到持平；連打/氣球 10+等級 下/秒。
- 區段：開始第一幀依 don/ka 音符數平均切 3 段（≥200 顆切 5 段）；每段良 2 可 1，分高者勝；段數多者勝、同段數比總分。
- 入口（照街機：主選單選模式，不是設定開關）：entry 的模式板「AI 對戰」（skin 文字 `entry_ai_battle`，沒有就不出板子；兩人入場時灰掉）→ `Screens::AI_SELECT`（AiSongSelectScreen，選曲 skin、不能 2P 加入）→ GAME_2P，2P lane 由 AI 打（P2 鍵盤無效）；CLI `--ai`（加 `--auto` 1P 自動）。AI 不存分數、結算名牌/角色用 AI 專屬資料，結束回 AI 選曲。`ZDON_AI_SEED` 固定亂數（測試用）。
- 等級 `[general] zdon_ai_level`（1–10，預設 5）：每場贏 +1、輸 −1 並存檔（auto 不動）；`tex.ai_level()` 給選曲畫面顯示。
- Lua：`tex.ai_battle()` → table（level、state、section、count、live_p/live_ai、wins_p/wins_ai、total_*、finished、result、sections[]）。
- 上游的 Screens::AI_GAME / PlayerNum::AI 是空殼，沒用（遊玩版面沿用 GAME_2P）；AI_SELECT 是 PyTaiko 留下的名字，Nijiiro 的 coin_overlay 與 entry 說明（entry_ai_battle_comment_*）已認得。

## 2026-10-31 特訓（參考 PaulSimulator 的特訓模式）、等速、成績旗標、搜尋面板
- 共用：`objects/global/zdon_hud.*`（引擎自己畫的小面板＋快取文字，日英中韓內建字串，skin 的 `zdon_<key>` 文字可覆寫），所以任何 skin 都看得到。
- 特訓暫停時：數字鍵輸入小節號（Enter 跳、Backspace 刪、Esc 取消，輸入框開著時鼓不作用）；F6 把整首固定成 普通→玄人→達人→自動（分歧譜才有；重新開始仍保留）；音符上方顯示每一下的偏差 ms（藍＝早、橘＝晚，兩排錯開）。
- 循環：A→B 之外的音符變灰（`Player::zdon_dim_range`）；只有 A 沒有後面的點＝A 到曲末循環（曲末＋1.5 s 跳回 A，`zdon_loop.to_end`）；每次繼續/循環至少 2 秒預備（往前多退幾小節）、音樂 0.9 秒淡入。
- 資訊列：目前 BPM（倍速時顯示換算後）、分歧狀態、暫停時的按鍵說明；預設左下，skin 可用 `"zdon_practice_info": {"x", "y"}` 指定左上角（Nijiiro 放在上方紫帶）。
- 等速＝選曲的演奏選項多一列「等速」（使用者要求放 song option、不用熱鍵）：每個玩家各自記住，存在 Z-DON 自己的表 `zdon_player_options`（key equal_speed，不加 players 欄位，上游 scores.db 不受影響）；開著時每顆音符改用 #BPM、#SCROLL 1，#BMSCROLL/#HBSCROLL 譜也一樣改成一般捲動（判定時間不變），演奏畫面右下角「等速」標籤。Nijiiro 面板多一列時把 7 列的框圖再往下接一段。
- 成績有效旗標：`GameScreen::zdon_unsaved_reasons()` 集中判斷（自動演奏［中途開過也算］、回放、等速）→ 不存並記在 `session_data.zdon_unsaved`，結算畫面在該玩家下方顯示「本次成績不保存（…）」；段位道場也用同一個判斷（不存、不算段位）。
- 選曲搜尋面板：搜尋框開著時（輪盤上任何地方按 F2 也能開）即時列出最多 8 首（標題、類型、難度★），Tab 類型、F3 難度、PgUp/PgDn 星級、↑↓ 選、Enter 直接跳到那首（沒選時 Enter 照舊開搜尋資料夾；只用篩選沒打字時 Enter 跳第一首）。
- 測試鉤子：`YATAIDON_FAKE_KEYS` 補上 raylib 特殊鍵（Esc/Enter/Tab/Backspace/方向/PgUp/PgDn/F1–F12）轉 SDL；要讓 `key_pressed` 收到要加 `YATAIDON_FAKE_KEYS_RAW=1`。

## 2026-11-01 判定分布、隱藏/突現、自己最佳、搜尋滑鼠、特訓鼓手勢
- 結算判定分布：有記錄偏差時（自動演奏預設不記）結算右下角提示「咔：判定分布」，該玩家敲咔開/關面板：可窗內的偏差直方圖（良窗內金色）、0 線與平均線、平均/標準差、早/晚數；開著時不會自動離開結算。skin 可用 `"zdon_hit_histogram": {x, y}` 指定 1P 面板左上角（2P 版面固定兩側）。
- 選項面板新增兩列（存在 `zdon_player_options`：`fade` 0–6、`ghost`）：「隱藏／突現」關、隱藏 25/50/75%（音符在離判定線該比例的跑道處淡出）、突現 25/50/75%（音符到剩該比例才淡入），淡化帶 8% 跑道寬，只作用在咚/咔與其文字（連打、氣球照常）；存分照常（跟ドロン一樣是限制）。「自己最佳」開：演奏中跑道下方右側顯示「自己最佳 ±N」＝目前分數減掉最佳那次在同一時刻的分數（2P 版面 1P 的放在跑道上方）；skin 可用 `"zdon_ghost": {x, y}` 指定。每次遊玩都記分數曲線（分數改變時記一點），存分且分數高於舊最佳時寫進 `zdon_ghost`（hash, difficulty, player_id, "ms:分數;…"）；自動演奏不顯示、不寫。只有一般/2P 遊玩（段位、特訓不用）。
- Nijiiro 選項框：列數多於 7 時上移並重畫框的列段（上回已做）；這回修正 selector.lua 在面板沒開時 `modifier_offset()` 為 nil 的算術錯誤（上回引入，每幀報錯）。SimpleStyle 的面板是逐列畫的，產生器把 Nijiiro 的重畫段拿掉、只保留上移。
- 搜尋面板滑鼠：滑過結果列＝選取（滑鼠有移動才改，不搶鍵盤的選取），點擊＝跳到該曲，滾輪上下移選取；篩選列改成三個可點的小塊（左鍵下一個、右鍵上一個）。Ctrl+V 貼上原本就有（poll_text_edit）。
- 特訓暫停時的鼓操作：有分歧的譜，左右咚 80 ms 內同時打＝切分歧（同 F6）；單邊咚在 80 ms 後才繼續（沒分歧的譜照舊立刻繼續）。同方向咔在 250 ms 內連敲：第 4 下起每下 4 小節、第 8 下起 8 小節。暫停時資訊列多一行「小節 n／總數」。
- 測試鉤子：`YATAIDON_FAKE_MOUSE="<ms>:<x>:<y>:<按鍵>:<滾輪>,..."`（render px，按鍵 0＝只移動），之後 mouse_position 回報該位置。

- 自動演奏卡頓補打：拖曳視窗等造成主迴圈卡住時，時鐘一次跳過好幾顆音符與連打。Player::update 在 auto 時把跳過的區間以 4 ms 一步重跑 note_manager/timeline/autoplay（補打不出聲）。測試鉤子 ZDON_TEST_STALL="<歌曲 ms>:<ms>,..."、ZDON_NO_AUTO_CATCHUP=1（舊行為）。即時跑 4 次卡頓（0.25–1.5 s）：dense 舊 977/1/21 → 新 999/0/0；longroll 舊 156/0/12、連打 73 → 新 168/0/0、連打 82（同無卡頓）。

## 還可以做的
- 載入時的 texture.json／目錄列表：2026-11-02 已做（見下方「2026-11-02 效能檢查」）。剩下規劃約 20–40 ms 多半是第一次解析 JSON，同一場遊戲第二次起走快取。
- PBO／共用 context 上傳執行緒：第一次畫到的同步上傳已用 warm-up 搬到前幾幀的空檔（見上表），剩載入那幀就畫到的那幾張（Nijiiro 選曲 10 張 7–10 ms，在 480–560 ms 的載入幀裡）。要再省只能把它們跟解碼重疊（載入時上傳）或另開 GL context，收益是載入幀的 1–2%，沒做。
- `PlaybackClock` 與 vblank 限幀：已讓 synchronize 用實測的刷新週期（02f375d）；更深的整合會改變演奏時鐘，等於判定行為改動，不做。
- 譜面音符改成不用 deque：約 90 處參考/迭代器語意要改，每幀的迭代早已走 vector，收益小，不做。

## 2026-11-01 BC7 壓縮貼圖（texture_bc7）

- 占用分析（SimpleStyle、2560×1440 無邊框、不限幀率，隱藏即時跑）：選曲 ws 779 / private 915 MB / 顯存 538 MB；演奏 private 1315 / 顯存 958 MB。程式本體約 100 MB，其餘幾乎都是貼圖：共用 270 MB、演奏 355 MB（解碼後 RGBA），private 約貼圖的 1.7–2 倍（推測 NVIDIA GL 驅動在系統記憶體另留一份）。CPU 每幀工作約 1 ms。
- 做法：skin 貼圖解碼後在 load_image 結尾轉 BC7（GL_COMPRESSED_RGBA_BPTC_UNORM，raylib patch `cmake/patches/raylib-bptc.patch` 新增 PIXELFORMAT_COMPRESSED_BPTC_RGBA）。編碼器 bc7enc（bc7enc_rdo b943862，只編 bc7enc.cpp/bc7decomp.cpp）。每張圖編完逐 block 解回比對，可見像素任一通道誤差 > 4/255 就整張留 RGBA。
- 快取 `cache/bc7/textures.bin`：以解碼後像素的 64-bit hash 為 key（任何 skin/子 skin/改過的檔都對得上），blocks 以 deflate 存、用到才從檔案讀。沒命中時這次照傳 RGBA，Background 工作編碼，下次載入才用（第一次載入不變慢）。開關 `[video] zdon_texture_compression`（預設 true）、`ZDON_BC7=0/1`；GL 沒有 4.2 / ARB_texture_compression_bptc 就自動關。
- 透明邊界（opaque_right 用）：壓縮前從 RGBA 算，用 blocks 的 hash 記住；查不到就解 blocks 算。
- 結果（第二次啟動、快取命中）：選曲 ws 779→300 MB、private 915→525、顯存 538→266；演奏 private 1318→690、顯存 958→384；共用貼圖 270→72 MB、演奏 355→95 MB。SimpleStyle 全 skin 1177→303 MB，快取檔 3.3 MB。固定步進截圖 on/off 比對：99.99% 像素差 ≤2/255、最大 5/255，版面不變；crazylover 魔王 auto 全良。
- Nijiiro（漸層/手繪）在同樣門檻下只有 8.8% 的位元組過關（整體 PSNR 51.7 dB）。使用者決定放寬：門檻改成「每像素 ≤ 4/255，或整張 PSNR ≥ 45 dB（MSE ≤ 2.06，同樣以 alpha 加權）」，快取版本 2（舊快取自動重來）。Nijiiro 實測：選曲 ws 714→368、private 875→470、顯存 172→106 MB；演奏 private 1128→765、顯存 767→415 MB；快取檔約 30 MB（全 skin 估 131 MB，上限 256 MB）。固定步進截圖：演奏約 0.3% 像素差 > 8/255、最大 90，放大 4 倍目視分不出；分數/連段相同。
- 未解：Nijiiro 選曲 BC7 關閉的即時隱藏跑，7 次有 2 次在進選曲約 1 秒後無聲結束（log 停在 nameplate 載入、沒有 Game closed、Event Log 無當機紀錄），之後 5 次重現不了；BC7 開著的 6 次都正常。

## 2026-11-01 AI 對戰卡頓補打、沒畫到的貼圖

- AI 對戰的 2P 也走自動演奏的卡頓補打（Player::update 的補打迴圈多跑 zdon_ai_manager，補打中不出打擊音）。dense 譜 AI Lv5、`ZDON_AI_SEED=7`、`ZDON_TEST_STALL=4000:300,9000:700,15000:1500,22000:250`：沒卡頓 良 749/可 224/不可 26；卡頓、修正前 732/225/42；修正後 749/224/26（與沒卡頓相同）。
- 量測鉤子 `ZDON_TEX_USE_STATS=1`：每次換畫面（unload_textures）記下已上傳卻從沒交給 GL 的貼圖，依資料夾排（`[tex_use]`）。crazylover 魔王 auto 一首：SimpleStyle 演奏畫面已上傳 133 MB 中 109 MB 沒畫到、Nijiiro 132 MB 中 84 MB。大宗：
  - SimpleStyle 吉祥物 `global/ss_mascot_k3/k6/k9`（22 套服裝 × 3 尺寸，共用貼圖的 50 MB/72 MB，每個畫面都在）：產生器改成一個精靈一個資料夾 `Graphics/ss_mascot/k<k>_<name>`，沒有畫面會載入它，Lua（ss_ui.lua `UI.TM`）要畫時才由 resolve_lua_texture 載入，texture_ondemand 下次跟著畫面預解碼（第二次起不再臨時載入）。共用貼圖 72→18 MB；選曲 ws 300→256、private 525→428、顯存 266→177；演奏 private 690→599、顯存 384→301。BC7 關閉時固定步進截圖與改前逐位元相同。
  - 2026-11-02 已做（practice 除外，見下方「2026-11-02 效能檢查」）。原註：只在特定模式用的資料夾（practice、ai_battle、gauge_ai、dan_info、gauge_dan、SimpleStyle 的 ss_dan）在一般演奏也載入：SimpleStyle 約 33 MB、Nijiiro 約 18 MB（BC7 後）。要按模式略過得逐一確認各模式的 Lua 不會在 init 就查這些圖。其餘（balloon、kusudama、branch、各結尾動畫）看歌，不能略過。
- density 2 畫到 3840×2160 再縮到螢幕：畫布本身 33 MB，改成螢幕大小的收益小（1440p 省約 18 MB），沒做。

## 2026-11-01 錄影：指定歌曲/難度直接輸出影片（zdon_record）

- `YataiDON.exe <song.tja> [難度] --record <out.mp4> [--auto] [--record-fps 60] [--record-result 6]`；沒 `--auto` = 完全不敲。`tools/record_video.bat <tja> [難度] [auto|nohit] [out]`（拖 .tja 上去＝魔王自動，輸出到 exe 旁的 Records），bat 也複製到 build/bin。
- 做法：測試模式（YATAIDON_TEST：隱藏視窗、忽略真實輸入、失焦不暫停、不寫歌單）＋固定步進 1000/fps ms；每幀畫完讀回視窗（3 個 PBO 輪流、晚 2 幀取出，ffmpeg `-vf vflip`）→ 背景執行緒寫 ffmpeg stdin（h264_nvenc 直接吃 RGBA，否則 libx264）；音訊裝置暫停，主執行緒每幀叫 `AudioEngine::dev_mix_offline` 混剛好一幀的樣本（先等串流 ring、邊解邊播的長音效）寫 float WAV，最後 AAC mux。視窗＝skin 畫面大小（不全螢幕），audio_offset/visual_offset 歸 0、master 音量 100%（各類音量照設定），`zdon_config_read_only` 讓 save_config 不寫、成績因 "record" 不存、結算不畫「未保存」。失敗 exit code 1。
- 實測 1080p60：crazylover 魔王 239 s 影片 74 s 錄完（約 3.2×）；每幀遊戲 3.1 ms、讀回 0.2、等 ffmpeg 1.6–2。同步：click 音軌測試譜（每 0.5 s 一顆）30/60/120 fps 打擊音都在 click 後 0.3 ms 內；一般譜面音符不在格上時晚最多一格（60 fps 0–16.7 ms）。
- 抓到自動演奏補打的 bug：門檻 8 ms 讓一般 60 fps（16.7 ms）每幀都進補打，補打中不出聲 → 自動演奏 60 fps 約 7 成、30 fps 全部打擊音被吃掉（判定都良所以之前沒發現）。改成間隔 > 50 ms 才補打；卡頓測試結果不變（1P 999/0/0、AI 749/224/26）。上游 PR #352 同樣問題，修正 commit 在 YataiDON_autocatch（b7b1c6c，待使用者同意才推）。

## 2026-11-01 錄影多軌（--record-tracks）

- 使用者要求：同一首歌多個難度同時錄，2 個＝1P＋2P，3 個以上＝x 條 1P，最多 5。`--record-tracks 4,3,2,1,0`（第一個在最上面/1P；可配 --auto 或不敲）。
- 2 個：走 Game2PScreen（1P 第一個難度、2P 第二個，不套 --2p 的強制自動），一樣分 3 段並行；ANTIHERO 31 s。
- 3–5 個：上游 draw_players 雖然能把 N 個 1P 平均排開，但 skin 的頭像、魂量表、分數面板、飛向魂量表的音符都照 1P 固定位置畫，疊成一團（SimpleStyle 實測）。改成合成：每個難度各錄一支 1P（同時跑、每支一個子程序，用 part 機制留下 video＋WAV），`--record-sync-end <最長難度最後音符 ms>` 讓每支的歌同時結束（GameScreen 結束時間取 max），結算畫面不錄（result 0）；協調程序 ffprobe 取畫面大小，裁出 720p 座標 y 136–360 的帶狀（魂量表＋軌道，lane 在 184、高 176）由上往下 vstack，超過畫面高就縮到畫面高、置中疊在第一個難度的模糊背景上，SimpleStyle、Nijiiro 都對得剛好。聲音（11-01 使用者要求全部軌道一起）：第 2 個以後的難度帶 `--record-no-music`（begin() 把 VolumeConfig 的 sound/music/voice/attract_mode 設 0、只留 hitsound＝打擊、氣球、くす玉，再 set_volume_presets；轉場/語音只出現一次），合成時各軌 WAV `amix normalize=0` 加總＋`alimiter limit=0.95 level=0 latency=1`（level 預設開會自動拉音量 5%）。驗證：3 軌不敲擊與單軌逐樣本相同（含結尾轉場）、自動演奏打擊音 RMS 為單軌 2.0 倍。
- 合成速度：全尺寸 boxblur 是大頭（30 s 片段 36.5 s），改成縮到 1/5 再模糊後 8.3 s；多執行緒、bilinear 沒差。實測：ANTIHERO 5 難度 150 s 影片 274 → 142 s（錄 93 s＋合成約 47 s）；crazylover 3 難度不敲 149 s；4 難度 119 s。
- 錯誤：超過 5 個、不存在的難度、跟 --2p/--practice/--ai 一起用 → exit 1。
- 錄影模式 UI（使用者要求）：選擇視窗的「自動演奏／不敲擊」下面加一排這首歌有的難度（`rec_courses`），左右咔移動、在難度上咚＝勾選（綠底；決定的難度固定勾選），在自動/不敲擊上咚＝開始錄，原本「決定難度→咚」流程不變。軌道順序：決定的難度第一（2 個時是 1P），其餘由難到易。`export_start/export_path` 改收難度清單，檔名 `<曲名>_Oni+Normal+Easy_auto.mp4`。假按鍵實測：勾普通＋簡單 → 3 軌影片。
- 2026-11-21 只錄一段（做 Shorts 用）：`--record-clip <起秒> <終秒>`（影片時間，含歌前的開場）讓多軌錄影的每個難度用 part 的 `--record-range` 跑到起點前不讀回畫面、只錄這段，管道等待時限加上起秒；`--record-lane-volume 0.3` 讓第 2 軌以後的打擊音在混音時降到 0.3（預設 1.0 照舊）。實測 5 軌 20 s 片段 10–12 s 錄完（原本整首錄完再剪約 115 s），分數/連打與整首錄的同一時刻相同、音量 mean −12.3 dB 一致。

## 2026-11-01 錄影加速：GPU NV12＋分段並行

- 使用者選 B+A。crazylover 魔王 239 s 影片（1080p60、NVENC）：單一程序 RGBA 89 s → NV12 70 s → 預設 3 段並行 **53 s**（2 段 55–58、4 段 51、6 段 49：GPU/NVENC 搶滿，3 段起持平，預設 `kDefaultJobs = 3`）。
- B（NV12）：shader 把畫面轉成 W/4 × 1.5H 的 RGBA8 目標（亮度列每 texel 4 個 Y，色度列 U0V0U1V1；BT.709 limited；畫時關 blend，alpha 是資料），ffmpeg 收 3 MB/幀而不是 8 MB。色彩標記要放在**輸入**端（`-pix_fmt nv12 -color_range tv -colorspace bt709 ...`），放輸出端會逐幀轉換（69 fps 對 ~180）。寬非 4 的倍數或 shader 失敗就退回 RGBA；`ZDON_RECORD_RGBA=1` 強制 RGBA。對 RGBA 版 PSNR 36 dB、色彩正確。
- A（分段）：協調程序（不開視窗）用譜面最後一個音符（含分歧）＋20 s 開場/轉場＋結算秒數估總幀數，平分給 N 個子程序（`--record-part k --record-range a b --record-jobs 1 --record-status cache/record/part<k>.txt`，kill-on-close Job）；子程序從頭跑固定步進，到起始幀前**照畫但不 present、不讀回、不編碼**，音訊照混但丟掉；最後一段跑到自然結束。合併：concat demuxer 直接複製影像，WAV 段接起來（跳 44 byte 標頭）一次 AAC mux。log：協調 record.log、各段 record_part<k>.log。
- 快轉時一開始連 draw 都跳過，結果分段處畫面不一致：SimpleStyle 的 `Combo:draw` 才呼叫 `Play.combo`（最大連擊停在 0）、吉祥物反應在 draw 觸發（動畫格錯開）。skin 在 draw 裡改狀態很常見，所以改成照畫，只省 present（多約 3 s）。驗證：3/4 段對單一程序，影像 14360 幀沒有一幀低於 40 dB（平均 59–71 dB，差異是 NVENC 分段 GOP），聲音解碼後逐樣本相同。
- 錄影時 density skin（SimpleStyle 2×）直接畫在 1920×1080 視窗，不再畫進 3840×2160 目標再縮（`density::factor()` 錄影時回 1；`ZDON_RECORD_FULL_DENSITY=1` 恢復）。對 4K 路徑 Y PSNR 42 dB，像素風看不出差別；4 段 55.9 → 45 s。順帶：4K 路徑在進結算那一幀有一張全黑幀，新路徑沒有。
- 測試注意：test_ss_fixgame_zh 的 config.toml 原檔音量全 0，錄出來是靜音（不是 bug）；驗聲音要暫時調高再還原。

## 2026-11-01 錄影再加速＋批次佇列（使用者選 1+2+4+7）

- **錄影不 present**：錄影子程序的視窗是隱藏的，畫面在 present 之前就讀回，所以每一幀都不 present（`skip_present()` 錄影時一律 true；`ZDON_RECORD_PRESENT=1` 恢復，A/B 用）。原本「遊戲」那段每幀 1.9–5.6 ms 其實是卡在 present／SwapBuffers。對有 present 的版本：5 軌 PSNR 平均 64.5 dB、只有 1 幀 44.9。crazylover 239 s：單一程序 70 → 55 s、3 段 54.5 → 45.6 s（4 段 42.9、6 段 42.0，維持 3）。
- **多軌只錄軌道條（1）**：每個難度的子程序加 `--record-band <軌數>`，在 GPU 上把魂量表＋軌道那一條（720p y 136–360）用 LINEAR blit 縮到它在疊圖裡的大小（5 軌 1080p：1236×216），第一個難度再加 `--record-backdrop`，在條下面多放一張 1/5 的整個畫面（384×216）當模糊背景來源。版面算法 `band_layout()` 兩邊共用，子程序把視窗大小與格式寫進 `<out>.size`。
- **管線直送合成（2）**：子程序不再各自編 mp4，改把原始 NV12 幀寫進命名管線（`--record-pipe`；Windows `\\.\pipe\zdon-record-<pid>-<k>`、POSIX mkfifo，`fifo_writer` 先建管線、寫 .size、等讀端），協調程序等所有 .size 出現就開一個 ffmpeg 同時讀 N 條管線，邊錄邊疊、只編碼一次；錄完再 amix 各軌 WAV mux 進去。協調程序 90 s 內等不到管線、或 ffmpeg 失敗 → `run_children` 的 tick 回 false 停掉所有子程序。注意：boxblur 會把 NV12 交錯色度當成半寬平面（背景右半變綠），所以每個輸入先 `format=yuv420p`。
- **AAC**：ffmpeg 內建 aac（twoloop）3 分鐘音訊要 11.6 s，換 Windows 的 `aac_mf` 2.2 s（偵測得到才用，否則 `aac -aac_coder fast` 4.1 s）；對原 WAV SNR 47 dB（twoloop 42），延遲對齊 0。所有錄影的 mux 都受惠。
- **結果**：Abyss of hell 5 難度 1080p（179 s 影片）**130 → 36.5 s**（錄＋疊 33.6 s、聲音 2.9 s）；3 難度不敲擊 38.8 s，聲音對單軌逐樣本相同（最大差 9e-7）；2 難度（2P 畫面）25.7 s。合成畫面對舊版 51 dB、版面相同。
- **批次佇列（4）＋剩餘時間（7）**：錄影視窗按咔＝丟到背景，選曲照常可用，左上角面板顯示「录像中 N%  剩余约 m:ss」、進度條、佇列數；背景中再決定的歌（選項視窗提示改成「咚：加入队列」）排進佇列，一個錄完自動開下一個；返回鍵在選曲時叫回錄影視窗（不會離開畫面），視窗裡返回＝全部中止。全部錄完：視窗開著時顯示「录像全部完成：N 个（失败 M 个）」＋最後一個檔名，在背景時顯示 6 秒提示。剩餘時間＝已用時間×(1−p)/p（p<2% 或未滿 2 s 不顯示）。面板放左上：右上角是 skin 的計時器/連線圖示。
- **修掉的舊 bug**：取消或遊戲在錄影中途關掉，`.part<k>.mp4/.wav` 會留在 Records（原本只刪 .video.tmp/.audio.tmp，而且關遊戲時根本沒取消）。現在主程式結束時呼叫 `export_cancel()`，刪 `<stem>.part*/track*/video.tmp.mp4/audio.tmp.wav`；剛被終止的子程序會鎖檔一下（job 的 ActiveProcesses 歸零時還鎖著），所以刪不掉就每 100 ms 重試、最多 3 s。實測：中途關遊戲、返回全部中止都清乾淨、沒有殘留程序。POSIX 取消只 SIGTERM 協調程序（子程序不跟著死），沒處理。
- 假按鍵實測（SimpleStyle、測試資料夾）：錄第一首 → 背景 → 排第二首（提示「已加入队列（第 1 个）」）→ 返回叫回視窗 → 兩部依序完成、提示「录像全部完成：2 个（失败 0 个）」。

## 2026-11-01 錄影模式（主選單模式板，RECORD_SELECT）

- 使用者選：主選單模式板＋等待畫面。entry「錄影模式」板（skin 文字 `entry_record`，沒有就不出板；2P 時灰掉）→ `Screens::RECORD_SELECT`（RecordSongSelectScreen，選曲 skin，`scenes/song_select_record.*`）。決定難度不進演奏：引擎畫的視窗選「自動演奏／不敲擊」→ 以 `--record --record-status cache/record/status.txt` 開自己當隱藏子程序（`libs/zdon_record_job.cpp`：CreateProcessW＋kill-on-close Job Object，ffmpeg 一起收；POSIX posix_spawn）→ 等待視窗顯示進度（子程序依譜面位置/結束時間寫 0..1，結算畫面算最後 5%）→ 完成顯示檔名，咚＝檔案總管選取、咔＝返回；返回鍵＝中止並刪檔。輸出 exe 旁的 `Records\<曲名>_<Easy..Ura>_<auto|nohit>[ (n)].mp4`（11-01 使用者指定；原本是 Videos\YataiDON）。
- 等待中主程式每幀睡 30 ms、master 音量 0（預覽聲）；錄影程序寫 record.log（不蓋 latest.log），並清掉從父程序繼承的開發鉤子（JUMP_SCREEN、FAKE_KEYS 等，測試時曾讓子程序 30 幀就跳走）。
- SimpleStyle：v2_standalone `entry_record`＋comment 1/2、entry/box.lua COMMENTS、coin_overlay/guide_panel 的畫面表加 RECORD_SELECT。
- 實測（測試資料夾、假按鍵）：万戈イムーーノ十 魔王 auto 147.8 s 影片 62 s 完成；不敲擊中途取消，子程序/ffmpeg 都結束、檔案清掉。
- 發現：build/bin 的 Nijiiro 在 11-01 00:33 被換成沒有 Z-DON 等速列修正的版本（modifier.lua 的 7 列以上重畫、selector.lua 的 mod_lift / nil 修正；有修正的版本在 D:\GitHubRepos
j_ai_wt 未 commit）。SimpleStyle 全量重建會跟著拿到舊版，這次把那兩個檔換回已安裝的版本；產生器的該補丁改成 optional。Nijiiro 本身待使用者決定。

## 2026-11-02 效能檢查

- 演奏中（dense 魔王 auto、180 Hz、隱藏即時跑）：每幀工作 avg 0.7–0.9、p99 1.3–1.7 ms，歌曲開始後沒有 >8.33 ms 的間隔，沒有可再省的。`ZDON_FRAME_SPIKE_MS` 記到的 5–7 ms 幀都在 overlay（EndMode2D 的 flush）、F1 重來後照樣出現：是等前一幀翻頁，不是工作。
- 一般選曲進遊戲 players made 13–58 ms；命令列直接開歌的 180–270 ms 是 3D 角色第一次載入。
- 延後特定模式的 game 資料夾（`deferred_game_subsets`）：ai_battle/gauge_ai 只在 GAME_2P/AI_GAME、dan_info/gauge_dan/ss_dan 只在 GAME_DAN、tutorial 只在 TUTORIAL/INPUT_CALI 載入；其他模式先不載，C++ 或 Lua 查到其中的貼圖時才 load_folder（記一行 `left out of ... loaded when first asked for`）。predecode 依目前畫面推下一個模式（`next_game_mode`）也不解它們。practice 不延後：每個模式的暫停選單都畫它的面板（延後時開暫停 SimpleStyle 卡 386 ms）。`ZDON_DEFER_MODE_FOLDERS=0` 關。同一 exe 開/關交替各 3 次：SimpleStyle 遊戲貼圖上傳 93→76 MB、private 610–619→575–587 MB、載入少 80–160 ms；Nijiiro 138→118 MB、634–673→578–600 MB、少 45–100 ms。固定步進截圖（暫停、練習、段位、教學，兩個 skin）開/關逐位元相同（AI 對戰本身每次不同，不比）。
- 目錄平行列出（`DirIndex::prefetch`）：規劃前先把各 skin 的 subset 資料夾、再把它們的 frame 資料夾丟到 job pool 一起列。列目錄 SimpleStyle 遊戲 38–42→15–16 ms、選曲 27–29→11–12 ms；Nijiiro 遊戲 16–18→8 ms、選曲 19–20→7 ms。載入 log 多了 `(總 ms: animations, listing and plan, N folders listed in ms)`。
- texture.json：`read_json_shared` 直接共用解析快取那份（以前規劃、load_folder 各深拷貝一次）。單次新程序量不出差別（第一次是解析），留著省拷貝。
- BC7 快取存檔不再擋讀取：`write_file` 以前整段（重寫約 60 MB）拿著 file_mutex，那時任何快取命中都要等，主執行緒看過 310–335 ms 的停頓（Nijiiro 背景 footer 3 張圖）。改成每讀一筆 payload 才拿鎖、換檔＋更新位移一起鎖，另加 write_mutex 讓存檔一次一個。
- Nijiiro 背景（skin 端，nj_pr_wt3 分支 perf/lazy-collab-backgrounds）：background.lua 一載入就 require 全部 37 個 collab（第一場約 250–300 ms），改成用到才 require。Background 建立 390–465→182–245 ms（8 次）。剩下的是 Lua 依亂數選的背景組合（dancer/bg_fever/footer 等）當場 load_folder，選擇在 Background.new 裡才決定，引擎沒辦法事先知道。
- 開發鉤子 `ZDON_LUA_REQUIRE_LOG=1`：每個 ≥1 ms 的 require 記一行、給 skin `zdon_clock_ms()`；Background.new 耗時每次記一行。包 require 會多一層 Lua frame，skin_require 找呼叫者時跳過它（不然父 skin 的模組會解析錯）。

## 2026-11-03 BC7 快取命中時不解 PNG（cache/bc7/files.bin）

- 原因：BC7 快取以解碼後像素的雜湊為鍵，所以每張圖每次都先完整解一次 PNG，快取只省上傳和顯存。Nijiiro 的大圖（1920×540 背景一張 20–30 ms）讓背景資料夾的 load_folder 幾乎等於「最大那張的解碼時間」。
- 做法：`texture_bc7::cached_image(path, stamp)`。`load_image` 先取檔案大小＋最後寫入時間，`files.bin` 有同路徑同 stamp 的紀錄、且對應的 Blocks 項目還在，就直接讀 BC7 blocks 組成 Image（不讀檔、不碰 texture pack），邊界從紀錄放回 `remember_border`。紀錄在 `compress` 命中時寫入（所以第二次載入才生效），隨 textures.bin 一起存、關遊戲時也存；項目被裁掉的紀錄在存檔時丟掉。Keep（保留 RGBA）的圖照舊解碼。`ZDON_BC7_FILES=0` 關（A/B 用），`YATAIDON_TEXPACK_VERIFY=1` 時也不走。
- 量測（Nijiiro，同一 exe 開/關交替）：一般流程選曲畫面 243→129 ms；教學背景 20→11 ms；命令列開歌 Background.new 198→143–149 ms（bga_bg_1 19.6→6.0、bg_fever 21.6→11.1、dancer 68→55）；遊戲畫面本身 242→221–245 ms（解碼本來就被平行 prefetch 蓋掉，瓶頸不在這）。SimpleStyle（小圖）遊戲 271–298→248–258 ms、選曲不變。第一次建立紀錄那輪比關閉時多約 25 ms（stat＋記錄）。
- 驗證：兩個 skin 的遊戲（dense 魔王 auto 6 格）與選曲（7 格）固定步進截圖開/關逐位元相同，log 無錯誤。冷硬碟沒量（textures.bin 的隨機讀取在 HDD 上可能不如 pack 的循序讀）。

## 2026-11-03 BC7 快取改 zstd、3D 咚動畫預先解析

- 一般流程（選歌→選難度→開始，非固定步進）：選曲結束到遊戲初始化約 317 ms＝遊戲畫面貼圖 118（選難度時已預解，只來得及先上傳 61 張）＋Background.new 119＋其他。「選曲 player 建立 190 ms」只出現在直接跳進選曲的測試：一般流程登入畫面已建過咚，選曲建 player 約 20 ms、選曲啟動共約 165 ms。
- BC7 快取的區塊壓縮率 10.7 倍（skin 圖大量透明），不能存原始資料；Python 實測同一批資料 zlib 0.48 GB/s、zstd 9 1.40 GB/s 且小 13%。改用 zstd（cpptrace 帶進來的 `libzstd_static`，CMake 有這個 target 才開 `ZDON_HAVE_ZSTD`），level 9。舊檔的 deflate 項目照讀，第一次讀到時由 Background job 轉成 zstd（不重新編碼），隨下次存檔寫回；log 尾巴記轉了幾筆。交替 A/B 各 3 次（Nijiiro dense 命令列）：Background.new 175–187→127–135 ms（背景資料夾讀＋解 73–81→41–46 ms）、遊戲畫面 276–290→237–253 ms；快取檔 63.9→59.5 MB。
- 順手修掉的競態：write_file 先複製項目清單再寫，寫完更新位移時沒檢查項目是否在中間被換掉（背景編碼完成、轉 zstd），會讓記憶體裡的項目指向檔案裡舊的資料、下一次存檔把不相符的資料寫進去（實際出現 752 筆讀不出來）。現在只更新跟清單一致的項目，其他的留到下次存。
- 3D 咚：每次執行第一次建咚要解析 Models/animations.glb（load_model 52 ms 只為了骨骼名稱＋動畫 110 ms），落在登入畫面初始化。骨骼名稱改用 cgltf 直接讀（第一個 skin 的 joints、截 31 字元，同 raylib LoadBoneInfoGLTF；兩個 skin 的 animations/head/body/cos 共 8 次比對全相同），整段不碰 GL，`load_skin` 成功後由 `chara3d_prewarm()` 丟 Background job 先解析；快取改成不解構的 heap 物件（結束時 job 可能還在跑）。啟動→登入畫面就緒 765–829→386–393 ms。
- 驗證：Nijiiro／SimpleStyle 遊戲 6 格＋選曲 7 格固定步進截圖對上一版（4eae1042）逐位元相同，0 筆讀取失敗、log 無錯誤。
- 沒做：背景隨機組合提前準備。只改引擎的話不知道 Lua 會選哪組，Nijiiro 候選總量約 686 MB（BC7），學「上次用過的」命中率低；要做得讓 skin 在選歌時就決定（改 Nijiiro）。父 skin 用不到的舞者圖（每首約 30 ms）也是 skin 端的事。

## 2026-11-10 失誤就結束、手電筒、COMPAT:、MIDI 設定、osu! 太鼓譜（參考 OpenTaiko / taiko-web / osu!）
- 選項面板兩列（存在 `zdon_player_options`：`sudden_death` 0–2、`flashlight`）。「失誤就結束」出不可／出可：Player 判到第一個不可（或可）就標記，GameScreen / Game2PScreen 截斷該玩家的譜（剩下算不可）；所有玩家都出局才停歌（不設上游的 `skipped`，Nijiiro 不會畫「跳略演奏」），2.8 s 後進結算，成績不存（理由「失誤結束」）；AI 那一側不套用，AI 對戰時算 AI 贏。「手電筒」：音符畫完後在判定線右邊 30% 跑道外蓋暗幕（8% 漸層），100／200 連段縮到 25%／20%，斷連段放回（150 ms 緩動），AI 那一側不套用。
- TJA `COMPAT:`（OpenTaiko 的名字 jiro1 / jiro2 / tmg / tjap3 / oos）：沒寫或 oos、tjap3 = 原本的行為。jiro1：`#BPMCHANGE` 與含 BPM 變化的小節結尾，把距上次變化的時間取整到 ms；jiro1/jiro2：`#DELAY` 取整到 ms；jiro1/jiro2 有 BALLOONEXP/MAS 沒有 BALLOONNOR 時 BALLOON 當三條分歧共用。tmg、方向／#SUDDEN 垂直分量等 OpenTaiko 其他差異沒有做。測試鉤子 `ZDON_CHART_DUMP=<檔>`（TJA 與 osu 都會把解析後的音符時間附加寫入）。
- MIDI：上游已有 libremidi 輸入（`[midi]` device／notes→手把按鈕），只能手改 config。Z-DON 在設定畫面自動加「MIDI」分頁（不改 skin 範本）：MIDI 裝置（偵測到的輸入埠，選了立刻切換）、學習對應（咚開始，依序在 MIDI 鼓敲 1P 左咔、左咚、右咚、右咔；對到該角色的第一個手把按鈕，沒有就從 32 往下找沒用到的號碼加進 `[gamepad_1p]`）。MIDI 監聽改成需要時才啟動（設定了裝置或打開設定畫面）。測試鉤子 `YATAIDON_FAKE_MIDI="<ms>:<note>[:<vel>],..."`。實機 MIDI 鼓未測（這台沒有裝置）。
- osu!（上游已有 OsuParser，一個資料夾＝一個資料夾格，每個 .osu 一首歌）：難度名稱（Kantan／Futsuu／Muzukashii／Oni／Inner Oni、Easy／Normal／Hard／Ura、日中文）決定難度格，看不出就依 OD；等級＝1＋每秒咚咔數×1.1（★1–10）；spinner 氣球數＝長度×OD 對應每秒 3／5／7.5 下×1.65（osu!lazer swell）；kiai＝GOGO。同一首歌的多個 .osu 還是各自一首（合併成一首多難度要改選曲導航，沒做）。

## 2026-11-10 自架伺服器＋差量更新（使用者：「增強 updater 跟 server，我未來要在這台電腦上架設」）

- `server/zdon_server.py`（只用 Python 標準函式庫）：遊戲線上 API（照 network.cpp 的請求格式與 HMAC 簽章；
  客戶端簽了沒送的空 idm、送了沒簽的空 source_folder/gauge/judge_params 都要容忍）、更新頻道
  `/update/<頻道>/{version.txt,checksums.sha256,files/…}`、skin（skin updater 的 `raw/branch` 版面）、狀態頁。
  活動類端點（獎牌、AI 對戰段位、きのたけ、ドンポイント、商店）是簡化規則，只讓畫面有回應。說明在 `server/README.md`。
- 客戶端：桌面版一律編入線上程式碼（`ZDON_NETWORK_CODE`；讀卡機仍跟著編譯時伺服器），`config.toml [network]`
  `zdon_server_url`/`zdon_server_key` 優先於編譯時的 NETWORK_URL/KEY；沒設＝跟以前一樣離線。
- 更新：`zdon_update.ps1/.bat` 逐檔 SHA-256 差量（大小＋修改時間快取雜湊、暫存資料夾驗證後才換上、
  只移除前一版清單裡有而新版沒有的檔案、絕不碰 config/scores/cache/Songs/Records）。
  遊戲啟動時背景檢查（`zdon_update_check`），標題／登入畫面顯示新版，F9 交給更新程式並關閉遊戲、更新完自動重開。
  上游的 update.ps1 仍指向 yonokid 的 GitHub release（會換成上游版），Z-DON 不發布它。

## 2026-11-10 線上對戰（使用者：挑戰紀錄＋即時對戰，房間代碼＋自動配對，分數高者勝）

- 共通：2P 譜面用重播的路徑（Player::load_replay / append_replay）吃對手的打擊；對手的譜面選項（random／inverse／seed／
  簡化譜）照對手的，速度等顯示選項照自己的。結算橫幅（RESULT_2P）、2P 成績板顯示對手且不算個人最佳。`zdon_online.h`。
- 挑戰：GET /challenges、/replay 多回 score／modifiers、網站「挑戰」按鈕（jumps.challenge → poll_jump challenge_id）。
- 即時：`server/zdon_match.py`（create／quick／join／state／song／ready／sync／finish／leave）。Game2PScreen 的
  2P 譜面跟音樂同步捲動（2026-11-11 使用者：晚一秒不行），Player::zdon_stream：打擊一到就判定（先把它之前的漏打處理掉），
  漏打／分歧只推進到對手回報的位置（那之前的打擊都已送到）和下一個未處理的打擊；對手結束或離開就全部結算。
  hold_song_end 等對手的最終成績（最多 20 秒）。對手的打擊不出聲（挑戰也是，zdon_silent_replay）。
  自動演奏時打擊也記進 input_log（log_auto_hits）以便兩個實例對打測試。
- 實測：兩個遊戲實例（第二個在沙盒，Skins／Songs 用 junction）自動配對→選歌→對打→結算，
  兩邊的對手譜面結果與對手自己的完全相同（良 429、連打 43、1003870）。
- 2026-11-11 使用者：Flashlight／隱藏只套自己的譜面（mods_2 關掉）；客人不再用簡化的難度視窗，選曲游標跳到房主的歌
  （Navigator::jump_to_song），用一般難度選擇（含選項）決定後送 ready，選到別首會提醒並跳回；跳不過去（合集資料夾）才用舊視窗。
  缺歌：客人 ready has_song=0 want=1 → 房間 transfer → 房主 zdon_pack_song（資料夾只有這張譜時整個資料夾，否則譜面＋WAVE＋BGMOVIE，
  不壓縮的 zip）→ signed_upload（參數放 query 簽名、sha256 驗內容）→ 客人下載、zdon_install_song 解到 `<tja_path[0]>/Z-DON Online/<名稱>`，
  有一個難度的雜湊等於房主的才 add_path_binding，並用 LibraryIndex::note_changed 讓選曲改讀磁碟（啟動時的索引沒有新資料夾）。
  實測：2P 的歌曲庫只有教學，收到「1・2・3」後跳過去、自己選魔王，兩邊結果一致（良 429、1003870）。
- 2026-11-11 WebSocket：curl 開 ENABLE_WEBSOCKETS（cmake/deps.cmake），`src/libs/network/zdon_ws.*` 背景執行緒收發；
  伺服器 `server/zdon_ws.py`（最小 RFC 6455）＋ Matches.ws_session，一收到就推給對手（{"t":"o"}），結束／離開也推。
  握手簽名照 signed_headers（參數放 query）。連不上或斷線就退回輪詢（ZDON_ONLINE_WS=0 強制輪詢）。
  每筆打擊帶 t0（送方 chart 0 的牆上時鐘），收方在結束時 log 晚到的平均／中位數／95%／最大值。
  實測同一台：本機 9 ms vs 輪詢 104–114 ms；經 Cloudflare 300 ms vs 1.4 秒（zdon.taiko.ac 被分到 SJC，連線本身就 130 ms）。
- 2026-11-11 直連：`src/libs/network/zdon_p2p.*`，IPv4／IPv6 各一個 UDP socket，STUN（Google、Cloudflare）取公網位址，
  位址經 WebSocket {"t":"cand"} 交換，雙方每 100 ms 對所有位址送探測（打洞），先回應的就用；封包帶房間這一局的 key。
  打擊同時走直連和 WebSocket，live_take 依 at 去重、先到先用；直連封包帶 ack，送方每次重送對方還沒確認的（最多 60 個）。
  位置只在之前的打擊都到齊時才採用，且只往前。ZDON_ONLINE_P2P=0 關閉。實測同一台經 Cloudflare：509 下全部直連先到，平均 8 ms。
  未驗證：跨網路（不同 NAT）的打洞成功率，要兩位玩家實際試。
- 2026-11-11 同時開始（使用者：1P 會先開始不對）：Game2PScreen 進畫面先停住譜面時鐘，WebSocket {"t":"loaded"}
  （輪詢帶 loaded=1），伺服器在兩邊都 loaded 時同時送 {"t":"go"}；最多等 20 秒或對手離開就開始。
  實測經 Cloudflare：兩邊開始相差約 80 ms（原本差一整個載入時間）。
  再改成對時（使用者：80 ms 要改）：WebSocket ping/pong 量和伺服器的時鐘差（取最短來回），go 帶 at（伺服器時間＋800 ms）；
  有直連時房主每 20 ms 經直連 ping 對方 12 次，把開始時間換成對方時鐘送 {"t":"start"}（送 3 次）。實測開始只差 3 ms。
  順便：連線檢查（heartbeat）離線時每 5 秒重試、逾時 2→5 秒（第一次經 Cloudflare 的 TLS 逾時會讓線上對戰卡 2 分鐘）。
- 2026-11-11 三戰兩勝與聊天：zdon_match.py 的 "host" 改成「選下一首的人」，每首結束（兩邊 final 都到）由 _score_song
  計分並交換 host/guest；先 2 勝 state="over"（_drop 不再把 over 的房間改回 waiting），兩邊看到後各自 leave。
  聊天：POST /match/chat、state 的 chat（chat_since），遊戲中走 WebSocket {"t":"chat"}（目前遊戲中不顯示）。
  驗證：Python 直接呼叫 Matches 模擬多場（先選隨機、輸的人選第二首、1:1 再隨機、2 勝結束）；遊戲畫面用腳本對手發訊息截圖。
  未驗證：實際用鍵盤打字送出（測試用的假按鍵不產生文字輸入）、比賽結束的提示畫面。
- 2026-11-11（使用者）：客人收到房主的歌就 jump_to_song 後直接 player->select_song() 進難度選擇，
  on_diff_select_cancelled 再打開、Esc 吞掉（只能選難度與選項）；線上對戰 mods_1.skip=false、zdon_sudden_death=0；聊天鍵改 Y。
  驗證：腳本當房主選 Ryuusei，遊戲客人直接進難度選擇，Esc 兩次仍在同一首。
- 2026-11-11 信用分／檢舉／投降（使用者）：zdon_credit.py（credit、credit_log、reports 表），Matches 的入口檢查分數、
  _drop 在比賽中途離開扣分、比賽結束 +2；/match/surrender（或 WebSocket {"t":"surrender"}）直接判這首輸並推給對手，
  r.scored 防止之後兩邊的 finish 再算一次；網站 /admin 審核（site.admins）。遊戲：暫停鍵＝投降確認，live_end_now 照
  sudden death 的方式結束；結算橫幅看 zdon_online.surrender；選單顯示信用分、R 寫事由檢舉。客人自動進難度選擇時
  游標停在房主的難度，游標在「返回」時咚鍵無效（原本退出再打開會讓歌單動畫亂掉）。
  驗證：Python 用暫存資料庫測投降計分、中途離開扣分、門檻、檢舉與審核；遊戲裡腳本當房主，客人投降→結算顯示你輸了／你投降了。
  未驗證：網站 /admin 頁面實際登入操作、R 檢舉的打字、信用分在選單的顯示。
- 已知限制：random 的種子若在遊戲開始時才決定，對手那邊無法重建同一套音符（目前送 seed 0）；舊的匯入成績沒有打擊紀錄不能挑戰。

## 2026-11-18 3D 視角、USIO 機台、演奏輔助、練習與結果時序（已完成）

- **3D 視角**（`[general] zdon_lane3d`，預設關；設定 →「3D 視角」）
  - 演奏畫面分層（背景、跑道、判定圈、音符、判定、HUD、特效、前景、覆蓋）各畫到同大小的 render target，再依深度當平面組合。平面時與以前逐像素相同；轉場與暫停選單維持平面。
  - 視角：滑鼠拖曳、滾輪、中鍵切換預設、右鍵回平面；Lua `tex.zdon_lane3d(yaw, pitch, dist, ms)`。不做自動晃動（使用者：會頭暈）。
  - `[general] zdon_lane3d_preset`（0 平面、1 傾斜、2 公路、3 斜角）：每首歌的起始視角，從平面緩入。第一次使用時顯示操作提示（`zdon_lane3d_hint_shown`）。
  - 音符畫成硬幣（純色側面＋淺色邊），跑道有側牆，逐平面光源、陰影、暗角，縮小時用 mipmap。
  - 一般遊玩、2P、わいわい、段位、練習（演奏中；暫停時平面）。AI 對戰仍為平面。
- **USIO 機台**（`[usio] enabled`，預設關；開啟時設定多一個「USIO」，內有 USIO 測試）
  - `dll`（預設 `bnusio.dll`，機台自己的，不隨 Z-DON 發佈）、`threshold`（2000）、`thresholds`（8 值：P1 左咔、左咚、右咚、右咔，再 P2；未設定則全用 `threshold`）、`holdoff_ms`（25）、`poll_us`（1000）。
  - 鼓、TEST／SERVICE／UP／DOWN／ENTER 與投幣計數都透過 bnusio.dll；按鍵位元與函式型別照 TaikoArcadeLoader，真機待驗證。
  - 投幣、SERVICE 加 `zdon_coin_credits`；`zdon_coin_mode` 開啟時開始遊戲扣 1 credit。
  - 輸入測試畫面的 USIO 面板：感應器讀數、門檻線、按鍵、計數器、讀取頻率；`[` `]` 即時調整全部門檻（Shift 為 ±50），`R` 還原、`S` 存回 config.toml；載入失敗時顯示原因。真機驗證步驟見 [ZDON_USIO.md](ZDON_USIO.md)。
  - 輸入測試畫面也顯示輸入執行緒的輪詢頻率。
- **演奏輔助**（設定 →「演奏輔助」，皆預設關）
  - `[general] zdon_hit_error_bar`：判定圈下方的誤差條（最近的打擊、良／可範圍、平均標記）；自動演奏與練習不顯示；2P 時 1P 的條放在跑道上方；skin 可用 `zdon_hit_error_bar` {x,y} 覆寫位置。
  - `[general] zdon_new_best_chip`：結算畫面頂端「自己ベスト更新 +N」（皇冠變好也算）；高分語音只在真的存了新紀錄時播放。skin 覆寫 `zdon_new_best`。
  - `[general] zdon_target_score`（整數，0 關）：演奏中在 PB 提示的下一列顯示「目標 N　還差 M」，達成變綠；自動演奏、段位、暫停時不顯示。「演奏輔助」可設定（關、50 萬–100 萬每 1 萬）。
  - `[general] zdon_hist_auto_open`：結算畫面一開始就打開打擊時間面板；沒有操作時照常自動前進（按咔開關過才停住計時）。
  - `[general] zdon_ka_mark`：咔音符上畫白色十字（大咔較大），不靠顏色也能分辨咚咔。
  - Lua：`hit_error_mean`、`usio_active`、`best_accuracy(_p2)`。
  - `[general] zdon_remember_difficulty`：同一次開啟遊戲內，每首歌重開時選回上次玩的難度。
  - `[general] zdon_practice_metronome`：練習模式每拍一聲輕的咔（照譜面 tempo map，跟著變速；#DELAY 不響；跳轉、A-B 循環時不補響）。
  - `[keys] uncleared_song_key`（預設不綁定）：選曲時跳到清單中下一首「目前難度還沒通關」的歌（還沒選過難度時以鬼為準）。
  - `[keys] random_song_key`（預設不綁定，「演奏輔助」裡可設定）：選曲時跳到目前清單裡隨機一首歌。
- **練習**：暫停時 M 鍵跳到最後一個失誤；A-B 循環列出最近幾輪與最佳一輪。
- **結算**：打擊時間面板多一行「開頭→結尾」四段平均偏差。
