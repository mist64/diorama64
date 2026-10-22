#include "glsl_emitter.h"
#include "vk_layout.h"
// The Vulkan GLSL port of msl_emitter.cpp, kept parallel to it (the same prelude order, helper names, rgb_op / a_op and body
// assembly) so the remaining helpers port one for one. Mapping from the Metal source:
// - float2/3/4 -> vec2/3/4, int3 -> ivec3, float4x4 -> mat4, saturate -> the saturate overloads below, dfdx/dfdy -> dFdx/dFdy,
//   discard_fragment() -> discard, t.sample(s, uv) -> texture(t, uv), sm.sample_compare(cs, uv, ref) -> texture(sm, vec3(uv, ref)),
//   calculate_unclamped_lod -> textureQueryLod(...).y.
// - The uniform structs are std140 uniform blocks with the same member names and order (the sizes and offsets match the C++
//   structs in metal_device.mm: ViewU 160, DrawU 80, LightU 704). GLSL cannot pass a block to a function, so the helpers read
//   the blocks (d, L, V) and textures (t0, t1, sm, under, ao, env, env_prev, foot) as globals instead of taking them as
//   parameters; the fragment's stage input `in` (a GLSL keyword) is `fi`, a VOut built at the top of main.
// - vmain is the vertex shader's main, fmain the fragment shader's main. VOutV's clip_distance is gl_ClipDistance[5], its
//   layer gl_Layer (only when CombinerIR::layer_out; otherwise no layer is written at all: the fragment stage never reads it).
// - The vertex normals (kBindNormals): Metal's char4 per vertex is one int per vertex in an SSBO, x in the low byte, unpacked
//   with signed bitfieldExtract (exactly float3(char4.xyz)). The previous positions (kBindPrevPos): Metal's packed_float3
//   is three floats per vertex in an SSBO of float.
// Room-lit helpers marked "plan 2: port from msl_emitter.cpp" return their neutral value (the feature off) for now.
namespace ar {
static const char *kUniforms = R"(// eye_view: this view's own eye (xyz); layer: the render target array slice of this pass's view (0 unless layered).
layout(std140, set = @SET_U@, binding = @BIND_VIEW@) uniform ViewU {
    mat4 clip_from_model; vec4 eye_pull; vec4 eye_view; uint layer; uint pad0; uint pad1; uint pad2;
    vec4 wclip_lo; vec4 wclip_hi;          // world clip (level units): lo.w floor on, hi.w box on
    vec4 interp;                           // render interpolation: x alpha, y 1 = on (prevp, kBindPrevPos)
} @VIEW@;
layout(std140, set = @SET_U@, binding = @BIND_DRAW@) uniform DrawU {
    vec4 prim; vec4 env; float lod; float decal; float noise_seed; float fade; vec4 clip; float clip_on; uint lflags; float pull; float talpha;
} d;
layout(std140, set = @SET_U@, binding = @BIND_LIGHT@) uniform LightU {
    vec4 dir_on; vec4 tint_amb;       // xyz towards the key light (model space), w 1 = room-lit; rgb tint x gain, a ambient
    vec4 contact;                     // x strength (0 = off), y fade height, z blur radius, w table top y (level units)
    mat4 under_uv_from_model;         // the underside map's (u, v, depth)
    vec4 under_range;                 // x: y at depth 0, y: height span
    vec4 shadow;                      // x 1 = shadow map bound, y PCF radius (texels), z 1/size, w normal offset (level units)
    mat4 shadow_uv_from_model;        // the shadow map's (u, v, depth)
    vec4 cast_;                       // x: the key-light shadow's strength on the table (Metal: cast, a GLSL reserved word)
    vec4 relight;                     // x 1 = per-pixel light of draws with normals (BAKED, static GAME_LIT), y strength
    vec4 ao;                          // x 1 = the AO volume (kBindAo) holds this frame's static scene, y strength
    vec4 ao_min;                      // xyz the volume's first voxel (level units), w voxel size
    vec4 ao_inv;                      // xyz 1 / dims: voxel i's centre is at (i + 0.5) / dims
    vec4 toy;                         // x 1 = toy material, y gloss, z rim, w 1 = reflections (a probe cube at kBindEnv)
    vec4 env0; vec4 env1; vec4 env2;  // rows of probe_from_level (xyz); env1.w the probe's exposure
    vec4 env3; vec4 env4; vec4 env5;  // the outgoing cube's rows (kBindEnvOut); env4.w its exposure, env5.w the
                                      // incoming cube's share (1: kBindEnv alone)
    vec4 liquid;                      // x 1 = the liquid look, y the game's clock (s), z water boxes (0..8), w level
                                      // units of water to the full tint
    vec4 liquid2;                     // x ripple wave number (rad per level unit), y ripple slope, z 1 = the water trim
                                      // (the footprint map, kBindWaterFoot), w the trim's sheet tolerance (level units)
    vec4 water_box[8];                // x0, z0 (xy), x1, z1 (zw), level units
    vec4 water_y[2];                  // the boxes' surfaces: water_y[i / 4][i % 4]
    vec4 water_foot;                  // the footprint map's window: x0, z0 (xy), 1 / its size (zw)
    vec4 cut;                         // xyz the kept point (Mario's middle), w the cone's radius there (0 = off)
    vec4 cut2;                        // x share of cells kept in the core, y floors kept this near the point, z cell size
    vec4 trans;                       // x kind (0 off, 1 fade, 2 iris), y cover, z iris radius at cover 0, w soft edge
    vec4 trans_rgb;                   // rgb the transition colour
    vec4 trans_c;                     // xyz the iris's centre (level units)
    vec4 tex;                         // x 1 = cutouts keep their coverage, y alpha gain per mip level
} L;
// DrawU.lflags bit 256: the pass has per-vertex normals (kBindNormals, by vertex index); bit 512: the draw is
// FC_LR_SELF_LIT; bit 1024: FC_LR_MIRROR; bits 2048 / 8192: FC_LR_WATER / FC_LR_LAVA; bit 4096: ink
// (FC_GROUP_INK).
// ViewU.interp.y: the pass has previous positions (kBindPrevPos, by vertex index).
)";
// vmain. VIn: locations 0-3 (vk_layout.h's vertex layout). VOut: locations 0-6, the same members as Metal's VOut after pos.
// nrm: the vertex normal (level space; 0 without normals); veye: towards the centre eye; lit: the light model at the vertex
// normal, A + K max(n.l, 0) (the reference of game-lit per-pixel light).
static const char *kVertex = R"(layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv0;
layout(location = 2) in vec2 in_uv1;
layout(location = 3) in vec4 in_shade;
layout(location = 0) out vec2 o_uv0;
layout(location = 1) out vec2 o_uv1;
layout(location = 2) out vec4 o_shade;
layout(location = 3) out vec3 o_mpos;
layout(location = 4) out vec3 o_nrm;
layout(location = 5) out vec3 o_veye;
layout(location = 6) out float o_lit;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[5]; };
layout(std430, set = @SET_U@, binding = @BIND_NORMALS@) readonly buffer Normals { int nrm[]; };
layout(std430, set = @SET_U@, binding = @BIND_PREVPOS@) readonly buffer PrevPos { float prevp[]; };
void main() {
    const int vid = gl_VertexIndex;
    // render interpolation: this vertex between the previous tick (alpha 0) and now (1); off: exactly in_pos
    const vec3 pos = v.interp.y > 0.5 ? mix(vec3(prevp[3 * vid], prevp[3 * vid + 1], prevp[3 * vid + 2]), in_pos, v.interp.x) : in_pos;
    // Pull toward the centre eye (level units): an N64 decal's (eye_pull.w; not polygon offset) plus a
    // silhouette's clearance (d.pull). A decal moves; a silhouette copy (never a decal) keeps its unpulled x, y in every
    // view and takes only the pulled point's depth, so its coverage matches its owner's draw in each eye.
    vec3 p = pos;
    if (d.decal > 0.5 || d.pull > 0.0) {
        const float pull = (d.decal > 0.5 ? v.eye_pull.w : 0.0) + d.pull;
        vec3 to_eye = v.eye_pull.xyz - p;
        float len = length(to_eye);
        // A silhouette's pull stops short of the eye (never past it: a negative-w depth); a decal's is as before.
        if (len > 0.0) { p += to_eye * ((d.pull > 0.0 ? min(pull, 0.99 * len) : pull) / len); }
    }
    gl_Position = v.clip_from_model * vec4(d.pull > 0.0 ? pos : p, 1.0);
    if (d.pull > 0.0) {                        // depth only: the pulled point's NDC depth at the unpulled position
        const vec4 cp = v.clip_from_model * vec4(p, 1.0);
        gl_Position.z = cp.z / cp.w * gl_Position.w;
    }
    o_uv0 = in_uv0; o_uv1 = in_uv1; o_shade = in_shade; o_mpos = pos;
    // world clip: hardware clip distances (no discard, early-Z stays). Undecaled position: the plane is in level units.
    const float off = 1.0;
    gl_ClipDistance[0] = v.wclip_lo.w > 0.5 ? pos.y - v.wclip_lo.y : off;
    const bool box = v.wclip_hi.w > 0.5;
    gl_ClipDistance[1] = box ? pos.x - v.wclip_lo.x : off;
    gl_ClipDistance[2] = box ? v.wclip_hi.x - pos.x : off;
    gl_ClipDistance[3] = box ? pos.z - v.wclip_lo.z : off;
    gl_ClipDistance[4] = box ? v.wclip_hi.z - pos.z : off;
@LAYER@    const int nw = nrm[vid];
    o_nrm = (d.lflags & 256u) != 0u ? vec3(bitfieldExtract(nw, 0, 8), bitfieldExtract(nw, 8, 8), bitfieldExtract(nw, 16, 8)) / 127.0 : vec3(0.0);
    o_veye = v.eye_pull.xyz - pos;
    const float nl = length(o_nrm);
    o_lit = L.tint_amb.a + (1.0 - L.tint_amb.a) * (nl > 0.5 ? max(dot(o_nrm / nl, L.dir_on.xyz), 0.0) : 0.0);
}
)";
static const char *kPrelude = R"(layout(location = 0) in vec2 i_uv0;
layout(location = 1) in vec2 i_uv1;
layout(location = 2) in vec4 i_shade;
layout(location = 3) in vec3 i_mpos;
layout(location = 4) in vec3 i_nrm;
layout(location = 5) in vec3 i_veye;
layout(location = 6) in float i_lit;
layout(location = 0) out vec4 o_color;
layout(set = @SET_T@, binding = @BIND_TEX0@) uniform sampler2D t0;
layout(set = @SET_T@, binding = @BIND_TEX1@) uniform sampler2D t1;
layout(set = @SET_T@, binding = @BIND_SHADOW@) uniform sampler2DShadow sm;
layout(set = @SET_T@, binding = @BIND_UNDERSIDE@) uniform sampler2D under;
layout(set = @SET_T@, binding = @BIND_AO@) uniform sampler3D ao;
layout(set = @SET_T@, binding = @BIND_ENV@) uniform samplerCube env;
layout(set = @SET_T@, binding = @BIND_ENV_OUT@) uniform samplerCube env_prev;
layout(set = @SET_T@, binding = @BIND_WATER_FOOT@) uniform sampler2D foot;
// fmain's stage input (Metal's VOut, less the layer, which no fragment reads); pos is the window position.
struct VOut { vec4 pos; vec2 uv0; vec2 uv1; vec4 shade; vec3 mpos; vec3 nrm; vec3 veye; float lit; };
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }
vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }
// The draw's normal at this fragment (interpolated, unit), or up without normals.
vec3 surface_normal(VOut fi) {
    const float l = length(fi.nrm);
    return ((d.lflags & 256u) != 0u && l > 0.5) ? fi.nrm / l : vec3(0.0, 1.0, 0.0);
}
// The key light's visibility at this fragment (1 lit, 0 shadowed): 3x3 hardware-PCF taps of the shadow map, the receiver
// pushed along n against acne. 1 without a shadow map or outside it.
float shadow_vis(VOut fi, vec3 n) {
    if (L.shadow.x < 0.5) { return 1.0; }
    const vec4 q = L.shadow_uv_from_model * vec4(fi.mpos + n * L.shadow.w, 1.0);
    const vec2 uv = q.xy / q.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { return 1.0; }
    const float r = L.shadow.y * L.shadow.z;
    const float ref = min(q.z / q.w, 1.0);             // beyond the far plane: lit against the cleared depth 1
    float s = 0.0;
    for (int j = -1; j <= 1; j++) { for (int i = -1; i <= 1; i++) { s += texture(sm, vec3(uv + vec2(i, j) * r, ref)); } }
    return s / 9.0;
}
// The room-lit look: the factor on a FC_LF_TINT draw's colour (see msl_emitter.cpp's room_light).
// plan 2: port from msl_emitter.cpp. Neutral: 1 (the colour unchanged).
vec3 room_light(VOut fi) {
    return vec3(1.0);
}
// The vertex colour the combiner sees. Original N64 (dir_on.w == 0) or a draw without FC_LF_TINT: exactly fi.shade.
vec4 room_shade(VOut fi) {
    vec4 s = fi.shade;
    if (L.dir_on.w < 0.5 || (d.lflags & 1u) == 0u) { return s; }
    s.rgb = clamp(s.rgb * room_light(fi), 0.0, 1.0);
    return s;
}
// The toy material: a painted-plastic highlight and a rim on lit objects (see msl_emitter.cpp's toy_surface).
// plan 2: port from msl_emitter.cpp. Neutral: c (the toy look off).
vec3 toy_surface(VOut fi, vec3 c, vec3 ref) {
    return c;
}
// The contact shadow on the table plus the key light's shadow of the diorama on the table (see msl_emitter.cpp's
// table_shadow). plan 2: port from msl_emitter.cpp. Neutral: no contact and no cast shadow, alpha 0, which Metal discards.
vec4 table_shadow(VOut fi) {
    discard;
    return vec4(0.0);
}
// lava's heat shimmer: a small wobble of its texture coordinates, by the game's clock.
vec2 lava_shimmer(vec3 p) {
    const float t = L.liquid.y, k = L.liquid2.x * 0.5;
    return 0.004 * vec2(sin(p.x * k + t * 3.1), cos(p.z * k + t * 2.7));
}
// water (DrawU.lflags 2048) and lava (8192) in the room-lit look (see msl_emitter.cpp's liquid_surface).
// plan 2: port from msl_emitter.cpp. Neutral: c (the liquid look off).
vec4 liquid_surface(VOut fi, vec4 c) {
    return c;
}
// what lies inside a water box and under its surface (receivers, not the liquids themselves) takes a blue-green
// depth tint, full (0.6) at LightU.liquid.w below the surface.
vec3 under_water(VOut fi, vec3 c) {
    if (L.liquid.x < 0.5 || L.liquid.z < 0.5 || (d.lflags & 32u) == 0u || (d.lflags & 10240u) != 0u) { return c; }
    const int n = min(int(L.liquid.z), 8);
    for (int i = 0; i < n; i++) {
        const vec4 b = L.water_box[i];
        const float y = L.water_y[i / 4][i % 4];
        if (fi.mpos.x > b.x && fi.mpos.x < b.z && fi.mpos.z > b.y && fi.mpos.z < b.w && fi.mpos.y < y) {
            const float f = 0.6 * saturate((y - fi.mpos.y) / max(L.liquid.w, 1e-3));
            return mix(c, vec3(0.10, 0.35, 0.45) * L.tint_amb.rgb, f);
        }
    }
    return c;
}
// the share of a water draw (lflags 2048) kept by the water trim (see msl_emitter.cpp's water_trim).
// plan 2: port from msl_emitter.cpp. Neutral: 1 (nothing trimmed).
float water_trim(VOut fi) {
    return 1.0;
}
// the near-wall cutaway's cell hash (see msl_emitter.cpp's cut_hash and cut_away).
uint cut_hash(ivec3 c) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ uint(c.z) * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
// plan 2: port from msl_emitter.cpp. Neutral: false (nothing cut away).
bool cut_away(VOut fi, float fade) {
    return false;
}
// vanilla's warp transitions on the diorama (see msl_emitter.cpp's transition_cover).
// plan 2: port from msl_emitter.cpp. Neutral: c (no transition).
vec3 transition_cover(VOut fi, vec3 c) {
    return c;
}
// FC_GROUP_INK: the lightness inverted, the hue kept (see msl_emitter.cpp's ink_flip).
// plan 2: port from msl_emitter.cpp. Neutral: c.
vec3 ink_flip(vec3 c) { return c; }
void main() {
    const VOut fi = VOut(gl_FragCoord, i_uv0, i_uv1, i_shade, i_mpos, i_nrm, i_veye, i_lit);
)";
// The game's scissor, only in the pipelines of draws that have one (CombinerIR::clip): a discard makes depth and stencil
// tests late (see msl_emitter.cpp's kClip).
static const char *kClip = R"(    if (d.clip_on > 0.5 && (fi.mpos.x < d.clip.x || fi.mpos.y < d.clip.y || fi.mpos.x > d.clip.z || fi.mpos.y > d.clip.w)) { discard; }
)";
static const char *kBodyStart = R"(    vec2 uv0 = fi.uv0;
    if ((d.lflags & 8192u) != 0u && L.liquid.x > 0.5) { uv0 += lava_shimmer(fi.mpos); }   // lava's heat shimmer
    vec4 tex0 = texture(t0, uv0);
    vec4 tex1 = texture(t1, fi.uv1);
    vec3 cell = floor(fi.mpos) + d.noise_seed;   // world-space cells keep the dither stereo-stable
    float noise = floor(fract(sin(dot(sin(cell), vec3(12.9898, 78.233, 37.719))) * 143758.5453) + 0.5);   // gfx_opengl random()
    vec4 c = vec4(0.0);
)";

