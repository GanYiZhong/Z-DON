# Z-DON 自架伺服器

`zdon_server.py` 一支程式包辦三件事，只需要 Python 3.11 以上（標準函式庫，不用 pip 安裝任何東西）：

| 功能 | 網址 | 給誰用 |
|---|---|---|
| 遊戲線上 API（帳號、成績、排行、重播、段位、獎牌、AI 對戰…） | `/health`、`/user`、`/submit_score`… | 遊戲（`config.toml` 的 `zdon_server_url`） |
| 遊戲更新（差量） | `/update/<頻道>/…` | `zdon_update.bat` 與遊戲內 F9 更新 |
| Skin 更新 | `/skins/<名稱>/raw/branch/<分支>/…` | 遊戲內建的 skin updater（`.skin-repo`） |
| 網站 zdon.taiko.ac | `/`、`/download`、`/songs`、`/leaderboard`、`/tables`、`/player/…`、`/me` | 瀏覽器：下載、排行、玩家頁、會員綁定遊戲帳號（`zdon_site.py`） |

資料全部在 `data_dir`（預設 `server/zdon_data/`）：`zdon.db`（SQLite）和發布出去的檔案。備份就是複製這個資料夾。

## 1. 啟動

```bat
cd server
copy zdon_server.example.toml zdon_server.toml
rem 編輯 zdon_server.toml：至少設定 auth_key（任意一長串文字）
start_server.bat
```

- 預設聽 `0.0.0.0:8730`（這台電腦所有網卡）。只想本機用就改 `host = "127.0.0.1"`。
- 防火牆（要讓區網／外網連進來時，系統管理員 PowerShell 執行一次）：
  `New-NetFirewallRule -DisplayName "Z-DON server" -Direction Inbound -Protocol TCP -LocalPort 8730 -Action Allow`
- 開機自動啟動：工作排程器 → 建立基本工作 → 「電腦啟動時」→ 啟動程式 `server\start_server.bat`，
  勾「不論使用者是否登入都執行」。
- 要從外網連：路由器把 TCP 8730 轉到這台電腦；**一定要設 `auth_key`**。這支伺服器只講 HTTP，
  要 HTTPS 就在前面放 Caddy／nginx 之類的反向代理（遊戲的網址改成 `https://…`）。

## 2. 讓遊戲連上來

每台玩的電腦，`config.toml`：

```toml
[network]
online_play = true
zdon_server_url = 'http://192.168.1.10:8730'   # 伺服器那台的位址
zdon_server_key = '和 zdon_server.toml 的 auth_key 一樣'
# zdon_update_url = ''        # 空白 = <zdon_server_url>/update/windows
# zdon_update_check = true    # 啟動時檢查新版（標題／登入畫面顯示，F9 更新）
```

- `online_play = false` 時線上 API 不連，但更新檢查照樣依 `zdon_update_check` 進行。
- 玩家帳號：登入畫面輸入名字（或刷卡）時伺服器會發一組 20 位數的 access code，存在 `config.toml`。
  `python zdon_server.py users` 列出所有玩家與 code。

## 3. 發布遊戲更新

建置完之後：

```bat
python server\zdon_server.py publish build\bin
```

- 預設發布 `YataiDON.exe`、`*.dll`、`shader\`、`zdon_update.ps1/.bat`（後兩個 install 資料夾沒有時從 repo 取）。
  `--with-skins` 連 `Skins\` 一起發；`--include`／`--exclude` 自訂（glob，可重複）。
- 永遠不會發布 `config.toml`、`scores.db`、`cache\`、`Songs\`、`Records\`、記錄檔。
- 版本字串預設是「日期-時間-git 短雜湊」，可用 `--version` 指定。
- 發布是先寫到暫存資料夾再整個換上，下載到一半的玩家不會拿到混合版本的清單。
- 多個頻道：`--channel windows-test`，玩家那邊把 `zdon_update_url` 指到 `http://…/update/windows-test`。

玩家端：

- 遊戲啟動時在背景檢查，有新版就在標題／登入畫面上方顯示，按 **F9**：遊戲關閉 → 更新程式只下載有變動的檔案
  （逐檔 SHA-256 驗證）→ 自動重新開啟遊戲。
