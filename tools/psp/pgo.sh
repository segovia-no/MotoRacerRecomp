#!/bin/bash
# Profile-guided PSP game code (docs/25-psp-port.md): builds an instrumented EBOOT (build/psp-pgo-gen,
# MOTORACER_PGO=gen; 64 MB memory: the counters do not fit a PSP-1000 next to the game), runs a
# scripted race in PPSSPP that writes the execution counts (.gcda; execution counts do not depend on
# PPSSPP's timing), keeps them in build/pgo-data/ and rebuilds build/psp with MOTORACER_PGO=use.
# The counts are not committed (they come from the player's game.EXE). Rerun after translator or
# game-code changes (functions whose code changed are compiled without profile).
#
#   tools/psp/pgo.sh            train and rebuild
#   tools/psp/pgo.sh use        rebuild build/psp with the counts already in build/pgo-data/
set -euo pipefail
cd "$(dirname "$0")/../.."
export PSPDEV="${PSPDEV:-$HOME/pspdev}" PATH="$HOME/pspdev/bin:$PATH"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

if [ "${1:-}" != "use" ]; then
    cmake -S . -B build/psp-pgo-gen -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMOTORACER_PGO=gen >/dev/null
    cmake --build build/psp-pgo-gen -j"$jobs" >/dev/null
    # the .gcda paths compiled in: /Users/.../build/psp-pgo-gen/CMakeFiles/gamecode.dir/gen/code_NNN.c.gcda;
    # GCOV_PREFIX_STRIP drops all directories, GCOV_PREFIX puts them in the run folder's gcda/
    obj_dir="$PWD/build/psp-pgo-gen/CMakeFiles/gamecode.dir/gen"
    strip=$(echo "$obj_dir" | tr -cd '/' | wc -c | tr -d ' ')
    rm -rf build/psp/run/gcda && mkdir -p build/psp/run/gcda
    # menus, then a Speed Bay race on the throttle (the timer ends it), then the Continue screen
    keys="6000 ESC 3000 ESC 2500 ENTER 1500 ENTER 4000 LEFT ENTER 3000 UP UP ENTER 3000 UP UP UP ENTER 4000 DOWN DOWN ENTER 25000 +UP 90000 -UP"
    WITH_MUSIC=1 PSP_BUILD=build/psp-pgo-gen tools/psp/e2e_psp.sh pgotrain "$keys" 150 \
        GCOV_PREFIX=umd0:/gcda GCOV_PREFIX_STRIP="$strip" MOTORACER_GCOV_DUMP_MS=125000 >/dev/null 2>&1 || true
    rm -rf build/psp/run/snap
    n=$(ls build/psp/run/gcda/*.gcda 2>/dev/null | wc -l | tr -d ' ')
    [ "$n" -gt 0 ] || { echo "no .gcda files written (see build/e2e/pgotrain.log)"; exit 1; }
    rm -rf build/pgo-data && mkdir -p build/pgo-data && cp build/psp/run/gcda/*.gcda build/pgo-data/
    echo "training run: $n .gcda files in build/pgo-data/"
fi
cmake -DMOTORACER_PGO=use build/psp >/dev/null
cp build/pgo-data/*.gcda build/psp/CMakeFiles/gamecode.dir/gen/
touch build/psp/gen/code_*.c   # recompile the game code with the counts
cmake --build build/psp -j"$jobs" >/dev/null
echo "build/psp: game code built with profile feedback"
