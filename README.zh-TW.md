# Z-DON

[English](README.md) | 繁體中文

<img src="docs/logo.png">

**Z-DON** 是 [YataiDON](https://github.com/Yonokid/YataiDON) 的分支：一套用 C++ 寫的 TJA 播放器／太鼓模擬器。
YataiDON 本身由它的貢獻者撰寫；**Z-DON 在它之上的修改完全由 AI 開發**，目標是極致的效能，以及上游沒有的新功能與線上對戰。

- 官網：<https://zdon.taiko.ac>
- Windows 版下載：[Z-DON-windows.zip](https://github.com/GanYiZhong/Z-DON/releases/latest/download/Z-DON-windows.zip)（rolling release，永遠是最新版）
- 瀏覽器直接玩（網頁版）：<https://zdon.taiko.ac/play/>
- 授權：GPL-3.0（見 [LICENSE](LICENSE)），源自 YataiDON

## 與上游的關係

- 持續合併上游 master，上游的功能（多平台、Lua skin、3D 角色、AI 對戰、商店、段位、教學…）都在。
- **相容**：YataiDON／ESE 的 skin、`scores.db`、`config.toml` 都能直接用；Z-DON 自己的資料放在另外的表或設定項。
- 新功能大多可以在 `config.toml` 或環境變數關掉，回到上游行為。
- 完整的變更紀錄、量測數字與開關在 [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md)。

## Z-DON 加了什麼

### 效能

每一項都用確定性測試（固定時間步的逐位元截圖比對）確認畫面不變，並在實機量過。

- **載入**：跨畫面預讀、每畫面貼圖打包快取（texpack）、zlib-ng PNG 解碼、BC7 壓縮貼圖快取、
  貼圖用到時才上傳 GPU、預先準備下一個畫面（predecode）、長音效先解前 2 秒、曲庫只走訪一次。
  冷硬碟進選曲的貼圖載入約 1.4 s → 0.35 s。
- **執行時**：無鎖音訊 callback、打擊音複音、輸入執行緒改高解析度計時器（不再空轉一整顆核心）、
  vblank 對齊限幀、影片以 YUV 平面上傳由 shader 轉色、字型 glyph 快取、Lua 分代 GC、批次繪圖精簡、
  Windows Release 用 `-O3`＋LTO。
- **貼圖密度**：skin 可以出 2× 貼圖，在 4K 畫面上清晰繪製（`texture_density`）。

### 遊玩

- **暫停選單**：演奏中 Esc／空白鍵暫停，繼續前 3-2-1 倒數；視窗失焦自動暫停。
- **練習／特訓**：A→B 區段循環、輸入小節號跳轉、固定分歧、每一下的偏差顯示、只用鼓就能操作。
- **演奏選項**：等速（忽略 BPM／SCROLL 變化）、隱藏／突現、自己最佳的影子、失誤就結束、手電筒。
- **結算**：判定分布直方圖（平均、標準差、早／晚數）、不保存成績時顯示原因。
- **選曲**：即時搜尋面板（類型、難度、星級篩選，支援滑鼠）、難度資料夾只列未達成、最近 10 筆成績。
- **AI 對戰**（Z-DON 版）：AI 依等級與你的偏差調整，分段計分，贏輸自動調整等級。
- **錄影**：指定歌曲與難度直接輸出 MP4（主選單「錄影模式」或 `tools/record_video.bat`），
  多難度同框、分段並行、背景佇列。
- **譜面相容**：TJA `COMPAT:`（jiro1／jiro2）、osu!taiko 譜面直接放進曲庫、MIDI 鼓在設定畫面裡學習對應。
- 修掉許多上游也有的 bug（巢狀資料夾當機、同名歌曲選錯檔、含假名路徑開不了、練習模式結束當機…）。

### 線上

- **自架伺服器** [`server/`](server/README.md)：只用 Python 標準函式庫；遊戲線上 API、差量更新頻道、skin 發佈。
- **差量更新**：遊戲啟動時檢查，標題畫面按 F9 更新（逐檔 SHA-256，不碰設定、成績、歌曲）。
- **線上對戰**：房間代碼或自動配對、三戰兩勝輪流選歌、聊天、投降、信用分與檢舉；
  打擊經 WebSocket 與 UDP 直連（打洞）同時送，雙方對時同時開始；對手缺歌時自動傳送。
- **挑戰**：用別人的重播打同一張譜。
- **網站**（zdon.taiko.ac）：下載、歌曲與譜面頁、Rating 排行榜、難易度表、玩家頁（名牌、王冠統計、Best 50、段位）、
  我的頁面（稱號、名牌、角色外觀、隱私、成績管理與匯出）。

### 網頁版

用 Emscripten 編成 WebAssembly，在瀏覽器裡玩（<https://zdon.taiko.ac/play/>），音訊走 AudioWorklet 降低延遲。

## 系統需求

- **Windows**：Windows 10 以上（主要開發與測試平台）
- **Linux／macOS／Android／iOS／PS5／Switch**：沿用上游的建置，Z-DON 的改動盡量跨平台，但測試較少
- **瀏覽器**：支援 WebAssembly 與 AudioWorklet 的新版 Chrome／Edge／Firefox

## 安裝

1. 下載 [Z-DON-windows.zip](https://github.com/GanYiZhong/Z-DON/releases/latest/download/Z-DON-windows.zip)（或到 <https://zdon.taiko.ac>）並解壓縮。
   Z-DON 是 rolling release：沒有版本號，每次發布新版都會覆蓋 [`rolling`](https://github.com/GanYiZhong/Z-DON/releases/tag/rolling) 這個 release。
2. 執行 `YataiDON.exe`（執行檔名稱沿用上游，方便合併）。
3. 把 TJA 歌曲放進 `Songs`，或在設定裡指定歌曲資料夾。之後的更新在遊戲裡按 F9 就好。

延遲高的話，把設定裡的 `device_type` 改成 `WDM-KS`、`WASAPI` 或 `ASIO`。

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
- `YATAIDON_FRAME_STATS=1`：每 2 秒記一行幀時間、CPU 與記憶體；`ZDON_FRAME_SPIKE_MS=<ms>` 記下超過門檻的幀。
- `YATAIDON_FAKE_KEYS`、`YATAIDON_JUMP_SCREEN`、`YATAIDON_FORCE_AUTO`：假按鍵、直接跳到某畫面、強制自動演奏。

完整清單與每個效能開關見 [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md)。

## 操作

- 演奏中 **Esc**／**空白鍵**：暫停選單；**F1**：重來
- 任何畫面 **Esc**：返回
- 選曲：**空白鍵** 加入最愛、**左右 Ctrl** 一次跳 7 首、**F2** 搜尋、**F5** 重新整理
- 線上對戰的選單：**Y** 聊天、**R** 檢舉
- 標題畫面 **F9**：更新到最新版
- 鼓的按鍵可以在遊戲設定或 `config.toml` 裡改

## 回報問題

Z-DON 的問題請開在這個 repo 的 [Issues](https://github.com/GanYiZhong/Z-DON/issues)，不要回報到上游；
上游 YataiDON 本身的問題請到 [YataiDON 的 Issues](https://github.com/Yonokid/YataiDON/issues)。

## 致謝

Z-DON 建立在 [YataiDON](https://github.com/Yonokid/YataiDON) 與它所有貢獻者的成果上：
IID、mc08、QBaraki、KabanFriends、DragonRatTiger、Komi、sumandrew、WallK、dpkgluci、Ape、chuchy（osu parser）、
churrochef（vertical scroll）、cainan（3D don chan）、RyutoSetsujin、Nyannurs、fehpizza（logo）、Radix、Thonktheta、
SomehowScarlet、SunnyDeez。網站的難易度表資料來自 [YataiDON-Syakai](https://github.com/Yonokid/YataiDON-Syakai)。

使用的函式庫：[raylib](https://www.raylib.com/)、[SDL3](https://github.com/libsdl-org/SDL)、[SQLite](https://www.sqlite.org/)、
[RapidJSON](https://github.com/Tencent/rapidjson)、[toml++](https://github.com/marzer/tomlplusplus)、[spdlog](https://github.com/gabime/spdlog)、
[Lua](https://github.com/marovira/lua)、[sol2](https://github.com/ThePhD/sol2)、[cpptrace](https://github.com/jeremy-rifkin/cpptrace)、
[libsndfile](https://github.com/libsndfile/libsndfile)、[libsamplerate](https://github.com/libsndfile/libsamplerate)、
[FFmpeg](https://ffmpeg.org/)、[PortAudio](http://www.portaudio.com/)、[miniz](https://github.com/richgel999/miniz)、
[zlib-ng](https://github.com/zlib-ng/zlib-ng)、[bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo)、[curl](https://curl.se/)。
