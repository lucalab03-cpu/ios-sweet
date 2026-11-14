# tools/metal2: Metal on metal2vulkan + PanVK (phase 2 of docs/METAL-PANVK.md)

- `bridge/NativeMetalBridge.rs`, `bridge/NativeMetalObjects.rs` and their headers: the native Metal bridge of
  NativeMetal (UniXNU), MIT (`bridge/LICENSE`), as ios-flame adapted it for iOS 16
  (https://github.com/NWylynko/ios-flame, `phone/metal2/bridge`, MIT). Our one change: in the self-test, compute,
  render and import failures are recorded instead of ending the run (PanVK has no `VK_EXT_external_memory_host`
  yet, which the import step needs).
- `bridge/patches/`: ios-flame's patches to metal2vulkan (0001-0006 from UniXNU, 0100-0101 ios-flame's). They
  fall under metal2vulkan's licence.
- metal2vulkan itself (https://github.com/steelbrain/metal2vulkan, GPL-2.0-or-later OR LGPL-3.0-or-later) is
  fetched by `.github/workflows/metal2.yml` at the pin 720ef1ce and linked statically. A shipped binary that
  contains it must come with its source (the pin and these patches).
- `m2vtest.c`: the bridge's self-test runner, after ios-flame's `phone/metal2/m2vtest.c` (MIT), using PanVK
  (`/usr/local/lib/libvulkan_panfrost.dylib`) and logging to `/var/tmp/sweetm2v.log`.
