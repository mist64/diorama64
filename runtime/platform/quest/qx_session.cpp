// runtime/platform/quest/qx_session.cpp — one frame, as vos_session.cpp's: runtime begin (xrWaitFrame, xrBeginFrame,
// acquire) → lighting → game ticks → AR layer into our layered target → resolve each eye into the swapchain slice →
// submit (views to the platform) → GPU submit → end submission (release the image, xrEndFrame) → the completion thread
// ends the runtime frame.
#include "qx_session.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include "ar_layer.h"
#include "ar_mem_stats.h"
#include "lighting.h"
#include "panel_raster.h"
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
// The settings panel's quad: its texture as is (TEXEL0 colour and alpha), blended, depth-tested, never writing depth.
DrawPacket panel_packet() {
    DrawPacket p{};
    p.vertex_count = 6;
    p.cc = normalize_combiner(fc_combiner_key{((uint64_t)FC_SRC_TEXEL0 << 12) | ((uint64_t)FC_SRC_TEXEL0 << 28), (uint32_t)FC_CC_ALPHA, 0});
    p.tex[0] = QxSession::kPanelTexture;
    p.samp[0] = fc_sampler{1, FC_WRAP_CLAMP, FC_WRAP_CLAMP, 0};
    p.depth_test = 1; p.depth_write = 0; p.blend = 1; p.cull = FC_CULL_NONE;
    return p;
}
// The renderer's fixed draws plus the panel's, for the background warm-ups.
std::vector<DrawPacket> fixed_draws() {
    std::vector<DrawPacket> v = Renderer::fixed_pipeline_draws();
    v.push_back(panel_packet());
    return v;
}
constexpr const char *kPanelFont = "/system/fonts/Roboto-Regular.ttf";
constexpr float kStickDeadZone = 0.2f;
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
    if (platform_.api()->set_passthrough) platform_.api()->set_passthrough(platform_.api()->user, 1);   // parity: always on
    ar_runtime_set_texture_sink(rt_, upload, dev_.get());
    if (!save_dir.empty() && ::mkdir(save_dir.c_str(), 0700) != 0 && errno != EEXIST)
        vos_log("AR_QUEST save folder %s: mkdir failed (errno %d)", save_dir.c_str(), errno);
    // The audio mode is fixed at init_game: SOURCES when the game asks for spatial sound and the adapter has lanes.
    const AudioChoice choice = audio_choice_ ? audio_choice_() : AudioChoice{};
    audio_sources_ = choice.spatial && ar_runtime_set_audio_mode(rt_, AR_AUDIO_MODE_SOURCES) != 0;
    if (choice.spatial && !audio_sources_) vos_log("AR_AUDIO spatial refused: the adapter is stereo only");
    want_spatial_ = choice.spatial;
    resampler_now_ = choice.resampler;
    choice_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    if (!ar_runtime_init_game(rt_, rom_path.c_str(), save_dir.c_str())) {
        const char *why = adapter_->last_error ? adapter_->last_error() : nullptr;
        return fail(std::string("init_game failed: ") + (why && *why ? why : "(no reason given)"));
    }
    // No game thread: frame() steps the game (step_ticks), as VosSession.
    vos_log("AR_QUEST_SESSION adapter=%s", adapter_->name);
    // After init_game, whatever the game's settings (as VosSession); a stream that cannot start leaves the game silent.
    audio_.reset(new QxAudio(rt_));
    audio_focus_ = true;   // the stream starts playing; the first unfocused frame pauses it
    audio_gen_ = 0;
    apply_audio_store();
    audio_->set_feed_choice(feed_choice(choice.resampler));
    std::string why;
    audio_on_ = audio_->start(audio_sources_, &why);
    if (!audio_on_) vos_log("AR_AUDIO off reason=%s", why.c_str());
    return true;
}

// The store's tuning, when its generation changed (VosSession::frame's block, without the spatializer).
void QxSession::apply_audio_store() {
    if (!audio_ || !audio_store_ || audio_store_->generation() == audio_gen_) return;
    const AudioSettings a = audio_store_->get(&audio_gen_);
    const ar_audio_tuning t = audio_tuning_from_settings(a);
    audio_->set_tuning(t);
    vos_log("AR_AUDIO tuning head_focus=%g distance_exponent=%g spotlight=%g cone=%g widen=%g sfx_db=%g music_db=%g",
            (double)a.head_focus, (double)t.distance_exponent, (double)t.spotlight_floor_db, (double)t.spotlight_cone_deg,
            (double)t.widen, (double)t.sfx_db, (double)t.music_db);
}

