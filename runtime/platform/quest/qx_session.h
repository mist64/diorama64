// The Quest session: the shared runtime and renderer on QxPlatform's OpenXR frame loop and the Vulkan device, the
// counterpart of VosSession without audio (plan 3), foveation, the render probe or sarec replays: the room-lit look from
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
#include "look.h"
#include "qx_audio.h"
#include "qx_platform.h"
#include "render_device.h"
#include "renderer.h"
#include "room_light_driver.h"
#include "vos_input.h"
#include "vos_perf.h"
#include "vos_state.h"

namespace ar {

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
    void set_world_clip(bool floor, bool box) { wclip_floor_ = floor; wclip_box_ = box; }
    // The key light's steering (on, direction in table axes) whenever it changes; render thread.
    void set_key_light_callback(void (*fn)(void *ctx, int on, const float dir[3]), void *ctx) { light_.set_key_light_callback(fn, ctx); }
    // The game pointer (sm64's title screen and menus), as visionOS's set_pointer_claim: the right controller's aim
    // ray while its trigger is held, in table space with the head, offered at BEGIN only while the table is placed and
    // the panel is closed; the claim's MOVE / END / CANCEL follow whatever it returns at BEGIN (true: claimed). Before
    // start; empty (the default): no game pointer.
    void set_pointer_claim(PointerClaim claim) { claim_ = std::move(claim); }
    bool panel_open() const { return panel_open_; }   // toggled by the left menu button (plan 3's panel)
    PointerOwner pointer_owner() const { return owner_; }
    LookSettings effective_look() const;
    void apply_tex_filter();   // the look's edge_aa and tex_smooth onto the device, when they changed

private:
    LightingFrame update_lighting(const ar_render_frame &rf);
    void step_ticks();
    void update_pointer(const ar_render_frame &rf);
    bool pointer_event(PointerEvent::Phase phase, const ar_render_frame &rf);
    void cancel_game_pointer(const ar_render_frame &rf);
    void warm_pipelines(uint32_t samples);
    void warm_in_background(uint32_t samples);
    void save_pipelines();
    void log_perf_window();
    void record_perf(int64_t display_ns, std::chrono::steady_clock::time_point cpu_start, uint64_t ticks_before);
    void submit_empty(const ar_render_frame &rf);
    void update_audio();

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
    bool panel_open_ = false;
    // Game audio (render thread), running whenever the game runs, as visionOS's (the game's own "audio" setting mutes
    // inside the game). audio_on_: the stream started; audio_focus_: the last session focus applied to it (pause when
    // leaving FOCUSED, resume on return).
    std::unique_ptr<QxAudio> audio_;
    bool audio_on_ = false, audio_focus_ = true;
};

}
