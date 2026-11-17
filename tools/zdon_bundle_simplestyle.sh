#!/usr/bin/env bash
# YataiDONSimpleStyle and YataiDONSimpleStyleDark (its child) for the install zip
# (server/zdon_server.toml bundled_skins_dir). Only SimpleStyle's own files: it keeps "parent": "YataiDONNijiiro"
# (which builds on PyTaikoGreen), and Dark keeps "parent": "YataiDONSimpleStyle". The parents are not copied in:
# they belong to their authors, and a new install gets them from the server on first run (the onboarding screen
# selects a skin's parents with it). Updater bookkeeping (checksums, .skin-repo, tools/) is left out.
#   bash tools/zdon_bundle_simplestyle.sh [Skins dir] [out dir]
set -euo pipefail
cd "$(dirname "$0")/.."
SKINS="${1:-build/bin/Skins}"
OUT="${2:-build/skins_standalone}"
python -X utf8 tools/zdon_standalone_skin.py "$SKINS" YataiDONSimpleStyle "$OUT" --keep-parent
python -X utf8 tools/zdon_standalone_skin.py "$SKINS" YataiDONSimpleStyleDark "$OUT" --keep-parent
