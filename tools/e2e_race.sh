#!/bin/bash
# E2E: skip the intro, optionally cycle Options > SCREEN MODE <n> times (Enter cycles the mode
# list), accept, and start a Single Race (Speed Bay). Opens a game window.
#
#   tools/e2e_race.sh <name> <screen-mode-presses> [seconds] [env assignments...]
#
# e.g. tools/e2e_race.sh race1024 2 50 MOTORACER_D3D_DEBUG=1
# TOGGLE_169=1 also toggles Options > 16/9 MODE.
# TOGGLE_169=1 also toggles Options > 16/9 MODE.
# Note: SCREEN MODE persists between runs (the game saves it), so count presses from the saved mode.
set -euo pipefail
name=$1 presses=$2 secs=${3:-50}
shift $(( $# < 3 ? $# : 3 ))
k="4000 ESC 1500 ESC 1500 ENTER 1500 ENTER 2000"            # intro, title, name entry -> main menu
if [ "$presses" -gt 0 ] || [ -n "${TOGGLE_169:-}" ]; then
    k="$k DOWN 300 DOWN 500 ENTER 2500"                      # main menu -> Options
    for _ in 1 2 3 4 5 6 7 8 9; do k="$k DOWN 300"; done     # -> SCREEN MODE
    for _ in $(seq "$presses"); do k="$k ENTER 400"; done    # cycle modes
    down=7
    if [ -n "${TOGGLE_169:-}" ]; then k="$k DOWN 300 ENTER 400"; down=6; fi   # 16/9 MODE
    for _ in $(seq $down); do k="$k DOWN 300"; done          # -> Accept
    k="$k ENTER 2500"
fi
k="$k LEFT 500 ENTER 1500"                                  # Play Solo
k="$k UP 300 UP 500 ENTER 1500"                             # Single Race
k="$k UP 300 UP 300 UP 500 ENTER 1500"                      # Speed Bay -> Continue
k="$k DOWN 300 DOWN 500 ENTER"                              # bike -> Start
exec "$(dirname "$0")/e2e.sh" "$name" "$k" "$secs" "$@"
