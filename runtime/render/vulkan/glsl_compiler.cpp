#include "glsl_compiler.h"
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
namespace ar {
void glsl_compiler_init() { glslang::InitializeProcess(); }
void glsl_compiler_finish() { glslang::FinalizeProcess(); }
bool compile_glsl(const std::string &source, GlslStage stage, std::vector<uint32_t> *spirv, std::string *error) {
    const EShLanguage lang = stage == GlslStage::vertex ? EShLangVertex : stage == GlslStage::fragment ? EShLangFragment : EShLangCompute;
    const EShMessages msgs = (EShMessages)(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader shader(lang);
    const char *src = source.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        if (error) *error = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        if (error) *error = program.getInfoLog();
        return false;
    }
    std::vector<uint32_t> out;
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), out, &opt);
    if (out.empty()) {
        if (error) *error = "GlslangToSpv produced no SPIR-V";
        return false;
    }
    if (spirv) *spirv = std::move(out);
    if (error) error->clear();
    return true;
}
}
