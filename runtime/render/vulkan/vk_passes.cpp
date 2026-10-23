#include <algorithm>
#include <cmath>
#include <cstring>
#include "vk_device.h"
#include "vk_layout.h"
#include "vos_log.h"
// The room-lit look's passes (metal_device.mm set_lighting, encode_depth_map, the shadow / underside / footprint maps,
// bake_ao, encode_composite). The class and the Vulkan-specific notes: vk_device.h.
namespace ar {
namespace vkd {
using vk::image_barrier;
using vk::kColorFormat;
using vk::kMapFormat;
namespace {
const VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kAttach = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
// depth.vert's blocks (glsl_fixed.h)
struct DepthU { float interp[4]; float wclip_lo[4]; float wclip_hi[4]; };
static_assert(sizeof(DepthU) == 48, "DepthU matches the GLSL block");
// ao_fill.comp's AoU: Metal's members, then the views' (u, v, depth) matrices
struct AoU { float min_voxel[4]; uint32_t dims_n[4]; float range[4]; float uv[kAoMaxViews][16]; };
static_assert(sizeof(AoU) == 48 + 32 * 64, "AoU matches the GLSL block (std140)");
}

// ---------------------------------------------------------------------------------------------------------------------
// The lighting constants (metal_device.mm set_lighting). The environment cubes are always the placeholder here: the
// Quest has no probe, so reflections stay off (toy.w 0).
// ---------------------------------------------------------------------------------------------------------------------
void VulkanDevice::set_lighting(const LightingFrame *lf) {
    light_ = LightU{};
    ao_bound_ = VK_NULL_HANDLE;
    if (!lf || !lf->on) return;
    for (int k = 0; k < 3; k++) { light_.dir_on[k] = lf->dir[k]; light_.tint_amb[k] = lf->tint[k]; }
    light_.dir_on[3] = 1.f;
    light_.tint_amb[3] = lf->ambient;
    light_.contact[0] = lf->contact ? lf->contact_strength : 0.f;
    light_.contact[1] = 0.03f * lf->upm;
    light_.contact[2] = 0.015f * lf->upm;
    light_.contact[3] = lf->ground_y;
    uv_from_clip(under_desc_.clip_from_level, light_.under_uv);
    light_.under_range[0] = under_desc_.y0;
    light_.under_range[1] = under_desc_.span;
    // PCF radius 1..3 texels; the receiver is pushed 3 texels along its face normal
    light_.shadow[0] = shadow_valid_ && lf->shadows ? 1.f : 0.f;
    light_.shadow[1] = 1.f + 2.f * lf->softness;
    light_.shadow[2] = 1.f / (float)kShadowSize;
    light_.shadow[3] = 3.f * shadow_fit_.texel_units;
    uv_from_clip(shadow_fit_.clip_from_level, light_.shadow_uv);
    light_.cast[0] = lf->table_cast;
    light_.relight[0] = lf->relight ? 1.f : 0.f;
    light_.relight[1] = lf->relight_strength;
    // AO: only the bake of this frame's scene (the renderer's ao_ready / ao_slot), never another slot's volume
    if (lf->ao && lf->ao_ready && lf->ao_slot < kAoSlots && ao_[lf->ao_slot].view) {
        const AoVolume &v = ao_vol_[lf->ao_slot];
        ao_bound_ = ao_[lf->ao_slot].view;
        light_.ao[0] = 1.f;
        light_.ao[1] = lf->ao_strength;
        for (int k = 0; k < 3; k++) { light_.ao_min[k] = v.min[k]; light_.ao_inv[k] = 1.f / (float)v.dims[k]; }
        light_.ao_min[3] = v.voxel;
    }
    light_.toy[0] = lf->toy ? 1.f : 0.f;
    light_.toy[1] = lf->gloss;
    light_.toy[2] = lf->rim;
    light_.toy[3] = 0.f;   // no probe cube bound
    for (int k = 0; k < 3; k++) {
        light_.env0[k] = lf->env_from_level[k]; light_.env1[k] = lf->env_from_level[3 + k]; light_.env2[k] = lf->env_from_level[6 + k];
        light_.env3[k] = lf->env_prev_from_level[k]; light_.env4[k] = lf->env_prev_from_level[3 + k];
        light_.env5[k] = lf->env_prev_from_level[6 + k];
    }
    light_.env1[3] = lf->env_exposure;
    light_.env4[3] = lf->env_prev_exposure;
    light_.env5[3] = 1.f;   // the incoming cube's share: no outgoing cube
    if (lf->liquid) {
        const uint32_t n = std::min<uint32_t>(lf->water_count, 8u);
        light_.liquid[0] = 1.f; light_.liquid[1] = lf->liquid_time_s; light_.liquid[2] = (float)n; light_.liquid[3] = 0.25f * lf->liquid_upm;
        light_.liquid2[0] = 6.2831853f / (0.04f * lf->liquid_upm); light_.liquid2[1] = 0.08f;
        light_.liquid2[2] = lf->water_trim && foot_valid_ && n > 0 ? 1.f : 0.f;
        light_.liquid2[3] = 0.01f * lf->liquid_upm;
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 4; k++) light_.water_box[i][k] = lf->water[i][k];
            light_.water_y[i / 4][i % 4] = lf->water[i][4];
        }
        if (light_.liquid2[2] > 0.5f) {
            light_.water_foot[0] = foot_window_[0]; light_.water_foot[1] = foot_window_[1];
            light_.water_foot[2] = 1.f / (foot_window_[2] - foot_window_[0]); light_.water_foot[3] = 1.f / (foot_window_[3] - foot_window_[1]);
        }
    }
    if (lf->cut && lf->cut_radius > 0.f) {
        for (int k = 0; k < 3; k++) light_.cut[k] = lf->cut_target[k];
        light_.cut[3] = lf->cut_radius;
        // the core keeps none of its cells: an open hole
        light_.cut2[0] = 0.f; light_.cut2[1] = lf->cut_keep; light_.cut2[2] = std::max(lf->cut_cell, 1e-4f);
    }
    if (lf->trans_kind) {
        light_.trans[0] = (float)lf->trans_kind; light_.trans[1] = lf->trans_cover; light_.trans[2] = lf->trans_radius; light_.trans[3] = lf->trans_soft;
        for (int k = 0; k < 3; k++) { light_.trans_rgb[k] = lf->trans_rgb[k]; light_.trans_c[k] = lf->trans_centre[k]; }
    }
    if (lf->fine_textures && filter_ == TextureFilter::smooth) { light_.tex[0] = 1.f; light_.tex[1] = 0.25f; }
}

