#include "glsl_emitter.h"
#include "vk_layout.h"
// The Vulkan GLSL port of msl_emitter.cpp, kept parallel to it (the same prelude order, helper names, rgb_op / a_op and body
// assembly) so the remaining helpers port one for one. Mapping from the Metal source:
// - float2/3/4 -> vec2/3/4, int3 -> ivec3, float4x4 -> mat4, saturate -> the saturate overloads below, dfdx/dfdy -> dFdx/dFdy,
//   discard_fragment() -> discard, t.sample(s, uv) -> texture(t, uv), sm.sample_compare(cs, uv, ref) -> texture(sm, vec3(uv, ref)),
//   calculate_unclamped_lod -> textureQueryLod(...).y. Reads through Metal's linear_clamp_ sampler (s3, not mipmapped: the
//   underside map, the AO volume, the probe cubes) are textureLod(..., 0.0) (that sampler's level 0); a depth2d sampled as a
//   value is a sampler2D's .r, and foot.read(q) is texelFetch(foot, q, 0).r with get_width / get_height from textureSize.
// - Metal names that are GLSL keywords or reserved words take a trailing underscore: LightU.cast_, room_light's flat_ and
//   table_shadow's cast_.
// - The uniform structs are std140 uniform blocks with the same member names and order (the sizes and offsets match the C++
//   structs in metal_device.mm: ViewU 160, DrawU 80, LightU 704). GLSL cannot pass a block to a function, so the helpers read
//   the blocks (d, L, V) and textures (t0, t1, sm, under, ao, env, env_prev, foot) as globals instead of taking them as
//   parameters; the fragment's stage input `in` (a GLSL keyword) is `fi`, a VOut built at the top of main.
// - vmain is the vertex shader's main, fmain the fragment shader's main. VOutV's clip_distance is gl_ClipDistance[5], its
//   layer gl_Layer (only when CombinerIR::layer_out; otherwise no layer is written at all: the fragment stage never reads it).
// - The vertex normals (kBindNormals): Metal's char4 per vertex is one int per vertex in an SSBO, x in the low byte, unpacked
//   with signed bitfieldExtract (exactly float3(char4.xyz)). The previous positions (kBindPrevPos): Metal's packed_float3
//   is three floats per vertex in an SSBO of float.
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
@LAYER@    o_nrm = (d.lflags & 256u) != 0u ? vec3(bitfieldExtract(nrm[vid], 0, 8), bitfieldExtract(nrm[vid], 8, 8), bitfieldExtract(nrm[vid], 16, 8)) / 127.0 : vec3(0.0);
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
// The room-lit look: the factor on a FC_LF_TINT draw's colour, tint x M. S is the key light's visibility
// (FC_LF_RECEIVER draws with a shadow map), A the ambient share, K = 1 - A, n the surface normal, u = max(l.y, 0.2).
// - BAKED (and other tinted draws): M = (A + K u S)/(A + K u), lit flat ground keeps its colour (shadows only); with
//   relight and normals, mixed by strength towards (A + K max(n.l, 0) S)/(A + K u): slopes turned away from the key darken.
// - GAME_LIT: the colour already holds the game's light from the key, so the shadow takes away only its direct part:
//   M = (A + K c S)/(A + K c), c = max(n.l, 0) (n: the normal, or the visible side of the face without normals). A face
//   turned away from the key is not darkened again. Static GAME_LIT with relight and normals mixes towards the model per
//   pixel relative to its value at the vertices, (A + K c S)/lit: flat faces keep the game's colour exactly.
// Both forms are written as 1 - (...)(1 - S) so that S = 1 gives exactly 1.
// AO: O, the sky visibility of the static scene from the baked volume (RECEIVER draws), mixed by strength, scales the
// ambient part only (A becomes A O); the key light's direct part is the shadow map's (S). O = 1 without a bake.
// SELF_LIT (DrawU.lflags bit 512, fc_draw.light_role FC_LR_SELF_LIT): S = 1. The colour holds the model's own complete
// light (SM64's title head: gd's front lights), so the room's key does not shadow it again (the cap shadowed the face: it
// was the whole of the dim face); AO and the tint still apply, and it still casts into the shadow map.
// (Metal's local flat is flat_ here: flat is a GLSL keyword.)
vec3 room_light(VOut fi) {
    const vec3 l = L.dir_on.xyz;
    const float A = L.tint_amb.a, K = 1.0 - A;
    const bool has_n = (d.lflags & 256u) != 0u && length(fi.nrm) > 0.5;
    const vec3 n = surface_normal(fi);
    const bool receiver = (d.lflags & 32u) != 0u;
    const bool self_lit = (d.lflags & 512u) != 0u;          // FC_LR_SELF_LIT: not shadowed by the key again
    vec3 face = vec3(0.0, 1.0, 0.0);
    vec3 g = vec3(0.0, 1.0, 0.0);
    if (receiver && (L.shadow.x > 0.5 || L.ao.x > 0.5)) {   // uniform per draw: derivatives stay defined
        g = cross(dFdx(fi.mpos), dFdy(fi.mpos));           // the face's normal
        g = length(g) > 1e-12 ? normalize(g) : vec3(0.0, 1.0, 0.0);
        face = dot(g, fi.veye) < 0.0 ? -g : g;              // the side the eye sees
    }
    float S = 1.0;
    if (receiver && L.shadow.x > 0.5 && !self_lit) {        // turned towards the light: the receiver offset then also
        S = shadow_vis(fi, dot(g, l) < 0.0 ? -g : g);       // clears walls and slopes
    }
    float O = 1.0;
    if (receiver && L.ao.x > 0.5) {                         // the sky visibility 1.5 voxels out along the normal
        const vec3 an = has_n ? n : face;
        const vec3 t = (fi.mpos + an * 1.5 * L.ao_min.w - L.ao_min.xyz) / L.ao_min.w + 0.5;   // in texels
        const vec3 beyond = max(-t, t - 1.0 / L.ao_inv.xyz);  // texels outside the volume (<= 0 inside)
        const float fade = saturate(max(beyond.x, max(beyond.y, beyond.z)));   // to no AO within a voxel outside it
        // The volume counts sky directions over the whole upper hemisphere, so a free wall sees only the half in front
        // of it: relative to an unoccluded surface of this orientation (1 facing up, 1/2 vertical, less facing down), so
        // open walls keep their light and only real occlusion darkens.
        const float free_sky = clamp(0.5 + 0.5 * an.y, 0.2, 1.0);
        const float vis = min(textureLod(ao, t * L.ao_inv.xyz, 0.0).r / free_sky, 1.0);
        O = mix(1.0, mix(vis, 1.0, fade), L.ao.y);
    }
    const bool relight = L.relight.x > 0.5 && has_n;
    float m;
    if ((d.lflags & 4u) != 0u) {
        const float c = max(dot(has_n ? n : face, l), 0.0);
        const float flat_ = 1.0 - (A * (1.0 - O) + K * c * (1.0 - S)) / (A + K * c);
        const float full = clamp((A * O + K * c * S) / max(fi.lit, 1e-3), 0.0, 1.6);
        m = (relight && (d.lflags & 8u) != 0u) ? mix(flat_, full, L.relight.y) : flat_;
    } else {
        const float u = max(l.y, 0.2);
        const float flat_ = 1.0 - (A * (1.0 - O) + K * u * (1.0 - S)) / (A + K * u);
        const float full = clamp((A * O + K * max(dot(n, l), 0.0) * S) / (A + K * u), 0.0, 1.6);
        m = ((d.lflags & 2u) != 0u && relight) ? mix(flat_, full, L.relight.y) : flat_;
    }
    return L.tint_amb.rgb * m;
}
// The vertex colour the combiner sees. Original N64 (dir_on.w == 0) or a draw without FC_LF_TINT: exactly fi.shade.
vec4 room_shade(VOut fi) {
    vec4 s = fi.shade;
    if (L.dir_on.w < 0.5 || (d.lflags & 1u) == 0u) { return s; }
    s.rgb = clamp(s.rgb * room_light(fi), 0.0, 1.0);
    return s;
}
// The toy material: a painted-plastic highlight and a rim on lit objects (FC_LF_GAME_LIT, not
// FC_LF_STATIC: most of the level is game-lit too, and the level never looks glossy). Known gap: lit terrain that is also
// see-through loses STATIC (and CASTER) in the capture's roles, so it passes this gate and takes the toy look; it is rare
// (most water and smoke are unlit) and an exact test needs a role bit in the flags. The real room in the room mirror only
// (FC_LF_REFLECTIVE with FC_LR_MIRROR: Mario's metal cap) when the probe is there; every other environment-mapped
// draw (Chain Chomp, King Bob-omb) keeps the game's own environment map, as does the cap without a probe.
// Added after the combiner; off in Original N64 and with toy off. The highlight is a dielectric's (matte painted
// plastic): Blinn-Phong towards the key light in the light's colour (never the surface's), broad (exponent 4..32),
// normalised so a tighter gloss keeps its energy, taken away by the shadow map like the direct light. The cube is
// sampled at its top mip (sharp mirror, no roughness blur): the probe itself is low-resolution. The mirror is never
// brighter than 1.5 x ref, the draw's unlit texel (texture 0, or the combined colour of a combiner without it): at the
// exposures of a dark room (34-52 on device) the tone-mapped room is near white, which made the cap a chrome light.
// DrawU.lflags bit 1024: FC_LR_MIRROR.
vec3 toy_surface(VOut fi, vec3 c, vec3 ref) {
    if (L.dir_on.w < 0.5 || L.toy.x < 0.5 || (d.lflags & 12u) != 4u) { return c; }
    const vec3 n = surface_normal(fi);
    const vec3 v = normalize(V.eye_view.xyz - fi.mpos);
    const vec3 l = L.dir_on.xyz;
    const float K = 1.0 - L.tint_amb.a;
    const float S = ((d.lflags & 32u) != 0u && (d.lflags & 512u) == 0u) ? shadow_vis(fi, n) : 1.0;   // self-lit
    const float p = mix(4.0, 32.0, L.toy.y);
    const float spec = L.toy.y * K * S * pow(max(dot(n, normalize(l + v + vec3(0.0, 1e-4, 0.0))), 0.0), p) * (p + 8.0) / 64.0;
    const float rim = L.toy.z * pow(1.0 - max(dot(n, v), 0.0), 3.0) * (0.6 + 0.4 * L.tint_amb.a);
    vec3 o = c + (spec + rim) * L.tint_amb.rgb;
    if (L.toy.w > 0.0 && (d.lflags & 1152u) == 1152u) {   // FC_LF_REFLECTIVE (128) and FC_LR_MIRROR (1024)
        const vec3 r = reflect(-v, n);
        vec3 e = max(textureLod(env, vec3(dot(L.env0.xyz, r), dot(L.env1.xyz, r), dot(L.env2.xyz, r)), 0.0).rgb, vec3(0.0)) * L.env1.w;
        e = pow(e / (1.0 + e), vec3(1.0 / 2.2));   // tone-mapped into the gamma-space colours of the N64 look
        if (L.env5.w < 1.0) {                          // cross-fading from the last probe's cube
            vec3 q = max(textureLod(env_prev, vec3(dot(L.env3.xyz, r), dot(L.env4.xyz, r), dot(L.env5.xyz, r)), 0.0).rgb, vec3(0.0)) * L.env4.w;
            e = mix(pow(q / (1.0 + q), vec3(1.0 / 2.2)), e, L.env5.w);
        }
        const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
        const float cap = 1.5 * dot(max(ref, vec3(0.0)), lw), le = dot(e, lw);
        if (le > cap) { e *= cap / le; }                 // never brighter than 1.5 x the unlit texel; the room's hue stays
        o = mix(o, e, 0.6 * L.toy.w);   // toy.w: the room reflection's weight (LightingFrame::env_mix)
    }
    return min(o, vec3(1.0));
}
// The contact shadow on the table: 12 taps on a disc; the lowest geometry above each tap darkens it the
// more the closer it is to the table. Plus the key light's shadow of the diorama on the table. Black
// with alpha: the layer darkens passthrough. (Metal's local cast is cast_ here: cast is a GLSL reserved word.)
vec4 table_shadow(VOut fi) {
    const vec2 disc[12] = vec2[12](vec2(1.0, 0.0), vec2(0.5, 0.866), vec2(-0.5, 0.866), vec2(-1.0, 0.0), vec2(-0.5, -0.866),
                                   vec2(0.5, -0.866), vec2(0.35, 0.2), vec2(0.0, 0.4), vec2(-0.35, 0.2), vec2(-0.35, -0.2),
                                   vec2(0.0, -0.4), vec2(0.35, -0.2));
    float occ = 0.0;
    if (L.contact.x > 0.0) {
        for (int i = 0; i < 12; i++) {
            const vec3 p = fi.mpos + vec3(disc[i].x, 0.0, disc[i].y) * L.contact.z;
            const vec4 q = L.under_uv_from_model * vec4(p, 1.0);
            const float d = textureLod(under, q.xy / q.w, 0.0).r;
            const float h = max(L.under_range.x + d * L.under_range.y - L.contact.w, 0.0);
            occ += d < 1.0 ? saturate(1.0 - h / L.contact.y) : 0.0;
        }
    }
    const float cast_ = L.cast_.x * (1.0 - L.tint_amb.a) * (1.0 - shadow_vis(fi, vec3(0.0, 1.0, 0.0)));
    const float a = 1.0 - (1.0 - L.contact.x * occ / 12.0) * (1.0 - cast_);
    if (a <= 1.0 / 255.0) { discard; }
    return vec4(0.0, 0.0, 0.0, a);
}
// lava's heat shimmer: a small wobble of its texture coordinates, by the game's clock.
vec2 lava_shimmer(vec3 p) {
    const float t = L.liquid.y, k = L.liquid2.x * 0.5;
    return 0.004 * vec2(sin(p.x * k + t * 3.1), cos(p.z * k + t * 2.7));
}
// water (DrawU.lflags 2048) and lava (8192) in the room-lit look (LightU.liquid.x). Water: the face normal (the side
// the eye sees) tilted by two crossing ripples, the room probe's reflection (kBindEnv, when bound: LightU.toy.w; else the
// room's tint) by Schlick's Fresnel (F0 0.02), and its alpha raised toward 1 by F (a mirror at grazing angles). Lava: its
// colour divided by the room's dimming (never by less than 0.35) times a slow pulse, so it glows in a dark room.
vec4 liquid_surface(VOut fi, vec4 c) {
    if (L.liquid.x < 0.5 || (d.lflags & 10240u) == 0u) { return c; }
    const float t = L.liquid.y;
    if ((d.lflags & 8192u) != 0u) {
        const float pulse = 0.85 + 0.15 * sin(t * 2.0 + dot(fi.mpos.xz, vec2(0.004, 0.003)));
        const float dim = max(dot(L.tint_amb.rgb, vec3(0.299, 0.587, 0.114)), 0.35);
        c.rgb = min(c.rgb * pulse / dim, vec3(1.0));
        return c;
    }
    vec3 g = cross(dFdx(fi.mpos), dFdy(fi.mpos));
    g = length(g) > 1e-12 ? normalize(g) : vec3(0.0, 1.0, 0.0);
    const vec3 v = normalize(V.eye_view.xyz - fi.mpos);
    if (dot(g, v) < 0.0) { g = -g; }
    const float k = L.liquid2.x;
    const vec2 w = vec2(cos(dot(fi.mpos.xz, vec2(k, 0.31 * k)) + t * 1.7), cos(dot(fi.mpos.xz, vec2(-0.43 * k, 0.9 * k)) + t * 1.3));
    const vec3 n = normalize(g + L.liquid2.y * vec3(w.x, 0.0, w.y));
    const float F = 0.02 + 0.98 * pow(1.0 - saturate(dot(n, v)), 5.0);
    vec3 e = L.tint_amb.rgb * vec3(0.80, 0.88, 1.0);
    if (L.toy.w > 0.0) {
        const vec3 r = reflect(-v, n);
        const vec3 q = max(textureLod(env, vec3(dot(L.env0.xyz, r), dot(L.env1.xyz, r), dot(L.env2.xyz, r)), 0.0).rgb, vec3(0.0)) * L.env1.w;
        e = pow(q / (1.0 + q), vec3(1.0 / 2.2));
    }
    c.rgb = mix(c.rgb, min(e, vec3(1.0)), F);
    c.a = mix(c.a, 1.0, F);
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
// the share of a water draw (lflags 2048) kept by the water trim
// (LightU.liquid2.z): 1 off a water surface (waterfalls, streams: a sheet lies within liquid2.w of a box's surface height;
// SM64 draws its own quad shapes at that height, not the box's rectangle), else the bilinear coverage of the footprint map
// (kBindWaterFoot: the static scene under the water from above, depth < 1 where there is any) at the fragment's x, z over
// LightU.water_foot's window (nothing outside it). It scales the alpha (no discard: the pipelines keep early depth tests),
// so it trims blended sheets only.
float water_trim(VOut fi) {
    if (L.liquid.x < 0.5 || L.liquid2.z < 0.5 || (d.lflags & 2048u) == 0u) { return 1.0; }
    const int n = min(int(L.liquid.z), 8);
    bool sheet = false;
    for (int i = 0; i < n; i++) { sheet = sheet || abs(fi.mpos.y - L.water_y[i / 4][i % 4]) <= L.liquid2.w; }
    if (!sheet) { return 1.0; }
    const vec2 uv = (fi.mpos.xz - L.water_foot.xy) * L.water_foot.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { return 0.0; }
    const vec2 size = vec2(textureSize(foot, 0));
    const vec2 t = uv * size - 0.5;
    const vec2 f = fract(t);
    const ivec2 i0 = ivec2(floor(t)), top = ivec2(size) - 1;
    float c[4];
    for (int k = 0; k < 4; k++) {
        const ivec2 q = clamp(i0 + ivec2(k & 1, k >> 1), ivec2(0), top);
        c[k] = texelFetch(foot, q, 0).r < 1.0 ? 1.0 : 0.0;
    }
    return mix(mix(c[0], c[1], f.x), mix(c[2], c[3], f.x), f.y);
}
// the near-wall cutaway (static terrain the renderer marked: CombinerIR::cutaway; LightU.cut.w > 0). A cone from
// the centre eye E (ViewU.eye_pull, the same for both eyes) to the kept point T (LightU.cut.xyz, Mario's middle),
// LightU.cut.w its radius at T and 0 at E. A fragment inside it and nearer to E than T is dithered away in world cells
// (LightU.cut2.z level units: the same cells in both eyes), keeping LightU.cut2.x of them in the core (0) and more toward the
// rim. Floors (normal y > 0.01, the game's floor rule, at any slope) within LightU.cut2.y of T along the axis stay: the
// ground Mario stands on is never cut. The cells lie in the surface's own plane (the axis it
// faces most is left out: a wall on a cell boundary along it would flip cells per pixel and per eye), hashed as integers.
// a draw fading in (DrawU.fade < 1) loses only that share of the cells it would lose: a room joins the cut
// as it fades in, and its hole is whole when the fade ends (no pop).
uint cut_hash(ivec3 c) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ uint(c.z) * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
bool cut_away(VOut fi, float fade) {
    vec3 g = cross(dFdx(fi.mpos), dFdy(fi.mpos));            // first: the derivatives need the whole quad
    const vec3 e = V.eye_pull.xyz, a = L.cut.xyz - e;
    const float len2 = dot(a, a);
    if (len2 < 1e-6) { return false; }
    const vec3 p = fi.mpos - e;
    const float s = dot(p, a) / len2;                        // 0 at the eye, 1 at T
    if (s <= 0.0 || s >= 1.0) { return false; }
    const float r = L.cut.w * s;
    const float dist = length(p - a * s);
    if (dist >= r) { return false; }
    g = length(g) > 1e-12 ? normalize(g) : vec3(0.0, 1.0, 0.0);
    if (dot(g, -p) < 0.0) { g = -g; }                        // the side the eye sees
    if (g.y > 0.01 && (1.0 - s) * sqrt(len2) < L.cut2.y) { return false; }
    const vec3 q = floor(fi.mpos / L.cut2.z), n = abs(g);
    ivec3 cell;                                              // ties (45 degrees) go to y, then x: the same axis every pixel
    if (n.y >= max(n.x, n.z) - 0.01) { cell = ivec3(int(q.x), 0, int(q.z)); }
    else if (n.x >= n.z - 0.01) { cell = ivec3(0, int(q.y), int(q.z)); }
    else { cell = ivec3(int(q.x), int(q.y), 0); }
    const float h = float(cut_hash(cell) >> 8) * (1.0 / 16777216.0);
    const float keep = mix(L.cut2.x, 1.0, smoothstep(0.7 * r, r, dist));
    return h >= (fade < 1.0 ? mix(1.0, keep, fade) : keep);   // a fading room's cells go with its fade
}
// vanilla's warp transitions on the diorama (LightU.trans.x: 1 fade, 2 iris). Fade: every world draw mixes toward
// the transition colour by the cover (0 clear .. 1 covered). Iris: only outside a circle around trans_c on the table plane,
// whose radius shrinks from trans.z (cover 0) to 0 (cover 1), with a trans.w soft edge. Cover 0 (or off) returns c exactly
// (an iris at cover 0 would otherwise recolour its soft rim).
vec3 transition_cover(VOut fi, vec3 c) {
    if (L.trans.x < 0.5 || L.trans.y <= 0.0) { return c; }
    float f = L.trans.y;
    if (L.trans.x > 1.5) {
        const float r = L.trans.z * (1.0 - L.trans.y);
        f = smoothstep(r - L.trans.w, r, length(fi.mpos.xz - L.trans_c.xz));
    }
    return mix(c, L.trans_rgb.rgb, saturate(f));
}
// FC_GROUP_INK: the lightness inverted, the hue kept (rgb shifted by 1 - (max + min): white to black, black to white,
// a mid-lightness colour such as red unchanged).
vec3 ink_flip(vec3 c) { return saturate(c + (1.0 - (max(c.r, max(c.g, c.b)) + min(c.r, min(c.g, c.b))))); }
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
