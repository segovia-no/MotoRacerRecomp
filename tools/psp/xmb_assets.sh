#!/bin/bash
# PSP home-menu (XMB) assets for the EBOOT, made from the player's own game (nothing of the game is in
# the repository): the title screen is rendered by the desktop build (tools/e2e.sh) and cut into
#   ICON0.PNG  144x80   the item icon (the Moto Racer logo)
#   PIC1.PNG   480x272  the background (the title picture)
#   SND0.AT3            the item's music: a CD-audio track as ATRAC3 (needs atracdenc, see below)
# in build/psp-xmb/. tools/psp/package_hw.sh puts whatever is there into the EBOOTs.
#
#   tools/psp/xmb_assets.sh [track]     (default track 02: the title music; 30 s from SND0_START s)
#
# Opens a game window for ~16 s. ATRAC3 encoder: ATRACDENC=<path to atracdenc> (built from
# https://github.com/dcherednik/atracdenc); without it there is no SND0.AT3.
set -euo pipefail
cd "$(dirname "$0")/../.."
game="${MOTORACER_GAME_DIR:-$HOME/Library/Application Support/MotoRacerRecomp/game}"
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

track="${1:-02}"
if [ -n "${ATRACDENC:-}" ] && [ -f "$game/$track.wav" ]; then
    tmp=$(mktemp -d)
    # 30 s, 44.1 kHz 16-bit stereo (atracdenc's input), faded in and out
    ffmpeg -v error -y -ss "${SND0_START:-0}" -t 30 -i "$game/$track.wav" -ar 44100 -ac 2 -sample_fmt s16 \
        -af "afade=t=in:d=1,afade=t=out:st=28:d=2" "$tmp/snd0.wav"
    "$ATRACDENC" -e atrac3 --container riff -i "$tmp/snd0.wav" -o "$out/SND0.AT3" >/dev/null
    rm -rf "$tmp"
else
    rm -f "$out/SND0.AT3"
    echo "no SND0.AT3 (ATRACDENC not set or $track.wav missing)"
fi
ls -la "$out"
