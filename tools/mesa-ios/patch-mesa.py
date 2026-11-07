#!/usr/bin/env python3
"""Patches an unpacked Mesa 25.0.7 so panfrost can drive the Mali through the SweetMali kernel service on iOS.
usage: patch-mesa.py MESA_DIR SHIM_DIR   (every anchor is asserted: a Mesa that differs stops here with a clear message)"""
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
        sys.exit("patch-mesa: %s: anchor found %d times, expected 1:\n%s" % (path, s.count(old), old))
    wr(path, s.replace(old, new))

# 1. the shim goes in the tree, and `dep_libdrm` becomes it
dst = os.path.join(mesa, 'src/sweet-drm')
shutil.rmtree(dst, ignore_errors=True)
os.makedirs(dst)
for n in ('xf86drm.h', 'sweet.drm.c'):
    shutil.copy(os.path.join(shim, 'sweet-drm', n), dst)
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

# the dependency is made where Mesa makes it, but `inc_include` must exist by then: it is made later,
# so the shim's meson.build is entered from src/meson.build's start instead, and meson.build only
# gets a placeholder here.
sub_once('meson.build',
"""else
  # We should prevent libdrm from being available when the target doesn't have it to avoid transitive
  # dependencies (such as vk-runtime) linking to it
  dep_libdrm = null_dep
endif
""",
"""else
  # We should prevent libdrm from being available when the target doesn't have it to avoid transitive
  # dependencies (such as vk-runtime) linking to it
  dep_libdrm = null_dep
endif
sweet_ios = host_machine.system() == 'darwin' and get_option('sweet-ios')
""")
mo = rd('meson_options.txt')
wr('meson_options.txt', "option('sweet-ios', type : 'boolean', value : false,\n  description : 'panfrost on iOS through the SweetMali service')\n" + mo)

# 2. src/meson.build: the shim's library right after the include dirs exist
s = rd('src/meson.build')
m = re.search(r"^subdir\('util'\)\n", s, re.M)
assert m, "src/meson.build: no subdir('util')"
s = s[:m.start()] + "if sweet_ios\n  subdir('sweet-drm')\nendif\n" + s[m.start():]
wr('src/meson.build', s)

# 3. panfrost's own winsys directory is replaced by one function, in the osmesa target's directory
sub_once('src/gallium/meson.build', "  subdir('winsys/panfrost/drm')\n",
"""  if sweet_ios
    libpanfrostwinsys = static_library('panfrostwinsys_sweet', 'targets/osmesa/sweet_winsys.c',
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_drivers],
      dependencies : [dep_libdrm, idep_mesautil])
  else
    subdir('winsys/panfrost/drm')
  endif
""")

# 4. the kernel mapping: the buffer's memory comes from the service, not from mmap() on the device
sub_once('src/panfrost/lib/kmod/pan_kmod.h',
"""   host_addr = os_mmap(host_addr, size, prot, flags, bo->dev->fd,
                       mmap_offset + bo_offset);
""",
"""#ifdef SWEET_IOS
   host_addr = sweet_drm_mmap(size, mmap_offset + bo_offset);
#else
   host_addr = os_mmap(host_addr, size, prot, flags, bo->dev->fd,
                       mmap_offset + bo_offset);
#endif
""")
s = rd('src/panfrost/lib/kmod/pan_kmod.h')
i = s.index('#include')
s = s[:i] + "#ifdef SWEET_IOS\nvoid *sweet_drm_mmap(size_t size, uint64_t offset);\n#endif\n" + s[i:]
wr('src/panfrost/lib/kmod/pan_kmod.h', s)
sub_once('meson.build', "sweet_ios = host_machine.system() == 'darwin' and get_option('sweet-ios')\n",
"sweet_ios = host_machine.system() == 'darwin' and get_option('sweet-ios')\nif sweet_ios\n  add_project_arguments('-DSWEET_IOS', language : ['c', 'cpp'])\nendif\n")

# 4b. and its unmapping
sub_once('src/gallium/drivers/panfrost/pan_bo.c',
"   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n",
"#ifdef SWEET_IOS\n   if (sweet_drm_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n#else\n   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n#endif\n")

# 5. the osmesa target: panfrost's screen, software rendering as the fallback
shutil.copy(os.path.join(shim, "osmesa/sweet_winsys.c"), os.path.join(mesa, "src/gallium/targets/osmesa/sweet_winsys.c"))
t = rd('src/gallium/targets/osmesa/osmesa_target.c')
t = t.replace('#include "sw/null/null_sw_winsys.h"\n',
 '#include "sw/null/null_sw_winsys.h"\n#ifdef SWEET_IOS\n#include "sweet_screen.h"\n#endif\n', 1)
assert 'sweet_screen.h' in t
sub_once_t = "   struct sw_winsys *winsys;\n   struct pipe_screen *screen;\n"
assert t.count(sub_once_t) == 1
t = t.replace(sub_once_t, sub_once_t + "\n#ifdef SWEET_IOS\n   screen = sweet_panfrost_screen();\n   if (screen)\n      return debug_screen_wrap(screen);\n#endif\n")
wr('src/gallium/targets/osmesa/osmesa_target.c', t)
shutil.copy(os.path.join(shim, 'osmesa/sweet_screen.h'), os.path.join(mesa, 'src/gallium/targets/osmesa/sweet_screen.h'))
sub_once('src/gallium/targets/osmesa/meson.build', "driver_swrast, idep_mesautil,", "driver_swrast, driver_panfrost, dep_libdrm, idep_mesautil,")
print("patch-mesa: ok")
