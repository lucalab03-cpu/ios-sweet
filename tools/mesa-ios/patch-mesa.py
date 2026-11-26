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

# 1. the shim goes in the tree, and `dep_libdrm` becomes it. 2026-11-25: PanVK's shim (tools/panvk-ios/sweet-drm), the
#    one that is kept up to date: write-combined mappings (the old one mapped cached, and drawtest read zeros), queued
#    SUBMITs, the kernel's syncobjs, host-page imports (IOSurfaces for glEGLImageTargetTexture2DOES).
dst = os.path.join(mesa, 'src/sweet-drm')
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(os.path.join(shim, '..', 'panvk-ios', 'sweet-drm'), dst)
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
  link_args : ['-framework', 'IOKit', '-framework', 'IOSurface', '-framework', 'CoreFoundation'],
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
   host_addr = sweet_os_mmap(host_addr, size, prot, flags, bo->dev->fd, mmap_offset + bo_offset);
#else
   host_addr = os_mmap(host_addr, size, prot, flags, bo->dev->fd,
                       mmap_offset + bo_offset);
#endif
""")
s = rd('src/panfrost/lib/kmod/pan_kmod.h')
i = s.index('#include')
s = s[:i] + "#ifdef SWEET_IOS\n#include <stddef.h>\nvoid *sweet_os_mmap(void *address, size_t size, int prot, int flags, int fd, long long offset);\n#endif\n" + s[i:]
wr('src/panfrost/lib/kmod/pan_kmod.h', s)
sub_once('meson.build', "sweet_ios = host_machine.system() == 'darwin' and get_option('sweet-ios')\n",
"sweet_ios = host_machine.system() == 'darwin' and get_option('sweet-ios')\nif sweet_ios\n  add_project_arguments('-DSWEET_IOS', language : ['c', 'cpp'])\nendif\n")

# 4b. and its unmapping
sub_once('src/gallium/drivers/panfrost/pan_bo.c',
"   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n",
"#ifdef SWEET_IOS\n   if (sweet_os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n#else\n   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n#endif\n")

# 4e. (2026-11-26) pan_fb_preload.c: the padding ATTRIBUTE_BUFFER of the preload's varyings was written 0x100 bytes
#     past a 0x20-byte allocation -- pan_cast_and_pack() casts its first argument without parentheses, so
#     "cpu + pan_size(ATTRIBUTE_BUFFER)" became 16 descriptors on, not 16 bytes. It overwrote whatever the pool held
#     there and, at a buffer's end, faulted: backboardd SIGSEGV in pan_preload_fb_part (rounds aa, ab).
sub_once('src/gallium/drivers/panfrost/pan_fb_preload.c',
"      pan_cast_and_pack(varying_buffer.cpu + pan_size(ATTRIBUTE_BUFFER),\n",
"      pan_cast_and_pack(((uint8_t *)varying_buffer.cpu + pan_size(ATTRIBUTE_BUFFER)),\n")

# 4f. (2026-11-26) a cached buffer keeps its processor mapping. Mesa unmaps a buffer when its last reference goes, before
#     it enters the buffer cache, and maps it again when the cache hands it out: on Linux an mmap; here an
#     IOConnectMapMemory64 and an IOConnectUnmapMemory64 into the kernel each time (round ah: panfrost_bo_mmap and
#     sweet_os_mmap among backboardd's heaviest paths once 0157 let the cache hit). Unmapped now only when the buffer is
#     freed (panfrost_bo_free: cache eviction, or not cacheable).
sub_once('src/gallium/drivers/panfrost/pan_bo.c',
"""      /* When the reference count goes to zero, we need to cleanup */
      panfrost_bo_munmap(bo);
""",
"""      /* When the reference count goes to zero, we need to cleanup */
#ifndef SWEET_IOS
      panfrost_bo_munmap(bo);
#endif
""")
sub_once('src/gallium/drivers/panfrost/pan_bo.c',
"""   struct pan_kmod_bo *kmod_bo = bo->kmod_bo;
   struct pan_kmod_vm *vm = bo->dev->kmod.vm;
   uint64_t gpu_va = bo->ptr.gpu;
""",
"""#ifdef SWEET_IOS
   panfrost_bo_munmap(bo);      /* (kept while cached: 4f) */
#endif
   struct pan_kmod_bo *kmod_bo = bo->kmod_bo;
   struct pan_kmod_vm *vm = bo->dev->kmod.vm;
   uint64_t gpu_va = bo->ptr.gpu;
""")
s_bo = rd('src/gallium/drivers/panfrost/pan_bo.c')
i = s_bo.index('\npanfrost_bo_free(struct panfrost_bo *bo)')
j = s_bo.rindex('\n', 0, i)          # the line before: the return type
s_bo = s_bo[:j] + "\n#ifdef SWEET_IOS\nstatic void panfrost_bo_munmap(struct panfrost_bo *bo);\n#endif" + s_bo[j:]
wr('src/gallium/drivers/panfrost/pan_bo.c', s_bo)

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
# 4c. pan_bo.c's prototype of the unmapping
s = rd('src/gallium/drivers/panfrost/pan_bo.c')
i = s.index('#include')
s = s[:i] + "#ifdef SWEET_IOS\n#include <stddef.h>\nint sweet_os_munmap(void *address, size_t size);\n#endif\n" + s[i:]
wr('src/gallium/drivers/panfrost/pan_bo.c', s)

# 4d. an imported buffer's size: a host-page import has no file to seek to the end of (the shim knows its size)
sub_once('src/panfrost/lib/kmod/pan_kmod.c', "      size_t size = lseek(fd, 0, SEEK_END);\n",
"""#ifdef SWEET_IOS
      size_t size = (size_t)sweet_drm_fd_size(fd);
      if (size == 0)
         size = lseek(fd, 0, SEEK_END);
