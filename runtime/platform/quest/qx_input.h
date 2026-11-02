// The Touch controllers through OpenXR (ar_quest only): one action set "diorama" bound for
// /interaction_profiles/oculus/touch_controller, synced once per wait_frame on the render thread. The portable half
// (QxActionState, the pad mapping, the pointer router) is qx_input_pure.h, host-tested in ar_qx_logic.
#pragma once
#include <stdint.h>
#include <mutex>
#include <string>
#include <openxr/openxr.h>
#include "qx_input_pure.h"
#include "vos_state.h"

namespace ar {

class QxInput {   // owns the XrActionSet; profile /interaction_profiles/oculus/touch_controller
public:
    QxInput() = default;
    ~QxInput() { destroy(); }
    QxInput(const QxInput &) = delete;
    QxInput &operator=(const QxInput &) = delete;
    bool create(XrInstance, XrSession, std::string *err);   // actions, suggested bindings, aim spaces
    void attach();                                  // xrAttachSessionActionSets (once, after create)
    // Render thread, from wait_frame: xrSyncActions and every action read, aim poses located in app_space at t. An
    // unfocused session (XR_SESSION_NOT_FOCUSED) or a failed call reads as no input: *out zeroed (time_ns = t).
    void sync(XrSpace app_space, XrTime t, QxActionState *out);
    void clear(XrTime t, QxActionState *out);       // no input this frame (the session is not focused or not running)
    int get_pad(ar_pad *out);                       // game thread: last synced state under a mutex; 1 when any controller is active
    void hold_until_released(uint32_t mask);        // HeldButtonGate, as VosPads
    void destroy();                                 // the aim spaces and the action set; before the session goes

private:
    void publish(const QxActionState &s);
    bool ok(XrResult r, const char *step, std::string *err = nullptr);

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrActionSet set_ = XR_NULL_HANDLE;
    XrPath hand_[2] = {XR_NULL_PATH, XR_NULL_PATH};   // /user/hand/left, /user/hand/right
    XrAction stick_ = XR_NULL_HANDLE, stick_click_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, squeeze_ = XR_NULL_HANDLE,
             aim_ = XR_NULL_HANDLE, a_ = XR_NULL_HANDLE, b_ = XR_NULL_HANDLE, x_ = XR_NULL_HANDLE, y_ = XR_NULL_HANDLE,
             menu_ = XR_NULL_HANDLE;
    XrSpace aim_space_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool attached_ = false;
    std::string last_error_;   // the last failed step logged, so a failing call per frame logs once

    std::mutex m_;
    ar_pad pad_{};
    bool active_ = false;
    HeldButtonGate gate_;
};

}  // namespace ar
