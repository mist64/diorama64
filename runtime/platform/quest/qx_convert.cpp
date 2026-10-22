#include "qx_convert.h"
#include <math.h>
#include <string.h>
#include <vulkan/vulkan.h>

namespace ar {

ar_pose qx_ar_pose(const XrPosef &p) {
    ar_pose r;
    r.pos[0] = p.position.x; r.pos[1] = p.position.y; r.pos[2] = p.position.z;
    r.quat[0] = p.orientation.x; r.quat[1] = p.orientation.y; r.quat[2] = p.orientation.z; r.quat[3] = p.orientation.w;
    return r;
}

XrPosef qx_xr_pose(const ar_pose &p) {
    XrPosef r;
    r.position = {p.pos[0], p.pos[1], p.pos[2]};
    r.orientation = {p.quat[0], p.quat[1], p.quat[2], p.quat[3]};
    return r;
}

ar_view qx_view_from_xr(const XrView &v, uint32_t width, uint32_t height, float near_m) {
    ar_view r;
    memset(&r, 0, sizeof r);
    r.tracking_from_eye = qx_ar_pose(v.pose);
    r.fov.left = tanf(v.fov.angleLeft);
    r.fov.right = tanf(v.fov.angleRight);
    r.fov.up = tanf(v.fov.angleUp);
    r.fov.down = tanf(v.fov.angleDown);
    r.width = width;
    r.height = height;
    r.near_m = near_m;
    return r;
}

bool qx_fill_frame_info(const XrView *views, uint32_t n, XrViewStateFlags flags, XrTime display_time, float display_hz,
                        uint32_t width, uint32_t height, float near_m, const ar_pose &last_head, ar_frame_info *out) {
    if (n == 0 || n > AR_MAX_VIEWS) return false;
    memset(out, 0, sizeof *out);
    out->view_count = n;
    for (uint32_t i = 0; i < n; i++) out->views[i] = qx_view_from_xr(views[i], width, height, near_m);
    out->display_time_ns = display_time;
    out->display_hz = display_hz;
    const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    out->head_tracked = (flags & need) == need ? 1 : 0;
    if (out->head_tracked) {
        ar_pose head = out->views[0].tracking_from_eye;
        if (n >= 2) {
            for (int k = 0; k < 3; k++)
                head.pos[k] = 0.5f * (out->views[0].tracking_from_eye.pos[k] + out->views[1].tracking_from_eye.pos[k]);
        }
        out->tracking_from_head = head;
    } else {
        out->tracking_from_head = last_head;
    }
    return true;
}

ar_session_state qx_session_state(XrSessionState s, bool exit_requested) {
    if (exit_requested) return AR_SESSION_STOPPING;
    switch (s) {
    case XR_SESSION_STATE_READY: return AR_SESSION_READY;
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_VISIBLE: return AR_SESSION_VISIBLE;
    case XR_SESSION_STATE_FOCUSED: return AR_SESSION_FOCUSED;
    case XR_SESSION_STATE_STOPPING: return AR_SESSION_STOPPING;
    case XR_SESSION_STATE_LOSS_PENDING:
    case XR_SESSION_STATE_EXITING: return AR_SESSION_LOST;
    default: return AR_SESSION_IDLE;
    }
}

int64_t qx_pick_swapchain_format(const int64_t *formats, uint32_t n, bool *linearize) {
    static const int64_t srgb[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
    static const int64_t unorm[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (int64_t want : srgb)
        for (uint32_t i = 0; i < n; i++)
            if (formats[i] == want) { *linearize = true; return want; }
    for (int64_t want : unorm)
        for (uint32_t i = 0; i < n; i++)
            if (formats[i] == want) { *linearize = false; return want; }
    return 0;
}

}
