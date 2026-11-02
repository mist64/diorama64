#include "vulkan_device.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "combiner.h"
#include "glsl_compiler.h"
#include "vk_device.h"
#include "vk_layout.h"
#include "vos_log.h"
// Init, targets, recording, the resolve, frames and read-back. The class and the Vulkan-specific notes: vk_device.h.
namespace ar {
namespace vkd {
using vk::image_barrier;
using vk::kColorFormat;
using vk::kDepthFormat;
using vk::ResolveU;
static_assert(kVertexFormatR32G32B32Sfloat == VK_FORMAT_R32G32B32_SFLOAT, "vk_layout.h");
static_assert(kVertexFormatR32G32Sfloat == VK_FORMAT_R32G32_SFLOAT, "vk_layout.h");
static_assert(kVertexFormatR8G8B8A8Unorm == VK_FORMAT_R8G8B8A8_UNORM, "vk_layout.h");
static_assert(sizeof(fc_vertex) == kVertexStride, "fc_vertex is the 32-byte vertex");
namespace {
bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
VkImageAspectFlags depth_aspects(VkFormat f) {
    return VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}
}

// ---------------------------------------------------------------------------------------------------------------------
// Construction and teardown
// ---------------------------------------------------------------------------------------------------------------------
VulkanDevice::VulkanDevice(const VulkanDeviceInit &init)
    : inst_(init.instance), phys_(init.physical), dev_(init.device), qfam_(init.queue_family), queue_(init.queue),
      gipa_(init.get_instance_proc_addr), cache_path_(init.pipeline_cache_path), exts_(init.device_extensions),
      alpha_to_one_(init.alpha_to_one) {
    glsl_compiler_init();   // before any compile, on this thread or the pool's
    ok_ = init_device();
    if (!ok_) vos_log("AR_VULKAN init failed: no render device");
    else vos_log("AR_VULKAN device %s ready (ring alignment %llu, timestamps %d, samples %u, transient %d, dynamic cull %d, alpha-to-one %d)",
                 props_.deviceName, (unsigned long long)ring_align_, timestamps_ ? 1 : 0, supported_samples(4, 1),
                 memoryless_msaa() ? 1 : 0, set_cull_ ? 1 : 0, alpha_to_one_ ? 1 : 0);
    done_thread_ = std::thread([this] { completion_loop(); });
    if (ok_) pool_.start([this](const vk::WarmJob &j) { return vk::build_warm(env_, j); });
}
VulkanDevice::~VulkanDevice() {
    wait_idle();
    pool_.stop();   // held jobs are dropped; a running one finishes
    {
        std::lock_guard<std::mutex> g(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (done_thread_.joinable()) done_thread_.join();
    if (dev_) {   // null only when init failed before touching the device (create_vulkan_device returns null)
        vkDeviceWaitIdle(dev_);
        collect_warmed();
        if (fixed_job_.valid()) (void)fixed_pipes();   // the off-thread compiles end before glslang does
        if (mip_job_.valid() && !mip_taken_) { mip_pipe_ = mip_job_.get(); mip_taken_ = true; }
        for (auto &kv : resolve_jobs_) if (VkPipeline p = kv.second.get()) vkDestroyPipeline(dev_, p, nullptr);
        resolve_jobs_.clear();
        save_pipeline_cache();
        destroy_all();
    }
    glsl_compiler_finish();
}
bool VulkanDevice::init_device() {
    if (!dev_ || !phys_ || !queue_) return false;
    vkGetPhysicalDeviceProperties(phys_, &props_);
    res_.init(dev_, phys_);
    ring_align_ = std::max<VkDeviceSize>({256, props_.limits.minUniformBufferOffsetAlignment, props_.limits.minStorageBufferOffsetAlignment});
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qf.data());
    const uint32_t ts_bits = qfam_ < qn ? qf[qfam_].timestampValidBits : 0;
    timestamps_ = ts_bits > 0 && props_.limits.timestampPeriod > 0.f;
    timestamp_mask_ = ts_bits >= 64 ? ~0ull : ((1ull << ts_bits) - 1);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys_, kDepthFormat, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vos_log("AR_VULKAN D32_SFLOAT_S8_UINT is not a depth attachment format here");
        return false;
    }
    depth_sampled_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    vkGetPhysicalDeviceFormatProperties(phys_, kPlaceholderDepth, &fp);
    depth_linear_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!depth_linear_) vos_log("AR_VULKAN D32_SFLOAT has no linear filtering here: the depth maps are sampled nearest");
    vkGetPhysicalDeviceFormatProperties(phys_, kColorFormat, &fp);
    storage_rgba8_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R8_UNORM, &fp);   // the AO volume: R8 storage, else RGBA8
    ao_rgba8_ = !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    vos_log("AR_VULKAN ao_volume=%s", ao_rgba8_ ? "rgba8" : "r8");
    fixed_layouts_.ao_rgba8 = ao_rgba8_;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam_;
    if (vkCreateCommandPool(dev_, &pci, nullptr, &cmd_pool_) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev_, &ai, &once_cmd_) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev_, &fci, nullptr, &once_fence_) != VK_SUCCESS) return false;
    for (Frame &f : frames_) {
        if (vkAllocateCommandBuffers(dev_, &ai, &f.cmd) != VK_SUCCESS) return false;
        if (vkCreateFence(dev_, &fci, nullptr, &f.fence) != VK_SUCCESS) return false;
        if (timestamps_) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (vkCreateQueryPool(dev_, &qi, nullptr, &f.queries) != VK_SUCCESS) return false;
        }
    }
    load_pipeline_cache();

    // samplers: Metal's linear_clamp_ and compare sampler (linear, clamp, LESS_OR_EQUAL); nearest_ reads by texel
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev_, &si, nullptr, &linear_clamp_) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev_, &si, nullptr, &nearest_) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = depth_linear_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(dev_, &si, nullptr, &compare_sampler_) != VK_SUCCESS) return false;
    if (!make_layouts()) return false;

    // the extensions the creator enabled (VulkanDeviceInit::device_extensions)
    if (has_ext(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))
        create_rp2_ = (PFN_vkCreateRenderPass2KHR)vkGetDeviceProcAddr(dev_, "vkCreateRenderPass2KHR");
    if (has_ext(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME))
        set_cull_ = (PFN_vkCmdSetCullModeEXT)vkGetDeviceProcAddr(dev_, "vkCmdSetCullModeEXT");
    env_.alpha_to_one = alpha_to_one_;
    env_.dev = dev_;
    env_.cache = cache_;
    env_.layout = pipe_layout_;
    env_.dynamic_cull = set_cull_ != nullptr;
    if (!(env_.rp[1] = target_pass(1, true, false))) return false;
    detect_msaa(false);
    ring_.init(&res_, ring_align_, [this](Buffer b) { retire([this, b]() mutable { res_.destroy_buffer(b); }); });
    if (!make_placeholders() || !make_resolve_shaders()) return false;
    fixed_layouts_.fullscreen_vs = resolve_vs_;   // the composite's vertex shader too
    fixed_layouts_.composite = composite_layout_;
    return (fixed_layouts_.composite_pass = resolve_render_pass(kColorFormat, VK_FORMAT_UNDEFINED)) != VK_NULL_HANDLE;
}
// supported_samples' inputs: the sample counts both attachments take, and a depth resolve mode (MAX: reverse-Z's nearest
// sample; any: the first the device offers, for host tests on lavapipe, which has SAMPLE_ZERO only).
void VulkanDevice::detect_msaa(bool any_depth_mode) {
    const VkPhysicalDeviceLimits &lim = props_.limits;
    sample_counts_ = lim.framebufferColorSampleCounts & lim.framebufferDepthSampleCounts & lim.framebufferStencilSampleCounts;
    VkImageFormatProperties ip;
    if (vkGetPhysicalDeviceImageFormatProperties(phys_, kColorFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ip) == VK_SUCCESS)
        sample_counts_ &= ip.sampleCounts;
    if (vkGetPhysicalDeviceImageFormatProperties(phys_, kDepthFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &ip) == VK_SUCCESS)
        sample_counts_ &= ip.sampleCounts;
    depth_resolve_ = stencil_resolve_ = VK_RESOLVE_MODE_NONE;
    const bool dsr_ext = has_ext(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
    PFN_vkGetPhysicalDeviceProperties2 props2 = nullptr;
    if (gipa_) {
        props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa_(inst_, "vkGetPhysicalDeviceProperties2");
        if (!props2) props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa_(inst_, "vkGetPhysicalDeviceProperties2KHR");
    }
    if (create_rp2_ && dsr_ext && props2) {
        VkPhysicalDeviceDepthStencilResolveProperties dsr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dsr;
        props2(phys_, &p2);
        VkResolveModeFlagBits dm = VK_RESOLVE_MODE_NONE;
        if (dsr.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT) dm = VK_RESOLVE_MODE_MAX_BIT;
        else if (any_depth_mode)
            for (VkResolveModeFlagBits m : {VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, VK_RESOLVE_MODE_MIN_BIT})
                if (!dm && (dsr.supportedDepthResolveModes & m)) dm = m;
        // the stencil is not resolved (Metal discards it) where the modes may differ; else it takes the depth's mode
        VkResolveModeFlagBits sm = VK_RESOLVE_MODE_NONE;
        if (dm && !dsr.independentResolveNone) {
            if (dsr.supportedStencilResolveModes & dm) sm = dm;
            else dm = VK_RESOLVE_MODE_NONE;
        }
        depth_resolve_ = dm;
        stencil_resolve_ = sm;
    }
    static bool modes_logged = false;
    if (!modes_logged && create_rp2_ && dsr_ext && props2) {
        modes_logged = true;
        VkPhysicalDeviceDepthStencilResolveProperties lp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 lp2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        lp2.pNext = &lp;
        props2(phys_, &lp2);
        vos_log("AR_VULKAN resolve_modes depth=0x%x stencil=0x%x independent_none=%d independent=%d", (unsigned)lp.supportedDepthResolveModes,
                (unsigned)lp.supportedStencilResolveModes, (int)lp.independentResolveNone, (int)lp.independentResolve);
    }
    static bool logged = false;
    if (!depth_resolve_ && !logged) { vos_log("AR_VULKAN depth_resolve=0"); logged = true; }
    for (uint32_t s : {2u, 4u})
        if (depth_resolve_ && (sample_counts_ & s) && !env_.rp[s]) env_.rp[s] = target_pass(s, true, false);
}
bool VulkanDevice::has_ext(const char *name) const {
    for (const std::string &e : exts_) if (e == name) return true;
    return false;
}
void VulkanDevice::accept_any_depth_resolve() {
    if (ok_ && !depth_resolve_) detect_msaa(true);
}
bool VulkanDevice::make_layouts() {
    auto set_layout = [&](const std::vector<std::pair<VkDescriptorType, VkShaderStageFlags>> &b, VkDescriptorSetLayout *out) {
        std::vector<VkDescriptorSetLayoutBinding> v(b.size());
        for (uint32_t i = 0; i < b.size(); i++) v[i] = VkDescriptorSetLayoutBinding{i, b[i].first, 1, b[i].second, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)v.size();
        li.pBindings = v.data();
        return vkCreateDescriptorSetLayout(dev_, &li, nullptr, out) == VK_SUCCESS;
    };
    auto pipe_layout = [&](std::initializer_list<VkDescriptorSetLayout> sets, const VkPushConstantRange *pc, VkPipelineLayout *out) {
        std::vector<VkDescriptorSetLayout> v(sets);
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = (uint32_t)v.size();
        pli.pSetLayouts = v.data();
        pli.pushConstantRangeCount = pc ? 1 : 0;
        pli.pPushConstantRanges = pc;
        return vkCreatePipelineLayout(dev_, &pli, nullptr, out) == VK_SUCCESS;
    };
    const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, fr = VK_SHADER_STAGE_FRAGMENT_BIT,
                             vx = VK_SHADER_STAGE_VERTEX_BIT, cs = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorType ubd = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, sb = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           cis = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, si = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    // glsl_emitter.cpp's prelude (vk_layout.h): set 0 the uniforms and buffers, set 1 bindings 0..7 the textures
    static_assert(kSetUniforms == 0 && kSetTextures == 1, "set order");
    static_assert(kBindView == 0 && kBindDraw == 1 && kBindLight == 2 && kBindNormals == 3 && kBindPrevPos == 4, "set 0 order");
    static_assert(kBindTex0 == 0 && kBindWaterFoot == 7, "set 1 is bindings 0..7");
    if (!set_layout({{ubd, vf}, {ubd, vf}, {ubd, vf}, {sb, vf}, {sb, vf}}, &set0_layout_) ||
        !set_layout(std::vector<std::pair<VkDescriptorType, VkShaderStageFlags>>(8, {cis, fr}), &set1_layout_) ||
        !set_layout({{cis, fr}, {cis, fr}}, &resolve_set_layout_) ||
        // glsl_fixed.h: depth.vert (M, DepthU, PrevPos) + depth_cut.frag (tex0), ao_fill.comp, mip_gamma.comp
        !set_layout({{ubd, vx}, {ubd, vx}, {sb, vx}}, &depth_set0_layout_) || !set_layout({{cis, fr}}, &depth_set1_layout_) ||
        !set_layout({{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cs}, {cis, cs}, {si, cs}}, &ao_set_layout_) ||
        !set_layout({{cis, cs}, {si, cs}}, &mip_set_layout_))
        return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResolveU)};
    return pipe_layout({set0_layout_, set1_layout_}, nullptr, &pipe_layout_) &&
           pipe_layout({resolve_set_layout_}, &pr, &resolve_layout_) &&
           pipe_layout({depth_set0_layout_, depth_set1_layout_}, nullptr, &fixed_layouts_.depth) &&
           pipe_layout({ao_set_layout_}, nullptr, &fixed_layouts_.ao) && pipe_layout({mip_set_layout_}, nullptr, &fixed_layouts_.mip) &&
           // composite.frag: under, over (the resolve's two combined image samplers)
           pipe_layout({resolve_set_layout_}, nullptr, &composite_layout_) &&
           (fixed_layouts_.map_pass = vk::make_map_pass(dev_)) != VK_NULL_HANDLE;
}
// white (texture id 0 and unknown ids), the depth maps' stand-in (depth 1: "nothing above"), AO 1, a black env cube
bool VulkanDevice::make_placeholders() {
    const uint8_t white[4] = {255, 255, 255, 255};
    const uint8_t one = 255;
    const uint16_t black[6][4] = {{0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00},
                                  {0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00}};
    const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto desc = [&](VkImageType t, VkFormat f, uint32_t layers, VkImageCreateFlags fl) {
        vk::ImageDesc d;
        d.type = t; d.format = f; d.layers = layers; d.usage = u; d.flags = fl;
        return d;
    };
    if (!res_.make_image(desc(VK_IMAGE_TYPE_2D, kColorFormat, 1, 0), &white_) ||
        !res_.make_image(desc(VK_IMAGE_TYPE_2D, kPlaceholderDepth, 1, 0), &depth_ph_) ||
        !res_.make_image(desc(VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, 1, 0), &ao_ph_) ||
        !res_.make_image(desc(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT), &env_ph_))
        return false;
    white_.view = res_.make_view(white_.img, VK_IMAGE_VIEW_TYPE_2D, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
    depth_ph_.view = res_.make_view(depth_ph_.img, VK_IMAGE_VIEW_TYPE_2D, kPlaceholderDepth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1);
    ao_ph_.view = res_.make_view(ao_ph_.img, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
    env_ph_.view = res_.make_view(env_ph_.img, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 6, 1);
    if (!white_.view || !depth_ph_.view || !ao_ph_.view || !env_ph_.view) return false;
    Buffer st;
    if (!res_.make_buffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &st)) return false;
    std::memcpy(st.map, white, 4);
    st.map[4] = one;
    std::memcpy(st.map + 8, black, sizeof black);
    VkCommandBuffer cb = begin_once();
    bool ok = cb != VK_NULL_HANDLE;
    if (ok) {
        const VkImageLayout dstl = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rd = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_barrier(cb, white_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, dstl);
        image_barrier(cb, depth_ph_.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, dstl);
        image_barrier(cb, ao_ph_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, dstl);
        image_barrier(cb, env_ph_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED, dstl);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cb, st.buf, white_.img, dstl, 1, &c);
        c.bufferOffset = 4;
        vkCmdCopyBufferToImage(cb, st.buf, ao_ph_.img, dstl, 1, &c);
        c.bufferOffset = 8;
        c.imageSubresource.layerCount = 6;
        vkCmdCopyBufferToImage(cb, st.buf, env_ph_.img, dstl, 1, &c);
        const VkClearDepthStencilValue far{1.f, 0};
        const VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, depth_ph_.img, dstl, &far, 1, &dr);
        image_barrier(cb, white_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, dstl, rd);
        image_barrier(cb, depth_ph_.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, dstl, rd);
        image_barrier(cb, ao_ph_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, dstl, rd);
        image_barrier(cb, env_ph_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 6, dstl, rd);
        ok = end_once();
    }
    res_.destroy_buffer(st);
    return ok;
}
bool VulkanDevice::make_resolve_shaders() {
    VkShaderModule m[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::string err;
    const bool ok = vk::make_resolve_shaders(dev_, m, &err);
    resolve_vs_ = m[0]; resolve_fs_ = m[1]; resolve_fs_depth_ = m[2];   // destroy_all frees whichever exist
    if (!ok) vos_log("AR_VULKAN resolve shader: %s", err.c_str());
    return ok;
}
void VulkanDevice::destroy_all() {
    while (!garbage_.empty()) { garbage_.front().free(); garbage_.pop_front(); }
    for (Target &t : targets_) destroy_target(t);
    for (auto &kv : textures_) res_.destroy_image(kv.second.im);
    for (Upload &u : uploads_) { res_.destroy_image(u.tex.im); res_.destroy_buffer(u.staging); }
    for (Image *im : {&white_, &depth_ph_, &ao_ph_, &env_ph_}) res_.destroy_image(*im);
    for (Map *m : {&shadow_, &under_, &foot_}) destroy_map(*m);
    for (Image &im : ao_) res_.destroy_image(im);
    for (auto &kv : composite_pipes_) if (kv.second) vkDestroyPipeline(dev_, kv.second, nullptr);
    if (fixed_.composite_fs) vkDestroyShaderModule(dev_, fixed_.composite_fs, nullptr);
    for (auto &kv : pipes_) {
        for (auto &v : kv.second.variants) if (v.second) vkDestroyPipeline(dev_, v.second, nullptr);
        if (kv.second.vs) vkDestroyShaderModule(dev_, kv.second.vs, nullptr);
        if (kv.second.fs) vkDestroyShaderModule(dev_, kv.second.fs, nullptr);
    }
    for (auto &kv : resolve_pipes_) if (kv.second) vkDestroyPipeline(dev_, kv.second, nullptr);
    for (auto &kv : resolve_passes_) if (kv.second) vkDestroyRenderPass(dev_, kv.second, nullptr);
    for (auto &kv : target_passes_) if (kv.second) vkDestroyRenderPass(dev_, kv.second, nullptr);
    for (VkPipeline p : {fixed_.depth[0][0], fixed_.depth[0][1], fixed_.depth[1][0], fixed_.depth[1][1], fixed_.ao, fixed_.composite, mip_pipe_})
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (VkShaderModule m : {resolve_vs_, resolve_fs_, resolve_fs_depth_}) if (m) vkDestroyShaderModule(dev_, m, nullptr);
    for (auto &kv : samplers_) vkDestroySampler(dev_, kv.second, nullptr);
    for (VkSampler s : {linear_clamp_, nearest_, compare_sampler_}) if (s) vkDestroySampler(dev_, s, nullptr);
    ring_.destroy();
    for (Frame &f : frames_) {
        for (VkDescriptorPool p : f.pools) vkDestroyDescriptorPool(dev_, p, nullptr);
        if (f.fence) vkDestroyFence(dev_, f.fence, nullptr);
        if (f.queries) vkDestroyQueryPool(dev_, f.queries, nullptr);
    }
    if (once_fence_) vkDestroyFence(dev_, once_fence_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(dev_, cmd_pool_, nullptr);   // frees every command buffer
    if (fixed_layouts_.map_pass) vkDestroyRenderPass(dev_, fixed_layouts_.map_pass, nullptr);
    for (VkPipelineLayout l : {pipe_layout_, resolve_layout_, composite_layout_, fixed_layouts_.depth, fixed_layouts_.ao, fixed_layouts_.mip})
        if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
    for (VkDescriptorSetLayout l : {set0_layout_, set1_layout_, resolve_set_layout_, depth_set0_layout_, depth_set1_layout_, ao_set_layout_,
                                    mip_set_layout_})
        if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
    if (cache_) vkDestroyPipelineCache(dev_, cache_, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames, retirement, one-off submissions, descriptor sets
// ---------------------------------------------------------------------------------------------------------------------
// destroyed once every frame begun so far completed (it may be recorded in the open frame or one in flight)
void VulkanDevice::retire(std::function<void()> f) { garbage_.push_back(Garbage{frame_serial_, std::move(f)}); }
void VulkanDevice::collect_garbage() {
    uint64_t done;
    {
        std::lock_guard<std::mutex> g(m_);
        done = completed_serial_;
    }
    while (!garbage_.empty() && garbage_.front().serial <= done) {
        garbage_.front().free();
        garbage_.pop_front();
    }
}
// A one-off submission on the render thread, waited for (read-back, placeholders).
VkCommandBuffer VulkanDevice::begin_once() {
    if (vkResetCommandBuffer(once_cmd_, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(once_cmd_, &bi) == VK_SUCCESS ? once_cmd_ : VK_NULL_HANDLE;
}
bool VulkanDevice::end_once() {
    if (vkEndCommandBuffer(once_cmd_) != VK_SUCCESS) return false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &once_cmd_;
    VkResult r = vkQueueSubmit(queue_, 1, &si, once_fence_);
    if (r == VK_SUCCESS) r = vkWaitForFences(dev_, 1, &once_fence_, VK_TRUE, UINT64_MAX);
    const bool ok = r == VK_SUCCESS;
    if (!ok) vos_log("AR_VULKAN one-off submission failed: %d", (int)r);
    if (vkResetFences(dev_, 1, &once_fence_) != VK_SUCCESS) return false;
    return ok;
}
VkDescriptorSet VulkanDevice::alloc_set(VkDescriptorSetLayout layout) {
    Frame &f = frames_[slot_];
    for (;;) {
        bool fresh = false;
        if (f.pool_at == f.pools.size()) {
            const uint32_t kSets = 1024;
            const VkDescriptorPoolSize sizes[5] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 8},
                                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSets * 3},
                                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets},
                                                   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets * 2},
                                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSets;
            pi.poolSizeCount = 5;
            pi.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev_, &pi, nullptr, &p) != VK_SUCCESS) { vos_log("AR_VULKAN descriptor pool failed"); return VK_NULL_HANDLE; }
            f.pools.push_back(p);
            fresh = true;
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pool_at];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet s = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(dev_, &ai, &s);
        if (r == VK_SUCCESS) return s;
        // a full pool (some drivers say OUT_OF_DEVICE_MEMORY for it): the next one; a fresh pool that fails is a failure
        if (fresh || (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL && r != VK_ERROR_OUT_OF_DEVICE_MEMORY)) {
            vos_log("AR_VULKAN vkAllocateDescriptorSets failed: %d", (int)r);
            return VK_NULL_HANDLE;
        }
        f.pool_at++;
    }
}
void VulkanDevice::begin_frame() {
    collect_warmed();
    if (frame_open_) return;
    frame_ok_ = false;   // true only once the frame's command buffer is open
    depth_written_ = true;
    if (!ok_) return;
    slot_ = (slot_ + 1) % kFrames;
    Frame &f = frames_[slot_];
    {   // the slot's previous frame completed and the completion thread is done with its fence and queries
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return completed_serial_ >= f.serial; });
    }
    if (f.submitted && vkResetFences(dev_, 1, &f.fence) != VK_SUCCESS) { vos_log("AR_VULKAN vkResetFences failed"); return; }
    f.submitted = false;
    collect_garbage();
    for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(dev_, p, 0);
    f.pool_at = 0;
    ring_.begin(slot_);
    if (vkResetCommandBuffer(f.cmd, 0) != VK_SUCCESS) { vos_log("AR_VULKAN vkResetCommandBuffer failed"); return; }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(f.cmd, &bi) != VK_SUCCESS) { vos_log("AR_VULKAN vkBeginCommandBuffer failed"); return; }
    cmd_ = f.cmd;
    if (timestamps_) {
        vkCmdResetQueryPool(cmd_, f.queries, 0, 2);
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.queries, 0);
    }
    f.serial = ++frame_serial_;
    frame_open_ = true;
    frame_ok_ = true;
    encoders_ = 0;
    for (Target &t : targets_) t.encoded = false;
    gamma_n_ = gamma_texels_ = 0;
    under_valid_ = shadow_valid_ = foot_valid_ = false;   // a map is sampled only in the frame that rendered it
    fresh_images_.clear();
    record_textures(true);   // the uploads since the last frame, their mips and the re-mips due
}
void VulkanDevice::end_frame(void (*done)(void *), void *ctx, void *drawable) {
    (void)drawable;   // OpenXR presents (xrEndFrame), not the device
    if (!frame_open_) { if (done) done(ctx); return; }
    for (Target &t : targets_) { flush(t); t.encoded = false; }
    frame_open_ = false;
    encoders_last_ = encoders_;
    encoders_ = 0;
    Frame &f = frames_[slot_];
    if (timestamps_) vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.queries, 1);
    cmd_ = VK_NULL_HANDLE;
    bool submitted = vkEndCommandBuffer(f.cmd) == VK_SUCCESS;
    if (submitted) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        const VkResult r = vkQueueSubmit(queue_, 1, &si, f.fence);
        submitted = r == VK_SUCCESS;
        if (!submitted) vos_log("AR_VULKAN vkQueueSubmit failed: %d", (int)r);
    } else {
        vos_log("AR_VULKAN vkEndCommandBuffer failed");
    }
    f.submitted = submitted;
    {   // queued even when the submit failed: the frames complete (and call done) in order
        std::lock_guard<std::mutex> g(m_);
        jobs_.push_back(Job{f.serial, submitted ? f.fence : VK_NULL_HANDLE, timestamps_ && submitted ? f.queries : VK_NULL_HANDLE, done, ctx});
        submitted_serial_ = f.serial;
    }
    cv_.notify_all();
    maybe_save_pipeline_cache(false);
}
void VulkanDevice::wait_idle() {
    {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return completed_serial_ >= submitted_serial_; });
    }
    collect_garbage();
}
// The completion thread: waits the frames' fences in submission order.
void VulkanDevice::completion_loop() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;   // stopping, nothing left
            j = jobs_.front();
        }
        double ms = -1.0;
        if (j.fence) {
            const VkResult r = vkWaitForFences(dev_, 1, &j.fence, VK_TRUE, UINT64_MAX);
            if (r != VK_SUCCESS) vos_log("AR_VULKAN vkWaitForFences failed: %d", (int)r);
            uint64_t ts[2] = {0, 0};
            if (r == VK_SUCCESS && j.queries &&
                vkGetQueryPoolResults(dev_, j.queries, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const uint64_t ticks = (ts[1] - ts[0]) & timestamp_mask_;   // the counter wraps at timestampValidBits
                ms = (double)ticks * props_.limits.timestampPeriod * 1e-6;
                if (!j.bake) gpu_ms_.store(ms);
            }
        }
        if (j.done) j.done(j.ctx);
        {
            std::lock_guard<std::mutex> g(m_);
            jobs_.pop_front();
            if (!j.bake) completed_serial_ = j.serial;   // a bake precedes its frame's job: that frame moves the serial
            else if (j.fence && j.bake > ao_done_gen_) { ao_done_gen_ = j.bake; ao_done_ms_ = std::max(0.0, ms); }
        }
        cv_.notify_all();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------------------------------------------------
uint32_t VulkanDevice::create_target(uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) {
    targets_.emplace_back();
    make_target(targets_.back(), w, h, layers, samples);
    return (uint32_t)targets_.size();
}
void VulkanDevice::resize_target(uint32_t t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) {
    if (t == 0 || t > targets_.size()) return;
    Target &x = targets_[t - 1];
    if (x.w != std::max(w, 1u) || x.h != std::max(h, 1u) || x.layers != std::max(layers, 1u) ||
        x.samples != supported_samples(samples, layers) || !x.color.img)
        make_target(x, w, h, layers, samples);
}
// Layered multisample rendering (gl_Layer into a multisample array) is core Vulkan: layers do not lower the count.
uint32_t VulkanDevice::supported_samples(uint32_t wanted, uint32_t layers) const {
    (void)layers;
    if (msaa_refused_ || !depth_resolve_) return 1;
    for (uint32_t s : {4u, 2u})
        if (s <= wanted && (sample_counts_ & s) && env_.rp[s]) return s;
    return 1;
}
void VulkanDevice::bind_target_texture(uint32_t tex_id, uint32_t target) {
    if (!tex_id) return;
    if (target == 0 || target > targets_.size()) target_tex_.erase(tex_id);
    else target_tex_[tex_id] = target;
}
void VulkanDevice::release_target(uint32_t t) {
    if (t == 0 || t > targets_.size()) return;
    Target &x = targets_[t - 1];
    flush(x);
    retire_target(x);
    x.w = 0; x.h = 0; x.samples = 1; x.memoryless = false;
}
VkRenderPass VulkanDevice::target_pass(uint32_t samples, bool clear, bool transient) {
    const uint32_t key = samples | (clear ? 1u << 8 : 0u) | (transient ? 1u << 9 : 0u);
    auto it = target_passes_.find(key);
    if (it != target_passes_.end()) return it->second;
    vk::TargetPassDesc d;
    d.samples = samples; d.clear = clear; d.transient = transient;
    d.depth_mode = depth_resolve_; d.stencil_mode = stencil_resolve_; d.create2 = create_rp2_;
    return target_passes_[key] = vk::make_target_pass(dev_, d);
}
void VulkanDevice::make_target(Target &t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) {
    flush(t);
    retire_target(t);
    w = std::max(w, 1u); h = std::max(h, 1u); layers = std::max(layers, 1u);
    t.w = w; t.h = h; t.layers = layers; t.samples = supported_samples(samples, layers);
    t.memoryless = false;
    t.ms_ready = false;
    if (!ok_) return;
    const VkImageUsageFlags su = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vk::ImageDesc cd, dd;
    cd.format = kColorFormat; cd.extent = {w, h, 1}; cd.layers = layers; cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | su;
    dd.format = kDepthFormat; dd.extent = {w, h, 1}; dd.layers = layers;
    dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (depth_sampled_ ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
    bool ok = res_.make_image(cd, &t.color) && res_.make_image(dd, &t.depth);
    const bool ms = t.samples > 1;
    if (ok && ms) {   // transient multisample attachments, resolved into color and depth at the end of each render pass
        vk::ImageDesc mc, md;
        mc.format = kColorFormat; mc.extent = {w, h, 1}; mc.layers = layers; mc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        mc.samples = (VkSampleCountFlagBits)t.samples; mc.lazy = res_.lazy_available();
        md = mc;
        md.format = kDepthFormat; md.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        bool lazy_c = false, lazy_d = false;
        ok = res_.make_image(mc, &t.ms_color, &lazy_c) && res_.make_image(md, &t.ms_depth, &lazy_d);
        t.memoryless = lazy_c && lazy_d;
    }
    if (ok) {
        t.color.view = res_.make_view(t.color.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers, 1);
        t.color_2d = res_.make_view(t.color.img, VK_IMAGE_VIEW_TYPE_2D, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
        t.depth.view = res_.make_view(t.depth.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kDepthFormat, depth_aspects(kDepthFormat), 0, layers, 1);
        if (depth_sampled_)
            t.depth_sample = res_.make_view(t.depth.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 0, layers, 1);
        ok = t.color.view && t.color_2d && t.depth.view && (!depth_sampled_ || t.depth_sample);
        if (ok && ms) {
            t.ms_color.view = res_.make_view(t.ms_color.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers, 1);
            t.ms_depth.view = res_.make_view(t.ms_depth.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kDepthFormat, depth_aspects(kDepthFormat), 0, layers, 1);
            ok = t.ms_color.view && t.ms_depth.view;
        }
    }
    VkRenderPass rp = ok ? target_pass(t.samples, true, t.memoryless) : VK_NULL_HANDLE;
    if (rp) {
        const VkImageView att[4] = {ms ? t.ms_color.view : t.color.view, ms ? t.ms_depth.view : t.depth.view, t.color.view, t.depth.view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp;
        fi.attachmentCount = ms ? 4 : 2;
        fi.pAttachments = att;
        fi.width = w; fi.height = h; fi.layers = layers;
        ok = vkCreateFramebuffer(dev_, &fi, nullptr, &t.fb) == VK_SUCCESS;
        if (!ok) t.fb = VK_NULL_HANDLE;
    }
    if (!ok || !rp) {
        vos_log("AR_VULKAN target %ux%ux%u (%u samples): creation failed", w, h, layers, t.samples);
        destroy_target(t);
    }
    t.color_layout = t.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}
void VulkanDevice::retire_target(Target &t) {
    Target old;
    old.color = t.color; old.depth = t.depth; old.ms_color = t.ms_color; old.ms_depth = t.ms_depth;
    old.depth_sample = t.depth_sample; old.color_2d = t.color_2d; old.fb = t.fb;
    t.color = t.depth = t.ms_color = t.ms_depth = Image{};
    t.depth_sample = t.color_2d = VK_NULL_HANDLE;
    t.fb = VK_NULL_HANDLE;
    t.color_layout = t.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t.ms_ready = false;
    if (old.color.img || old.depth.img || old.fb) retire([this, old]() mutable { destroy_target(old); });
}
void VulkanDevice::destroy_target(Target &t) {
    if (t.fb) vkDestroyFramebuffer(dev_, t.fb, nullptr);
    for (VkImageView v : {t.depth_sample, t.color_2d}) if (v) vkDestroyImageView(dev_, v, nullptr);
    for (Image *im : {&t.color, &t.depth, &t.ms_color, &t.ms_depth}) res_.destroy_image(*im);
    t.fb = VK_NULL_HANDLE;
    t.depth_sample = t.color_2d = VK_NULL_HANDLE;
}
void VulkanDevice::transition_color(Target &t, VkImageLayout to, bool discard) {
    image_barrier(cmd_or_once(), t.color.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, t.layers, t.color_layout, to, discard);
    t.color_layout = to;
}
void VulkanDevice::transition_depth(Target &t, VkImageLayout to, bool discard) {
    image_barrier(cmd_or_once(), t.depth.img, depth_aspects(kDepthFormat), 0, 1, t.layers, t.depth_layout, to, discard);
    t.depth_layout = to;
}

// ---------------------------------------------------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------------------------------------------------
void VulkanDevice::encode_pass(uint32_t target, const PassDesc &pass, const ViewConstants &view, const fc_vertex *verts, uint32_t nv,
                               const DrawPacket *draws, uint32_t nd, const int8_t *normals, const float *prev_pos) {
    if (!frame_open_ || target == 0 || target > targets_.size()) return;
    Target &t = targets_[target - 1];
    if (!t.color.img) return;
    if (pass.clear && t.nrec) flush(t);
    if (!target_tex_.empty())   // a draw sampling a target sees what it drew this frame
        for (uint32_t i = 0; i < nd; i++)
            for (uint32_t k : {draws[i].tex[0], draws[i].tex[1]}) {
                auto bt = k ? target_tex_.find(k) : target_tex_.end();
                if (bt != target_tex_.end() && bt->second != target && bt->second <= targets_.size()) flush(targets_[bt->second - 1]);
            }
    if (t.nrec == t.recs.size()) t.recs.emplace_back();
    Rec &r = t.recs[t.nrec++];
    r.pass = pass; r.view = view; r.light = light_; r.nv = nv;
    r.shadow = shadow_valid_ ? shadow_.im.view : depth_ph_.view;
    r.under = under_valid_ ? under_.im.view : depth_ph_.view;
    r.foot = foot_valid_ ? foot_.im.view : depth_ph_.view;
    r.ao = ao_bound_ ? ao_bound_ : ao_ph_.view;
    r.draws.clear();
    r.tex.clear();
    r.sampled.clear();
    r.vb = r.nb = r.pb = VK_NULL_HANDLE; r.voff = r.noff = r.poff = 0;
    if (nv && nd && verts) {
        Slice s;
        if (!ring_.stage(verts, (size_t)nv * sizeof(fc_vertex), &s)) return;
        r.vb = s.buf; r.voff = s.off;
        if (normals && ring_.stage(normals, (size_t)nv * 4, &s)) { r.nb = s.buf; r.noff = s.off; }
        if (prev_pos && view.interp_on && ring_.stage(prev_pos, (size_t)nv * 3 * sizeof(float), &s)) { r.pb = s.buf; r.poff = s.off; }
        r.draws.assign(draws, draws + nd);
        r.tex.resize((size_t)nd * 2);
        for (uint32_t i = 0; i < nd; i++)
            for (int k = 0; k < 2; k++) {
                r.tex[2 * i + k] = texture(draws[i].tex[k], target);
                auto bt = draws[i].tex[k] ? target_tex_.find(draws[i].tex[k]) : target_tex_.end();
                if (bt != target_tex_.end() && bt->second != target && bt->second <= targets_.size() &&
                    std::find(r.sampled.begin(), r.sampled.end(), bt->second) == r.sampled.end())
                    r.sampled.push_back(bt->second);
            }
    }
}
// A target's passes in one render pass (metal_device.mm flush). Multisample: the passes draw into the multisample
// attachments, which the render pass resolves into color and depth at its end; a transient target's attachments cannot be
// loaded, so passes whose first does not clear are dropped (late_passes).
void VulkanDevice::flush(Target &t) {
    if (!t.nrec) return;
    const size_t n = t.nrec;
    t.nrec = 0;
    if (!frame_open_ || !t.fb) return;
    const bool ms = t.samples > 1;
    const PassDesc &p0 = t.recs[0].pass;
    if (ms && t.memoryless && !p0.clear) { late_ += n; return; }
    for (size_t i = 0; i < n; i++)   // the targets these passes sample: their resolved colour, read in the fragment stage
        for (uint32_t s : t.recs[i].sampled) {
            Target &o = targets_[s - 1];
            if (&o != &t && o.color.img && o.color_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                transition_color(o, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    // cleared, or overwritten by the resolve: the old contents may go
    transition_color(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, p0.clear || ms);
    transition_depth(t, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, p0.clear || ms);
    if (ms) {   // ordered after the target's previous render pass (and out of UNDEFINED the first time)
        const VkImageLayout cl = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dl = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const bool fresh = !t.ms_ready;
        image_barrier(cmd_, t.ms_color.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, t.layers, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : cl, cl,
                      p0.clear || fresh);
        image_barrier(cmd_, t.ms_depth.img, depth_aspects(kDepthFormat), 0, 1, t.layers, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : dl, dl,
                      p0.clear || fresh);
        t.ms_ready = true;
    }
    VkClearValue cv[2];
    for (int k = 0; k < 4; k++) cv[0].color.float32[k] = p0.clear_color[k];
    cv[1].depthStencil = {0.f, 0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = target_pass(t.samples, p0.clear, t.memoryless);
    bi.framebuffer = t.fb;
    bi.renderArea = {{0, 0}, {t.w, t.h}};
    bi.clearValueCount = p0.clear ? 2 : 0;
    bi.pClearValues = cv;
    vkCmdBeginRenderPass(cmd_, &bi, VK_SUBPASS_CONTENTS_INLINE);
    for (size_t i = 0; i < n; i++) encode_rec(t, t.recs[i]);
    vkCmdEndRenderPass(cmd_);
    t.encoded = true;
    encoders_++;
}
bool VulkanDevice::scissor_for(const Viewport &v, uint32_t bw, uint32_t bh, VkRect2D *out) const {
    const uint32_t sx = (uint32_t)std::max(0.f, v.x), sy = (uint32_t)std::max(0.f, v.y);
    const uint32_t sw = std::min<uint32_t>((uint32_t)std::max(0.f, v.w), bw > sx ? bw - sx : 0);
    const uint32_t sh = std::min<uint32_t>((uint32_t)std::max(0.f, v.h), bh > sy ? bh - sy : 0);
    *out = VkRect2D{{(int32_t)sx, (int32_t)sy}, {sw, sh}};
    return sw && sh;
}
void VulkanDevice::encode_rec(const Target &t, const Rec &r) {
    Viewport v = r.pass.viewport;
    if (v.w <= 0 || v.h <= 0) v = Viewport{0, 0, float(t.w), float(t.h)};
    VkRect2D sc;
    if (!scissor_for(v, t.w, t.h, &sc) || !r.vb || r.draws.empty()) return;
    if (r.view.layer >= t.layers) { vos_log("AR_VULKAN pass into layer %u of a target with %u", r.view.layer, t.layers); return; }
    const VkViewport vp{v.x, v.y + v.h, v.w, -v.h, 0.f, 1.f};   // the Y flip (Metal's NDC +y up)
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &sc);
    const uint32_t nd = (uint32_t)r.draws.size();
    // the pass's uniforms in one block of the ring: ViewU, LightU, then one DrawU per draw
    const VkDeviceSize av = vk::align_up(sizeof(ViewU), ring_align_), al = vk::align_up(sizeof(LightU), ring_align_),
                       ad = vk::align_up(sizeof(DrawU), ring_align_);
    Slice ub;
    if (!ring_.alloc(av + al + ad * nd, &ub)) return;
    const ViewConstants &view = r.view;
    ViewU vu{};
    std::memcpy(vu.clip_from_model, view.clip_from_model, sizeof vu.clip_from_model);
    vu.eye_pull[0] = view.eye_model[0]; vu.eye_pull[1] = view.eye_model[1]; vu.eye_pull[2] = view.eye_model[2]; vu.eye_pull[3] = view.decal_pull;
    for (int k = 0; k < 3; k++) vu.eye_view[k] = view.eye_view[k];
    vu.layer = view.layer;
    for (int k = 0; k < 4; k++) { vu.wclip_lo[k] = view.wclip_lo[k]; vu.wclip_hi[k] = view.wclip_hi[k]; }
    vu.interp[0] = view.interp_alpha; vu.interp[1] = (view.interp_on && r.pb) ? 1.f : 0.f;
    std::memcpy(ub.ptr, &vu, sizeof vu);
    std::memcpy(ub.ptr + av, &r.light, sizeof r.light);
    VkDescriptorSet s0 = alloc_set(set0_layout_);
    if (!s0) return;
    // the shaders read the normals / previous positions only when the flags say so; otherwise the vertices will do
    const VkDescriptorBufferInfo bufs[5] = {
        {ub.buf, 0, sizeof(ViewU)}, {ub.buf, 0, sizeof(DrawU)}, {ub.buf, 0, sizeof(LightU)},
        r.nb ? VkDescriptorBufferInfo{r.nb, r.noff, (VkDeviceSize)r.nv * 4} : VkDescriptorBufferInfo{r.vb, r.voff, (VkDeviceSize)r.nv * kVertexStride},
        r.pb ? VkDescriptorBufferInfo{r.pb, r.poff, (VkDeviceSize)r.nv * 12} : VkDescriptorBufferInfo{r.vb, r.voff, (VkDeviceSize)r.nv * kVertexStride}};
    const uint32_t bind[5] = {kBindView, kBindDraw, kBindLight, kBindNormals, kBindPrevPos};
    VkWriteDescriptorSet w0[5];
    for (int k = 0; k < 5; k++) {
        w0[k] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w0[k].dstSet = s0; w0[k].dstBinding = bind[k]; w0[k].descriptorCount = 1;
        w0[k].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w0[k].pBufferInfo = &bufs[k];
    }
    vkUpdateDescriptorSets(dev_, 5, w0, 0, nullptr);
    const VkDeviceSize vbo = r.voff;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &r.vb, &vbo);
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet s1 = VK_NULL_HANDLE;
    VkImageView last_tex[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSampler last_samp[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    const bool dyn_cull = set_cull_ != nullptr;
    for (uint32_t i = 0; i < nd; i++) {
        const DrawPacket &d = r.draws[i];
        if (d.vertex_count == 0 || d.first_vertex + d.vertex_count > r.nv) continue;
        VkPipeline ps = draw_pipeline(d, t.samples, vk::draw_state(d, r.pass.depth, dyn_cull), nullptr, true);
        if (!ps) continue;
        if (ps != bound) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ps); bound = ps; }
        if (dyn_cull)
            set_cull_(cmd_, d.cull == FC_CULL_BACK ? VK_CULL_MODE_BACK_BIT : d.cull == FC_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
        vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, r.pass.depth ? vk::stencil_ref(vk::stencil_mode(d)) : 0u);
        DrawU du{};
        std::memcpy(du.prim, d.prim, sizeof du.prim); std::memcpy(du.env, d.env, sizeof du.env);
        du.lod = d.lod_frac; du.decal = d.decal ? 1.f : 0.f; du.noise_seed = view.noise_seed; du.pull = d.pull;
        du.fade = d.fade; du.talpha = d.trans_alpha; std::memcpy(du.clip, d.clip, sizeof du.clip); du.clip_on = d.clip_on ? 1.f : 0.f;
        du.lflags = d.light_flags | (r.nb ? 256u : 0u) | ((d.light_role & FC_LR_SELF_LIT) ? 512u : 0u) |
                    ((d.light_role & FC_LR_MIRROR) ? 1024u : 0u) |
                    ((d.light_role & FC_LR_WATER) ? 2048u : 0u) | ((d.light_role & FC_LR_LAVA) ? 8192u : 0u) |
                    (d.ink ? 4096u : 0u);
        std::memcpy(ub.ptr + av + al + ad * i, &du, sizeof du);
        const VkSampler samp[2] = {sampler(d.samp[0]), sampler(d.samp[1])};
        const VkImageView tex[2] = {r.tex[2 * i], r.tex[2 * i + 1]};
        if (!s1 || tex[0] != last_tex[0] || tex[1] != last_tex[1] || samp[0] != last_samp[0] || samp[1] != last_samp[1]) {
            s1 = texture_set(r, tex, samp);
            if (!s1) continue;
            last_tex[0] = tex[0]; last_tex[1] = tex[1]; last_samp[0] = samp[0]; last_samp[1] = samp[1];
        }
        const uint32_t dyn[3] = {(uint32_t)ub.off, (uint32_t)(ub.off + av + al + ad * i), (uint32_t)(ub.off + av)};
        const VkDescriptorSet sets[2] = {s0, s1};
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 2, sets, 3, dyn);
        // reverse-Z: a positive slope factor moves toward the viewer (Metal's setDepthBias(0, decal_slope, 0))
        vkCmdSetDepthBias(cmd_, 0.f, 0.f, r.pass.depth && d.decal ? view.decal_slope : 0.f);
        vkCmdDraw(cmd_, d.vertex_count, 1, d.first_vertex, 0);
    }
}
// set 1: the draw's two textures, the pass's maps (or their placeholders) and the placeholder environment cubes
VkDescriptorSet VulkanDevice::texture_set(const Rec &r, const VkImageView tex[2], const VkSampler samp[2]) {
    VkDescriptorSet s = alloc_set(set1_layout_);
    if (!s) return VK_NULL_HANDLE;
    const VkImageLayout rd = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo ii[8] = {
        {samp[0], tex[0], rd}, {samp[1], tex[1], rd}, {compare_sampler_, r.shadow, rd}, {depth_linear_ ? linear_clamp_ : nearest_, r.under, rd},
        {linear_clamp_, r.ao, rd}, {linear_clamp_, env_ph_.view, rd}, {linear_clamp_, env_ph_.view, rd}, {nearest_, r.foot, rd}};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s;
    w.dstBinding = 0;
    w.descriptorCount = 8;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// The resolve: the full-screen copy of one target slice into the external image's slice dst.slice
// ---------------------------------------------------------------------------------------------------------------------
// A resolve that records nothing marks the frame not ok (vulkan_device_frame_ok).
void VulkanDevice::encode_resolve(uint32_t src, const ExternalView &dst) {
    if (!record_resolve(src, dst)) frame_ok_ = false;
}
bool VulkanDevice::record_resolve(uint32_t src, const ExternalView &dst) {
    if (!frame_open_ || src == 0 || src > targets_.size() || !dst.color) return false;
    Target &s = targets_[src - 1];
    if (!s.color.img) return false;
    flush(s);
    const VulkanExternalImage &ci = *static_cast<const VulkanExternalImage *>(dst.color);
    const VulkanExternalImage *di = static_cast<const VulkanExternalImage *>(dst.depth);
    if (dst.src_layer >= s.layers) { vos_log("AR_VULKAN resolve of slice %u from a target with %u", dst.src_layer, s.layers); return false; }
    if (dst.slice >= ci.layers || (di && dst.slice >= di->layers)) { vos_log("AR_VULKAN resolve into slice %u of %u", dst.slice, ci.layers); return false; }
    if (di && !s.depth_sample) {
        if (!depth_sample_warned_) vos_log("AR_VULKAN resolve: target depth is not sampleable here: colour only");
        depth_sample_warned_ = true;
        depth_written_ = false;
        di = nullptr;
    }
    VkRenderPass rp = resolve_render_pass(ci.format, di ? di->format : VK_FORMAT_UNDEFINED);
    bool pending = false;
    VkPipeline ps = rp ? resolve_pipeline(rp, di != nullptr, &pending) : VK_NULL_HANDLE;
    if (!ps && !pending) { vos_log("AR_VULKAN no resolve pipeline for colour %d / depth %d", (int)ci.format, di ? (int)di->format : 0); return false; }
    VkImageView views[2] = {res_.make_view(ci.image, VK_IMAGE_VIEW_TYPE_2D, ci.format, VK_IMAGE_ASPECT_COLOR_BIT, dst.slice, 1, 1),
                            di ? res_.make_view(di->image, VK_IMAGE_VIEW_TYPE_2D, di->format, depth_aspects(di->format), dst.slice, 1, 1)
                               : VK_NULL_HANDLE};
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (views[0] && (!di || views[1])) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp;
        fi.attachmentCount = di ? 2 : 1;
        fi.pAttachments = views;
        fi.width = ci.width; fi.height = ci.height; fi.layers = 1;
        if (vkCreateFramebuffer(dev_, &fi, nullptr, &fb) != VK_SUCCESS) fb = VK_NULL_HANDLE;
    }
    VkDevice dv = dev_;
    retire([dv, views, fb] {
        if (fb) vkDestroyFramebuffer(dv, fb, nullptr);
        for (VkImageView v : views) if (v) vkDestroyImageView(dv, v, nullptr);
    });
    if (!fb) { vos_log("AR_VULKAN resolve: framebuffer creation failed"); return false; }
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {ci.width, ci.height}};
    if (!ps) {   // async: still compiling; the slice is cleared (transparent, far) this frame, the stencil kept (Metal)
        vkCmdBeginRenderPass(cmd_, &bi, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment ca[2] = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}}};
        ca[1].clearValue.depthStencil = {0.f, 0};
        const VkClearRect cr{{{0, 0}, {ci.width, ci.height}}, 0, 1};
        vkCmdClearAttachments(cmd_, di ? 2 : 1, ca, 1, &cr);
        vkCmdEndRenderPass(cmd_);
        deferred_++;
        encoders_++;
        return true;
    }
    VkDescriptorSet set = alloc_set(resolve_set_layout_);
    if (!set) return false;
    VkDescriptorImageInfo ii[2] = {{nearest_, s.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {nearest_, s.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    if (s.depth_sample) ii[1] = {nearest_, s.depth_sample, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[2];
    for (uint32_t k = 0; k < 2; k++) {
        wr[k] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[k].dstSet = set; wr[k].dstBinding = k; wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);
    transition_color(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (s.depth_sample) transition_depth(s, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    Viewport v = dst.viewport;
    if (v.w <= 0 || v.h <= 0) v = Viewport{0.f, 0.f, (float)ci.width, (float)ci.height};
    VkRect2D sc;
    if (!scissor_for(v, ci.width, ci.height, &sc)) return false;
    vkCmdBeginRenderPass(cmd_, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{v.x, v.y, v.w, v.h, 0.f, 1.f};   // no flip: the triangle covers it and reads gl_FragCoord
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &sc);
    const uint32_t vw = std::max(1u, (uint32_t)std::ceil(v.w)), vh = std::max(1u, (uint32_t)std::ceil(v.h));
    const ResolveU u = {{v.x, v.y}, {dst.src_x, dst.src_y}, {dst.src_x + vw - 1, dst.src_y + vh - 1}, dst.linearize ? 1u : 0u, dst.src_layer};
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ps);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd_, resolve_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, &u);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd_);
    encoders_++;
    return true;
}
VkRenderPass VulkanDevice::resolve_render_pass(VkFormat cf, VkFormat df) {
    const uint64_t key = ((uint64_t)cf << 32) | (uint64_t)df;
    auto it = resolve_passes_.find(key);
    return it != resolve_passes_.end() ? it->second : (resolve_passes_[key] = vk::make_resolve_pass(dev_, cf, df));
}
// metal_device.mm resolve_pipeline: with async pipelines a missing one compiles on another thread (*pending meanwhile).
VkPipeline VulkanDevice::resolve_pipeline(VkRenderPass rp, bool depth, bool *pending) {
    const uint64_t key = (uint64_t)rp;
    auto it = resolve_pipes_.find(key);
    if (it != resolve_pipes_.end()) return it->second;
    VkDevice dev = dev_;
    VkPipelineCache cache = cache_;
    VkPipelineLayout layout = resolve_layout_;
    VkShaderModule vs = resolve_vs_, fs = depth ? resolve_fs_depth_ : resolve_fs_;
    if (!async_) { built_since_save_++; return resolve_pipes_[key] = vk::make_resolve_pipeline(dev, cache, layout, rp, vs, fs, depth); }
    auto jt = resolve_jobs_.find(key);
    if (jt == resolve_jobs_.end())
        jt = resolve_jobs_.emplace(key, std::async(std::launch::async, [=] { return vk::make_resolve_pipeline(dev, cache, layout, rp, vs, fs, depth); })
                                            .share()).first;
    if (jt->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { *pending = true; return VK_NULL_HANDLE; }
    VkPipeline ps = jt->second.get();
    resolve_jobs_.erase(jt);
    built_since_save_++;
    return resolve_pipes_[key] = ps;
}

// ---------------------------------------------------------------------------------------------------------------------
// Read-back (between frames: what the submitted frames left in the target)
// ---------------------------------------------------------------------------------------------------------------------
bool VulkanDevice::read_target(uint32_t t, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    if (frame_open_ || t == 0 || t > targets_.size() || !targets_[t - 1].color.img) return false;
    Target &x = targets_[t - 1];
    std::vector<uint8_t> bytes;
    if (!read_image(x, false, &bytes)) return false;
    w = x.w; h = x.h;
    rgba = std::move(bytes);
    return true;
}
bool VulkanDevice::read_target_depth(uint32_t t, std::vector<float> &out, uint32_t &w, uint32_t &h) {
    if (frame_open_ || t == 0 || t > targets_.size() || !targets_[t - 1].depth.img) return false;
    Target &x = targets_[t - 1];
    std::vector<uint8_t> bytes;
    if (!read_image(x, true, &bytes)) return false;
    w = x.w; h = x.h;
    out.resize((size_t)w * h);
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
    return true;
}
// layer 0 of the colour (RGBA8) or the depth (D32 floats) through a host-visible buffer
bool VulkanDevice::read_image(Target &x, bool depth, std::vector<uint8_t> *out) {
    wait_idle();
    Buffer st;
    const VkDeviceSize bytes = (VkDeviceSize)x.w * x.h * 4;
    if (!res_.make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &st)) return false;
    VkCommandBuffer cb = begin_once();
    bool ok = cb != VK_NULL_HANDLE;
    if (ok) {
        recording_once_ = true;
        if (depth) transition_depth(x, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        else transition_color(x, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        recording_once_ = false;
        VkBufferImageCopy c{};
        c.imageSubresource = {depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {x.w, x.h, 1};
        vkCmdCopyImageToBuffer(cb, depth ? x.depth.img : x.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buf, 1, &c);
        VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = st.buf;
        hb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
        ok = end_once();
    }
    if (ok) out->assign(st.map, st.map + bytes);
    res_.destroy_buffer(st);
    return ok;
}
}

std::unique_ptr<IRenderDevice> create_vulkan_device(const VulkanDeviceInit &init) {
    std::unique_ptr<vkd::VulkanDevice> d(new vkd::VulkanDevice(init));
    if (!d->ok()) return nullptr;   // the constructor logged "AR_VULKAN init failed: ..."
    return d;
}
bool vulkan_device_frame_ok(IRenderDevice &d) { return static_cast<vkd::VulkanDevice &>(d).frame_ok(); }
bool vulkan_device_depth_written(IRenderDevice &d) { return static_cast<vkd::VulkanDevice &>(d).depth_written(); }
void vulkan_device_accept_any_depth_resolve(IRenderDevice &d) { static_cast<vkd::VulkanDevice &>(d).accept_any_depth_resolve(); }
}
