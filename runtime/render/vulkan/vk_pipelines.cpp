#include "vk_pipelines.h"
#include <cstdio>
#include "combiner.h"
#include "glsl_compiler.h"
#include "glsl_emitter.h"
#include "glsl_fixed.h"
#include "vk_layout.h"
namespace ar {
namespace vkp {
namespace {
const size_t kMaxWarmLines = 4096;     // parse_pipeline_lines reads at most this many lines
const size_t kMaxWarmLineBytes = 96;   // and skips longer lines unparsed
// One subpass, colour attachment 0 and (n == 2) depth attachment 1. external: dependencies with the work before and after
// the pass on the same attachments (the external image's; the targets' passes have explicit barriers instead).
VkRenderPass make_pass(VkDevice dev, const VkAttachmentDescription *a, uint32_t n, bool external) {
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cr;
    sp.pDepthStencilAttachment = n > 1 ? &dr : nullptr;
    const VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags ac = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkSubpassDependency deps[2] = {{VK_SUBPASS_EXTERNAL, 0, st, st, ac, ac, 0}, {0, VK_SUBPASS_EXTERNAL, st, st, ac, ac, 0}};
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = n;
    ri.pAttachments = a;
    ri.subpassCount = 1;
    ri.pSubpasses = &sp;
    ri.dependencyCount = external ? 2 : 0;
    ri.pDependencies = external ? deps : nullptr;
    VkRenderPass rp = VK_NULL_HANDLE;
    return vkCreateRenderPass(dev, &ri, nullptr, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}
}

VkShaderModule make_module(VkDevice dev, const std::vector<uint32_t> &spv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * sizeof(uint32_t);
    ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(dev, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkRenderPass make_target_pass(VkDevice dev, bool clear) {
    VkAttachmentDescription a[2] = {};
    a[0].format = kColorFormat;
    a[0].samples = VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1].format = kDepthFormat;
    a[1].samples = VK_SAMPLE_COUNT_1_BIT;
    a[1].loadOp = a[1].stencilLoadOp = a[0].loadOp;
    a[1].storeOp = a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[1].initialLayout = a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    // the barriers before (flush) and after (resolve, read-back) are explicit: no subpass dependencies
    return make_pass(dev, a, 2, false);
}
VkRenderPass make_resolve_pass(VkDevice dev, VkFormat cf, VkFormat df) {
    VkAttachmentDescription a[2] = {};
    a[0].format = cf;
    a[0].samples = VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // views may share an image: never discard the others
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1].format = df;
    a[1].samples = VK_SAMPLE_COUNT_1_BIT;
    a[1].loadOp = a[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a[1].storeOp = a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[1].initialLayout = a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    return make_pass(dev, a, df == VK_FORMAT_UNDEFINED ? 1 : 2, true);
}

bool make_resolve_shaders(VkDevice dev, VkShaderModule out[3], std::string *err) {
    // The resolve: a full-screen triangle; each fragment copies its source texel 1:1 (glsl_fixed.h). The targets are 2D
    // arrays (RESOLVE_ARRAY); the third module also writes the source depth (RESOLVE_DEPTH).
    std::string color_src = fixed_shader("resolve.frag"), depth_src = color_src;
    color_src.insert(color_src.find('\n') + 1, "#define RESOLVE_ARRAY 1\n");
    depth_src.insert(depth_src.find('\n') + 1, "#define RESOLVE_ARRAY 1\n#define RESOLVE_DEPTH 1\n");
    const std::string srcs[3] = {fixed_shader("fullscreen.vert"), color_src, depth_src};
    const GlslStage stages[3] = {GlslStage::vertex, GlslStage::fragment, GlslStage::fragment};
    for (int i = 0; i < 3; i++) {
        std::vector<uint32_t> spv;
        if (!compile_glsl(srcs[i], stages[i], &spv, err)) return false;
        if (!(out[i] = make_module(dev, spv))) { *err = "vkCreateShaderModule failed"; return false; }
    }
    return true;
}

VkPipeline make_resolve_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, VkRenderPass rp, VkShaderModule vs,
                                 VkShaderModule fs, bool depth) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         fs, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = depth ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = layout;
    gi.renderPass = rp;
    VkPipeline out = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(dev, cache, 1, &gi, nullptr, &out) == VK_SUCCESS ? out : VK_NULL_HANDLE;
}

DrawPacket pipeline_form(const DrawPacket &d) {
    if (!d.silhouette) return d;
    static const fc_combiner_key flat = normalize_combiner(fc_combiner_key{((uint64_t)FC_SRC_SHADE << 12) | ((uint64_t)FC_SRC_SHADE << 28), 0, 0});
    DrawPacket c = d;
    c.cc = flat;
    return c;
}

bool make_combiner_shaders(VkDevice dev, const DrawPacket &draw, VkShaderModule *vs_out, VkShaderModule *fs_out, std::string *why,
                           bool *refused) {
    const DrawPacket d = pipeline_form(draw);
    CombinerIR ir = decode_combiner(d.cc);
    ir.flat_color = d.silhouette != kSilhouetteNone;
    ir.flat_cutout = d.silhouette == kSilhouetteCutout;
    ir.depth_fill = d.depth_only;
    ir.table_shadow = d.table_shadow != 0;
    ir.clip = d.clip_on != 0;
    ir.alpha_coverage = false;   // single sample in this plan
    ir.layer_out = true;
    ir.cutaway = d.cutaway != 0;
    *refused = false;
    if (ir.unsupported) { *why = "unsupported combiner feature (light map)"; return false; }
    const GlslProgram prog = emit_glsl(ir);
    std::vector<uint32_t> vs, fs;
    std::string err;
    if (!compile_glsl(prog.vertex, GlslStage::vertex, &vs, &err)) { *why = "vertex GLSL: " + err; *refused = true; return false; }
    if (!compile_glsl(prog.fragment, GlslStage::fragment, &fs, &err)) { *why = "fragment GLSL: " + err; *refused = true; return false; }
    *vs_out = make_module(dev, vs);
    *fs_out = make_module(dev, fs);
    if (!*vs_out || !*fs_out) {
        for (VkShaderModule *m : {vs_out, fs_out}) if (*m) { vkDestroyShaderModule(dev, *m, nullptr); *m = VK_NULL_HANDLE; }
        *why = "vkCreateShaderModule failed";
        *refused = true;
        return false;
    }
    return true;
}

VkPipeline make_draw_pipeline(VkDevice dev, const DrawPipelineDesc &d, std::string *why) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, d.vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d.fs, "main", nullptr}};
    VkVertexInputBindingDescription vb{0, kVertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[kVertexAttribs];
    for (uint32_t i = 0; i < kVertexAttribs; i++) va[i] = {i, 0, (VkFormat)kVertexFormats[i], kVertexOffsets[i]};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = kVertexAttribs;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = (d.state >> 4) & 3u;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = VK_TRUE;   // the slope is dynamic: 0 except decals
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;   // ALWAYS when not testing: Metal's Always still writes when writes are on
    ds.depthWriteEnable = (d.state & 8u) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (VkCompareOp)(d.state & 7u);
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = d.depth_only ? 0u
                                     : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (d.blend) {   // RGB SrcAlpha/1-SrcAlpha, alpha One/1-SrcAlpha -> premultiplied coverage (the shaders write straight alpha)
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = d.layout;
    gi.renderPass = d.rp;
    gi.subpass = 0;
    VkPipeline out = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(dev, d.cache, 1, &gi, nullptr, &out);
    if (r != VK_SUCCESS) { *why = "vkCreateGraphicsPipelines failed: " + std::to_string((int)r); return VK_NULL_HANDLE; }
    return out;
}

std::vector<ListLine> parse_pipeline_lines(const std::string &list) {
    std::vector<ListLine> out;
    size_t pos = 0, lines = 0;
    while (pos < list.size() && lines++ < kMaxWarmLines) {
        size_t e = list.find('\n', pos);
        if (e == std::string::npos) e = list.size();
        const size_t len = e - pos, at = pos;
        pos = e + 1;
        if (len > kMaxWarmLineBytes) continue;
        const std::string line = list.substr(at, len);
        unsigned long long slots = 0;
        unsigned opts = 0, fmt = 0, blend = 0, depth = 0, flat = 0, depth_only = 0, table_shadow = 0, clip = 0, samples = 1, cut = 0;
        char tail = 0;
        const int got = std::sscanf(line.c_str(), "%llx %x %u %u %u %u %u %u %u %u %u %c", &slots, &opts, &fmt, &blend, &depth, &flat,
                                    &depth_only, &table_shadow, &clip, &samples, &cut, &tail);
        if ((got != 8 && got != 9 && got != 10 && got != 11) || blend > 1 || depth > 1 || flat > kSilhouetteCutout ||
            depth_only > kDepthOnlyPrepass || table_shadow > 1 || clip > 1 || (samples != 1 && samples != 2 && samples != 4) ||
            cut > 1 || fmt != (unsigned)kColorFormat)
            continue;
        DrawPacket d{};
        d.cc = fc_combiner_key{slots, opts, 0};
        d.blend = (uint8_t)blend; d.silhouette = (uint8_t)flat; d.depth_only = (uint8_t)depth_only; d.table_shadow = (uint8_t)table_shadow;
        d.clip_on = (uint8_t)clip;
        d.cutaway = (uint8_t)cut;
        out.push_back(ListLine{d, depth != 0, samples});
    }
    return out;
}
}
}
