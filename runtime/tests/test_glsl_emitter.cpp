#include "check.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "combiner.h"
#include "glsl_compiler.h"
#include "glsl_emitter.h"

using namespace ar;

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
    // a broken source reports an error and no SPIR-V
    spv.clear(); CHECK(!compile_glsl("#version 450\nvoid main() { undefined(); }", GlslStage::fragment, &spv, &err) && !err.empty() && spv.empty());
    glsl_compiler_finish();
    return 0;
}
