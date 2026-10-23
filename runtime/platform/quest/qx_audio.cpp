// runtime/platform/quest/qx_audio.cpp — vos_audio.mm's stereo path on AAudio, and the spatial mode's scene panned to stereo.
#include "qx_audio.h"
#include <algorithm>
#include <cstring>
extern "C" {
#include "ar_audio_types.h"
#include "ar_math.h"
}
#include "vos_log.h"

namespace ar {
namespace {
constexpr int32_t kOutHz = 48000;
constexpr int kStopWaitMs = 50;                          // close_stream's bound on the wait for STOPPED
constexpr uint32_t kDrainBlock = 1024;
constexpr uint32_t kDrainMaxBlocks = 64;                 // 65536 frames, beyond any ring the runtime keeps
}  // namespace

QxAudio::QxAudio(ar_runtime *rt) : rt_(rt), drain_(2 * kDrainBlock, 0), err_(new ErrorFlag) {}

QxAudio::~QxAudio() { stop(); }

uint32_t QxAudio::pull_game(void *user, int16_t *stereo, uint32_t frames) {
    return ar_runtime_pull_audio(static_cast<QxAudio *>(user)->rt_, stereo, frames);
}

uint32_t QxAudio::pull_scene(void *user, ar_audio_pull *p) {
    return ar_runtime_pull_audio_scene(static_cast<QxAudio *>(user)->rt_, p);
}

// The callback's spatial path. The newest plan is taken without waiting (the triple buffer's exchange); a lane whose
// emitter changed (another nonzero id) starts at the new plan, as SceneFeed's snap; any other lane ramps from where the
// previous chunk ended. Each chunk is one SceneFeed cycle: every channel rendered under one key (the first call renders
// the cycle, the rest copy it), interleaved, then panned. The feed counts what it could not cover as missing.
void QxAudio::render_spatial(float *out, uint32_t total) {
    if (plan_mid_.load(std::memory_order_acquire) & kPlanFresh)
        plan_front_ = plan_mid_.exchange(plan_front_, std::memory_order_acq_rel) & 3u;
    const PlanSlot &p = plans_[plan_front_];
    for (uint32_t l = 0; l < AR_AUDIO_MAX_SOURCES; l++) {
        if (p.id[l] == prev_id_[l]) continue;
        if (prev_id_[l] != 0 && p.id[l] != 0) prev_[l] = p.lane[l];
        prev_id_[l] = p.id[l];
    }
    for (uint32_t done = 0; done < total;) {
        const uint32_t n = std::min<uint32_t>(total - done, kMaxChunk);
        const uint64_t key = key_++;
        for (uint32_t c = 0; c < kSceneChannels; c++) feed_->render(key, n, c, &planar_[(size_t)c * n]);
        for (uint32_t c = 0; c < kSceneChannels; c++) {
            const float *src = &planar_[(size_t)c * n];
            float *dst = &inter_[c];
            for (uint32_t f = 0; f < n; f++) dst[(size_t)f * kSceneChannels] = src[f];
        }
        qx_panner_mix(inter_.data(), n, prev_, p.lane, 1.f, out + 2 * (size_t)done);   // the bed's gain is the feed's
        std::memcpy(prev_, p.lane, sizeof prev_);
        done += n;
    }
}

// AAudio's real-time thread: no allocation, no lock, no logging. The pull ends in ar_audio_ring_read, which returns 0
// frames at once while the game thread is writing; what the bound's queue cannot cover plays as silence and is counted
// as missing, exactly as vos_audio.mm's render_cb.
aaudio_data_callback_result_t QxAudio::on_data(AAudioStream *, void *user, void *data, int32_t frames) {
    auto *a = static_cast<QxAudio *>(user);
    float *out = static_cast<float *>(data);
    const uint32_t total = frames > 0 ? (uint32_t)frames : 0;
    if (a->spatial_) {   // set only while no stream is open
        a->render_spatial(out, total);
        a->frames_.fetch_add(total, std::memory_order_relaxed);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint64_t missing = 0;
    for (uint32_t done = 0; done < total;) {
        const uint32_t n = std::min<uint32_t>(total - done, kMaxChunk);
        missing += a->rs_->render(out + 2 * (size_t)done, n, AudioLatencyBound::pull, a->bound_.get());
        done += n;
    }
    if (missing) a->missing_.fetch_add(missing, std::memory_order_relaxed);
    a->frames_.fetch_add(total, std::memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// Any thread AAudio picks (on some paths a thread of its own that close does not join); the stream must not be closed
// here: poll() reopens it on the render thread. user is the ErrorFlag, which is never freed.
void QxAudio::on_error(AAudioStream *, void *user, aaudio_result_t) {
    static_cast<ErrorFlag *>(user)->set.store(true, std::memory_order_release);
}

void QxAudio::reset_input() {
    if (bound_) trimmed_base_ += bound_->trimmed();
    if (feed_) {
        feed_base_[0] += feed_->missing();
        feed_base_[1] += feed_->trimmed();
        feed_base_[2] += feed_->busy();
        feed_base_[3] += feed_->stale();
    }
    if (spatial_) {
        rs_.reset();
        bound_.reset();
        SceneFeed::Config c;   // queue, standing and window: vos_latency.h's, as the stereo bound
        c.in_hz = AR_AUDIO_RATE_HZ;
        c.out_hz = kOutHz;
        c.max_block = kMaxChunk;
        c.polyphase = polyphase_;
        feed_.reset(new SceneFeed(c, pull_scene, this));
        feed_->set_poly_mode(poly_mode_);
        feed_->set_bed_gain(ar_db_to_gain(tuning_.music_db));
        positions_ = ScenePositions();
        for (PlanSlot &p : plans_) p = PlanSlot{};
        plan_mid_.store(1, std::memory_order_relaxed);
        plan_back_ = 0;
        plan_front_ = 2;
        std::memset(prev_, 0, sizeof prev_);
        std::memset(prev_id_, 0, sizeof prev_id_);
        key_ = 0;
        planar_.assign((size_t)kSceneChannels * kMaxChunk, 0.f);
        inter_.assign((size_t)kSceneChannels * kMaxChunk, 0.f);
    } else {
        feed_.reset();
        std::vector<float>().swap(planar_);
        std::vector<float>().swap(inter_);
        rs_.reset(new StereoResampler(AR_AUDIO_RATE_HZ, kOutHz, kMaxChunk));
        bound_.reset(new AudioLatencyBound(kAudioQueueFrames, kAudioStandingFrames, kAudioWindowFrames, pull_game, this));
    }
    // The game kept writing while nothing pulled: drop it (the runtime's ring has one consumer, and no callback runs).
    for (uint32_t i = 0; i < kDrainMaxBlocks && ar_runtime_pull_audio(rt_, drain_.data(), kDrainBlock) == kDrainBlock; i++) {}
}

bool QxAudio::open_stream(std::string *err) {
    AAudioStreamBuilder *b = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) { *err = std::string("AAudio_createStreamBuilder: ") + AAudio_convertResultToText(r); return false; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, kOutHz);
    AAudioStreamBuilder_setDataCallback(b, on_data, this);
    AAudioStreamBuilder_setErrorCallback(b, on_error, err_);
    AAudioStream *s = nullptr;
    err_->set.store(false, std::memory_order_relaxed);   // the previous stream is closed: its flag is stale
    r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !s) { *err = std::string("AAudioStreamBuilder_openStream: ") + AAudio_convertResultToText(r); return false; }
    // The callback writes exactly this format: refuse anything else rather than play noise.
    if (AAudioStream_getFormat(s) != AAUDIO_FORMAT_PCM_FLOAT || AAudioStream_getChannelCount(s) != 2 ||
        AAudioStream_getSampleRate(s) != kOutHz) {
        *err = "AAudio stream format " + std::to_string((int)AAudioStream_getFormat(s)) + " channels " +
               std::to_string(AAudioStream_getChannelCount(s)) + " rate " + std::to_string(AAudioStream_getSampleRate(s));
        AAudioStream_close(s);
        return false;
    }
    stream_ = s;
    return true;
}

// The restart and stop paths only (pause never waits): STOPPED within 50 ms, else logged; close joins the callback anyway.
void QxAudio::close_stream() {
    if (!stream_) return;
    aaudio_stream_state_t st = AAudioStream_getState(stream_);
    if (st == AAUDIO_STREAM_STATE_STARTING || st == AAUDIO_STREAM_STATE_STARTED) {
        AAudioStream_requestStop(stream_);
        st = AAudioStream_getState(stream_);
    }
    if (st == AAUDIO_STREAM_STATE_STOPPING || st == AAUDIO_STREAM_STATE_STARTING || st == AAUDIO_STREAM_STATE_STARTED) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopWaitMs);
        while (st != AAUDIO_STREAM_STATE_STOPPED && st != AAUDIO_STREAM_STATE_DISCONNECTED) {
            const int64_t left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) { vos_log("AR_AUDIO stop wait expired state=%d", (int)st); break; }
            aaudio_stream_state_t next = st;
            AAudioStream_waitForStateChange(stream_, st, &next, left);
            st = next;
        }
    }
    // AAudioStream_close stops the stream and joins its callback thread: after it, no callback runs.
    AAudioStream_close(stream_);
    stream_ = nullptr;
}

