# Z-DON roadmap

## 從 YataiDON wip-all 搬過來（已在本機驗證過）

- [x] #314、#315：上游已合併，不用搬
- [x] 開發用鉤子（JUMP_SCREEN / DUMP_SCREEN / FAKE_KEYS / FORCE_AUTO / DUMP_CHARA），冷硬碟測試架要用
- [x] 響度統一（上游 PR #277，Ape）、段位每項考試最佳成績（cda1702，改存到 dan_scores.exam_bests）
- [x] Shift-JIS：上游 #292 已含
- [ ] 其餘 wip-all 本機修正視需要挑選（`git log origin/master..yataidon-local/wip-all`）

## 效能

- [x] 恢復跨畫面預讀（上游 #297 被拒）。`YATAIDON_NO_READAHEAD=1` 可關掉做 A/B。
  冷硬碟選曲貼圖（2026-10-18，各 3 次）：標題停 15 s 時 1196–1723 → 357–365 ms；停 5 s 時 1269–1333 → 542–832 ms（還沒讀完）
- [x] 預讀改成 8 個同時讀取（硬碟 NCQ）＋新畫面開始載入就取消：停 5 s 時 849–1021 → 342–358 ms；停 3 s 時 1328–1397（關）→ 964–1216 ms
- [x] 8 秒以上的音效先解前 2 秒、其餘背景解碼（選曲 bgm 載入 ≈211 → 23 ms；冷硬碟選曲 init 801–859 → 651–741 ms）
- [ ] 測試架改成直接清 Windows 檔案快取（不用每次重複製 skin，每輪 1.5 分 → 約 30 秒；需要管理員權限）
- [ ] 每個畫面的貼圖打包成單一檔案（冷硬碟效果應最大；需改 skin 格式與更新器，要先問）
- [ ] 預解碼更早開始（在上一個畫面淡出時）

進選曲畫面的時間分解（2026-10-18，暖快取 ≈680 ms）：
bgm 211 / GPU 上傳 147 / 等預解碼 129 / SongSelectPlayer 41 / 釋放 image 38 / 子集掃描 24 / Lua 17。
冷硬碟：貼圖 1.1–1.6 s ＋ 初始化 1.0–1.3 s。

## 程式碼精簡

- [x] 2026-10-19 全 src 精簡（haiku 審查 → sonnet 套用 → opus 驗證）：140 檔，+2191 / −4643。
  TJA 解析新舊對拍 3043 首 / 12116 譜面全部相同（工具：scratchpad 的 tja_equiv harness，連結舊 tja.cpp 比對）。
  順手修：TimelineObject::end_time 未初始化。

## 功能

（待使用者決定）
