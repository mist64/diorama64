// The Quest's game audio on AAudio, the counterpart of vos_audio.mm: a low-latency shared float stereo stream at 48 kHz
// whose data callback pulls the game's 32 kHz int16 stereo through AudioLatencyBound (vos_latency.h) and StereoResampler
// (vos_resample.h). Spatial output (start(true, ...), set_tuning, set_poly_mode, listener) is plan 3's Task 2.
//
// Threads: every member is for the render thread (QxSession's) except the counters (any thread). The data callback runs
// on AAudio's real-time thread and touches only rt_, the resampler, the latency bound (both swapped only while no
// stream is started) and the atomics; the error callback only sets restart_needed_, which poll() acts on.
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
#include "ar_runtime.h"
#include "ar_types.h"
}
#include "vos_latency.h"
#include "vos_polyphase.h"
#include "vos_resample.h"

namespace ar {

class QxAudio {
public:
    explicit QxAudio(ar_runtime *rt);
    ~QxAudio();
    QxAudio(const QxAudio &) = delete;
    QxAudio &operator=(const QxAudio &) = delete;
    // AAudio: PERFORMANCE_MODE_LOW_LATENCY, SHARING_MODE_SHARED, float, 2 channels, 48000 Hz, data + error callbacks;
    // started (playing) on success. spatial=true: Task 2 (fails for now).
    bool start(bool spatial, std::string *err);
    // Session VISIBLE / FOCUSED transitions. pause() returns once the callback has stopped; resume() drops the game
    // audio queued meanwhile, so playback resumes at the game's present without a burst.
    void pause();
    void resume();
    // Stops and closes the stream: no callback runs after it returns. Safe to call twice and without start.
    void stop();
    // Each frame: a disconnected stream (the error callback's flag, or its state) is closed and reopened, the queued
    // input dropped; a failed reopen is retried once a second.
    void poll();
    bool running() const;   // a stream is open and not paused
    bool spatial() const { return spatial_; }
    uint64_t frames_out() const { return frames_.load(std::memory_order_relaxed); }
    uint64_t missing_in() const { return missing_.load(std::memory_order_relaxed); }
    uint64_t trimmed_in() const;
    void set_tuning(const ar_audio_tuning &t);   // spatial only (Task 2): a no-op for now
    void set_poly_mode(PolyMode m);              // spatial only (Task 2): a no-op for now
    void listener(const ar_pose &tracking_from_head, const ar_pose &tracking_from_table);   // render thread, each frame

private:
    static constexpr uint32_t kMaxChunk = 4096;   // the resampler's max_out_frames: larger requests render in chunks
    static uint32_t pull_game(void *user, int16_t *stereo, uint32_t frames);
    static aaudio_data_callback_result_t on_data(AAudioStream *s, void *user, void *data, int32_t frames);
    static void on_error(AAudioStream *s, void *user, aaudio_result_t error);
    bool open_stream(std::string *err);
    void close_stream();
    bool stop_stream();     // requestStop, then wait (bounded) until STOPPED; true: the callback is no longer running
    void reset_input();     // only while no callback runs: a fresh bound and resampler, the runtime's queued audio dropped

    ar_runtime *rt_;
    std::unique_ptr<StereoResampler> rs_;
    std::unique_ptr<AudioLatencyBound> bound_;
    std::vector<int16_t> drain_;   // reset_input's scratch
    AAudioStream *stream_ = nullptr;
    bool started_ = false;         // start() succeeded and stop() has not run (the stream may be reopening)
    bool paused_ = false;
    bool spatial_ = false;
    uint64_t trimmed_base_ = 0;    // the trimmed frames of the bounds reset_input replaced
    std::chrono::steady_clock::time_point retry_at_{};
    std::atomic<bool> restart_needed_{false};
    std::atomic<uint64_t> frames_{0}, missing_{0};
};

}
