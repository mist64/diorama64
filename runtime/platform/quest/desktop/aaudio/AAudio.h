// The desktop build's stand-in for <aaudio/AAudio.h>: exactly the slice of the NDK's AAudio API that qx_audio.cpp
// uses, implemented over PulseAudio (aaudio_pulse.cpp), so QxAudio runs unchanged on Linux. Output only, float, with
// a data callback driven by a worker thread that writes each burst with pa_simple_write (which paces it).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_stream_state_t;
typedef int32_t aaudio_data_callback_result_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_sharing_mode_t;
enum { AAUDIO_OK = 0, AAUDIO_ERROR_BASE = -900, AAUDIO_ERROR_DISCONNECTED = -899, AAUDIO_ERROR_INVALID_STATE = -895,
       AAUDIO_ERROR_UNAVAILABLE = -889, AAUDIO_ERROR_TIMEOUT = -885 };
enum { AAUDIO_STREAM_STATE_UNINITIALIZED = 0, AAUDIO_STREAM_STATE_UNKNOWN, AAUDIO_STREAM_STATE_OPEN,
       AAUDIO_STREAM_STATE_STARTING, AAUDIO_STREAM_STATE_STARTED, AAUDIO_STREAM_STATE_PAUSING, AAUDIO_STREAM_STATE_PAUSED,
       AAUDIO_STREAM_STATE_FLUSHING, AAUDIO_STREAM_STATE_FLUSHED, AAUDIO_STREAM_STATE_STOPPING, AAUDIO_STREAM_STATE_STOPPED,
       AAUDIO_STREAM_STATE_CLOSING, AAUDIO_STREAM_STATE_CLOSED, AAUDIO_STREAM_STATE_DISCONNECTED };
enum { AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP };
enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_DIRECTION_INPUT };
enum { AAUDIO_FORMAT_INVALID = -1, AAUDIO_FORMAT_UNSPECIFIED = 0, AAUDIO_FORMAT_PCM_I16, AAUDIO_FORMAT_PCM_FLOAT };
enum { AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_POWER_SAVING, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY };
enum { AAUDIO_SHARING_MODE_EXCLUSIVE = 0, AAUDIO_SHARING_MODE_SHARED };

typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream *stream, void *user, void *audio, int32_t frames);
typedef void (*AAudioStream_errorCallback)(AAudioStream *stream, void *user, aaudio_result_t error);

const char *AAudio_convertResultToText(aaudio_result_t r);
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **out);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t d);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t m);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t m);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t f);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t n);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t hz);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *user);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *user);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b);
aaudio_result_t AAudioStream_requestStart(AAudioStream *s);
aaudio_result_t AAudioStream_requestStop(AAudioStream *s);
aaudio_result_t AAudioStream_close(AAudioStream *s);
aaudio_stream_state_t AAudioStream_getState(AAudioStream *s);
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s, aaudio_stream_state_t input, aaudio_stream_state_t *next, int64_t timeout_ns);
aaudio_format_t AAudioStream_getFormat(AAudioStream *s);
int32_t AAudioStream_getChannelCount(AAudioStream *s);
int32_t AAudioStream_getSampleRate(AAudioStream *s);
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s);
#ifdef __cplusplus
}
#endif
