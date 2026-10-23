// runtime/platform/quest/qx_session.cpp — one frame, as vos_session.cpp's: runtime begin (xrWaitFrame, xrBeginFrame,
// acquire) → lighting → game ticks → AR layer into our layered target → resolve each eye into the swapchain slice →
// submit (views to the platform) → GPU submit → end submission (release the image, xrEndFrame) → the completion thread
// ends the runtime frame.
#include "qx_session.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include "ar_layer.h"
#include "ar_mem_stats.h"
#include "lighting.h"
#include "pipeline_warm.h"
#include "qx_lighting.h"
#include "room_light.h"
#include "vos_color.h"
#include "vos_log.h"
#include "vulkan_device.h"

namespace ar {
namespace {
// The array swapchain's slices: one per eye (QxPlatform's kEyes), the layered target's layer count.
constexpr uint32_t kLayers = 2;
struct Done { ar_runtime *rt; ar_render_frame f; };
void on_done(void *ctx) {   // device completion thread: release the snapshot the GPU was reading
    auto *d = static_cast<Done *>(ctx);
    ar_runtime_end_frame(d->rt, &d->f);
    delete d;
}
void upload(void *user, uint32_t id, uint32_t w, uint32_t h, const uint8_t *rgba) {
    static_cast<IRenderDevice *>(user)->upload_texture(id, w, h, rgba);
}
// vos_session.cpp's (file-local there).
bool read_text(const std::string &path, std::string *out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char b[4096];
    size_t n;
    out->clear();
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out->append(b, n);
    std::fclose(f);
    return true;
}
bool write_text(const std::string &path, const std::string &text) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) { ::unlink(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

QxSession::QxSession(const ar_adapter *a, QxPlatform &p, const std::string &files_dir)
    : adapter_(a), platform_(p), files_dir_(files_dir) {
    light_.set_log_callback([](void *, const char *line) { vos_log("%s", line); }, nullptr);
}

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
                          files_dir_ + "/pipelines.vkcache", vk.device_extensions, vk.alpha_to_one};
    dev_ = create_vulkan_device(init);
    if (!dev_) return fail("Vulkan render device creation failed");
    // The render thread never compiles a pipeline: a draw whose pipeline is not built yet is skipped while the
    // background warm-up compiles it (as visionOS's layer thread).
    dev_->set_async_pipelines(true);
    dev_->prepare_passes();   // the fixed passes' pipelines compile off the render thread from here on
    renderer_.reset(new Renderer(*dev_));
    target_ = dev_->create_target(1, 1);
    w_ = h_ = layers_ = samples_ = 0;   // the first renderable frame sizes the target
    look_ = effective_look();
    look_gen_ = 0;
    apply_tex_filter();                 // before the game uploads any texture
    const uint32_t samples = dev_->supported_samples(look_.msaa, kLayers);
    warm_pipelines(samples);
    warm_in_background(samples);
    rt_ = ar_runtime_create(adapter_, platform_.api());
    if (!rt_) return fail("ar_runtime_create failed");
    ar_runtime_set_texture_sink(rt_, upload, dev_.get());
    if (!save_dir.empty() && ::mkdir(save_dir.c_str(), 0700) != 0 && errno != EEXIST)
        vos_log("AR_QUEST save folder %s: mkdir failed (errno %d)", save_dir.c_str(), errno);
    if (!ar_runtime_init_game(rt_, rom_path.c_str(), save_dir.c_str())) {
        const char *why = adapter_->last_error ? adapter_->last_error() : nullptr;
        return fail(std::string("init_game failed: ") + (why && *why ? why : "(no reason given)"));
    }
    // No game thread: frame() steps the game (step_ticks), as VosSession.
    vos_log("AR_QUEST_SESSION adapter=%s", adapter_->name);
    return true;
}

// A draw pipeline compiles at its combiner's first draw; files_dir/pipelines.txt lists every pipeline this app has
// built, so the next launch compiles them here, before the first frame (start runs before the frame loop).
void QxSession::warm_pipelines(uint32_t samples) {
    std::string list;
    if (!read_text(files_dir_ + "/pipelines.txt", &list)) { vos_log("AR_WARMUP none"); return; }
    pipeline_file_ = list;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t n = dev_->warm_pipelines(list, samples);
    vos_log("AR_WARMUP list_bytes=%zu samples=%u pipelines=%zu ms=%.0f", list.size(), samples, n, ms_since(t0));
}
void QxSession::warm_in_background(uint32_t samples) {
    const size_t n = dev_->warm_pipelines_background(merge_pipeline_lists(pipeline_file_, dev_->pipeline_list()),
                                                     Renderer::fixed_pipeline_draws(), samples);
    vos_log("AR_WARMUP background=%zu samples=%u", n, samples);
}

// After every frame (a no-op unless a pipeline was built since) and at stop, as VosSession::save_pipelines.
void QxSession::save_pipelines() {
    if (!dev_) return;
    const size_t n = dev_->pipeline_count();
    if (n == pipelines_saved_) return;
    pipelines_saved_ = n;
    const size_t at_draw = dev_->draw_time_builds();
    if (at_draw != at_draw_queued_) {
        at_draw_queued_ = at_draw;
        dev_->warm_pipelines_background(dev_->pipeline_list(), {}, samples_ ? samples_ : 1);
    }
    pipeline_file_ = merge_pipeline_lists(pipeline_file_, dev_->pipeline_list());
    vos_log("AR_PIPELINES built=%zu at_draw=%zu tick=%llu saved=%d", n, at_draw, (unsigned long long)ticks_done(),
            (int)write_text(files_dir_ + "/pipelines.txt", pipeline_file_));
}

void QxSession::log_perf_window() {
    vos_log("%s", perf_line(perf_.report()).c_str());
    vos_log("AR_MEM phys_mb=%.1f", ar_phys_footprint_mb());
    vos_log("AR_WARMUP deferred_draws=%llu warming=%zu built=%zu promoted=%llu", (unsigned long long)dev_->deferred_draws(),
            dev_->pipelines_warming(), dev_->pipeline_count(), (unsigned long long)dev_->warm_promotions());
    perf_.reset();
}

uint64_t QxSession::ticks_done() const { return rt_ ? ar_runtime_ticks_done(rt_) : 0; }

std::string QxSession::status() const {
    char b[256];
    std::snprintf(b, sizeof b, "%s: tick %llu, table %s", adapter_->name, (unsigned long long)ticks_done(),
                  table_valid_ ? "placed" : "not placed yet");
    return b;
}

// The right aim ray (tracking space) into table space with the head, as PointerRouter::route builds visionOS's
// PointerEvent; the last valid ray when the aim is lost for a frame. Returns the claim's answer.
bool QxSession::pointer_event(PointerEvent::Phase phase, const ar_render_frame &rf) {
    const QxActionState &a = platform_.actions();
    if (a.right_aim_valid) {
        const float fwd[3] = {0.f, 0.f, -1.f};   // OpenXR's aim pose points down its -Z
        for (int k = 0; k < 3; k++) ray_o_[k] = a.right_aim.pos[k];
        ar_quat_rotate(a.right_aim.quat, fwd, ray_d_);
    }
    const ar_pose table_from_tracking = ar_pose_inverse(rf.tracking_from_table);
    PointerEvent p;
    p.id = pointer_id_;
    p.phase = phase;
    const ar_pose ray_pose{{ray_o_[0], ray_o_[1], ray_o_[2]}, {0.f, 0.f, 0.f, 1.f}};
    const ar_pose ro = ar_pose_mul(table_from_tracking, ray_pose);
    for (int k = 0; k < 3; k++) p.ray_o[k] = ro.pos[k];
    ar_quat_rotate(table_from_tracking.quat, ray_d_, p.ray_d);
    p.has_ray = true;
    p.has_hand = false;   // the controller is a ray, not a fingertip
    p.head = rf.head_table;
    return claim_(p);
}

void QxSession::cancel_game_pointer(const ar_render_frame &rf) {
    if (game_live_) pointer_event(PointerEvent::cancel, rf);
    game_live_ = false;
}

// Before step_ticks, so this frame's ticks see the pointer. The menu edge toggles the panel (plan 3), which then owns
// the trigger: no game pointer while it is open. An unfocused session (no input) cancels a held pointer, so a pause
// never leaves a BEGIN without its END or CANCEL.
void QxSession::update_pointer(const ar_render_frame &rf) {
    const QxActionState &now = platform_.actions(), &before = platform_.actions_before();
    if (now.menu && !before.menu) {
        panel_open_ = !panel_open_;
        vos_log("AR_PANEL toggle open=%d", panel_open_ ? 1 : 0);
        if (panel_open_) cancel_game_pointer(rf);
    }
    if (!platform_.input_focused()) {
        cancel_game_pointer(rf);
        owner_ = PointerOwner::none;   // the platform's router ended the placer's drag with the focus too
        return;
    }
    // The platform routed this sample already (in wait_frame, so its placer acted on the same decision first).
    const PointerOwner prev = owner_;
    const PointerDecision d = platform_.pointer_decision();
    owner_ = d.owner;
    if (prev == PointerOwner::game && d.cancel) cancel_game_pointer(rf);
    if (prev == PointerOwner::game && d.end) {
        if (game_live_) pointer_event(PointerEvent::end, rf);
        game_live_ = false;
    }
    if (d.owner == PointerOwner::game && d.begin) {
        pointer_id_++;
        game_live_ = claim_ && !panel_open_ && rf.table_valid && pointer_event(PointerEvent::begin, rf);
    } else if (d.owner == PointerOwner::game && game_live_) {
        if (panel_open_) cancel_game_pointer(rf);
        else pointer_event(PointerEvent::move, rf);
    }
}

void QxSession::step_ticks() { vos_step_due_ticks(rt_, hold_.limit(), nullptr, nullptr); }

void QxSession::record_perf(int64_t display_ns, std::chrono::steady_clock::time_point c0, uint64_t ticks_before) {
    const double cpu_ms = ms_since(c0);
    const uint32_t k = (uint32_t)(ar_runtime_ticks_done(rt_) - ticks_before);
    perf_.frame(display_ns, cpu_ms, k, dev_->last_gpu_ms());
    const double gap_ms = perf_prev_ns_ ? double(display_ns - perf_prev_ns_) * 1e-6 : 0.0;
    perf_prev_ns_ = display_ns;
    if (cpu_ms > 100.0 || gap_ms > 100.0)
        vos_log("AR_HITCH tick=%llu gap_ms=%.1f cpu_ms=%.1f step_ms=%.1f ticks=%u", (unsigned long long)ar_runtime_ticks_done(rt_),
                gap_ms, cpu_ms, step_ms_, k);
    const RoomLightStatus ls = light_.status();
    const bool flipped = (ls.room ? 1 : 0) != light_room_logged_;
    bool window = false;
    if (perf_.frames() >= 900) { log_perf_window(); window = true; }
    if (ls.reduce_error[0] && light_reduce_error_logged_ != ls.reduce_error) {
        vos_log("AR_LIGHT reduce_error=%s", ls.reduce_error);
        light_reduce_error_logged_ = ls.reduce_error;
    }
    if (window && dev_) vos_log("AR_MSAA samples=%u late_passes=%llu", samples_, (unsigned long long)dev_->late_passes());
    if (window || flipped) {
        vos_log("AR_LIGHT %s", room_light_line(ls).c_str());
        light_room_logged_ = ls.room ? 1 : 0;
    }
    if (renderer_) {
        const uint64_t bakes = renderer_->ao_bakes();
        uint64_t timed = 0;
        double bake_ms = -1.0;
        const bool have = dev_->ao_bake_time(&timed, &bake_ms);
        if (bakes > 0 && ((bakes != ao_bakes_logged_ && have && timed == bakes) || window)) {
            const AoVolume v = renderer_->last_ao();
            vos_log("AR_AO bakes=%llu dims=%u,%u,%u voxel=%.1f bake ms=%.2f cpu_ms=%.2f", (unsigned long long)bakes, v.dims[0],
                    v.dims[1], v.dims[2], v.voxel, have && timed == bakes ? bake_ms : -1.0, renderer_->last_ao_cpu_ms());
            ao_bakes_logged_ = bakes;
        }
        if (renderer_->ao_failures() != ao_failures_logged_) {
            ao_failures_logged_ = renderer_->ao_failures();
            vos_log("AR_AO failed=%llu", (unsigned long long)ao_failures_logged_);
        }
    }
}

// No render probe on the Quest, so no safe-mode fallback: the store's look as it is.
LookSettings QxSession::effective_look() const { return look_store_ ? look_store_->get() : look_; }

void QxSession::apply_tex_filter() {
    if (int(look_.edge_aa) != edge_aa_) {
        edge_aa_ = int(look_.edge_aa);
        dev_->set_edge_coverage(look_.edge_aa);
        vos_log("AR_RENDER edge_aa=%d", edge_aa_);
    }
    if (int(look_.tex_smooth) == tex_smooth_) return;
    tex_smooth_ = int(look_.tex_smooth);
    dev_->set_texture_filter(look_.tex_smooth ? TextureFilter::smooth : TextureFilter::vanilla);
    vos_log("AR_RENDER tex_filter=%s", look_.tex_smooth ? "smooth" : "vanilla");
}

LightingFrame QxSession::update_lighting(const ar_render_frame &rf) {
    if (look_store_ && look_store_->generation() != look_gen_) look_ = look_store_->get(&look_gen_);
    return qx_lighting_step(light_, look_, rf);   // no environment reduction to begin or encode: the fixed key light
}

// Nothing is drawn, or what was drawn cannot be vouched for: submit no views, so the XR frame carries passthrough only
// (an acquired image is still released), and release the runtime frame here, since no GPU completion will (as
// vos_submit_if_empty).
void QxSession::submit_empty(const ar_render_frame &rf) {
    ar_render_frame none = rf;
    none.view_count = 0;
    ar_runtime_submit_frame(rt_, &none);
    platform_.end_submission();
    ar_runtime_end_frame(rt_, &rf);
}

bool QxSession::frame() {
    if (!rt_) return false;
    ar_render_frame rf{};
    if (!ar_runtime_begin_frame(rt_, &rf)) return false;
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t ticks_before = ar_runtime_ticks_done(rt_);
    table_valid_ = rf.table_valid != 0;
    const LightingFrame lf = update_lighting(rf);       // before step_ticks: this frame's ticks already use the steering
    update_pointer(rf);
    // The ticks stepped below read this frame's head but are first shown by the next frame; the view matrices always
    // use the current head.
    step_ticks();
    step_ms_ = ms_since(c0);
    if (!rf.snapshot || rf.view_count == 0 || !platform_.frame_renderable()) {
        submit_empty(rf);
        record_perf(rf.display_time_ns, c0, ticks_before);
        return true;
    }
    uint32_t tw = 0;
    const uint32_t th = rf.views[0].height, tl = rf.view_count;
    for (uint32_t i = 0; i < rf.view_count; i++) tw = std::max(tw, rf.views[i].width);
    // A change recreates the target; its pipelines are first warmed in the background while frames keep the old count,
    // so the change never stalls the render thread.
    uint32_t samples = dev_->supported_samples(look_.msaa, tl);
    if (samples != samples_ && samples_ != 0) {
        if (msaa_warming_ != samples) {
            msaa_warming_ = samples;
            const size_t n = dev_->warm_pipelines_background(merge_pipeline_lists(pipeline_file_, dev_->pipeline_list()),
                                                             Renderer::fixed_pipeline_draws(), samples);
            vos_log("AR_WARMUP samples=%u background=%zu (applies once built)", samples, n);
        }
        if (dev_->pipelines_warming() > 0) samples = samples_;
    } else if (msaa_warming_ != 0 && dev_->pipelines_warming() == 0) {
        msaa_warming_ = 0;
    }
    if (tw != w_ || th != h_ || tl != layers_ || samples != samples_) {
        dev_->wait_idle();
        if (samples != samples_ && samples_ != 0) {
            const auto t0 = std::chrono::steady_clock::now();
            const size_t n = dev_->warm_pipelines(merge_pipeline_lists(pipeline_file_, dev_->pipeline_list()), samples);
            vos_log("AR_WARMUP samples=%u pipelines=%zu ms=%.0f", samples, n, ms_since(t0));
            msaa_warming_ = 0;
        }
        dev_->resize_target(target_, tw, th, tl, samples);
        if (samples != samples_)
            vos_log("AR_MSAA samples=%u requested=%u memoryless=%d late_passes=%llu", samples, (unsigned)look_.msaa,
                    dev_->memoryless_msaa() ? 1 : 0, (unsigned long long)dev_->late_passes());
        w_ = tw; h_ = th; layers_ = tl; samples_ = samples;
        vos_log("AR_RENDER layered=1 target=%ux%ux%u samples=%u", tw, th, tl, samples);
    }
    ArLayerOptions lo;
    lo.layered = true;
    lo.view_w = platform_.swapchain_width();   // no rate map: the views are the swapchain's size
    lo.view_h = platform_.swapchain_height();
    lo.table = nullptr;                        // no fake table (plan 3 may add the table frame)
    lo.lighting = &lf;
    lo.points = &points_;
    const float dt = have_time_ ? std::max(0.f, float(rf.display_time_ns - last_time_) * 1e-9f) : 1.f / 90.f;
    lo.decal_fix = look_.decal_fix;
    lo.world_clip_floor = wclip_floor_; lo.world_clip_box = wclip_box_;
    apply_tex_filter();
    const bool linearize = vos_linearize(look_.colour, platform_.color_wants_linear());
    if (int(linearize) != logged_linearize_ || look_.colour != logged_colour_) {
        ExternalView ev0{};
        const uint32_t format = platform_.external_view(0, &ev0) && ev0.color
                                    ? (uint32_t)static_cast<const QxSwapchainImage *>(ev0.color)->format : 0u;
        vos_log("AR_COLOR format=%u linear_target=%d colour=%s linearize=%d", format, (int)platform_.color_wants_linear(),
                look_get(look_, "colour").c_str(), (int)linearize);
        logged_linearize_ = int(linearize);
        logged_colour_ = look_.colour;
    }
    dev_->begin_frame();
    draw_ar_layer(*renderer_, target_, rf, lo, dt);   // the table-shadow box and set_fine_textures are inside
    bool resolved = true;
    for (uint32_t i = 0; i < rf.view_count; i++) {
        ExternalView ev{};
        if (!platform_.external_view(i, &ev)) { resolved = false; continue; }
        ev.src_x = 0;
        ev.src_y = 0;
        ev.src_layer = i;
        ev.linearize = linearize;
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
    for (std::string why; dev_->take_pipeline_error(&why);)
        vos_log("AR_ERROR pipeline refused: %s (MSAA now %u)", why.c_str(), dev_->supported_samples(4, layers_));
    save_pipelines();
    for (std::string key; dev_->take_deferred_miss(&key);)
        vos_log("AR_WARMUP miss=%s deferred", key.c_str());
    last_time_ = rf.display_time_ns;
    have_time_ = true;
    if (!logged_first_) {
        vos_log("AR_FIRST_FRAME views=%u size=%ux%u", rf.view_count, tw, th);
        logged_first_ = true;
    }
    record_perf(rf.display_time_ns, c0, ticks_before);
    return true;
}

void QxSession::stop() {
    if (dev_) dev_->wait_idle();   // every completion has run (ended its runtime frame) before the runtime goes
    if (dev_ && rt_) {
        save_pipelines();
        if (perf_.frames() > 0) log_perf_window();
    }
    if (rt_ && game_live_ && claim_) {   // a held pointer is cancelled before the game goes
        PointerEvent p;
        p.id = pointer_id_;
        p.phase = PointerEvent::cancel;
        claim_(p);
    }
    game_live_ = false;
    owner_ = PointerOwner::none;
    if (rt_) ar_runtime_destroy(rt_);
    rt_ = nullptr;
    renderer_.reset();
    dev_.reset();
    target_ = w_ = h_ = layers_ = samples_ = 0;
    msaa_warming_ = 0;
    pipelines_saved_ = at_draw_queued_ = 0;
    perf_prev_ns_ = 0;
    edge_aa_ = tex_smooth_ = logged_linearize_ = -1;
    logged_colour_ = 0xFF;
    logged_first_ = have_time_ = false;
}

}  // namespace ar