bool QxAudio::start(bool spatial, std::string *err) {
    if (started_) { *err = "audio already started"; return false; }
    spatial_ = spatial;   // no stream is open: the callback reads it only from the stream opened below on
    reset_input();
    if (!open_stream(err)) return false;
    const aaudio_result_t r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
        *err = std::string("AAudioStream_requestStart: ") + AAudio_convertResultToText(r);
        close_stream();
        return false;
    }
    started_ = true;
    paused_ = start_pending_ = false;
    vos_log("AR_AUDIO backend=aaudio mode=%s rate=%d burst=%d%s", spatial_ ? "spatial" : "stereo",
            (int)AAudioStream_getSampleRate(stream_), (int)AAudioStream_getFramesPerBurst(stream_),
            spatial_ ? (polyphase_ ? (poly_mode_ == PolyMode::linear ? " resampler=linear" : " resampler=sinc") : " resampler=feed") : "");
    return true;
}

// Never waits: the stop is requested and the callback winds down on its own; resume() starts again once the state has
// left STOPPING (poll() checks each frame).
void QxAudio::pause() {
    if (!started_ || paused_) return;
    paused_ = true;
    start_pending_ = false;
    if (!stream_) return;
    const aaudio_stream_state_t st = AAudioStream_getState(stream_);
    if (st != AAUDIO_STREAM_STATE_STARTING && st != AAUDIO_STREAM_STATE_STARTED) return;
    const aaudio_result_t r = AAudioStream_requestStop(stream_);
    if (r != AAUDIO_OK) vos_log("AR_ERROR audio: AAudioStream_requestStop on pause: %s", AAudio_convertResultToText(r));
}