// ---------------------------------------------------------------------------------------------------------------------
// The depth maps
// ---------------------------------------------------------------------------------------------------------------------
bool VulkanDevice::ensure_map(Map &m, uint32_t size) {
    if (m.im.img && m.fb && m.size == size) return true;
    destroy_map(m);   // never made, or a failed one: nothing samples it
    vk::ImageDesc d;
    d.format = kMapFormat; d.extent = {size, size, 1};
    d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = res_.make_image(d, &m.im) &&
              (m.im.view = res_.make_view(m.im.img, VK_IMAGE_VIEW_TYPE_2D, kMapFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1)) != VK_NULL_HANDLE;
    if (ok) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = fixed_layouts_.map_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &m.im.view;
        fi.width = fi.height = size; fi.layers = 1;
        ok = vkCreateFramebuffer(dev_, &fi, nullptr, &m.fb) == VK_SUCCESS;
        if (!ok) m.fb = VK_NULL_HANDLE;
    }
    if (!ok) { vos_log("AR_VULKAN depth map %u x %u: creation failed", size, size); destroy_map(m); return false; }
    m.size = size;
    m.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}
void VulkanDevice::destroy_map(Map &m) {
    if (m.fb) vkDestroyFramebuffer(dev_, m.fb, nullptr);
    res_.destroy_image(m.im);
    m = Map{};
}
// Before a map pass overwrites a map, the targets whose recorded passes sample it are encoded (metal_device.mm).
void VulkanDevice::flush_sampling(VkImageView map) {
    if (!map) return;
    for (Target &t : targets_)
        for (size_t i = 0; i < t.nrec; i++)
            if (t.recs[i].shadow == map || t.recs[i].under == map || t.recs[i].foot == map) { flush(t); break; }
}
// A cutout's texture for a depth pass: an uploaded texture (white for none or a bound target, whose layout the map pass
// does not track). bake: the AO bake's command buffer runs before the open frame's, so a texture whose upload that frame
// records is white too.
VkImageView VulkanDevice::bake_texture(uint32_t tex_id, bool bake) const {
    if (!tex_id || target_tex_.count(tex_id)) return white_.view;
    auto it = textures_.find(tex_id);
    if (it == textures_.end()) return white_.view;
    if (bake && std::find(fresh_images_.begin(), fresh_images_.end(), it->second.im.img) != fresh_images_.end()) return white_.view;
    return it->second.im.view;
}
// metal_device.mm encode_depth_map: cleared to 1, LESS with depth writes, no culling, slope-scaled bias; the cut
// pipeline (depth_cut.frag) for an edge-cut draw with a texture.
void VulkanDevice::encode_depth_map(const MapDraw &md, const DrawPacket *draws, uint32_t nd, const DepthExtras &x) {
    const vk::FixedPipes &fp = fixed_pipes();
    const int cl = md.clamp ? 1 : 0;
    VkClearValue cv{};
    cv.depthStencil = {1.f, 0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = fixed_layouts_.map_pass;
    bi.framebuffer = md.fb;
    bi.renderArea = {{0, 0}, {md.size, md.size}};
    bi.clearValueCount = 1;
    bi.pClearValues = &cv;
    vkCmdBeginRenderPass(md.cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkDeviceSize am = vk::align_up(64, ring_align_), ad = vk::align_up(sizeof(DepthU), ring_align_);
    Slice ub;
    VkDescriptorSet s0 = VK_NULL_HANDLE;
    if (md.vb && nd && fp.depth[0][cl] && ring_.alloc(am + ad, &ub) && (s0 = alloc_set(depth_set0_layout_))) {
        std::memcpy(ub.ptr, md.clip_from_level, 64);
        DepthU du{{x.interp_alpha, md.pb ? 1.f : 0.f, 0.f, 0.f}, {}, {}};
        std::memcpy(du.wclip_lo, x.wclip_lo, sizeof du.wclip_lo);
        std::memcpy(du.wclip_hi, x.wclip_hi, sizeof du.wclip_hi);
        std::memcpy(ub.ptr + am, &du, sizeof du);
        const VkDescriptorBufferInfo bufs[3] = {
            {ub.buf, 0, 64}, {ub.buf, 0, sizeof(DepthU)},
            md.pb ? VkDescriptorBufferInfo{md.pb, md.poff, (VkDeviceSize)md.nv * 12} : VkDescriptorBufferInfo{md.vb, md.voff, (VkDeviceSize)md.nv * kVertexStride}};
        VkWriteDescriptorSet w[3];
        for (uint32_t k = 0; k < 3; k++) {
            w[k] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[k].dstSet = s0; w[k].dstBinding = k; w[k].descriptorCount = 1;
            w[k].descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[k].pBufferInfo = &bufs[k];
        }
        vkUpdateDescriptorSets(dev_, 3, w, 0, nullptr);
        const VkViewport vp{0.f, (float)md.size, (float)md.size, -(float)md.size, 0.f, 1.f};   // the Y flip (vk_device.h)
        const VkRect2D sc{{0, 0}, {md.size, md.size}};
        vkCmdSetViewport(md.cb, 0, 1, &vp);
        vkCmdSetScissor(md.cb, 0, 1, &sc);
        vkCmdSetDepthBias(md.cb, 0.f, 0.f, md.slope);
        vkCmdBindVertexBuffers(md.cb, 0, 1, &md.vb, &md.voff);
        const uint32_t dyn[2] = {(uint32_t)ub.off, (uint32_t)(ub.off + am)};
        vkCmdBindDescriptorSets(md.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fixed_layouts_.depth, 0, 1, &s0, 2, dyn);
        VkPipeline bound = VK_NULL_HANDLE;
        VkImageView last_tex = VK_NULL_HANDLE;
        VkSampler last_samp = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < nd; i++) {
            const DrawPacket &d = draws[i];
            if (d.vertex_count == 0 || d.first_vertex + d.vertex_count > md.nv) continue;
            const bool cut = (d.cc.opts & FC_CC_TEXTURE_EDGE) && d.tex[0] && fp.depth[1][cl];
            VkPipeline ps = fp.depth[cut ? 1 : 0][cl];
            if (ps != bound) { vkCmdBindPipeline(md.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps); bound = ps; }
            if (cut) {
                const VkImageView tv = bake_texture(d.tex[0], md.bake);
                const VkSampler sv = sampler(d.samp[0], true);
                if (tv != last_tex || sv != last_samp) {
                    VkDescriptorSet s1 = alloc_set(depth_set1_layout_);
                    if (!s1) continue;
                    const VkDescriptorImageInfo ii{sv, tv, kRead};
                    VkWriteDescriptorSet wt{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    wt.dstSet = s1; wt.dstBinding = 0; wt.descriptorCount = 1;
                    wt.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wt.pImageInfo = &ii;
                    vkUpdateDescriptorSets(dev_, 1, &wt, 0, nullptr);
                    vkCmdBindDescriptorSets(md.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fixed_layouts_.depth, 1, 1, &s1, 0, nullptr);
                    last_tex = tv; last_samp = sv;
                }
            }
            vkCmdDraw(md.cb, d.vertex_count, 1, d.first_vertex, 0);
        }
    }
    vkCmdEndRenderPass(md.cb);
}
// One of the frame's maps, in the open frame: SHADER_READ_ONLY (or new) -> attachment, the pass, -> SHADER_READ_ONLY.
bool VulkanDevice::render_map(Map &m, uint32_t size, const float clip_from_level[16], const fc_vertex *verts, uint32_t nv,
                              const DrawPacket *draws, uint32_t nd, float slope, bool clamp, const DepthExtras &x) {
    if (!frame_open_) return false;
    const vk::FixedPipes &fp = fixed_pipes();
    if (!fp.depth[0][clamp ? 1 : 0]) {
        if (!depth_warned_) vos_log("AR_VULKAN no depth-map pipeline: the depth maps stay placeholders");
        depth_warned_ = true;
        return false;
    }
    if (!ensure_map(m, size)) return false;
    flush_sampling(m.im.view);   // recorded passes that sample this frame's earlier map get it, not the new one
    MapDraw md{cmd_, m.fb, size, clip_from_level, slope, clamp, false, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, nv};
    Slice s;
    if (nv && verts && ring_.stage(verts, (size_t)nv * sizeof(fc_vertex), &s)) {
        md.vb = s.buf; md.voff = s.off;
        if (x.prev_pos && x.interp_alpha < 1.f && ring_.stage(x.prev_pos, (size_t)nv * 3 * sizeof(float), &s)) { md.pb = s.buf; md.poff = s.off; }
    }
    image_barrier(cmd_, m.im.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, m.layout, kAttach, true);   // cleared: the old contents go
    encode_depth_map(md, draws, nd, x);
    image_barrier(cmd_, m.im.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, kAttach, kRead);
    m.layout = kRead;
    return true;
}
// The key light's map clamps instead of clipping: a caster in front of the near plane (above the level's static bounds)
// lands at depth 0 and still shadows every receiver.
void VulkanDevice::encode_shadow_map(const ShadowFit &fit, const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd,
                                     const DepthExtras &x) {
    if (!render_map(shadow_, kShadowSize, fit.clip_from_level, verts, nv, draws, nd, 2.f, true, x)) return;
    shadow_fit_ = fit;
    shadow_valid_ = true;
}
void VulkanDevice::encode_underside_map(const UndersideDesc &u, const fc_vertex *verts, uint32_t nv, const DrawPacket *draws,
                                        uint32_t nd, const DepthExtras &x) {
    if (!render_map(under_, kUndersideSize, u.clip_from_level, verts, nv, draws, nd, 0.f, false, x)) return;
    under_desc_ = u;
    under_valid_ = true;
}
void VulkanDevice::encode_water_footprint(const float window[4], const float clip_from_level[16], const fc_vertex *verts, uint32_t nv,
                                          const DrawPacket *draws, uint32_t nd) {
    if (!(window[2] > window[0]) || !(window[3] > window[1])) return;
    if (!render_map(foot_, kFootprintSize, clip_from_level, verts, nv, draws, nd, 0.f, false, DepthExtras{})) return;
    std::memcpy(foot_window_, window, sizeof foot_window_);
    foot_valid_ = true;
    foot_kept_ = true;
}
bool VulkanDevice::reuse_water_footprint() {
    if (!frame_open_ || !foot_.im.img || !foot_kept_) return false;
    foot_valid_ = true;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The AO bake (metal_device.mm bake_ao): each view's depth map into slice j of a 2D array, then ao_fill into the
// volume. Its own command buffer, submitted now, before the open frame's on the same queue: its GPU time is the bake's
// alone, and the frame's passes, queued after it, sample the finished volume (the final barrier's second scope covers
// every later submission's fragment reads).
// ---------------------------------------------------------------------------------------------------------------------
bool VulkanDevice::bake_ao(uint32_t slot, const AoVolume &vol, const std::vector<ShadowFit> &views, float radius,
                           const fc_vertex *verts, uint32_t nv, const DrawPacket *draws, uint32_t nd, const DepthExtras &x) {
    const uint32_t n = (uint32_t)std::min<size_t>(views.size(), kAoMaxViews);
    auto fail = [](const char *why) { vos_log("AR_AO bake failed: %s", why); return false; };
    if (slot >= kAoSlots) return fail("slot");
    if (!frame_open_ || n == 0 || !nv || !verts) return fail("no frame or nothing to bake");
    if (!vol.dims[0] || !vol.dims[1] || !vol.dims[2]) return fail("empty volume");
    const vk::FixedPipes &fp = fixed_pipes();
    if (!fp.depth[0][0] || !fp.ao) return fail("no pipeline");
    // the resources, each freed at once on a failure before the submission, else retired with the open frame
    struct Bake {
        Image maps, out;
        std::vector<VkImageView> layer;
        std::vector<VkFramebuffer> fb;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
    } k;
    auto free_bake = [this](Bake &b, bool keep_out) {
        for (VkFramebuffer f : b.fb) if (f) vkDestroyFramebuffer(dev_, f, nullptr);
        for (VkImageView v : b.layer) if (v) vkDestroyImageView(dev_, v, nullptr);
        res_.destroy_image(b.maps);
        if (!keep_out) res_.destroy_image(b.out);
        if (b.cb) vkFreeCommandBuffers(dev_, cmd_pool_, 1, &b.cb);
        if (b.fence) vkDestroyFence(dev_, b.fence, nullptr);
        if (b.queries) vkDestroyQueryPool(dev_, b.queries, nullptr);
    };
    auto abandon = [&](const char *why) { free_bake(k, false); return fail(why); };
    vk::ImageDesc md;
    md.format = kMapFormat; md.extent = {kAoMapSize, kAoMapSize, 1}; md.layers = n;
    md.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkFormat vf = ao_rgba8_ ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8_UNORM;
    vk::ImageDesc vd;
    vd.type = VK_IMAGE_TYPE_3D; vd.format = vf; vd.extent = {vol.dims[0], vol.dims[1], vol.dims[2]};
    vd.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!res_.make_image(md, &k.maps) || !res_.make_image(vd, &k.out)) return abandon("no memory");
    k.maps.view = res_.make_view(k.maps.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, kMapFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 0, n, 1);
    k.out.view = res_.make_view(k.out.img, VK_IMAGE_VIEW_TYPE_3D, vf, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
    if (!k.maps.view || !k.out.view) return abandon("no view");
    k.layer.assign(n, VK_NULL_HANDLE);
    k.fb.assign(n, VK_NULL_HANDLE);
    for (uint32_t j = 0; j < n; j++) {
        if (!(k.layer[j] = res_.make_view(k.maps.img, VK_IMAGE_VIEW_TYPE_2D, kMapFormat, VK_IMAGE_ASPECT_DEPTH_BIT, j, 1, 1)))
            return abandon("no view");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = fixed_layouts_.map_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &k.layer[j];
        fi.width = fi.height = kAoMapSize; fi.layers = 1;
        if (vkCreateFramebuffer(dev_, &fi, nullptr, &k.fb[j]) != VK_SUCCESS) { k.fb[j] = VK_NULL_HANDLE; return abandon("no framebuffer"); }
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev_, &ai, &k.cb) != VK_SUCCESS) { k.cb = VK_NULL_HANDLE; return abandon("no command buffer"); }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev_, &fci, nullptr, &k.fence) != VK_SUCCESS) { k.fence = VK_NULL_HANDLE; return abandon("no fence"); }
    if (timestamps_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        if (vkCreateQueryPool(dev_, &qi, nullptr, &k.queries) != VK_SUCCESS) k.queries = VK_NULL_HANDLE;   // untimed
    }
    // the vertices and uniforms live in the open frame's ring, its descriptor sets in its pools: the frame completes after
    // the bake, so both outlive it
    Slice vs, us;
    if (!ring_.stage(verts, (size_t)nv * sizeof(fc_vertex), &vs) || !ring_.alloc(sizeof(AoU), &us)) return abandon("no ring");
    const VkDescriptorSet set = alloc_set(ao_set_layout_);
    if (!set) return abandon("no descriptor set");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(k.cb, &bi) != VK_SUCCESS) return abandon("vkBeginCommandBuffer");
    if (k.queries) {
        vkCmdResetQueryPool(k.cb, k.queries, 0, 2);
        vkCmdWriteTimestamp(k.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, k.queries, 0);
    }
    image_barrier(k.cb, k.maps.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, n, VK_IMAGE_LAYOUT_UNDEFINED, kAttach);
    AoU u{{vol.min[0], vol.min[1], vol.min[2], vol.voxel}, {vol.dims[0], vol.dims[1], vol.dims[2], n}, {radius, 0.f, 0.f, 0.f}, {}};
    for (uint32_t j = 0; j < n; j++) {
        const MapDraw d{k.cb, k.fb[j], kAoMapSize, views[j].clip_from_level, 0.f, false, true, vs.buf, vs.off, VK_NULL_HANDLE, 0, nv};
        encode_depth_map(d, draws, nd, x);
        uv_from_clip(views[j].clip_from_level, u.uv[j]);
    }
    std::memcpy(us.ptr, &u, sizeof u);
    image_barrier(k.cb, k.maps.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, n, kAttach, kRead);
    image_barrier(k.cb, k.out.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const VkDescriptorBufferInfo ub{us.buf, us.off, sizeof(AoU)};
    const VkDescriptorImageInfo mi{nearest_, k.maps.view, kRead}, oi{VK_NULL_HANDLE, k.out.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3];
    for (uint32_t b = 0; b < 3; b++) {
        w[b] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[b].dstSet = set; w[b].dstBinding = b; w[b].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ub;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &mi;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo = &oi;
    vkUpdateDescriptorSets(dev_, 3, w, 0, nullptr);
    vkCmdBindPipeline(k.cb, VK_PIPELINE_BIND_POINT_COMPUTE, fp.ao);
    vkCmdBindDescriptorSets(k.cb, VK_PIPELINE_BIND_POINT_COMPUTE, fixed_layouts_.ao, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(k.cb, (vol.dims[0] + 3) / 4, (vol.dims[1] + 3) / 4, (vol.dims[2] + 3) / 4);
    image_barrier(k.cb, k.out.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_GENERAL, kRead);
    if (k.queries) vkCmdWriteTimestamp(k.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, k.queries, 1);
    if (vkEndCommandBuffer(k.cb) != VK_SUCCESS) return abandon("vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &k.cb;
    const VkResult r = vkQueueSubmit(queue_, 1, &si, k.fence);
    if (r != VK_SUCCESS) { vos_log("AR_VULKAN AO bake vkQueueSubmit failed: %d", (int)r); return abandon("submit"); }
    const uint64_t gen = ++ao_gen_;
    {   // ahead of the open frame's job: the completion thread waits the bake's fence first
        std::lock_guard<std::mutex> g(m_);
        jobs_.push_back(Job{frame_serial_, k.fence, k.queries, nullptr, nullptr, gen});
    }
    cv_.notify_all();
    if (ao_[slot].img) retire_image(ao_[slot]);   // recorded or in-flight passes may still sample the old volume
    ao_[slot] = k.out;
    ao_vol_[slot] = vol;
    retire([this, k, free_bake]() mutable { free_bake(k, true); });
    return true;
}
bool VulkanDevice::ao_bake_time(uint64_t *bake, double *ms) const {
    std::lock_guard<std::mutex> g(m_);
    if (ao_done_gen_ == 0) return false;
    *bake = ao_done_gen_;
    *ms = ao_done_ms_;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Composite (metal_device.mm encode_composite): over on under, premultiplied, by texel, into a target or an external
// image (VulkanExternalImage, slice 0, left in COLOR_ATTACHMENT_OPTIMAL). One-layer targets only.
// ---------------------------------------------------------------------------------------------------------------------
VkPipeline VulkanDevice::composite_pipeline(VkFormat fmt) {
    const vk::FixedPipes &fp = fixed_pipes();
    if (fmt == kColorFormat && fp.composite) return fp.composite;
    auto it = composite_pipes_.find((uint32_t)fmt);
    if (it != composite_pipes_.end()) return it->second;
    VkRenderPass rp = resolve_render_pass(fmt, VK_FORMAT_UNDEFINED);
    VkPipeline ps = rp && fp.composite_fs ? vk::make_resolve_pipeline(dev_, cache_, composite_layout_, rp, resolve_vs_, fp.composite_fs, false)
                                          : VK_NULL_HANDLE;
    if (ps) built_since_save_++;
    return composite_pipes_[(uint32_t)fmt] = ps;
}
void VulkanDevice::encode_composite(uint32_t dst, void *external, uint32_t under, uint32_t over) {
    const uint32_t nt = (uint32_t)targets_.size();
    if (!frame_open_ || under == 0 || over == 0 || under > nt || over > nt || dst > nt || (!dst && !external)) return;
    if (dst && (dst == under || dst == over)) { vos_log("AR_VULKAN composite into one of its sources"); return; }
    Target &u = targets_[under - 1], &o = targets_[over - 1];
    Target *d = dst ? &targets_[dst - 1] : nullptr;
    if (!u.color_2d || !o.color_2d || (d && !d->color_2d)) return;
    if (u.layers > 1 || o.layers > 1 || (d && d->layers > 1)) { vos_log("AR_VULKAN composite of layered targets is not supported"); return; }
    const VulkanExternalImage *ext = d ? nullptr : static_cast<const VulkanExternalImage *>(external);
    const VkFormat fmt = d ? kColorFormat : ext->format;
    const uint32_t w = d ? d->w : ext->width, h = d ? d->h : ext->height;
    flush(u); flush(o);
    if (d) flush(*d);
    VkPipeline ps = composite_pipeline(fmt);
    if (!ps) { vos_log("AR_VULKAN no composite pipeline for format %d", (int)fmt); return; }
    VkRenderPass rp = resolve_render_pass(fmt, VK_FORMAT_UNDEFINED);
    VkImageView view = d ? d->color_2d : res_.make_view(ext->image, VK_IMAGE_VIEW_TYPE_2D, fmt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1);
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (view) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = w; fi.height = h; fi.layers = 1;
        if (vkCreateFramebuffer(dev_, &fi, nullptr, &fb) != VK_SUCCESS) fb = VK_NULL_HANDLE;
    }
    VkDevice dv = dev_;
    const VkImageView own = d ? VK_NULL_HANDLE : view;
    retire([dv, own, fb] {
        if (fb) vkDestroyFramebuffer(dv, fb, nullptr);
        if (own) vkDestroyImageView(dv, own, nullptr);
    });
    const VkDescriptorSet set = fb ? alloc_set(resolve_set_layout_) : VK_NULL_HANDLE;
    if (!set) { vos_log("AR_VULKAN composite: no framebuffer or descriptor set"); return; }
    const VkDescriptorImageInfo ii[2] = {{nearest_, u.color_2d, kRead}, {nearest_, o.color_2d, kRead}};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = set; wr.dstBinding = 0; wr.descriptorCount = 2;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = ii;
    vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    if (u.color_layout != kRead) transition_color(u, kRead);
    if (o.color_layout != kRead) transition_color(o, kRead);
    if (d) transition_color(*d, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);   // every texel is written
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(cmd_, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.f, 0.f, (float)w, (float)h, 0.f, 1.f};   // no flip: the triangle covers it and reads gl_FragCoord
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &sc);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ps);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd_);
    encoders_++;
}
}
}
