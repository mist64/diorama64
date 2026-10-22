#include "vulkan_device.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "combiner.h"
#include "glsl_compiler.h"
#include "pipeline_warm.h"
#include "vk_layout.h"
#include "vk_pipelines.h"
#include "vos_log.h"

// The Vulkan port of metal_device.mm (read it for the behaviour; the comments here say only where Vulkan differs).
// - Y: the shared matrices are Metal's (NDC +y up). Every draw pass flips with a negative-height viewport (Vulkan 1.1), so
//   the image, the winding (front face counter-clockwise, as Metal's encoder sets it) and gl_FragCoord (top-left origin)
//   all match Metal.
// - Depth: reverse-Z, cleared to 0, GREATER_OR_EQUAL when testing; D32_SFLOAT_S8_UINT, stencil attached but unused here.
// - Depth state and cull mode are pipeline state in Vulkan 1.1: a PipeKey (Metal's key) holds the compiled shader modules
//   and a small set of VkPipeline variants by (compare, write, cull).
// - Frames: one command buffer per frame, kFrames in flight; a completion thread waits the fences in order and calls
//   on_gpu_done. Resources replaced while frames may use them are retired with the current frame serial and destroyed
//   once that frame completed.
// - Decals: slope-scaled depth bias +decal_slope (reverse-Z: positive is toward the viewer, as Metal's setDepthBias).
// The render passes, pipelines and shader modules are built in vk_pipelines.cpp.
namespace ar {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Constants and the uniform structs (byte for byte metal_device.mm's, also glsl_emitter.cpp's std140 blocks)
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kFrames = 2;
using vkp::kColorFormat;
using vkp::kDepthFormat;
using vkp::ResolveU;
using vkp::kDefaultState;
using vkp::state_key;
constexpr VkFormat kPlaceholderDepth = VK_FORMAT_D32_SFLOAT;
static_assert(kVertexFormatR32G32B32Sfloat == VK_FORMAT_R32G32B32_SFLOAT, "vk_layout.h");
static_assert(kVertexFormatR32G32Sfloat == VK_FORMAT_R32G32_SFLOAT, "vk_layout.h");
static_assert(kVertexFormatR8G8B8A8Unorm == VK_FORMAT_R8G8B8A8_UNORM, "vk_layout.h");
static_assert(sizeof(fc_vertex) == kVertexStride, "fc_vertex is the 32-byte vertex");

struct DrawU { float prim[4]; float env[4]; float lod; float decal; float noise_seed; float fade; float clip[4]; float clip_on; uint32_t lflags;
               float pull; float talpha; };
static_assert(sizeof(DrawU) == 80, "DrawU matches the GLSL block");
struct LightU {
    float dir_on[4]; float tint_amb[4]; float contact[4]; float under_uv[16]; float under_range[4];
    float shadow[4]; float shadow_uv[16]; float cast[4];
    float relight[4];
    float ao[4];
    float ao_min[4];
    float ao_inv[4];
    float toy[4];
    float env0[4], env1[4], env2[4];
    float env3[4], env4[4], env5[4];
    float liquid[4];
    float liquid2[4];
    float water_box[8][4];
    float water_y[2][4];
    float water_foot[4];
    float cut[4]; float cut2[4];
    float trans[4]; float trans_rgb[4]; float trans_c[4];
    float tex[4];
};
static_assert(sizeof(LightU) == 704, "LightU matches the GLSL block");
struct ViewU { float clip_from_model[16]; float eye_pull[4]; float eye_view[4]; uint32_t layer, pad[3]; float wclip_lo[4]; float wclip_hi[4];
               float interp[4]; };
static_assert(sizeof(ViewU) == 160, "ViewU matches the GLSL block");
VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }
VkSamplerAddressMode address(uint8_t w) {
    return w == FC_WRAP_CLAMP ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
         : w == FC_WRAP_MIRROR ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
VkImageAspectFlags depth_aspects(VkFormat f) {
    return VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}
// The stage and access of an image's use in a layout (the barriers derive both sides from the layouts).
void layout_scope(VkImageLayout l, VkPipelineStageFlags *stage, VkAccessFlags *access) {
    switch (l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        return;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        return;
    default:
        *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        *access = 0;
        return;
    }
}
// discard: the contents may go (oldLayout UNDEFINED), but the source scope still covers the previous use of the image (a
// layout transition writes, so it must wait for the last writes).
void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t base_level, uint32_t levels,
                   uint32_t layers, VkImageLayout from, VkImageLayout to, bool discard = false) {
    VkPipelineStageFlags ss, ds;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    layout_scope(from, &ss, &b.srcAccessMask);
    layout_scope(to, &ds, &b.dstAccessMask);
    b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, base_level, levels, 0, layers};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------------------------------------------------------------
