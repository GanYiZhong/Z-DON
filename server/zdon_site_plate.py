"""The nameplate on the site, drawn as PyTaikoGreen draws it (Scripts/global/nameplate.lua): the shadow, the title row of
frame_top for the background (0 1P, 1 2P, 2 wood, 3 purple/platinum, 4 gold), the outline, the 1P badge, the name in
white with a black edge centred right of the badge, the title in black in the top row, and on purple and gold the
shimmer. The pieces are the published PyTaikoGreen skin's (Graphics/global/nameplate), the text is its font, cut
down to the characters shown (GET /cosm/font.ttf?t=...), so a page loads a few KB and not the whole 6.7 MB font.
"""
from __future__ import annotations

import html
import io
import struct
import threading
from collections import OrderedDict
from pathlib import Path

PLATE_W, PLATE_H, TOP_H, BADGE_W = 272, 64, 32, 64
PADDING = 25
PIECES = ("shadow", "frame_top", "outline", "1p", "2p", "shimmer")

PLATE_CSS = """
.npwrap{display:inline-block;vertical-align:middle}
.np{display:block;position:relative;width:272px;height:69px}.np>*{position:absolute;left:0;top:0}
.np .sh{left:3px;top:5px;width:272px;height:64px;opacity:.5}
.np .ft{width:272px;height:32px;background-repeat:no-repeat}
.np .ol,.np .bd{width:272px;height:64px}.np .bd{width:64px}
.np .tx{white-space:nowrap;transform-origin:center center;line-height:1;font-family:zdonplate,"Noto Sans JP",sans-serif}
.np .nm{top:30px;font-size:22px;color:#fff;-webkit-text-stroke:6px #000;paint-order:stroke fill}
.np .tt{top:8px;font-size:20px;color:#000}
.np .shim{width:70px;height:70px;top:-19px;opacity:0;pointer-events:none;animation:npshim 2s linear infinite}
@keyframes npshim{0%{transform:translateX(-22px) scale(.4);opacity:0}5%{transform:translateX(9px) scale(1);opacity:1}
20%{transform:translateX(189px) scale(1);opacity:1}25%{transform:translateX(220px) scale(.2);opacity:0}100%{opacity:0}}
"""

# fits the name and the title into their width as the game does (squeezed, not cut), once the font is in
PLATE_JS = """<script>
function npFit(root){(root||document).querySelectorAll('.np').forEach(function(p){
 p.querySelectorAll('.tx').forEach(function(t){t.style.transform='none';var w=t.scrollWidth,max=+t.dataset.max,c=+t.dataset.cx;
  var s=w>max?max/w:1;t.style.left=(c-w/2)+'px';t.style.transform='scaleX('+s+')';});});}
function npFont(text){var f=new FontFace('zdonplate','url(/cosm/font.ttf?t='+encodeURIComponent(text)+')');
 f.load().then(function(x){document.fonts.add(x);npFit();}).catch(function(){npFit();});}
document.addEventListener('DOMContentLoaded',function(){npFit();
 var all='';document.querySelectorAll('.np .tx').forEach(function(t){all+=t.textContent;});if(all)npFont(all);});
</script>"""


def nameplate_html(name: str, title: str, title_bg: int, scale: float = 1.5, ids: bool = False) -> str:
    """The plate as the player sees it on the song select (with the 1P badge)."""
    bg = title_bg if 0 <= title_bg <= 4 else 0
    left = BADGE_W / 2                       # the 1P badge takes half its width
    name_cx = (left + PLATE_W) / 2
    title_cx = left + (PLATE_W - left) / 2
    text_max = PLATE_W - PADDING - left
    nid, tid, fid, sid = ((' id="np_n"', ' id="np_t"', ' id="np_f"', ' id="np_s"') if ids else ("", "", "", ""))
    shim = "" if bg in (3, 4) else "display:none;"
    esc = html.escape
    return (f'<span class="npwrap" style="zoom:{scale}"><span class="np">'
            f'<img class="sh" src="/cosm/plate/shadow.png" alt="">'
            f'<span class="ft"{fid} style="background-image:url(/cosm/plate/frame_top.png);background-position:0 -{bg * TOP_H}px"></span>'
            f'<img class="shim"{sid} style="{shim}" src="/cosm/plate/shimmer.png" alt="">'
            f'<img class="ol" src="/cosm/plate/outline.png" alt=""><img class="bd" src="/cosm/plate/1p.png" alt="">'
            f'<span class="tx tt"{tid} data-cx="{title_cx}" data-max="{text_max}">{esc(title or "")}</span>'
            f'<span class="tx nm"{nid} data-cx="{name_cx}" data-max="{text_max}">{esc(name or "")}</span>'
            f'</span></span>')


def fixed_os2(data: bytes) -> bytes:
    """PyTaikoGreen's font says its OS/2 table is version 5 but stops at version 4's length; fontTools then can't
    read it. Say version 4 (as long as the table actually is)."""
    num = struct.unpack(">H", data[4:6])[0]
    for i in range(num):
        rec = 12 + 16 * i
        tag, _, offset, length = struct.unpack(">4sIII", data[rec:rec + 16])
        if tag == b"OS/2":
            version = struct.unpack(">H", data[offset:offset + 2])[0]
            if version >= 5 and length < 100:
                data = data[:offset] + struct.pack(">H", 4) + data[offset + 2:]
            break
    return data


class PlateFiles:
    """Serves the plate pieces and the cut-down font from the published PyTaikoGreen skin."""

    def __init__(self, skin_root: Path):
        self.root = skin_root          # data_dir/skins/PyTaikoGreen
        self.lock = threading.Lock()
        self.fonts: OrderedDict[str, bytes] = OrderedDict()

    def _skin(self) -> Path | None:
        for branch in ("main", *sorted(p.name for p in self.root.glob("*") if p.is_dir())):
            d = self.root / branch
            if (d / "Graphics" / "global" / "nameplate" / "outline.png").is_file():
                return d
        return None

    def piece(self, name: str) -> bytes | None:
        d = self._skin()
        if d is None or name not in PIECES:
            return None
        p = d / "Graphics" / "global" / "nameplate" / f"{name}.png"
        return p.read_bytes() if p.is_file() else None

    def font(self, text: str) -> bytes | None:
        chars = "".join(sorted({c for c in text[:400] if c >= " "})) or " "
        with self.lock:
            if chars in self.fonts:
                self.fonts.move_to_end(chars)
                return self.fonts[chars]
        d = self._skin()
        path = d / "Fonts" / "font.ttf" if d else None
        if path is None or not path.is_file():
            return None
        try:
            from fontTools import subset
            from fontTools.ttLib import TTFont
        except ImportError:
            return None
        font = TTFont(io.BytesIO(fixed_os2(path.read_bytes())))
        opts = subset.Options()
        opts.layout_features = ["*"]
        opts.name_IDs = ["*"]
        sub = subset.Subsetter(opts)
        sub.populate(text=chars)
        sub.subset(font)
        buf = io.BytesIO()
        font.save(buf)
        data = buf.getvalue()
        with self.lock:
            self.fonts[chars] = data
            while len(self.fonts) > 300:
                self.fonts.popitem(last=False)
        return data

