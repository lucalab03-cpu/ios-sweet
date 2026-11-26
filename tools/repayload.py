#!/usr/bin/env python3
"""A new kernel into a payload that already boots: its device tree (ADT) and boot-args stay as they are.

  repayload.py OLD_PAYLOAD KERNEL OUT [--set NAME=VALUE ...] [--expect TEXT ...] [--adt-u32 /node/path:prop=N ...] [--adt-str /node/path:prop=TEXT ...]

KERNEL is the Mach-O from the xnu workflow's xnu-kernel artifact. --set adds a boot-arg or changes its value
(e.g. --set sweetmaliasync=0); --expect TEXT checks that the kernel contains TEXT (a log line of the patch it
must have), so a kernel from the wrong build is noticed before the phone boots it. --adt-u32 sets a 32-bit property of
an existing ADT node (path from the root, e.g. /arm-io/disp0:canvas-width=828); --adt-str sets a property to a
NUL-terminated string (e.g. /product:artwork-display-gamut=sRGB); everything else in the ADT stays. Needs mkpayload.py and adt.py
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
        if item != name + '=':          # NAME= takes the argument out
            args.append(item)
    return ' '.join(args)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('old')
    ap.add_argument('kernel')
    ap.add_argument('out')
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE')
    ap.add_argument('--expect', action='append', default=[], metavar='TEXT')
    ap.add_argument('--adt-u32', action='append', default=[], metavar='/PATH:PROP=N')
    ap.add_argument('--adt-str', action='append', default=[], metavar='/PATH:PROP=TEXT')
    ap.add_argument('--adt-bytes', action='append', default=[], metavar='/PATH:PROP=HEX',
                    help='set or add a property with these bytes (e.g. /chosen:mac-address-wifi0=025357454501)')
    a = ap.parse_args()

    _, adt_blob, cmdline = read_payload(a.old)
    kernel = open(a.kernel, 'rb').read()
    if kernel[:4] != b'\xcf\xfa\xed\xfe':
        sys.exit(f'{a.kernel}: not a 64-bit Mach-O kernel (unzip the artifact first)')
    for text in a.expect:
        if text.encode() not in kernel:
            sys.exit(f'{a.kernel}: does not contain "{text}" -- not the kernel with that patch')
    cmdline = set_args(cmdline, a.set)
    changed_adt = bool(a.adt_u32 or a.adt_str or a.adt_bytes)
    if a.adt_bytes:
        adt_blob = set_adt_bytes(adt_blob, a.adt_bytes)
    if a.adt_u32:
        adt_blob = set_adt_u32(adt_blob, a.adt_u32)
    if a.adt_str:
        adt_blob = set_adt_u32(adt_blob, a.adt_str, text=True)

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
    print(f'  ADT from {a.old}: {len(adt_blob)} bytes, ' + (', '.join(a.adt_u32 + a.adt_str + a.adt_bytes) if changed_adt else 'unchanged'))
    print(f'  boot-args: {cmdline}')


def set_adt_u32(blob, settings, text=False):
    import adt
    root, _ = adt.parse(blob)
    for item in settings:
        path, rest = item.split(':', 1)
        prop, value = rest.split('=', 1)
        node = root
        for part in [p for p in path.split('/') if p]:
            node = next((c for c in node.children if c.name == part), None)
            if node is None:
                sys.exit(f'ADT: no node {path}')
        for i, (n, flags, v) in enumerate(node.props):
            if n == prop:
                if text:
                    node.props[i] = (n, flags, value.encode() + b'\0')
                    break
                if len(v) != 4:
                    sys.exit(f'ADT: {path}:{prop} is {len(v)} bytes, not a u32')
                node.props[i] = (n, flags, struct.pack('<I', int(value, 0)))
                break
        else:
            sys.exit(f'ADT: {path} has no property {prop}')
    return adt.serialize(root)


def set_adt_bytes(blob, settings):
    """Sets a property to raw bytes, adding it to the node (flags 0) when it is not there."""
    import adt
    root, _ = adt.parse(blob)
    for item in settings:
        path, rest = item.split(':', 1)
        prop, value = rest.split('=', 1)
        data = bytes.fromhex(value)
        node = root
        for part in [p for p in path.split('/') if p]:
            node = next((c for c in node.children if c.name == part), None)
            if node is None:
                sys.exit(f'ADT: no node {path}')
        for i, (n, flags, v) in enumerate(node.props):
            if n == prop:
                node.props[i] = (n, flags, data)
                break
        else:
            node.props.append((prop, 0, data))
    return adt.serialize(root)


def read_payload_bytes(blob):
    adt_off, adt_size = struct.unpack_from('<2Q', blob, 0x20)
    cmdline = blob[mkpayload.CMDLINE:mkpayload.CMDLINE + mkpayload.BOOT_LINE_LENGTH].split(b'\0')[0].decode()
    return blob[adt_off:adt_off + adt_size], cmdline


if __name__ == '__main__':
    main()
