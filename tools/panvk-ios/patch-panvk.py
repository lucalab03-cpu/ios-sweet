#!/usr/bin/env python3
"""Patches an unpacked Mesa 26.2.7 so PanVK (Mesa's Vulkan driver for Mali) runs on iOS over the SweetMali service.
usage: patch-panvk.py MESA_DIR SHIM_DIR      (SHIM_DIR = tools/panvk-ios/sweet-drm)

Everything is under the `sweet-ios` option (-DSWEET_IOS), so the same tree also builds the native tools
(mesa_clc, vtn_bindgen2, panfrost_compile) unchanged. Every anchor is asserted: a Mesa that differs stops here."""
import os, re, shutil, sys

mesa, shim = sys.argv[1], sys.argv[2]

def rd(p):
    with open(os.path.join(mesa, p)) as f:
        return f.read()

def wr(p, s):
    with open(os.path.join(mesa, p), 'w') as f:
        f.write(s)

def sub_once(path, old, new):
    s = rd(path)
    if s.count(old) != 1:
        sys.exit("patch-panvk: %s: anchor found %d times, expected 1:\n%s" % (path, s.count(old), old))
    wr(path, s.replace(old, new))

# 1. The shim goes in the tree as src/sweet-drm; `dep_libdrm` becomes it. Its sys/ holds the Linux headers
#    panfrost_kmod.c and PanVK include (eventfd, timerfd, sysmacros), as stubs.
dst = os.path.join(mesa, 'src/sweet-drm')
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(shim, dst)
with open(os.path.join(dst, 'meson.build'), 'w') as f:
    f.write("""libsweetdrm = static_library(
  'sweetdrm', 'sweet.drm.c',
  include_directories : [inc_include],
  c_args : ['-Wno-deprecated-declarations'],
  gnu_symbol_visibility : 'hidden',
)
dep_libdrm = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libsweetdrm,
  link_args : ['-framework', 'IOKit'],
)
""")

mo = rd('meson.options')
wr('meson.options', "option('sweet-ios', type : 'boolean', value : false,\n"
   "  description : 'PanVK on iOS through the SweetMali service')\n" + mo)

# The top level has no libdrm on Darwin (dep_libdrm = null_dep). The shim's dependency replaces it at the start
# of src/meson.build, before util, the Vulkan runtime, WSI and panfrost look at it. HAVE_LIBDRM=1 makes
# util/libdrm.h, u_sync_provider.h and WSI take the real libdrm paths, which the shim provides.
sub_once('meson.build',
"""  # We should prevent libdrm from being available when the target doesn't have it to avoid transitive
  # dependencies (such as vk-runtime) linking to it
  dep_libdrm = null_dep
endif
""",
"""  # We should prevent libdrm from being available when the target doesn't have it to avoid transitive
  # dependencies (such as vk-runtime) linking to it
  dep_libdrm = null_dep
endif
sweet_ios = host_machine.system() == 'darwin' and get_option('sweet-ios')
if sweet_ios
  add_project_arguments('-DSWEET_IOS', '-DHAVE_LIBDRM=1', language : ['c', 'cpp'])
endif
""")

s = rd('src/meson.build')
m = re.search(r"^subdir\('util'\)\n", s, re.M)
assert m, "src/meson.build: no subdir('util')"
s = s[:m.start()] + "if sweet_ios\n  subdir('sweet-drm')\nendif\n" + s[m.start():]
wr('src/meson.build', s)

# 2. Mappings of the device become the service's client memory (the shim decides per call; everything that is
#    not the device stays plain mmap/munmap).
sub_once('src/util/os_mman.h', "#if DETECT_OS_ANDROID && !defined(__LP64__)\n",
"""#if defined(SWEET_IOS)
void *sweet_os_mmap(void *address, size_t size, int prot, int flags, int fd, long long offset);
int sweet_os_munmap(void *address, size_t size);
#  define os_mmap(addr, length, prot, flags, fd, offset) \\
             sweet_os_mmap(addr, length, prot, flags, fd, offset)
#  define os_munmap(addr, length) \\
             sweet_os_munmap(addr, length)

#elif DETECT_OS_ANDROID && !defined(__LP64__)
""")