- 或手動執行遊戲資料夾裡的 `zdon_update.bat`（`-CheckOnly` 只檢查：有更新時結束碼 10）。
- 更新只動伺服器發布過的檔案；新版不再包含、而且是之前由更新裝上的檔案會被移除。

## 4. 發布 Skin

```bat
python server\zdon_server.py publish-skin build\bin\Skins\YataiDONSimpleStyle --branch main
```

然後在玩家的 `Skins\YataiDONSimpleStyle\.skin-repo` 寫兩行：

```
http://192.168.1.10:8730/skins/YataiDONSimpleStyle
main
```

並把 `config.toml` 的 `[general] skin_updater = true`。遊戲啟動時就會比對並只下載變動的檔案。
（路徑含空白的檔案 skin updater 抓不到，發布時會跳過並列出。）

## 5. 其他指令

```bat
python server\zdon_server.py users                                  # 玩家列表
python server\zdon_server.py rename <access code> <新名字>
python server\zdon_server.py jump <access code> --hash <譜面 hash>   # 讓該玩家的遊戲跳到這首歌
python server\zdon_server.py jump <access code> --score-id <成績編號> # 或播放這筆重播
```

## 活動類功能

`zdon_server.toml` 裡的 `tournament`、`campaigns`、`kinotake`、`gacha_puchichara`、`[[shop]]` 預設全關或空白；
這些是簡化版（AI 對戰段位 = 每 10 個星星升一段、獎牌 = 每場 1 枚＋每日首場 3 枚、ドンポイント = 每首 魂量表÷10），
只為了讓遊戲的對應畫面有回應，不是上游 hiroba 的規則。`force_unlock = true` 時所有服裝都可用（同離線）。

## 網站 zdon.taiko.ac（會員與 TaikoForge 共用）

網站和遊戲 API 是同一個程序、同一個埠。會員是 TaikoForge 的：網站按「登入」會導到
`forge.taiko.ac/sso/zdon`，在那裡登入（或註冊、收驗證信、忘記密碼），再帶一張 60 秒有效、簽章過的票證回來。
這裡只記會員的 TaikoForge id 與 email、以及他綁了哪些遊戲帳號。

頁面：首頁（下載、Rating 前五名、本月熱門、最近遊玩）、下載（每個更新頻道的完整安裝包 zip，`publish` 時自動產生）、
Skin（`publish-skin` 時產生 zip，並附 `.skin-repo` 寫法）、歌曲 `/songs`（最多人玩的歌、可搜尋；曲名由遊戲送成績時附上，
舊網址 `/charts` 也導到這裡）與歌曲頁 `/song/<hash>`（同曲名的所有難度排行）、單一譜面排行 `/chart/<hash>`（挑戰、送到遊戲）、
Rating 排行 `/leaderboard`、難易度表 `/tables`、玩家頁（Rating、對照難易度表的王冠統計、Best 50、段位、最近遊玩）與
全部成績 `/player/<id>/scores`（搜尋、難度、排序、分頁）、我的頁面（`zdon_site_me.py`：綁定遊戲帳號；每個帳號的名牌〔名字、稱號、底色〕、角色外觀〔三個顏色、頭飾＋身體或全身服裝、
小夥伴，名稱與圖示取自已發布的 PyTaikoGreen skin 的 `Models/`，`force_unlock = false` 時只列已擁有的〕，遊戲下次登入時套用；
信用分與紀錄、送出的檢舉結果、獎牌、AI 對戰勝場；不列入 Rating 排行、玩家頁不公開〔`member_prefs` 表〕；遊玩紀錄可刪除、匯出 CSV；
從排行頁把歌送到遊戲的選曲畫面）。

