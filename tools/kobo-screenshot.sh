#!/bin/sh
# Capture what is on the Kobo's screen right now (framebuffer page 0) as a PNG.
# Usage: tools/kobo-screenshot.sh [out.png]   (needs `ssh kobo` working)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../build/kobo-$(date +%Y%m%d-%H%M%S).png}"
ssh -o HostName=192.168.1.237 kobo 'cat /dev/fb0 > /tmp/fb.raw; /mnt/onboard/.adds/koreader/fbink -e 2>/dev/null | tr ";" "\n" | grep -E "^(viewWidth|viewHeight|BPP|lineLength)=" | tr "\n" " "' > /tmp/kobo-fbinfo.$$ || true
INFO="$(cat /tmp/kobo-fbinfo.$$)"; rm -f /tmp/kobo-fbinfo.$$
W=$(echo "$INFO" | sed -n 's/.*viewWidth=\([0-9]*\).*/\1/p'); H=$(echo "$INFO" | sed -n 's/.*viewHeight=\([0-9]*\).*/\1/p')
BPP=$(echo "$INFO" | sed -n 's/.*BPP=\([0-9]*\).*/\1/p'); LL=$(echo "$INFO" | sed -n 's/.*lineLength=\([0-9]*\).*/\1/p')
: "${W:=1072}" "${H:=1448}" "${BPP:=8}" "${LL:=1072}"
scp -q -o HostName=192.168.1.237 kobo:/tmp/fb.raw /tmp/kobo-fb.$$.raw
python3 -I - /tmp/kobo-fb.$$.raw "$OUT" "$W" "$H" "$BPP" "$LL" <<'PY'
import sys, zlib, struct
src, dst, w, h, bpp, ll = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
data = open(src, 'rb').read()
rows = []
for y in range(h):
    row = data[y*ll:(y+1)*ll]
    if bpp == 8: g = row[:w]
    elif bpp == 32: g = bytes(((row[i]*114 + row[i+1]*587 + row[i+2]*299)//1000) for i in range(0, w*4, 4))
    elif bpp == 16: g = bytes((((row[i+1] >> 3) * 255) // 31) for i in range(0, w*2, 2))
    else: g = row[:w]
    rows.append(b'\x00' + g)
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(dst, 'wb').write(png); print(dst)
PY
rm -f /tmp/kobo-fb.$$.raw
