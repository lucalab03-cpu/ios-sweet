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
dev, bs, extents, cap = m['device'], m['block_size'], m['extents'], 100 << 20
OUTBOX = 128 << 20

def read_range(offset, length):
    """Bytes of the dropbox file from the phone's raw blocks (offset and length in multiples of the block size)."""
    out = b''
    pos = 0                               # file offset of the current extent
    for start_block, nblocks in extents:
        ext_bytes = nblocks * bs
        lo, hi = max(offset, pos), min(offset + length, pos + ext_bytes)
        if lo < hi:
            skip_blocks = (lo - pos) // bs
            count = (hi - lo + bs - 1) // bs
            out += subprocess.run(['adb', 'exec-out', 'dd if=%s bs=%d skip=%d count=%d 2>/dev/null' % (dev, bs, start_block + skip_blocks, count)],
                                  stdout=subprocess.PIPE, check=True).stdout
        pos += ext_bytes
        if pos >= offset + length:
            break
    return out

def fetch():
    """Prints the logs sweet-boot copied to the second half of the dropbox:  pack-dropbox.py MAP --fetch"""
    head = read_range(OUTBOX, bs)
    if head[:8] != b'SWOUT001':
        sys.exit("la outbox e' vuota: sweet-boot non e' ancora arrivato a scriverla")
    n = struct.unpack('<Q', head[8:16])[0]
    total = ((n + 16 + bs - 1) // bs) * bs
    data = read_range(OUTBOX, total)
    sys.stdout.write(data[16:16 + n].decode('utf-8', 'replace'))
    sys.exit(0)

if '--fetch' in sys.argv:
    fetch()
tree = sys.argv[2]

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
