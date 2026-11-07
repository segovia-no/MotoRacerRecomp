"""Writes the largest PNG image stored in a Windows .ico file: ico_to_png.py <in.ico> <out.png>"""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
count = struct.unpack_from('<HHH', data)[2]
best = None
for i in range(count):
    w, h, _, _, _, _, size, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
    blob = data[offset:offset + size]
    if blob.startswith(b'\x89PNG') and (best is None or (w or 256) > best[0]):
        best = (w or 256, blob)
if best is None:
    sys.exit('no PNG image in ' + sys.argv[1])
open(sys.argv[2], 'wb').write(best[1])
