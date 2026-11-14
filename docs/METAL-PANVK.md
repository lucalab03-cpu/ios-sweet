# Metal on the Mali-G52: metal2vulkan + PanVK (plan, 2026-11-14)

Goal: real Metal for every process, including backboardd, SpringBoard and apps, with shaders compiled on the
phone. Without that, iOS cannot be a daily OS on the lamu.

## The stack

```
Metal.framework  ->  SweetMTLDriver.bundle (frontend, ObjC; MIT, NativeMetal/UniXNU via ios-flame)
                 ->  native Metal bridge (Rust staticlib, MIT) over metal2vulkan-runtime (AIR -> SPIR-V,
                     the whole Metal object model on Vulkan; GPL-2.0+ OR LGPL-3.0+)
                 ->  PanVK (Mesa's Vulkan driver for Mali, libvulkan_panfrost.dylib inside the bundle)
                 ->  tools/mesa-ios/sweet-drm (libdrm on IOKit, already used by gltest/shadertest/drawtest)
                 ->  SweetMali kernel service (patches 0121-0125, panfrost DRM ioctls as selectors)
```

The bundle keeps the kernel side of patch 0126: SweetAccelerator names it with MetalPluginName and
MetalPluginClassName. Today the bundle is a tracing stub that the metaltest process uses.

## Why this stack

- **ios-flame proved it on iOS 16** (Pixel 4, Adreno 640, Turnip). The lock screen, home screen, Settings,
  Control Center, Spotlight, Clock, Calendar, Notes, Photos and Safari render through it. Swipes run at
  20-26 fps, and about 270 CA pipelines compile on the phone, 3-30 ms each, cached in
  /var/local/m2v-cache/<process>. See flame's HANDOFF.md ("THE HOME SCREEN RENDERS THROUGH metal2vulkan +
  TURNIP") and METAL-NEXT.md. Their frontend already has the iOS 16 changes:
  `newPipelineLibraryWithFilePath:` returns nil, MTLFailureTypeSetErrorModeType, no framebuffer
  compression or deep framebuffer, and a nil device when the GPU cannot be opened.
- **The bridge only sees "a Vulkan loader".** NativeMetalBridge.rs opens a dylib that exports
  vkGetInstanceProcAddr, and metal2vulkan links nothing at build time (ash, loaded at run time). Nothing
  in the frontend or bridge is specific to Adreno. vendorName comes from the Vulkan device.
- **metal2vulkan's hard requirement is small:** "a Vulkan 1.2 device and `timelineSemaphore`" (runtime
  README). VK_EXT_external_memory_host and a few others are optional.
- **Mesa's docs list Bifrost v7 (G31/G51/G52/G76) at Vulkan 1.3 in PanVK.** This is driver-wide; the real
  features on our G52 r1 must be checked with vulkaninfo (Phase 1).
- **Our advantage over flame:** Turnip needed a new kernel backend in Mesa (tu_knl_darwin.cc, 1437
  lines) and a new kernel ABI. PanVK reaches the kernel through pan_kmod -> panfrost_kmod.c -> libdrm,
  and our sweet-drm shim already speaks panfrost's ioctls on iOS. Gallium Panfrost on the phone already
  draws (gltest triangle, 43/43 CA shaders in shadertest, drawtest). So the kernel and libdrm path is
  already proven.

## What we must write or change

