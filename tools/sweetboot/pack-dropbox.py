#!/usr/bin/env python3
"""Puts files into the phone's dropbox from the VM, with the phone in Android and `adb root` done.

  python3 pack-dropbox.py dropbox.map.json CARTELLA [--dry-run]

CARTELLA is a tree as the root of iOS: CARTELLA/usr/local/lib/libOSMesa.8.dylib goes to /usr/local/lib/...
dropbox.map.json was made by install-gl.sh when the image was prepared (where the dropbox file's blocks are).
Takes seconds. At the next iOS boot, sweet-boot installs the files.
"""
import hashlib, json, os, struct, subprocess, sys, time

mapfile, tree = sys.argv[1], sys.argv[2]
dry = '--dry-run' in sys.argv
m = json.load(open(mapfile))
dev, bs, extents, cap = m['device'], m['block_size'], m['extents'], m['size']

payload = bytearray()
count = 0
for base, _, names in os.walk(tree):
    for n in sorted(names):
        full = os.path.join(base, n)
        rel = '/' + os.path.relpath(full, tree).replace(os.sep, '/')
        data = open(full, 'rb').read()
        mode = os.stat(full).st_mode & 0o777
        p = rel.encode()
        payload += struct.pack('<IIQ', len(p), mode, len(data)) + p + data
        count += 1
head = b'SWDROP01' + struct.pack('<QQ', int(time.time()), len(payload)) + hashlib.sha256(payload).digest() + b'\0' * 8
blob = head + bytes(payload)
print('%d file(s), %d bytes, dropbox holds %d' % (count, len(blob), cap))
if len(blob) > cap:
    sys.exit('troppo grande per la dropbox')
if dry:
    sys.exit(0)
blob += b'\0' * ((-len(blob)) % bs)

def run(cmd, data=None):
    return subprocess.run(cmd, input=data, stdout=subprocess.PIPE, check=True).stdout

pos = 0
for start_block, nblocks in extents:        # in file system blocks of `bs` bytes, from the device's start
    if pos >= len(blob):
        break
    chunk = blob[pos:pos + nblocks * bs]
    run(['adb', 'shell', 'dd of=%s bs=%d seek=%d conv=notrunc 2>/dev/null' % (dev, bs, start_block)], chunk)
    pos += len(chunk)
# read it back
pos = 0
for start_block, nblocks in extents:
    if pos >= len(blob):
        break
    want = blob[pos:pos + nblocks * bs]
    got = run(['adb', 'exec-out', 'dd if=%s bs=%d skip=%d count=%d 2>/dev/null' % (dev, bs, start_block, len(want) // bs)])
    if got != want:
        sys.exit('VERIFICA FALLITA nel blocco %d' % start_block)
    pos += len(want)
print('scritto e verificato; al prossimo avvio di iOS sweet-boot installa i file')
