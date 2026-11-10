# Z-DON 自架伺服器

`zdon_server.py` 一支程式包辦三件事，只需要 Python 3.11 以上（標準函式庫，不用 pip 安裝任何東西）：

| 功能 | 網址 | 給誰用 |
|---|---|---|
| 遊戲線上 API（帳號、成績、排行、重播、段位、獎牌、AI 對戰…） | `/health`、`/user`、`/submit_score`… | 遊戲（`config.toml` 的 `zdon_server_url`） |
| 遊戲更新（差量） | `/update/<頻道>/…` | `zdon_update.bat` 與遊戲內 F9 更新 |
| Skin 更新 | `/skins/<名稱>/raw/branch/<分支>/…` | 遊戲內建的 skin updater（`.skin-repo`） |
| 網站 zdon.taiko.ac | `/`、`/download`、`/charts`、`/player/…`、`/me` | 瀏覽器：下載、排行、玩家頁、會員綁定遊戲帳號（`zdon_site.py`） |

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

頁面：首頁（下載、最近遊玩、本月熱門）、下載（每個更新頻道的完整安裝包 zip，`publish` 時自動產生）、
Skin（`publish-skin` 時產生 zip，並附 `.skin-repo` 寫法）、排行（可搜尋曲名；曲名由遊戲送成績時附上）、
玩家頁、我的頁面（輸入 access code 綁定遊戲帳號、改名、從排行頁把歌送到遊戲的選曲畫面）。

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
