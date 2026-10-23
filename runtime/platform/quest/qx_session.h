// The Quest session: the shared runtime and renderer on QxPlatform's OpenXR frame loop and the Vulkan device, the
// counterpart of VosSession without foveation, the render probe or sarec replays: the room-lit look from
// a LookStore (the fixed key light: the Quest has no environment probe), MSAA with background pipeline warming, the
// pipeline list at files_dir/pipelines.txt, and the AR_PERF / AR_HITCH / AR_MSAA / AR_LIGHT / AR_AO logs.
//
// Threads: every member runs on the thread that calls start() and frame() (android_main's loop), which also steps the
// game (vos_step_due_ticks, as visionOS's layer thread), except the device's completion thread (it only ends runtime
// frames) and the warm-up threads inside the device.
#pragma once
#include <stdint.h>
#include <atomic>
#include <functional>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include "ar_runtime.h"
}
#include "ar_audio_settings.h"
#include "look.h"
#include "panel_model.h"
#include "panel_place.h"
#include "qx_audio.h"
#include "qx_platform.h"
#include "render_device.h"
#include "renderer.h"
#include "room_light_driver.h"
#include "vos_input.h"
#include "vos_perf.h"
#include "vos_state.h"

namespace ar {
namespace panel { class Raster; }

class QxSession {
public:
    QxSession(const ar_adapter *adapter, QxPlatform &platform, const std::string &files_dir);
    ~QxSession();
    QxSession(const QxSession &) = delete;
    QxSession &operator=(const QxSession &) = delete;
    // The Vulkan device (pipeline cache at files_dir/pipelines.vkcache, async pipelines, the passes prepared, the
    // pipelines of files_dir/pipelines.txt warmed), the renderer, the runtime, init_game and the texture sink. After the
    // platform started. On failure everything it made is torn down again.
    bool start(const std::string &rom_path, const std::string &save_dir, std::string *err);
    bool frame();        // one display frame; false when the platform stopped
    void stop();         // save the pipeline list, wait_idle, destroy; safe to call twice and after a failed start
    uint64_t ticks_done() const;
    std::string status() const;   // one line: the game, its tick, whether the table is placed
    // Before start; the store outlives the session. Re-read on the render thread when its generation changes.
    void set_look_store(const LookStore *store) { look_store_ = store; }
    // Before start; the store outlives the session. On a generation change (render thread): the spatial tuning
    // (spatializer and show_sources have no use on the Quest).
    void set_audio_store(const AudioStore *store) { audio_store_ = store; }
    // The game's audio choices (sm64's "sound" and "resampler" settings), read before init_game and then once a second;
    // unset (any other game): stereo. spatial at start puts the runtime in SOURCES mode, which cannot change after
    // init_game: a later switch to spatial in a game started in stereo is logged (AR_AUDIO spatial needs restart) and
    // stays stereo; any other switch restarts the stream in the other mode.
    struct AudioChoice { bool spatial = false; int resampler = 0; };   // resampler: feed_choice's index
    void set_audio_choice(std::function<AudioChoice()> fn) { audio_choice_ = std::move(fn); }
    void set_world_clip(bool floor, bool box) { wclip_floor_ = floor; wclip_box_ = box; }
    // The key light's steering (on, direction in table axes) whenever it changes; render thread.
    void set_key_light_callback(void (*fn)(void *ctx, int on, const float dir[3]), void *ctx) { light_.set_key_light_callback(fn, ctx); }
    // The game pointer (sm64's title screen and menus), as visionOS's set_pointer_claim: the right controller's aim
    // ray while its trigger is held, in table space with the head, offered at BEGIN only while the table is placed and
    // the panel is closed; the claim's MOVE / END / CANCEL follow whatever it returns at BEGIN (true: claimed). Before
    // start; empty (the default): no game pointer.
    void set_pointer_claim(PointerClaim claim) { claim_ = std::move(claim); }
    // Called after each AR_PERF window is logged (render thread), as VosSession's.
    void set_perf_window_callback(void (*fn)(void *ctx), void *ctx) { perf_window_fn_ = fn; perf_window_ctx_ = ctx; }
    // Before start; the box outlives the session. The room light's status after every frame (arvos_room_light_get).
    void set_room_light_status(RoomLightStatusBox *box) { light_box_ = box; }
    // The game audio runs in spatial mode (SOURCES with the stream up); any thread (vos_session.h's audio_spatial).
    bool audio_spatial() const { return spatial_ok_.load(std::memory_order_acquire); }
    bool panel_open() const { return panel_.open; }   // toggled by the left menu button
    // The settings panel's calls into the app (arvos_* behind std::functions); before the first frame. The panel's tab
    // is read and stored through it as "ui.tab".
    void set_panel_bridge(const panel::Bridge &b) { bridge_ = b; }
    // The panel's texture id: below the renderer's own ids (0xFFFFFF00 and up), never an adapter's.
    static constexpr uint32_t kPanelTexture = 0xFFFFFE01u;
    static constexpr float kPanelWidthM = 0.6f;
    PointerOwner pointer_owner() const { return owner_; }
    LookSettings effective_look() const;
    void apply_tex_filter();   // the look's edge_aa and tex_smooth onto the device, when they changed

private:
    LightingFrame update_lighting(const ar_render_frame &rf);
    void step_ticks();
    void update_pointer(const ar_render_frame &rf);
    void open_panel();
    void close_panel();
    void update_panel(const ar_render_frame &rf, float dt_s);
    void apply_panel_action(const panel::Action &a);
    void draw_panel(const ar_render_frame &rf);
    bool pointer_event(PointerEvent::Phase phase, const ar_render_frame &rf);
    void cancel_game_pointer(const ar_render_frame &rf);
    void warm_pipelines(uint32_t samples);
    void warm_in_background(uint32_t samples);
    void save_pipelines();
    void log_perf_window();
    void record_perf(int64_t display_ns, std::chrono::steady_clock::time_point cpu_start, uint64_t ticks_before);
    void submit_empty(const ar_render_frame &rf);
    void update_audio(const ar_render_frame &rf);
    void apply_audio_store();
    void poll_audio_choice();

