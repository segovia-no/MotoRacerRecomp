#!/bin/bash
# Folder for a real PSP (docs/guides/psp-build.md, docs/notes/25-psp-port.md): <out>/PSP/GAME/MOTORACER/ with a
# stripped EBOOT.PBP
# (5 MB instead of 50 MB with debug info) and the game files in game/ (the player's own GOG files;
# the CD-audio WAVs as MP3, scripts/psp/cd_mp3.sh: 31 MB instead of 345 MB). Copy build/psp-hw/PSP to the
# memory stick root. Needs custom firmware.
# Home-menu icon and background from build/psp-xmb/ when present (scripts/psp/xmb_assets.sh).
# Builds from build/psp as configured (MOTORACER_PSP_1000=ON: runs on every model, MEMSIZE=2).
#
#   scripts/psp/package_hw.sh [game dir]     (default: the desktop install folder)
#
# PSP_BUILD=<dir>: the configured PSP build (default build/psp); PSP_OUT=<dir>: where PSP/ goes
# (default build/psp-hw; its PSP/ is replaced). PSPDEV: the toolchain (default ~/pspdev).
# PROF=1: psp_env.txt turns the sampling profiler on (docs/notes/26-psp-profile.md).
# PROFILE_BUILD=<build dir>: also PSP/GAME/MOTORACER_PROF/ with that build's EBOOT (e.g. one configured
# with MOTORACER_PROF_FUNCS=ON), the profiler on, and the game files of MOTORACER/ (no second copy).
# PROFILE_ENV="KEY=VALUE ...": more settings for that one (e.g. MOTORACER_OPPONENTS=7 for an A/B).
# PROFILE_BUILD_B / _C (+ PROFILE_ENV_x, PROFILE_TITLE_x): more of them, PSP/GAME/MOTORACER_PROF_x/.
set -euo pipefail
cd "$(dirname "$0")/../.."
export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PSPDEV/bin:$PATH"
if [ "$(uname -s)" = Darwin ]; then default_game="$HOME/Library/Application Support/MotoRacerRecomp/game"
else default_game="${XDG_DATA_HOME:-$HOME/.local/share}/MotoRacerRecomp/game"; fi
game="${1:-$default_game}"
[ -f "$game/game.EXE" ] || { echo "error: no game.EXE in $game" >&2; exit 1; }
build="${PSP_BUILD:-build/psp}" hw="${PSP_OUT:-build/psp-hw}"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

eboot() {   # <build dir> <out folder>
    cmake --build "$1" -j"$jobs" >/dev/null
    psp-strip "$1/motoracer" -o "$hw/motoracer.elf"
    mksfoex -d MEMSIZE=2 -s APP_VER=01.00 "$3" "$hw/PARAM.SFO"
    # home-menu icon / background (scripts/psp/xmb_assets.sh: made from the player's game)
    local icon=NULL pic=NULL
    [ -f build/psp-xmb/ICON0.PNG ] && icon=build/psp-xmb/ICON0.PNG
    [ -f build/psp-xmb/PIC1.PNG ] && pic=build/psp-xmb/PIC1.PNG
    pack-pbp "$2/EBOOT.PBP" "$hw/PARAM.SFO" "$icon" NULL NULL "$pic" NULL "$hw/motoracer.elf" NULL >/dev/null
    rm -f "$hw/motoracer.elf" "$hw/PARAM.SFO"
}

out=$hw/PSP/GAME/MOTORACER
rm -rf "$hw/PSP" && mkdir -p "$out/game"
eboot "$build" "$out" "Moto Racer Recompiled"
# the game files without the CD-audio WAVs (MP3 below), logs, or the Windows launcher and its files
(cd "$game" && tar cf - --exclude '__support' --exclude '*.log*' --exclude 'psp_env.txt' --exclude '*.wav' \
    --exclude '*.dll' --exclude 'MotoRacer.exe' --exclude 'webcache.zip' .) |
    (cd "$out/game" && tar xf -)
scripts/psp/cd_mp3.sh "$game" "$out/game" >/dev/null
# the race settings / best times as shipped (data/bestscor.dat also keeps Time Attack and the lap count;
# desktop test runs change it): the original from the development checkout when there is one
if [ -f extracted/app/data/bestscor.dat ]; then cp extracted/app/data/bestscor.dat "$out/game/data/bestscor.dat"; fi
# the desktop config's draw distance (often 2) costs the PSP ~35% per race frame: original distance
if [ -f "$out/game/motoracer.ini" ]; then
    sed -e 's/^DrawDistance=.*/DrawDistance=1/' "$out/game/motoracer.ini" > "$out/game/motoracer.ini.new"
    mv "$out/game/motoracer.ini.new" "$out/game/motoracer.ini"
fi
if [ -n "${PROF:-}" ]; then echo "MOTORACER_PROF=1" > "$out/psp_env.txt"; fi
echo "$out: $(du -sh "$out" | cut -f1) (EBOOT $(du -h "$out/EBOOT.PBP" | cut -f1))"

profile_app() {   # <build dir> <folder> <title> <extra env>
    local pout=$hw/PSP/GAME/$2
    mkdir -p "$pout"
    eboot "$1" "$pout" "$3"
    printf 'MOTORACER_PROF=1\nMOTORACER_GAME_DIR=ms0:/PSP/GAME/MOTORACER/game\n' > "$pout/psp_env.txt"
    for kv in $4; do echo "$kv" >> "$pout/psp_env.txt"; done
    echo "$pout: EBOOT $(du -h "$pout/EBOOT.PBP" | cut -f1), game files from MOTORACER/"
}
if [ -n "${PROFILE_BUILD:-}" ]; then profile_app "$PROFILE_BUILD" MOTORACER_PROF "Moto Racer Recompiled (profiling)" "${PROFILE_ENV:-}"; fi
# PROFILE_BUILD_B / PROFILE_ENV_B / PROFILE_TITLE_B (and _C): more profiling apps (A/B tests on hardware)
for x in B C; do
    bvar=PROFILE_BUILD_$x evar=PROFILE_ENV_$x tvar=PROFILE_TITLE_$x
    if [ -n "${!bvar:-}" ]; then profile_app "${!bvar}" "MOTORACER_PROF_$x" "${!tvar:-Moto Racer Recompiled (profiling $x)}" "${!evar:-}"; fi
done
