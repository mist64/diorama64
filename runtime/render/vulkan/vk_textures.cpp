#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include "vk_device.h"
#include "vos_log.h"
// The game's textures: uploads staged and recorded into the frame's command buffer (no wait), their mip chains (a blit
// chain, or with fine textures the gamma kernel within a per-frame budget), the samplers and bound targets
// (metal_device.mm upload_texture, begin_frame's mips, sampler, texture).
namespace ar {
namespace vkd {
using vk::image_barrier;
using vk::kColorFormat;
namespace {
const VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
VkSamplerAddressMode address(uint8_t w) {
    return w == FC_WRAP_CLAMP ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
         : w == FC_WRAP_MIRROR ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
uint32_t level_size(uint32_t v, uint32_t level) { return std::max(v >> level, 1u); }
}

void VulkanDevice::set_texture_filter(TextureFilter f) {
    if (f == filter_) return;
    filter_ = f;   // the samplers are keyed by the filter: none retired
    apply_fine();
}
void VulkanDevice::set_fine_textures(bool on) {
    fine_want_ = on;
    apply_fine();
}
// The image is created now and its pixels staged; the copy and the mip chain are recorded into the open frame (now) or
// the next one (begin_frame). A replaced image is retired: recorded or in-flight passes may still sample it.
void VulkanDevice::upload_texture(uint32_t tex_id, uint32_t w, uint32_t h, const uint8_t *rgba) {
    if (!ok_ || !tex_id || !w || !h || !rgba) return;
    Tex t;
    t.w = w; t.h = h;
    t.levels = (w > 1 || h > 1) ? (uint32_t)std::floor(std::log2((double)std::max(w, h))) + 1 : 1;
    t.storage = storage_rgba8_ && t.levels > 1 && (size_t)w * h <= kFineMaxTexels;
    vk::ImageDesc d;
    d.format = kColorFormat; d.extent = {w, h, 1}; d.levels = t.levels;
    d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              (t.storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    if (!res_.make_image(d, &t.im) ||
        !(t.im.view = res_.make_view(t.im.img, VK_IMAGE_VIEW_TYPE_2D, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, t.levels))) {
        res_.destroy_image(t.im);
        vos_log("AR_VULKAN texture %u (%ux%u): image creation failed", tex_id, w, h);
        return;
    }
    Buffer st;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    if (!res_.make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &st)) { res_.destroy_image(t.im); return; }
    std::memcpy(st.map, rgba, (size_t)bytes);
    auto old = textures_.find(tex_id);
    if (old != textures_.end()) {
        const VkImage oi = old->second.im.img;
        forget(pending_mips_, oi);
        forget(late_gamma_, oi);
        forget(late_blit_, oi);
        for (size_t i = 0; i < uploads_.size(); i++)
            if (uploads_[i].tex.im.img == oi) {   // never recorded: no frame uses it
                Buffer s = uploads_[i].staging;
                retire([this, s]() mutable { res_.destroy_buffer(s); });
                uploads_.erase(uploads_.begin() + (ptrdiff_t)i);
                break;
            }
        retire_image(old->second.im);
    }
    textures_[tex_id] = t;
    uploads_.push_back(Upload{t, st});
    if (frame_open_) record_textures(false);
}
void VulkanDevice::forget(std::vector<Tex> &list, VkImage img) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Tex &t) { return t.im.img == img; }), list.end());
}
// The staged uploads (level 0, then every level SHADER_READ_ONLY), their mip chains, and at a frame's start the re-mips a
// fine-textures switch left (metal_device.mm begin_frame).
void VulkanDevice::record_textures(bool frame_start) {
    if (!frame_open_) return;
    for (Upload &u : uploads_) {
        const Tex &t = u.tex;
        image_barrier(cmd_, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {t.w, t.h, 1};
        vkCmdCopyBufferToImage(cmd_, u.staging.buf, t.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        image_barrier(cmd_, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kRead);
        if (t.levels > 1) {
            image_barrier(cmd_, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, t.levels - 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, kRead);
            pending_mips_.push_back(t);
        }
        Buffer s = u.staging;
        retire([this, s]() mutable { res_.destroy_buffer(s); });
    }
    uploads_.clear();
    if (!pending_mips_.empty()) {
        VkPipeline mp = fine_ ? mip_pipeline_if_ready() : VK_NULL_HANDLE;
        std::vector<Tex> gamma, blit;
        for (const Tex &t : pending_mips_) {
            if (mp && gamma_eligible(t) && within_gamma_budget(gamma_n_, gamma_texels_)) {
                gamma.push_back(t); gamma_n_++; gamma_texels_ += (size_t)t.w * t.h;
            } else {
                blit.push_back(t);
            }
        }
        pending_mips_.clear();
        encode_gamma_mips(gamma);
        if (!blit.empty()) {
            encode_blit_mips(blit);
            if (fine_ && !mip_failed_)
                for (const Tex &t : blit) if (gamma_eligible(t)) late_gamma_.push_back(t);
        }
    }
    if (!frame_start) return;
    if (!late_blit_.empty()) {
        size_t n = 0;
        while (n < late_blit_.size() && within_gamma_budget(gamma_n_, gamma_texels_)) {
            gamma_texels_ += (size_t)late_blit_[n].w * late_blit_[n].h; gamma_n_++; n++;
        }
        encode_blit_mips(std::vector<Tex>(late_blit_.begin(), late_blit_.begin() + (ptrdiff_t)n));
        late_blit_.erase(late_blit_.begin(), late_blit_.begin() + (ptrdiff_t)n);
    }
    if (fine_ && !late_gamma_.empty() && within_gamma_budget(gamma_n_, gamma_texels_)) {
        if (mip_pipeline_if_ready()) {
            size_t n = 0;
            while (n < late_gamma_.size() && within_gamma_budget(gamma_n_, gamma_texels_)) {
                gamma_texels_ += (size_t)late_gamma_[n].w * late_gamma_[n].h; gamma_n_++; n++;
            }
            encode_gamma_mips(std::vector<Tex>(late_gamma_.begin(), late_gamma_.begin() + (ptrdiff_t)n));
            late_gamma_.erase(late_gamma_.begin(), late_gamma_.begin() + (ptrdiff_t)n);
        }
    }
}
// With async pipelines the render thread never waits for the gamma kernel and re-mips a bounded number of textures per
// frame; without (the harness, tests) it waits and re-mips at once.
bool VulkanDevice::within_gamma_budget(size_t count, size_t texels) const {
    static constexpr size_t kGammaTextures = 64, kGammaTexels = 4u << 20;
    return !async_ || count == 0 || (count < kGammaTextures && texels < kGammaTexels);
}
// Every level SHADER_READ_ONLY before and after; level i is the linear blit of level i - 1.
void VulkanDevice::encode_blit_mips(const std::vector<Tex> &list) {
    for (const Tex &t : list) {
        const VkImage img = t.im.img;
        image_barrier(cmd_, img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, kRead, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(cmd_, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, t.levels - 1, 1, kRead, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
        for (uint32_t i = 1; i < t.levels; i++) {
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
            b.srcOffsets[1] = {(int32_t)level_size(t.w, i - 1), (int32_t)level_size(t.h, i - 1), 1};
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            b.dstOffsets[1] = {(int32_t)level_size(t.w, i), (int32_t)level_size(t.h, i), 1};
            vkCmdBlitImage(cmd_, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            image_barrier(cmd_, img, VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        image_barrier(cmd_, img, VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kRead);
    }
    mips_blit_ += list.size();
}
// The gamma kernel (glsl_fixed.h mip_gamma.comp) writes level l from level l - 1, one dispatch per level, straight into the
// texture (a storage image here; Metal's scratch chain only avoids ShaderWrite textures). A texture whose views or sets
// cannot be made takes the blit chain.
void VulkanDevice::encode_gamma_mips(const std::vector<Tex> &list) {
    if (list.empty()) return;
    std::vector<Tex> fallback;
    VkDevice dv = dev_;
    for (const Tex &t : list) {
        std::vector<VkImageView> views(t.levels, VK_NULL_HANDLE);
        std::vector<VkDescriptorSet> sets(t.levels, VK_NULL_HANDLE);
        bool ok = true;
        for (uint32_t l = 0; l < t.levels && ok; l++)
            ok = (views[l] = res_.make_view(t.im.img, VK_IMAGE_VIEW_TYPE_2D, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, l)) != VK_NULL_HANDLE;
        for (uint32_t l = 1; l < t.levels && ok; l++) ok = (sets[l] = alloc_set(mip_set_layout_)) != VK_NULL_HANDLE;
        retire([dv, views] { for (VkImageView v : views) if (v) vkDestroyImageView(dv, v, nullptr); });
        if (!ok) { fallback.push_back(t); continue; }
        image_barrier(cmd_, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, t.levels - 1, 1, kRead, VK_IMAGE_LAYOUT_GENERAL, true);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, mip_pipe_);
        for (uint32_t l = 1; l < t.levels; l++) {
            const VkDescriptorImageInfo src{nearest_, views[l - 1], kRead}, dst{VK_NULL_HANDLE, views[l], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[2];
            for (int k = 0; k < 2; k++) {
                w[k] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[k].dstSet = sets[l]; w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
                w[k].descriptorType = k ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[k].pImageInfo = k ? &dst : &src;
            }
            vkUpdateDescriptorSets(dev_, 2, w, 0, nullptr);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fixed_layouts_.mip, 0, 1, &sets[l], 0, nullptr);
            vkCmdDispatch(cmd_, (level_size(t.w, l) + 7) / 8, (level_size(t.h, l) + 7) / 8, 1);
            image_barrier(cmd_, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 1, VK_IMAGE_LAYOUT_GENERAL, kRead);
        }
        mips_kernel_++;
    }
    encode_blit_mips(fallback);
}
void VulkanDevice::prepare_mips() {
    if (mip_job_.valid() || mip_taken_ || !ok_) return;
    VkDevice dev = dev_;
    VkPipelineCache cache = cache_;
    VkPipelineLayout layout = fixed_layouts_.mip;
    mip_job_ = std::async(std::launch::async, [=] { return vk::make_mip_pipeline(dev, cache, layout); }).share();
}
VkPipeline VulkanDevice::mip_pipeline_if_ready() {
    prepare_mips();
    if (!mip_taken_) {
        if (!mip_job_.valid()) return VK_NULL_HANDLE;
        if (async_ && mip_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return VK_NULL_HANDLE;
        mip_pipe_ = mip_job_.get();
        mip_taken_ = true;
        mip_job_ = std::shared_future<VkPipeline>();
    }
    if (!mip_pipe_ && !mip_failed_) { mip_failed_ = true; late_gamma_.clear(); }
    return mip_pipe_;
}
bool VulkanDevice::wait_mips_ready(double timeout_s) {
    prepare_mips();
    if (!mip_taken_ && (!mip_job_.valid() ||
                        mip_job_.wait_for(std::chrono::duration<double>(std::max(timeout_s, 0.0))) != std::future_status::ready))
        return false;
    return mip_pipeline_if_ready() != VK_NULL_HANDLE;
}
// metal_device.mm apply_fine: switching re-mips every eligible texture once (late_gamma_ / late_blit_, budgeted).
void VulkanDevice::apply_fine() {
    const bool on = fine_want_ && filter_ == TextureFilter::smooth;
    if (on) prepare_mips();
    if (on == fine_) return;
    fine_ = on;
    std::unordered_set<VkImage> skip;   // mipped (or uploaded) anyway: never twice
    for (const Tex &t : pending_mips_) skip.insert(t.im.img);
    for (const Upload &u : uploads_) skip.insert(u.tex.im.img);
    if (on) {
        for (const Tex &t : late_blit_) skip.insert(t.im.img);
        late_blit_.clear();
        if (!mip_failed_)
            for (auto &kv : textures_)
                if (gamma_eligible(kv.second) && !skip.count(kv.second.im.img)) late_gamma_.push_back(kv.second);
    } else {
        for (const Tex &t : late_gamma_) skip.insert(t.im.img);
        late_gamma_.clear();
        for (auto &kv : textures_)
            if (gamma_eligible(kv.second) && !skip.count(kv.second.im.img)) late_blit_.push_back(kv.second);
    }
}
// Between frames, once the texture's upload was recorded: the level as the GPU left it.
bool VulkanDevice::read_texture_level(uint32_t tex_id, uint32_t level, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    auto it = textures_.find(tex_id);
    if (frame_open_ || it == textures_.end() || level >= it->second.levels) return false;
    const Tex t = it->second;
    for (const Upload &u : uploads_) if (u.tex.im.img == t.im.img) return false;
    wait_idle();
    w = level_size(t.w, level); h = level_size(t.h, level);
    Buffer st;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    if (!res_.make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &st)) return false;
    VkCommandBuffer cb = begin_once();
    bool ok = cb != VK_NULL_HANDLE;
    if (ok) {
        image_barrier(cb, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 1, kRead, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, t.im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buf, 1, &c);
        image_barrier(cb, t.im.img, VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kRead);
        VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = st.buf;
        hb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
        ok = end_once();
    }
    if (ok) rgba.assign(st.map, st.map + bytes);
    res_.destroy_buffer(st);
    return ok;
}
// metal_device.mm sampler: keyed by (linear, wrap_s, wrap_t, aniso16) and the filter. smooth: trilinear with anisotropy
// 4 (16 with fine textures, never in a depth pass); vanilla: the texture's own filter, level 0 only.
VkSampler VulkanDevice::sampler(const fc_sampler &s, bool depth_pass) {
    const bool smooth = filter_ == TextureFilter::smooth, aniso16 = fine_ && !depth_pass;
    const uint32_t key = s.linear | (uint32_t)s.wrap_s << 8 | (uint32_t)s.wrap_t << 16 | (aniso16 ? 1u << 24 : 0u) | (smooth ? 1u << 25 : 0u);
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
        si.maxAnisotropy = std::min(aniso16 ? 16.f : 4.f, props_.limits.maxSamplerAnisotropy);
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
// A texture id bound to a target (bind_target_texture) samples the target's resolved colour: a one-layer target other
// than the one being drawn (white otherwise, as Metal's white_ for a missing one).
VkImageView VulkanDevice::texture(uint32_t tex_id, uint32_t drawing_target) const {
    if (!tex_id) return white_.view;
    if (!target_tex_.empty()) {
        auto bt = target_tex_.find(tex_id);
        if (bt != target_tex_.end()) {
            const uint32_t t = bt->second;
            return t <= targets_.size() && t != drawing_target && targets_[t - 1].layers == 1 && targets_[t - 1].color_2d
                   ? targets_[t - 1].color_2d : white_.view;
        }
    }
    auto it = textures_.find(tex_id);
    return it == textures_.end() ? white_.view : it->second.im.view;
}
}
}
