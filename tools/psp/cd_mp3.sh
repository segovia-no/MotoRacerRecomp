#!/bin/bash
# CD-audio WAVs -> MP3 for the PSP (docs/25-psp-port.md): the PSP decodes MP3 on its Media Engine,
# ~35 MB instead of 345 MB on the memory stick and a tenth of the reads. Writes <out>/NN.mp3 (CBR
# 128 kbps, 44.1 kHz stereo, no ID3 / Xing header: the decoder starts at byte 0) and <out>/cdaudio.txt
# ("NN.mp3 <CD frames>" per track: the virtual disc's table of contents keeps the WAV lengths).
# Converted files are kept in build/psp-mp3 and reused while the WAV is unchanged.
#
#   tools/psp/cd_mp3.sh <game dir> <out dir>
set -euo pipefail
game=$1 out=$2
cache="$(cd "$(dirname "$0")/../.." && pwd)/build/psp-mp3"
mkdir -p "$cache" "$out"
: > "$out/cdaudio.txt"
for wav in "$game"/[0-9][0-9].wav; do
    [ -e "$wav" ] || continue
    n=$(basename "$wav" .wav)
    mp3="$cache/$n.mp3"
    if [ ! -s "$mp3" ] || [ "$wav" -nt "$mp3" ]; then
        ffmpeg -nostdin -loglevel error -y -i "$wav" -codec:a libmp3lame -b:a 128k -ar 44100 -ac 2 \
            -id3v2_version 0 -write_xing 0 -map_metadata -1 "$mp3.tmp.mp3"
        mv "$mp3.tmp.mp3" "$mp3"
    fi
    cp "$mp3" "$out/$n.mp3"
    # CD frames = PCM bytes / 2352, from the WAV's "data" chunk
    frames=$(python3 -I - "$wav" <<'PY'
import struct, sys
with open(sys.argv[1], 'rb') as f:
    f.seek(12)
    while True:
        h = f.read(8)
        if len(h) < 8: sys.exit("no data chunk")
        cid, n = h[:4], struct.unpack('<I', h[4:])[0]
        if cid == b'data': print(n // 2352); break
        f.seek(n + (n & 1), 1)
PY
)
    echo "$n.mp3 $frames" >> "$out/cdaudio.txt"
done
echo "$out: $(wc -l < "$out/cdaudio.txt" | tr -d ' ') tracks, $(du -ch "$out"/*.mp3 | tail -1 | cut -f1)"
