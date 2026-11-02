// The AAudio slice of desktop/aaudio/AAudio.h over PulseAudio's simple API (PipeWire serves it too). One worker thread
// per stream, alive from open to close: while STARTED it calls the data callback for one burst and writes it with
// pa_simple_write, which blocks once the server holds kBufferedBursts bursts and so paces the callback like a device.
#include "aaudio/AAudio.h"
#include <pulse/error.h>
#include <pulse/simple.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr int32_t kBurstFrames = 480;     // 10 ms at 48 kHz
constexpr uint32_t kBufferedBursts = 4;   // the server-side target: about 40 ms
}  // namespace

struct AAudioStreamBuilderStruct {
    int32_t channels = 2, rate = 48000;
    aaudio_format_t format = AAUDIO_FORMAT_PCM_FLOAT;
    AAudioStream_dataCallback data = nullptr;
    void *data_user = nullptr;
    AAudioStream_errorCallback error = nullptr;
    void *error_user = nullptr;
};

struct AAudioStreamStruct {
    AAudioStreamBuilderStruct cfg;
    pa_simple *pa = nullptr;
    std::mutex m;
    std::condition_variable cv;
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_OPEN;
    bool quit = false;
    std::thread worker;

    void set_state(aaudio_stream_state_t s) {   // m held
        state = s;
        cv.notify_all();
    }
    void run() {
        std::vector<float> buf((size_t)kBurstFrames * (size_t)cfg.channels);
        for (;;) {
            {
                std::unique_lock<std::mutex> l(m);
                if (state == AAUDIO_STREAM_STATE_STOPPING) set_state(AAUDIO_STREAM_STATE_STOPPED);
                if (state == AAUDIO_STREAM_STATE_STARTING) set_state(AAUDIO_STREAM_STATE_STARTED);
                cv.wait(l, [&] { return quit || state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_STARTING ||
                                        state == AAUDIO_STREAM_STATE_STOPPING; });
                if (quit) return;
                if (state != AAUDIO_STREAM_STATE_STARTED) continue;
            }
            const aaudio_data_callback_result_t r = cfg.data(this, cfg.data_user, buf.data(), kBurstFrames);
            int pa_err = 0;
            if (pa_simple_write(pa, buf.data(), buf.size() * sizeof(float), &pa_err) < 0) {
                {
                    std::lock_guard<std::mutex> l(m);
                    set_state(AAUDIO_STREAM_STATE_DISCONNECTED);
                }
                if (cfg.error) cfg.error(this, cfg.error_user, AAUDIO_ERROR_DISCONNECTED);
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return quit; });
                return;
            }
            if (r == AAUDIO_CALLBACK_RESULT_STOP) {
                std::lock_guard<std::mutex> l(m);
                if (state == AAUDIO_STREAM_STATE_STARTED) set_state(AAUDIO_STREAM_STATE_STOPPING);
            }
        }
    }
};

extern "C" {

const char *AAudio_convertResultToText(aaudio_result_t r) {
    switch (r) {
    case AAUDIO_OK: return "AAUDIO_OK";
    case AAUDIO_ERROR_DISCONNECTED: return "AAUDIO_ERROR_DISCONNECTED";
    case AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
    case AAUDIO_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE (no PulseAudio / PipeWire server)";
    case AAUDIO_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
    default: return "AAUDIO_ERROR";
    }
}

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **out) {
    *out = new AAudioStreamBuilderStruct();
    return AAUDIO_OK;
}
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *, aaudio_direction_t) {}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *, aaudio_performance_mode_t) {}
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *, aaudio_sharing_mode_t) {}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t f) { b->format = f; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t n) { b->channels = n; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t hz) { b->rate = hz; }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *user) {
    b->data = cb;
    b->data_user = user;
}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *user) {
    b->error = cb;
    b->error_user = user;
}

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out) {
    *out = nullptr;
    if (!b->data || b->format != AAUDIO_FORMAT_PCM_FLOAT || b->channels <= 0 || b->rate <= 0) return AAUDIO_ERROR_INVALID_STATE;
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32NE;
    ss.channels = (uint8_t)b->channels;
    ss.rate = (uint32_t)b->rate;
    const uint32_t burst_bytes = (uint32_t)(kBurstFrames * b->channels * (int32_t)sizeof(float));
    pa_buffer_attr ba;
    ba.maxlength = (uint32_t)-1;
    ba.tlength = burst_bytes * kBufferedBursts;
    ba.prebuf = (uint32_t)-1;
    ba.minreq = burst_bytes;
    ba.fragsize = (uint32_t)-1;
    int pa_err = 0;
    pa_simple *pa = pa_simple_new(nullptr, "Diorama64", PA_STREAM_PLAYBACK, nullptr, "game", &ss, nullptr, &ba, &pa_err);
    if (!pa) return AAUDIO_ERROR_UNAVAILABLE;
    AAudioStream *s = new AAudioStreamStruct();
    s->cfg = *b;
    s->pa = pa;
    s->worker = std::thread([s] { s->run(); });
    *out = s;
    return AAUDIO_OK;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) {
    delete b;
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestStart(AAudioStream *s) {
    std::lock_guard<std::mutex> l(s->m);
    if (s->state == AAUDIO_STREAM_STATE_DISCONNECTED) return AAUDIO_ERROR_DISCONNECTED;
    if (s->state != AAUDIO_STREAM_STATE_OPEN && s->state != AAUDIO_STREAM_STATE_STOPPED) return AAUDIO_ERROR_INVALID_STATE;
    s->set_state(AAUDIO_STREAM_STATE_STARTING);
    return AAUDIO_OK;
}
aaudio_result_t AAudioStream_requestStop(AAudioStream *s) {
    std::lock_guard<std::mutex> l(s->m);
    if (s->state == AAUDIO_STREAM_STATE_STARTING || s->state == AAUDIO_STREAM_STATE_STARTED) s->set_state(AAUDIO_STREAM_STATE_STOPPING);
    return AAUDIO_OK;
}
aaudio_result_t AAudioStream_close(AAudioStream *s) {
    {
        std::lock_guard<std::mutex> l(s->m);
        s->quit = true;
        s->cv.notify_all();
    }
    if (s->worker.joinable()) s->worker.join();   // no callback runs after close returns
    pa_simple_free(s->pa);
    delete s;
    return AAUDIO_OK;
}
aaudio_stream_state_t AAudioStream_getState(AAudioStream *s) {
    std::lock_guard<std::mutex> l(s->m);
    return s->state;
}
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s, aaudio_stream_state_t input, aaudio_stream_state_t *next,
                                                int64_t timeout_ns) {
    std::unique_lock<std::mutex> l(s->m);
    const bool changed = s->cv.wait_for(l, std::chrono::nanoseconds(timeout_ns > 0 ? timeout_ns : 0), [&] { return s->state != input; });
    if (next) *next = s->state;
    return changed ? AAUDIO_OK : AAUDIO_ERROR_TIMEOUT;
}
aaudio_format_t AAudioStream_getFormat(AAudioStream *s) { return s->cfg.format; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s->cfg.channels; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s->cfg.rate; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *) { return kBurstFrames; }

}  // extern "C"
