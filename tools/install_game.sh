#!/bin/sh
# Extracts the GOG Moto Racer installer into the folder the game looks in by default.
#
# usage: tools/install_game.sh <setup_moto_racer_2.1.0.11.exe> [destination]
#   destination defaults to ~/Library/Application Support/MotoRacerRecomp/game (macOS) or
#   ~/.local/share/MotoRacerRecomp/game (Linux)
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 <setup_moto_racer_*.exe> [destination]" >&2
    exit 2
fi
SETUP=$1
if [ "$(uname -s)" = Darwin ]; then DEFAULT="$HOME/Library/Application Support/MotoRacerRecomp/game"
else DEFAULT="${XDG_DATA_HOME:-$HOME/.local/share}/MotoRacerRecomp/game"; fi
DEST=${2:-$DEFAULT}

if ! command -v innoextract >/dev/null 2>&1; then
    echo "innoextract not found: install it with 'brew install innoextract' (macOS) or your package manager" >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
innoextract --gog --silent -d "$TMP" "$SETUP"
if [ ! -f "$TMP/app/game.EXE" ]; then
    echo "game.EXE not found in the installer - is this the GOG Moto Racer setup?" >&2
    exit 1
fi
# The player's files in the installer (settings and controls, best times, launcher/port settings)
# are kept when the game is already installed; championship saves are not in the installer.
for f in data/CONFIG.BPT data/bestscor.dat motoracer.ini; do
    [ -f "$DEST/$f" ] && rm -f "$TMP/app/$f"
done
mkdir -p "$DEST"
cp -R "$TMP/app/." "$DEST/"
echo "Moto Racer installed to: $DEST"