// No buffer is swapped and nothing drained here (a stopped stream's callback may not have returned yet): the latency
// bound's first pull drains the game's ring and trims to the standing 800 frames, which is the burst protection.
void QxAudio::resume() {
    if (!started_ || !paused_) return;
    paused_ = false;
    start_pending_ = true;
    poll();
}

void QxAudio::poll() {
    if (!started_) return;
    const bool flagged = err_->set.exchange(false, std::memory_order_acq_rel);
    if (stream_ && (flagged || AAudioStream_getState(stream_) == AAUDIO_STREAM_STATE_DISCONNECTED)) {
        close_stream();
        retry_at_ = {};   // a live stream was lost: reopen at once
    }
    if (paused_) return;
    if (stream_) {
        const aaudio_stream_state_t st = AAudioStream_getState(stream_);
        if (st == AAUDIO_STREAM_STATE_STOPPING) return;   // still winding down: next frame
        if (st != AAUDIO_STREAM_STATE_STOPPED && st != AAUDIO_STREAM_STATE_OPEN) {
            start_pending_ = false;   // starting or playing (a stop that never took)
            stopped_polls_ = 0;
            return;
        }
        if (!start_pending_ && ++stopped_polls_ < 2) return;   // a lost resume: start it on the second poll
        start_pending_ = false;
        stopped_polls_ = 0;
        const aaudio_result_t r = AAudioStream_requestStart(stream_);
        if (r == AAUDIO_OK) return;
        vos_log("AR_ERROR audio: AAudioStream_requestStart on resume: %s", AAudio_convertResultToText(r));
        close_stream();
        retry_at_ = {};
    }
    start_pending_ = false;
    stopped_polls_ = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_at_) return;
    retry_at_ = now + std::chrono::seconds(1);   // a failed open retries once a second
    reset_input();   // no stream: no callback
    std::string err;
    aaudio_result_t r = AAUDIO_OK;
    if (open_stream(&err)) {
        r = AAudioStream_requestStart(stream_);
        if (r != AAUDIO_OK) {
            err = std::string("AAudioStream_requestStart: ") + AAudio_convertResultToText(r);
            close_stream();
        }
    }
    if (!stream_) {
        if (reopen_failures_++ == 0 || now >= fail_log_at_) {
            vos_log("AR_ERROR audio: reopening (failures=%u, retrying each second): %s", reopen_failures_, err.c_str());
            fail_log_at_ = now + std::chrono::seconds(10);
        }
        return;
    }
    vos_log("AR_AUDIO restart burst=%d failures=%u", (int)AAudioStream_getFramesPerBurst(stream_), reopen_failures_);
    reopen_failures_ = 0;
}

