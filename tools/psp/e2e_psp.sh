#!/bin/bash
# PSP E2E run in PPSSPP (docs/25-psp-port.md): stages build/psp/run (EBOOT + a copy of the game files
# without the CD-audio WAVs), writes psp_env.txt with the dev settings, runs PPSSPP for <seconds>
# and builds a contact sheet from the GE snapshots. Opens a PPSSPP window.
#
#   tools/psp/e2e_psp.sh <name> "<keys>" [seconds] [KEY=VALUE ...]
#
# <keys>: MOTORACER_AUTOKEYS syntax (tools/e2e.sh). Output: build/e2e/<name>.png, build/e2e/<name>.log.
set -euo pipefail
cd "$(dirname "$0")/../.."
name=$1 keys=$2 secs=${3:-60}
shift $(( $# < 3 ? $# : 3 ))
run=build/psp/run
game="${MOTORACER_GAME_DIR:-$HOME/Library/Application Support/MotoRacerRecomp/game}"
mkdir -p "$run/game" build/e2e
rsync -a --exclude '*.wav' --exclude '__support' "$game/" "$run/game/"
cp build/psp/EBOOT.PBP "$run/"
rm -rf "$run/snap" && mkdir -p "$run/snap"
{
    echo "MOTORACER_AUTOKEYS=$keys"
    echo "MOTORACER_SNAPSHOT_DIR=umd0:/snap"   # absolute: the game changes into game/
    for kv in "$@"; do echo "$kv"; done
} > "$run/psp_env.txt"
rm -f "$run/motoracer.log"
perl -e 'alarm shift; exec @ARGV' "$secs" "$HOME/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL" \
    "$PWD/$run/EBOOT.PBP" > "$run/ppsspp.out" 2>&1 || true
cp "$run/motoracer.log" "build/e2e/$name.log" 2>/dev/null || echo "no log"
py=python3
for c in .venv/bin/python python3; do "$c" -c 'import PIL' 2>/dev/null && { py=$c; break; }; done
if ls "$run"/snap/*.bmp >/dev/null 2>&1; then
    "$py" -I tools/e2e_sheet.py "$run/snap" "build/e2e/$name.png" "gpu_*.bmp"
    echo "sheet: build/e2e/$name.png"
else
    echo "no snapshots"
fi
