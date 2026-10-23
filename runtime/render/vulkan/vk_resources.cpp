#include "vk_resources.h"
#include <initializer_list>
#include "vos_log.h"
namespace ar {
namespace vk {
void layout_scope(VkImageLayout l, VkPipelineStageFlags *stage, VkAccessFlags *access) {
    switch (l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        return;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        // a depth/stencil resolve writes its attachment in the colour output stage, as a colour attachment write
        *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_GENERAL:
        *stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
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
void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t base_level, uint32_t levels,
                   uint32_t layers, VkImageLayout from, VkImageLayout to, bool discard) {
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

void Memory::init(VkDevice dev, VkPhysicalDevice phys) {
    dev_ = dev;
    vkGetPhysicalDeviceMemoryProperties(phys, &props_);
}
bool Memory::lazy_available() const {
    for (uint32_t i = 0; i < props_.memoryTypeCount; i++)
        if (props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) return true;
    return false;
}
uint32_t Memory::type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) const {
    for (VkMemoryPropertyFlags f : {want, fallback})
        for (uint32_t i = 0; i < props_.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (props_.memoryTypes[i].propertyFlags & f) == f) return i;
    return UINT32_MAX;
}
bool Memory::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer *out) const {
    *out = Buffer{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bi, nullptr, &out->buf) != VK_SUCCESS) { out->buf = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, out->buf, &req);
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t t = host ? type(req.memoryTypeBits, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, hv)
                            : type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = t;
    if (t == UINT32_MAX || vkAllocateMemory(dev_, &ai, nullptr, &out->mem) != VK_SUCCESS ||
        vkBindBufferMemory(dev_, out->buf, out->mem, 0) != VK_SUCCESS ||
        (host && vkMapMemory(dev_, out->mem, 0, VK_WHOLE_SIZE, 0, (void **)&out->map) != VK_SUCCESS)) {
        vos_log("AR_VULKAN buffer of %llu bytes: allocation failed", (unsigned long long)size);
        destroy_buffer(*out);
        return false;
    }
    out->size = size;
    return true;
}
void Memory::destroy_buffer(Buffer &b) const {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);   // unmaps
    b = Buffer{};
}
bool Memory::make_image(const ImageDesc &d, Image *out, bool *lazy_got) const {
    *out = Image{};
    if (lazy_got) *lazy_got = false;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = d.flags;
    ii.imageType = d.type;
    ii.format = d.format;
    ii.extent = d.extent;
    ii.mipLevels = d.levels;
    ii.arrayLayers = d.layers;
    ii.samples = d.samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = d.usage | (d.lazy ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : 0);
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ii, nullptr, &out->img) != VK_SUCCESS) { out->img = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, out->img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = UINT32_MAX;
    if (d.lazy) {
        ai.memoryTypeIndex = type(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
        if (ai.memoryTypeIndex != UINT32_MAX && lazy_got) *lazy_got = true;
    }
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev_, &ai, nullptr, &out->mem) != VK_SUCCESS ||
        vkBindImageMemory(dev_, out->img, out->mem, 0) != VK_SUCCESS) {
        vos_log("AR_VULKAN image %ux%u format %d: allocation failed", d.extent.width, d.extent.height, (int)d.format);
        destroy_image(*out);
        if (lazy_got) *lazy_got = false;
        return false;
    }
    return true;
}
VkImageView Memory::make_view(VkImage img, VkImageViewType type, VkFormat fmt, VkImageAspectFlags aspect, uint32_t base_layer,
                              uint32_t layers, uint32_t levels, uint32_t base_level) const {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = fmt;
    vi.subresourceRange = {aspect, base_level, levels, base_layer, layers};
    VkImageView v = VK_NULL_HANDLE;
    return vkCreateImageView(dev_, &vi, nullptr, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}
void Memory::destroy_image(Image &im) const {
    if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
    if (im.img) vkDestroyImage(dev_, im.img, nullptr);
    if (im.mem) vkFreeMemory(dev_, im.mem, nullptr);
    im = Image{};
}
}
}
