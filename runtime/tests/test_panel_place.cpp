// Where the Quest settings panel floats (apps/quest/panel/panel_place.h).
#include "check.h"
#include "panel_place.h"

#include <cmath>
#include <cstring>

using namespace ar::panel;

static ar_render_frame frame_with_level(float height_m) {
    ar_render_frame rf;
    std::memset(&rf, 0, sizeof rf);
    rf.table_valid = 1;
    rf.tracking_from_table = ar_pose_identity();
    rf.level.units_per_meter = 100.f;   // level units: centimetres
    rf.level.ground_y = 0.f;
    rf.level.bounds_min[0] = -50.f; rf.level.bounds_min[1] = 0.f; rf.level.bounds_min[2] = -50.f;
    rf.level.bounds_max[0] = 50.f; rf.level.bounds_max[1] = height_m * 100.f; rf.level.bounds_max[2] = 50.f;
    rf.head_table = ar_pose_identity();
    rf.head_table.pos[1] = 0.4f; rf.head_table.pos[2] = 1.0f;   // 1 m in front of the table's origin
    return rf;
}
static void axis(const ar_pose &p, float x, float y, float z, float out[3]) {
    const float v[3] = {x, y, z};
    ar_quat_rotate(p.quat, v, out);
}
static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void above_a_low_level() {
    const ar_render_frame rf = frame_with_level(0.3f);
    const PanelPose p = place_panel(rf, 0.6f, 4.f / 3.f);
    const float *c = p.tracking_from_panel.pos;
    CHECK(c[1] > 0.3f);
    CHECK(std::fabs(p.height_m - 0.45f) < 1e-4f);
    // the front (+Z) faces the head: the panel's -Z is the head-to-panel direction
    float back[3], up[3];
    axis(p.tracking_from_panel, 0.f, 0.f, 1.f, back);
    axis(p.tracking_from_panel, 0.f, 1.f, 0.f, up);
    float to_head[3] = {rf.head_table.pos[0] - c[0], rf.head_table.pos[1] - c[1], rf.head_table.pos[2] - c[2]};
    const float l = std::sqrt(dot(to_head, to_head));
    for (float &x : to_head) x /= l;
    CHECK(dot(back, to_head) > 0.9f);
    CHECK(up[1] > 0.999f);   // upright
    // the corners span width x height around the centre
    const float dx = p.corners[1][0] - p.corners[0][0], dy = p.corners[0][1] - p.corners[3][1];
    CHECK(std::fabs(dx - 0.6f) < 1e-4f && std::fabs(dy - 0.45f) < 1e-4f);
}

static void beside_a_tall_level() {
    const ar_render_frame rf = frame_with_level(2.f);
    const PanelPose p = place_panel(rf, 0.6f, 4.f / 3.f);
    const float *c = p.tracking_from_panel.pos;
    CHECK(c[2] > 0.5f);                 // past the level's near edge, toward the head
    CHECK(std::fabs(c[0]) < 1e-3f);
    CHECK(c[1] < 2.f);                  // not above the 2 m level
}

static void follows_the_table() {
    ar_render_frame rf = frame_with_level(0.3f);
    rf.tracking_from_table.pos[0] = 1.f;
    rf.tracking_from_table.pos[1] = 0.75f;
    rf.tracking_from_table.pos[2] = -1.f;
    const PanelPose p = place_panel(rf, 0.6f, 4.f / 3.f);
    CHECK(std::fabs(p.tracking_from_panel.pos[0] - 1.f) < 1e-3f);
    CHECK(p.tracking_from_panel.pos[1] > 0.75f + 0.3f);
}

static void ray_hits() {
    const ar_render_frame rf = frame_with_level(0.3f);
    const PanelPose p = place_panel(rf, 0.6f, 4.f / 3.f);
    const float *o = rf.head_table.pos, *c = p.tracking_from_panel.pos;
    const float d[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    float u = -1.f, v = -1.f;
    CHECK(ray_hit_panel(p, o, d, &u, &v));
    CHECK(std::fabs(u - 0.5f) < 1e-3f && std::fabs(v - 0.5f) < 1e-3f);
    // toward the top-left quarter: u < 0.5 (left), v < 0.5 (up)
    float right[3], up[3];
    axis(p.tracking_from_panel, 1.f, 0.f, 0.f, right);
    axis(p.tracking_from_panel, 0.f, 1.f, 0.f, up);
    float q[3], d2[3];
    for (int k = 0; k < 3; k++) { q[k] = c[k] - right[k] * 0.15f + up[k] * 0.1f; d2[k] = q[k] - o[k]; }
    CHECK(ray_hit_panel(p, o, d2, &u, &v));
    CHECK(std::fabs(u - 0.25f) < 1e-3f && v < 0.5f && std::fabs(v - (0.5f - 0.1f / 0.45f)) < 1e-3f);
    float back_pt[3];
    panel_point(p, u, v, back_pt);
    for (int k = 0; k < 3; k++) CHECK(std::fabs(back_pt[k] - q[k]) < 1e-3f);
    // past the right edge: a miss
    for (int k = 0; k < 3; k++) { q[k] = c[k] + right[k] * 0.36f; d2[k] = q[k] - o[k]; }
    CHECK(!ray_hit_panel(p, o, d2, &u, &v));
    // pointing away: a miss
    const float away[3] = {-d[0], -d[1], -d[2]};
    CHECK(!ray_hit_panel(p, o, away, &u, &v));
}

static void without_a_table() {
    ar_render_frame rf = frame_with_level(0.3f);
    rf.table_valid = 0;
    const PanelPose p = place_panel(rf, 0.6f, 4.f / 3.f);
    const float *c = p.tracking_from_panel.pos, *h = rf.head_table.pos;
    const float dist = std::sqrt((c[0] - h[0]) * (c[0] - h[0]) + (c[2] - h[2]) * (c[2] - h[2]));
    CHECK(std::fabs(dist - 0.7f) < 1e-3f);
    CHECK(c[1] < h[1]);
}

int main() {
    above_a_low_level();
    beside_a_tall_level();
    follows_the_table();
    ray_hits();
    without_a_table();
    printf("test_panel_place ok\n");
    return 0;
}
