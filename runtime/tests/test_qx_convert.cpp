#include "check.h"
#include <cmath>
#include <cstring>
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>
#include "ar_math.h"
#include "qx_convert.h"

using namespace ar;

int main() {
    // fov tangents and sign
    XrView v{XR_TYPE_VIEW}; v.pose = {{0,0,0,1},{0.1f,1.6f,-0.2f}}; v.fov = {-0.8f, 0.7f, 0.75f, -0.85f};
    ar_view a = qx_view_from_xr(v, 1832, 1920, 0.f);
    CHECK(fabsf(a.fov.left - tanf(-0.8f)) < 1e-6f && a.fov.left < 0.f && a.fov.down < 0.f && a.fov.right > 0.f);
    CHECK(a.width == 1832 && a.height == 1920 && a.tracking_from_eye.pos[1] == 1.6f && a.tracking_from_eye.quat[3] == 1.f);
    // tracked frame: head at the eye midpoint
    XrView vs[2] = {v, v}; vs[1].pose.position.x = 0.164f;
    ar_pose last = ar_pose_identity(); last.pos[1] = 9.f;
    ar_frame_info fi{};
    CHECK(qx_fill_frame_info(vs, 2, XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT, 123456789, 90.f, 1832, 1920, 0.f, last, &fi));
    CHECK(fi.head_tracked == 1 && fi.view_count == 2 && fi.display_time_ns == 123456789 && fi.display_hz == 90.f);
    CHECK(fabsf(fi.tracking_from_head.pos[0] - 0.132f) < 1e-6f && fi.tracking_from_head.pos[1] == 1.6f);
    // tracking lost: last head kept
    CHECK(qx_fill_frame_info(vs, 2, 0, 1, 90.f, 1832, 1920, 0.f, last, &fi));
    CHECK(fi.head_tracked == 0 && fi.tracking_from_head.pos[1] == 9.f);
    CHECK(!qx_fill_frame_info(vs, 0, 0, 1, 90.f, 1, 1, 0.f, last, &fi));
    // session states
    CHECK(qx_session_state(XR_SESSION_STATE_FOCUSED, false) == AR_SESSION_FOCUSED);
    CHECK(qx_session_state(XR_SESSION_STATE_VISIBLE, false) == AR_SESSION_VISIBLE);
    CHECK(qx_session_state(XR_SESSION_STATE_SYNCHRONIZED, false) == AR_SESSION_VISIBLE);
    CHECK(qx_session_state(XR_SESSION_STATE_IDLE, false) == AR_SESSION_IDLE);
    CHECK(qx_session_state(XR_SESSION_STATE_LOSS_PENDING, false) == AR_SESSION_LOST);
    CHECK(qx_session_state(XR_SESSION_STATE_FOCUSED, true) == AR_SESSION_STOPPING);
    // swapchain formats
    bool lin = false;
    const int64_t f1[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
    CHECK(qx_pick_swapchain_format(f1, 2, &lin) == VK_FORMAT_B8G8R8A8_SRGB && lin);
    const int64_t f2[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM};
    CHECK(qx_pick_swapchain_format(f2, 2, &lin) == VK_FORMAT_B8G8R8A8_UNORM && !lin);
    const int64_t f3[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    CHECK(qx_pick_swapchain_format(f3, 1, &lin) == 0);
    // pose round trip
    ar_pose p = ar_pose_identity(); p.pos[2] = -1.f; p.quat[0] = 0.5f; p.quat[3] = 0.5f;
    ar_pose q = qx_ar_pose(qx_xr_pose(p));
    CHECK(memcmp(&p, &q, sizeof p) == 0);
    return 0;
}