static std::string rgb_op(uint8_t s) {
    switch (s) {
        case FC_SRC_1: return "vec3(1.0)";
        case FC_SRC_TEXEL0: return "tex0.rgb";
        case FC_SRC_TEXEL0A: return "vec3(tex0.a)";
        case FC_SRC_TEXEL1: return "tex1.rgb";
        case FC_SRC_TEXEL1A: return "vec3(tex1.a)";
        case FC_SRC_PRIM: return "d.prim.rgb";
        case FC_SRC_PRIMA: return "vec3(d.prim.a)";
        case FC_SRC_SHADE: return "shade.rgb";
        case FC_SRC_SHADEA: return "vec3(shade.a)";
        case FC_SRC_ENV: return "d.env.rgb";
        case FC_SRC_ENVA: return "vec3(d.env.a)";
        case FC_SRC_LOD: return "vec3(d.lod)";
        case FC_SRC_COMBINED: return "c.rgb";
        case FC_SRC_COMBINEDA: return "vec3(c.a)";
        case FC_SRC_NOISE: return "vec3(noise)";
        default: return "vec3(0.0)";
    }
}
static std::string a_op(uint8_t s) {
    switch (s) {
        case FC_SRC_1: return "1.0";
        case FC_SRC_TEXEL0: case FC_SRC_TEXEL0A: return "tex0.a";
        case FC_SRC_TEXEL1: case FC_SRC_TEXEL1A: return "tex1.a";
        case FC_SRC_PRIM: case FC_SRC_PRIMA: return "d.prim.a";
        case FC_SRC_SHADE: case FC_SRC_SHADEA: return "shade.a";
        case FC_SRC_ENV: case FC_SRC_ENVA: return "d.env.a";
        case FC_SRC_LOD: return "d.lod";
        case FC_SRC_COMBINED: case FC_SRC_COMBINEDA: return "c.a";
        case FC_SRC_NOISE: return "noise";
        default: return "0.0";
    }
}

