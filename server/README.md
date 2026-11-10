# Z-DON 自架伺服器

`zdon_server.py` 一支程式包辦三件事，只需要 Python 3.11 以上（標準函式庫，不用 pip 安裝任何東西）：

| 功能 | 網址 | 給誰用 |
|---|---|---|
| 遊戲線上 API（帳號、成績、排行、重播、段位、獎牌、AI 對戰…） | `/health`、`/user`、`/submit_score`… | 遊戲（`config.toml` 的 `zdon_server_url`） |
| 遊戲更新（差量） | `/update/<頻道>/…` | `zdon_update.bat` 與遊戲內 F9 更新 |
| Skin 更新 | `/skins/<名稱>/raw/branch/<分支>/…` | 遊戲內建的 skin updater（`.skin-repo`） |
| 狀態頁 | `/` | 瀏覽器：玩家、最近成績、已發布的版本與 skin |

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