// Device-side records (metal_device.mm's Rec, Target and PipeKey)
// ---------------------------------------------------------------------------------------------------------------------
struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t *map = nullptr; VkDeviceSize size = 0; };
struct Image { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
struct Slice { VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0; uint8_t *ptr = nullptr; };
struct Rec {
    PassDesc pass{};
    ViewConstants view{};
    LightU light{};
    VkBuffer vb = VK_NULL_HANDLE, nb = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    VkDeviceSize voff = 0, noff = 0, poff = 0;
    uint32_t nv = 0;
    std::vector<DrawPacket> draws;
    // each draw's two game textures as they were at record time (an upload that replaces one retires the old image
    // with this frame's serial, so the view stays valid until the frame completed)
    std::vector<VkImageView> tex;
};
struct Target {
    Image color, depth;                       // views: 2D_ARRAY of all layers (depth: depth + stencil aspects)
    VkImageView depth_sample = VK_NULL_HANDLE;   // depth aspect only, for the resolve
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED, depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t w = 0, h = 0, layers = 1, samples = 1;
    std::vector<Rec> recs;
    size_t nrec = 0;
    bool encoded = false;
};
struct PipeKey {
    uint64_t slots; uint32_t opts; uint32_t fmt; uint8_t blend; uint8_t depth; uint8_t flat; uint8_t depth_only; uint8_t table_shadow;
    uint8_t clip;
    uint8_t samples;
    uint8_t a2c;
    uint8_t cut;
    uint64_t rp;   // the VkRenderPass the pipelines are built for (every target's passes are compatible with it)
    bool operator<(const PipeKey &o) const {
        return std::tie(slots, opts, fmt, blend, depth, flat, depth_only, table_shadow, clip, samples, a2c, cut, rp) <
               std::tie(o.slots, o.opts, o.fmt, o.blend, o.depth, o.flat, o.depth_only, o.table_shadow, o.clip, o.samples, o.a2c, o.cut, o.rp);
    }
};
// A PipeKey's compiled shaders and its pipelines by draw state (state_key).
struct Pipe {
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    bool ok = false;
    std::string why;
    uint8_t blend = 0, depth_only = 0;
    std::map<uint32_t, VkPipeline> variants;   // VK_NULL_HANDLE: that variant failed (logged once)
};


class VulkanDevice final : public IRenderDevice {
public:
    explicit VulkanDevice(const VulkanDeviceInit &init)
        : inst_(init.instance), phys_(init.physical), dev_(init.device), qfam_(init.queue_family), queue_(init.queue),
          cache_path_(init.pipeline_cache_path) {
        glsl_compiler_init();
        ok_ = init_device();
        if (!ok_) vos_log("AR_VULKAN init failed: the device draws nothing");
        else vos_log("AR_VULKAN device %s ready (ring alignment %llu, timestamps %d)", props_.deviceName,
                     (unsigned long long)ring_align_, timestamps_ ? 1 : 0);
        done_thread_ = std::thread([this] { completion_loop(); });
    }
    ~VulkanDevice() override {
        wait_idle();
        {
            std::lock_guard<std::mutex> g(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (done_thread_.joinable()) done_thread_.join();
        vkDeviceWaitIdle(dev_);
        save_pipeline_cache();
        destroy_all();
        glsl_compiler_finish();
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Resources: textures and samplers
    // -----------------------------------------------------------------------------------------------------------------
    void set_edge_coverage(bool on) override { edge_ = on; }
    void set_texture_filter(TextureFilter f) override { filter_ = f; }   // samplers are keyed by the filter: none retired
    // Synchronous: the pixels are copied, the mip chain blitted and the image left SHADER_READ_ONLY_OPTIMAL before it
    // returns (its own submission; the frame's command buffer is untouched).
    void upload_texture(uint32_t tex_id, uint32_t w, uint32_t h, const uint8_t *rgba) override {
        if (!ok_ || !tex_id || !w || !h || !rgba) return;
        uint32_t levels = 1;
        if (w > 1 || h > 1) levels = (uint32_t)std::floor(std::log2((double)std::max(w, h))) + 1;
        Image im;
        if (!make_image(VK_IMAGE_TYPE_2D, kColorFormat, {w, h, 1}, levels, 1,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &im) ||
            !(im.view = make_view(im.img, VK_IMAGE_VIEW_TYPE_2D, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, levels))) {
            destroy_image(im);
            vos_log("AR_VULKAN texture %u (%ux%u): image creation failed", tex_id, w, h);
            return;
        }
        Buffer st;
        const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &st)) { destroy_image(im); return; }
        std::memcpy(st.map, rgba, (size_t)bytes);
        VkCommandBuffer cb = begin_once();
        bool ok = cb != VK_NULL_HANDLE;
        if (ok) {
            image_barrier(cb, im.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cb, st.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            for (uint32_t i = 1; i < levels; i++) {
                image_barrier(cb, im.img, VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkImageBlit b{};
                b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
                b.srcOffsets[1] = {(int32_t)std::max(w >> (i - 1), 1u), (int32_t)std::max(h >> (i - 1), 1u), 1};
                b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
                b.dstOffsets[1] = {(int32_t)std::max(w >> i, 1u), (int32_t)std::max(h >> i, 1u), 1};
                vkCmdBlitImage(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                               VK_FILTER_LINEAR);
                image_barrier(cb, im.img, VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            image_barrier(cb, im.img, VK_IMAGE_ASPECT_COLOR_BIT, levels - 1, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            ok = end_once();
        }
        destroy_buffer(st);
        if (!ok) { destroy_image(im); vos_log("AR_VULKAN texture %u: upload failed", tex_id); return; }
        auto old = textures_.find(tex_id);
        if (old != textures_.end()) retire_image(old->second);   // recorded or in-flight passes may still sample it
        textures_[tex_id] = im;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Targets
    // -----------------------------------------------------------------------------------------------------------------
    uint32_t create_target(uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override {
        targets_.emplace_back();
        make_target(targets_.back(), w, h, layers, samples);
        return (uint32_t)targets_.size();
    }
    void resize_target(uint32_t t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override {
        if (t == 0 || t > targets_.size()) return;
        Target &x = targets_[t - 1];
        if (x.w != std::max(w, 1u) || x.h != std::max(h, 1u) || x.layers != std::max(layers, 1u) || !x.color.img)
            make_target(x, w, h, layers, samples);
    }
    uint32_t supported_samples(uint32_t wanted, uint32_t layers) const override { (void)wanted; (void)layers; return 1; }
    uint32_t target_samples(uint32_t t) const override { return t == 0 || t > targets_.size() ? 1u : targets_[t - 1].samples; }
    void bind_target_texture(uint32_t tex_id, uint32_t target) override {   // recorded only: sampling it is plan 2
        if (!tex_id) return;
        if (target == 0 || target > targets_.size()) target_tex_.erase(tex_id);
        else target_tex_[tex_id] = target;
    }
    void release_target(uint32_t t) override {
        if (t == 0 || t > targets_.size()) return;
        Target &x = targets_[t - 1];
        flush(x);
        retire_target(x);
        x.w = 0; x.h = 0; x.samples = 1;
    }
    bool memoryless_msaa() const override {
        for (uint32_t i = 0; i < mem_.memoryTypeCount; i++)
            if (mem_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) return true;
        return false;
    }
    void set_rate_map(uint32_t target, void *map) override { (void)target; (void)map; }

    // -----------------------------------------------------------------------------------------------------------------
    // Recording
    // -----------------------------------------------------------------------------------------------------------------
    void encode_pass(uint32_t target, const PassDesc &pass, const ViewConstants &view, const fc_vertex *verts, uint32_t nv,
                     const DrawPacket *draws, uint32_t nd, const int8_t *normals, const float *prev_pos) override {
        if (!frame_open_ || target == 0 || target > targets_.size()) return;
        Target &t = targets_[target - 1];
        if (!t.color.img) return;
        if (pass.clear && t.nrec) flush(t);
        if (t.nrec == t.recs.size()) t.recs.emplace_back();
        Rec &r = t.recs[t.nrec++];
        r.pass = pass; r.view = view; r.light = light_; r.nv = nv;
        r.draws.clear();
        r.tex.clear();
        r.vb = r.nb = r.pb = VK_NULL_HANDLE; r.voff = r.noff = r.poff = 0;
        if (nv && nd && verts) {
            Slice s;
            if (!stage_bytes(verts, (size_t)nv * sizeof(fc_vertex), &s)) return;
            r.vb = s.buf; r.voff = s.off;
            if (normals && stage_bytes(normals, (size_t)nv * 4, &s)) { r.nb = s.buf; r.noff = s.off; }
            if (prev_pos && view.interp_on && stage_bytes(prev_pos, (size_t)nv * 3 * sizeof(float), &s)) { r.pb = s.buf; r.poff = s.off; }
            r.draws.assign(draws, draws + nd);
            r.tex.resize((size_t)nd * 2);
            for (uint32_t i = 0; i < nd; i++) { r.tex[2 * i] = texture(draws[i].tex[0]); r.tex[2 * i + 1] = texture(draws[i].tex[1]); }
        }
    }
    void set_lighting(const LightingFrame *lf) override {
        light_ = LightU{};
        if (!lf || !lf->on) return;
        for (int k = 0; k < 3; k++) { light_.dir_on[k] = lf->dir[k]; light_.tint_amb[k] = lf->tint[k]; }
        light_.dir_on[3] = 1.f;
        light_.tint_amb[3] = lf->ambient;
        light_.contact[0] = lf->contact ? lf->contact_strength : 0.f;
        light_.contact[1] = 0.03f * lf->upm;
        light_.contact[2] = 0.015f * lf->upm;
        light_.contact[3] = lf->ground_y;
        // plan 2: the underside and shadow maps, AO, reflections and the water footprint (their textures are the
        // placeholders and their switches stay off)
        light_.shadow[1] = 1.f + 2.f * lf->softness;
        light_.cast[0] = lf->table_cast;
        light_.relight[0] = lf->relight ? 1.f : 0.f;
        light_.relight[1] = lf->relight_strength;
        light_.toy[0] = lf->toy ? 1.f : 0.f;
        light_.toy[1] = lf->gloss;
        light_.toy[2] = lf->rim;
        light_.env5[3] = 1.f;
        if (lf->liquid) {
            const uint32_t n = std::min<uint32_t>(lf->water_count, 8u);
            light_.liquid[0] = 1.f; light_.liquid[1] = lf->liquid_time_s; light_.liquid[2] = (float)n; light_.liquid[3] = 0.25f * lf->liquid_upm;
            light_.liquid2[0] = 6.2831853f / (0.04f * lf->liquid_upm); light_.liquid2[1] = 0.08f;
            light_.liquid2[3] = 0.01f * lf->liquid_upm;
            for (uint32_t i = 0; i < n; i++) {
                for (int k = 0; k < 4; k++) light_.water_box[i][k] = lf->water[i][k];
                light_.water_y[i / 4][i % 4] = lf->water[i][4];
            }
        }
        if (lf->cut && lf->cut_radius > 0.f) {
            for (int k = 0; k < 3; k++) light_.cut[k] = lf->cut_target[k];
            light_.cut[3] = lf->cut_radius;
            light_.cut2[0] = 0.f; light_.cut2[1] = lf->cut_keep; light_.cut2[2] = std::max(lf->cut_cell, 1e-4f);
        }
        if (lf->trans_kind) {
            light_.trans[0] = (float)lf->trans_kind; light_.trans[1] = lf->trans_cover; light_.trans[2] = lf->trans_radius; light_.trans[3] = lf->trans_soft;
            for (int k = 0; k < 3; k++) { light_.trans_rgb[k] = lf->trans_rgb[k]; light_.trans_c[k] = lf->trans_centre[k]; }
        }
        if (lf->fine_textures && filter_ == TextureFilter::smooth) { light_.tex[0] = 1.f; light_.tex[1] = 0.25f; }
    }
    // plan 2: the depth maps, AO, the environment reduction and composite
    void encode_underside_map(const UndersideDesc &, const fc_vertex *, uint32_t, const DrawPacket *, uint32_t, const DepthExtras &) override {}
    void encode_shadow_map(const ShadowFit &, const fc_vertex *, uint32_t, const DrawPacket *, uint32_t, const DepthExtras &) override {}
    bool bake_ao(uint32_t, const AoVolume &, const std::vector<ShadowFit> &, float, const fc_vertex *, uint32_t, const DrawPacket *,
                 uint32_t, const DepthExtras &) override { return false; }
    void encode_environment_reduce(void *, uint32_t) override {}
    bool environment_stats(EnvStats *, uint32_t *) const override { return false; }
    void encode_composite(uint32_t, void *, uint32_t, uint32_t) override {}

    // The full-screen copy of one target slice into the external image's slice dst.slice (metal_device.mm encode_resolve).
    void encode_resolve(uint32_t src, const ExternalView &dst) override {
        if (!frame_open_ || src == 0 || src > targets_.size() || !dst.color) return;
        Target &s = targets_[src - 1];
        if (!s.color.img) return;
        flush(s);
        const VulkanExternalImage &ci = *static_cast<const VulkanExternalImage *>(dst.color);
        const VulkanExternalImage *di = static_cast<const VulkanExternalImage *>(dst.depth);
        if (dst.src_layer >= s.layers) { vos_log("AR_VULKAN resolve of slice %u from a target with %u", dst.src_layer, s.layers); return; }
        if (dst.slice >= ci.layers || (di && dst.slice >= di->layers)) { vos_log("AR_VULKAN resolve into slice %u of %u", dst.slice, ci.layers); return; }
        if (di && !s.depth_sample) {
            if (!depth_sample_warned_) vos_log("AR_VULKAN resolve: target depth is not sampleable here: colour only");
            depth_sample_warned_ = true;
            di = nullptr;
        }
        VkRenderPass rp = resolve_render_pass(ci.format, di ? di->format : VK_FORMAT_UNDEFINED);
        VkPipeline ps = rp ? resolve_pipeline(rp, di != nullptr) : VK_NULL_HANDLE;
        if (!ps) { vos_log("AR_VULKAN no resolve pipeline for colour %d / depth %d", (int)ci.format, di ? (int)di->format : 0); return; }
        VkImageView views[2] = {make_view(ci.image, VK_IMAGE_VIEW_TYPE_2D, ci.format, VK_IMAGE_ASPECT_COLOR_BIT, dst.slice, 1, 1),
                                di ? make_view(di->image, VK_IMAGE_VIEW_TYPE_2D, di->format, depth_aspects(di->format), dst.slice, 1, 1)
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
        if (!fb) { vos_log("AR_VULKAN resolve: framebuffer creation failed"); return; }
        VkDescriptorSet set = alloc_set(resolve_set_layout_);
        if (!set) return;
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
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp;
        bi.framebuffer = fb;
        bi.renderArea = {{0, 0}, {ci.width, ci.height}};
        vkCmdBeginRenderPass(cmd_, &bi, VK_SUBPASS_CONTENTS_INLINE);
        Viewport v = dst.viewport;
        if (v.w <= 0 || v.h <= 0) v = Viewport{0.f, 0.f, (float)ci.width, (float)ci.height};
        const VkViewport vp{v.x, v.y, v.w, v.h, 0.f, 1.f};   // no flip: the triangle covers it and reads gl_FragCoord
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        VkRect2D sc;
        if (!scissor_for(v, ci.width, ci.height, &sc)) { vkCmdEndRenderPass(cmd_); return; }
        vkCmdSetScissor(cmd_, 0, 1, &sc);
        const uint32_t vw = std::max(1u, (uint32_t)std::ceil(v.w)), vh = std::max(1u, (uint32_t)std::ceil(v.h));
        const ResolveU u = {{v.x, v.y}, {dst.src_x, dst.src_y}, {dst.src_x + vw - 1, dst.src_y + vh - 1}, dst.linearize ? 1u : 0u,
                            dst.src_layer};
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ps);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_layout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd_, resolve_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, &u);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd_);
        encoders_++;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Frame
    // -----------------------------------------------------------------------------------------------------------------
    void begin_frame() override {
        if (!ok_ || frame_open_) return;
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
        f.ring_used = 0;
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
        encoders_ = 0;
        for (Target &t : targets_) t.encoded = false;
    }
    void end_frame(void (*done)(void *), void *ctx, void *drawable) override {
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
    }
    void wait_idle() override {
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return completed_serial_ >= submitted_serial_; });
        }
        collect_garbage();
    }
    double last_gpu_ms() const override { return gpu_ms_.load(); }
    uint32_t render_encoders_last_frame() const override { return encoders_last_; }
    uint64_t late_passes() const override { return 0; }
    void *command_buffer() override { return (void *)cmd_; }

    // -----------------------------------------------------------------------------------------------------------------
    // Read-back (between frames: what the submitted frames left in the target)
    // -----------------------------------------------------------------------------------------------------------------
    bool read_target(uint32_t t, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override {
        if (t == 0 || t > targets_.size() || !targets_[t - 1].color.img) return false;
        Target &x = targets_[t - 1];
        std::vector<uint8_t> bytes;
        if (!read_image(x, false, &bytes)) return false;
        w = x.w; h = x.h;
        rgba = std::move(bytes);
        return true;
    }
    bool read_target_depth(uint32_t t, std::vector<float> &out, uint32_t &w, uint32_t &h) override {
        if (t == 0 || t > targets_.size() || !targets_[t - 1].depth.img) return false;
        Target &x = targets_[t - 1];
        std::vector<uint8_t> bytes;
        if (!read_image(x, true, &bytes)) return false;
        w = x.w; h = x.h;
        out.resize((size_t)w * h);
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Pipelines (public face)
    // -----------------------------------------------------------------------------------------------------------------
    bool compile_combiner(const fc_combiner_key &key, std::string *error, uint32_t samples) override {
        (void)samples;
        DrawPacket d{};
        d.cc = normalize_combiner(key);
        d.blend = (d.cc.opts & FC_CC_ALPHA) != 0;
        return pipeline(d, true, error, false) != nullptr;
    }
    std::string pipeline_list() const override {
        std::string out;
        char b[96];
        for (const auto &[k, p] : pipes_) {
            if (!p.ok) continue;
            const int len = std::snprintf(b, sizeof b, "%016llx %x %u %u %u %u %u %u %u %u", (unsigned long long)k.slots, k.opts, k.fmt,
                                          (unsigned)k.blend, (unsigned)k.depth, (unsigned)k.flat, (unsigned)k.depth_only,
                                          (unsigned)k.table_shadow, (unsigned)k.clip, (unsigned)k.samples);
            if (len > 0 && (size_t)len < sizeof b) std::snprintf(b + len, sizeof b - (size_t)len, k.cut ? " 1\n" : "\n");
            out += b;
        }
        return out;
    }
    size_t warm_pipelines(const std::string &list, uint32_t at_samples) override {
        (void)at_samples;
        size_t built = 0;
        for (const vkp::ListLine &l : vkp::parse_pipeline_lines(list)) {
            const size_t before = pipes_.size();
            pipeline(l.d, l.depth, nullptr, false);
            built += pipes_.size() - before;
        }
        return built;
    }
    // Synchronous in this plan: compiles now and returns how many it built.
    size_t warm_pipelines_background(const std::string &list, const std::vector<DrawPacket> &draws, uint32_t samples) override {
        (void)samples;
        std::vector<DrawPacket> want = draws;
        for (const vkp::ListLine &l : vkp::parse_pipeline_lines(list)) {
            if (!l.depth) continue;
            want.push_back(l.d);
            for (const DrawPacket &z : pipeline_siblings(l.d)) want.push_back(z);
        }
        size_t built = 0;
        for (const DrawPacket &d : want) {
            if (d.silhouette && decode_combiner(d.cc).unsupported) continue;
            const size_t before = pipes_.size();
            pipeline(d, true, nullptr, false);
            built += pipes_.size() - before;
        }
        return built;
    }
    size_t pipelines_warming() override { return 0; }
    size_t draw_time_builds() const override { return draw_builds_; }
    bool take_pipeline_error(std::string *why) override {
        if (refused_.empty()) return false;
        if (why) *why = refused_.front();
        refused_.erase(refused_.begin());
        return true;
    }
    size_t pipeline_count() const override {
        size_t n = 0;
        for (const auto &kv : pipes_) n += kv.second.ok ? 1 : 0;
        return n;
    }

private:
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> pools;
        size_t pool_at = 0;
        Buffer ring;
        VkDeviceSize ring_used = 0;
        uint64_t serial = 0;        // the frame last recorded in this slot
        bool submitted = false;     // its fence will signal (reset it before reuse)
    };
    struct Job { uint64_t serial; VkFence fence; VkQueryPool queries; void (*done)(void *); void *ctx; };
    struct Garbage { uint64_t serial; std::function<void()> free; };

    // -----------------------------------------------------------------------------------------------------------------
    // Initialisation and teardown
    // -----------------------------------------------------------------------------------------------------------------
    bool init_device() {
        if (!dev_ || !phys_ || !queue_) return false;
        vkGetPhysicalDeviceProperties(phys_, &props_);
        vkGetPhysicalDeviceMemoryProperties(phys_, &mem_);
        ring_align_ = std::max<VkDeviceSize>({256, props_.limits.minUniformBufferOffsetAlignment,
                                              props_.limits.minStorageBufferOffsetAlignment});
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qf.data());
        timestamps_ = qfam_ < qn && qf[qfam_].timestampValidBits > 0 && props_.limits.timestampPeriod > 0.f;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys_, kDepthFormat, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            vos_log("AR_VULKAN D32_SFLOAT_S8_UINT is not a depth attachment format here");
            return false;
        }
        depth_sampled_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        vkGetPhysicalDeviceFormatProperties(phys_, kPlaceholderDepth, &fp);
        const bool depth_linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = qfam_;
        if (vkCreateCommandPool(dev_, &pci, nullptr, &pool_) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
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

        // samplers
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev_, &si, nullptr, &linear_clamp_) != VK_SUCCESS) return false;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(dev_, &si, nullptr, &nearest_) != VK_SUCCESS) return false;
        si.magFilter = si.minFilter = depth_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.compareEnable = VK_TRUE;
        si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (vkCreateSampler(dev_, &si, nullptr, &compare_sampler_) != VK_SUCCESS) return false;

        // descriptor set layouts (glsl_emitter.cpp's prelude) and the pipeline layouts
        VkDescriptorSetLayoutBinding u[5] = {};
        const VkDescriptorType ut[5] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        const uint32_t ub[5] = {kBindView, kBindDraw, kBindLight, kBindNormals, kBindPrevPos};
        for (int i = 0; i < 5; i++) {
            u[i].binding = ub[i]; u[i].descriptorType = ut[i]; u[i].descriptorCount = 1;
            u[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 5;
        li.pBindings = u;
        if (vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set0_layout_) != VK_SUCCESS) return false;
        VkDescriptorSetLayoutBinding t[8] = {};
        for (uint32_t i = 0; i < 8; i++) {
            t[i].binding = i; t[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; t[i].descriptorCount = 1;
            t[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        static_assert(kBindTex0 == 0 && kBindWaterFoot == 7, "set 1 is bindings 0..7");
        li.bindingCount = 8;
        li.pBindings = t;
        if (vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set1_layout_) != VK_SUCCESS) return false;
        li.bindingCount = 2;   // the resolve: colour and depth, sampler2DArray
        if (vkCreateDescriptorSetLayout(dev_, &li, nullptr, &resolve_set_layout_) != VK_SUCCESS) return false;
        static_assert(kSetUniforms == 0 && kSetTextures == 1, "set order");
        VkDescriptorSetLayout sets[2] = {set0_layout_, set1_layout_};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = sets;
        if (vkCreatePipelineLayout(dev_, &pli, nullptr, &pipe_layout_) != VK_SUCCESS) return false;
        VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResolveU)};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &resolve_set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev_, &pli, nullptr, &resolve_layout_) != VK_SUCCESS) return false;

        // the targets' render passes: clear and load (compatible: one framebuffer, one pipeline set)
        if (!(rp_clear_ = vkp::make_target_pass(dev_, true)) || !(rp_load_ = vkp::make_target_pass(dev_, false))) return false;
        return make_placeholders() && make_resolve_shaders();
    }
    // white (texture id 0 and unknown ids), the depth maps' stand-in (depth 1: "nothing above"), AO 1, a black env cube
    bool make_placeholders() {
        const uint8_t white[4] = {255, 255, 255, 255};
        const uint8_t one = 255;
        const uint16_t black[6][4] = {{0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00},
                                      {0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00}, {0, 0, 0, 0x3c00}};
        const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!make_image(VK_IMAGE_TYPE_2D, kColorFormat, {1, 1, 1}, 1, 1, u, 0, &white_) ||
            !make_image(VK_IMAGE_TYPE_2D, kPlaceholderDepth, {1, 1, 1}, 1, 1, u, 0, &depth_ph_) ||
            !make_image(VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, {1, 1, 1}, 1, 1, u, 0, &ao_ph_) ||
            !make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {1, 1, 1}, 1, 6, u, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, &env_ph_))
            return false;
        white_.view = make_view(white_.img, VK_IMAGE_VIEW_TYPE_2D, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
        depth_ph_.view = make_view(depth_ph_.img, VK_IMAGE_VIEW_TYPE_2D, kPlaceholderDepth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1);
        ao_ph_.view = make_view(ao_ph_.img, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
        env_ph_.view = make_view(env_ph_.img, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 6, 1);
        if (!white_.view || !depth_ph_.view || !ao_ph_.view || !env_ph_.view) return false;
        Buffer st;
        if (!make_buffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &st)) return false;
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
        destroy_buffer(st);
        return ok;
    }
    bool make_resolve_shaders() {
        VkShaderModule m[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        std::string err;
        const bool ok = vkp::make_resolve_shaders(dev_, m, &err);
        resolve_vs_ = m[0]; resolve_fs_ = m[1]; resolve_fs_depth_ = m[2];   // destroy_all frees whichever exist
        if (!ok) vos_log("AR_VULKAN resolve shader: %s", err.c_str());
        return ok;
    }
    void load_pipeline_cache() {
        std::vector<char> data;
        if (!cache_path_.empty()) {
            std::ifstream in(cache_path_, std::ios::binary);
            if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) {   // a foreign or damaged file: start empty
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) cache_ = VK_NULL_HANDLE;
        }
        if (!data.empty()) vos_log("AR_VULKAN pipeline cache: %zu bytes from %s", data.size(), cache_path_.c_str());
    }
    void save_pipeline_cache() {
        if (!cache_ || cache_path_.empty()) return;
        size_t n = 0;
        if (vkGetPipelineCacheData(dev_, cache_, &n, nullptr) != VK_SUCCESS || n == 0) return;
        std::vector<char> data(n);
        if (vkGetPipelineCacheData(dev_, cache_, &n, data.data()) != VK_SUCCESS) return;
        std::ofstream out(cache_path_, std::ios::binary | std::ios::trunc);
        out.write(data.data(), (std::streamsize)n);
        if (!out) vos_log("AR_VULKAN pipeline cache: could not write %s", cache_path_.c_str());
    }
    void destroy_all() {
        while (!garbage_.empty()) { garbage_.front().free(); garbage_.pop_front(); }
        for (Target &t : targets_) destroy_target(t);
        for (auto &kv : textures_) destroy_image(kv.second);
        for (Image *im : {&white_, &depth_ph_, &ao_ph_, &env_ph_}) destroy_image(*im);
        for (auto &kv : pipes_) destroy_pipe(kv.second);
        for (auto &kv : resolve_pipes_) if (kv.second) vkDestroyPipeline(dev_, kv.second, nullptr);
        for (auto &kv : resolve_passes_) if (kv.second) vkDestroyRenderPass(dev_, kv.second, nullptr);
        for (VkShaderModule m : {resolve_vs_, resolve_fs_, resolve_fs_depth_}) if (m) vkDestroyShaderModule(dev_, m, nullptr);
        for (auto &kv : samplers_) vkDestroySampler(dev_, kv.second, nullptr);
        for (VkSampler s : {linear_clamp_, nearest_, compare_sampler_}) if (s) vkDestroySampler(dev_, s, nullptr);
        for (Frame &f : frames_) {
            for (VkDescriptorPool p : f.pools) vkDestroyDescriptorPool(dev_, p, nullptr);
            destroy_buffer(f.ring);
            if (f.fence) vkDestroyFence(dev_, f.fence, nullptr);
            if (f.queries) vkDestroyQueryPool(dev_, f.queries, nullptr);
        }
        if (once_fence_) vkDestroyFence(dev_, once_fence_, nullptr);
        if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);   // frees every command buffer
        for (VkRenderPass rp : {rp_clear_, rp_load_}) if (rp) vkDestroyRenderPass(dev_, rp, nullptr);
        for (VkPipelineLayout l : {pipe_layout_, resolve_layout_}) if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
        for (VkDescriptorSetLayout l : {set0_layout_, set1_layout_, resolve_set_layout_}) if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
        if (cache_) vkDestroyPipelineCache(dev_, cache_, nullptr);
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Memory, buffers, images
    // -----------------------------------------------------------------------------------------------------------------
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) const {
        for (VkMemoryPropertyFlags f : {want, fallback})
            for (uint32_t i = 0; i < mem_.memoryTypeCount; i++)
                if ((bits & (1u << i)) && (mem_.memoryTypes[i].propertyFlags & f) == f) return i;
        return UINT32_MAX;
    }
    bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer *out) {
        *out = Buffer{};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev_, &bi, nullptr, &out->buf) != VK_SUCCESS) { out->buf = VK_NULL_HANDLE; return false; }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev_, out->buf, &req);
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const uint32_t type = host ? memory_type(req.memoryTypeBits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, hv)
                                   : memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(dev_, &ai, nullptr, &out->mem) != VK_SUCCESS ||
            vkBindBufferMemory(dev_, out->buf, out->mem, 0) != VK_SUCCESS ||
            (host && vkMapMemory(dev_, out->mem, 0, VK_WHOLE_SIZE, 0, (void **)&out->map) != VK_SUCCESS)) {
            vos_log("AR_VULKAN buffer of %llu bytes: allocation failed", (unsigned long long)size);
            destroy_buffer(*out);
            return false;
        }
        out->size = size;
        return true;
    }
    void destroy_buffer(Buffer &b) {
        if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
        if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);   // unmaps
        b = Buffer{};
    }
    bool make_image(VkImageType type, VkFormat fmt, VkExtent3D ext, uint32_t levels, uint32_t layers, VkImageUsageFlags usage,
                    VkImageCreateFlags flags, Image *out) {
        *out = Image{};
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.flags = flags;
        ii.imageType = type;
        ii.format = fmt;
        ii.extent = ext;
        ii.mipLevels = levels;
        ii.arrayLayers = layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev_, &ii, nullptr, &out->img) != VK_SUCCESS) { out->img = VK_NULL_HANDLE; return false; }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev_, out->img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev_, &ai, nullptr, &out->mem) != VK_SUCCESS ||
            vkBindImageMemory(dev_, out->img, out->mem, 0) != VK_SUCCESS) {
            vos_log("AR_VULKAN image %ux%u format %d: allocation failed", ext.width, ext.height, (int)fmt);
            destroy_image(*out);
            return false;
        }
        return true;
    }
    VkImageView make_view(VkImage img, VkImageViewType type, VkFormat fmt, VkImageAspectFlags aspect, uint32_t base_layer,
                          uint32_t layers, uint32_t levels) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = type;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, levels, base_layer, layers};
        VkImageView v = VK_NULL_HANDLE;
        return vkCreateImageView(dev_, &vi, nullptr, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
    }
    void destroy_image(Image &im) {
        if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
        if (im.img) vkDestroyImage(dev_, im.img, nullptr);
        if (im.mem) vkFreeMemory(dev_, im.mem, nullptr);
        im = Image{};
    }
    // destroyed once every frame begun so far completed (it may be recorded in the open frame or one in flight)
    void retire(std::function<void()> f) { garbage_.push_back(Garbage{frame_serial_, std::move(f)}); }
    void retire_image(Image im) { retire([this, im]() mutable { destroy_image(im); }); }
    void collect_garbage() {
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
    // A one-off submission on the render thread, waited for (uploads, read-back, placeholders).
    VkCommandBuffer begin_once() {
        if (vkResetCommandBuffer(once_cmd_, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(once_cmd_, &bi) == VK_SUCCESS ? once_cmd_ : VK_NULL_HANDLE;
    }
    bool end_once() {
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
    // The frame's ring: vertices, normals, previous positions and the uniforms (one host-visible buffer per frame in flight,
    // doubled when full; the full one is retired with this frame).
    bool ring_alloc(VkDeviceSize n, Slice *out) {
        Frame &f = frames_[slot_];
        VkDeviceSize at = align_up(f.ring_used, ring_align_);
        if (!f.ring.buf || at + n > f.ring.size) {
            const VkDeviceSize len = std::max<VkDeviceSize>({f.ring.size * 2, n, 1u << 20});
            Buffer nb;
            if (!make_buffer(len, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             true, &nb))
                return false;
            if (f.ring.buf) {
                Buffer old = f.ring;
                retire([this, old]() mutable { destroy_buffer(old); });
            }
            f.ring = nb;
            at = 0;
        }
        *out = Slice{f.ring.buf, at, f.ring.map + at};
        f.ring_used = at + n;
        return true;
    }
    bool stage_bytes(const void *src, size_t n, Slice *out) {
        if (!ring_alloc(n, out)) return false;
        std::memcpy(out->ptr, src, n);
        return true;
    }
    VkDescriptorSet alloc_set(VkDescriptorSetLayout layout) {
        Frame &f = frames_[slot_];
        for (;;) {
            if (f.pool_at == f.pools.size()) {
                const uint32_t kSets = 1024;
                const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 8},
                                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSets},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets}};
                VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pi.maxSets = kSets;
                pi.poolSizeCount = 3;
                pi.pPoolSizes = sizes;
                VkDescriptorPool p = VK_NULL_HANDLE;
                if (vkCreateDescriptorPool(dev_, &pi, nullptr, &p) != VK_SUCCESS) { vos_log("AR_VULKAN descriptor pool failed"); return VK_NULL_HANDLE; }
                f.pools.push_back(p);
            }
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = f.pools[f.pool_at];
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &layout;
            VkDescriptorSet s = VK_NULL_HANDLE;
            const VkResult r = vkAllocateDescriptorSets(dev_, &ai, &s);
            if (r == VK_SUCCESS) return s;
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
                vos_log("AR_VULKAN vkAllocateDescriptorSets failed: %d", (int)r);
                return VK_NULL_HANDLE;
            }
            f.pool_at++;
        }
    }
    VkSampler sampler(const fc_sampler &s) {
        const bool smooth = filter_ == TextureFilter::smooth;
        const uint32_t key = s.linear | (uint32_t)s.wrap_s << 8 | (uint32_t)s.wrap_t << 16 | (smooth ? 1u << 24 : 0u);
        auto it = samplers_.find(key);
        if (it != samplers_.end()) return it->second;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        const VkFilter own = s.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.magFilter = own;
        if (smooth) {
            si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            si.maxLod = VK_LOD_CLAMP_NONE;
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = std::min(4.f, props_.limits.maxSamplerAnisotropy);
        } else {   // Metal's default: no mip filter, level 0 only
            si.minFilter = own;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.maxLod = 0.f;
        }
        si.addressModeU = address(s.wrap_s);
        si.addressModeV = address(s.wrap_t);
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSampler out = VK_NULL_HANDLE;
        if (vkCreateSampler(dev_, &si, nullptr, &out) != VK_SUCCESS) return linear_clamp_;
        samplers_[key] = out;
        return out;
    }
    VkImageView texture(uint32_t tex_id) const {
        if (!tex_id || target_tex_.count(tex_id)) return white_.view;   // a bound target samples white until plan 2
        auto it = textures_.find(tex_id);
        return it == textures_.end() ? white_.view : it->second.view;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Targets (internals)
    // -----------------------------------------------------------------------------------------------------------------
    void make_target(Target &t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) {
        (void)samples;   // rendered at 1 sample in this plan
        flush(t);
        retire_target(t);
        w = std::max(w, 1u); h = std::max(h, 1u); layers = std::max(layers, 1u);
        t.w = w; t.h = h; t.layers = layers; t.samples = 1;
        if (!ok_) return;
        const VkImageUsageFlags su = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        bool ok = make_image(VK_IMAGE_TYPE_2D, kColorFormat, {w, h, 1}, 1, layers, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | su, 0, &t.color) &&
                  make_image(VK_IMAGE_TYPE_2D, kDepthFormat, {w, h, 1}, 1, layers,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 (depth_sampled_ ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                             0, &t.depth);
        if (ok) {
            t.color.view = make_view(t.color.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers, 1);
            t.depth.view = make_view(t.depth.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kDepthFormat, depth_aspects(kDepthFormat), 0, layers, 1);
            if (depth_sampled_)
                t.depth_sample = make_view(t.depth.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 0, layers, 1);
            ok = t.color.view && t.depth.view && (!depth_sampled_ || t.depth_sample);
        }
        if (ok) {
            VkImageView att[2] = {t.color.view, t.depth.view};
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = rp_clear_;
            fi.attachmentCount = 2;
            fi.pAttachments = att;
            fi.width = w; fi.height = h; fi.layers = layers;
            ok = vkCreateFramebuffer(dev_, &fi, nullptr, &t.fb) == VK_SUCCESS;
            if (!ok) t.fb = VK_NULL_HANDLE;
        }
        if (!ok) {
            vos_log("AR_VULKAN target %ux%ux%u: creation failed", w, h, layers);
            destroy_target(t);
        }
        t.color_layout = t.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    void retire_target(Target &t) {
        Target old;
        old.color = t.color; old.depth = t.depth; old.depth_sample = t.depth_sample; old.fb = t.fb;
        t.color = Image{}; t.depth = Image{}; t.depth_sample = VK_NULL_HANDLE; t.fb = VK_NULL_HANDLE;
        t.color_layout = t.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (old.color.img || old.depth.img || old.fb) retire([this, old]() mutable { destroy_target(old); });
    }
    void destroy_target(Target &t) {
        if (t.fb) vkDestroyFramebuffer(dev_, t.fb, nullptr);
        if (t.depth_sample) vkDestroyImageView(dev_, t.depth_sample, nullptr);
        destroy_image(t.color);
        destroy_image(t.depth);
        t.fb = VK_NULL_HANDLE;
        t.depth_sample = VK_NULL_HANDLE;
    }
    void transition_color(Target &t, VkImageLayout to, bool discard = false) {
        image_barrier(cmd_or_once(), t.color.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, t.layers, t.color_layout, to, discard);
        t.color_layout = to;
    }
    void transition_depth(Target &t, VkImageLayout to, bool discard = false) {
        image_barrier(cmd_or_once(), t.depth.img, depth_aspects(kDepthFormat), 0, 1, t.layers, t.depth_layout, to, discard);
        t.depth_layout = to;
    }
    VkCommandBuffer cmd_or_once() const { return recording_once_ ? once_cmd_ : cmd_; }
    // layer 0 of the colour (RGBA8) or the depth (D32 floats) through a host-visible buffer
    bool read_image(Target &x, bool depth, std::vector<uint8_t> *out) {
        wait_idle();
        Buffer st;
        const VkDeviceSize bytes = (VkDeviceSize)x.w * x.h * 4;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &st)) return false;
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
        destroy_buffer(st);
        return ok;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Recording (internals): a target's passes in one render pass (metal_device.mm flush / encode_rec)
    // -----------------------------------------------------------------------------------------------------------------
    void flush(Target &t) {
        if (!t.nrec) return;
        const size_t n = t.nrec;
        t.nrec = 0;
        if (!frame_open_ || !t.fb) return;
        const PassDesc &p0 = t.recs[0].pass;
        transition_color(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, p0.clear);   // cleared: the old contents may go
        transition_depth(t, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, p0.clear);
        VkClearValue cv[2];
        for (int k = 0; k < 4; k++) cv[0].color.float32[k] = p0.clear_color[k];
        cv[1].depthStencil = {0.f, 0};
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = p0.clear ? rp_clear_ : rp_load_;
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
    bool scissor_for(const Viewport &v, uint32_t bw, uint32_t bh, VkRect2D *out) const {
        const uint32_t sx = (uint32_t)std::max(0.f, v.x), sy = (uint32_t)std::max(0.f, v.y);
        const uint32_t sw = std::min<uint32_t>((uint32_t)std::max(0.f, v.w), bw > sx ? bw - sx : 0);
        const uint32_t sh = std::min<uint32_t>((uint32_t)std::max(0.f, v.h), bh > sy ? bh - sy : 0);
        *out = VkRect2D{{(int32_t)sx, (int32_t)sy}, {sw, sh}};
        return sw && sh;
    }
    void encode_rec(const Target &t, const Rec &r) {
        Viewport v = r.pass.viewport;
        if (v.w <= 0 || v.h <= 0) v = Viewport{0, 0, float(t.w), float(t.h)};
        VkRect2D sc;
        if (!scissor_for(v, t.w, t.h, &sc) || !r.vb || r.draws.empty()) return;
        const VkViewport vp{v.x, v.y + v.h, v.w, -v.h, 0.f, 1.f};   // the Y flip (Metal's NDC +y up)
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &sc);
        const uint32_t nd = (uint32_t)r.draws.size();
        // the pass's uniforms in one block of the ring: ViewU, LightU, then one DrawU per draw
        const VkDeviceSize av = align_up(sizeof(ViewU), ring_align_), al = align_up(sizeof(LightU), ring_align_),
                           ad = align_up(sizeof(DrawU), ring_align_);
        Slice ub;
        if (!ring_alloc(av + al + ad * nd, &ub)) return;
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
        for (uint32_t i = 0; i < nd; i++) {
            const DrawPacket &d = r.draws[i];
            if (d.vertex_count == 0 || d.first_vertex + d.vertex_count > r.nv) continue;
            Pipe *p = pipeline(d, true, nullptr, true);
            if (!p) continue;
            const bool test = d.silhouette ? true : d.depth_test != 0, write = d.silhouette ? false : d.depth_write != 0;
            const VkCompareOp cmp = !r.pass.depth ? VK_COMPARE_OP_ALWAYS
                                  : d.silhouette ? VK_COMPARE_OP_LESS : test ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS;
            const VkCullModeFlags cull = d.cull == FC_CULL_BACK ? VK_CULL_MODE_BACK_BIT : d.cull == FC_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT
                                                                                                                : VK_CULL_MODE_NONE;
            VkPipeline ps = variant(*p, state_key(cmp, r.pass.depth && write, cull));
            if (!ps) continue;
            if (ps != bound) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ps); bound = ps; }
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
                s1 = texture_set(tex, samp);
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
    // set 1: the draw's two textures and the placeholders of the maps plan 2 renders
    VkDescriptorSet texture_set(const VkImageView tex[2], const VkSampler samp[2]) {
        VkDescriptorSet s = alloc_set(set1_layout_);
        if (!s) return VK_NULL_HANDLE;
        const VkImageLayout rd = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkDescriptorImageInfo ii[8] = {
            {samp[0], tex[0], rd}, {samp[1], tex[1], rd}, {compare_sampler_, depth_ph_.view, rd}, {nearest_, depth_ph_.view, rd},
            {linear_clamp_, ao_ph_.view, rd}, {linear_clamp_, env_ph_.view, rd}, {linear_clamp_, env_ph_.view, rd}, {nearest_, depth_ph_.view, rd}};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s;
        w.dstBinding = 0;
        w.descriptorCount = 8;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = ii;
        vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
        return s;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Pipelines (internals)
    // -----------------------------------------------------------------------------------------------------------------
    PipeKey pipe_key(const DrawPacket &draw, bool depth, uint32_t samples) const {
        const PipeIdentity k = pipe_identity(vkp::pipeline_form(draw));
        return PipeKey{k.slots, k.opts, (uint32_t)kColorFormat, k.blend, (uint8_t)depth, k.silhouette, k.depth_only, k.table_shadow,
                       k.clip, (uint8_t)samples, (uint8_t)(edge_ && samples > 1 && (k.opts & FC_CC_TEXTURE_EDGE) ? 1 : 0), k.cut,
                       (uint64_t)rp_clear_};
    }
    // at_draw: a draw needs it now (counted in draw_time_builds). The default variant is built with the shaders, so a
    // pipeline that exists has been accepted by the driver.
    Pipe *pipeline(const DrawPacket &d, bool depth, std::string *err, bool at_draw) {
        if (!ok_) { if (err) *err = "no device"; return nullptr; }
        if (d.silhouette && decode_combiner(d.cc).unsupported) return nullptr;
        const PipeKey pk = pipe_key(d, depth, 1);
        auto it = pipes_.find(pk);
        if (it != pipes_.end()) {
            if (!it->second.ok && err) *err = it->second.why;
            return it->second.ok ? &it->second : nullptr;
        }
        Pipe p;
        bool refused = false;
        p.ok = vkp::make_combiner_shaders(dev_, d, &p.vs, &p.fs, &p.why, &refused);
        if (p.ok) {
            p.blend = d.blend;
            p.depth_only = d.depth_only;
            VkPipeline v = build_variant(p, kDefaultState, &p.why);
            if (v) p.variants[kDefaultState] = v;
            else { p.ok = false; refused = true; destroy_pipe(p); }
        }
        if (refused) {
            vos_log("AR_VULKAN combiner %016llx/%x: %s", (unsigned long long)pk.slots, pk.opts, p.why.c_str());
            char head[80];
            std::snprintf(head, sizeof head, "combiner %016llx/%x samples=%u: ", (unsigned long long)pk.slots, pk.opts, (unsigned)pk.samples);
            refused_.push_back(head + p.why);
        }
        if (at_draw) draw_builds_++;
        if (!p.ok && err) *err = p.why;
        Pipe &slot = pipes_[pk] = std::move(p);
        return slot.ok ? &slot : nullptr;
    }
    void destroy_pipe(Pipe &p) {
        for (auto &kv : p.variants) if (kv.second) vkDestroyPipeline(dev_, kv.second, nullptr);
        p.variants.clear();
        if (p.vs) vkDestroyShaderModule(dev_, p.vs, nullptr);
        if (p.fs) vkDestroyShaderModule(dev_, p.fs, nullptr);
        p.vs = p.fs = VK_NULL_HANDLE;
    }
    VkPipeline variant(Pipe &p, uint32_t state) {
        auto it = p.variants.find(state);
        if (it != p.variants.end()) return it->second;
        std::string why;
        VkPipeline v = build_variant(p, state, &why);
        if (!v) {
            vos_log("AR_VULKAN pipeline variant %x: %s", state, why.c_str());
            refused_.push_back("pipeline variant: " + why);
        }
        p.variants[state] = v;
        return v;
    }
    VkPipeline build_variant(const Pipe &p, uint32_t state, std::string *why) {
        return vkp::make_draw_pipeline(dev_, vkp::DrawPipelineDesc{p.vs, p.fs, p.blend != 0, p.depth_only != 0, state, pipe_layout_, rp_clear_, cache_},
                                       why);
    }
    VkRenderPass resolve_render_pass(VkFormat cf, VkFormat df) {
        const uint64_t key = ((uint64_t)cf << 32) | (uint64_t)df;
        auto it = resolve_passes_.find(key);
        return it != resolve_passes_.end() ? it->second : (resolve_passes_[key] = vkp::make_resolve_pass(dev_, cf, df));
    }
    VkPipeline resolve_pipeline(VkRenderPass rp, bool depth) {
        auto it = resolve_pipes_.find((uint64_t)rp);
        if (it != resolve_pipes_.end()) return it->second;
        return resolve_pipes_[(uint64_t)rp] =
                   vkp::make_resolve_pipeline(dev_, cache_, resolve_layout_, rp, resolve_vs_, depth ? resolve_fs_depth_ : resolve_fs_, depth);
    }

    // -----------------------------------------------------------------------------------------------------------------
    // The completion thread: waits the frames' fences in submission order
    // -----------------------------------------------------------------------------------------------------------------
    void completion_loop() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return stop_ || !jobs_.empty(); });
                if (jobs_.empty()) return;   // stopping, nothing left
                j = jobs_.front();
            }
            if (j.fence) {
                const VkResult r = vkWaitForFences(dev_, 1, &j.fence, VK_TRUE, UINT64_MAX);
                if (r != VK_SUCCESS) vos_log("AR_VULKAN vkWaitForFences failed: %d", (int)r);
                uint64_t ts[2] = {0, 0};
                if (r == VK_SUCCESS && j.queries &&
                    vkGetQueryPoolResults(dev_, j.queries, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                    ts[1] >= ts[0])
                    gpu_ms_.store((double)(ts[1] - ts[0]) * props_.limits.timestampPeriod * 1e-6);
            }
            if (j.done) j.done(j.ctx);
            {
                std::lock_guard<std::mutex> g(m_);
                jobs_.pop_front();
                completed_serial_ = j.serial;
            }
            cv_.notify_all();
        }
    }

    VkInstance inst_;
    VkPhysicalDevice phys_;
    VkDevice dev_;
    uint32_t qfam_;
    VkQueue queue_;
    std::string cache_path_;
    bool ok_ = false;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties mem_{};
    VkDeviceSize ring_align_ = 256;
    bool timestamps_ = false;
    bool depth_sampled_ = false;
    bool depth_sample_warned_ = false;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer once_cmd_ = VK_NULL_HANDLE;
    VkFence once_fence_ = VK_NULL_HANDLE;
    bool recording_once_ = false;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    VkSampler linear_clamp_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE, compare_sampler_ = VK_NULL_HANDLE;
    std::map<uint32_t, VkSampler> samplers_;
    VkDescriptorSetLayout set0_layout_ = VK_NULL_HANDLE, set1_layout_ = VK_NULL_HANDLE, resolve_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE, resolve_layout_ = VK_NULL_HANDLE;
    VkRenderPass rp_clear_ = VK_NULL_HANDLE, rp_load_ = VK_NULL_HANDLE;
    VkShaderModule resolve_vs_ = VK_NULL_HANDLE, resolve_fs_ = VK_NULL_HANDLE, resolve_fs_depth_ = VK_NULL_HANDLE;
    std::map<uint64_t, VkRenderPass> resolve_passes_;
    std::map<uint64_t, VkPipeline> resolve_pipes_;
    Image white_, depth_ph_, ao_ph_, env_ph_;
    std::unordered_map<uint32_t, Image> textures_;
    std::map<uint32_t, uint32_t> target_tex_;
    std::deque<Target> targets_;   // a deque: create_target never moves the targets (flush holds references)
    std::map<PipeKey, Pipe> pipes_;
    std::vector<std::string> refused_;
    size_t draw_builds_ = 0;
    bool edge_ = false;
    TextureFilter filter_ = TextureFilter::smooth;
    LightU light_{};
    // frames
    Frame frames_[kFrames];
    uint32_t slot_ = 0;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;   // the open frame's
    bool frame_open_ = false;
    uint64_t frame_serial_ = 0;              // the last frame begun
    uint32_t encoders_ = 0, encoders_last_ = 0;
    std::deque<Garbage> garbage_;
    std::atomic<double> gpu_ms_{-1.0};
    // shared with the completion thread (under m_)
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    uint64_t completed_serial_ = 0, submitted_serial_ = 0;
    bool stop_ = false;
    std::thread done_thread_;
};
}

std::unique_ptr<IRenderDevice> create_vulkan_device(const VulkanDeviceInit &init) {
    return std::unique_ptr<IRenderDevice>(new VulkanDevice(init));
}
}
