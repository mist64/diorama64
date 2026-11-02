// encode_resolve under lavapipe: the cube rendered into a 2-layer target is resolved (slice 1, linearize) into a 2-layer
// R8G8B8A8_SRGB external image wrapped as the Quest platform wraps its swapchain images, then layer 1 is read back.
#include "check.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "ar_layer.h"
#include "cube_adapter.h"
#include "fake_platform.h"
#include "renderer.h"
#include "vk_host_context.h"
#include "vulkan_device.h"

using namespace ar;

namespace {
constexpr uint32_t kSize = 256;

uint32_t memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// Records rec into a one-shot command buffer, submits it and waits for the queue.
template <class F> bool run_once(const VulkanDeviceInit &vk, F rec) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = vk.queue_family;
    VkCommandPool pool;
    if (vkCreateCommandPool(vk.device, &pci, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    bool ok = vkAllocateCommandBuffers(vk.device, &ai, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        rec(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS && vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
             vkQueueWaitIdle(vk.queue) == VK_SUCCESS;
    }
    vkDestroyCommandPool(vk.device, pool, nullptr);
    return ok;
}

void layer_barrier(VkCommandBuffer cb, VkImage img, uint32_t layer, uint32_t count, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, count};
    vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

struct External {
    VulkanExternalImage ext{};
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

// What qx_platform.cpp gets from the OpenXR swapchain: a 2-layer colour image (in COLOR_ATTACHMENT_OPTIMAL, as the
// runtime hands it over) and its 2D_ARRAY view.
bool make_external(const VulkanDeviceInit &vk, External *e) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_SRGB;
    ii.extent = {kSize, kSize, 1};
    ii.mipLevels = 1; ii.arrayLayers = 2;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    if (vkCreateImage(vk.device, &ii, nullptr, &img) != VK_SUCCESS) return false;
    e->ext = VulkanExternalImage{img, VK_NULL_HANDLE, ii.format, kSize, kSize, 2};
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, img, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = memory_type(vk.physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX) ma.memoryTypeIndex = memory_type(vk.physical, mr.memoryTypeBits, 0);
    if (vkAllocateMemory(vk.device, &ma, nullptr, &e->mem) != VK_SUCCESS || vkBindImageMemory(vk.device, img, e->mem, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = ii.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    if (vkCreateImageView(vk.device, &vci, nullptr, &e->ext.view) != VK_SUCCESS) return false;
    return run_once(vk, [&](VkCommandBuffer cb) {
        layer_barrier(cb, img, 0, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    });
}

void destroy_external(const VulkanDeviceInit &vk, External &e) {
    if (e.ext.view) vkDestroyImageView(vk.device, e.ext.view, nullptr);
    if (e.ext.image) vkDestroyImage(vk.device, e.ext.image, nullptr);
    if (e.mem) vkFreeMemory(vk.device, e.mem, nullptr);
}

// Copies one layer to a host-visible buffer. The barrier leaves COLOR_ATTACHMENT_OPTIMAL: the layout encode_resolve
// promises to leave the image in (the validation layer reports a mismatch at submit).
bool read_layer(const VulkanDeviceInit &vk, const External &e, uint32_t layer, std::vector<uint8_t> *out) {
    const VkDeviceSize bytes = (VkDeviceSize)kSize * kSize * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (vkCreateBuffer(vk.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, buf, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = memory_type(vk.physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    bool ok = ma.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(vk.device, &ma, nullptr, &mem) == VK_SUCCESS &&
              vkBindBufferMemory(vk.device, buf, mem, 0) == VK_SUCCESS;
    ok = ok && run_once(vk, [&](VkCommandBuffer cb) {
        layer_barrier(cb, e.ext.image, layer, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        c.imageExtent = {kSize, kSize, 1};
        vkCmdCopyImageToBuffer(cb, e.ext.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.buffer = buf; hb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
        layer_barrier(cb, e.ext.image, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    });
    void *map = nullptr;
    ok = ok && vkMapMemory(vk.device, mem, 0, bytes, 0, &map) == VK_SUCCESS;
    if (ok) {
        out->assign((const uint8_t *)map, (const uint8_t *)map + bytes);
        vkUnmapMemory(vk.device, mem);
    }
    vkDestroyBuffer(vk.device, buf, nullptr);
    if (mem) vkFreeMemory(vk.device, mem, nullptr);
    return ok;
}

bool near(int a, int b) { return std::abs(a - b) <= 8; }
}  // namespace

int main() {
    VulkanDeviceInit init{}; std::string err;
    if (!vk_host_create(&init, &err)) SKIP(err.c_str());
    auto dev = create_vulkan_device(init);
    CHECK(dev);
    External ext;
    CHECK(make_external(init, &ext));
    Renderer renderer(*dev);
    fake_platform fp; fake_platform_init(&fp, 90.0); fp.width = kSize; fp.height = kSize; fp.view_count = 2; fp.stop_after = 4;
    ar_runtime *rt = ar_runtime_create(cube_adapter(), &fp.api);
    CHECK(ar_runtime_init_game(rt, "", ""));
    ar_runtime_set_texture_sink(rt, [](void *u, uint32_t id, uint32_t w, uint32_t h, const uint8_t *px) { static_cast<IRenderDevice *>(u)->upload_texture(id, w, h, px); }, dev.get());
    const uint32_t target = dev->create_target(kSize, kSize, 2, 1);
    ar_render_frame rf{};
    int resolved = 0;
    for (int i = 0; i < 4; i++) {
        CHECK(ar_runtime_begin_frame(rt, &rf));
        while (ar_runtime_ticks_due(rt)) ar_runtime_step_game(rt);
        if (rf.snapshot) {
            LightingFrame lf; ArLayerOptions lo; lo.layered = true; lo.lighting = &lf; lo.overlays = false;
            dev->begin_frame();
            draw_ar_layer(renderer, target, rf, lo, 1.f / 90.f);
            ExternalView ev{};
            ev.color = &ext.ext;
            ev.slice = 1;
            ev.viewport = Viewport{0.f, 0.f, (float)kSize, (float)kSize};
            ev.src_x = 0; ev.src_y = 0; ev.src_layer = 1;
            ev.linearize = true;
            dev->encode_resolve(target, ev);
            CHECK(vulkan_device_frame_ok(*dev));
            resolved++;
            ar_runtime_submit_frame(rt, &rf);
            dev->end_frame([](void *) {}, nullptr, nullptr);
        } else { ar_runtime_submit_frame(rt, &rf); }
        ar_runtime_end_frame(rt, &rf);
    }
    CHECK(resolved > 0);
    dev->wait_idle();
    std::vector<uint8_t> src, out; uint32_t w = 0, h = 0;
    CHECK(dev->read_target(target, src, w, h) && w == kSize && h == kSize);
    CHECK(read_layer(init, ext, 1, &out) && out.size() == (size_t)kSize * kSize * 4);
    auto at = [](const std::vector<uint8_t> &v, uint32_t x, uint32_t y) { return &v[(y * kSize + x) * 4]; };
    // the cube spans x 104..161, y 169..235 (test_vulkan_cube): its centre is opaque and keeps the target's
    // gamma-space colour (linearized by the resolve, encoded back to sRGB on store)
    const uint8_t *c = at(out, 128, 200), *s = at(src, 128, 200);
    printf("resolved centre %u %u %u %u, target centre %u %u %u %u\n", c[0], c[1], c[2], c[3], s[0], s[1], s[2], s[3]);
    CHECK(c[3] == 255 && c[0] + c[1] + c[2] > 0);
    CHECK(near(c[0], s[0]) && near(c[1], s[1]) && near(c[2], s[2]));
    CHECK(at(out, 2, 2)[3] == 0 && at(out, 253, 2)[3] == 0 && at(out, 2, 253)[3] == 0 && at(out, 253, 253)[3] == 0);
    // no Y flip: above the cube's top (toward the image's top) is clear, the cube's rows are opaque
    CHECK(at(out, 128, 150)[3] == 0 && at(out, 128, 60)[3] == 0);
    CHECK(at(out, 128, 175)[3] == 255 && at(out, 128, 228)[3] == 255);
    ar_runtime_destroy(rt); dev.reset();
    destroy_external(init, ext);
    vk_host_destroy(init);
    return 0;
}
