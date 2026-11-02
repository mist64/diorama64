// The Quest's spatial mode: plan 3's pure constant-power panner over SceneFeed's lanes (qx_panner.h).
#include <math.h>
#include <vector>
#include "check.h"
#include "qx_panner.h"
extern "C" {
#include "ar_math.h"
}

using namespace ar;

int main() {
    float l, r;
    qx_pan_gains(0.f, &l, &r);   CHECK(fabsf(l - r) < 1e-6f && fabsf(l * l + r * r - 1.f) < 1e-5f);
    qx_pan_gains(90.f, &l, &r);  CHECK(r > 0.99f && l < 0.01f);
    qx_pan_gains(-90.f, &l, &r); CHECK(l > 0.99f && r < 0.01f);
    qx_pan_gains(170.f, &l, &r); CHECK(r > 0.99f);                                  // behind: clamped to the side
    ar_audio_tuning t{}; ar_audio_tuning_clamp(&t);
    ar_pose lis = ar_pose_identity(); lis.pos[2] = 1.f;                                // listener 1 m in front, looking -Z
    ar_audio_source src[AR_AUDIO_MAX_SOURCES]{};
    src[0] = {1, AR_SRC_ACTIVE, {0.5f, 0.f, 0.f}, 1.f};                               // right of centre, ahead
    src[1] = {2, AR_SRC_ACTIVE, {NAN, 0.f, 0.f}, 1.f};
    src[2] = {3, AR_SRC_ACTIVE, {0.f, 0.f, 0.f}, 1.f};
    src[3] = {4, AR_SRC_ACTIVE | AR_SRC_NO_DISTANCE_LOSS, {0.f, 0.f, -5.f}, 1.f};
    PannerLane plan[AR_AUDIO_MAX_SOURCES]; qx_panner_plan(t, lis, src, plan);
    CHECK(plan[0].active && plan[0].azimuth_deg > 5.f && !plan[1].active && plan[1].gain == 0.f);
    // distance attenuation (the spec): 1 m ahead is louder than 3 m ahead; a NO_DISTANCE_LOSS lane 6 m away keeps gain 1
    CHECK(plan[2].active && fabsf(plan[2].azimuth_deg) < 1e-3f && fabsf(plan[3].gain - 1.f) < 1e-5f);
    { ar_audio_source far[AR_AUDIO_MAX_SOURCES]{}; far[0] = {5, AR_SRC_ACTIVE, {0.f, 0.f, -2.f}, 1.f}; PannerLane fp[AR_AUDIO_MAX_SOURCES];
      qx_panner_plan(t, lis, far, fp); CHECK(fp[0].active && fp[0].gain < plan[2].gain && plan[2].gain < 1.f); }
    CHECK(!plan[4].active);
    // mix: a lane fully right lands in the right channel only, the bed passes through
    std::vector<float> in(kSceneChannels * 4, 0.f), out(8, 0.f);
    for (uint32_t f = 0; f < 4; f++) { in[f * kSceneChannels + 0] = 0.25f; in[f * kSceneChannels + 1] = -0.25f; in[f * kSceneChannels + 2] = 1.f; }
    PannerLane z[AR_AUDIO_MAX_SOURCES]{}; PannerLane p1[AR_AUDIO_MAX_SOURCES]{}; p1[0] = {true, 1.f, 90.f};
    qx_panner_mix(in.data(), 4, p1, p1, 1.f, out.data());
    CHECK(fabsf(out[6] - 0.25f) < 1e-5f && fabsf(out[7] - (-0.25f + 1.f)) < 1e-5f);     // last frame: L = bed, R = bed + lane
    qx_panner_mix(in.data(), 4, z, p1, 1.f, out.data());
    CHECK(out[1] < out[7]);                                                           // ramping in: the first frame has less lane than the last
    // a non-finite listener mutes every lane
    lis.pos[0] = NAN; qx_panner_plan(t, lis, src, plan); for (auto &L : plan) CHECK(!L.active);
    printf("test_qx_panner ok\n");
    return 0;
}