Rating 與難易度表（`zdon_site_stats.py`）照 [YataiDON-Syakai](https://github.com/Yonokid/YataiDON-Syakai) 的算法：
魔王／裏魔王最好的 50 個譜面平均；譜面常數 = 星數 + 通關難易度表的等級加成，分數係數 60 萬分起算、100 萬為 1，
全連 +0.5、全良 +1.0。難易度表是 Syakai 的 `src/data/diffchart`（GPL-3.0），放在 `site_data/diffchart/`；
雜湊是遊戲的每難度雜湊，跟 YataiDON 相同，所以直接對得上。

### 網頁版（/play）

網頁版（Z-DON-Web）也由這個伺服器提供：`python zdon_server.py publish-web <Z-DON-Web>/build-web/bin` 會把
YataiDON.html/.js/.wasm/.data 放到 `data/web`，網址是 `/play/`（附 COOP/COEP 標頭，瀏覽器的多執行緒需要）。
網頁開始前會問 `/auth/game-account`：沒登入就給「用 TaikoForge 登入」（`/auth/login?next=/play/`，登入完回到
`/play/`）；登入了就用會員綁定的遊戲帳號玩（多個時讓玩家選、沒有就 `POST /auth/game-account/new` 建一個並綁定）。
TaikoForge 那邊不用改（回呼網址一樣是 `/auth/callback`）。網頁版建置時要編進同一把 `auth_key`
（Z-DON-Web 的 `tools/zdon_web_build.sh` 預設讀 Z-DON 的 `.env`）。

### 設定

1. TaikoForge 那邊（forge.taiko.ac 的環境變數）：
   ```
   TF_SSO_CLIENTS=zdon=https://zdon.taiko.ac/auth/callback
   TF_SSO_SECRET_ZDON=<openssl rand -hex 32 產生的值>
   ```
   改完重啟 TaikoForge。
2. `zdon_server.toml` 的 `[site]`：`url = "https://zdon.taiko.ac"`、`sso_secret` 填同一個值
   （或用環境變數 `ZDON_SSO_SECRET`）。
3. 用 Cloudflare Tunnel 開出去：同一條 tunnel（或新開一條），`config.yml` 的 `ingress` 加
   ```yaml
     - hostname: zdon.taiko.ac
       service: http://127.0.0.1:8730
   ```
   放在最後那行 `- service: http_status:404` 之前，然後 `cloudflared tunnel route dns <tunnel 名稱> zdon.taiko.ac`。
   只靠 tunnel 對外的話，`zdon_server.toml` 的 `host` 改成 `"127.0.0.1"`（區網的遊戲機要直連才留 `0.0.0.0`）。
4. 遊戲的 `zdon_server_url` 可以直接填 `https://zdon.taiko.ac`。

### 開機自動啟動

系統管理員 PowerShell 執行一次：

```bat
powershell -ExecutionPolicy Bypass -File server\install_autostart.ps1
```

會在工作排程器建立「Z-DON server」：開機就跑（不必登入）、當掉一分鐘後自動重啟。移除：
`Unregister-ScheduledTask -TaskName "Z-DON server" -Confirm:$false`。cloudflared 用 `cloudflared service install`
裝成服務即可（TaikoForge 的 DEPLOY.md 已經有）。

## 金鑰（tools/zdon_keygen.py）

```bat
python tools\zdon_keygen.py              rem 產生缺少的金鑰（已有的不會覆寫）
python tools\zdon_keygen.py --show       rem 只印指紋，比對各處是否一致
python tools\zdon_keygen.py --rotate auth  rem 換遊戲簽章 key（要重新建置並發布新版）
python tools\zdon_keygen.py --rotate sso   rem 換單一登入密鑰（TaikoForge 也要換）
```

每把 key 256 位元：作業系統 CSPRNG＋CPU 時序抖動＋本機識別（MachineGuid 等）＋時間等混成熵池，
以 SHA3-512／BLAKE2b／SHA-512 三種雜湊壓縮、HKDF-SHA512 依用途分開導出，最後再和一份 CSPRNG 亂數 XOR。
寫入（都不進 git，舊檔留 `.bak-時間`）：`.env`（編進遊戲的 `NETWORK_URL`／`NETWORK_AUTH_KEY`）、
`server/zdon_server.toml`、`server/zdon_secrets_taikoforge.env`（給 TaikoForge 的兩行環境變數）。

編了 key 的版本：`online_play` 預設開啟、更新來源預設 `NETWORK_URL/update/windows`；
config.toml 只填 `zdon_server_url` 不填 key 時沿用內建 key（方便連測試伺服器）。
換 `.env` 之後要 `cmake -U NETWORK_URL -U NETWORK_AUTH_KEY .` 再建置（CMake 會快取舊值）。
key 編進 exe 只是混淆，擋得住隨手亂打 API 的腳本，擋不住有心人拆 exe；真正的防線是伺服器端。

## 線上對戰

遊戲主選單的「線上對戰」（有設定伺服器時才出現）：

- **挑戰紀錄**：選歌和難度後，列出伺服器上這張譜面有打擊紀錄的成績（每人最佳一筆），選一筆就和它對打。
  對手在 2P 譜面照他的打擊紀錄打，分數高者勝。網站排行頁登入後每列也有「挑戰」按鈕，按了遊戲在選曲畫面就會開打。
- **自動配對／開房間／加入房間（4 位數代碼）**：即時對戰（`server/zdon_match.py`，房間只存在記憶體，伺服器重開就沒了）。
  房主選歌；對方的選曲畫面會自動跳到那首歌，用一般的難度選擇畫面選自己的難度和選項（Flashlight、隱藏等只套在自己的譜面）。兩邊都載入完歌曲後同時開始（先載入好的那邊會等，最多 20 秒）：伺服器給一個約 0.8 秒後的開始時間，
  兩邊先跟伺服器對過時鐘；有直連時房主再經直連對時、把開始時間換成對方的時鐘（同一台實測兩邊開始只差 3 ms），2P 譜面跟自己的音樂同步捲動，對手的打擊一送到就判定，對手的打擊不出聲。
  打擊經 WebSocket（`/match/ws`）一打就推給對方；連不上時才退回每 0.1 秒輪詢 `/match/sync`。
  另外兩邊會試著直接用 UDP 連線（伺服器只轉交雙方的位址：區網、IPv6、STUN 看到的公網位址，再同時互送封包打洞），
  打擊兩條路都送，先到的算；直連不通就只走伺服器。同一台實測經 Cloudflare 時直連平均 8 ms（WebSocket 約 300 ms）。
  遊戲第一次直連時 Windows 防火牆可能會詢問是否允許 YataiDON 使用網路，要允許才能被對方連到。
  實測（兩個遊戲在同一台、自動演奏）：本機伺服器 WebSocket 平均晚 9 ms、輪詢 104–114 ms；
  經 Cloudflare（這台連到的是聖荷西機房，來回約 350 ms）WebSocket 約 300 ms、輪詢約 1.4 秒（每次輪詢都重新建 TLS 連線）。
  結算以對手自己回報的最終成績為準。一首打完房間還在，房主接著選下一首。
- 需要遊戲帳號登入（不是本機的 0／1 或訪客）。對方沒有這個譜面（用譜面雜湊比對）時，房主的遊戲會把歌曲資料夾打包經伺服器傳過去
  （`/match/upload`、`/match/download`，只放在記憶體，上限約 85 MB）；對方收到後先比對雜湊，一致才放進第一個歌曲資料夾的
  `Z-DON Online` 分類；傳不過去或雜湊不符才請房主換歌。
- 三戰兩勝（使用者 2026-11-11）：第一首隨機決定誰選歌，之後由上一首輸的人選；1 比 1 或平手後再隨機決定；
  先拿到 2 勝的人贏得整場，宣布後房間解散。房間視窗顯示目前比數。
- 文字聊天：在房間裡按 Y 輸入（可用輸入法），Enter 送出、Esc 取消；訊息顯示在左下角（每則最多 120 字、一秒一則，
  房間保留最近 50 則）。遊戲進行中不顯示，回到房間後補上。
- 選歌的人選好後，對方直接進入那首歌的難度選擇，只能選難度和選項，不能離開（返回或 Esc 都會再打開）。
- 對戰中不能暫停或重新開始；「跳過」和「強制重來（sudden death）」選項不起作用；對手中途離開就算你贏。
- 投降：打歌時按暫停鍵（空白鍵）跳出確認，咚＝投降；這首對兩邊都直接結束，算投降的人輸（不扣信用分）。
- 信用分（`server/zdon_credit.py`）：100 分起、上限 100，完整打完一場 +2；比賽中途離開 -5；
  低於 80 不能自動配對、低於 60 不能線上對戰。選單顯示自己的信用分。
- 檢舉：在選單或房間按 R 寫事由送出（同一人一天一次）；管理員在網站 `/admin` 審核：成立＝被檢舉的人 -5，
  惡意檢舉＝檢舉的人 -5。管理員是 `zdon_server.toml` 的 `[site] admins`（TaikoForge 帳號 uid）。
