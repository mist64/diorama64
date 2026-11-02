#pragma once
#include <stdint.h>
#include <openxr/openxr.h>
#include "ar_platform.h"

namespace ar {
ar_view qx_view_from_xr(const XrView &v, uint32_t width, uint32_t height, float near_m);
bool qx_fill_frame_info(const XrView *views, uint32_t n, XrViewStateFlags flags, XrTime display_time, float display_hz,
                        uint32_t width, uint32_t height, float near_m, const ar_pose &last_head, ar_frame_info *out);
ar_session_state qx_session_state(XrSessionState s, bool exit_requested);
int64_t qx_pick_swapchain_format(const int64_t *formats, uint32_t n, bool *linearize);
XrPosef qx_xr_pose(const ar_pose &p);
ar_pose qx_ar_pose(const XrPosef &p);
}