| Piece | Work | Size |
|---|---|---|
| Mesa build | mesa-ios.yml gets a second target: `-Dvulkan-drivers=panfrost`, no WSI (`-Dplatforms=`), libvulkan_panfrost.dylib. Use a recent Mesa (26.x: PanVK changes quickly; 25.0.7 is old for Vulkan), so patch-mesa.py's anchors must be checked against that version. | medium |
| Loader export | iOS has no Vulkan loader: export `vkGetInstanceProcAddr` from the PanVK dylib (a one-line wrapper around vk_icdGetInstanceProcAddr, as flame's Turnip patch does). | small |
| Darwin fixes in Mesa | The same fixes flame needed: CTR_EL0 reads (SIGILL at EL0 on Darwin), `pthread_condattr_setclock`, weak imports of symbols that are missing on iOS 16 (check with `nm -m \| grep "weak external"`). | small |
| Real syncobjs | sweet-drm's syncobjs are fake ("everything is always signalled"). That works only because SweetMali's SUBMIT is synchronous. PanVK's queue submit and timeline semaphores (emulated on binary syncobjs by vk_sync) work with that, but slowly. Phase 4 makes them real: an asynchronous SUBMIT plus a kernel fence per syncobj, with WAIT in the service. | medium |
| IOSurface import | CA renders into IOSurfaces. flame used VK_EXT_external_memory_host (Turnip + kernel BoImport: pin the caller's pages). PanVK probably lacks it. Two options: (a) add host-pointer import to PanVK + a SweetMali import selector, or (b) a frontend path that maps IOSurface memory through an existing BO. (a) matches the bridge code, so choose it. | medium |
| Caches | SweetMali's CPU mappings are cacheable and the service does cache maintenance around each job. That is correct but slow for Vulkan's "map once, write often". Later: write-combined mappings (flame found the pmap ppnum_t shift bug behind the WC panic; check our pmap.c the same way). | later |
| Frontend | Copy flame's phone/metal2 (SweetMetal2.mm, NativeBridgeMetalFrontend.mm, bridge/ + patches) into tools/metal, rename it to our bundle and class (SweetMTLDriver / SweetMTLDevice), and replace the Turnip dylib with PanVK. Our allow list (/var/sweet/metal.allow) becomes flame's client list: backboardd last, after the smoke tests. | medium |
| Bridge build | Rust 1.98 + `aarch64-apple-ios` on the macos-15 runner, metal2vulkan pin 720ef1ce + flame's patches (0001-0006, 0100, 0101). New workflow metal.yml, like flame's tools/build-m2v-bridge.sh. | medium |

## Phases (each ends with one boot and the usual outbox + log-area)

Phase 1 code: `tools/panvk-ios` (patch-panvk.py for Mesa 26.2.7, its own fork of sweet-drm with in-process
syncobjs, device enumeration and mmap routing, vktest) and `.github/workflows/panvk-ios.yml` (runs on every push
to those paths). The Gallium build (mesa-ios.yml, Mesa 25.0.7) stays as it is: Mesa 25.1 dropped OSMesa.

1. **Vulkan up (no Metal).** Build PanVK for iOS and a `vktest` (flame's phone/turnip/vktest.c, adapted).
   It should print the device, API version, extensions and features (our vulkaninfo), then clear an image
   and read it back. Run it from sweet-boot.list.
   Pass: Vulkan >= 1.2, timelineSemaphore = 1, and the readback is correct.
   If PanVK on v7 needs `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, set it; it says nothing about our port.
2. **Bridge self-test.** Run m2vtest (`nmb_selftest`: device, AIR fixture library, compute, render,
   host-page import, QuartzCore metallib pairs) against PanVK.
   Pass: stage 0x650f, and counts of how many CA kernels and pairs compile. The refusal list from here is
   the PanVK/translator work list.
3. **Metal in metaltest only.** SweetMTLDriver becomes the real frontend. metaltest creates a device, a
   queue, a buffer, a compute kernel and a render pass into a texture, and checks the bytes.
4. **backboardd.** Add backboardd to metal.allow. CA composites the home screen on the GPU. Then measure
   cold compile time, keep the pipeline cache across boots, and add real syncobjs / async submit.
5. **Everyone.** SpringBoard, widgets (RenderBox: MRT, depth/stencil), apps, then WebKit's GPU process.

## Open questions (answered in order by the phases)

- Does PanVK expose Vulkan 1.2 + timelineSemaphore on our G52 r1 with our kernel's GPU properties
  (PANFROST_PARAM_* from patch 0123)? (Phase 1)
- Does SweetMali's single GPU address space hold up with several processes, each with its own PanVK
  device? flame needed per-connection VA windows because Turnip chooses its own IOVAs. PanVK on panfrost
  lets the kernel choose (PANFROST_CREATE_BO returns the offset), so ours should be fine. (Phase 4)
- Cold compile time on the A75/A55 cores and backboardd's memory with Rust + PanVK loaded (jetsam). A
  persistent cache is mandatory. (Phase 4)
- malitest's "submit 2" failure (0xe00002ca): does it matter for PanVK's job chains? Phase 1 shows it.

## Licences

The frontend and bridge are MIT ("NativeMetal contributors"). metal2vulkan is GPL-2.0-or-later OR
LGPL-3.0-or-later, and it is linked statically into the bundle: ship its source (pin + patches) with the
build. Mesa is MIT.
