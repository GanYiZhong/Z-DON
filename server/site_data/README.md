# site_data

`diffchart/*.json`: the difficulty tables (難易度表) from
[YataiDON-Syakai](https://github.com/Yonokid/YataiDON-Syakai) `src/data/diffchart/`, GPL-3.0, copied unchanged at
commit 1023550 (2026-11). The site's tables pages and the rating (`zdon_site_stats.py`) read them; the chart hashes
are the game's per-course hashes (`TJAParser::get_diff_hash`), the same in Z-DON and YataiDON.