void QxAudio::stop() {
    close_stream();
    started_ = paused_ = start_pending_ = false;
    stopped_polls_ = 0;
    reopen_failures_ = 0;
    err_->set.store(false, std::memory_order_relaxed);
}

bool QxAudio::running() const { return started_ && !paused_ && stream_ != nullptr; }

uint64_t QxAudio::missing_in() const {
    return missing_.load(std::memory_order_relaxed) + feed_base_[0] + (feed_ ? feed_->missing() : 0);
}
uint64_t QxAudio::trimmed_in() const {
    return trimmed_base_ + (bound_ ? bound_->trimmed() : 0) + feed_base_[1] + (feed_ ? feed_->trimmed() : 0);
}
uint64_t QxAudio::feed_busy() const { return feed_base_[2] + (feed_ ? feed_->busy() : 0); }
uint64_t QxAudio::feed_stale() const { return feed_base_[3] + (feed_ ? feed_->stale() : 0); }

void QxAudio::set_tuning(const ar_audio_tuning &t) {
    tuning_ = t;
    ar_audio_tuning_clamp(&tuning_);
    if (feed_) feed_->set_bed_gain(ar_db_to_gain(tuning_.music_db));   // atomic: the next cycle ramps to it
}

void QxAudio::set_poly_mode(PolyMode m) {
    poly_mode_ = m;
    if (feed_) feed_->set_poly_mode(m);   // atomic
}

void QxAudio::set_feed_choice(const FeedChoice &fc) {
    polyphase_ = fc.polyphase;
    set_poly_mode(fc.mode);
}

// The render thread's half of the spatial mode (see qx_audio.h). The scene's listener is the head in table space the
// adapter recorded with the tick being heard; an adapter that records none (a zero quaternion), or no metadata yet, falls
// back to this frame's head. No metadata yet: every lane silent.
void QxAudio::listener(const ar_pose &tracking_from_head, const ar_pose &tracking_from_table) {
    head_ = ar_pose_mul(ar_pose_inverse(tracking_from_table), tracking_from_head);
    if (!spatial_ || !feed_) return;
    ar_audio_tick_meta m;
    while (feed_->pop_meta(&m)) positions_.push(m);
    ar_audio_source lanes[AR_AUDIO_MAX_SOURCES];
    ar_pose lis{};
    const bool have = positions_.at(feed_->heard_frame(), lanes, &lis);
    if (!have) std::memset(lanes, 0, sizeof lanes);
    if (!have || (lis.quat[0] == 0.f && lis.quat[1] == 0.f && lis.quat[2] == 0.f && lis.quat[3] == 0.f)) lis = head_;
    PlanSlot &s = plans_[plan_back_];
    qx_panner_plan(tuning_, lis, lanes, s.lane);
    for (uint32_t l = 0; l < AR_AUDIO_MAX_SOURCES; l++) s.id[l] = s.lane[l].active ? lanes[l].id : 0;
    plan_back_ = plan_mid_.exchange((uint8_t)(plan_back_ | kPlanFresh), std::memory_order_acq_rel) & 3u;
}

}  // namespace ar
