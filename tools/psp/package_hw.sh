#!/bin/bash
# Folder for a real PSP (docs/25-psp-port.md): build/psp-hw/PSP/GAME/MOTORACER/ with a stripped EBOOT.PBP
# (5 MB instead of 50 MB with debug info) and the game files in game/ (the player's own GOG files;
# the CD-audio WAVs as MP3, tools/psp/cd_mp3.sh: 31 MB instead of 345 MB). Copy build/psp-hw/PSP to the
# memory stick root. Needs custom firmware.
# Home-menu icon, background and music from build/psp-xmb/ when present (tools/psp/xmb_assets.sh).
# Builds from build/psp as configured (MOTORACER_PSP_1000=ON: runs on every model, MEMSIZE=2).
#
#   tools/psp/package_hw.sh [game dir]     (default: the macOS install folder)
#
# PROF=1: psp_env.txt turns the sampling profiler on (docs/26-psp-profile.md).
# PROFILE_BUILD=<build dir>: also PSP/GAME/MOTORACER_PROF/ with that build's EBOOT (e.g. one configured
# with MOTORACER_PROF_FUNCS=ON), the profiler on, and the game files of MOTORACER/ (no second copy).
# PROFILE_ENV="KEY=VALUE ...": more settings for that one (e.g. MOTORACER_OPPONENTS=7 for an A/B).
# PROFILE_BUILD_B / _C (+ PROFILE_ENV_x, PROFILE_TITLE_x): more of them, PSP/GAME/MOTORACER_PROF_x/.
set -euo pipefail
cd "$(dirname "$0")/../.."
export PSPDEV="${PSPDEV:-$HOME/pspdev}" PATH="$HOME/pspdev/bin:$PATH"
game="${1:-$HOME/Library/Application Support/MotoRacerRecomp/game}"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

eboot() {   # <build dir> <out folder>
    cmake --build "$1" -j"$jobs" >/dev/null
    psp-strip "$1/motoracer" -o build/psp-hw/motoracer.elf
    mksfoex -d MEMSIZE=2 -s APP_VER=01.00 "$3" build/psp-hw/PARAM.SFO
    # home-menu icon / background / music (tools/psp/xmb_assets.sh: made from the player's game)
    local icon=NULL pic=NULL snd=NULL
    [ -f build/psp-xmb/ICON0.PNG ] && icon=build/psp-xmb/ICON0.PNG
    [ -f build/psp-xmb/PIC1.PNG ] && pic=build/psp-xmb/PIC1.PNG
    [ -f build/psp-xmb/SND0.AT3 ] && snd=build/psp-xmb/SND0.AT3
    pack-pbp "$2/EBOOT.PBP" build/psp-hw/PARAM.SFO "$icon" NULL NULL "$pic" "$snd" build/psp-hw/motoracer.elf NULL >/dev/null
    rm -f build/psp-hw/motoracer.elf build/psp-hw/PARAM.SFO
}

out=build/psp-hw/PSP/GAME/MOTORACER
rm -rf build/psp-hw && mkdir -p "$out"
eboot build/psp "$out" "Moto Racer Recompiled"
rsync -a --exclude '__support' --exclude '*.log*' --exclude 'psp_env.txt' --exclude '*.wav' "$game/" "$out/game/"
tools/psp/cd_mp3.sh "$game" "$out/game" >/dev/null
# the race settings / best times as shipped (data/bestscor.dat also keeps Time Attack and the lap count;
# desktop test runs change it): the original from the development checkout when there is one
if [ -f extracted/app/data/bestscor.dat ]; then cp extracted/app/data/bestscor.dat "$out/game/data/bestscor.dat"; fi
# the desktop config's draw distance (often 2) costs the PSP ~35% per race frame: original distance
sed -i '' -e 's/^DrawDistance=.*/DrawDistance=1/' "$out/game/motoracer.ini" 2>/dev/null || true
if [ -n "${PROF:-}" ]; then echo "MOTORACER_PROF=1" > "$out/psp_env.txt"; fi
echo "$out: $(du -sh "$out" | cut -f1) (EBOOT $(du -h "$out/EBOOT.PBP" | cut -f1))"

profile_app() {   # <build dir> <folder> <title> <extra env>
    local pout=build/psp-hw/PSP/GAME/$2
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
