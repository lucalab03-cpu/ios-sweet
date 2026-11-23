#!/usr/bin/env python3
"""A new kernel into a payload that already boots: its device tree (ADT) and boot-args stay as they are.

  repayload.py OLD_PAYLOAD KERNEL OUT [--set NAME=VALUE ...] [--expect TEXT ...]

KERNEL is the Mach-O from the xnu workflow's xnu-kernel artifact. --set adds a boot-arg or changes its value
(e.g. --set sweetmaliasync=0); --expect TEXT checks that the kernel contains TEXT (a log line of the patch it
must have), so a kernel from the wrong build is noticed before the phone boots it. Needs mkpayload.py and adt.py
next to this file (tools/ in the repo).
"""
import argparse, hashlib, os, struct, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkpayload  # noqa: E402


def read_payload(path):
    blob = open(path, 'rb').read()
    magic, version, nsegs = struct.unpack_from('<8sII', blob, 0)
    if magic != b'SWEETXNU':
        sys.exit(f'{path}: not a payload (magic {magic!r})')
    adt_off, adt_size = struct.unpack_from('<2Q', blob, 0x20)
    cmdline = blob[mkpayload.CMDLINE:mkpayload.CMDLINE + mkpayload.BOOT_LINE_LENGTH].split(b'\0')[0].decode()
    return blob, blob[adt_off:adt_off + adt_size], cmdline


def set_args(cmdline, settings):
    args = cmdline.split()
    for item in settings:
        name = item.split('=', 1)[0]
        args = [a for a in args if a.split('=', 1)[0] != name]
        args.append(item)
    return ' '.join(args)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('old')
    ap.add_argument('kernel')
    ap.add_argument('out')
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE')
    ap.add_argument('--expect', action='append', default=[], metavar='TEXT')
    a = ap.parse_args()

    _, adt_blob, cmdline = read_payload(a.old)
    kernel = open(a.kernel, 'rb').read()
    if kernel[:4] != b'\xcf\xfa\xed\xfe':
        sys.exit(f'{a.kernel}: not a 64-bit Mach-O kernel (unzip the artifact first)')
    for text in a.expect:
        if text.encode() not in kernel:
            sys.exit(f'{a.kernel}: does not contain "{text}" -- not the kernel with that patch')
    cmdline = set_args(cmdline, a.set)

    with tempfile.NamedTemporaryFile(suffix='.adt', delete=False) as f:
        f.write(adt_blob)
        adt_path = f.name
    try:
        blob, info = mkpayload.build(a.kernel, adt_path, cmdline)
    finally:
        os.unlink(adt_path)

    # the new payload carries the old one's ADT and boot-args, byte for byte
    new_adt, new_cmdline = read_payload_bytes(blob)
    assert new_adt == adt_blob, 'the ADT changed'
    assert new_cmdline == cmdline, 'the boot-args changed'
    open(a.out, 'wb').write(blob)
    print(f'{a.out}: {len(blob)} bytes, sha256 {hashlib.sha256(blob).hexdigest()}')
    print(f'  kernel {a.kernel} (sha256 {hashlib.sha256(kernel).hexdigest()[:16]}...), entry {info["entry"]}')
    print(f'  ADT from {a.old}: {len(adt_blob)} bytes, unchanged')
    print(f'  boot-args: {cmdline}')


def read_payload_bytes(blob):
    adt_off, adt_size = struct.unpack_from('<2Q', blob, 0x20)
    cmdline = blob[mkpayload.CMDLINE:mkpayload.CMDLINE + mkpayload.BOOT_LINE_LENGTH].split(b'\0')[0].decode()
    return blob[adt_off:adt_off + adt_size], cmdline


if __name__ == '__main__':
    main()
