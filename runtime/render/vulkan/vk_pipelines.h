// vulkan_device.cpp's pipeline and shader-module code (internal to ar_render_vulkan): the render passes, the draw and
// resolve pipelines, the combiner's shaders, and the pipeline list's line format.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "render_device.h"
namespace ar {
namespace vkp {
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
// encode_resolve's push constants (std430: the same offsets as the GLSL block)
struct ResolveU { float dst_origin[2]; uint32_t src_origin[2]; uint32_t src_last[2]; uint32_t linearize; uint32_t src_layer; };
static_assert(sizeof(ResolveU) == 32, "ResolveU matches the GLSL block");

// A draw pipeline's state beyond its shaders: compare op (3 bits), depth write (bit 3), cull mode (bits 4-5).
inline uint32_t state_key(VkCompareOp cmp, bool write, VkCullModeFlags cull) {
    return (uint32_t)cmp | (write ? 8u : 0u) | ((uint32_t)cull << 4);
}
const uint32_t kDefaultState = (uint32_t)VK_COMPARE_OP_GREATER_OR_EQUAL | 8u;

VkShaderModule make_module(VkDevice dev, const std::vector<uint32_t> &spv);
// A target's render pass (RGBA8 + D32S8, one subpass, attachment layouts in and out; the device's barriers order it).
VkRenderPass make_target_pass(VkDevice dev, bool clear);
// The resolve's render pass into an external image: colour (and depth when df is not UNDEFINED) loaded and stored,
// in and out in the attachment layouts.
VkRenderPass make_resolve_pass(VkDevice dev, VkFormat cf, VkFormat df);
// The resolve's shaders: vertex, fragment, and the fragment that also writes gl_FragDepth.
bool make_resolve_shaders(VkDevice dev, VkShaderModule out[3], std::string *err);
VkPipeline make_resolve_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, VkRenderPass rp, VkShaderModule vs,
                                 VkShaderModule fs, bool depth);

// metal_device.mm pipeline_form: every silhouette copy shares the shade combiner's pipeline.
DrawPacket pipeline_form(const DrawPacket &d);
// A draw's combiner shaders (emit_glsl + compile_glsl). refused: the GLSL or the driver failed, not a combiner the
// renderer cannot draw (unsupported).
bool make_combiner_shaders(VkDevice dev, const DrawPacket &draw, VkShaderModule *vs, VkShaderModule *fs, std::string *why, bool *refused);
struct DrawPipelineDesc {
    VkShaderModule vs, fs;
    bool blend, depth_only;
    uint32_t state;   // state_key
    VkPipelineLayout layout;
    VkRenderPass rp;
    VkPipelineCache cache;
};
VkPipeline make_draw_pipeline(VkDevice dev, const DrawPipelineDesc &d, std::string *why);

// metal_device.mm's pipeline list lines, with the VkFormat number as the format field: lines naming another format
// are skipped.
struct ListLine { DrawPacket d; bool depth; uint32_t samples; };
std::vector<ListLine> parse_pipeline_lines(const std::string &list);
}
}