# 3. CTR_EL0 cannot be read at EL0 on Darwin (SIGILL; ios-flame hit it with Turnip). The lamu's Cortex-A75/A55
#    have 64-byte lines: DminLine = IminLine = 4, CWG = ERG = 4.
sub_once('src/util/cache_ops_aarch64.c',
r'''   __asm("mrs\t%x0, ctr_el0" : "=r"(ctr_el0));
''',
r'''#ifdef SWEET_IOS
   ctr_el0 = 0x84448004;
#else
   __asm("mrs\t%x0, ctr_el0" : "=r"(ctr_el0));
#endif
''')

# 3b. Darwin has no sched_setscheduler (the perf-counter thread, never started here): POSIX's per-thread call.
sub_once('src/panfrost/lib/kmod/panfrost_kmod.c',
"   int ret = sched_setscheduler(0, SCHED_FIFO, &rt_params);\n",
"""#ifdef SWEET_IOS
   int ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt_params);
#else
   int ret = sched_setscheduler(0, SCHED_FIFO, &rt_params);
#endif
""")

# 3c. vk_image has drm_format_mod (VK_EXT_image_drm_format_modifier) only on Linux/BSD; PanVK uses it everywhere.
for path, want in (('src/vulkan/runtime/vk_image.h', 1), ('src/vulkan/runtime/vk_image.c', 3)):
    s = rd(path)
    old = "#if DETECT_OS_LINUX || DETECT_OS_BSD\n"
    if s.count(old) != want:
        sys.exit("patch-panvk: %s: %d Linux/BSD gates, expected %d" % (path, s.count(old), want))
    wr(path, s.replace(old, "#if DETECT_OS_LINUX || DETECT_OS_BSD || defined(SWEET_IOS)\n"))

# 3d. The generated entrypoint tables refer to every possible panvk_* function as a weak symbol. ELF resolves the
#     missing ones to NULL at link time; Apple's ld refuses them unless they are looked up at load time, which is
#     what Mesa's own macOS driver does (src/kosmickrisp/vulkan/meson.build). panvk-ios.yml lists every symbol
#     left to load time that is not an entrypoint.
sub_once('src/panfrost/vulkan/meson.build',
"  link_args : [vulkan_icd_link_args, ld_args_bsymbolic, ld_args_gc_sections, ld_args_build_id],\n",
"  link_args : [vulkan_icd_link_args, ld_args_bsymbolic, ld_args_gc_sections, ld_args_build_id,\n"
"               sweet_ios ? ['-Wl,-undefined,dynamic_lookup'] : []],\n")

# 3e. On Apple the build-id is the Mach-O LC_UUID, 16 bytes; PanVK wants a 20-byte SHA-1 and refused to make an
#     instance (vktest, boot 2026-11-14: vkCreateInstance -> INITIALIZATION_FAILED). Mesa's macOS driver takes
#     VK_UUID_SIZE (kk_instance.c). PanVK uses at most VK_UUID_SIZE of it afterwards (driverUUID,
#     optimalTilingLayoutUUID; the cache key hashes the zero-padded buffer).
sub_once('src/panfrost/vulkan/panvk_instance.c',
"   if (build_id_len < BUILD_ID_EXPECTED_HASH_LENGTH) {\n",
"#ifdef SWEET_IOS\n   if (build_id_len < VK_UUID_SIZE) {\n#else\n   if (build_id_len < BUILD_ID_EXPECTED_HASH_LENGTH) {\n#endif\n")

# 3f. VK_EXT_external_memory_host (phase 4: the Metal runtime imports IOSurfaces and no-copy buffers as host memory;
#     m2vtest's import step failed without it, boot 2026-11-14). The kernel wires the caller's pages (SweetMali
#     IMPORT_HOST, patch 0128); the shim hands the buffer to PanVK's own dma-buf import as a descriptor that stands
#     for it (sweet_drm_import_host), so the rest of PanVK is unchanged. Mapping such memory gives back the caller's
#     pointer, and unmapping or freeing it never unmaps the caller's pages. 16 KB alignment: iOS's page.
sub_once('src/panfrost/lib/kmod/pan_kmod.c',
"      size_t size = lseek(fd, 0, SEEK_END);\n",
"""#ifdef SWEET_IOS
      size_t size = (size_t)sweet_drm_fd_size(fd);
      if (size == 0)
         size = lseek(fd, 0, SEEK_END);
#else
      size_t size = lseek(fd, 0, SEEK_END);
#endif
""")

