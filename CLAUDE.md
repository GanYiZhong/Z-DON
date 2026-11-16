# Z-DON

Z-DON 是 [YataiDON](https://github.com/Yonokid/YataiDON) 的分支，**完全由 AI 開發**，目標是極致的效能優化與新功能。
不受上游「過度優化」「維護成本」等考量限制：上游不收的優化（例如 #297 預讀）在這裡可以做，但一樣要量測證明有效。

## Repo 與上游

- 位置：`D:\GitHubRepos\Z-DON`，分支 `main`，起點為上游 master `0f313b0`（2026-10-18）。
- `origin` = Yonokid/YataiDON，**只 fetch，永遠不 push**；定期 `git fetch origin && git merge origin/master`。
- 目前同步到上游 master `1cd3844`（2026-11-15，演奏對戰 enso battle、暫停選單、帳號歌單 #381、QR、讀卡機 libnfc、toast；暫停選單是 Z-DON 的那個，skin 有 `pause_menu.lua` 時由它畫；暫停過的一局照上游不存成績；新相依 qrcodegen／libfacedetection 要用 `-DFETCHCONTENT_FULLY_DISCONNECTED=OFF` 抓一次，見 merge 訊息）；前一次 `5dacd26`（2026-11-09，段位成績上傳、難度名稱、語言選擇、連打 Lua；skin 有 `bubble_unfold` 或在 Z-DON AI 舞台時，連打仍由 Z-DON C++ 畫，見 merge 訊息）；前一次 `09db50f`（2026-11-08，訪客模式、獎牌、活動、共用薬玉；上游 e56c73e 的 JudgeCore 判定重構與 Lua song info 沒有採用，Player 維持 Z-DON 版，見 merge 訊息）；前一次 `0ed42ec`（2026-11-06，上游 AI 對戰＋わいわい＋商店＋總結算＋活動＋Switch，見 merge 014454c 訊息與 roadmap「合併上游 0ed42ec」：兩套 AI 對戰並存，skin 有 `ai_support_menu` Lua 走上游的、否則走 Z-DON 的；前一次 `3910153`（2026-11-02，gfx 抽象層＋libs 子資料夾＋教學＋PS5，見 merge fa477c3 訊息）；再前 `29620bd` 2026-10-23，raylib 包裝重構＋輸入改 SDL；前一次 `ef867fb`、再前 `5e1b04d`）。`29620bd` 合併後：測試假按鍵 YATAIDON_FAKE_KEYS 仍收 raylib 鍵碼、在 dev_inject_key 轉成 SDL scancode；上游 cf9dcf9 拿掉 Lua `blend`（additive），畫面比對的背景光暈、煙火、皇冠閃光差異來自這裡。`ef867fb`（2026-10-22）`ef867fb` 合併時響度、skin 更新器、TJA 衝突照使用者決定用上游版（見 roadmap「合併上游 ef867fb」）；`.clang-tidy` 又多了 default-member-init／nullptr／override／else-after-return／redundant-lambda-parameter-list／performance-* 檢查。`bde4201` 合併時的重點：含 f2e1174「PlaybackClock＋歌曲生命週期」重構（`clock.position()` 取代 `ms_from_start`，`unload_song()`/`stop_song_audio()`），以及 `.clang-tidy` 新增的 modernize/readability 檢查。
  合併時 Z-DON 自己的程式也要套上同樣的轉換：`pip install clang-tidy==23.1.3`，用 build 的 `compile_commands.json`（只留 `src/*.cpp`）跑那幾個檢查、`-extra-arg=--target=x86_64-w64-mingw32`；modernize-use-ranges 的自動修正在這環境會留下 `,,`，要手動清。上游自己沒改到的 Windows 專用行（CI 只跑 Linux）保持上游原樣。
- `yataidon-local` = `D:/GitHubRepos/YataiDON`（wip-all 等本機分支），要搬舊修正時從這裡 cherry-pick。
- GitHub：公開 repo https://github.com/GanYiZhong/Z-DON（remote `github`，2026-11-13 建立），只推 `main`；推送照樣要使用者同意。
- 不從這個 repo 對上游開 PR（上游 PR 照舊走 YataiDON / YataiDON_bm）。
- 授權維持 GPL-3.0（LICENSE 不動），公開時須註明源自 YataiDON。

## 相容性原則（為了能持續合併上游）

- 保持與上游 skin 格式、`scores.db`、`config.toml` 相容：上游/ESE 的 skin 必須能直接用。
- 新功能預設關閉或以設定開關控制；不相容的格式改動要先問。
- 盡量把 Z-DON 的程式放在新檔案或小範圍改動，減少合併衝突。

## 建置

```bash
export PATH=/c/msys64/mingw64/bin:$PATH   # 少了這行 gcc 會靜默失敗
cd build && nice -n 10 cmake --build . -j 16   # 56 logical CPUs; -j 4 made builds needlessly slow
```

- Release 預設開 LTO（完整連結數分鐘）；開發中可 `cmake . -DZDON_LTO=OFF` 加快，**量效能和交付前要切回 ON**。
- PGO（`-DZDON_PGO=OFF|GEN|USE`，預設 OFF）：`bash tools/build_pgo.sh` 一次做完「無 LTO 插樁版 → 自動隱藏跑訓練（標題/選曲捲動/段位/3 張譜 auto 到結果，約 10 分鐘）→ LTO＋`-fprofile-use` 重建」，profile 在 `build/pgo-data`；跑完 build 目錄停在 USE，改回 `cmake -DZDON_PGO=OFF .`。2026-10-19 量測（240 fps 上限 FRAME_STATS、fixed-step 執行時間）**沒有可量到的增益**，所以交付版不用 PGO。插樁版不能開 LTO（GCC 16.2 MinGW 連結錯誤 `two or more sections for .gnu.lto___gcov_...`）。
- 改完 `src/` 要跑 clang-format（上游 CI 用 23.1.3 `--dry-run --Werror`）：
  `"$APPDATA/Python/Python313/Scripts/clang-format.exe" -i $(cygpath -m <file>)`
- 建置好就停，**不要自動啟動遊戲測試**（使用者可能正在玩）；需要時先 `tasklist` 確認。

## 效能工作的規則

- 使用者 2026-10-19：不必每項都堅持量冷/熱硬碟，**做了、沒壞就好**（行為/畫面比對才是必要的）；有現成量測工具時順手量即可。
- 冷硬碟測試架：skin 以 `robocopy /J` 從 `E:\yd_src\Skins` 複製到 `G:\yd_hddtest`（Toshiba HDD），再跑遊戲量畫面載入。
- 已證實無效、不要再試：
  - 單一執行緒依序讀檔（冷硬碟 1.1–1.6 s → 2.1–2.3 s；硬碟靠 NCQ 重排多個同時的讀取）。
  - Lua bytecode 快取（冷硬碟反而變慢，多了小檔 I/O）。
- 額外執行緒用背景優先權、記憶體要有上限；不要增加小檔案 I/O。

## 待辦

見 [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md)。
