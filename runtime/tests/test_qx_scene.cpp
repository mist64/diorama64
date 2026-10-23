#include <math.h>
#include "check.h"
#include "qx_scene_pure.h"

using namespace ar;

int main() {
    // a 1.2 x 0.8 m table plane, anchor at (1, 0.74, -2) with +Z up (anchor rotated -90 deg about X)
    XrPosef a{}; a.orientation = {-0.70710678f, 0.f, 0.f, 0.70710678f}; a.position = {1.f, 0.74f, -2.f};
    XrRect2Df r{{-0.4f, -0.6f}, {0.8f, 1.2f}};
    QxSceneTable t = qx_table_from_scene("u1", a, r);
    CHECK(fabsf(t.tracking_from_table.pos[1] - 0.74f) < 1e-4f && fabsf(t.half_u - 0.6f) < 1e-4f && fabsf(t.half_v - 0.4f) < 1e-4f);
    float up[3]; const float up_in[3] = {0.f, 1.f, 0.f}; ar_quat_rotate(t.tracking_from_table.quat, up_in, up); CHECK(up[1] > 0.999f);   // +Y is world up
    // chooser: nearest valid table; one 0.2 m above the floor is excluded
    QxSceneTable low = t; low.uuid = "low"; low.tracking_from_table.pos[1] = 0.2f; low.tracking_from_table.pos[0] = 0.f;
    ar_pose head = ar_pose_identity(); head.pos[1] = 1.6f;
    CHECK(qx_choose_table({low, t}, head, true, 0.f) == 1);
    CHECK(qx_choose_table({}, head, true, 0.f) == -1);
    // placement and drag clamp
    QxPlacement p; p.uuid = "u1";
    ar_pose lv = qx_place(t, p); CHECK(fabsf(lv.pos[0] - 1.f) < 1e-4f);
    const float o[3] = {1.f, 1.5f, -1.f}, d[3] = {0.f, -0.6f, -0.8f};      // ray toward the table from the front
    const float grab[2] = {0.f, 0.f};
    CHECK(qx_drag_update(t, o, d, grab, &p) && fabsf(p.offset_v) <= t.half_v + 1e-4f && fabsf(p.offset_u) <= t.half_u + 1e-4f);
    const float far_d[3] = {1.f, -0.1f, 0.f};                                 // would land 5 m off the table: clamped to the edge
    CHECK(qx_drag_update(t, o, far_d, grab, &p) && fabsf(fabsf(p.offset_u) - t.half_u) < 1e-4f);
    const float miss[3] = {0.f, 1.f, 0.f}; CHECK(!qx_drag_update(t, o, miss, grab, &p));   // ray away from the plane: no change
    CHECK(qx_drag_yaw_step(0.1f, 1.f) == 0.f && fabsf(qx_drag_yaw_step(1.f, 0.5f) - 45.f) < 1e-4f);
    // record round trip
    p.offset_u = 0.1f; p.offset_v = -0.2f; p.yaw_deg = 30.f;
    QxPlacement q; CHECK(qx_placement_from(qx_record_from(p), &q) && q.uuid == "u1" && q.offset_u == 0.1f && q.offset_v == -0.2f && q.yaw_deg == 30.f);
    printf("test_qx_scene ok\n");
    return 0;
}
