#!/bin/sh
# One-step setup: unpacks your GOG Moto Racer installer, builds the recompiled game and installs
# MotoRacer.app with sensible defaults.
#
#   ./setup.sh [path/to/setup_moto_racer_2.1.0.11.exe]
#
# Without an argument the installer is looked for in ~/Downloads and the current folder.
# Re-running the script rebuilds and replaces the app (it asks first); game files, saves and
# settings stay, also when the installer is given again.
#
# Options (environment):
#   GAME_DIR=<dir>     where the game files go (default: ~/Library/Application Support/MotoRacerRecomp/game)
#   APP_DIR=<dir>      where MotoRacer.app goes (default: ~/Applications)
#   NO_BREW=1          do not install missing Homebrew packages, just report them
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
GAME_DIR=${GAME_DIR:-"$HOME/Library/Application Support/MotoRacerRecomp/game"}
APP_DIR=${APP_DIR:-"$HOME/Applications"}
BUILD="$HERE/build/cmake"

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nerror: %s\n' "$*" >&2; exit 1; }
# Re-running setup updates an existing install in place (same location, shortcuts keep working).
confirm_update() {
    [ -t 0 ] || return 0
    printf '\nMoto Racer is already installed (%s).\nRebuild and replace it? Saves, best times and settings are kept. [Y/n] ' "$1"
    read -r answer || answer=""
    case $answer in [nN]*) echo "Nothing changed."; exit 0 ;; esac
}

# --- platform -------------------------------------------------------------------------------
[ "$(uname -s)" = Linux ] && exec "$HERE/tools/linux/setup_linux.sh" "$@"   # Linux / Steam Deck
[ "$(uname -s)" = Darwin ] || die "this setup is for macOS and Linux; on Windows run setup.cmd"
[ "$(uname -m)" = arm64 ] || die "an Apple Silicon Mac (arm64) is required for now"
xcode-select -p >/dev/null 2>&1 || die "Xcode Command Line Tools are missing: run 'xcode-select --install' and re-run this script"
command -v brew >/dev/null 2>&1 || die "Homebrew is required: see https://brew.sh, then re-run this script"

# --- find the installer ------------------------------------------------------------------------
SETUP=${1:-}
if [ -z "$SETUP" ] && [ ! -f "$GAME_DIR/game.EXE" ]; then
    for f in "$HOME"/Downloads/setup_moto_racer*.exe ./setup_moto_racer*.exe; do
        [ -f "$f" ] && SETUP=$f && break
    done
    [ -n "$SETUP" ] || die "GOG installer not found: pass it as an argument, e.g. ./setup.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe"
fi

[ -d "$APP_DIR/MotoRacer.app" ] && confirm_update "$APP_DIR/MotoRacer.app"

# --- dependencies ------------------------------------------------------------------------------
say "Checking dependencies (cmake, sdl3, innoextract)"
MISSING=""
for pkg in cmake sdl3 innoextract; do
    brew list --versions "$pkg" >/dev/null 2>&1 || MISSING="$MISSING $pkg"
done
if [ -n "$MISSING" ]; then
    [ -z "${NO_BREW:-}" ] || die "missing Homebrew packages:$MISSING (brew install$MISSING)"
    say "Installing with Homebrew:$MISSING"
    # shellcheck disable=SC2086
    brew install $MISSING
fi

say "Preparing the recompiler (Python virtual environment)"
[ -x "$HERE/.venv/bin/python" ] || python3 -m venv "$HERE/.venv"
"$HERE/.venv/bin/python" -m pip install --quiet --disable-pip-version-check -r "$HERE/tools/recomp/requirements.txt"

# --- game files ----------------------------------------------------------------------------------
if [ -n "$SETUP" ]; then
    say "Unpacking $(basename "$SETUP") to $GAME_DIR"
    "$HERE/tools/install_game.sh" "$SETUP" "$GAME_DIR"
else
    say "Game files already installed in $GAME_DIR"
fi

# Port settings with good defaults (only if the player has none yet).
INI="$GAME_DIR/motoracer.ini"
if [ -f "$INI" ] && ! grep -q '^\[Recomp\]' "$INI"; then
    say "Writing default settings to motoracer.ini"
    cat "$HERE/tools/default_settings.ini" >>"$INI"
fi

# --- build -----------------------------------------------------------------------------------------
say "Building (the first build recompiles game.EXE and takes a minute)"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
      -DGAME_EXE="$GAME_DIR/game.EXE" >/dev/null
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu)"
cmake --build "$BUILD" --target app >/dev/null

say "Installing MotoRacer.app to $APP_DIR"
mkdir -p "$APP_DIR"
rm -rf "$APP_DIR/MotoRacer.app"
cp -R "$BUILD/MotoRacer.app" "$APP_DIR/"

cat <<EOF

Done. Start the game with:
    open "$APP_DIR/MotoRacer.app"
(or find "MotoRacer" in Launchpad / Spotlight).

First start: the game switches to its 16/9 mode at a native resolution, and a connected gamepad
gets an Xbox-style layout (RT/LT throttle/brake, LB/RB gears, A stunts, Start pause).
Game files and settings: $GAME_DIR
EOF
