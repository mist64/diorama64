// runtime/platform/quest/qx_panner.cpp — the constant-power panner of the Quest's spatial mode.
#include "qx_panner.h"
#include <cmath>
#include "vos_scene_pos.h"

namespace ar {
namespace {
bool finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
bool finite_pose(const ar_pose &p) {
    return finite3(p.pos) && std::isfinite(p.quat[0]) && std::isfinite(p.quat[1]) && std::isfinite(p.quat[2]) && std::isfinite(p.quat[3]);
}
// A lane's left and right gains (0 when inactive).
void lane_lr(const PannerLane &p, float *l, float *r) {
    if (!p.active || !(p.gain > 0.f)) { *l = *r = 0.f; return; }
    qx_pan_gains(p.azimuth_deg, l, r);
    *l *= p.gain;
    *r *= p.gain;
}
}  // namespace

void qx_pan_gains(float azimuth_deg, float *left, float *right) {
    const double a = !(azimuth_deg == azimuth_deg) ? 0.0 : azimuth_deg < -90.f ? -90.0 : azimuth_deg > 90.f ? 90.0 : azimuth_deg;
    const double theta = (a + 90.0) * M_PI / 360.0;
    *left = (float)std::cos(theta);
    *right = (float)std::sin(theta);
}

void qx_panner_plan(const ar_audio_tuning &tuning, const ar_pose &listener, const ar_audio_source src[AR_AUDIO_MAX_SOURCES],
                    PannerLane out[AR_AUDIO_MAX_SOURCES]) {
    ar_audio_tuning t = tuning;
    ar_audio_tuning_clamp(&t);
    const bool lis_ok = finite_pose(listener);
    for (uint32_t l = 0; l < AR_AUDIO_MAX_SOURCES; l++) {
        out[l] = PannerLane{false, 0.f, 0.f};
        const ar_audio_source &s = src[l];
        if (!lis_ok || !(s.flags & AR_SRC_ACTIVE) || !finite3(s.pos)) continue;
        float pos[3];
        const float g = ar_audio_lane_gain(&t, &listener, &s, pos);
        if (!std::isfinite(g) || !finite3(pos)) continue;
        out[l] = PannerLane{true, g, (float)listener_direction(listener, pos).azimuth_deg};
    }
}

void qx_panner_mix(const float *in, uint32_t frames, const PannerLane prev[AR_AUDIO_MAX_SOURCES],
                   const PannerLane plan[AR_AUDIO_MAX_SOURCES], float bed_gain, float *out) {
    for (uint32_t f = 0; f < frames; f++) {
        out[2 * (size_t)f] = in[(size_t)f * kSceneChannels] * bed_gain;
        out[2 * (size_t)f + 1] = in[(size_t)f * kSceneChannels + 1] * bed_gain;
    }
    if (frames == 0) return;
    const float inv = 1.f / (float)frames;
    for (uint32_t l = 0; l < AR_AUDIO_MAX_SOURCES; l++) {
        float l0, r0, l1, r1;
        lane_lr(prev[l], &l0, &r0);
        lane_lr(plan[l], &l1, &r1);
        if (l0 == 0.f && r0 == 0.f && l1 == 0.f && r1 == 0.f) continue;
        const float dl = (l1 - l0) * inv, dr = (r1 - r0) * inv;
        const float *s = in + 2 + l;
        for (uint32_t f = 0; f < frames; f++) {
            const float x = s[(size_t)f * kSceneChannels], k = (float)(f + 1);
            out[2 * (size_t)f] += x * (l0 + dl * k);
            out[2 * (size_t)f + 1] += x * (r0 + dr * k);
        }
    }
}

}  // namespace ar
