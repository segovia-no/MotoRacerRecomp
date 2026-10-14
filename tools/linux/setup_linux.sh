#!/bin/sh
# Linux / Steam Deck setup (started by ./setup.sh): unpacks your GOG Moto Racer installer, builds
# the recompiled game in Valve's Steam Runtime SDK container and installs it for your user, with a
# menu entry and (on SteamOS) a Steam library entry for Game Mode.
#
#   ./setup.sh [path/to/setup_moto_racer_2.1.0.11.exe]
#
# Needs podman (preinstalled on SteamOS 3.5+) or docker, and about 3 GB of disk for the build
# images. Nothing is installed system-wide; no root/sudo needed with podman.
#
# Options (environment):
#   GAME_DIR=<dir>     game files (default: ~/.local/share/MotoRacerRecomp/game)
#   NO_STEAM=1         do not add the game to the Steam library
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
APP_DIR="$DATA/MotoRacerRecomp"
GAME_DIR=${GAME_DIR:-"$APP_DIR/game"}
BUILD="$HERE/build/linux"
SDK_IMAGE=registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest
EXTRACT_IMAGE=docker.io/library/debian:bookworm-slim

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

[ "$(uname -m)" = x86_64 ] || die "Linux builds need an x86-64 PC or Steam Deck for now (found $(uname -m))"

# --- container engine ------------------------------------------------------------------------------
# Rootless podman maps the container's root to you; with docker, run as your own user so the
# build files stay yours. label=disable: no SELinux relabelling of your folders.
if command -v podman >/dev/null 2>&1; then
    ENGINE=podman USER_ARGS=""
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker USER_ARGS="--user $(id -u):$(id -g)"
else
    die "podman or docker is required (SteamOS 3.5+ has podman; elsewhere install it from your distribution)"
fi
run() { "$ENGINE" run --rm --security-opt label=disable "$@"; }

# --- find the installer ------------------------------------------------------------------------
SETUP=${1:-}
if [ -z "$SETUP" ] && [ ! -f "$GAME_DIR/game.EXE" ]; then
    for f in "$HOME"/Downloads/setup_moto_racer*.exe ./setup_moto_racer*.exe; do
        [ -f "$f" ] && SETUP=$f && break
    done
    [ -n "$SETUP" ] || die "GOG installer not found: pass it as an argument, e.g. ./setup.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe"
fi

# --- game files ----------------------------------------------------------------------------------
if [ -n "$SETUP" ]; then
    SETUP=$(cd "$(dirname "$SETUP")" && pwd)/$(basename "$SETUP")
    say "Unpacking $(basename "$SETUP") to $GAME_DIR"
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    if command -v innoextract >/dev/null 2>&1; then
        innoextract --gog --silent -d "$TMP" "$SETUP"
    else
        # SteamOS has no innoextract: run Debian's in a throwaway container.
        OWNER=""
        [ "$ENGINE" = docker ] && OWNER="chown -R $(id -u):$(id -g) /out"
        run -v "$SETUP":/in/setup.exe:ro -v "$TMP":/out "$EXTRACT_IMAGE" sh -c \
            "apt-get -qq update >/dev/null && apt-get -qq install -y innoextract >/dev/null &&
             innoextract --gog --silent -d /out /in/setup.exe; $OWNER"
    fi
    [ -f "$TMP/app/game.EXE" ] || die "game.EXE not found in the installer - is this the GOG Moto Racer setup?"
    mkdir -p "$GAME_DIR"
    cp -R "$TMP/app/." "$GAME_DIR/"
else
    say "Game files already installed in $GAME_DIR"
fi

INI="$GAME_DIR/motoracer.ini"
if [ -f "$INI" ] && ! grep -q '^\[Recomp\]' "$INI"; then
    say "Writing default settings to motoracer.ini"
    cat "$HERE/tools/default_settings.ini" >>"$INI"
fi

# --- build -----------------------------------------------------------------------------------------
say "Building in the Steam Runtime SDK container (first run downloads it and takes a few minutes)"
mkdir -p "$BUILD"
# shellcheck disable=SC2086
run $USER_ARGS -v "$HERE":/src:ro -v "$BUILD":/build -v "$GAME_DIR/game.EXE":/game/game.EXE:ro \
    "$SDK_IMAGE" bash /src/tools/linux/sdk_build.sh >"$BUILD/build.log" 2>&1 \
    || { tail -30 "$BUILD/build.log"; die "build failed - full log: $BUILD/build.log"; }

# --- install ---------------------------------------------------------------------------------------
say "Installing to $APP_DIR"
mkdir -p "$APP_DIR/bin" "$DATA/applications"
cp "$BUILD/cmake/motoracer" "$APP_DIR/bin/motoracer.new"
strip --strip-debug "$APP_DIR/bin/motoracer.new" 2>/dev/null || true
mv "$APP_DIR/bin/motoracer.new" "$APP_DIR/bin/motoracer"
ICON="$APP_DIR/motoracer.png"
python3 "$HERE/tools/linux/ico_to_png.py" "$GAME_DIR/goggame-1207658895.ico" "$ICON" 2>/dev/null || ICON=applications-games
DESKTOP="$DATA/applications/motoracer-recomp.desktop"
cat >"$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Moto Racer
Comment=Moto Racer, recompiled from your GOG copy
Exec="$APP_DIR/bin/motoracer"
Icon=$ICON
Categories=Game;
Terminal=false
EOF

# steamos-add-to-steam hands a steam:// link to Steam and waits; with Steam not running it starts
# Steam and never returns. Only use it with Steam already running, detached and time-limited.
STEAM_ADDED=""
if [ -z "${NO_STEAM:-}" ] && command -v steamos-add-to-steam >/dev/null 2>&1 && pgrep -x steam >/dev/null 2>&1; then
    say "Adding Moto Racer to your Steam library (confirm in Steam if it asks)"
    (timeout 20 steamos-add-to-steam "$DESKTOP" >/dev/null 2>&1 &)
    STEAM_ADDED=1
fi

cat <<EOF

Done. Start the game from your applications menu ("Moto Racer"), or:
    "$APP_DIR/bin/motoracer"
Steam Deck Game Mode: ${STEAM_ADDED:+the game should now be in your library. Otherwise: }in Desktop Mode open
Steam > Games > Add a Non-Steam Game to My Library > tick "Moto Racer" > Add Selected Programs.
Then start it from Game Mode (Library > Non-Steam). The built-in controls work as a gamepad.

First start: the game picks its 16/9 MODE at the screen's native resolution (Steam Deck: 1280x800;
change it in Options > SCREEN MODE, e.g. 1920x1080 on a TV) and the gamepad layout
(RT/LT throttle/brake, LB/RB gears, A stunts, Start pause).
Game files and settings: $GAME_DIR
Log: ${XDG_STATE_HOME:-$HOME/.local/state}/MotoRacerRecomp/motoracer.log
EOF
