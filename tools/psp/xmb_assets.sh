#!/bin/bash
# PSP home-menu (XMB) images for the EBOOT, made from the player's own game (nothing of the game is in
# the repository): the title screen is rendered by the desktop build (tools/e2e.sh) and cut into
#   ICON0.PNG  144x80   the item icon (the Moto Racer logo)
#   PIC1.PNG   480x272  the background (the title picture)
# in build/psp-xmb/. tools/psp/package_hw.sh puts whatever is there into the EBOOTs.
#
#   tools/psp/xmb_assets.sh
#
# Opens a game window for ~16 s.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/psp-xmb
mkdir -p "$out"
py=python3
for c in .venv/bin/python python3; do "$c" -c 'import PIL' 2>/dev/null && { py=$c; break; }; done

# the title screen: skip the intro (ESC) and let the title picture settle
KEEP_FRAMES=1 tools/e2e.sh xmb_title "6000 ESC 6000" 16 MOTORACER_SNAPSHOT_EVERY=60 >/dev/null
frame=$(ls build/e2e/xmb_title/gpu_*.bmp | tail -1)
"$py" -I - "$frame" "$out" <<'EOF'
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
rm -rf build/e2e/xmb_title build/e2e/xmb_title.png build/e2e/xmb_title.log

ls -la "$out"
