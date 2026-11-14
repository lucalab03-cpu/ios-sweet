/*
 * mtltest: Metal on the Mali-G52 through iOS's own Metal API (docs/METAL-PANVK.md, phase 3).
 * MTLCreateSystemDefaultDevice() goes through Metal.framework, which loads SweetMTLDriver.bundle (the NativeMetal frontend
 * over metal2vulkan over PanVK). Then, all through that device:
 *   compute  scale_add over 4096 floats (out = in * 2 + 1)
 *   render   a 64x64 RGBA8 target cleared blue, the top-left quarter drawn orange, copied to a buffer by a blit
 *   repeat   the same render 100 times, for a first frame-time number
 * Shaders: /usr/local/share/sweet/mtltest.metallib (built from mtltest.metal). Each step logs and goes on after a failure;
 * the last line is "mtltest: RESULT ...". Log: /var/tmp/sweetmtltest.log. Driver log: /var/tmp/sweetmtl2.log.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <IOSurface/IOSurfaceRef.h>
#include <signal.h>
#include <sys/mman.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define LIB "/usr/local/share/sweet/mtltest.metallib"

static FILE *out;

static void say(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void say(NSString *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	fprintf(out, "%s\n", s.UTF8String);
	fflush(out);
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void on_signal(int sig)
{
	fprintf(out, "mtltest: RESULT %s (signal %d): see the step above\n", sig == SIGALRM ? "timeout" : "crashed", sig);
	fflush(out);
	_exit(3);
}

static BOOL wait_ok(id<MTLCommandBuffer> cb, NSString *what, double *ms)
{
	double t0 = now();
	[cb commit];
	[cb waitUntilCompleted];
	if (ms)
		*ms = (now() - t0) * 1000;
	if (cb.status != MTLCommandBufferStatusCompleted) {
		say(@"mtltest: %@: command buffer status %ld, error %@", what, (long)cb.status, cb.error);
		return NO;
	}
	return YES;
}

/* One render of the quad into `target`, copied into `readback` (64 rows of 256 bytes). */
static BOOL render_once(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> rps, id<MTLTexture> target,
			id<MTLBuffer> readback, double *ms)
{
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
	if (!re) {
		say(@"mtltest: render: no render encoder");
		return NO;
	}
	[re setRenderPipelineState:rps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	if (readback) {
		id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
		[be copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			 sourceSize:MTLSizeMake(64, 64, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:256
		   destinationBytesPerImage:256 * 64];
		[be endEncoding];
	}
	return wait_ok(cb, @"render", ms);
}

/* Texels of a 64x64 picture off the expected one: `inside` in the top-left quarter, `outside` elsewhere (bytes in memory order). */
static int wrong_texels(const uint8_t *b, size_t row, const uint8_t inside[4], const uint8_t outside[4], uint32_t *first)
{
	int bad = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const uint8_t *t = b + y * row + x * 4;
			const uint8_t *want = x < 32 && y < 32 ? inside : outside;
			if (memcmp(t, want, 4)) {
				if (!bad && first)
					*first = (uint32_t)(t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24);
				bad++;
			}
		}
	return bad;
}

/* The pages a host import uses: what the CPU wrote goes to memory before the GPU's use, and nothing stale stays after. */
static void flush_lines(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

int main(void)
{
	@autoreleasepool {
		out = fopen("/var/tmp/sweetmtltest.log", "a");
		if (!out)
			out = stdout;
		freopen("/var/tmp/sweetmtltest-stderr.log", "a", stderr);
		signal(SIGALRM, on_signal);
		signal(SIGSEGV, on_signal);
		signal(SIGBUS, on_signal);
		signal(SIGILL, on_signal);
		signal(SIGABRT, on_signal);
		alarm(180);
		time_t wall = time(NULL);
		say(@"mtltest: start %s", ctime(&wall));

		double t0 = now();
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		double device_ms = (now() - t0) * 1000;
		say(@"mtltest: device %@ (class %s) in %.0f ms", dev ? dev.name : @"nil", dev ? object_getClassName(dev) : "-",
		    device_ms);
		if (!dev) {
			say(@"mtltest: RESULT no Metal device (see /var/tmp/sweetmtl2.log)");
			return 1;
		}
		NSError *err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@LIB] error:&err];
		say(@"mtltest: library %@: %@", @LIB, lib ? @"loaded" : err);
		if (!lib) {
			say(@"mtltest: RESULT no library");
			return 1;
		}
		id<MTLCommandQueue> q = [dev newCommandQueue];
		int compute_bad = -1, render_bad = -1;
		double compute_ms = 0, render_ms = 0, repeat_ms = -1;

		/* compute */
		id<MTLFunction> kfn = [lib newFunctionWithName:@"scale_add"];
		double p0 = now();
		id<MTLComputePipelineState> cps = kfn ? [dev newComputePipelineStateWithFunction:kfn error:&err] : nil;
		say(@"mtltest: compute pipeline in %.1f ms", (now() - p0) * 1000);
		if (!cps) {
			say(@"mtltest: compute pipeline: %@", err);
		} else {
			const int N = 4096;
			id<MTLBuffer> in = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
			id<MTLBuffer> res = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
			float *p = in.contents;
			for (int i = 0; i < N; i++)
				p[i] = (float)i;
			memset(res.contents, 0, N * 4);
			id<MTLCommandBuffer> cb = [q commandBuffer];
			id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
			[ce setComputePipelineState:cps];
			[ce setBuffer:in offset:0 atIndex:0];
			[ce setBuffer:res offset:0 atIndex:1];
			[ce dispatchThreadgroups:MTLSizeMake(N / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[ce endEncoding];
			if (wait_ok(cb, @"compute", &compute_ms)) {
				const float *r = res.contents;
				compute_bad = 0;
				for (int i = 0; i < N; i++)
					compute_bad += r[i] != i * 2.0f + 1.0f;
				say(@"mtltest: compute: %d of %d wrong in %.1f ms%@", compute_bad, N, compute_ms,
				    compute_bad ? [NSString stringWithFormat:@" (out[1] = %f)", r[1]] : @" (PASS)");
			}
		}

		/* render */
		MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
		rd.vertexFunction = [lib newFunctionWithName:@"quad_vertex"];
		rd.fragmentFunction = [lib newFunctionWithName:@"quad_fragment"];
		rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		p0 = now();
		id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
		say(@"mtltest: render pipeline in %.1f ms", (now() - p0) * 1000);
		if (!rps) {
			say(@"mtltest: render pipeline: %@", err);
		} else {
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
											width:64 height:64 mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget;
			td.storageMode = MTLStorageModePrivate;
			id<MTLTexture> target = [dev newTextureWithDescriptor:td];
			id<MTLBuffer> rb = [dev newBufferWithLength:256 * 64 options:MTLResourceStorageModeShared];
			memset(rb.contents, 0x5a, 256 * 64);
			if (target && render_once(dev, q, rps, target, rb, &render_ms)) {
				const uint8_t *b = rb.contents;
				render_bad = 0;
				uint32_t first = 0;
				for (int y = 0; y < 64; y++)
					for (int x = 0; x < 64; x++) {
						const uint8_t *t = b + y * 256 + x * 4;
						BOOL inside = x < 32 && y < 32;
						uint8_t want[4] = { inside ? 255 : 0, inside ? 51 : 0, inside ? 0 : 255, 255 };
						if (memcmp(t, want, 4)) {
							if (!render_bad)
								first = (uint32_t)(t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24);
							render_bad++;
						}
					}
				say(@"mtltest: render: %d of 4096 texels wrong in %.1f ms%@", render_bad, render_ms,
				    render_bad ? [NSString stringWithFormat:@" (first wrong %#x)", first] : @" (PASS)");
				/* repeat: 100 frames of the same pass */
				double r0 = now();
				int done = 0;
				for (; done < 100; done++)
					if (!render_once(dev, q, rps, target, rb, NULL))
						break;
				if (done == 100) {
					repeat_ms = (now() - r0) * 1000 / 100;
					say(@"mtltest: repeat: 100 renders, %.2f ms each", repeat_ms);
				} else {
					say(@"mtltest: repeat: render %d failed", done);
				}
			} else if (!target) {
				say(@"mtltest: render: no target texture");
			}
		}
		/* phase 4 checks, each on its own: host import, IOSurface, framebuffer fetch */
		static const uint8_t rgba_in[4] = { 255, 51, 0, 255 }, rgba_out[4] = { 0, 0, 255, 255 };
		static const uint8_t bgra_in[4] = { 0, 51, 255, 255 }, bgra_out[4] = { 255, 0, 0, 255 };
		int import_bad = -1, surface_bad = -1, fetch_bad = -1;
		uint32_t first = 0;

		/* host import: a no-copy buffer over 16 KB of our own pages, a linear texture over it, the quad drawn in place */
		if (rps) {
			void *pages = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			memset(pages, 0x5a, 16384);
			flush_lines(pages, 16384);
			id<MTLBuffer> nb = [dev newBufferWithBytesNoCopy:pages length:16384 options:MTLResourceStorageModeShared
							    deallocator:nil];
			MTLTextureDescriptor *ld = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
											width:64 height:64 mipmapped:NO];
			ld.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			ld.storageMode = MTLStorageModeShared;
			id<MTLTexture> lt = nb ? [nb newTextureWithDescriptor:ld offset:0 bytesPerRow:256] : nil;
			if (!nb || !lt) {
				say(@"mtltest: import: no-copy buffer %@, texture over it %@ (see sweetmtl2.log)", nb ? @"ok" : @"refused",
				    lt ? @"ok" : @"refused");
			} else if (render_once(dev, q, rps, lt, nil, NULL)) {
				flush_lines(pages, 16384);
				import_bad = wrong_texels(pages, 256, rgba_in, rgba_out, &first);
				say(@"mtltest: import: %d of 4096 texels wrong%@", import_bad,
				    import_bad ? [NSString stringWithFormat:@" (first %#x)", first] : @" (PASS)");
			}
			lt = nil;
			nb = nil;
			munmap(pages, 16384);
		}

		/* IOSurface: what CoreAnimation renders into; a BGRA surface, a texture over it, read back by locking it */
		{
			NSDictionary *props = @{
				(__bridge NSString *)kIOSurfaceWidth : @64, (__bridge NSString *)kIOSurfaceHeight : @64,
				(__bridge NSString *)kIOSurfaceBytesPerElement : @4, (__bridge NSString *)kIOSurfaceBytesPerRow : @256,
				(__bridge NSString *)kIOSurfacePixelFormat : @(0x42475241) /* 'BGRA' */
			};
			IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
			MTLRenderPipelineDescriptor *bd = [MTLRenderPipelineDescriptor new];
			bd.vertexFunction = [lib newFunctionWithName:@"quad_vertex"];
			bd.fragmentFunction = [lib newFunctionWithName:@"quad_fragment"];
			bd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
			id<MTLRenderPipelineState> bps = [dev newRenderPipelineStateWithDescriptor:bd error:&err];
			MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											width:64 height:64 mipmapped:NO];
			sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			id<MTLTexture> st = surf ? [dev newTextureWithDescriptor:sd iosurface:surf plane:0] : nil;
			if (!surf || !bps || !st) {
				say(@"mtltest: iosurface: surface %@, BGRA pipeline %@, texture %@", surf ? @"ok" : @"refused",
				    bps ? @"ok" : (id)err, st ? @"ok" : @"refused (see sweetmtl2.log)");
			} else if (render_once(dev, q, bps, st, nil, NULL)) {
				IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
				size_t row = IOSurfaceGetBytesPerRow(surf);
				surface_bad = wrong_texels(IOSurfaceGetBaseAddress(surf), row, bgra_in, bgra_out, &first);
				IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
				say(@"mtltest: iosurface: %d of 4096 texels wrong (row %zu)%@", surface_bad, row,
				    surface_bad ? [NSString stringWithFormat:@" (first %#x)", first] : @" (PASS)");
			}
			st = nil;
			if (surf)
				CFRelease(surf);
		}

		/* framebuffer fetch: the quad reads the cleared blue and writes it with red and blue swapped */
		{
			MTLRenderPipelineDescriptor *fd = [MTLRenderPipelineDescriptor new];
			fd.vertexFunction = [lib newFunctionWithName:@"quad_vertex"];
			fd.fragmentFunction = [lib newFunctionWithName:@"fetch_fragment"];
			fd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			double p0 = now();
			id<MTLRenderPipelineState> fps = [dev newRenderPipelineStateWithDescriptor:fd error:&err];
			say(@"mtltest: fetch: pipeline %@ in %.1f ms", fps ? @"made" : (id)err, (now() - p0) * 1000);
			MTLTextureDescriptor *td2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
											 width:64 height:64 mipmapped:NO];
			td2.usage = MTLTextureUsageRenderTarget;
			td2.storageMode = MTLStorageModePrivate;
			id<MTLTexture> ft = [dev newTextureWithDescriptor:td2];
			id<MTLBuffer> frb = [dev newBufferWithLength:256 * 64 options:MTLResourceStorageModeShared];
			if (fps && ft && render_once(dev, q, fps, ft, frb, NULL)) {
				static const uint8_t swapped[4] = { 255, 0, 0, 255 };
				fetch_bad = wrong_texels(frb.contents, 256, swapped, rgba_out, &first);
				say(@"mtltest: fetch: %d of 4096 texels wrong%@", fetch_bad,
				    fetch_bad ? [NSString stringWithFormat:@" (first %#x)", first] : @" (PASS)");
			}
		}

		say(@"mtltest: RESULT device %s, compute %s, render %s, %.2f ms per render, import %s, iosurface %s, fetch %s, "
		    @"device %.0f ms (pipelines: a second run of mtltest shows the cache)",
		    object_getClassName(dev), compute_bad == 0 ? "ok" : "WRONG", render_bad == 0 ? "ok" : "WRONG", repeat_ms,
		    import_bad == 0 ? "ok" : "WRONG", surface_bad == 0 ? "ok" : "WRONG", fetch_bad == 0 ? "ok" : "WRONG",
		    device_ms);
	}
	return 0;
}
