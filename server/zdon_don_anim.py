"""One animation of a skin's Models/animations.glb as a small JSON, for the 3D look on my page (site_data/don3d.js).

animations.glb is 12 MB (168 animations); the page needs one idle loop of a few dozen keyframes. Out:
  {"name", "duration", "nodes": [{"name", "parent", "t", "r", "s"}],             the skeleton at rest
   "tracks": [{"node", "path": "translation"|"rotation"|"scale", "times", "values"}]}   LINEAR keyframes
The EXTRA node's translation x * 100 is the face expression (chara_3d.cpp read_glb_faces).
"""
from __future__ import annotations

import json
import struct
import threading
from pathlib import Path

COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
_cache: dict = {}
_lock = threading.Lock()


def _accessor(doc: dict, bin_: bytes, i: int) -> list[float]:
    a = doc["accessors"][i]
    if a.get("componentType") != 5126:  # FLOAT only (what the exporter writes for times and TRS)
        raise ValueError("non-float accessor")
    v = doc["bufferViews"][a["bufferView"]]
    n = COMPONENTS[a["type"]]
    stride = v.get("byteStride") or n * 4
    off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
    out = []
    for k in range(a["count"]):
        out.extend(struct.unpack_from(f"<{n}f", bin_, off + k * stride))
    return out


def extract(path: Path, name: str) -> bytes | None:
    """The animation called name as JSON bytes, or None when the file has none of that name."""
    st = path.stat()
    key = (str(path), st.st_mtime_ns, st.st_size, name)
    with _lock:
        if key in _cache:
            return _cache[key]
    data = path.read_bytes()
    if data[:4] != b"glTF":
        return None
    jlen = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + jlen])
    blen = struct.unpack_from("<I", data, 20 + jlen)[0]
    bin_ = data[28 + jlen:28 + jlen + blen]
    anim = next((a for a in doc.get("animations", []) if a.get("name") == name), None)
    out = None
    if anim is not None:
        parent = {}
        for i, n in enumerate(doc["nodes"]):
            for c in n.get("children", []):
                parent[c] = i
        nodes = [{"name": n.get("name", ""), "parent": parent.get(i, -1), "t": n.get("translation", [0, 0, 0]),
                  "r": n.get("rotation", [0, 0, 0, 1]), "s": n.get("scale", [1, 1, 1])} for i, n in enumerate(doc["nodes"])]
        tracks, duration = [], 0.0
        for ch in anim["channels"]:
            target = ch.get("target", {})
            if "node" not in target or target.get("path") not in ("translation", "rotation", "scale"):
                continue
            s = anim["samplers"][ch["sampler"]]
            times = _accessor(doc, bin_, s["input"])
            values = _accessor(doc, bin_, s["output"])
            duration = max(duration, times[-1] if times else 0.0)
            tracks.append({"node": target["node"], "path": target["path"], "times": [round(t, 4) for t in times],
                           "values": [round(x, 5) for x in values]})
        out = json.dumps({"name": name, "duration": round(duration, 4), "nodes": nodes, "tracks": tracks},
                         separators=(",", ":")).encode()
    with _lock:
        if len(_cache) > 16:
            _cache.clear()
        _cache[key] = out
    return out
