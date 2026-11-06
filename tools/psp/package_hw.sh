#!/bin/bash
# Folder for a real PSP (docs/25-psp-port.md): build/psp-hw/PSP/GAME/MOTORACER/ with a stripped EBOOT.PBP
# (5 MB instead of 50 MB with debug info) and the game files in game/ (the player's own GOG files,
# CD-audio WAVs included). Copy build/psp-hw/PSP to the memory stick root. Needs custom firmware.
# Builds from build/psp as configured (MOTORACER_PSP_1000=ON: runs on every model, MEMSIZE=2).
#
#   tools/psp/package_hw.sh [game dir]     (default: the macOS install folder)
set -euo pipefail
cd "$(dirname "$0")/../.."
export PSPDEV="${PSPDEV:-$HOME/pspdev}" PATH="$HOME/pspdev/bin:$PATH"
game="${1:-$HOME/Library/Application Support/MotoRacerRecomp/game}"
cmake --build build/psp -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null
out=build/psp-hw/PSP/GAME/MOTORACER
rm -rf build/psp-hw && mkdir -p "$out"
psp-strip build/psp/motoracer -o build/psp-hw/motoracer.elf
mksfoex -d MEMSIZE=2 -s APP_VER=01.00 "Moto Racer" build/psp-hw/PARAM.SFO
pack-pbp "$out/EBOOT.PBP" build/psp-hw/PARAM.SFO NULL NULL NULL NULL NULL build/psp-hw/motoracer.elf NULL >/dev/null
rm -f build/psp-hw/motoracer.elf build/psp-hw/PARAM.SFO
rsync -a --exclude '__support' --exclude '*.log*' --exclude 'psp_env.txt' "$game/" "$out/game/"
echo "$out: $(du -sh "$out" | cut -f1) (EBOOT $(du -h "$out/EBOOT.PBP" | cut -f1))"
