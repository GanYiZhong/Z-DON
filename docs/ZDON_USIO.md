# USIO 實機驗證清單

USIO 支援是照 TaikoArcadeLoader 模擬的 bnusio.dll 寫的，還沒在真機台上驗證過。以下是到機台上確認的步驟。

## 1. 放置 bnusio.dll

- `[usio] dll` 預設是 `bnusio.dll`。相對路徑以**遊戲 exe 所在資料夾**為準。
- 把機台的 bnusio.dll 放在 exe 旁邊，或在 config 寫絕對路徑。程式用 `LOAD_WITH_ALTERED_SEARCH_PATH` 載入，bnusio.dll 需要的其他 DLL 放在同一資料夾即可。
- bnusio.dll 是萬代南夢宮的檔案，不隨 Z-DON 發佈。

## 2. config.toml 最小設定

```toml
[general]
log_level = "debug"        # 要看鼓面數值與 peaks 時才需要
zdon_coin_mode = true      # 測投幣扣 credit 時才需要
zdon_coin_credits = 0

[usio]
enabled = true
dll = "bnusio.dll"
threshold = 2000
```

## 3. 開機後看 latest.log

一定會出現：

- `USIO: using '<路徑>' (thresholds ..., hold-off ... ms, poll ... us)`
- `USIO: bnusio_Open returned <n>`（程式不檢查這個值，之後照樣繼續）
- `USIO: connected = 0` 或 `1`
- `USIO: board '<名稱>'`、`USIO: firmware <n>`
- `USIO: switches yes|no, coin counter yes|no, service counter yes|no`
- 約 3 秒後：`USIO: <n> reads/s (<n> us each)`

失敗時：

- `USIO: cannot load '<路徑>' (error N)`
- `USIO: '<路徑>' has no bnusio_Open / bnusio_GetAnalogIn`

debug 等級才有：

- `USIO: P1 left kat value N (other M)`（每次打擊）
- `USIO: peaks P1 ... P2 ...`（每 5 秒，各感應器的最大值）
- `USIO: switches 0x... (new 0x...)`

## 4. USIO 測試畫面

- 入口：設定 →「USIO」→「USIO 測試」（`[usio] enabled = true` 時才有這個項目）。
- 鍵盤：`[` `]` 全部門檻 ±250（按住 Shift 為 ±50），`R` 還原成 config 的值，`S` 存回 config.toml；TEST 或返回鍵離開。
- 畫面：黃線是門檻，實心條是當下讀數，淡色條是 1.5 秒內的最高值；數字欄是當下、最高、打擊次數、上一下的值。
- 同一邊打到之後，要等數值降回門檻以下才會算下一下。
- 下方：TEST、SERVICE、UP、DOWN、ENTER 按住時亮起，旁邊是原始位元值；投幣、SERVICE 計數器與 credit。

## 5. 沒有讀數時

- `connected = 0` 或畫面寫 NOT CONNECTED：板子連線有問題。
- 畫面寫 "USIO is not running"：下一行是原因，也看 latest.log。
- 投幣沒反應：程式固定用 `bnusio_Communication(0)`、`bnusio_GetCoin(0)`、`bnusio_GetService(0)`，沒有設定可改投幣口，要改得改 `src/libs/input/zdon_usio.cpp`。
- 遊戲啟動前已投的幣不算（第一次讀數當基準）；一次跳 100 以上視為板子重置而忽略；credit 上限 99。
- 按鍵位元（TEST 7、ENTER 9、DOWN 12、UP 13、SERVICE 14）照 TaikoArcadeLoader，真機可能不同：看 `switches 0x...` 那行對照。

## 6. 回報時請附上

- 板子名稱、firmware、connected 的值。
- latest.log 全檔（`log_level = "debug"`）。
- 測試畫面上每個感應器靜止時的數值，以及輕打、重打的最高值，還有你設的門檻。
- 投一枚幣前後的投幣計數；SERVICE、TEST 會不會亮。
- bnusio.dll 的來源與機台遊戲版本。