#else
      size_t size = lseek(fd, 0, SEEK_END);
#endif
""")
s = rd('src/panfrost/lib/kmod/pan_kmod.c')
i = s.index('#include')
s = s[:i] + "#ifdef SWEET_IOS\n#include <stdint.h>\nuint64_t sweet_drm_fd_size(int fd);\n#endif\n" + s[i:]
wr('src/panfrost/lib/kmod/pan_kmod.c', s)

# 6. OpenGL ES contexts from OSMesa (tools/sweetgl/osmesa-es-profile.patch, from HakiOS): CoreAnimation's OpenGL ES
#    renderer asks EAGL for API 3, and tools/sweetgl/gl_osmesa.c makes it an OSMesa context of the ES profile.
import subprocess
r = subprocess.run(['patch', '-p1', '-N', '-i', os.path.join(shim, '..', 'sweetgl', 'osmesa-es-profile.patch')],
                   cwd=mesa, capture_output=True, text=True)
if r.returncode != 0:
    sys.exit("patch-mesa: osmesa-es-profile.patch: " + r.stdout + r.stderr)

# 7. glEGLImageTargetTexture2DOES with an IOSurface's ID as the image (as HakiOS does it): the texture is the surface's
#    own memory, imported page by page (SweetMali IMPORT_HOST). osmesa_surface_resource() is in the target below.
sub_once('src/gallium/frontends/osmesa/osmesa.c', "extern struct pipe_screen *\nosmesa_create_screen(void);\n",
"""extern struct pipe_screen *
osmesa_create_screen(void);

extern struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface);

/* ios-sweet: there is no EGL here; what glEGLImageTargetTexture2DOES is given is an IOSurface's ID */
static bool
osmesa_st_get_egl_image(struct pipe_frontend_screen *fscreen, void *image, struct st_egl_image *out)
{
   out->texture = osmesa_surface_resource(fscreen->screen, (uint32_t)(uintptr_t)image);
   if (!out->texture)
      return false;
   out->format = out->texture->format;
   return true;
}

static bool
osmesa_st_validate_egl_image(struct pipe_frontend_screen *fscreen, void *image)
{
   (void)fscreen;
   return image != NULL;
}
""")
sub_once('src/gallium/frontends/osmesa/osmesa.c', "      global_fscreen->get_egl_image = NULL;\n",
"      global_fscreen->get_egl_image = osmesa_st_get_egl_image;\n      global_fscreen->validate_egl_image = osmesa_st_validate_egl_image;\n")

t = rd('src/gallium/targets/osmesa/osmesa_target.c')
anchor = "struct pipe_screen *\nosmesa_create_screen(void);\n"
assert t.count(anchor) == 1, "osmesa_target.c: osmesa_create_screen prototype"
t = t.replace(anchor, """#include "frontend/winsys_handle.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include <unistd.h>

#ifdef SWEET_IOS
#include <IOSurface/IOSurfaceRef.h>
int sweet_drm_import_host(void *address, uint64_t size);
void sweet_drm_forget_fd(int fd);
#endif

struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface);

/*
 * ios-sweet: an IOSurface of this process as a texture to draw with and into, its own memory (no copy): one plane of
 * 32-bit pixels, blue first, page-aligned; its pages imported into the GPU's address space (SweetMali IMPORT_HOST).
 * NULL: not such a surface, or the screen is the software one; eagl.c then copies.
 */
struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface)
{
#ifdef SWEET_IOS
   IOSurfaceRef s = IOSurfaceLookup(surface);
   struct pipe_resource *resource = NULL;
   if (s == NULL)
      return NULL;
   void *base = IOSurfaceGetBaseAddress(s);
   size_t alloc = IOSurfaceGetAllocSize(s), size = (alloc + 16383) & ~(size_t)16383;
   if (IOSurfaceGetPixelFormat(s) == 0x42475241 /* 'BGRA' */ && IOSurfaceGetPlaneCount(s) <= 1 && base != NULL &&
       ((uintptr_t)base & 16383) == 0 && screen->resource_from_handle != NULL) {
      int fd = sweet_drm_import_host(base, size);
      if (fd >= 0) {
         struct pipe_resource templ = {
            .target = PIPE_TEXTURE_2D,
            .format = PIPE_FORMAT_B8G8R8A8_UNORM,
            .width0 = (unsigned)IOSurfaceGetWidth(s),
            .height0 = (uint16_t)IOSurfaceGetHeight(s),
            .depth0 = 1,
            .array_size = 1,
            .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET,
         };
         struct winsys_handle handle = { .type = WINSYS_HANDLE_TYPE_FD };
         handle.handle = (unsigned)fd;
         handle.stride = (unsigned)IOSurfaceGetBytesPerRow(s);
         handle.offset = 0;
         handle.size = size;
         resource = screen->resource_from_handle(screen, &templ, &handle, 0);
         sweet_drm_forget_fd(fd);
      }
   }
   CFRelease(s);
   return resource;
#else
   (void)screen;
   (void)surface;
   return NULL;
#endif
}

""" + anchor, 1)
wr('src/gallium/targets/osmesa/osmesa_target.c', t)
sub_once('src/gallium/targets/osmesa/meson.build', "driver_swrast, driver_panfrost, dep_libdrm, idep_mesautil,",
         "driver_swrast, driver_panfrost, dep_libdrm, idep_mesautil,")

print("patch-mesa: ok")
