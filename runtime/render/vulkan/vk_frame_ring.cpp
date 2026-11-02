#include "vk_frame_ring.h"
#include <algorithm>
#include <cstring>
namespace ar {
namespace vk {
void FrameRing::init(const Memory *mem, VkDeviceSize align, std::function<void(Buffer)> retire) {
    mem_ = mem;
    align_ = std::max<VkDeviceSize>(align, 256);
    retire_ = std::move(retire);
}
void FrameRing::begin(uint32_t slot) {
    slot_ = slot % kSlots;
    Ring &r = rings_[slot_];
    // the slot's last frame completed (the device waited its fence), so nothing else reads this buffer
    if (r.buf.buf && r.buf.size > 4 * std::max<VkDeviceSize>(r.total, kMinBytes)) mem_->destroy_buffer(r.buf);
    r.used = 0;
    r.total = 0;
    for (Memo &m : memo_) m = Memo{};
    memo_next_ = 0;
}
bool FrameRing::alloc(VkDeviceSize n, Slice *out) {
    Ring &r = rings_[slot_];
    VkDeviceSize at = align_up(r.used, align_);
    if (!r.buf.buf || at + n > r.buf.size) {
        const VkDeviceSize len = std::max<VkDeviceSize>({r.buf.size * 2, at + n, kMinBytes});
        Buffer nb;
        if (!mem_->make_buffer(len, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               true, &nb))
            return false;
        if (r.buf.buf) retire_(r.buf);   // this frame's earlier slices stay valid until it completed
        r.buf = nb;
        at = 0;
    }
    *out = Slice{r.buf.buf, at, r.buf.map + at};
    r.used = at + n;
    r.total += align_up(n, align_);
    return true;
}
bool FrameRing::stage(const void *src, size_t n, Slice *out) {
    const bool big = n >= kMemoMin;
    if (big)
        for (const Memo &m : memo_)
            if (m.src == src && m.n == n && std::memcmp(m.at.ptr, src, n) == 0) { *out = m.at; return true; }
    if (!alloc(n, out)) return false;
    std::memcpy(out->ptr, src, n);
    if (big) {
        memo_[memo_next_] = Memo{src, n, *out};
        memo_next_ = (memo_next_ + 1) % kMemoSlots;
    }
    return true;
}
void FrameRing::destroy() {
    if (!mem_) return;   // never initialised (the device's init failed first)
    for (Ring &r : rings_) { mem_->destroy_buffer(r.buf); r = Ring{}; }
    for (Memo &m : memo_) m = Memo{};
}
}
}
