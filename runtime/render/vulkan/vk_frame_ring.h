// The frames' staging rings (internal to ar_render_vulkan; metal_device.mm's Ring and StageMemo): one host-visible buffer per
// frame in flight for vertices, normals, previous positions and uniforms. Render thread only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include "vk_resources.h"
namespace ar {
namespace vk {
struct Slice { VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0; uint8_t *ptr = nullptr; };
class FrameRing {
public:
    static constexpr uint32_t kSlots = 3;                   // the frames in flight
    static constexpr VkDeviceSize kMinBytes = 1u << 20;
    static constexpr size_t kMemoMin = 64 * 1024;           // arrays this large are deduplicated
    static constexpr int kMemoSlots = 6;
    // align: at least 256 (and the device's uniform / storage offset alignments). retire: destroys a buffer once the
    // frames that may read it completed (a ring outgrown mid-frame).
    void init(const Memory *mem, VkDeviceSize align, std::function<void(Buffer)> retire);
    // The slot's previous frame completed: its ring is reused, or dropped when more than four times what that frame
    // used (at least kMinBytes).
    void begin(uint32_t slot);
    // n bytes at the alignment; grows the slot's ring by doubling (the full one is retired)
    bool alloc(VkDeviceSize n, Slice *out);
    // a copy of src; an array of kMemoMin bytes or more staged again this frame with the same pointer, size and bytes
    // reuses the first copy (the renderer re-stages the frame's vertices for every pass)
    bool stage(const void *src, size_t n, Slice *out);
    void destroy();   // the device is idle

private:
    struct Ring { Buffer buf; VkDeviceSize used = 0, total = 0; };
    struct Memo { const void *src = nullptr; size_t n = 0; Slice at; };
    const Memory *mem_ = nullptr;
    VkDeviceSize align_ = 256;
    std::function<void(Buffer)> retire_;
    Ring rings_[kSlots];
    uint32_t slot_ = 0;
    Memo memo_[kMemoSlots];
    int memo_next_ = 0;
};
}
}
