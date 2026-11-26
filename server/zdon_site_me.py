"""My page (/me) on the site: per linked game account, the nameplate (name, title, background), the character's look
(colours, costume or head and body, puchichara), the account's state (credit and its log, reports sent, medals, AI
battles), privacy (off the rating leaderboard, a private player page) and the scores (delete a play, CSV export).
A mixin of zdon_site.Site.

The game reads the nameplate and the look from GET /user when the account logs in (entry.cpp), so what is set here
shows from the next login. Costume names and icons come from the published PyTaikoGreen skin (Models/).
"""
from __future__ import annotations

import csv
import io
import json
import re
import time
import urllib.parse
from pathlib import Path

import zdon_don_anim as don_anim
import zdon_site as S
from zdon_site_plate import PLATE_CSS, PLATE_JS, PlateFiles, nameplate_html

TITLE_BG = {0: "1P", 1: "2P", 2: "木色", 3: "紫色", 4: "金色"}
COSM_KINDS = {"costume": "costume_icon", "head": "costume_head_icon", "body": "costume_body_icon", "puchichara": "acce"}
OWNED_TYPE = {"costume": 2, "head": 3, "body": 4, "puchichara": 5}
HEX = re.compile(r"^#[0-9a-fA-F]{6}$")
PLAYS_PAGE = 50
DON3D_JS = Path(__file__).parent / "site_data" / "don3d.js"  # the 3D look (the URL carries its version)

PREFS_SCHEMA = """
CREATE TABLE IF NOT EXISTS member_prefs (
    access_code TEXT PRIMARY KEY, no_board INTEGER NOT NULL DEFAULT 0, private INTEGER NOT NULL DEFAULT 0
);
"""

ME_CSS = """
.acct-tabs{display:flex;gap:.4em;flex-wrap:wrap;margin:.4em 0 1em}.acct-tabs a,.acct-tabs span{padding:.3em .8em;border-radius:6px;
border:1px solid var(--line);background:#fff}.acct-tabs span{background:var(--ink);color:#fff}
.row{display:flex;gap:1em;flex-wrap:wrap;align-items:center;margin:.5em 0}.row label{min-width:5em;color:var(--muted);font-size:.9em}
.pick{display:flex;align-items:center;gap:.6em}.pick img{width:56px;height:56px;object-fit:contain;background:#f5ede4;border-radius:8px}
#i_puchichara{object-fit:cover;object-position:top}
input[type=color]{width:3em;height:2em;border:1px solid var(--line);border-radius:6px;padding:0;background:#fff}
.kv{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:.6em}.kv div{background:#faf5ef;border-radius:8px;padding:.5em .8em}
.kv b{display:block;font-size:1.4em}
.don3d-wrap{float:right;text-align:center;margin:0 0 .5em 1em}.don3d{display:block;width:220px;height:260px;border-radius:10px;
background:radial-gradient(circle at 50% 40%,#fff 0,#f5ede4 70%);cursor:grab}.don3d.loading{opacity:.55}.don3d.failed,.don3d.failed+span{display:none}
@media(max-width:600px){.don3d-wrap{float:none;margin:0 auto .5em}.don3d{margin:0 auto}}.danger{background:#fff;color:#c0392b;border:1px solid #f3c3bf}
""" + PLATE_CSS


def credit_reason(text: str) -> str:
    for pat, out in ((r"left room (\w+) mid-match", "對戰中途離開（房間 {0}）"), (r"finished a match in room (\w+)", "完成一場對戰（房間 {0}）"),
                     (r"report #(\d+) confirmed", "被檢舉成立（#{0}）"), (r"report #(\d+) found malicious", "惡意檢舉（#{0}）")):
        m = re.fullmatch(pat, text or "")
        if m:
            return out.format(*m.groups())
    return text or ""


