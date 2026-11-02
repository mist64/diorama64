#pragma once
#include <string>
#include "combiner.h"
namespace ar {
struct GlslProgram { std::string vertex, fragment; };
// The Vulkan GLSL (#version 450) counterpart of emit_msl: one vertex and one fragment shader per combiner configuration.
GlslProgram emit_glsl(const CombinerIR &ir);
}
