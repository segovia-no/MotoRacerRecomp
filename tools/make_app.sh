#!/bin/sh
# Assembles MotoRacer.app from a built motoracer binary.
#
# usage: tools/make_app.sh <motoracer binary> <output dir> [game dir for the icon]
# The app bundles SDL3 so it runs without Homebrew paths, and is ad-hoc signed.
# Game files are NOT bundled: the app finds them in ~/Library/Application Support/MotoRacerRecomp/game
# (see tools/install_game.sh) or via $MOTORACER_GAME_DIR.
set -eu
BIN=$1
OUT=$2
GAME=${3:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
APP="$OUT/MotoRacer.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
cp "$BIN" "$APP/Contents/MacOS/motoracer"
cp "$HERE/packaging/Info.plist" "$APP/Contents/Info.plist"

# Bundle SDL3 and point the executable at the bundled copy.
SDL=$(otool -L "$BIN" | awk '/libSDL3/ {print $1; exit}')
if [ -n "$SDL" ]; then
    REAL=$(cd "$(dirname "$SDL")" && pwd -P)/$(basename "$SDL")
    [ -f "$REAL" ] || REAL=$SDL
    cp -L "$REAL" "$APP/Contents/Frameworks/libSDL3.0.dylib"
    chmod u+w "$APP/Contents/Frameworks/libSDL3.0.dylib"
    install_name_tool -id @rpath/libSDL3.0.dylib "$APP/Contents/Frameworks/libSDL3.0.dylib"
    install_name_tool -change "$SDL" @rpath/libSDL3.0.dylib "$APP/Contents/MacOS/motoracer"
    install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/MacOS/motoracer" 2>/dev/null || true
fi

# Icon from the user's own GOG files (never committed).
if [ -n "$GAME" ] && [ -f "$GAME/goggame-1207658895.ico" ]; then
    TMP=$(mktemp -d)
    sips -s format png "$GAME/goggame-1207658895.ico" --out "$TMP/icon.png" >/dev/null
    mkdir "$TMP/MotoRacer.iconset"
    for s in 16 32 128 256; do
        sips -z $s $s "$TMP/icon.png" --out "$TMP/MotoRacer.iconset/icon_${s}x${s}.png" >/dev/null
        d=$((s * 2))
        sips -z $d $d "$TMP/icon.png" --out "$TMP/MotoRacer.iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$TMP/MotoRacer.iconset" -o "$APP/Contents/Resources/MotoRacer.icns"
    rm -rf "$TMP"
fi

codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || echo "warning: ad-hoc codesign failed" >&2
echo "built $APP"
