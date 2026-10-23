#pragma once
// The fixed (non-combiner) Vulkan GLSL shaders, ports of metal_device.mm's kCompositeMsl, kMipMsl, kResolveMsl, kDepthMsl and
// kAoMsl (kEnvMsl is not ported: the Quest has no probe). Every source starts with "#version 450\n" as its first line; a
// caller selects a variant by inserting #define lines right after it. Compile with compile_glsl (glsl_compiler.h).
//
// Names, stages and interfaces (set, binding):
//
// fullscreen.vert (vertex): a full-screen triangle from gl_VertexIndex 0..2, no inputs, no vertex buffer (Metal's
//   cvert / rvert). Shared by composite.frag and resolve.frag.
//
// composite.frag (fragment; Metal cfrag): premultiplied over: o + u (1 - o.a), both read by texel (texelFetch level 0 at
//   gl_FragCoord.xy, Metal's texture.read).
//   set 0 binding 0  sampler2D under
//   set 0 binding 1  sampler2D over
//   location 0       vec4 colour out
//
// resolve.frag (fragment; Metal rfrag / rfrag_color / rfrag_a / rfrag_color_a): copies the source texel 1:1, optionally
//   un-premultiply / sRGB -> linear / premultiply (RU.linearize).
//   defines: RESOLVE_ARRAY (the sources are 2D arrays, read at RU.src_layer; else plain 2D),
//            RESOLVE_DEPTH (also writes gl_FragDepth from srcd).
//   set 0 binding 0  sampler2D (RESOLVE_ARRAY: sampler2DArray) src
//   set 0 binding 1  sampler2D (RESOLVE_ARRAY: sampler2DArray) srcd, the source depth (declared in every variant)
//   push constant    RU { vec2 dst_origin; uvec2 src_origin; uvec2 src_last; uint linearize; uint src_layer; } (32 bytes,
//                    vk_pipelines.h ResolveU)
//   location 0       vec4 colour out
//
// depth.vert (vertex; Metal dmain): the depth-only passes (shadow map, underside, footprint, AO views). Optional render
//   interpolation from the previous positions, the world clip as five gl_ClipDistance values.
//   vertex input     location 0 vec3 pos, location 1 vec2 uv0 (vk_layout.h's vertex layout, binding 0 stride 32)
//   set 0 binding 0  uniform M { mat4 m; }                         clip from model
//   set 0 binding 1  uniform DepthU { vec4 interp; vec4 wclip_lo; vec4 wclip_hi; }
//                    interp: x alpha, y 1 = interpolate (prevp); world clip as ViewU (lo.w floor on, hi.w XZ box on)
//   set 0 binding 2  readonly buffer PrevPos { float prevp[]; }     three floats per vertex, by vertex index
//   location 0 out   vec2 uv0
//
// depth_cut.frag (fragment; Metal dcut): discard when texture(tex0, uv0).a <= 0.3; no colour output.
//   set 1 binding 0  sampler2D tex0
//   location 0 in    vec2 uv0
//
// ao_fill.comp (compute, local size 4 x 4 x 4; Metal ao_fill): one invocation per voxel of the AO volume; writes the
//   share of the views that see the sky from it.
//   set 0 binding 0  uniform AoU { vec4 min_voxel; uvec4 dims_n; vec4 range; mat4 uv[32]; }
//                    min_voxel: xyz the first voxel's centre, w voxel size; dims_n: xyz dims, w view count (<= 32);
//                    range.x the occlusion radius (level units); uv[j]: view j's (u, v, depth) from level space
//                    (the matrices live in the same block, after the Metal AoU members: 48 + 32 x 64 = 2096 bytes std140)
//   set 0 binding 1  sampler2DArray maps                           the views' depth maps, read by texel
//   set 0 binding 2  writeonly image3D (r8) vol                    the AO volume
//
// mip_gamma.comp (compute, local size 8 x 8; Metal mip_gamma): one invocation per destination texel; the alpha-weighted
//   2 x 2 mean in linear light (3 taps along an odd source's last row / column), written sRGB-encoded.
//   set 0 binding 0  sampler2D src                                 the source level (a view of that one level), by texel
//   set 0 binding 1  writeonly image2D (rgba8) dst                 the destination level
namespace ar {
// The named shader's source, or nullptr for an unknown name.
const char *fixed_shader(const char *name);
}
