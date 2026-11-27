#!/usr/bin/env python3
"""Pictures of gltrace_shot=1 (eagl.c shot_take) out of a fetch: each "sweetshot N: ..." block to OUTDIR/shot-N.png.
Rows come bottom up (OpenGL); they are turned the right way up here. usage: sweetshot.py FETCH.txt OUTDIR"""
import base64, re, sys, zlib, os
from PIL import Image

text = open(sys.argv[1], 'r', errors='replace').read()
os.makedirs(sys.argv[2], exist_ok=True)
head = re.compile(r'sweetshot (\d+): (.*?) \((\d+) x (\d+) kept\); RGBA rows bottom up, zlib, (\d+) bytes, base64:\n')
for m in head.finditer(text):
    n, what, w, h, size = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))
    end = text.find('\nsweetshot %d end' % n, m.end())
    body = re.sub(r'[^A-Za-z0-9+/=]', '', text[m.end():end if end > 0 else len(text)])
    try:
        raw = zlib.decompress(base64.b64decode(body))
    except Exception as x:
        print('shot %d: %s (%s)' % (n, x, what)); continue
    img = Image.frombytes('RGBA', (w, h), raw[:w * h * 4]).transpose(Image.FLIP_TOP_BOTTOM)
    out = os.path.join(sys.argv[2], 'shot-%02d.png' % n)
    img.save(out)
    print('%s: %s' % (out, what))