class MePages:
    """Mixed into zdon_site.Site."""

    _cosm_names: dict | None = None

    def init_me(self):
        with self.s.lock:
            self.s.db.executescript(PREFS_SCHEMA)
        self.plates = PlateFiles(Path(self.cfg["data_dir"]) / "skins" / "PyTaikoGreen")

    def plate_piece(self, req, name: str):
        data = self.plates.piece(name)
        if data is None:
            req.send(404, b"", "image/png")
            return
        req.send(200, data, "image/png", {"Cache-Control": "max-age=86400"})

    def plate_font(self, req, query):
        data = self.plates.font(query.get("t", ""))
        if data is None:
            req.send(404, b"", "font/ttf")
            return
        req.send(200, data, "font/ttf", {"Cache-Control": "max-age=604800"})

    # ------------------------------------------------------------------------------------------------------------
    # costume data from the published PyTaikoGreen skin

    def _models_dir(self) -> Path | None:
        root = Path(self.cfg["data_dir"]) / "skins" / "PyTaikoGreen"
        for branch in ("main", *sorted(p.name for p in root.glob("*") if p.is_dir())):
            d = root / branch / "Models"
            if (d / "costume_names.json").is_file():
                return d
        return None

    def cosm_names(self) -> dict:
        if MePages._cosm_names is None:
            names = {k: {} for k in COSM_KINDS}
            d = self._models_dir()
            if d:
                try:
                    j = json.loads((d / "costume_names.json").read_text(encoding="utf-8"))
                    for kind in COSM_KINDS:
                        names[kind] = {int(k): v.get("name", "") for k, v in j.get(kind, {}).items() if k.isdigit()}
                except (OSError, ValueError):
                    pass
                # puchichara: every image in acce/, named where the names list has it
                acce = d / "acce"
                if acce.is_dir():
                    for f in acce.glob("*.png"):
                        if f.stem.isdigit():
                            names["puchichara"].setdefault(int(f.stem), "")
            MePages._cosm_names = names
        return MePages._cosm_names

    def cosm_icon(self, req, rest: str):
        kind, _, file = rest.partition("/")
        if kind == "plate" and file.endswith(".png"):
            self.plate_piece(req, file[:-4])
            return
        if kind == "don3d.js":
            req.send(200, DON3D_JS.read_bytes(), "text/javascript; charset=utf-8", {"Cache-Control": "max-age=86400"})
            return
        d = self._models_dir()
        if kind == "glb" and d:  # the 3D look on my page (site_data/don3d.js): cos/N.glb, head/N.glb, body/N.glb, face.png
            a = re.fullmatch(r"anim/([a-z0-9_]{1,40})\.json", file)
            if a:  # one animation out of the 12 MB animations.glb (zdon_don_anim.py)
                src = d / "animations.glb"
                body = don_anim.extract(src, a.group(1)) if src.is_file() else None
                if body is None:
                    req.send(404, b"", "text/plain")
                else:
                    req.send(200, body, "application/json", {"Cache-Control": "max-age=86400"})
                return
            m = re.fullmatch(r"(cos|head|body)/(\d{1,4})\.glb", file)
            path = d / m.group(1) / f"{m.group(2)}.glb" if m else d / "face" / "face_000000.png" if file == "face.png" else None
            if path is not None and path.is_file():
                req.send_cached_file(path, "model/gltf-binary" if m else "image/png")
                return
            req.send(404, b"", "text/plain")
            return
        if kind in COSM_KINDS and d and re.fullmatch(r"\d{1,4}\.png", file):
            path = d / COSM_KINDS[kind] / file
            if path.is_file():
                req.send(200, path.read_bytes(), "image/png", {"Cache-Control": "max-age=86400"})
                return
        req.send(404, b"", "image/png")

    def _choices(self, code: str, kind: str) -> list[int]:
        ids = sorted(self.cosm_names()[kind])
        if not self.cfg.get("force_unlock", True):
            owned = {r["id"] for r in self.s.q("SELECT id FROM owned WHERE access_code=? AND type=?",
                                                (code, OWNED_TYPE[kind]))}
            ids = [i for i in ids if i in owned or i == 0]
        return ids

    # ------------------------------------------------------------------------------------------------------------
    # prefs

    def prefs(self, code: str) -> dict:
        r = self.s.one("SELECT no_board, private FROM member_prefs WHERE access_code=?", (code,))
        return {"no_board": bool(r and r["no_board"]), "private": bool(r and r["private"])}

    def hidden_codes(self, field: str) -> set[str]:
        return {r["access_code"] for r in self.s.q(f"SELECT access_code FROM member_prefs WHERE {field}=1")}

    def viewer_owns(self, req, code: str) -> bool:
        return bool(req.sess and self._owns(req.sess["uid"], code))

    # ------------------------------------------------------------------------------------------------------------
    # the page

    def me(self, req, query, flash: str = "", err: bool = False):
        if not req.sess:
            req.redirect("/auth/login")
            return
        csrf = self._csrf(req.sess)
        linked = self._linked(req.sess["uid"])
        parts = [f"<h1>我的頁面</h1><p class='muted small'>TaikoForge 帳號：{S.esc(req.sess['email'])}"
                 f"（<a href='{S.esc(self.forge_url)}/account'>管理帳號</a>）</p>"]
        if linked:
            want = query.get("a", "")
            cur = next((r for r in linked if r["access_code"] == want), linked[0])
            if len(linked) > 1:
                parts.append('<div class="acct-tabs">' + "".join(
                    f'<span>{S.esc(r["username"])}</span>' if r is cur else
                    f'<a href="/me?a={urllib.parse.quote(r["access_code"])}">{S.esc(r["username"])}</a>' for r in linked) + "</div>")
            parts.append(self._account(cur, csrf))
        else:
            parts.append('<p class="muted">還沒有綁定遊戲帳號。</p>')
        parts.append(f"""<div class="card"><h2 style="margin-top:0">綁定遊戲帳號</h2>
<p class="small">遊戲帳號的 access code 在遊戲資料夾 <code>config.toml</code> 的 <code>[network]</code>
<code>access_code_1</code>（20 位數字）。綁定後可以在這裡改名牌和角色、從排行頁把歌送到遊戲。</p>
<form method="post" action="/me/link"><input type="hidden" name="csrf" value="{csrf}">
<input type="text" name="code" inputmode="numeric" autocomplete="off" placeholder="access code">
<button class="btn small">綁定</button></form></div>""")
        self.page(req, "我的頁面", "".join(parts), flash=flash, err=err)

    def _account(self, r, csrf: str) -> str:
        code = r["access_code"]
        u = self.s.one("SELECT * FROM users WHERE access_code=?", (code,))
        masked = code[:4] + "…" + code[-4:]
        hidden = (f'<input type="hidden" name="csrf" value="{csrf}">'
                  f'<input type="hidden" name="code" value="{S.esc(code)}">')
        out = [f'<div class="card"><h2 style="margin-top:0"><a href="/player/{r["pid"]}">{S.esc(r["username"])}</a> '
               f'<span class="muted small">遊戲帳號 {S.esc(masked)}</span></h2>'
               f'<p class="small muted">名牌與角色在遊戲下次登入這個帳號時套用。</p></div>']

        # nameplate
        bg = int(u["title_bg"] or 0)
        opts = "".join(f'<option value="{k}"{" selected" if k == bg else ""}>{v}</option>' for k, v in TITLE_BG.items())
        out.append(f"""<div class="card"><h2 style="margin-top:0">名牌</h2>
<p>{nameplate_html(u["username"], u["title"] or "", bg, 1.6, ids=True)}</p>
<form method="post" action="/me/profile">{hidden}
<div class="row"><label>名字</label><input type="text" name="name" maxlength="32" value="{S.esc(u["username"])}"
 oninput="npText('np_n',this.value)"></div>
<div class="row"><label>稱號</label><input type="text" name="title" maxlength="40" value="{S.esc(u["title"] or "")}"
 oninput="npText('np_t',this.value)"></div>
<div class="row"><label>底色</label><select name="title_bg" onchange="npBg(+this.value)">{opts}</select></div>
<button class="btn small">儲存名牌</button></form></div>{PLATE_JS}
<script>var npT;function npText(id,v){{document.getElementById(id).textContent=v;npFit();clearTimeout(npT);
npT=setTimeout(function(){{npFont(document.getElementById('np_n').textContent+document.getElementById('np_t').textContent)}},300);}}
function npBg(b){{document.getElementById('np_f').style.backgroundPosition='0 -'+(b*32)+'px';
document.getElementById('np_s').style.display=(b==3||b==4)?'':'none';}}</script>""")

        # character
        names = self.cosm_names()
        if any(names.values()):
            def picker(kind, label, current):
                ids = self._choices(code, kind)
                if current is not None and current not in ids:
                    ids = [current, *ids]
                items = "".join(f'<option value="{i}"{" selected" if i == current else ""}>'
                                f'{S.esc(names[kind].get(i) or f"#{i}")}</option>' for i in ids)
                img = f'/cosm/{kind}/{current if current is not None else 0}.png'
                none = '<option value="">（不戴）</option>' if kind == "puchichara" else ""
                return (f'<div class="row"><label>{label}</label><div class="pick"><img id="i_{kind}" src="{img}" alt="" '
                        f'onerror="this.style.visibility=\'hidden\'"><select name="{kind}" onchange="var i=document.getElementById'
                        f'(\'i_{kind}\');i.style.visibility=\'visible\';i.src=\'/cosm/{kind}/\'+(this.value||0)+\'.png\'">'
                        f'{none}{items}</select></div></div>')
            # きぐるみをはずす (costume 0) is no costume: the head and body are what is worn
            is_cos = bool(u["chara_is_costume"]) and bool(u["chara_cos_index"])
            colors = "".join(f'<input type="color" name="color{i}" value="{S.esc(u[f"chara_color_{i}"] if u[f"chara_color_{i}"] and HEX.match(u[f"chara_color_{i}"]) else d)}"> '
                             for i, d in ((1, "#68bfc0"), (2, "#f94728"), (3, "#f9f0e1")))  # the game's 1P default
            out.append(f"""<div class="card"><h2 style="margin-top:0">角色外觀</h2>
<div class="don3d-wrap"><canvas id="don3d" class="don3d" title="拖曳可以轉動"></canvas>
<span class="small muted">預覽：拖曳轉動</span></div>
<form method="post" action="/me/look" id="lookform">{hidden}
<div class="row"><label>顏色</label>{colors}<span class="small muted">身體、臉、臉的外框</span></div>
<div class="row"><label>穿法</label><label style="min-width:0;color:inherit"><input type="radio" name="mode" value="parts"{"" if is_cos else " checked"}> 頭飾＋身體</label>
<label style="min-width:0;color:inherit"><input type="radio" name="mode" value="costume"{" checked" if is_cos else ""}> 全身服裝</label></div>
{picker("head", "頭飾", u["chara_head_index"])}{picker("body", "身體", u["chara_body_index"])}
{picker("costume", "全身服裝", u["chara_cos_index"])}{picker("puchichara", "小夥伴", u["puchichara"])}
<button class="btn small">儲存外觀</button></form><div style="clear:both"></div></div>
<script src="/cosm/don3d.js?v={DON3D_JS.stat().st_mtime_ns:x}"></script><script>(function(){{var f=document.getElementById('lookform'),c=document.getElementById('don3d');
if(!window.Don3D||!f||!c)return;function look(){{var m=f.querySelector('input[name=mode]:checked');return{{mode:m&&m.value==='costume'?'costume':'parts',
costume:+f.costume.value||0,head:+f.head.value||0,body:+f.body.value||0,puchi:+f.puchichara.value||0,colors:[f.color1.value,f.color2.value,f.color3.value]}};}}
var v=Don3D.attach(c,look());f.addEventListener('input',function(){{v.set(look());}});f.addEventListener('change',function(e){{var n=e.target.name,m=n==='costume'?(+e.target.value?'costume':'parts'):(n==='head'||n==='body')?'parts':'';if(m)f.querySelector('input[name=mode][value='+m+']').checked=true;v.set(look());}});}})();</script>""")

        # account state
        credit = self.credit.get(code)
        log = self.s.q("SELECT delta, reason, at FROM credit_log WHERE access_code=? ORDER BY id DESC LIMIT 10", (code,))
        reports = self.s.q("SELECT id, reported, reason, status, created FROM reports WHERE reporter=? ORDER BY id DESC LIMIT 10",
                           (code,))
        ai = self.s.one("SELECT medals, ai_wins, ai_tokens FROM users WHERE access_code=?", (code,))
        log_rows = "".join(f'<tr><td class="small muted">{S.when(x["at"])}</td><td>{S.esc(credit_reason(x["reason"]))}</td>'
                           f'<td class="num">{"+" if x["delta"] > 0 else ""}{x["delta"]}</td></tr>' for x in log)
        verdict = {"pending": "審核中", "accepted": "成立", "rejected": "不成立", "malicious": "判定為惡意檢舉"}
        rep_rows = "".join(f'<tr><td class="small muted">{S.when(x["created"])}</td><td>{S.esc(self._name(x["reported"]))}</td>'
                           f'<td class="small">{S.esc(x["reason"])}</td><td>{verdict.get(x["status"], x["status"])}</td></tr>'
                           for x in reports)
        out.append(f"""<div class="card"><h2 style="margin-top:0">帳號狀態</h2>
<div class="kv"><div>信用分<b>{credit}</b><span class="small muted">80 分以上可快速配對、60 分以上可線上對戰</span></div>
<div>獎牌<b>{ai["medals"]}</b></div><div>AI 對戰勝場<b>{ai["ai_wins"]}</b></div></div>
<h3>信用分紀錄</h3>{('<table>' + log_rows + '</table>') if log else '<p class="small muted">沒有增減紀錄。</p>'}
<h3>我送出的檢舉</h3>{('<table><tr><th>時間</th><th>對象</th><th>事由</th><th>結果</th></tr>' + rep_rows + '</table>') if reports else '<p class="small muted">沒有送出過檢舉。</p>'}
</div>""")

        # privacy and scores
        p = self.prefs(code)
        out.append(f"""<div class="card"><h2 style="margin-top:0">隱私與成績</h2>
<form method="post" action="/me/privacy">{hidden}
<div class="row"><label style="min-width:0;color:inherit"><input type="checkbox" name="no_board" value="1"{" checked" if p["no_board"] else ""}>
 不列入 Rating 排行</label></div>
<div class="row"><label style="min-width:0;color:inherit"><input type="checkbox" name="private" value="1"{" checked" if p["private"] else ""}>
 玩家頁不公開（只有自己看得到；歌曲排行仍會列出成績）</label></div>
<button class="btn small sec">儲存</button></form>
<p class="small"><a href="/me/scores?code={urllib.parse.quote(code)}">管理遊玩紀錄（刪除打錯的成績）→</a>
<a href="/me/export.csv?code={urllib.parse.quote(code)}">匯出全部成績（CSV）</a>
<a href="/me/import?code={urllib.parse.quote(code)}">從 scores.db 匯入成績</a></p>
<form method="post" action="/me/unlink" onsubmit="return confirm('解除綁定這個遊戲帳號？')">{hidden}
<button class="btn small sec">解除綁定</button></form></div>""")
        return "".join(out)

    # ------------------------------------------------------------------------------------------------------------
    # posts

    def _back(self, req, code: str, flash: str, err: bool = False):
        self.me(req, {"a": code}, flash, err)

    def save_profile(self, req, form):
        code = form.get("code", "")
        name = form.get("name", "").strip()[:32]
        title = "".join(ch for ch in form.get("title", "") if ch >= " ").strip()[:40]
        bg = int(form.get("title_bg", "0")) if form.get("title_bg", "").isdigit() else 0
        if not self._owns(req.sess["uid"], code) or not name or bg not in TITLE_BG:
            self._back(req, code, "無法儲存名牌。", True)
            return
        self.s.run("UPDATE users SET username=?, title=?, title_bg=? WHERE access_code=?", (name, title, bg, code))
        self._back(req, code, "名牌已儲存；遊戲下次登入時套用。")

    def save_look(self, req, form):
        code = form.get("code", "")
        if not self._owns(req.sess["uid"], code):
            self._back(req, code, "無法儲存外觀。", True)
            return
        colors = [form.get(f"color{i}", "") for i in (1, 2, 3)]
        if not all(HEX.match(c) for c in colors):
            self._back(req, code, "顏色格式不對。", True)
            return
        worn = self.s.one("""SELECT chara_head_index AS head, chara_body_index AS body, chara_cos_index AS costume,
                                    puchichara FROM users WHERE access_code=?""", (code,))
        picks = {}
        for kind in ("head", "body", "costume", "puchichara"):
            v = form.get(kind, "")
            if v == "" and kind == "puchichara":
                picks[kind] = None
                continue
            # one of the choices, or the one already worn
            if not v.isdigit() or (int(v) not in self._choices(code, kind) and worn[kind] != int(v)):
                self._back(req, code, "這件服裝無法選擇。", True)
                return
            picks[kind] = int(v)
        self.s.run("""UPDATE users SET chara_color_1=?, chara_color_2=?, chara_color_3=?, chara_head_index=?,
                      chara_body_index=?, chara_cos_index=?, chara_is_costume=?, puchichara=? WHERE access_code=?""",
                   (*[c.upper() for c in colors], picks["head"], picks["body"], picks["costume"],
                    int(form.get("mode") == "costume" and picks["costume"] != 0), picks["puchichara"], code))
        self._back(req, code, "外觀已儲存；遊戲下次登入時套用。")

    def save_privacy(self, req, form):
        code = form.get("code", "")
        if not self._owns(req.sess["uid"], code):
            self._back(req, code, "無法儲存。", True)
            return
        self.s.run("""INSERT INTO member_prefs(access_code, no_board, private) VALUES (?,?,?)
                      ON CONFLICT(access_code) DO UPDATE SET no_board=excluded.no_board, private=excluded.private""",
                   (code, int(form.get("no_board") == "1"), int(form.get("private") == "1")))
        self.invalidate_board()
        self._back(req, code, "已儲存。")

    # ------------------------------------------------------------------------------------------------------------
    # scores: list, delete, export

    def my_scores(self, req, query, flash: str = "", err: bool = False):
        if not req.sess:
            req.redirect("/auth/login")
            return
        code = query.get("code", "")
        if not self._owns(req.sess["uid"], code):
            self.page(req, "找不到", "<h1>找不到這個遊戲帳號</h1>", 404)
            return
        page = int(query["page"]) if query.get("page", "").isdigit() and int(query["page"]) > 0 else 1
        total = self.s.one("SELECT COUNT(*) AS n FROM scores WHERE access_code=?", (code,))["n"]
        pages = max(1, (total + PLAYS_PAGE - 1) // PLAYS_PAGE)
        page = min(page, pages)
        rows = self.s.q("""SELECT id, hash, difficulty, score, crown, good, ok, bad, received_at FROM scores
                           WHERE access_code=? ORDER BY id DESC LIMIT ? OFFSET ?""", (code, PLAYS_PAGE, (page - 1) * PLAYS_PAGE))
        csrf = self._csrf(req.sess)
        q = urllib.parse.quote(code)
        trs = "".join(f'<tr><td class="small muted">{S.when(r["received_at"])}</td><td><a href="/song/{S.esc(r["hash"])}">'
                      f'{S.esc(self.song_name(r["hash"]))}</a></td><td>{S.course_badge(r["difficulty"])}</td>'
                      f'<td class="num">{(r["score"] or 0):,}</td><td>{S.crown_badge(r["crown"])}</td>'
                      f'<td class="small muted num">{r["good"]}/{r["ok"]}/{r["bad"]}</td><td><form method="post" action="/me/score/delete" '
                      f'onsubmit="return confirm(\'刪除這筆成績？刪掉後無法復原。\')"><input type="hidden" name="csrf" value="{csrf}">'
                      f'<input type="hidden" name="code" value="{S.esc(code)}"><input type="hidden" name="id" value="{r["id"]}">'
                      f'<input type="hidden" name="page" value="{page}"><button class="btn small danger">刪除</button></form></td></tr>'
                      for r in rows)
        pager = ""
        if pages > 1:
            prev = f'<a href="?code={q}&page={page - 1}">← 上一頁</a>' if page > 1 else "<span></span>"
            nxt = f'<a href="?code={q}&page={page + 1}">下一頁 →</a>' if page < pages else "<span></span>"
            pager = f'<div class="pager">{prev}<span class="muted small">{page} / {pages}</span>{nxt}</div>'
        name = self._name(code)
        body = (f'<h1>{S.esc(name)} 的遊玩紀錄</h1><p class="small"><a href="/me?a={q}">← 我的頁面</a>　'
                f'<a href="/me/export.csv?code={q}">匯出 CSV</a></p><p class="small muted">共 {total} 筆。刪除的成績會從排行、'
                f'Rating 和玩家頁消失；遊戲裡本機的紀錄不受影響。</p>'
                f'<div class="card">{("<table><tr><th>時間</th><th>曲目</th><th>難度</th><th class=num>分數</th><th></th>"
                                      "<th class=num>良/可/不可</th><th></th></tr>" + trs + "</table>") if rows else "<p class=muted>沒有成績。</p>"}</div>{pager}')
        self.page(req, "遊玩紀錄", body, flash=flash, err=err)

    def delete_score(self, req, form):
        code = form.get("code", "")
        sid = int(form["id"]) if form.get("id", "").isdigit() else 0
        q = {"code": code, "page": form.get("page", "1")}
        if not self._owns(req.sess["uid"], code) or not self.s.one("SELECT 1 FROM scores WHERE id=? AND access_code=?", (sid, code)):
            self.my_scores(req, q, "無法刪除這筆成績。", True)
            return
        self.s.run("DELETE FROM scores WHERE id=? AND access_code=?", (sid, code))
        self.invalidate_board()
        self.my_scores(req, q, "已刪除。")

    def export_csv(self, req, query):
        if not req.sess:
            req.redirect("/auth/login")
            return
        code = query.get("code", "")
        if not self._owns(req.sess["uid"], code):
            self.page(req, "找不到", "<h1>找不到這個遊戲帳號</h1>", 404)
            return
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["received_at", "title", "subtitle", "difficulty", "score", "crown", "good", "ok", "bad", "drumroll",
                    "max_combo", "hash"])
        for r in self.s.q("""SELECT s.*, g.title, g.subtitle FROM scores s LEFT JOIN songs g ON g.hash = s.hash
                             WHERE s.access_code=? ORDER BY s.id""", (code,)):
            w.writerow([time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(r["received_at"] or 0)), r["title"] or "",
                        r["subtitle"] or "", S.COURSES[r["difficulty"]] if 0 <= (r["difficulty"] or 0) < 5 else r["difficulty"],
                        r["score"], r["crown"], r["good"], r["ok"], r["bad"], r["drumroll"], r["max_combo"], r["hash"]])
        data = ("﻿" + buf.getvalue()).encode("utf-8")  # the BOM: Excel reads it as UTF-8
        req.send(200, data, "text/csv; charset=utf-8",
                 {"Content-Disposition": 'attachment; filename="zdon-scores.csv"', "Cache-Control": "no-store"})