// Once a second: the resampler's mode goes to the feed live; a changed "sound" restarts the stream in the other mode
// (stop, then start: the callback is gone in between), except into spatial in a game started in stereo.
void QxSession::poll_audio_choice() {
    const AudioChoice c = audio_choice_();
    if (c.resampler != resampler_now_) {
        resampler_now_ = c.resampler;
        const FeedChoice fc = feed_choice(c.resampler);
        audio_->set_feed_choice(fc);   // polyphase itself applies at the next spatial start
        vos_log("AR_AUDIO resampler=%s", fc.mode == PolyMode::linear ? "linear" : "sinc");
    }
    if (c.spatial == want_spatial_) return;
    want_spatial_ = c.spatial;
    if (c.spatial && !audio_sources_) vos_log("AR_AUDIO spatial needs restart (the game started in stereo)");
    const bool spatial = c.spatial && audio_sources_;
    if (audio_on_ && audio_->spatial() == spatial) return;
    audio_->stop();
    std::string why;
    audio_on_ = audio_->start(spatial, &why);
    audio_focus_ = true;   // playing again: an unfocused session pauses it below
    vos_log("AR_AUDIO mode=%s%s%s", spatial ? "spatial" : "stereo", audio_on_ ? "" : " off reason=", audio_on_ ? "" : why.c_str());
}

// Each frame, before the ticks: the store's tuning and (once a second) the game's choices, the head for the spatial
// plan, then the session's focus pauses or resumes the stream (neither blocks) and poll() starts a resumed stream once
// its stop settled and reopens a disconnected one.
void QxSession::update_audio(const ar_render_frame &rf) {
    if (!audio_) return;
    apply_audio_store();
    if (audio_choice_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= choice_at_) {
            choice_at_ = now + std::chrono::seconds(1);
            poll_audio_choice();
        }
    }
    if (!audio_on_) return;
    audio_->listener(ar_pose_mul(rf.tracking_from_table, rf.head_table), rf.tracking_from_table);
    const ar_platform *api = platform_.api();
    const bool focused = api->session_state && api->session_state(api->user) == AR_SESSION_FOCUSED;
    if (focused != audio_focus_) {
        audio_focus_ = focused;
        if (focused) audio_->resume();
        else audio_->pause();
        vos_log("AR_AUDIO %s frames=%llu missing=%llu trimmed=%llu", focused ? "resume" : "pause",
                (unsigned long long)audio_->frames_out(), (unsigned long long)audio_->missing_in(),
                (unsigned long long)audio_->trimmed_in());
    }
    audio_->poll();
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
                                                     fixed_draws(), samples);
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
    // The window's line has hz, cpu_ms p50/p95/max and gpu_ms p50/p95/max already; add the device's counters.
    vos_log("%s late=%llu deferred=%llu gpu_last_ms=%.2f", perf_line(perf_.report()).c_str(),
            (unsigned long long)dev_->late_passes(), (unsigned long long)dev_->deferred_draws(), dev_->last_gpu_ms());
    vos_log("AR_MEM phys_mb=%.1f", ar_phys_footprint_mb());
    vos_log("AR_WARMUP deferred_draws=%llu warming=%zu built=%zu promoted=%llu", (unsigned long long)dev_->deferred_draws(),
            dev_->pipelines_warming(), dev_->pipeline_count(), (unsigned long long)dev_->warm_promotions());
    if (audio_on_)
        vos_log("AR_AUDIO running=%d frames=%llu missing=%llu trimmed=%llu", audio_->running() ? 1 : 0,
                (unsigned long long)audio_->frames_out(), (unsigned long long)audio_->missing_in(),
                (unsigned long long)audio_->trimmed_in());
    if (perf_window_fn_) perf_window_fn_(perf_window_ctx_);
    perf_.reset();
}

uint64_t QxSession::ticks_done() const { return rt_ ? ar_runtime_ticks_done(rt_) : 0; }

