// mtltest's shaders, compiled to AIR at build time (xcrun metal + metallib, iOS 16) and loaded with newLibraryWithURL:.
#include <metal_stdlib>
using namespace metal;

kernel void scale_add(device const float *in [[buffer(0)]], device float *out [[buffer(1)]],
                      uint id [[thread_position_in_grid]])
{
	out[id] = in[id] * 2.0f + 1.0f;
}

struct Varyings {
	float4 position [[position]];
};

// The top-left quarter of the target, as two triangles; no vertex buffers.
vertex Varyings quad_vertex(uint vid [[vertex_id]])
{
	const float2 corners[6] = { float2(-1, 1), float2(0, 1), float2(-1, 0), float2(0, 1), float2(0, 0), float2(-1, 0) };
	Varyings out;
	out.position = float4(corners[vid], 0, 1);
	return out;
}

fragment float4 quad_fragment(Varyings in [[stage_in]])
{
	return float4(1.0, 0.2, 0.0, 1.0);
}
