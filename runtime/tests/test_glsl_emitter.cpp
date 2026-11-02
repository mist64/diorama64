#include "check.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "combiner.h"
#include "glsl_compiler.h"
#include "glsl_emitter.h"
#include "glsl_fixed.h"

using namespace ar;

// Compiles one stage; on failure prints the log and the source.
static bool compiles(const std::string &src, GlslStage stage) {
    std::vector<uint32_t> spv; std::string err;
    if (compile_glsl(src, stage, &spv, &err) && !spv.empty()) return true;
    fprintf(stderr, "%s\n%s\n", err.c_str(), src.c_str());
    return false;
}
// A fixed shader with #define lines inserted after its #version line (the caller's convention).
static std::string with_defines(const char *src, const char *defines) {
    std::string s = src;
    s.insert(s.find('\n') + 1, defines);
    return s;
}

int main() {
    glsl_compiler_init();
    // the cube's key: shade only
    fc_combiner_key k{}; k.slots = ((uint64_t)FC_SRC_SHADE << 12) | ((uint64_t)FC_SRC_SHADE << 28);
    GlslProgram p = emit_glsl(decode_combiner(normalize_combiner(k)));
    std::vector<uint32_t> spv; std::string err;
    CHECK(compile_glsl(p.vertex, GlslStage::vertex, &spv, &err) && !spv.empty());
    CHECK(compile_glsl(p.fragment, GlslStage::fragment, &spv, &err) && !spv.empty());
    CHECK(p.vertex.find("gl_Layer") != std::string::npos && p.vertex.find("gl_ClipDistance") != std::string::npos);
    // every operand source in every slot compiles, with and without each option bit
    for (uint32_t src = 0; src < 16; src++) for (uint32_t opts = 0; opts < 128; opts += 9) {
        fc_combiner_key kk{}; for (int s = 0; s < 16; s++) kk.slots |= (uint64_t)src << (4 * s); kk.opts = opts;
        CombinerIR ir = decode_combiner(normalize_combiner(kk));
        if (ir.unsupported) continue;
        GlslProgram q = emit_glsl(ir);
        CHECK(compile_glsl(q.vertex, GlslStage::vertex, &spv, &err));
        if (!compile_glsl(q.fragment, GlslStage::fragment, &spv, &err)) { fprintf(stderr, "src=%u opts=%u\n%s\n%s\n", src, opts, err.c_str(), q.fragment.c_str()); CHECK(false); }
    }
    // the IR variants the renderer uses
    CombinerIR ir = decode_combiner(normalize_combiner(k));
    for (uint8_t df : {kDepthOnlyHalf, kDepthOnlyFaint, kDepthOnlyPrepass}) { CombinerIR v = ir; v.depth_fill = df; CHECK(compile_glsl(emit_glsl(v).fragment, GlslStage::fragment, &spv, &err)); }
    CombinerIR v = ir; v.layer_out = false; CHECK(emit_glsl(v).vertex.find("gl_Layer") == std::string::npos);
    v = ir; v.flat_color = true; v.flat_cutout = true; v.clip = true; v.table_shadow = true; v.cutaway = true;
    CHECK(compile_glsl(emit_glsl(v).fragment, GlslStage::fragment, &spv, &err));
    // each renderer feature alone
    for (int f = 0; f < 4; f++) {
        CombinerIR w = ir;
        if (f == 0) w.table_shadow = true;
        if (f == 1) w.cutaway = true;
        if (f == 2) { w.flat_color = true; w.flat_cutout = true; }
        if (f == 3) w.clip = true;
        const GlslProgram q = emit_glsl(w);
        CHECK(compiles(q.vertex, GlslStage::vertex));
        CHECK(compiles(q.fragment, GlslStage::fragment));
        CHECK(q.fragment.find("plan 2") == std::string::npos);
    }
    // two textures, two cycles, cutout edges with alpha to coverage
    {
        fc_combiner_key kt{};
        kt.slots = ((uint64_t)FC_SRC_TEXEL0 << 0) | ((uint64_t)FC_SRC_TEXEL1 << 4) | ((uint64_t)FC_SRC_SHADE << 8) |
                   ((uint64_t)FC_SRC_TEXEL0 << 16) | ((uint64_t)FC_SRC_TEXEL1 << 20) | ((uint64_t)FC_SRC_SHADE << 24);
        CombinerIR t = decode_combiner(normalize_combiner(kt));
        t.uses_tex0 = t.uses_tex1 = t.two_cycle = t.texture_edge = t.alpha_coverage = t.alpha_enabled = true;
        const GlslProgram q = emit_glsl(t);
        CHECK(compiles(q.vertex, GlslStage::vertex));
        CHECK(compiles(q.fragment, GlslStage::fragment));
        CHECK(q.fragment.find("plan 2") == std::string::npos);
    }
    // the room-lit helpers are present in the plain IR's fragment, and no plan-1 stub is left
    CHECK(p.fragment.find("room_light(") != std::string::npos && p.fragment.find("transition_cover(") != std::string::npos);
    CHECK(p.fragment.find("plan 2") == std::string::npos);
    // the fixed shaders
    CHECK(fixed_shader("ao_fill.comp") && compiles(fixed_shader("ao_fill.comp"), GlslStage::compute));
    CHECK(fixed_shader("resolve.frag") && compiles(fixed_shader("resolve.frag"), GlslStage::fragment));
    CHECK(compiles(with_defines(fixed_shader("resolve.frag"), "#define RESOLVE_ARRAY 1\n#define RESOLVE_DEPTH 1\n"), GlslStage::fragment));
    CHECK(compiles(with_defines(fixed_shader("resolve.frag"), "#define RESOLVE_ARRAY 1\n"), GlslStage::fragment));
    CHECK(compiles(with_defines(fixed_shader("resolve.frag"), "#define RESOLVE_DEPTH 1\n"), GlslStage::fragment));
    CHECK(fixed_shader("depth.vert") && compiles(fixed_shader("depth.vert"), GlslStage::vertex));
    CHECK(fixed_shader("depth_cut.frag") && compiles(fixed_shader("depth_cut.frag"), GlslStage::fragment));
    CHECK(fixed_shader("composite.frag") && compiles(fixed_shader("composite.frag"), GlslStage::fragment));
    CHECK(fixed_shader("fullscreen.vert") && compiles(fixed_shader("fullscreen.vert"), GlslStage::vertex));
    CHECK(fixed_shader("mip_gamma.comp") && compiles(fixed_shader("mip_gamma.comp"), GlslStage::compute));
    CHECK(fixed_shader("nope") == nullptr);
    // a broken source reports an error and no SPIR-V
    spv.clear(); CHECK(!compile_glsl("#version 450\nvoid main() { undefined(); }", GlslStage::fragment, &spv, &err) && !err.empty() && spv.empty());
    glsl_compiler_finish();
    return 0;
}
