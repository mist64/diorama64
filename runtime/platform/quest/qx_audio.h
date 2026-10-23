// The Quest's game audio on AAudio, the counterpart of vos_audio.mm: a low-latency shared float stereo stream at 48 kHz
// whose data callback pulls the game's 32 kHz int16 stereo through AudioLatencyBound (vos_latency.h) and StereoResampler
// (vos_resample.h). Spatial output (start(true, ...)) takes the adapter's scene instead (the runtime in SOURCES mode):
// SceneFeed (vos_scene_audio.h) resamples the bed and the 32 mono lanes, and qx_panner pans the lanes into the stereo
// stream with constant power, where visionOS hands them to PHASE.
//
// Threads: every member is for the render thread (QxSession's) except the counters (any thread). The data callback runs
// on AAudio's real-time thread and touches only rt_, the resampler, the latency bound, the atomics and (spatial) the
// feed, its scratch buffers, the plan slots it takes and its own ramp state. The resampler, bound and feed are replaced (reset_input) only before the first start and after AAudioStream_close, which is documented to end
// the callbacks; never after a mere stop, since STOPPED does not promise the callback has returned (a legacy AudioTrack
// callback may still be inside on_data). A resume therefore just starts the stream again: the bound's first pull drains
// the game's ring and trims it to the standing 800 frames, which is the burst protection. The error callback only sets
// a flag that lives in a never-freed ErrorFlag, so an error callback that outlives the stream or QxAudio stays harmless;
// poll() consumes it.
//
// Spatial: the render thread plans (listener(): the scene metadata popped into ScenePositions, whose deque allocates, the
// lanes at the feed's heard frame, qx_panner_plan with the tuning) and publishes the plan through a triple buffer; the
// callback takes the newest plan without waiting, renders one SceneFeed cycle per chunk and mixes it with
// qx_panner_mix, each lane ramping from the plan the previous chunk ended on. The feed, its scratch buffers and the
// plan slots are made by reset_input (no callback runs then), so the callback allocates nothing and takes no lock.
#pragma once
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <aaudio/AAudio.h>
extern "C" {
#include "ar_audio_tuning.h"
#include "ar_math.h"
#include "ar_runtime.h"
#include "ar_types.h"
}
#include "qx_panner.h"
#include "vos_latency.h"
#include "vos_polyphase.h"
#include "vos_resample.h"
#include "vos_scene_audio.h"
#include "vos_scene_pos.h"

namespace ar {

class QxAudio {
public:
    explicit QxAudio(ar_runtime *rt);
    ~QxAudio();
    QxAudio(const QxAudio &) = delete;
    QxAudio &operator=(const QxAudio &) = delete;
    // AAudio: PERFORMANCE_MODE_LOW_LATENCY, SHARING_MODE_SHARED, float, 2 channels, 48000 Hz, data + error callbacks;
    // started (playing) on success. spatial=true pulls the scene (ar_runtime_pull_audio_scene: the runtime must be in
    // SOURCES mode, else the lanes are silent and the bed carries everything) through a SceneFeed and pans the lanes.
    // To change the mode: stop(), then start() again.
    bool start(bool spatial, std::string *err);
    // Session VISIBLE / FOCUSED transitions; neither blocks. pause() requests the stop; resume() restarts once AAudio
    // reports STOPPED (in poll() if not yet), after dropping the game audio queued meanwhile, so there is no burst.
    void pause();
    void resume();
    // Stops and closes the stream: no callback runs after it returns. Safe to call twice and without start.
    void stop();
    // Each frame: a resume waiting for STOPPED starts the stream (as does a stream found STOPPED on two polls while not
    // paused: a lost resume); a disconnected stream (the error callback's flag, or its state) is closed and reopened
    // with fresh input; a failed reopen is retried once a second, logged at the first failure and then every 10 s.
    void poll();
    bool running() const;   // a stream is open and not paused
    bool spatial() const { return spatial_; }
    uint64_t frames_out() const { return frames_.load(std::memory_order_relaxed); }
    // Both modes' input frames lost (not covered) and trimmed (dropped to bound the latency), summed over every stream.
    uint64_t missing_in() const;
    uint64_t trimmed_in() const;
    uint64_t feed_busy() const;    // the spatial feed's busy and stale cycles, summed over every feed
    uint64_t feed_stale() const;
    // The spatial mode's settings; render thread, any time (kept for the next start). The tuning is clamped; its music_db
    // is the bed's gain (as VosPhase::set_tuning). set_feed_choice: polyphase for the next spatial start, its mode live.
    void set_tuning(const ar_audio_tuning &t);
    void set_poly_mode(PolyMode m);
    void set_feed_choice(const FeedChoice &fc);
    // Render thread, each frame: the head in table space (the listener when the scene's metadata has none) and, when
    // spatial, the next plan (see the thread note above).
    void listener(const ar_pose &tracking_from_head, const ar_pose &tracking_from_table);

private:
    static constexpr uint32_t kMaxChunk = 4096;   // the resampler's max_out_frames: larger requests render in chunks
    static uint32_t pull_game(void *user, int16_t *stereo, uint32_t frames);
    static uint32_t pull_scene(void *user, ar_audio_pull *p);
    void render_spatial(float *out, uint32_t frames);   // the callback's spatial path
    static aaudio_data_callback_result_t on_data(AAudioStream *s, void *user, void *data, int32_t frames);
    static void on_error(AAudioStream *s, void *user, aaudio_result_t error);
    bool open_stream(std::string *err);
    void close_stream();    // requestStop, a wait for STOPPED bounded to 50 ms (logged when it expires), close
    void reset_input();     // before the first start or after close only: a fresh bound and resampler, the ring drained

