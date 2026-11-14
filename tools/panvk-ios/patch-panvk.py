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
