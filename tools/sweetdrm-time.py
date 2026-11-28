#!/usr/bin/env python3
"""Where CoreAnimation's render thread spends its time, from fix30's /var/tmp/sweetdrm-time.log.

  sweetdrm-time.py TIME.log [--all]

The shim (tools/panvk-ios/sweet-drm/sweet.drm.c, fix30) writes a window every 10 s: for each SweetMali request the
number of calls, the total time and the longest call, once for CoreAnimation's render thread ("render") and once for
every other thread("others"), then the ten heaviest stacks of the waits/SUBMITs of a millisecond or more on the
render thread. /var/tmp is not cleared between boots, so the file holds more than one boot: only the last one is
read unless --all is given.

The stacks come out as image+offset: name them on the PC with the bundle's dylibs, e.g.
  llvm-nm -n --defined-only libOSMesa.8.dylib | awk '$1 <= "0x1234"' | tail -1
The column that matters is ms/s: the milliseconds of one second of wall time the request took on that thread.
"""
import re
import sys

NAMES = {
    0x40: 'SUBMIT', 0x41: 'WAIT_BO', 0x4e: 'IMPORT_HOST', 0x4f: 'MODE',
    0xbf: 'SYNCOBJ_CREATE', 0xc0: 'SYNCOBJ_DESTROY', 0xc3: 'SYNCOBJ_WAIT',
    0xc4: 'SYNCOBJ_RESET', 0xc5: 'SYNCOBJ_SIGNAL', 0xcc: 'SYNCOBJ_TRANSFER',
}
WINDOW = re.compile(r'^=== kernel calls in (\d+) ms \(pid (\d+)\)$')
CALL = re.compile(r'^(render|others) (0x[0-9a-f]+): (\d+) calls, (\d+) us, longest (\d+) us$')
STACK = re.compile(r'^wait stack: (\d+) waits, (\d+) us, longest (\d+) us:(.*)$')


def name(number):
    return NAMES.get(number, 'request %#x' % number)


def parse(path):
    """-> list of boots, each a list of windows: {'ms', 'pid', 'calls', 'stacks'}"""
    boots, boot, window = [], [], None
    for line in open(path, 'r', errors='replace').read().splitlines():
        m = WINDOW.match(line.strip())
        if m:
            window = {'ms': int(m.group(1)), 'pid': int(m.group(2)), 'calls': [], 'stacks': []}
            if boot and boot[-1]['pid'] != window['pid']:
                boots.append(boot)
                boot = []
            boot.append(window)
            continue
        if window is None:
            continue
        m = CALL.match(line.strip())
        if m:
            window['calls'].append({'render': m.group(1) == 'render', 'nr': int(m.group(2), 16),
                                    'n': int(m.group(3)), 'us': int(m.group(4)), 'max': int(m.group(5))})
            continue
        m = STACK.match(line.strip())
        if m:
            window['stacks'].append({'n': int(m.group(1)), 'us': int(m.group(2)),
                                     'max': int(m.group(3)), 'frames': m.group(4).strip()})
    if boot:
        boots.append(boot)
    return boots


def report(windows):
    ms = sum(w['ms'] for w in windows)
    print('%d window%s, %d ms of wall time (pid %d)' %
          (len(windows), '' if len(windows) == 1 else 's', ms, windows[-1]['pid']))
    if not ms:
        return
    totals = {}
    for w in windows:
        for c in w['calls']:
            t = totals.setdefault((c['render'], c['nr']), {'n': 0, 'us': 0, 'max': 0})
            t['n'] += c['n']
            t['us'] += c['us']
            t['max'] = max(t['max'], c['max'])
    for render in (True, False):
        rows = [(nr, t) for (r, nr), t in totals.items() if r == render and t['n']]
        print('\n%s: %.1f ms/s of the wall time in all' %
              ('render thread' if render else 'other threads',
               sum(t['us'] for _, t in rows) / ms))
        print('  %-18s %10s %9s %10s %9s %9s' % ('request', 'calls', 'calls/s', 'us/call', 'ms/s', 'longest'))
        for nr, t in sorted(rows, key=lambda r: -r[1]['us']):
            print('  %-18s %10d %9.1f %10.1f %9.1f %9d' %
                  (name(nr), t['n'], t['n'] * 1000 / ms, t['us'] / t['n'], t['us'] / ms, t['max']))
    stacks = {}
    for w in windows:
        for s in w['stacks']:
            t = stacks.setdefault(s['frames'], {'n': 0, 'us': 0, 'max': 0})
            t['n'] += s['n']
            t['us'] += s['us']
            t['max'] = max(t['max'], s['max'])
    if stacks:
        print('\nwaits and SUBMITs of 1 ms or more on the render thread, heaviest first:')
    for frames, t in sorted(stacks.items(), key=lambda kv: -kv[1]['us'])[:12]:
        print('  %6d waits %8.1f ms/s longest %6d us: %s' % (t['n'], t['us'] / ms, t['max'], frames or '(no stack)'))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip().splitlines()[0] + '\nusage: sweetdrm-time.py TIME.log [--all]')
    boots = parse(sys.argv[1])
    if not boots:
        sys.exit('no window in it (not a sweetdrm-time.log, or the shim never ran)')
    print('%d boot%s in the file\n' % (len(boots), '' if len(boots) == 1 else 's'))
    for i, boot in enumerate(boots[-1:] if '--all' not in sys.argv else boots):
        if '--all' in sys.argv:
            print('boot %d of %d:' % (i + 1, len(boots)))
        report(boot)
        print()


if __name__ == '__main__':
    main()
