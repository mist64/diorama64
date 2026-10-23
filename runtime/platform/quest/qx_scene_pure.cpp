#include "qx_scene_pure.h"
#include <cmath>
#include <cstdio>
#include "vos_planes.h"

namespace ar {

namespace {
float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
bool normalize3(float v[3]) {
    const float n = std::sqrt(dot3(v, v));
    if (!(n > 1e-6f)) return false;
    for (int k = 0; k < 3; k++) v[k] /= n;
    return true;
}
float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
void table_axes(const QxSceneTable &t, float x[3], float y[3], float z[3]) {
    const float ex[3] = {1.f, 0.f, 0.f}, ey[3] = {0.f, 1.f, 0.f}, ez[3] = {0.f, 0.f, 1.f};
    ar_quat_rotate(t.tracking_from_table.quat, ex, x);
    ar_quat_rotate(t.tracking_from_table.quat, ey, y);
    ar_quat_rotate(t.tracking_from_table.quat, ez, z);
}
}  // namespace

QxSceneTable qx_table_from_scene(const std::string &uuid, const XrPosef &anchor, const XrRect2Df &rect) {
    const ar_pose a{{anchor.position.x, anchor.position.y, anchor.position.z},
                    {anchor.orientation.x, anchor.orientation.y, anchor.orientation.z, anchor.orientation.w}};
    const float lx[3] = {1.f, 0.f, 0.f}, ly[3] = {0.f, 1.f, 0.f}, lz[3] = {0.f, 0.f, 1.f};
    float n[3], along[3];
    ar_quat_rotate(a.quat, lz, n);
    if (n[1] < 0.f) for (int k = 0; k < 3; k++) n[k] = -n[k];   // the surface normal toward world up
    const bool width_longer = rect.extent.width >= rect.extent.height;
    ar_quat_rotate(a.quat, width_longer ? lx : ly, along);
    // Table +X: the longer side, kept in the plane; +Y the normal; +Z = X x Y (right-handed).
    const float d = dot3(along, n);
    float x[3] = {along[0] - d * n[0], along[1] - d * n[1], along[2] - d * n[2]};
    if (!normalize3(x)) { x[0] = 1.f; x[1] = 0.f; x[2] = 0.f; }
    float z[3];
    cross3(x, n, z);
    normalize3(z);
    ar_mat4 m = ar_mat4_identity();
    for (int k = 0; k < 3; k++) { m.m[0 + k] = x[k]; m.m[4 + k] = n[k]; m.m[8 + k] = z[k]; }
    const float c_local[3] = {rect.offset.x + rect.extent.width * 0.5f, rect.offset.y + rect.extent.height * 0.5f, 0.f};
    float c[3];
    ar_quat_rotate(a.quat, c_local, c);
    for (int k = 0; k < 3; k++) m.m[12 + k] = a.pos[k] + c[k];
    QxSceneTable t;
    t.uuid = uuid;
    t.tracking_from_table = ar_pose_from_mat4(m);
    t.half_u = 0.5f * (width_longer ? rect.extent.width : rect.extent.height);
    t.half_v = 0.5f * (width_longer ? rect.extent.height : rect.extent.width);
    return t;
}

int qx_choose_table(const std::vector<QxSceneTable> &tables, const ar_pose &head, bool floor_known, float floor_y) {
    int best = -1;
    float best_d2 = 0.f;
    for (size_t i = 0; i < tables.size(); i++) {
        const QxSceneTable &t = tables[i];
        PlaneInfo p;
        for (int k = 0; k < 3; k++) p.center[k] = t.tracking_from_table.pos[k];
        const float ey[3] = {0.f, 1.f, 0.f};
        ar_quat_rotate(t.tracking_from_table.quat, ey, p.normal);
        p.half_u = t.half_u;
        p.half_v = t.half_v;
        p.is_table = true;
        if (!vos_plane_is_table_candidate(p, floor_known, floor_y)) continue;
        float d2 = 0.f;
        for (int k = 0; k < 3; k++) d2 += (p.center[k] - head.pos[k]) * (p.center[k] - head.pos[k]);
        if (best < 0 || d2 < best_d2) { best = (int)i; best_d2 = d2; }
    }
    return best;
}

ar_pose qx_place(const QxSceneTable &t, const QxPlacement &p) {
    const double h = (double)p.yaw_deg * M_PI / 360.0;
    const ar_pose local{{p.offset_u, 0.f, p.offset_v}, {0.f, (float)std::sin(h), 0.f, (float)std::cos(h)}};
    return ar_pose_mul(t.tracking_from_table, local);
}

bool qx_table_hit_uv(const QxSceneTable &t, const float o[3], const float d[3], float uv[2]) {
    float x[3], n[3], z[3];
    table_axes(t, x, n, z);
    const float denom = dot3(d, n);
    if (std::fabs(denom) < 1e-4f) return false;
    const float *c = t.tracking_from_table.pos;
    const float w[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const float s = dot3(w, n) / denom;
    if (!(s > 0.f)) return false;   // behind the origin (or NaN)
    const float r[3] = {o[0] + d[0] * s - c[0], o[1] + d[1] * s - c[1], o[2] + d[2] * s - c[2]};
    uv[0] = dot3(r, x);
    uv[1] = dot3(r, z);
    return std::isfinite(uv[0]) && std::isfinite(uv[1]);
}

bool qx_drag_update(const QxSceneTable &t, const float o[3], const float d[3], const float grab_uv[2], QxPlacement *p) {
    float uv[2];
    if (!qx_table_hit_uv(t, o, d, uv)) return false;
    p->offset_u = clampf(uv[0] - grab_uv[0], -t.half_u, t.half_u);
    p->offset_v = clampf(uv[1] - grab_uv[1], -t.half_v, t.half_v);
    return true;
}

float qx_drag_yaw_step(float rx, float dt_s) {
    constexpr float kDead = 0.2f, kRate = 90.f;
    if (!(std::fabs(rx) > kDead)) return 0.f;
    const float s = rx > 0.f ? 1.f : -1.f;
    return (clampf(rx, -1.f, 1.f) - s * kDead) / (1.f - kDead) * kRate * dt_s;
}

PlacementRecord qx_record_from(const QxPlacement &p) {
    PlacementRecord r;
    r.anchor = p.uuid;
    r.game = "quest";
    r.yaw_deg = p.yaw_deg;
    r.tracking_from_table = ar_pose_identity();
    r.tracking_from_table.pos[0] = p.offset_u;
    r.tracking_from_table.pos[2] = p.offset_v;
    return r;
}

bool qx_placement_from(const PlacementRecord &r, QxPlacement *out) {
    if (r.anchor.empty() || r.anchor == "fixed") return false;
    QxPlacement p;
    p.uuid = r.anchor;
    p.yaw_deg = r.yaw_deg;
    p.offset_u = r.tracking_from_table.pos[0];
    p.offset_v = r.tracking_from_table.pos[2];
    *out = p;
    return true;
}

std::string qx_uuid_string(const uint8_t b[16]) {
    char s[37];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
                  b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

bool qx_has_label(const std::string &labels, const char *label) {
    const std::string want = label;
    size_t at = 0;
    while (at <= labels.size()) {
        size_t end = labels.find(',', at);
        if (end == std::string::npos) end = labels.size();
        if (labels.compare(at, end - at, want) == 0) return true;
        at = end + 1;
    }
    return false;
}

}  // namespace ar
