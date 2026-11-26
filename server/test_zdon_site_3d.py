"""Tests for the 3D look on my page: the model files and the viewer script (zdon_site_me.py, site_data/don3d.js).

    cd server && python -m unittest test_zdon_site_3d -v

The drawing itself is checked in a browser (it needs WebGL); here the server side: what is served, what is not.
"""
from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_zdon_score_import as base  # noqa: E402
import zdon_site_me  # noqa: E402

GLB = b"glTF" + b"\x02\x00\x00\x00" + b"\x00" * 12


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