    const ar_adapter *adapter_;
    QxPlatform &platform_;
    std::string files_dir_;
    std::unique_ptr<IRenderDevice> dev_;
    std::unique_ptr<Renderer> renderer_;
    ar_runtime *rt_ = nullptr;
    uint32_t target_ = 0, w_ = 0, h_ = 0, layers_ = 0, samples_ = 0;
    int64_t last_time_ = 0;
    bool have_time_ = false, logged_first_ = false, table_valid_ = false;
    const LookStore *look_store_ = nullptr;
    uint64_t look_gen_ = 0;
    LookSettings look_{};
    RoomLightDriver light_;
    std::atomic<bool> wclip_floor_{false}, wclip_box_{false};
    HoldSchedule hold_{{}};
    uint32_t msaa_warming_ = 0;
    int edge_aa_ = -1, tex_smooth_ = -1;
    int logged_linearize_ = -1;
    uint8_t logged_colour_ = 0xFF;
    std::vector<DebugPoint> points_;   // always empty (the sound sources' points come with plan 3's audio)
    double step_ms_ = 0.0;
    int64_t perf_prev_ns_ = 0;
    PerfWindow perf_;
    int light_room_logged_ = -1;
    std::string light_reduce_error_logged_;
    uint64_t ao_bakes_logged_ = 0, ao_failures_logged_ = 0;
    size_t pipelines_saved_ = 0, at_draw_queued_ = 0;
    std::string pipeline_file_;
    PointerClaim claim_;
    PointerOwner owner_ = PointerOwner::none;
    bool game_live_ = false;           // a BEGIN was claimed: an END or CANCEL is still owed
    uint64_t pointer_id_ = 0;
    float ray_o_[3] = {0.f, 0.f, 0.f}, ray_d_[3] = {0.f, 0.f, -1.f};   // the last valid right aim ray, tracking space
    // The settings panel (render thread): open while toggled by the menu button. While open it owns the right trigger
    // and the right stick X on a hovered slider; tabs_ is rebuilt from the bridge every frame and the texture re-drawn
    // (at most once a frame) when what it shows changed.
    struct PanelState { bool open = false; int tab = 0; int scroll_rows = 0; panel::Hit hover; bool dirty = true; };
    PanelState panel_;
    panel::Bridge bridge_;
    panel::Layout layout_;
    std::vector<panel::Tab> tabs_;
    std::unique_ptr<panel::Raster> raster_;
    panel::PanelPose pose_{};
    bool pose_ok_ = false, aim_ok_ = false, aim_hit_ = false;
    float aim_o_[3] = {0.f, 0.f, 0.f}, aim_d_[3] = {0.f, 0.f, -1.f}, aim_end_[3] = {0.f, 0.f, 0.f};
    std::string panel_sig_;
    panel::Slide slide_;   // the slider the stick is moving: its last value sent (shown) and the raw accumulator
    float scroll_acc_ = 0.f;
    // tabs_, the status and hint text (and so the room-light readout) are rebuilt once a second, on any panel action and
    // on open, not every frame; hover and hit testing run every frame against the cached tabs.
    std::string status_cache_, hint_cache_;
    std::chrono::steady_clock::time_point tabs_built_{};
    bool tabs_stale_ = true;
    void refresh_tabs(bool force);
    // Game audio (render thread), running whenever the game runs, as visionOS's (the game's own "audio" setting mutes
    // inside the game). audio_on_: the stream started; audio_focus_: the last session focus applied to it (pause when
    // leaving FOCUSED, resume on return).
    std::unique_ptr<QxAudio> audio_;
    bool audio_on_ = false, audio_focus_ = true;
    const AudioStore *audio_store_ = nullptr;
    uint64_t audio_gen_ = 0;
    std::function<AudioChoice()> audio_choice_;
    bool audio_sources_ = false;   // the runtime was put in SOURCES mode before init_game
    bool want_spatial_ = false;    // the game's last "sound" choice
    int resampler_now_ = -1;
    std::chrono::steady_clock::time_point choice_at_{};
    RoomLightStatusBox *light_box_ = nullptr;
    void (*perf_window_fn_)(void *) = nullptr;
    void *perf_window_ctx_ = nullptr;
    std::atomic<bool> spatial_ok_{false};
};

}
