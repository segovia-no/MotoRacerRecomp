#!/bin/bash
# Unattended end-to-end run: starts the game with scripted keys, captures snapshots and builds
# a contact sheet.  Opens a game window for the duration.
#
#   tools/e2e.sh <name> "<keys>" [seconds] [extra env assignments...]
#
# <keys> is MOTORACER_AUTOKEYS syntax: numbers wait (ms), names tap a key (ENTER ESC UP DOWN
# LEFT RIGHT SPACE F1..F12 or one letter/digit), +KEY / -KEY press / release.
# Output: build/e2e/<name>/ (BMP snapshots: gpu_* every 120 GPU frames, frame_* every 60
# software frames), build/e2e/<name>.png (contact sheet), build/e2e/<name>.log (game log).
# MOTORACER_BIN=<path> runs another build (default build/cmake/motoracer).
# Snapshots are full render-target BMPs (~20 MB each at 2560x1920), so they are deleted once the
# contact sheet is made; KEEP_FRAMES=1 keeps them for closer inspection. Kept frames older than an
# hour are removed at the start of every run. Runs are windowed (MOTORACER_FULLSCREEN=0), whatever
# motoracer.ini says.
set -euo pipefail
cd "$(dirname "$0")/.."
name=$1 keys=$2 secs=${3:-60}
shift $(( $# < 3 ? $# : 3 ))
out=build/e2e/$name
mkdir -p build/e2e
find build/e2e -mindepth 2 -name '*.bmp' -mmin +60 -delete 2>/dev/null || true
find build/e2e -mindepth 1 -maxdepth 1 -type d -empty -delete 2>/dev/null || true
rm -rf "$out" && mkdir -p "$out"
env MOTORACER_FULLSCREEN=0 MOTORACER_SNAPSHOT_DIR="$PWD/$out" MOTORACER_AUTOKEYS="$keys" "$@" \
    perl -e 'alarm shift; exec @ARGV' "$secs" "${MOTORACER_BIN:-build/cmake/motoracer}" >/dev/null 2>&1 || true
if [ "$(uname -s)" = Darwin ]; then logdir=$HOME/Library/Logs/MotoRacerRecomp
else logdir=${XDG_STATE_HOME:-$HOME/.local/state}/MotoRacerRecomp; fi
cp "$logdir/motoracer.log" "build/e2e/$name.log"
py=python3
for c in .venv/bin/python python3; do "$c" -c 'import PIL' 2>/dev/null && { py=$c; break; }; done
"$py" -I tools/e2e_sheet.py "$out" "build/e2e/$name.png" "${SHEET_GLOB:-gpu_*.bmp}"
echo "sheet: build/e2e/$name.png"
if [ -z "${KEEP_FRAMES:-}" ]; then rm -rf "$out"; else echo "frames: $out (deleted after an hour)"; fi
