#!/bin/bash
# Z-DON: profile-guided Windows Release build (MinGW GCC) of the game target.
#   1. configure -DZDON_PGO=GEN -DZDON_LTO=OFF, build an instrumented YataiDON.exe (profile: build/pgo-data)
#   2. train: run it hidden (YATAIDON_TEST=1) through scenes that exit by themselves:
#      title, song select with scrolling, dan select, three charts on auto (dense, drumroll,
#      HBSCROLL) up to the result screen
#   3. configure -DZDON_PGO=USE -DZDON_LTO=ON and rebuild with the profile
# usage: tools/build_pgo.sh [--skip-gen] [--skip-train]
# Training charts default to C:/zdtest/charts/{dense,roll,hbs}/*.tja (ASCII paths); override with
# PGO_DENSE / PGO_ROLL / PGO_HBS. The build dir stays at ZDON_PGO=USE afterwards; a plain build
# again: cmake -DZDON_PGO=OFF . in build/.
set -eu
export PATH=/c/msys64/mingw64/bin:$PATH
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
BIN="$BUILD/bin"
PGO_DIR="$BUILD/pgo-data"
DENSE=${PGO_DENSE:-C:/zdtest/charts/dense/dense.tja}
ROLL=${PGO_ROLL:-C:/zdtest/charts/roll/roll.tja}
HBS=${PGO_HBS:-C:/zdtest/charts/hbs/hbs.tja}
GEN=1
TRAIN=1
for a in "$@"; do
  case $a in
    --skip-gen) GEN=0 ;;
    --skip-train) TRAIN=0 ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done

# configure <GEN|USE> <LTO ON|OFF>. The instrumented build is not LTO: GCC 16.2 MinGW fails the
# instrumented LTO link ("two or more sections for .gnu.lto___gcov_...__tcf_..."). Counters are
# added at compile time, before LTO, so the profile still matches the LTO compile of the USE build.
configure() { (cd "$BUILD" && cmake -DZDON_PGO="$1" -DZDON_LTO="$2" "-DZDON_PGO_DIR=$PGO_DIR" . >/dev/null); }
build() { (cd "$BUILD" && nice -n 10 cmake --build . --target YataiDON -j 4); }

game_running() {
  tasklist 2>/dev/null | grep -qi "yataidon\|zdon"
}
wait_free() {
  local i
  for i in $(seq 1 360); do
    if ! game_running; then return 0; fi
    echo "a game is running, waiting..."
    sleep 10
  done
  echo "game still running after an hour, giving up"; exit 2
}

# state files a run changes: config.toml and the "Recently Played" list in each song folder
TJA_DIRS=$(sed -n "s/^tja_path *= *\[\(.*\)\]/\1/p" "$BIN/config.toml" | tr ',' '\n' | sed "s/^ *['\"]//; s/['\"] *$//")
BAK=$(mktemp -d)
backup_state() {
  cp "$BIN/config.toml" "$BAK/config.toml"
  local n=0 d
  while IFS= read -r d; do
    [ -n "$d" ] || continue
    for f in "$d"/*"Recently Played"/song_list.txt; do
      [ -f "$f" ] && { cp "$f" "$BAK/rp$n"; echo "$f" >"$BAK/rp$n.path"; n=$((n + 1)); }
    done
  done <<<"$TJA_DIRS"
}
restore_state() {
  cp "$BAK/config.toml" "$BIN/config.toml"
  for p in "$BAK"/rp*.path; do
    [ -f "$p" ] && cp "${p%.path}" "$(cat "$p")"
  done
}

# run <name> <exit ms> <jump> <keys> [cli args...]; times are ms after the first frame
run() {
  local name=$1 at=$2 jump=$3 keys=$4
  shift 4
  wait_free
  local start=$SECONDS
  (cd "$BIN" && env YATAIDON_TEST=1 YATAIDON_DUMP_SCREEN="$(cygpath -w "$BAK/shots");$at" \
    YATAIDON_JUMP_SCREEN="$jump" YATAIDON_FAKE_KEYS="$keys" \
    timeout 900 ./YataiDON.exe "$@" >/dev/null 2>&1) || echo "  ($name exit code $?)"
  echo "  $name: $((SECONDS - start)) s"
}

train() {
  echo "== training"
  backup_state
  trap 'restore_state; rm -rf "$BAK"' EXIT
  run title 30000 "" ""
  run song_select 20000 "song_select;2000" \
    "6000:75,6400:75,6800:75,7200:75,7600:75,8000:75,8400:75,9000:74,11800:75,12100:75,12400:75,12700:75,13000:75,13300:75,13600:75,13900:68,14200:68,15400:75"
  run dan_select 12000 "dan_select;2000" "6000:75,6800:75,8000:68"
  run dense 200000 "" "154000:74,166000:74,172000:74,178000:74" "$DENSE" 3 --auto
  run roll 165000 "" "" "$ROLL" 3 --auto
  run hbs 155000 "" "" "$HBS" 3 --auto
  restore_state
  trap 'rm -rf "$BAK"' EXIT
  echo "  $(find "$PGO_DIR" -name '*.gcda' | wc -l) .gcda files in $PGO_DIR"
}

if [ $GEN = 1 ]; then
  echo "== instrumented build"
  rm -rf "$PGO_DIR"
  configure GEN OFF
  build
fi
[ $TRAIN = 1 ] && train
if [ "$(find "$PGO_DIR" -name '*.gcda' 2>/dev/null | wc -l)" = 0 ]; then
  echo "no profile data in $PGO_DIR"; exit 1
fi
echo "== optimized build"
configure USE ON
build
echo "done: $BIN/YataiDON.exe (build dir left at ZDON_PGO=USE)"
