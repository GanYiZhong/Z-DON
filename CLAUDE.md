# Z-DON

Z-DON 是 [YataiDON](https://github.com/Yonokid/YataiDON) 的分支，**完全由 AI 開發**，目標是極致的效能優化與新功能。
不受上游「過度優化」「維護成本」等考量限制：上游不收的優化（例如 #297 預讀）在這裡可以做，但一樣要量測證明有效。

## Repo 與上游

- 位置：`D:\GitHubRepos\Z-DON`，分支 `main`，起點為上游 master `0f313b0`（2026-10-18）。
- `origin` = Yonokid/YataiDON，**只 fetch，永遠不 push**；定期 `git fetch origin && git merge origin/master`。
- `yataidon-local` = `D:/GitHubRepos/YataiDON`（wip-all 等本機分支），要搬舊修正時從這裡 cherry-pick。
- 目前只放本機，沒有 GitHub remote；要建 repo 先問使用者。
- 不從這個 repo 對上游開 PR（上游 PR 照舊走 YataiDON / YataiDON_bm）。
- 授權維持 GPL-3.0（LICENSE 不動），公開時須註明源自 YataiDON。

## 相容性原則（為了能持續合併上游）

- 保持與上游 skin 格式、`scores.db`、`config.toml` 相容：上游/ESE 的 skin 必須能直接用。
- 新功能預設關閉或以設定開關控制；不相容的格式改動要先問。
- 盡量把 Z-DON 的程式放在新檔案或小範圍改動，減少合併衝突。

## 建置

```bash
export PATH=/c/msys64/mingw64/bin:$PATH   # 少了這行 gcc 會靜默失敗
cd build && nice -n 10 cmake --build . -j 4
```

- Release 預設開 LTO（完整連結數分鐘）；開發中可 `cmake . -DZDON_LTO=OFF` 加快，**量效能和交付前要切回 ON**。
- 改完 `src/` 要跑 clang-format（上游 CI 用 23.1.3 `--dry-run --Werror`）：
  `"$APPDATA/Python/Python313/Scripts/clang-format.exe" -i $(cygpath -m <file>)`
- 建置好就停，**不要自動啟動遊戲測試**（使用者可能正在玩）；需要時先 `tasklist` 確認。

## 效能工作的規則

- 每個優化都要量「暖快取」和「冷硬碟」兩種數字，兩種都報；只在暖快取變快不算數。
- 冷硬碟測試架：skin 以 `robocopy /J` 從 `E:\yd_src\Skins` 複製到 `G:\yd_hddtest`（Toshiba HDD），再跑遊戲量畫面載入。
- 已證實無效、不要再試：
  - 單一執行緒依序讀檔（冷硬碟 1.1–1.6 s → 2.1–2.3 s；硬碟靠 NCQ 重排多個同時的讀取）。
  - Lua bytecode 快取（冷硬碟反而變慢，多了小檔 I/O）。
- 額外執行緒用背景優先權、記憶體要有上限；不要增加小檔案 I/O。

## 待辦

見 [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md)。
