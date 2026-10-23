// Where the Quest settings panel floats: a quad of width_m facing the head (yaw only: it stays upright) at
// ar_float_panel_anchor's point (above the level, or beside it toward the head when the level is tall), and the
// controller ray's hit on it. Pure.
#pragma once
extern "C" {
#include "ar_runtime.h"
}

namespace ar {
namespace panel {

// tracking_from_panel: origin at the quad's centre, +X right, +Y up, +Z toward the viewer (the front face). corners:
// top-left, top-right, bottom-right, bottom-left in tracking space. Without a placed table: 0.7 m ahead of the head,
// 0.15 m below it.
struct PanelPose { ar_pose tracking_from_panel; float corners[4][3]; float height_m; float width_m = 0.f; };
constexpr float kAboveTopM = 0.05f, kMaxAboveGroundM = 0.45f, kEdgeGapM = 0.1f;
PanelPose place_panel(const ar_render_frame &rf, float width_m, float aspect);   // aspect: width / height
// o, d: a ray in tracking space (d need not be unit). u right, v down in 0..1; false when it misses the quad or points away.
bool ray_hit_panel(const PanelPose &p, const float o[3], const float d[3], float *u, float *v);
// The hit point in tracking space for u, v.
void panel_point(const PanelPose &p, float u, float v, float out[3]);

}  // namespace panel
}  // namespace ar
