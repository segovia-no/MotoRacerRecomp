#!/bin/bash
# PSP home-menu (XMB) images for the EBOOT, made from the player's own game (nothing of the game is in
# the repository), into build/psp-xmb/ (scripts/psp/package_hw.sh puts whatever is there into the EBOOTs):
#   ICON0.PNG  144x80   the item icon
#   PIC1.PNG   480x272  the background
# With a desktop build (build/cmake/motoracer, or MOTORACER_BIN) and a display, the title screen is
# rendered (scripts/dev/e2e.sh, a game window for ~16 s) and cut into the logo and the title picture.
# Otherwise the icon is the GOG installer's 256 px game icon and there is no background.
#
#   scripts/psp/xmb_assets.sh [game dir]
#
# PY=<python with Pillow> (default: .venv's, then python3); without Pillow no images are made.
set -uo pipefail
cd "$(dirname "$0")/../.."
if [ "$(uname -s)" = Darwin ]; then default_game="$HOME/Library/Application Support/MotoRacerRecomp/game"
else default_game="${XDG_DATA_HOME:-$HOME/.local/share}/MotoRacerRecomp/game"; fi
game="${1:-$default_game}"
out=build/psp-xmb
mkdir -p "$out"
rm -f "$out/ICON0.PNG" "$out/PIC1.PNG"
py=""
for c in ${PY:-} .venv/bin/python python3; do "$c" -c 'import PIL' 2>/dev/null && { py=$c; break; }; done
if [ -z "$py" ]; then
    echo "xmb_assets: no Python with Pillow: the EBOOT gets no home-menu images"
    exit 0
fi

bin="${MOTORACER_BIN:-build/cmake/motoracer}"
rendered=false
if [ -x "$bin" ] && { [ "$(uname -s)" = Darwin ] || [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; }; then
    # the title screen: skip the intro (ESC) and let the title picture settle
    if KEEP_FRAMES=1 MOTORACER_BIN="$bin" scripts/dev/e2e.sh xmb_title "6000 ESC 6000" 16 MOTORACER_SNAPSHOT_EVERY=60 \
        MOTORACER_GAME_DIR="$game" >/dev/null 2>&1; then
        frame=$(ls build/e2e/xmb_title/gpu_*.bmp 2>/dev/null | tail -1)
        if [ -n "$frame" ] && "$py" -I - "$frame" "$out" <<'EOF'
import sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGB')
w, h = im.size                      # the 640x480 title picture at the render scale
s = w / 640.0
box = lambda x0, y0, x1, y1: tuple(int(round(v * s)) for v in (x0, y0, x1, y1))
# icon: the logo with "RECOMPILED" (1.8:1)
im.crop(box(58, 44, 550, 317)).resize((144, 80), Image.LANCZOS).save(f'{out}/ICON0.PNG', optimize=True)
# background: a 16:9 band of the title picture, above the Start button
im.crop(box(0, 38, 640, 401)).resize((480, 272), Image.LANCZOS).save(f'{out}/PIC1.PNG', optimize=True)
EOF
        then rendered=true; fi
    fi
    rm -rf build/e2e/xmb_title build/e2e/xmb_title.png build/e2e/xmb_title.log
fi

if ! $rendered; then
    # the installer's game icon (largest size), centred on a dark band
    ico=$(ls "$game"/goggame-*.ico "$game"/Motoracer.ico 2>/dev/null | head -1)
    if [ -n "$ico" ]; then
        "$py" -I - "$ico" "$out" <<'EOF'
import sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
ico = Image.open(src)
sizes = sorted(ico.info.get('sizes', [ico.size]))
ico.size = sizes[-1]
icon = ico.convert('RGBA')
icon.thumbnail((76, 76), Image.LANCZOS)
band = Image.new('RGBA', (144, 80), (12, 14, 24, 255))
band.alpha_composite(icon, ((144 - icon.width) // 2, (80 - icon.height) // 2))
band.save(f'{out}/ICON0.PNG', optimize=True)
EOF
    else
        echo "xmb_assets: no game icon in $game: the EBOOT gets no home-menu images"
    fi
fi
ls "$out"
