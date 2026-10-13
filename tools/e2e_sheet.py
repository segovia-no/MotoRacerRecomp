"""Contact sheet of E2E snapshots: e2e_sheet.py <snapshot dir> <out.png> [glob, default gpu_*.bmp]"""
import sys, glob, os
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
pat = sys.argv[3] if len(sys.argv) > 3 else "gpu_*.bmp"
files = sorted(glob.glob(os.path.join(d, pat)))
W, cols = 320, 4
ims = []
for f in files:
    im = Image.open(f).convert("RGB")
    im.thumbnail((W, W))
    ims.append((os.path.basename(f), im))
rows = (len(ims) + cols - 1) // cols
H = max(i.height for _, i in ims) + 14
sheet = Image.new("RGB", (cols * W, rows * H), (40, 40, 40))
dr = ImageDraw.Draw(sheet)
for k, (n, im) in enumerate(ims):
    x, y = (k % cols) * W, (k // cols) * H
    sheet.paste(im, (x, y + 14))
    dr.text((x + 2, y), n, fill=(255, 255, 0))
sheet.save(out)
print(len(ims), sheet.size)
