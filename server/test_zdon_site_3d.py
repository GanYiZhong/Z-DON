"""Tests for the 3D look on my page: the model files and the viewer script (zdon_site_me.py, site_data/don3d.js).

    cd server && python -m unittest test_zdon_site_3d -v

The drawing itself is checked in a browser (it needs WebGL); here the server side: what is served, what is not.
"""
from __future__ import annotations

import json
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_zdon_score_import as base  # noqa: E402
import zdon_site_me  # noqa: E402

GLB = b"glTF" + b"\x02\x00\x00\x00" + b"\x00" * 12


def anim_glb() -> bytes:
    """A skeleton of two nodes and one animation moving EXTRA (the face track)."""
    binary = struct.pack("<2f", 0.0, 0.5) + struct.pack("<6f", 0.01, 0, 0, 0.03, 0, 0)
    doc = {"asset": {"version": "2.0"}, "buffers": [{"byteLength": len(binary)}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 8}, {"buffer": 0, "byteOffset": 8, "byteLength": 24}],
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [0.5]},
                         {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}],
           "nodes": [{"name": "BONE_root", "children": [1]}, {"name": "EXTRA", "translation": [0, 0, 0]}],
           "animations": [{"name": "don_normal", "samplers": [{"input": 0, "output": 1}],
                           "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]}]}
    js = json.dumps(doc).encode()
    js += b" " * (-len(js) % 4)
    body = struct.pack("<I4s", len(js), b"JSON") + js + struct.pack("<I4s", len(binary), b"BIN\x00") + binary
    return b"glTF" + struct.pack("<II", 2, 12 + len(body)) + body


class Site3DTest(base.ImportTest):
    """On the scores.db test's server, with a small published PyTaikoGreen Models folder."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        models = Path(cls.tmp.name) / "data" / "skins" / "PyTaikoGreen" / "main" / "Models"
        for sub in ("cos", "head", "body", "face", "costume_icon"):
            (models / sub).mkdir(parents=True, exist_ok=True)
        (models / "costume_names.json").write_text(json.dumps({"costume": {"0": {"name": "なし"}}}), encoding="utf-8")
        (models / "cos" / "12.glb").write_bytes(GLB)
        (models / "head" / "0.glb").write_bytes(GLB)
        (models / "face" / "face_000000.png").write_bytes(b"\x89PNG face")
        (models / "secret.txt").write_text("no", encoding="utf-8")
        (models / "animations.glb").write_bytes(anim_glb())
        zdon_site_me.MePages._cosm_names = None  # names are kept per process; an earlier test class had no Models

    test_1_import_keeps_best = test_2_other_player_in_file = test_3_refusals = None

    def get(self, path: str) -> tuple[int, bytes, str]:
        try:
            with urllib.request.urlopen(self.base + path, timeout=10) as r:
                return r.status, r.read(), r.headers.get("Content-Type", "")
        except urllib.error.HTTPError as e:
            return e.code, e.read(), ""

    def test_files(self):
        status, body, ctype = self.get("/cosm/glb/cos/12.glb")
        self.assertEqual((status, body, ctype), (200, GLB, "model/gltf-binary"))
        self.assertEqual(self.get("/cosm/glb/head/0.glb")[0], 200)
        self.assertEqual(self.get("/cosm/glb/face.png")[:2], (200, b"\x89PNG face"))
        for bad in ("/cosm/glb/cos/13.glb", "/cosm/glb/body/0.glb", "/cosm/glb/secret.txt", "/cosm/glb/cos/..%2Fsecret.txt",
                    "/cosm/glb/../Models/secret.txt", "/cosm/glb/acce/1.glb", "/cosm/glb/cos/12.png"):
            self.assertEqual(self.get(bad)[0], 404, bad)

    def test_animation(self):
        status, body, ctype = self.get("/cosm/glb/anim/don_normal.json")
        self.assertEqual((status, ctype), (200, "application/json"))
        doc = json.loads(body)
        self.assertEqual(doc["duration"], 0.5)
        self.assertEqual([n["name"] for n in doc["nodes"]], ["BONE_root", "EXTRA"])
        self.assertEqual(doc["nodes"][1]["parent"], 0)
        self.assertEqual(doc["tracks"], [{"node": 1, "path": "translation", "times": [0.0, 0.5],
                                          "values": [0.01, 0.0, 0.0, 0.03, 0.0, 0.0]}])
        for bad in ("/cosm/glb/anim/don_nothing.json", "/cosm/glb/anim/Don.json", "/cosm/glb/anim/..%2Fx.json"):
            self.assertEqual(self.get(bad)[0], 404, bad)

    def test_no_costume_is_head_and_body(self):
        """きぐるみをはずす (costume 0) opens and saves as 頭飾＋身體."""
        import urllib.parse
        self.store.run("UPDATE users SET chara_is_costume=1, chara_cos_index=0, chara_head_index=0, chara_body_index=0 "
                       "WHERE access_code=?", (base.CODE_A,))
        status, page = self.request("/me", "uid-a")
        self.assertIn('name="mode" value="parts" checked', page)
        self.assertNotIn('name="mode" value="costume" checked', page)
        form = {"csrf": self.csrf("uid-a"), "code": base.CODE_A, "color1": "#68bfc0", "color2": "#f94728",
                "color3": "#f9f0e1", "mode": "costume", "head": "0", "body": "0", "costume": "0", "puchichara": ""}
        status, page = self.request("/me/look", "uid-a", urllib.parse.urlencode(form).encode(),
                                    "application/x-www-form-urlencoded")
        self.assertIn("外觀已儲存", page)
        row = self.store.one("SELECT chara_is_costume FROM users WHERE access_code=?", (base.CODE_A,))
        self.assertEqual(row["chara_is_costume"], 0)

    def test_script_and_page(self):
        status, body, ctype = self.get("/cosm/don3d.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", ctype)
        self.assertIn(b"window.Don3D", body)
        status, page = self.request("/me", "uid-a")
        self.assertEqual(status, 200)
        self.assertIn('id="don3d"', page)
        self.assertRegex(page, r'/cosm/don3d\.js\?v=[0-9a-f]+')
        self.assertIn('id="lookform"', page)


if __name__ == "__main__":
    import unittest
    unittest.main()
