#include "vk_pipelines.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "combiner.h"
#include "glsl_compiler.h"
#include "glsl_emitter.h"
#include "glsl_fixed.h"
#include "vk_layout.h"
#include "vos_log.h"
namespace ar {
namespace vk {
namespace {
const size_t kMaxWarmLines = 4096;     // parse_pipeline_lines reads at most this many lines
const size_t kMaxWarmLineBytes = 96;   // and skips longer lines unparsed (pipeline_line's lines fit 96 bytes)
const VkPipelineStageFlags kAttStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
const VkAccessFlags kAttAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
// One subpass, colour attachment 0 (when n_color) and depth attachment n_color (when depth). external: dependencies with
// the work before and after the pass on the same attachments (the external image's; the targets' passes have explicit
// barriers instead).
VkRenderPass make_pass(VkDevice dev, const VkAttachmentDescription *a, uint32_t n_color, bool depth, bool external) {
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{n_color, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = n_color;
    sp.pColorAttachments = n_color ? &cr : nullptr;
    sp.pDepthStencilAttachment = depth ? &dr : nullptr;
    const VkSubpassDependency deps[2] = {{VK_SUBPASS_EXTERNAL, 0, kAttStages, kAttStages, kAttAccess, kAttAccess, 0},
                                         {0, VK_SUBPASS_EXTERNAL, kAttStages, kAttStages, kAttAccess, kAttAccess, 0}};
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = n_color + (depth ? 1 : 0);
    ri.pAttachments = a;
    ri.subpassCount = 1;
    ri.pSubpasses = &sp;
    ri.dependencyCount = external ? 2 : 0;
    ri.pDependencies = external ? deps : nullptr;
    VkRenderPass rp = VK_NULL_HANDLE;
    return vkCreateRenderPass(dev, &ri, nullptr, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}
VkPipelineShaderStageCreateInfo stage_info(VkShaderStageFlagBits s, VkShaderModule m) {
    return {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, s, m, "main", nullptr};
}
VkShaderModule fixed_module(VkDevice dev, const std::string &src, GlslStage stage, const char *name) {
    std::vector<uint32_t> spv;
    std::string err;
    if (src.empty() || !compile_glsl(src, stage, &spv, &err)) { vos_log("AR_VULKAN %s: %s", name, src.empty() ? "no source" : err.c_str()); return VK_NULL_HANDLE; }
    VkShaderModule m = make_module(dev, spv);
    if (!m) vos_log("AR_VULKAN %s: vkCreateShaderModule failed", name);
    return m;
}
std::string fixed_source(const char *name) {
    const char *s = fixed_shader(name);
    return s ? std::string(s) : std::string();
}
}

// ---------------------------------------------------------------------------------------------------------------------
// State, keys and the list format
// ---------------------------------------------------------------------------------------------------------------------
uint8_t stencil_mode(const DrawPacket &d) {
    if (d.silhouette) return kStencilSilhouette;
    if (d.depth_only == kDepthOnlyFaint || d.table_shadow) return kStencilFaint;
    if (!d.depth_write) return kStencilKeep;
    return d.focus ? kStencilMarkFocus : kStencilClearFocus;
}
uint32_t stencil_ref(uint8_t mode) {
    switch (mode) {
    case kStencilMarkFocus: return kStencilDepthWritten | 1u;
    case kStencilClearFocus: return kStencilDepthWritten;
    default: return 0u;
    }
}
uint32_t state_key(VkCompareOp cmp, bool write, uint8_t stencil, VkCullModeFlags cull) {
    if (stencil == kStencilMarkFocus) stencil = kStencilClearFocus;
    return (uint32_t)cmp | (write ? 8u : 0u) | ((uint32_t)stencil << 4) | ((uint32_t)cull << 7);
}
uint32_t draw_state(const DrawPacket &d, bool pass_depth, bool dynamic_cull) {
    const VkCullModeFlags cull = dynamic_cull ? VK_CULL_MODE_NONE
                               : d.cull == FC_CULL_BACK ? VK_CULL_MODE_BACK_BIT : d.cull == FC_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT
                                                                                                          : VK_CULL_MODE_NONE;
    if (!pass_depth) return state_key(VK_COMPARE_OP_ALWAYS, false, kStencilKeep, cull);   // Metal's no_depth_state
    const bool test = d.silhouette ? true : d.depth_test != 0, write = d.silhouette ? false : d.depth_write != 0;
    const VkCompareOp cmp = d.silhouette ? VK_COMPARE_OP_LESS : test ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
    return state_key(cmp, write, stencil_mode(d), cull);
}
size_t PipeKeyHash::operator()(const PipeKey &k) const {
    uint64_t h = k.slots * 0x9E3779B97F4A7C15ull;
    const uint64_t f[] = {k.opts, (uint64_t)k.fmt, k.blend, k.depth, k.flat, k.depth_only, k.table_shadow, k.clip, k.samples, k.a2c, k.cut};
    for (uint64_t v : f) h = (h ^ v) * 0x100000001B3ull;
    return (size_t)(h ^ (h >> 29));
}
std::string pipeline_line(const PipeKey &k) {
    char b[kMaxWarmLineBytes];
    const int len = std::snprintf(b, sizeof b, "%016llx %x %u %u %u %u %u %u %u %u", (unsigned long long)k.slots, k.opts, (unsigned)k.fmt,
                                  (unsigned)k.blend, (unsigned)k.depth, (unsigned)k.flat, (unsigned)k.depth_only,
                                  (unsigned)k.table_shadow, (unsigned)k.clip, (unsigned)k.samples);
    // the 11th field (cut) is written only when it is 1, so every other line keeps the older format
    if (len > 0 && (size_t)len < sizeof b) std::snprintf(b + len, sizeof b - (size_t)len, k.cut ? " 1\n" : "\n");
    return b;
}
bool parse_pipeline_line(const std::string &line, PipeKey *out) {
    if (line.size() > kMaxWarmLineBytes) return false;
    unsigned long long slots = 0;
    unsigned opts = 0, fmt = 0, blend = 0, depth = 0, flat = 0, depth_only = 0, table_shadow = 0, clip = 0, samples = 1, cut = 0;
    char tail = 0;
    // 8 fields: no clip; 9: no samples (single sample); 10: no cutaway. A 12th field is a bad line.
    const int got = std::sscanf(line.c_str(), "%llx %x %u %u %u %u %u %u %u %u %u %c", &slots, &opts, &fmt, &blend, &depth, &flat,
                                &depth_only, &table_shadow, &clip, &samples, &cut, &tail);
    if ((got != 8 && got != 9 && got != 10 && got != 11) || blend > 1 || depth > 1 || flat > kSilhouetteCutout ||
        depth_only > kDepthOnlyPrepass || table_shadow > 1 || clip > 1 || (samples != 1 && samples != 2 && samples != 4) || cut > 1)
        return false;
    PipeKey k;
    k.slots = slots; k.opts = opts; k.fmt = (VkFormat)fmt;
    k.blend = (uint8_t)blend; k.depth = (uint8_t)depth; k.flat = (uint8_t)flat; k.depth_only = (uint8_t)depth_only;
    k.table_shadow = (uint8_t)table_shadow; k.clip = (uint8_t)clip; k.samples = (uint8_t)samples; k.a2c = 0; k.cut = (uint8_t)cut;
    *out = k;
    return true;
}
std::vector<ListLine> parse_pipeline_lines(const std::string &list) {
    std::vector<ListLine> out;
    size_t pos = 0, lines = 0;
    while (pos < list.size() && lines++ < kMaxWarmLines) {
        size_t e = list.find('\n', pos);
        if (e == std::string::npos) e = list.size();
        const size_t len = e - pos, at = pos;
        pos = e + 1;
        PipeKey k;
        if (len > kMaxWarmLineBytes || !parse_pipeline_line(list.substr(at, len), &k) || k.fmt != kColorFormat) continue;
        DrawPacket d{};
        d.cc = fc_combiner_key{k.slots, k.opts, 0};
        d.blend = k.blend; d.silhouette = k.flat; d.depth_only = k.depth_only; d.table_shadow = k.table_shadow;
        d.clip_on = k.clip; d.cutaway = k.cut;
        d.depth_test = 1; d.depth_write = k.blend ? 0 : 1; d.cull = FC_CULL_BACK;   // the likeliest draw state
        out.push_back(ListLine{d, k.depth != 0, k.samples});
    }
    return out;
}
DrawPacket pipeline_form(const DrawPacket &d) {
    if (!d.silhouette) return d;
    static const fc_combiner_key flat = normalize_combiner(fc_combiner_key{((uint64_t)FC_SRC_SHADE << 12) | ((uint64_t)FC_SRC_SHADE << 28), 0, 0});
    DrawPacket c = d;
    c.cc = flat;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------------------------------------------------
VkShaderModule make_module(VkDevice dev, const std::vector<uint32_t> &spv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * sizeof(uint32_t);
    ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(dev, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
VkRenderPass make_target_pass(VkDevice dev, const TargetPassDesc &d) {
    const VkAttachmentLoadOp load = d.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    const VkImageLayout cl = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dl = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (d.samples <= 1) {
        VkAttachmentDescription a[2] = {};
        a[0].format = kColorFormat;
        a[0].samples = VK_SAMPLE_COUNT_1_BIT;
        a[0].loadOp = load;
        a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[0].initialLayout = a[0].finalLayout = cl;
        a[1].format = kDepthFormat;
        a[1].samples = VK_SAMPLE_COUNT_1_BIT;
        a[1].loadOp = a[1].stencilLoadOp = load;
        a[1].storeOp = a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[1].initialLayout = a[1].finalLayout = dl;
        return make_pass(dev, a, 1, true, false);   // the barriers before (flush) and after (resolve, read-back) are explicit
    }
    if (!d.create2) return VK_NULL_HANDLE;
    const VkSampleCountFlagBits ms = (VkSampleCountFlagBits)d.samples;
    const VkAttachmentStoreOp ms_store = d.transient ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentDescription2 a[4];
    for (VkAttachmentDescription2 &x : a) x = VkAttachmentDescription2{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    a[0].format = kColorFormat; a[0].samples = ms;
    a[0].loadOp = load; a[0].storeOp = ms_store;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = a[0].finalLayout = cl;
    a[1].format = kDepthFormat; a[1].samples = ms;
    a[1].loadOp = a[1].stencilLoadOp = load; a[1].storeOp = a[1].stencilStoreOp = ms_store;
    a[1].initialLayout = a[1].finalLayout = dl;
    a[2].format = kColorFormat; a[2].samples = VK_SAMPLE_COUNT_1_BIT;   // the resolves overwrite every texel
    a[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[2].initialLayout = a[2].finalLayout = cl;
    a[3].format = kDepthFormat; a[3].samples = VK_SAMPLE_COUNT_1_BIT;
    a[3].loadOp = a[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[3].stencilStoreOp = d.stencil_mode != VK_RESOLVE_MODE_NONE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[3].initialLayout = a[3].finalLayout = dl;
    VkAttachmentReference2 r[4];
    for (uint32_t i = 0; i < 4; i++) {
        r[i] = VkAttachmentReference2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        r[i].attachment = i;
        r[i].layout = (i & 1) ? dl : cl;
    }
    VkSubpassDescriptionDepthStencilResolve dsr{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    dsr.depthResolveMode = d.depth_mode;
    dsr.stencilResolveMode = d.stencil_mode;
    dsr.pDepthStencilResolveAttachment = &r[3];
    VkSubpassDescription2 sp{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sp.pNext = &dsr;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &r[0];
    sp.pResolveAttachments = &r[2];
    sp.pDepthStencilAttachment = &r[1];
    VkRenderPassCreateInfo2 ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    ri.attachmentCount = 4;
    ri.pAttachments = a;
    ri.subpassCount = 1;
    ri.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    return d.create2(dev, &ri, nullptr, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
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
    return make_pass(dev, a, 1, df != VK_FORMAT_UNDEFINED, true);
}
VkRenderPass make_map_pass(VkDevice dev) {
    VkAttachmentDescription a{};
    a.format = kMapFormat;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    return make_pass(dev, &a, 0, true, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// The resolve
// ---------------------------------------------------------------------------------------------------------------------
bool make_resolve_shaders(VkDevice dev, VkShaderModule out[3], std::string *err) {
    // The resolve: a full-screen triangle; each fragment copies its source texel 1:1 (glsl_fixed.h). The targets are 2D
    // arrays (RESOLVE_ARRAY); the third module also writes the source depth (RESOLVE_DEPTH).
    const std::string frag = fixed_source("resolve.frag"), vert = fixed_source("fullscreen.vert");
    if (frag.empty() || vert.empty()) { *err = "no resolve shader source"; return false; }
    std::string color_src = frag, depth_src = frag;
    color_src.insert(color_src.find('\n') + 1, "#define RESOLVE_ARRAY 1\n");
    depth_src.insert(depth_src.find('\n') + 1, "#define RESOLVE_ARRAY 1\n#define RESOLVE_DEPTH 1\n");
    const std::string srcs[3] = {vert, color_src, depth_src};
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
    const VkPipelineShaderStageCreateInfo stages[2] = {stage_info(VK_SHADER_STAGE_VERTEX_BIT, vs), stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
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

// ---------------------------------------------------------------------------------------------------------------------
// Draw pipelines
// ---------------------------------------------------------------------------------------------------------------------
bool make_combiner_shaders(VkDevice dev, const DrawPacket &draw, bool a2c, VkShaderModule *vs_out, VkShaderModule *fs_out,
                           std::string *why, bool *refused) {
    const DrawPacket d = pipeline_form(draw);
    CombinerIR ir = decode_combiner(d.cc);
    ir.flat_color = d.silhouette != kSilhouetteNone;
    ir.flat_cutout = d.silhouette == kSilhouetteCutout;
    ir.depth_fill = d.depth_only;
    ir.table_shadow = d.table_shadow != 0;
    ir.clip = d.clip_on != 0;
    ir.alpha_coverage = a2c;
    ir.layer_out = true;   // Metal: samples == 1 || layered_msaa; Vulkan rasterizes multisample layers (gl_Layer) everywhere
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
VkPipeline make_draw_pipeline(const BuildEnv &env, VkShaderModule vs, VkShaderModule fs, const PipeKey &k, uint32_t state, std::string *why) {
    const uint32_t samples = k.samples == 2 || k.samples == 4 ? k.samples : 1;
    if (!env.rp[samples]) { *why = "no render pass for " + std::to_string(samples) + " samples"; return VK_NULL_HANDLE; }
    const VkPipelineShaderStageCreateInfo stages[2] = {stage_info(VK_SHADER_STAGE_VERTEX_BIT, vs), stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
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
    rs.cullMode = (state >> 7) & 3u;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = VK_TRUE;   // the slope is dynamic: 0 except decals
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
    ms.alphaToCoverageEnable = k.a2c ? VK_TRUE : VK_FALSE;
    ms.alphaToOneEnable = k.a2c && env.alpha_to_one ? VK_TRUE : VK_FALSE;   // Metal sets both (an optional Vulkan feature)
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;   // ALWAYS when not testing: Metal's Always still writes when writes are on
    ds.depthWriteEnable = (state & 8u) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (VkCompareOp)(state & 7u);
    const uint8_t mode = (uint8_t)((state >> 4) & 7u);
    if (mode != kStencilKeep) {   // metal_device.mm depth_state; the reference is dynamic (stencil_ref)
        VkStencilOpState st{};
        const bool tests = mode == kStencilSilhouette || mode == kStencilFaint;
        st.compareOp = tests ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_ALWAYS;
        st.failOp = VK_STENCIL_OP_KEEP;
        st.depthFailOp = VK_STENCIL_OP_KEEP;
        st.passOp = mode == kStencilSilhouette ? VK_STENCIL_OP_INCREMENT_AND_CLAMP : mode == kStencilFaint ? VK_STENCIL_OP_KEEP
                                                                                                         : VK_STENCIL_OP_REPLACE;
        st.compareMask = mode == kStencilSilhouette ? kStencilFocusBits : mode == kStencilFaint ? kStencilDepthWritten : 0xFFu;
        st.writeMask = mode == kStencilFaint ? 0u : 0xFFu;
        ds.stencilTestEnable = VK_TRUE;
        ds.front = ds.back = st;
    }
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = k.depth_only ? 0u
                                     : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (k.blend) {   // RGB SrcAlpha/1-SrcAlpha, alpha One/1-SrcAlpha -> premultiplied coverage (the shaders write straight alpha)
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[5] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE_EXT};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = env.dynamic_cull ? 5 : 4;
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
    gi.layout = env.layout;
    gi.renderPass = env.rp[samples];
    gi.subpass = 0;
    VkPipeline out = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(env.dev, env.cache, 1, &gi, nullptr, &out);
    if (r != VK_SUCCESS) { *why = "vkCreateGraphicsPipelines failed: " + std::to_string((int)r); return VK_NULL_HANDLE; }
    return out;
}
Built build_warm(const BuildEnv &env, const WarmJob &j) {
    Built b;
    b.key = j.key;
    if (j.vs && j.fs) {
        b.vs = j.vs; b.fs = j.fs;
    } else {
        if (!make_combiner_shaders(env.dev, j.d, j.key.pk.a2c != 0, &b.vs, &b.fs, &b.why, &b.refused)) return b;
        b.own_modules = true;
    }
    b.pipeline = make_draw_pipeline(env, b.vs, b.fs, j.key.pk, j.key.state, &b.why);
    if (!b.pipeline) {
        b.refused = true;
        if (b.own_modules) {
            vkDestroyShaderModule(env.dev, b.vs, nullptr);
            vkDestroyShaderModule(env.dev, b.fs, nullptr);
            b.own_modules = false;
        }
        b.vs = b.fs = VK_NULL_HANDLE;
        return b;
    }
    b.ok = true;
    return b;
}

// ---------------------------------------------------------------------------------------------------------------------
// The fixed passes
// ---------------------------------------------------------------------------------------------------------------------
VkPipeline make_compute_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, const char *shader, const char *defines) {
    std::string src = fixed_source(shader);
    if (!src.empty() && defines && *defines) src.insert(src.find('\n') + 1, defines);
    VkShaderModule m = fixed_module(dev, src, GlslStage::compute, shader);
    if (!m || !layout) { if (m) vkDestroyShaderModule(dev, m, nullptr); return VK_NULL_HANDLE; }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage_info(VK_SHADER_STAGE_COMPUTE_BIT, m);
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(dev, cache, 1, &ci, nullptr, &p) != VK_SUCCESS) { p = VK_NULL_HANDLE; vos_log("AR_VULKAN %s: pipeline failed", shader); }
    vkDestroyShaderModule(dev, m, nullptr);
    return p;
}
VkPipeline make_mip_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout) {
    return make_compute_pipeline(dev, cache, layout, "mip_gamma.comp");
}
FixedPipes make_fixed_pipelines(VkDevice dev, VkPipelineCache cache, const FixedLayouts &l) {
    FixedPipes out;
    out.ao = make_compute_pipeline(dev, cache, l.ao, "ao_fill.comp", l.ao_rgba8 ? "#define AO_RGBA8 1\n" : "");
    out.composite_fs = fixed_module(dev, fixed_source("composite.frag"), GlslStage::fragment, "composite.frag");
    if (out.composite_fs && l.fullscreen_vs && l.composite && l.composite_pass) {
        out.composite = make_resolve_pipeline(dev, cache, l.composite, l.composite_pass, l.fullscreen_vs, out.composite_fs, false);
        if (!out.composite) vos_log("AR_VULKAN composite pipeline failed");
    }
    VkShaderModule vs = fixed_module(dev, fixed_source("depth.vert"), GlslStage::vertex, "depth.vert");
    VkShaderModule fs = fixed_module(dev, fixed_source("depth_cut.frag"), GlslStage::fragment, "depth_cut.frag");
    if (vs && l.depth && l.map_pass) {
        VkVertexInputBindingDescription vb{0, kVertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription va[2] = {{0, 0, (VkFormat)kVertexFormats[0], kVertexOffsets[0]},
                                                         {1, 0, (VkFormat)kVertexFormats[1], kVertexOffsets[1]}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = va;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;   // Metal's depth_less_write_ (the maps are cleared to 1, not reverse-Z)
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 3;
        dy.pDynamicStates = dyn;
        for (int cut = 0; cut < 2; cut++) {
            if (cut && !fs) continue;
            for (int clamp = 0; clamp < 2; clamp++) {
                const VkPipelineShaderStageCreateInfo stages[2] = {stage_info(VK_SHADER_STAGE_VERTEX_BIT, vs),
                                                                   stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, fs)};
                VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                rs.depthClampEnable = clamp ? VK_TRUE : VK_FALSE;   // the key light's map clamps instead of clipping
                rs.polygonMode = VK_POLYGON_MODE_FILL;
                rs.cullMode = VK_CULL_MODE_NONE;
                rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                rs.depthBiasEnable = VK_TRUE;
                rs.lineWidth = 1.f;
                VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
                gi.stageCount = cut ? 2 : 1;
                gi.pStages = stages;
                gi.pVertexInputState = &vi;
                gi.pInputAssemblyState = &ia;
                gi.pViewportState = &vp;
                gi.pRasterizationState = &rs;
                gi.pMultisampleState = &ms;
                gi.pDepthStencilState = &ds;
                gi.pColorBlendState = &cb;
                gi.pDynamicState = &dy;
                gi.layout = l.depth;
                gi.renderPass = l.map_pass;
                if (vkCreateGraphicsPipelines(dev, cache, 1, &gi, nullptr, &out.depth[cut][clamp]) != VK_SUCCESS) {
                    out.depth[cut][clamp] = VK_NULL_HANDLE;
                    vos_log("AR_VULKAN depth pipeline (cut %d, clamp %d) failed", cut, clamp);
                }
            }
        }
    }
    if (vs) vkDestroyShaderModule(dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(dev, fs, nullptr);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// WarmPool
// ---------------------------------------------------------------------------------------------------------------------
void WarmPool::start(Compile compile) {
    compile_ = std::move(compile);
    stop_ = false;
    for (int i = 0; i < kWorkers; i++) workers_.emplace_back([this] { loop(); });
}
void WarmPool::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        stop_ = true;
        hold_bg_ = hold_promo_ = hold_compile_ = false;
        for (const Entry &e : queue_) keys_.erase(e.job.key);
        queue_.clear();
        promoting_ = 0;
    }
    cv_.notify_all();
    for (std::thread &t : workers_) if (t.joinable()) t.join();
    workers_.clear();
}
bool WarmPool::pickable(size_t *at) const {
    for (size_t i = 0; i < queue_.size(); i++) if (queue_[i].kind == kInline) { *at = i; return true; }
    if (!hold_promo_)
        for (size_t i = 0; i < queue_.size(); i++) if (queue_[i].promoted) { *at = i; return true; }
    if (!hold_bg_) {
        for (size_t i = 0; i < queue_.size(); i++) if (queue_[i].kind == kUrgent) { *at = i; return true; }
        if (!queue_.empty()) { *at = 0; return true; }
    }
    return false;
}
void WarmPool::loop() {
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
        size_t at = 0;
        cv_.wait(l, [&] { return stop_ || pickable(&at); });
        if (stop_) return;   // stop() dropped the queue
        Entry e = std::move(queue_[at]);
        queue_.erase(queue_.begin() + (ptrdiff_t)at);
        execute(std::move(e), l, true);
    }
}
void WarmPool::execute(Entry e, std::unique_lock<std::mutex> &l, bool honour_compile_hold) {
    keys_[e.job.key] = kRunning;
    running_++;
    const bool lane = e.kind == kBackground && !e.promoted;   // Metal's lanes (a promotion runs on its own block)
    if (lane) { bg_running_++; peak_ = std::max(peak_, bg_running_); }
    if (e.kind != kInline) {
        compiles_++;
        if (honour_compile_hold) cv_.wait(l, [&] { return !hold_compile_ || stop_; });
    }
    l.unlock();
    Built b = compile_(e.job);
    l.lock();
    running_--;
    if (lane) bg_running_--;
    if (e.promoted && promoting_) promoting_--;
    keys_[e.job.key] = kDone;
    done_.push_back(std::move(b));
    cv_.notify_all();
}
bool WarmPool::enqueue(const WarmJob &job, Kind kind) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (stop_ || workers_.empty() || keys_.count(job.key)) return false;
        keys_[job.key] = kQueued;
        queue_.push_back(Entry{job, kind, false});
    }
    cv_.notify_all();
    return true;
}
bool WarmPool::claim(const WarmKey &key, WarmJob *out) {
    std::lock_guard<std::mutex> g(m_);
    for (size_t i = 0; i < queue_.size(); i++) {
        if (!(queue_[i].job.key == key)) continue;
        *out = queue_[i].job;
        if (queue_[i].promoted && promoting_) promoting_--;
        queue_.erase(queue_.begin() + (ptrdiff_t)i);
        keys_.erase(key);
        return true;
    }
    return false;
}
bool WarmPool::promote(const WarmKey &key) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (promoting_ >= kMaxPromotions) return false;
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Entry &e) { return e.job.key == key; });
        if (it == queue_.end() || it->kind != kBackground || it->promoted) return false;
        it->promoted = true;
        promoting_++;
    }
    cv_.notify_all();
    return true;
}
bool WarmPool::busy_pipe(const PipeKey &pk) const {
    std::lock_guard<std::mutex> g(m_);
    for (const auto &kv : keys_) if (kv.first.pk == pk) return true;
    return false;
}
bool WarmPool::promote_pipe(const PipeKey &pk) {
    WarmKey key;
    {
        std::lock_guard<std::mutex> g(m_);
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Entry &e) { return e.job.key.pk == pk && e.kind == kBackground && !e.promoted; });
        if (it == queue_.end()) return false;
        key = it->job.key;
    }
    return promote(key);
}
bool WarmPool::busy(const WarmKey &key) const {
    std::lock_guard<std::mutex> g(m_);
    return keys_.count(key) != 0;
}
bool WarmPool::done(const WarmKey &key) const {
    std::lock_guard<std::mutex> g(m_);
    auto it = keys_.find(key);
    return it != keys_.end() && it->second == kDone;
}
void WarmPool::wait(const WarmKey &key) {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [&] { auto it = keys_.find(key); return it == keys_.end() || it->second == kDone; });
}
bool WarmPool::wait_all(double timeout_s) {
    std::unique_lock<std::mutex> l(m_);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(timeout_s, 0.0));
    return cv_.wait_until(l, end, [&] { return queue_.empty() && running_ == 0; });
}
void WarmPool::run(const std::vector<WarmJob> &jobs) {
    std::vector<WarmKey> mine;
    {
        std::lock_guard<std::mutex> g(m_);
        for (const WarmJob &j : jobs) {
            if (keys_.count(j.key)) continue;
            keys_[j.key] = kQueued;
            queue_.push_back(Entry{j, kInline, false});
            mine.push_back(j.key);
        }
    }
    cv_.notify_all();
    std::unique_lock<std::mutex> l(m_);
    for (;;) {   // the calling thread compiles too (the workers may be held or busy)
        auto it = std::find_if(queue_.begin(), queue_.end(), [](const Entry &e) { return e.kind == kInline; });
        if (it == queue_.end()) break;
        Entry e = std::move(*it);
        queue_.erase(it);
        execute(std::move(e), l, false);
    }
    cv_.wait(l, [&] {
        for (const WarmKey &k : mine) {
            auto it = keys_.find(k);
            if (it != keys_.end() && it->second != kDone) return false;
        }
        return true;
    });
}
void WarmPool::collect(const std::function<void(const Built &)> &f) {
    std::vector<Built> out;
    {
        std::lock_guard<std::mutex> g(m_);
        out.swap(done_);
        for (const Built &b : out) keys_.erase(b.key);
    }
    for (const Built &b : out) f(b);
}
size_t WarmPool::pending() const { std::lock_guard<std::mutex> g(m_); return keys_.size(); }
uint32_t WarmPool::running() const { std::lock_guard<std::mutex> g(m_); return bg_running_; }
uint32_t WarmPool::peak() const { std::lock_guard<std::mutex> g(m_); return peak_; }
uint64_t WarmPool::compiles() const { std::lock_guard<std::mutex> g(m_); return compiles_; }
void WarmPool::count_compile() { std::lock_guard<std::mutex> g(m_); compiles_++; }
void WarmPool::hold_background(bool hold) { { std::lock_guard<std::mutex> g(m_); hold_bg_ = hold; } cv_.notify_all(); }
void WarmPool::hold_promoted(bool hold) { { std::lock_guard<std::mutex> g(m_); hold_promo_ = hold; } cv_.notify_all(); }
void WarmPool::hold_compiles(bool hold) { { std::lock_guard<std::mutex> g(m_); hold_compile_ = hold; } cv_.notify_all(); }
}
}
