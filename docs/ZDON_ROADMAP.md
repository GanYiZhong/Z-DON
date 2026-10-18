# Z-DON roadmap

## 從 YataiDON wip-all 搬過來（已在本機驗證過）

- [x] #314、#315：上游已合併，不用搬
- [x] 開發用鉤子（JUMP_SCREEN / DUMP_SCREEN / FAKE_KEYS / FORCE_AUTO / DUMP_CHARA），冷硬碟測試架要用
- [ ] 其餘 wip-all 本機修正視需要挑選（`git log origin/master..yataidon-local/wip-all`）

## 效能

- [x] 恢復跨畫面預讀（上游 #297 被拒）。`YATAIDON_NO_READAHEAD=1` 可關掉做 A/B。
  冷硬碟選曲貼圖（2026-10-18，各 3 次）：標題停 15 s 時 1196–1723 → 357–365 ms；停 5 s 時 1269–1333 → 542–832 ms（還沒讀完）
- [ ] 預讀改成多個同時的讀取（硬碟 NCQ），讓短停留時也讀得完
- [ ] BGM 先解前幾秒就開始播（選曲 bgm.ogg 整檔解碼 暖 ≈211 ms / 冷 ≈447 ms）
- [ ] 每個畫面的貼圖打包成單一檔案（冷硬碟效果應最大；需改 skin 格式與更新器，要先問）
- [ ] 預解碼更早開始（在上一個畫面淡出時）

進選曲畫面的時間分解（2026-10-18，暖快取 ≈680 ms）：
bgm 211 / GPU 上傳 147 / 等預解碼 129 / SongSelectPlayer 41 / 釋放 image 38 / 子集掃描 24 / Lua 17。
冷硬碟：貼圖 1.1–1.6 s ＋ 初始化 1.0–1.3 s。

## 功能

（待使用者決定）