s = rd('src/panfrost/vulkan/panvk_device_memory.c')
i = s.index('#include')
s = s[:i] + ("#ifdef SWEET_IOS\n#include <stdint.h>\nint sweet_drm_import_host(void *address, uint64_t size);\nvoid sweet_drm_forget_fd(int fd);\n#endif\n") + s[i:]
wr('src/panfrost/vulkan/panvk_device_memory.c', s)
sub_once('src/panfrost/vulkan/panvk_device_memory.c',
"   if (fd_info) {\n      assert(\n",
"""#ifdef SWEET_IOS
   if (mem->vk.host_ptr) {
      int host_fd = sweet_drm_import_host(mem->vk.host_ptr, pAllocateInfo->allocationSize);
      if (host_fd < 0) {
         result = panvk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE, "host pointer %p (%" PRIu64 " bytes) not imported",
                               mem->vk.host_ptr, (uint64_t)pAllocateInfo->allocationSize);
         goto err_destroy_mem;
      }
      mem->bo = pan_kmod_bo_import(device->kmod.dev, host_fd);
      sweet_drm_forget_fd(host_fd);
      if (!mem->bo) {
         result = panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }
   } else
#endif
   if (fd_info) {
      assert(
""")
sub_once('src/panfrost/vulkan/panvk_device_memory.c',
"   void *host_addr = NULL;\n   int map_flags = MAP_SHARED;\n",
"""#ifdef SWEET_IOS
   /* imported host memory: it is the caller's own mapping */
   if (mem->vk.host_ptr) {
      mem->addr.host = mem->vk.host_ptr;
      *ppData = (char *)mem->vk.host_ptr + offset;
      return VK_SUCCESS;
   }
#endif
   void *host_addr = NULL;
   int map_flags = MAP_SHARED;
""")
sub_once('src/panfrost/vulkan/panvk_device_memory.c',
"   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {\n",
"""#ifdef SWEET_IOS
   if (mem->vk.host_ptr) {
      mem->addr.host = NULL;
      return VK_SUCCESS;
   }
#endif
   if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
""")
sub_once('src/panfrost/vulkan/panvk_device_memory.c',
"   if (mem->addr.host) {\n      ASSERTED int ret = os_munmap(mem->addr.host, pan_kmod_bo_size(mem->bo));\n      assert(!ret);\n   }\n",
"""#ifdef SWEET_IOS
   if (mem->vk.host_ptr)
      mem->addr.host = NULL; /* the caller's pages: never unmapped here */
#endif
   if (mem->addr.host) {
      ASSERTED int ret = os_munmap(mem->addr.host, pan_kmod_bo_size(mem->bo));
      assert(!ret);
   }
""")
s = rd('src/panfrost/vulkan/panvk_device_memory.c')
s += """
#ifdef SWEET_IOS
/* VK_EXT_external_memory_host: any memory type can take host pages (all of ours are host-visible). */
VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryHostPointerPropertiesEXT(VkDevice _device, VkExternalMemoryHandleTypeFlagBits handleType,
                                        const void *pHostPointer,
                                        VkMemoryHostPointerPropertiesEXT *pMemoryHostPointerProperties)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device = to_panvk_physical_device(device->vk.physical);

   if (handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT ||
       ((uintptr_t)pHostPointer & (16384 - 1)) != 0)
      return panvk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   uint32_t bits = 0;
   for (uint32_t i = 0; i < physical_device->memory.type_count; i++) {
      if (physical_device->memory.types[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         bits |= 1u << i;
   }
   pMemoryHostPointerProperties->memoryTypeBits = bits;
   return VK_SUCCESS;
}
#endif
"""
wr('src/panfrost/vulkan/panvk_device_memory.c', s)

sub_once('src/panfrost/vulkan/panvk_vX_physical_device.c',
"      .EXT_external_memory_dma_buf = true,\n",
"      .EXT_external_memory_dma_buf = true,\n#ifdef SWEET_IOS\n      .EXT_external_memory_host = true,\n#endif\n")
sub_once('src/panfrost/vulkan/panvk_vX_physical_device.c',
"      .minMemoryMapAlignment = os_page_size,\n",
"      .minMemoryMapAlignment = os_page_size,\n#ifdef SWEET_IOS\n      .minImportedHostPointerAlignment = 16384,\n#endif\n")

