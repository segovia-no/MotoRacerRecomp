#!/bin/bash
# Unattended end-to-end run: starts the game with scripted keys, captures snapshots and builds
# a contact sheet.  Opens a game window for the duration.
#
#   tools/e2e.sh <name> "<keys>" [seconds] [extra env assignments...]
#
# <keys> is MOTORACER_AUTOKEYS syntax: numbers wait (ms), names tap a key (ENTER ESC UP DOWN
# LEFT RIGHT SPACE or one letter/digit), +KEY / -KEY press / release.
# Output: build/e2e/<name>/ (BMP snapshots: gpu_* every 120 GPU frames, frame_* every 60
# software frames), build/e2e/<name>.png (contact sheet), build/e2e/<name>.log (game log).
# MOTORACER_BIN=<path> runs another build (default build/cmake/motoracer).
set -euo pipefail
cd "$(dirname "$0")/.."
name=$1 keys=$2 secs=${3:-60}
shift $(( $# < 3 ? $# : 3 ))
out=build/e2e/$name
rm -rf "$out" && mkdir -p "$out"
env MOTORACER_SNAPSHOT_DIR="$PWD/$out" MOTORACER_AUTOKEYS="$keys" "$@" \
    perl -e 'alarm shift; exec @ARGV' "$secs" "${MOTORACER_BIN:-build/cmake/motoracer}" >/dev/null 2>&1 || true
if [ "$(uname -s)" = Darwin ]; then logdir=$HOME/Library/Logs/MotoRacerRecomp
else logdir=${XDG_STATE_HOME:-$HOME/.local/state}/MotoRacerRecomp; fi
cp "$logdir/motoracer.log" "build/e2e/$name.log"
py=python3
for c in .venv/bin/python python3; do "$c" -c 'import PIL' 2>/dev/null && { py=$c; break; }; done
"$py" -I tools/e2e_sheet.py "$out" "build/e2e/$name.png" "${SHEET_GLOB:-gpu_*.bmp}"
echo "sheet: build/e2e/$name.png"
