// runtime/platform/quest/qx_audio.cpp — vos_audio.mm's stereo path on AAudio.
#include "qx_audio.h"
#include <algorithm>
#include <cstring>
#include "ar_audio_types.h"
#include "vos_log.h"

namespace ar {
namespace {
constexpr int32_t kOutHz = 48000;
constexpr int kStopWaitMs = 50;                          // close_stream's bound on the wait for STOPPED
constexpr uint32_t kDrainBlock = 1024;
constexpr uint32_t kDrainMaxBlocks = 64;                 // 65536 frames, beyond any ring the runtime keeps
}  // namespace

QxAudio::QxAudio(ar_runtime *rt) : rt_(rt), drain_(2 * kDrainBlock, 0) {}

QxAudio::~QxAudio() { stop(); }

uint32_t QxAudio::pull_game(void *user, int16_t *stereo, uint32_t frames) {
    return ar_runtime_pull_audio(static_cast<QxAudio *>(user)->rt_, stereo, frames);
}

// AAudio's real-time thread: no allocation, no lock, no logging. The pull ends in ar_audio_ring_read, which returns 0
// frames at once while the game thread is writing; what the bound's queue cannot cover plays as silence and is counted
// as missing, exactly as vos_audio.mm's render_cb.
aaudio_data_callback_result_t QxAudio::on_data(AAudioStream *, void *user, void *data, int32_t frames) {
    auto *a = static_cast<QxAudio *>(user);
    float *out = static_cast<float *>(data);
    const uint32_t total = frames > 0 ? (uint32_t)frames : 0;
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

// Any thread AAudio picks; the stream must not be closed here: poll() reopens it on the render thread.
void QxAudio::on_error(AAudioStream *, void *user, aaudio_result_t) {
    static_cast<QxAudio *>(user)->restart_needed_.store(true, std::memory_order_release);
}

void QxAudio::reset_input() {
    if (bound_) trimmed_base_ += bound_->trimmed();
    rs_.reset(new StereoResampler(AR_AUDIO_RATE_HZ, kOutHz, kMaxChunk));
    bound_.reset(new AudioLatencyBound(kAudioQueueFrames, kAudioStandingFrames, kAudioWindowFrames, pull_game, this));
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
    AAudioStreamBuilder_setErrorCallback(b, on_error, this);
    AAudioStream *s = nullptr;
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
    restart_needed_.store(false, std::memory_order_relaxed);
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
    if (spatial) { *err = "spatial audio: Task 2"; return false; }
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
    spatial_ = false;
    vos_log("AR_AUDIO backend=aaudio rate=%d burst=%d", (int)AAudioStream_getSampleRate(stream_),
            (int)AAudioStream_getFramesPerBurst(stream_));
    return true;
}

// Never waits: the stop is requested and the callback winds down on its own; resume() starts again only once AAudio
// reports STOPPED (poll() checks each frame).
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

// The burst protection: the bound and resampler are fresh and the game audio queued while paused is dropped
// (reset_input), done in poll() once no callback can run.
void QxAudio::resume() {
    if (!started_ || !paused_) return;
    paused_ = false;
    start_pending_ = true;
    poll();
}

void QxAudio::poll() {
    if (!started_) return;
    const bool disconnected = restart_needed_.load(std::memory_order_acquire) ||
                              (stream_ && AAudioStream_getState(stream_) == AAUDIO_STREAM_STATE_DISCONNECTED);
    if (disconnected) {
        close_stream();
        retry_at_ = {};
    }
    if (paused_) return;
    if (stream_) {
        if (!start_pending_) return;
        const aaudio_stream_state_t st = AAudioStream_getState(stream_);
        if (st == AAUDIO_STREAM_STATE_STOPPING) return;   // still winding down: next frame
        start_pending_ = false;
        if (st == AAUDIO_STREAM_STATE_STARTING || st == AAUDIO_STREAM_STATE_STARTED) return;   // the stop never took: playing
        reset_input();   // STOPPED (or OPEN): no callback runs
        const aaudio_result_t r = AAudioStream_requestStart(stream_);
        if (r == AAUDIO_OK) return;
        vos_log("AR_ERROR audio: AAudioStream_requestStart on resume: %s", AAudio_convertResultToText(r));
        close_stream();
        retry_at_ = {};
    }
    start_pending_ = false;
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_at_) return;
    retry_at_ = now + std::chrono::seconds(1);
    reset_input();   // no stream: no callback
    std::string err;
    if (!open_stream(&err)) { vos_log("AR_ERROR audio: reopening: %s", err.c_str()); return; }
    const aaudio_result_t r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
        vos_log("AR_ERROR audio: AAudioStream_requestStart on reopen: %s", AAudio_convertResultToText(r));
        close_stream();
        return;
    }
    vos_log("AR_AUDIO restart burst=%d", (int)AAudioStream_getFramesPerBurst(stream_));
}

void QxAudio::stop() {
    close_stream();
    started_ = paused_ = start_pending_ = false;
    restart_needed_.store(false, std::memory_order_relaxed);
}

bool QxAudio::running() const { return started_ && !paused_ && stream_ != nullptr; }

uint64_t QxAudio::trimmed_in() const { return trimmed_base_ + (bound_ ? bound_->trimmed() : 0); }

void QxAudio::set_tuning(const ar_audio_tuning &) {}
void QxAudio::set_poly_mode(PolyMode) {}
void QxAudio::listener(const ar_pose &, const ar_pose &) {}

}  // namespace ar
