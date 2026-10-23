// The Quest's spatial mode: SceneFeed's 32 mono lanes panned into the stereo stream with constant power, where visionOS
// hands them to PHASE. Pure functions (host-tested); QxAudio plans on the render thread and mixes in its data callback.
#pragma once
#include <stdint.h>
extern "C" {
#include "ar_audio_tuning.h"
#include "ar_audio_types.h"
#include "ar_types.h"
}
#include "vos_scene_audio.h"

namespace ar {

struct PannerLane { bool active; float gain; float azimuth_deg; };   // azimuth from listener_direction after ar_widen_azimuth_deg
// Pure. From the lanes' sources (positions in table metres) and the listener (table space): per-lane gain via
// ar_audio_lane_gain (the tuning clamped first) times the base distance model PHASE applied on visionOS (the app's
// curve, ar_audio_distance_gain; 1 for AR_SRC_NO_DISTANCE_LOSS lanes) and the azimuth of the position it returns, which ar_audio_widen already
// widened (ar_widen_azimuth_deg): widening it again would double the tuning's widen. A lane with a non-finite position,
// an inactive source or a non-finite gain is {false, 0, 0}; a non-finite listener makes every lane so.
void qx_panner_plan(const ar_audio_tuning &t, const ar_pose &listener, const ar_audio_source src[AR_AUDIO_MAX_SOURCES],
                    PannerLane out[AR_AUDIO_MAX_SOURCES]);
// Pure. Constant-power stereo: left = cos(theta), right = sin(theta), theta = (azimuth clamped to [-90, 90] + 90) * pi / 360.
void qx_pan_gains(float azimuth_deg, float *left, float *right);
// Pure. Mixes one SceneFeed cycle: in = kSceneChannels interleaved floats (bed L, bed R, then 32 lanes), out = stereo
// interleaved. Each lane's left and right gains ramp linearly from prev's to plan's over the block, reaching plan's on the
// last frame (no zipper noise; the next block starts where this one ended). The bed passes through scaled by bed_gain.
void qx_panner_mix(const float *in, uint32_t frames, const PannerLane prev[AR_AUDIO_MAX_SOURCES],
                   const PannerLane plan[AR_AUDIO_MAX_SOURCES], float bed_gain, float *out);

}
