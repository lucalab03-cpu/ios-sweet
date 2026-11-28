# Drop/payload scripts (from the session of 2026-11-27)

These are the scripts used to turn a `sweet-bundle-*` artifact (bundle.yml) into what the phone takes.
They assume the layout of that session (`/home/claude/in`, `/home/claude/out`); adjust the paths.

- `make-fix25.sh BUNDLE.zip`: checks SHA256SUMS, copies the bundle's `drop/` into `drop-fix25/`, adds
  `var/sweet/outbox.list`, `var/sweet/root.cmd` (`apply` + optional `run ...` lines) and `var/sweet/glargs`, zips it.
- `make-an.sh BUNDLE.zip`: new kernel into payloads with `tools/repayload.py OLD KERNEL OUT [--set NAME=VALUE] --expect TEXT`.
- `make-fix31.sh BUNDLE.zip PREVIOUS-PAYLOAD.bin [OUT]`: kernel 0173 only (0173 fixes no shim file): closed buffers
  handed out again, runs mapped whole, imports stored once per group (`--set sweetmalistore=1` for the old way).
- `make-fix32.sh BUNDLE.zip [OUT] [PREV-DROP]`: measurement drop only (kernel and shim unchanged): `glargs` adds
  `touch_lat=1` (when every frame began and ended) and `glslow=20000` (every GL call of 20 ms or more, by name).
- `make-fix19.sh`: example of a root.cmd with `sweetjob remove` and two `sweetstack prof` runs.
- `make-fix30.sh BUNDLE.zip [OUT] [PREV-DROP]`: 0172/`sweetdrm-time.log` checks, drop-fix30 (no census, no trace, the
  two render-thread profiles); works with or without `zip`/`strings` (falls back to python's zipfile and `grep -a`).
  Its log is read by `tools/sweetdrm-time.py`.

On the PC (user side): `rm -rf drop-X && unzip -o drop-X.zip -d drop-X && python3 pack-dropbox.py dropbox.map.json drop-X`,
then `adb shell "echo 0 > /proc/gpufreqv2/fix_target_opp_index"` and `adb shell "sh /data/local/tmp/go-ios.sh payload-X.bin"`.
Drop names must be new every time. Payloads (kernel + ADT, Apple-derived) are never committed.
