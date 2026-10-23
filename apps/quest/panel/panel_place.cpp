#include "panel_place.h"
#include <cmath>
extern "C" {
#include "ar_float_panel.h"
#include "ar_math.h"
}

namespace ar {
namespace panel {
namespace {
// The rotation whose columns are r, u, b (orthonormal, right-handed) as a quaternion xyzw.
void quat_from_basis(const float r[3], const float u[3], const float b[3], float q[4]) {
    const float m00 = r[0], m11 = u[1], m22 = b[2], tr = m00 + m11 + m22;
    if (tr > 0.f) {
        const float s = std::sqrt(tr + 1.f) * 2.f;
        q[3] = 0.25f * s; q[0] = (u[2] - b[1]) / s; q[1] = (b[0] - r[2]) / s; q[2] = (r[1] - u[0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q[3] = (u[2] - b[1]) / s; q[0] = 0.25f * s; q[1] = (u[0] + r[1]) / s; q[2] = (b[0] + r[2]) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q[3] = (b[0] - r[2]) / s; q[0] = (u[0] + r[1]) / s; q[1] = 0.25f * s; q[2] = (b[1] + u[2]) / s;
    } else {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q[3] = (r[1] - u[0]) / s; q[0] = (b[0] + r[2]) / s; q[1] = (b[1] + u[2]) / s; q[2] = 0.25f * s;
    }
}
// An upright quad of w x h centred at c (tracking space), its front turned toward the head in the horizontal plane;
// fallback_back when the head is straight above or below.
PanelPose upright_quad(const float c[3], const float head[3], const float fallback_back[3], float w, float h) {
    float b[3] = {head[0] - c[0], 0.f, head[2] - c[2]};
    float l = std::sqrt(b[0] * b[0] + b[2] * b[2]);
    if (!(l > 1e-4f)) { b[0] = fallback_back[0]; b[2] = fallback_back[2]; l = std::sqrt(b[0] * b[0] + b[2] * b[2]); }
    if (!(l > 1e-6f)) { b[0] = 0.f; b[2] = 1.f; l = 1.f; }
    b[0] /= l; b[2] /= l;
    const float u[3] = {0.f, 1.f, 0.f};
    const float r[3] = {b[2], 0.f, -b[0]};   // up x back
    PanelPose p{};
    for (int k = 0; k < 3; k++) p.tracking_from_panel.pos[k] = c[k];
    quat_from_basis(r, u, b, p.tracking_from_panel.quat);
    const float sx[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, sy[4] = {0.5f, 0.5f, -0.5f, -0.5f};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) p.corners[i][k] = c[k] + r[k] * sx[i] * w + u[k] * sy[i] * h;
    p.height_m = h;
    p.width_m = w;
    return p;
}
float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
}  // namespace

PanelPose place_panel(const ar_render_frame &rf, float width_m, float aspect) {
    const float w = width_m > 0.f ? width_m : 0.6f;
    const float h = aspect > 0.f ? w / aspect : w * 0.75f;
    const ar_pose head = ar_pose_mul(rf.tracking_from_table, rf.head_table);
    const float mz[3] = {0.f, 0.f, -1.f};
    float fwd[3];
    ar_quat_rotate(head.quat, mz, fwd);
    if (!rf.table_valid) {   // no table to float over: ahead of the head, a little below it
        float f[3] = {fwd[0], 0.f, fwd[2]};
        float l = std::sqrt(f[0] * f[0] + f[2] * f[2]);
        if (!(l > 1e-4f)) { f[0] = 0.f; f[2] = -1.f; l = 1.f; }
        const float c[3] = {head.pos[0] + 0.7f * f[0] / l, head.pos[1] - 0.15f, head.pos[2] + 0.7f * f[2] / l};
        const float back[3] = {-f[0], 0.f, -f[2]};
        return upright_quad(c, head.pos, back, w, h);
    }
    // The level's bounds in table metres (ground at y = 0), then the anchor there, as ar_layer's floating panel.
    const ar_level_info &lv = rf.level;
    const float upm = std::isfinite(lv.units_per_meter) && lv.units_per_meter > 0.f ? lv.units_per_meter : 1.f;
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++) {
        const float off = k == 1 ? lv.ground_y : 0.f;
        mn[k] = (lv.bounds_min[k] - off) / upm;
        mx[k] = (lv.bounds_max[k] - off) / upm;
    }
    const ar_float_panel_params params{kAboveTopM, kMaxAboveGroundM, kEdgeGapM};
    float a[3];
    if (ar_float_panel_anchor(&params, mn, mx, 0.f, 1.f, rf.head_table.pos, a) == AR_FLOAT_PANEL_HIDDEN) {
        a[0] = 0.f; a[1] = kMaxAboveGroundM; a[2] = 0.f;
    }
    a[1] += 0.5f * h;   // the anchor is the quad's bottom centre
    const ar_pose anchor{{a[0], a[1], a[2]}, {0.f, 0.f, 0.f, 1.f}};
    const ar_pose c = ar_pose_mul(rf.tracking_from_table, anchor);
    const float table_z[3] = {0.f, 0.f, 1.f};
    float back[3];
    ar_quat_rotate(rf.tracking_from_table.quat, table_z, back);
    return upright_quad(c.pos, head.pos, back, w, h);
}

bool ray_hit_panel(const PanelPose &p, const float o[3], const float d[3], float *u, float *v) {
    float r[3], up[3], b[3];
    const float ex[3] = {1.f, 0.f, 0.f}, ey[3] = {0.f, 1.f, 0.f}, ez[3] = {0.f, 0.f, 1.f};
    ar_quat_rotate(p.tracking_from_panel.quat, ex, r);
    ar_quat_rotate(p.tracking_from_panel.quat, ey, up);
    ar_quat_rotate(p.tracking_from_panel.quat, ez, b);
    const float denom = dot3(d, b);
    if (!(std::fabs(denom) > 1e-7f)) return false;
    const float *c = p.tracking_from_panel.pos;
    const float oc[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const float t = dot3(oc, b) / denom;
    if (!(t > 0.f)) return false;
    const float q[3] = {o[0] + t * d[0] - c[0], o[1] + t * d[1] - c[1], o[2] + t * d[2] - c[2]};
    if (!(p.width_m > 0.f) || !(p.height_m > 0.f)) return false;
    const float uu = dot3(q, r) / p.width_m + 0.5f, vv = 0.5f - dot3(q, up) / p.height_m;
    if (!(uu >= 0.f && uu <= 1.f && vv >= 0.f && vv <= 1.f)) return false;
    *u = uu; *v = vv;
    return true;
}

void panel_point(const PanelPose &p, float u, float v, float out[3]) {
    float r[3], up[3];
    const float ex[3] = {1.f, 0.f, 0.f}, ey[3] = {0.f, 1.f, 0.f};
    ar_quat_rotate(p.tracking_from_panel.quat, ex, r);
    ar_quat_rotate(p.tracking_from_panel.quat, ey, up);
    for (int k = 0; k < 3; k++)
        out[k] = p.tracking_from_panel.pos[k] + r[k] * (u - 0.5f) * p.width_m + up[k] * (0.5f - v) * p.height_m;
}

}  // namespace panel
}  // namespace ar