static bool reads_shade_rgb(const CombinerIR &ir) {
    for (int cyc = 0; cyc < (ir.two_cycle ? 2 : 1); cyc++)
        for (int k = 0; k < 4; k++) if (ir.rgb[cyc][k] == FC_SRC_SHADE) return true;
    return false;
}
static bool reads_texel0_rgb(const CombinerIR &ir) {
    for (int cyc = 0; cyc < (ir.two_cycle ? 2 : 1); cyc++)
        for (int k = 0; k < 4; k++) if (ir.rgb[cyc][k] == FC_SRC_TEXEL0) return true;
    return false;
}
static void replace_all(std::string &s, const std::string &from, const std::string &to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}
// The vk_layout.h numbers in the layout(set=, binding=) qualifiers.
static std::string with_layout(std::string s) {
    const struct { const char *token; uint32_t value; } kTokens[] = {
        {"@SET_U@", kSetUniforms}, {"@SET_T@", kSetTextures},
        {"@BIND_VIEW@", kBindView}, {"@BIND_DRAW@", kBindDraw}, {"@BIND_LIGHT@", kBindLight},
        {"@BIND_NORMALS@", kBindNormals}, {"@BIND_PREVPOS@", kBindPrevPos},
        {"@BIND_TEX0@", kBindTex0}, {"@BIND_TEX1@", kBindTex1}, {"@BIND_SHADOW@", kBindShadow},
        {"@BIND_UNDERSIDE@", kBindUnderside}, {"@BIND_AO@", kBindAo}, {"@BIND_ENV@", kBindEnv},
        {"@BIND_ENV_OUT@", kBindEnvOut}, {"@BIND_WATER_FOOT@", kBindWaterFoot},
    };
    for (const auto &t : kTokens) replace_all(s, t.token, std::to_string(t.value));
    return s;
}
// The uniform blocks; Metal's vmain names its ViewU v, fmain V.
static std::string uniforms(const char *view) {
    std::string s = kUniforms;
    replace_all(s, "@VIEW@", view);
    return s;
}
static std::string emit_vertex(const CombinerIR &ir) {
    std::string s = "#version 450\n";
    if (ir.layer_out) s += "#extension GL_ARB_shader_viewport_layer_array : require\n";
    s += uniforms("v");
    s += kVertex;
    replace_all(s, "@LAYER@", ir.layer_out ? "    gl_Layer = int(v.layer);\n" : "");
    return with_layout(s);
}
static std::string emit_fragment(const CombinerIR &ir) {
    std::string s = "#version 450\n";
    s += uniforms("V");
    s += kPrelude;
    if (ir.clip) s += kClip;
    if (ir.cutaway) s += "    if (L.cut.w > 0.0 && cut_away(fi, d.fade)) { discard; }\n";
    s += kBodyStart;
    if (ir.flat_color) {
        if (ir.flat_cutout) s += "    if (tex0.a <= 0.3) { discard; }\n";
        s += "    o_color = vec4(transition_cover(fi, d.prim.rgb), d.prim.a * d.talpha);\n}\n";
        return with_layout(s);
    }
    if (ir.table_shadow) { s += "    o_color = table_shadow(fi);\n}\n"; return with_layout(s); }
    // The room-lit look modulates the vertex colour before the combiner; a combiner that never reads it (TEXEL, PRIM or
    // ENV only) gets the same factor on its colour instead, so every FC_LF_TINT draw takes the room's light.
    const bool shade_rgb = reads_shade_rgb(ir);
    s += shade_rgb ? "    vec4 shade = room_shade(fi);\n" : "    vec4 shade = fi.shade;\n";
    for (int cyc = 0; cyc < (ir.two_cycle ? 2 : 1); cyc++) {
        const uint8_t *r = ir.rgb[cyc], *a = ir.alpha[cyc];
        s += "    {\n";
        s += "        vec3 rgb = (" + rgb_op(r[0]) + " - " + rgb_op(r[1]) + ") * " + rgb_op(r[2]) + " + " + rgb_op(r[3]) + ";\n";
        s += "        float a = (" + a_op(a[0]) + " - " + a_op(a[1]) + ") * " + a_op(a[2]) + " + " + a_op(a[3]) + ";\n";
        s += "        c = vec4(rgb, a);\n";
        s += "    }\n";
    }
    if (!shade_rgb)
        s += "    if (L.dir_on.w > 0.5 && (d.lflags & 1u) != 0u) { c.rgb = clamp(c.rgb * room_light(fi), 0.0, 1.0); }\n";
    s += "    vec4 texel = c;\n";
    s += std::string("    texel.rgb = toy_surface(fi, texel.rgb, ") + (reads_texel0_rgb(ir) ? "tex0.rgb" : "c.rgb") + ");\n";
    s += "    texel = liquid_surface(fi, texel);\n";
    s += "    texel.rgb = under_water(fi, texel.rgb);\n";
    s += "    texel.rgb = transition_cover(fi, texel.rgb);\n";
    s += "    if ((d.lflags & 4096u) != 0u) { texel.rgb = ink_flip(texel.rgb); }\n";
    s += "    texel.a *= water_trim(fi);\n";
    if (!ir.alpha_enabled) {
        s += "    texel.a = 1.0;\n";
    } else {
        if (ir.texture_edge)
            s += "    if (L.tex.x > 0.5) { texel.a = min(texel.a * (1.0 + max(textureQueryLod(t0, uv0).y, 0.0) * L.tex.y), 1.0); }\n";
        if (ir.texture_edge && ir.alpha_coverage)
            s += "    texel.a = clamp((texel.a - 0.3) / max(fwidth(texel.a), 1.0e-4) + 0.5, 0.0, 1.0);\n"
                 "    if (texel.a <= 0.0) { discard; }\n";
        else if (ir.texture_edge) s += "    if (texel.a > 0.3) { texel.a = 1.0; } else { discard; }\n";
        if (ir.dither) s += "    texel.a *= noise;\n";
    }
    if (ir.depth_fill == kDepthOnlyPrepass) s += "    if (texel.a < 0.5) { discard; }\n";
    s += "    texel.a *= d.fade * d.talpha;\n";
    if (ir.depth_fill == kDepthOnlyHalf) s += "    if (texel.a < 0.5) { discard; }\n";
    if (ir.depth_fill == kDepthOnlyFaint) s += "    if (texel.a <= 1.0 / 255.0) { discard; }\n";
    s += "    o_color = texel;\n}\n";
    return with_layout(s);
}
GlslProgram emit_glsl(const CombinerIR &ir) { return {emit_vertex(ir), emit_fragment(ir)}; }
}