    ar_runtime *rt_;
    std::unique_ptr<StereoResampler> rs_;
    std::unique_ptr<AudioLatencyBound> bound_;
    std::vector<int16_t> drain_;   // reset_input's scratch
    AAudioStream *stream_ = nullptr;
    bool started_ = false;         // start() succeeded and stop() has not run (the stream may be reopening)
    bool paused_ = false;
    bool start_pending_ = false;   // resumed while the stop was still settling: poll() starts the stream at STOPPED
    int stopped_polls_ = 0;        // consecutive polls that found the unpaused stream STOPPED / OPEN
    uint32_t reopen_failures_ = 0;
    std::chrono::steady_clock::time_point fail_log_at_{};
    bool spatial_ = false;
    uint64_t trimmed_base_ = 0;    // the trimmed frames of the bounds reset_input replaced
    std::chrono::steady_clock::time_point retry_at_{};
    struct ErrorFlag { std::atomic<bool> set{false}; };
    ErrorFlag *err_;               // the error callback's user pointer: allocated once, deliberately never freed
    std::atomic<uint64_t> frames_{0}, missing_{0};

    // Spatial. feed_ is replaced only by reset_input; positions_, tuning_, head_ and plan_back_ are the render thread's;
    // prev_, prev_id_, key_, plan_front_ and the scratch buffers are the callback's.
    struct PlanSlot { PannerLane lane[AR_AUDIO_MAX_SOURCES]; uint32_t id[AR_AUDIO_MAX_SOURCES]; };
    static constexpr uint8_t kPlanFresh = 4;   // in plan_mid_: the middle slot holds a plan the callback has not taken
    std::unique_ptr<SceneFeed> feed_;
    ScenePositions positions_;
    ar_audio_tuning tuning_{};
    bool polyphase_ = true;
    PolyMode poly_mode_ = PolyMode::sinc;
    ar_pose head_ = ar_pose_identity();
    PlanSlot plans_[3]{};
    std::atomic<uint8_t> plan_mid_{1};
    uint8_t plan_back_ = 0, plan_front_ = 2;
    PannerLane prev_[AR_AUDIO_MAX_SOURCES]{};
    uint32_t prev_id_[AR_AUDIO_MAX_SOURCES]{};
    uint64_t key_ = 0;
    std::vector<float> planar_, inter_;   // kSceneChannels x kMaxChunk: the feed's channels, then interleaved
    uint64_t feed_base_[4] = {0, 0, 0, 0};   // missing, trimmed, busy, stale of the feeds reset_input replaced
};

}
