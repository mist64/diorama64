#include "glsl_fixed.h"
#include <cstring>
// Ports of metal_device.mm's fixed shaders, in its order; the interfaces are documented in glsl_fixed.h. Every source's
// first line is "#version 450": callers insert #define lines after it.
namespace ar {
namespace {
// Metal's cvert / rvert: a full-screen triangle from the vertex index.
const char *kFullscreenVert = R"(#version 450
void main() {
    const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
// Metal's cfrag: over on under, premultiplied, by texel.
const char *kCompositeFrag = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D under;
layout(set = 0, binding = 1) uniform sampler2D over;
layout(location = 0) out vec4 o_color;
void main() {
    const ivec2 p = ivec2(uvec2(gl_FragCoord.xy));
    const vec4 o = texelFetch(over, p, 0);
    const vec4 u = texelFetch(under, p, 0);
    o_color = o + u * (1.0 - o.a);
}
)";
// Alpha-weighted mean of the 2 x 2 parents in linear light, so mips neither darken nor take a dark fringe from clear
// texels. (Metal's select(a, b, c) is mix(a, b, c) with a bvec.)
const char *kMipGammaComp = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
vec3 to_linear(vec3 c) { return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045))); }
vec3 to_srgb(vec3 c) { return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, lessThanEqual(c, vec3(0.0031308))); }
void main() {
    const uvec2 g = gl_GlobalInvocationID.xy;
    const uvec2 dsz = uvec2(imageSize(dst));
    if (g.x >= dsz.x || g.y >= dsz.y) return;
    const uvec2 sz = uvec2(textureSize(src, 0)), last = sz - 1u;
    const uint nx = (sz.x > 1u && (sz.x & 1u) != 0u && g.x == dsz.x - 1u) ? 3u : 2u;
    const uint ny = (sz.y > 1u && (sz.y & 1u) != 0u && g.y == dsz.y - 1u) ? 3u : 2u;
    vec3 sum = vec3(0.0), plain = vec3(0.0);
    float a = 0.0;
    for (uint j = 0u; j < ny; j++) for (uint i = 0u; i < nx; i++) {
        const vec4 c = texelFetch(src, ivec2(min(g * 2u + uvec2(i, j), last)), 0);
        const vec3 l = to_linear(c.rgb);
        sum += l * c.a; plain += l; a += c.a;
    }
    const float n = float(nx * ny);
    const vec3 l = a > 0.0 ? sum / a : plain / n;
    imageStore(dst, ivec2(g), vec4(to_srgb(clamp(l, 0.0, 1.0)), a / n));
}
)";
// Metal's rfrag / rfrag_color (2D) and rfrag_a / rfrag_color_a (RESOLVE_ARRAY: 2D arrays at RU.src_layer); RESOLVE_DEPTH
// also writes the source depth. src_last: the view's last source pixel.
const char *kResolveFrag = R"(#version 450
#ifdef RESOLVE_ARRAY
layout(set = 0, binding = 0) uniform sampler2DArray src;
layout(set = 0, binding = 1) uniform sampler2DArray srcd;
#define SOURCE_TEXEL(t, p) texelFetch(t, ivec3(p, u.src_layer), 0)
#else
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D srcd;
#define SOURCE_TEXEL(t, p) texelFetch(t, ivec2(p), 0)
#endif
layout(push_constant) uniform RU { vec2 dst_origin; uvec2 src_origin; uvec2 src_last; uint linearize; uint src_layer; } u;
layout(location = 0) out vec4 o_color;
void main() {
    const uvec2 sz = uvec2(textureSize(src, 0).xy);
    const uvec2 p = min(uvec2(gl_FragCoord.xy - u.dst_origin) + u.src_origin, min(u.src_last, sz - 1u));
    vec4 c = SOURCE_TEXEL(src, p);
    if (u.linearize != 0u && c.a > 0.0) {   // un-premultiply, sRGB -> linear, premultiply again
        const vec3 g = clamp(c.rgb / c.a, 0.0, 1.0);
        c.rgb = mix(pow((g + 0.055) / 1.055, vec3(2.4)), g / 12.92, lessThanEqual(g, vec3(0.04045))) * c.a;
    }
    o_color = c;
#ifdef RESOLVE_DEPTH
    gl_FragDepth = SOURCE_TEXEL(srcd, p).r;
#endif
}
)";
// Metal's dmain. interp x alpha, y 1 = interpolate (prevp); world clip as ViewU (lo.w floor on, hi.w XZ box on).
const char *kDepthVert = R"(#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv0;
layout(location = 0) out vec2 o_uv0;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[5]; };
layout(std140, set = 0, binding = 0) uniform M { mat4 m; };
layout(std140, set = 0, binding = 1) uniform DepthU { vec4 interp; vec4 wclip_lo; vec4 wclip_hi; } u;
layout(std430, set = 0, binding = 2) readonly buffer PrevPos { float prevp[]; };
void main() {
    const int vid = gl_VertexIndex;
    const vec3 pos = u.interp.y > 0.5 ? mix(vec3(prevp[3 * vid], prevp[3 * vid + 1], prevp[3 * vid + 2]), in_pos, u.interp.x) : in_pos;
    gl_Position = m * vec4(pos, 1.0); o_uv0 = in_uv0;
    const float off = 1.0;
    gl_ClipDistance[0] = u.wclip_lo.w > 0.5 ? pos.y - u.wclip_lo.y : off;
    const bool box = u.wclip_hi.w > 0.5;
    gl_ClipDistance[1] = box ? pos.x - u.wclip_lo.x : off;
    gl_ClipDistance[2] = box ? u.wclip_hi.x - pos.x : off;
    gl_ClipDistance[3] = box ? pos.z - u.wclip_lo.z : off;
    gl_ClipDistance[4] = box ? u.wclip_hi.z - pos.z : off;
}
)";
// Metal's dcut: a cutout's clear texels cast no shadow.
const char *kDepthCutFrag = R"(#version 450
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec2 i_uv0;
void main() {
    if (texture(tex0, i_uv0).a <= 0.3) { discard; }
}
)";
// Metal's ao_fill. AoU: min_voxel xyz min, w voxel; dims_n xyz dims, w view count; range x radius; uv: the views'
// (u, v, depth) from level space (Metal's buffer 1, here in the same block).
// A view occludes a voxel when the nearest surface towards the sky lies in front of it by more than a small bias (0.002 of
// the map's depth) and less than the radius (level units): AO, not the whole level's sky.
const char *kAoFillComp = R"(#version 450
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
layout(std140, set = 0, binding = 0) uniform AoU { vec4 min_voxel; uvec4 dims_n; vec4 range; mat4 uv[32]; } u;
layout(set = 0, binding = 1) uniform sampler2DArray maps;
#ifdef AO_RGBA8
layout(set = 0, binding = 2, rgba8) uniform writeonly image3D vol;
#else
layout(set = 0, binding = 2, r8) uniform writeonly image3D vol;
#endif
void main() {
    const uvec3 id = gl_GlobalInvocationID;
    if (any(greaterThanEqual(id, u.dims_n.xyz))) { return; }
    const vec3 p = u.min_voxel.xyz + vec3(id) * u.min_voxel.w;
    const uvec2 wh = uvec2(textureSize(maps, 0).xy);
    float seen = 0.0;
    for (uint j = 0u; j < u.dims_n.w; j++) {
        const vec4 q = u.uv[j] * vec4(p, 1.0);
        const uvec2 px = uvec2(clamp(q.xy / q.w, 0.0, 0.9999) * vec2(wh));
        const float per_unit = length(vec3(u.uv[j][0].z, u.uv[j][1].z, u.uv[j][2].z));   // depth per level unit (orthographic)
        const float ahead = q.z / q.w - texelFetch(maps, ivec3(px, j), 0).r;               // depth from the occluder to the voxel
        seen += (ahead <= 0.002 || ahead >= u.range.x * per_unit) ? 1.0 : 0.0;
    }
    imageStore(vol, ivec3(id), vec4(seen / float(u.dims_n.w)));
}
)";
const struct { const char *name, *source; } kShaders[] = {
    {"fullscreen.vert", kFullscreenVert}, {"composite.frag", kCompositeFrag}, {"mip_gamma.comp", kMipGammaComp},
    {"resolve.frag", kResolveFrag},       {"depth.vert", kDepthVert},         {"depth_cut.frag", kDepthCutFrag},
    {"ao_fill.comp", kAoFillComp},
};
}
const char *fixed_shader(const char *name) {
    if (!name) return nullptr;
    for (const auto &s : kShaders) if (std::strcmp(s.name, name) == 0) return s.source;
    return nullptr;
}
}
