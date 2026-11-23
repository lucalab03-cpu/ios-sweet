# Open Mapple's Metal driver on SweetMali (2026-11-23)

[Open Mapple](https://github.com/open-mapple)'s `mapple_gpu_mali` is a Metal driver on Mesa's **Gallium Panfrost** (the
same driver that already draws on our Mali-G52: gltest, shadertest 43/43, drawtest), with their own AIR -> NIR
translator. Their build (`mapple-gpu.yml`, 2026-11-23) contains Panfrost's Bifrost v7 back end and "Mali-G52 r1" in its
GPU table; their direct AIR -> Valhall compiler is opt-in (`VMG_ISA=1`) and for the G57 only, so it stays off.

Their bundle talks to their kernel's `MappleGPU` service. Instead of writing that service, this directory gives their
driver our kernel, **with no kernel change**:

| File | What it does |
|---|---|
| `sweet-mapple-drm.c` | Replaces their `libdrm-xnu` + `vgpu`: the same entry points (drmIoctl, drmMap/drmUnmap, syncobjs and their XNU extras, IOSurface and user-memory imports, vgpu_open...) on SweetMali, as `tools/panvk-ios/sweet-drm` does for PanVK. Imports go through IMPORT_HOST (0128); a plane not on a page is refused and their driver copies instead. Surface promises answer ENOTSUP (their driver then waits). Log `/var/tmp/sweetmdrm.log`. |
| `SweetMapple.m` | `SweetMTLDevice : VMDevice`, the class patch 0126 names. Gives a device only to the processes in `/var/sweet/metal.allow`, keeps CA off the deep framebuffer, logs validation failures. Log `/var/tmp/sweetmapple.log`; their driver's messages in `/var/tmp/sweetvmg.<process>.<pid>.log`. |
| `Info.plist` | `CFBundleExecutable = SweetMapple`: Metal loads Mapple's binary from `SweetMTLDriver.bundle`. metal2's binary (`SweetMTLDriver`) stays in the bundle; `tools/metal/Info.plist` selects it again. |
| `root.cmd`, `sweet-boot.list`, `metal.allow` | The test boot: sweetroot runs `vmprobe` (Mapple's own probe) and logs its output; then `mtltest` (our Metal test, the same one metal2 passed) runs on Mapple. Only vmprobe, mtltest, metaltest get a device: backboardd draws with the CPU in this boot. |
| `back/` | The drop back to metal2: its Info.plist, its allow list (backboardd, PosterBoard), and a root.cmd that does nothing. |

Workflow: `.github/workflows/mapple-sweet.yml` (manual). Mesa (1-2 h) is cached between runs; their libdrm is replaced
only after Mesa, which uses it for the macOS host build. Artifacts `mapple-sweet` (drop), `mapple-sweet-back`,
`mapple-sweet-logs`.

Licence: their driver is GPL-3.0-or-later with an attribution term (viva-gpu by Nicola Leone Ciardi); the drop carries
their LICENSE and NOTICE to `/usr/local/share/sweet/mapple/`.

## What to read after the boot (outbox)

- `sweetroot.log`: vmprobe's lines: `bundle loaded, VMDevice found`, `libLLVM: ok`, `VMDevice: <name>`,
  `MTLCreateSystemDefaultDevice: <name>`. A name like "Mali-G52 ... (Panfrost)" means their driver runs on SweetMali.
- `sweetmapple.log`, `sweetvmg.*.log`: why a device was not made (screen creation, shader translation).
- `sweetmdrm.log`: our libdrm: SweetMali opened, CREATE_BO, IMPORT_HOST, failed requests.
- `sweetmtltest.log`: mtltest's steps and its `RESULT` line, to compare with metal2's.

## Next, if mtltest passes

Add `backboardd` and `backboardd.orig` (and `PosterBoard`) to `/var/sweet/metal.allow`: a dropbox of one file.
