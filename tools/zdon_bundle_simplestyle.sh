#!/usr/bin/env bash
# YataiDONSimpleStyle (standalone) and YataiDONSimpleStyleDark (its child) for the install zip
# (server/zdon_server.toml bundled_skins_dir). SimpleStyle carries the parts of YataiDONNijiiro and PyTaikoGreen
# it still uses in _base/ (tools/zdon_standalone_skin.py); Dark keeps "parent": "YataiDONSimpleStyle".
# Left out: the parents' background system (art packs, dancers, their data scripts), replaced by SimpleStyle's
# festival stage (Scripts/background/background.lua), and PyTaikoGreen's 3D character Models (SimpleStyle sets
# disable_chara_3d and draws its own 2D characters). Also left out, as the user decided 2026-11-13 (SimpleStyle
# doesn't use them): the art and sounds of upstream's AI battle, わいわい, campaigns / きのたけ, the reward shop,
# the gacha, tournaments and the total result.
#   bash tools/zdon_bundle_simplestyle.sh [Skins dir] [out dir]
set -euo pipefail
cd "$(dirname "$0")/.."
SKINS="${1:-build/bin/Skins}"
OUT="${2:-build/skins_standalone}"
python -X utf8 tools/zdon_standalone_skin.py "$SKINS" YataiDONSimpleStyle "$OUT" --exclude 'Models/*' \
    --exclude 'Graphics/background/*' --exclude 'Scripts/background/bg_data/*' \
    --exclude 'Scripts/background/dancer_data/*' --exclude 'Scripts/background/bg_collabs/*'     --exclude 'Graphics/*/ai_battle/*' --exclude 'Graphics/global/ai_transition/*'     --exclude 'Graphics/game/gauge_ai/*' --exclude 'Graphics/song_select/ai_intro/*'     --exclude 'Graphics/song_select/ai_rank/*' --exclude 'Graphics/ai_section_result/*'     --exclude 'Graphics/waiwai_*' --exclude 'Graphics/kinotake_*' --exclude 'Graphics/campaign/*'     --exclude 'Graphics/reward_shop/*' --exclude 'Graphics/total_result/*' --exclude 'Graphics/title/gacha/*'     --exclude 'Graphics/title/tournament/*' --exclude 'Graphics/entry/campaign/*'     --exclude 'Sounds/waiwai*' --exclude 'Sounds/campaign/*' --exclude 'Sounds/ai_section_result/*'     --exclude 'Sounds/reward_shop/*' --exclude 'Sounds/total_result/*' --exclude 'Sounds/kinotake_result/*'     --exclude 'Sounds/ai_select/*'
python -X utf8 tools/zdon_standalone_skin.py "$SKINS" YataiDONSimpleStyleDark "$OUT" --keep-parent