sub_once('src/panfrost/vulkan/panvk_physical_device.c',
"""   const VkExternalMemoryHandleTypeFlags supported_handle_types =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

   /* From the Vulkan 1.3.298 spec:
""",
"""#ifdef SWEET_IOS
   if (pExternalBufferInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) {
      pExternalBufferProperties->externalMemoryProperties = (VkExternalMemoryProperties){
         .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
         .exportFromImportedHandleTypes = 0,
         .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      };
      return;
   }
#endif
   const VkExternalMemoryHandleTypeFlags supported_handle_types =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

   /* From the Vulkan 1.3.298 spec:
""")
sub_once('src/panfrost/vulkan/panvk_physical_device.c',
"""   VkExternalMemoryProperties *external_properties)
{
   const VkExternalMemoryHandleTypeFlags supported_handle_types =
""",
"""   VkExternalMemoryProperties *external_properties)
{
#ifdef SWEET_IOS
   /* linear 2D images over imported host pages (the runtime's textures over IOSurfaces) */
   if (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) {
      if (pImageFormatInfo->type != VK_IMAGE_TYPE_2D || pImageFormatInfo->tiling != VK_IMAGE_TILING_LINEAR)
         return panvk_errorf(physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                             "host memory: linear 2D images only");
      *external_properties = (VkExternalMemoryProperties){
         .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
         .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      };
      return VK_SUCCESS;
   }
#endif
   const VkExternalMemoryHandleTypeFlags supported_handle_types =
""")

# 3g. Framebuffer fetch (rasterization-order attachment access) on Bifrost v7 too: metal2vulkan needs it for
#     QuartzCore's path_blit (m2vtest, 2026-11-14). PanVK enables it only for v10+, whose CSF path emits its own
#     ordering; on v7's job manager the tile buffer is read in primitive order per pixel by the hardware.
#     SWEET_NO_FBFETCH=1 in the environment turns it off again. mtltest checks a framebuffer-fetch draw.
for old in ("      .EXT_rasterization_order_attachment_access = PAN_ARCH >= 10,\n",
            "      .ARM_rasterization_order_attachment_access = PAN_ARCH >= 10,\n"):
    name = old.strip().split(' ')[0]
    sub_once('src/panfrost/vulkan/panvk_vX_physical_device.c', old,
             "#ifdef SWEET_IOS\n      %s = PAN_ARCH >= 10 || (PAN_ARCH >= 7 && !getenv(\"SWEET_NO_FBFETCH\")),\n#else\n%s#endif\n" % (name, old))
s = rd('src/panfrost/vulkan/panvk_vX_physical_device.c')
old = "      .rasterizationOrderColorAttachmentAccess = PAN_ARCH >= 10,\n"
if s.count(old) != 1:
    sys.exit("patch-panvk: rasterizationOrderColorAttachmentAccess anchor")
wr('src/panfrost/vulkan/panvk_vX_physical_device.c', s.replace(old,
   "#ifdef SWEET_IOS\n      .rasterizationOrderColorAttachmentAccess = PAN_ARCH >= 10 || (PAN_ARCH >= 7 && !getenv(\"SWEET_NO_FBFETCH\")),\n#else\n" + old + "#endif\n"))

# 3h. Where PanVK's job batches come from (2026-11-25: 2 chains for a one-pass commit, 24-63 for those of 4-11
#     passes, about half a chain more per draw): every closed batch is counted by its caller and the caller's
#     caller, with whether it has a fragment job and vertex/tiler jobs (sweet.drm.c: sweet_drm_note_batch; the
#     Metal driver prints the busiest sites in its telemetry's Y rows, as offsets in this image).
sub_once('src/panfrost/vulkan/jm/panvk_vX_cmd_buffer.c',
"""   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (!batch)
      return;

   assert(batch);
""",
"""   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (!batch)
      return;

#ifdef SWEET_IOS
   {
      extern void sweet_drm_note_batch(void *a0, void *a1, int frag, int vtc);
      sweet_drm_note_batch(__builtin_return_address(0), __builtin_return_address(1),
                           batch->fb.desc.gpu != 0, batch->vtc_jc.first_job != 0);
   }
#endif
   assert(batch);
""")

# 4. iOS has no Vulkan loader: the driver itself exports vkGetInstanceProcAddr (the metal2vulkan bridge opens
#    the driver as its loader, as ios-flame does with Turnip).
s = rd('src/panfrost/vulkan/panvk_instance.c')
assert 'vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)' in s, "panvk_instance.c: no ICD entry point"
s += """
#ifdef SWEET_IOS
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return panvk_GetInstanceProcAddr(instance, pName);
}
#endif
"""
wr('src/panfrost/vulkan/panvk_instance.c', s)

print("patch-panvk: ok")