std::string QxSession::status() const {
    char b[448];
    const bool on = audio_ && audio_on_, spatial = on && audio_->spatial();
    int n = std::snprintf(b, sizeof b, "%s: tick %llu, table %s, audio=%s frames=%llu missing=%llu trimmed=%llu", adapter_->name,
                          (unsigned long long)ticks_done(), table_valid_ ? "placed" : "not placed yet",
                          on ? (spatial ? "spatial" : "stereo") : "off", on ? (unsigned long long)audio_->frames_out() : 0ull,
                          on ? (unsigned long long)audio_->missing_in() : 0ull, on ? (unsigned long long)audio_->trimmed_in() : 0ull);
    if (spatial && n > 0 && (size_t)n < sizeof b)
        std::snprintf(b + n, sizeof b - (size_t)n, " busy=%llu stale=%llu", (unsigned long long)audio_->feed_busy(),
                      (unsigned long long)audio_->feed_stale());
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
        if (panel_.open) close_panel();
        else open_panel();
        vos_log("AR_PANEL toggle open=%d", panel_.open ? 1 : 0);
        if (panel_.open) cancel_game_pointer(rf);
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
        game_live_ = claim_ && !panel_.open && rf.table_valid && pointer_event(PointerEvent::begin, rf);
    } else if (d.owner == PointerOwner::game && game_live_) {
        if (panel_.open) cancel_game_pointer(rf);
        else pointer_event(PointerEvent::move, rf);
    }
}

// ---- The settings panel ----
void QxSession::open_panel() {
    panel_.open = true;
    panel_.scroll_rows = 0;
    panel_.hover = panel::Hit{};
    panel_.dirty = true;
    pose_ok_ = false;
    panel_sig_.clear();
    slide_ = panel::Slide{};
    if (platform_.input_focused() && platform_.actions().rt > 0.5f) platform_.hold_until_released(AR_BTN_RB);
    refresh_tabs(true);
    const std::string want = bridge_.get ? bridge_.get("ui.tab") : std::string();
    panel_.tab = 0;
    for (size_t i = 0; i < tabs_.size(); i++) if (tabs_[i].name == want) panel_.tab = (int)i;
    if (!raster_) {
        raster_.reset(new panel::Raster(layout_.width, layout_.height));
        const bool font = raster_->load_font(kPanelFont);
        vos_log("AR_PANEL font=%s", font ? kPanelFont : "builtin 5x7");
    }
    vos_log("AR_PANEL open tab=%s tabs=%zu", tabs_.empty() ? "-" : tabs_[(size_t)panel_.tab].name.c_str(), tabs_.size());
}

void QxSession::close_panel() {
    if (!slide_.id.empty() && bridge_.set) bridge_.set(slide_.id, slide_.sent, true);   // a slide in progress ends final
    slide_ = panel::Slide{};
    panel_.open = false;
    aim_ok_ = aim_hit_ = false;
}

// Rebuilds the cached tabs and the status / hint strings (one bridge read each) when forced, flagged stale or a second old.
void QxSession::refresh_tabs(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && !tabs_stale_ && now - tabs_built_ < std::chrono::seconds(1)) return;
    tabs_ = panel::build_tabs(bridge_);
    status_cache_ = bridge_.status ? bridge_.status() : std::string();
    hint_cache_ = bridge_.hint ? bridge_.hint() : std::string();
    tabs_built_ = now;
    tabs_stale_ = false;
}

void QxSession::apply_panel_action(const panel::Action &a) {
    if (a.kind != panel::Action::none) tabs_stale_ = true;   // rebuilt by update_panel after this frame's action
    switch (a.kind) {
    case panel::Action::select_tab:
        if (a.tab < 0 || a.tab >= (int)tabs_.size()) return;
        panel_.tab = a.tab;
        panel_.scroll_rows = 0;
        if (bridge_.set) bridge_.set("ui.tab", a.value, true);
        vos_log("AR_PANEL tab=%s", a.value.c_str());
        return;
    case panel::Action::set:
        if (!bridge_.set) return;
        // The stick slides: values go live, the final one when the stick lets go or the ray leaves. A final set (the
        // trigger) on the sliding id ends the slide, so the release does not overwrite it (slide_apply).
        if (!panel::slide_apply(slide_, a)) return;
        bridge_.set(a.id, a.value, a.final);
        if (a.final) vos_log("AR_PANEL set %s=%s", a.id.c_str(), a.value.c_str());
        return;
    case panel::Action::press:
        vos_log("AR_PANEL press %s", a.id.c_str());
        if (bridge_.press) bridge_.press(a.id);
        return;
    case panel::Action::scroll:
        panel_.scroll_rows += a.delta;
        return;
    case panel::Action::none: return;
    }
}

