// runtime/platform/quest/qx_session.cpp — one frame: runtime begin (xrWaitFrame, xrBeginFrame, acquire) → AR layer into
// our layered target → resolve each eye into the swapchain slice → submit (views to the platform) → GPU submit →
// end submission (release the image, xrEndFrame) → the completion thread ends the runtime frame.
#include "qx_session.h"
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include "ar_layer.h"
#include "lighting.h"
#include "vos_log.h"
#include "vulkan_device.h"

namespace ar {
namespace {
struct Done { ar_runtime *rt; ar_render_frame f; };
void on_done(void *ctx) {   // device completion thread: release the snapshot the GPU was reading
    auto *d = static_cast<Done *>(ctx);
    ar_runtime_end_frame(d->rt, &d->f);
    delete d;
}
void upload(void *user, uint32_t id, uint32_t w, uint32_t h, const uint8_t *rgba) {
    static_cast<IRenderDevice *>(user)->upload_texture(id, w, h, rgba);
}
}  // namespace

QxSession::QxSession(const ar_adapter *a, QxPlatform &p, const std::string &files_dir)
    : adapter_(a), platform_(p), files_dir_(files_dir) {}

QxSession::~QxSession() { stop(); }

bool QxSession::start(const std::string &rom_path, const std::string &save_dir, std::string *err) {
    if (rt_) {   // already running: leave it alone
        *err = "session already started";
        return false;
    }
    auto fail = [&](const std::string &why) {
        *err = why;
        stop();
        return false;
    };
    const QxVulkan &vk = platform_.vulkan();
    if (vk.device == VK_NULL_HANDLE) return fail("no Vulkan device (the platform did not start)");
    VulkanDeviceInit init{vk.instance, vk.physical, vk.device, vk.queue_family, vk.queue, vk.get_instance_proc_addr,
                          files_dir_ + "/pipelines.vkcache"};
    dev_ = create_vulkan_device(init);
    if (!dev_) return fail("Vulkan render device creation failed");
    renderer_.reset(new Renderer(*dev_));
    target_ = dev_->create_target(1, 1);
    w_ = h_ = layers_ = samples_ = 0;   // the first renderable frame sizes the target
    rt_ = ar_runtime_create(adapter_, platform_.api());
    if (!rt_) return fail("ar_runtime_create failed");
    ar_runtime_set_texture_sink(rt_, upload, dev_.get());
    if (!save_dir.empty() && ::mkdir(save_dir.c_str(), 0700) != 0 && errno != EEXIST)
        vos_log("AR_QUEST save folder %s: mkdir failed (errno %d)", save_dir.c_str(), errno);
    if (!ar_runtime_init_game(rt_, rom_path.c_str(), save_dir.c_str())) {
        const char *why = adapter_->last_error ? adapter_->last_error() : nullptr;
        return fail(std::string("init_game failed: ") + (why && *why ? why : "(no reason given)"));
    }
    if (!ar_runtime_start_game_thread(rt_)) return fail("ar_runtime_start_game_thread failed");
    vos_log("AR_QUEST_SESSION adapter=%s", adapter_->name);
    return true;
}

bool QxSession::frame() {
    if (!rt_) return false;
    ar_render_frame rf{};
    if (!ar_runtime_begin_frame(rt_, &rf)) return false;
    if (!rf.snapshot || rf.view_count == 0 || !platform_.frame_renderable()) {
        // Nothing to draw (no snapshot yet, or the compositor does not want this frame): submit no views, so the XR
        // frame carries passthrough only (an acquired image is still released), and release the runtime frame here,
        // since no GPU completion will (as vos_submit_if_empty).
        ar_render_frame none = rf;
        none.view_count = 0;
        ar_runtime_submit_frame(rt_, &none);
        platform_.end_submission();
        ar_runtime_end_frame(rt_, &rf);
        return true;
    }
    uint32_t tw = 0;
    const uint32_t th = rf.views[0].height, tl = rf.view_count;
    for (uint32_t i = 0; i < rf.view_count; i++) tw = std::max(tw, rf.views[i].width);
    const uint32_t samples = dev_->supported_samples(4, tl);
    if (tw != w_ || th != h_ || tl != layers_ || samples != samples_) {
        dev_->wait_idle();
        dev_->resize_target(target_, tw, th, tl, samples);
        w_ = tw; h_ = th; layers_ = tl; samples_ = samples;
        vos_log("AR_RENDER layered=1 target=%ux%ux%u samples=%u", tw, th, tl, samples);
    }
    LightingFrame lf;   // default: lighting off
    ArLayerOptions lo;
    lo.layered = true;
    lo.lighting = &lf;
    const float dt = have_time_ ? std::max(0.f, float(rf.display_time_ns - last_time_) * 1e-9f) : 1.f / 90.f;
    dev_->begin_frame();
    draw_ar_layer(*renderer_, target_, rf, lo, dt);
    bool resolved = true;
    for (uint32_t i = 0; i < rf.view_count; i++) {
        ExternalView ev{};
        if (!platform_.external_view(i, &ev)) { resolved = false; continue; }
        ev.src_layer = i;
        ev.linearize = platform_.color_wants_linear();
        dev_->encode_resolve(target_, ev);
    }
    // A failed begin_frame, a target left without images by the (re)size (its resolve then records nothing) or any
    // resolve that recorded nothing: submit no views (as the no-draw path), so no unwritten image is composited.
    if (resolved && vulkan_device_frame_ok(*dev_)) {
        ar_runtime_submit_frame(rt_, &rf);
    } else {
        ar_render_frame none = rf;
        none.view_count = 0;
        ar_runtime_submit_frame(rt_, &none);
    }
    dev_->end_frame(on_done, new Done{rt_, rf}, nullptr);   // vkQueueSubmit before the image is released
    platform_.end_submission();
    last_time_ = rf.display_time_ns;
    have_time_ = true;
    if (!logged_first_) {
        vos_log("AR_FIRST_FRAME views=%u size=%ux%u", rf.view_count, tw, th);
        logged_first_ = true;
    }
    return true;
}

void QxSession::stop() {
    if (rt_) ar_runtime_stop_game_thread(rt_);
    if (dev_) dev_->wait_idle();   // every completion has run (ended its runtime frame) before the runtime goes
    if (rt_) ar_runtime_destroy(rt_);
    rt_ = nullptr;
    renderer_.reset();
    dev_.reset();
    target_ = w_ = h_ = layers_ = samples_ = 0;
}

uint64_t QxSession::ticks_done() const { return rt_ ? ar_runtime_ticks_done(rt_) : 0; }

}  // namespace ar
