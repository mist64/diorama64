// The Vulkan device's buffers, images and barriers (internal to ar_render_vulkan). Memory's methods read only state set
// by init, so any thread may call them.
#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>
namespace ar {
namespace vk {
struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t *map = nullptr; VkDeviceSize size = 0; };
struct Image { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
inline VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// The stage and access of an image's use in a layout (the barriers derive both sides from the layouts). SHADER_READ_ONLY
// covers the fragment and compute stages; GENERAL is a compute storage image; DEPTH_STENCIL_ATTACHMENT includes the
// colour output stage (where a depth resolve writes).
void layout_scope(VkImageLayout l, VkPipelineStageFlags *stage, VkAccessFlags *access);
// discard: the contents may go (oldLayout UNDEFINED), but the source scope still covers the previous use of the image (a
// layout transition writes, so it must wait for the last writes).
void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t base_level, uint32_t levels,
                   uint32_t layers, VkImageLayout from, VkImageLayout to, bool discard = false);

struct ImageDesc {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{1, 1, 1};
    uint32_t levels = 1, layers = 1;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool lazy = false;   // LAZILY_ALLOCATED memory when a type offers it (transient attachments)
};
class Memory {
public:
    void init(VkDevice dev, VkPhysicalDevice phys);
    VkDevice device() const { return dev_; }
    bool lazy_available() const;
    uint32_t type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) const;   // UINT32_MAX: none
    // host: mapped, host-visible (cached when offered) and coherent
    bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer *out) const;
    void destroy_buffer(Buffer &b) const;
    // *lazy_got: the image lives in LAZILY_ALLOCATED memory
    bool make_image(const ImageDesc &d, Image *out, bool *lazy_got = nullptr) const;
    VkImageView make_view(VkImage img, VkImageViewType type, VkFormat fmt, VkImageAspectFlags aspect, uint32_t base_layer,
                          uint32_t layers, uint32_t levels, uint32_t base_level = 0) const;
    void destroy_image(Image &im) const;

private:
    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
};
}
}