// After update_pointer (which toggles the panel): the quad's pose, the right aim ray's hit on it, the hovered control,
// the trigger's rising edge and the right stick X acted on, and whether the texture must be re-drawn.
void QxSession::update_panel(const ar_render_frame &rf, float dt_s) {
    if (!panel_.open) return;
    refresh_tabs(false);
    if (tabs_.empty()) return;
    panel_.tab = std::min(std::max(panel_.tab, 0), (int)tabs_.size() - 1);
    // Over a placed table the panel follows the level and turns to the head; without one it stays where it opened.
    if (rf.table_valid || !pose_ok_) {
        pose_ = panel::place_panel(rf, kPanelWidthM, (float)layout_.width / (float)layout_.height);
        pose_ok_ = true;
    }
    const QxActionState &now = platform_.actions(), &before = platform_.actions_before();
    const bool focused = platform_.input_focused();
    panel::Hit hit;
    aim_ok_ = focused && now.right_aim_valid;
    aim_hit_ = false;
    if (aim_ok_) {
        const float fwd[3] = {0.f, 0.f, -1.f};   // OpenXR's aim pose points down its -Z
        for (int k = 0; k < 3; k++) aim_o_[k] = now.right_aim.pos[k];
        ar_quat_rotate(now.right_aim.quat, fwd, aim_d_);
        float u = 0.f, v = 0.f;
        if (panel::ray_hit_panel(pose_, aim_o_, aim_d_, &u, &v)) {
            aim_hit_ = true;
            panel::panel_point(pose_, u, v, aim_end_);
            hit = panel::hit_test(tabs_, panel_.tab, layout_, u, v, panel_.scroll_rows);
        } else {
            for (int k = 0; k < 3; k++) aim_end_[k] = aim_o_[k] + aim_d_[k];   // 1 m along the ray
        }
    }
    panel_.hover = hit;
    const bool trigger = focused && now.rt > 0.5f && !(before.rt > 0.5f);
    // The panel owns the trigger while open: the game never sees R, on the panel or off it.
    // (armed on the pull, anywhere; the gate holds it until the trigger is released. A pull already held when the panel
    // opens is armed by open_panel.)
    if (trigger) platform_.hold_until_released(AR_BTN_RB);
    std::vector<panel::Control> *cs = hit.tab == panel_.tab && hit.control >= 0 ? &tabs_[(size_t)panel_.tab].controls : nullptr;
    panel::Control *c = cs ? &(*cs)[(size_t)hit.control] : nullptr;
    float sx = focused && std::fabs(now.rx) >= kStickDeadZone ? now.rx : 0.f;
    if (!c || c->kind != panel::Kind::slider) sx = 0.f;   // the stick X belongs to a hovered slider only
    if (!slide_.id.empty() && (sx == 0.f || !c || c->id != slide_.id)) {
        if (bridge_.set) bridge_.set(slide_.id, slide_.sent, true);
        vos_log("AR_PANEL set %s=%s", slide_.id.c_str(), slide_.sent.c_str());
        slide_ = panel::Slide{};
        tabs_stale_ = true;
    }
    if (c && c->id == slide_.id) {   // continue from the unrounded value, so a slow push accumulates
        char b[32];
        std::snprintf(b, sizeof b, "%.7g", (double)slide_.raw);
        c->value = b;
    }
    apply_panel_action(panel::act_on(tabs_, panel_.tab, hit, trigger, sx, dt_s));
    if (tabs_stale_) {   // an action (or a slide's end) changed settings: rebuild before the signature and the draw
        refresh_tabs(false);
        if (tabs_.empty()) return;
        panel_.tab = std::min(panel_.tab, (int)tabs_.size() - 1);
    }
    if (!slide_.id.empty())   // the panel shows the value actually sent, not the raw accumulator
        for (panel::Control &x : tabs_[(size_t)panel_.tab].controls)
            if (x.id == slide_.id) x.value = slide_.sent;
    // The right stick Y scrolls the rows while the ray is on the panel.
    if (aim_hit_ && std::fabs(now.ry) >= 0.5f) {
        scroll_acc_ -= now.ry * 10.f * dt_s;
        const int whole = (int)scroll_acc_;
        panel_.scroll_rows += whole;
        scroll_acc_ -= (float)whole;
    } else {
        scroll_acc_ = 0.f;
    }
    if (panel_.tab >= (int)tabs_.size()) return;
    panel_.scroll_rows = std::min(std::max(panel_.scroll_rows, 0), panel::max_scroll(tabs_, panel_.tab, layout_));
    // What the texture shows, as one string: re-drawn when it changes.
    std::string sig = std::to_string(panel_.tab) + '|' + std::to_string(panel_.scroll_rows) + '|' + std::to_string(hit.tab) + ',' +
                      std::to_string(hit.control) + ',' + std::to_string(hit.scroll) + ',' + (hit.tab_strip ? "t" : "") + '|';
    sig += status_cache_ + '|' + hint_cache_ + '|';
    for (const panel::Control &x : tabs_[(size_t)panel_.tab].controls)
        sig += x.label + '\x1f' + x.value + '\x1f' + x.note + '\x1f' + (x.enabled ? '1' : '0') + '\x1e';
    if (sig != panel_sig_) {
        panel_sig_.swap(sig);
        panel_.dirty = true;
    }
}

