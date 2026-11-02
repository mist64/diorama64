#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ar {
enum class GlslStage { vertex, fragment, compute };
// glslang's process-wide state: once before the first compile_glsl, once after the last.
void glsl_compiler_init();
void glsl_compiler_finish();
// Vulkan GLSL (Vulkan 1.1, SPIR-V 1.3, the default resource limits) to SPIR-V. On failure: false, *spirv untouched, and
// *error the info log of the failing step (parse or link).
bool compile_glsl(const std::string &source, GlslStage stage, std::vector<uint32_t> *spirv, std::string *error);
}