// After the AR layer, into the same layered target: one pass per eye (layer = eye) that neither clears nor writes depth,
// then the controller ray.
void QxSession::draw_panel(const ar_render_frame &rf) {
    if (!panel_.open || !pose_ok_ || rf.view_count == 0 || !raster_) return;
    if (panel_.dirty) {
        raster_->draw(tabs_, panel_.tab, panel_.hover, layout_, panel_.scroll_rows, status_cache_, hint_cache_);
        dev_->upload_texture(kPanelTexture, raster_->width(), raster_->height(), raster_->rgba());
        panel_.dirty = false;
    }
    const float hw = 0.5f * pose_.width_m, hh = 0.5f * pose_.height_m;
    const float c[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    const float uv[4][2] = {{0.f, 1.f}, {1.f, 1.f}, {1.f, 0.f}, {0.f, 0.f}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    fc_vertex q[6] = {};
    for (int k = 0; k < 6; k++) {
        q[k].pos[0] = c[idx[k]][0]; q[k].pos[1] = c[idx[k]][1];
        q[k].uv0[0] = uv[idx[k]][0]; q[k].uv0[1] = uv[idx[k]][1];
        for (int ch = 0; ch < 4; ch++) q[k].shade[ch] = 255;
    }
    const DrawPacket p = panel_packet();
    const ar_mat4 tracking_from_panel = ar_mat4_from_pose(pose_.tracking_from_panel);
    const Viewport vp{0.f, 0.f, float(platform_.swapchain_width()), float(platform_.swapchain_height())};
    FrameView tracking[AR_MAX_VIEWS];
    for (uint32_t i = 0; i < rf.view_count; i++) {
        ViewConstants vc{};
        const ar_mat4 cm = ar_mat4_mul(rf.views[i].clip_from_tracking, tracking_from_panel);
        std::memcpy(vc.clip_from_model, cm.m, sizeof vc.clip_from_model);
        vc.layer = i;
        PassDesc pd{};
        pd.clear = false;   // over the AR layer drawn this frame
        pd.depth = true;
        pd.viewport = vp;
        dev_->encode_pass(target_, pd, vc, q, 6, &p, 1);
        tracking[i] = FrameView{rf.views[i].clip_from_tracking, {0.f, 0.f, 0.f}, vp, {0.f, 0.f, 0.f}, i};
    }
    if (!aim_ok_) return;
    PreviewLine ray{{aim_o_[0], aim_o_[1], aim_o_[2], aim_end_[0], aim_end_[1], aim_end_[2]}, {255, 255, 255, 153}, 0.004f};
    const ar_pose head = ar_pose_mul(rf.tracking_from_table, rf.head_table);
    renderer_->draw_lines(target_, std::vector<PreviewLine>{ray}, tracking, rf.view_count, head.pos);
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
    if (light_box_) light_box_->set(ls);
    spatial_ok_.store(audio_ && audio_on_ && audio_->spatial(), std::memory_order_release);
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
    const float frame_dt = have_time_ ? std::min(0.1f, std::max(0.f, float(rf.display_time_ns - last_time_) * 1e-9f)) : 1.f / 90.f;
    update_panel(rf, frame_dt);
    update_audio(rf);
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
                                                             fixed_draws(), samples);
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
    draw_panel(rf);
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
    platform_.set_depth_written(vulkan_device_depth_written(*dev_));
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
    // The stream is stopped and closed before the runtime goes: its callback never runs against a destroyed runtime.
    audio_.reset();
    audio_on_ = false;
    spatial_ok_.store(false, std::memory_order_release);
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
    if (panel_.open) close_panel();
    panel_.dirty = true;   // a new device needs the texture again
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
