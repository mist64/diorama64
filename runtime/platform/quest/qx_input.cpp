#include "qx_input.h"
#include <string.h>
#include "qx_convert.h"
#include "vos_log.h"

namespace ar {

namespace {
constexpr int kLeft = 0, kRight = 1;
}  // namespace

bool QxInput::ok(XrResult r, const char *step, std::string *err) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE];
    if (instance_ == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance_, r, name))) snprintf(name, sizeof name, "%d", (int)r);
    const std::string line = std::string(step) + " failed: " + name;
    if (line != last_error_) {   // once per distinct failure, not every frame
        vos_log("AR_INPUT %s", line.c_str());
        last_error_ = line;
    }
    if (err) *err = line;
    return false;
}

bool QxInput::create(XrInstance instance, XrSession session, std::string *err) {
    destroy();
    instance_ = instance;
    session_ = session;
    if (!ok(xrStringToPath(instance_, "/user/hand/left", &hand_[kLeft]), "xrStringToPath(/user/hand/left)", err) ||
        !ok(xrStringToPath(instance_, "/user/hand/right", &hand_[kRight]), "xrStringToPath(/user/hand/right)", err))
        return false;

    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(si.actionSetName, "diorama", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(si.localizedActionSetName, "Diorama64", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    si.priority = 0;
    if (!ok(xrCreateActionSet(instance_, &si, &set_), "xrCreateActionSet", err)) return false;

    auto make = [&](XrAction *out, const char *name, const char *label, XrActionType type, bool per_hand) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        ai.actionType = type;
        strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(ai.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        ai.countSubactionPaths = per_hand ? 2 : 0;
        ai.subactionPaths = per_hand ? hand_ : nullptr;
        return ok(xrCreateAction(set_, &ai, out), "xrCreateAction", err);
    };
    if (!make(&stick_, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true) ||
        !make(&stick_click_, "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, true) ||
        !make(&trigger_, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) ||
        !make(&squeeze_, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true) ||
        !make(&aim_, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, true) ||
        !make(&a_, "a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !make(&b_, "b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !make(&x_, "x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !make(&y_, "y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !make(&menu_, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false))
        return false;

    struct Binding { XrAction action; const char *path; };
    const Binding table[] = {
        {stick_, "/user/hand/left/input/thumbstick"},        {stick_, "/user/hand/right/input/thumbstick"},
        {stick_click_, "/user/hand/left/input/thumbstick/click"}, {stick_click_, "/user/hand/right/input/thumbstick/click"},
        {trigger_, "/user/hand/left/input/trigger/value"},   {trigger_, "/user/hand/right/input/trigger/value"},
        {squeeze_, "/user/hand/left/input/squeeze/value"},   {squeeze_, "/user/hand/right/input/squeeze/value"},
        {aim_, "/user/hand/left/input/aim/pose"},            {aim_, "/user/hand/right/input/aim/pose"},
        {x_, "/user/hand/left/input/x/click"},               {y_, "/user/hand/left/input/y/click"},
        {menu_, "/user/hand/left/input/menu/click"},
        {a_, "/user/hand/right/input/a/click"},              {b_, "/user/hand/right/input/b/click"},
    };
    XrActionSuggestedBinding sb[sizeof table / sizeof table[0]];
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        sb[i].action = table[i].action;
        if (!ok(xrStringToPath(instance_, table[i].path, &sb[i].binding), "xrStringToPath(binding)", err)) return false;
    }
    XrInteractionProfileSuggestedBinding ps{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    if (!ok(xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller", &ps.interactionProfile),
            "xrStringToPath(touch_controller)", err))
        return false;
    ps.countSuggestedBindings = (uint32_t)(sizeof sb / sizeof sb[0]);
    ps.suggestedBindings = sb;
    if (!ok(xrSuggestInteractionProfileBindings(instance_, &ps), "xrSuggestInteractionProfileBindings", err)) return false;
#if !defined(__ANDROID__)
    // The desktop build: Monado's simulated (qwerty) controllers are Windows Mixed Reality motion controllers. Trackpad
    // click is A on the right hand and X on the left, the right menu button is B, the right thumbstick click is Y
    // (Start). Best effort.
    const Binding wmr[] = {
        {stick_, "/user/hand/left/input/thumbstick"},        {stick_, "/user/hand/right/input/thumbstick"},
        {stick_click_, "/user/hand/left/input/thumbstick/click"}, {stick_click_, "/user/hand/right/input/thumbstick/click"},
        {trigger_, "/user/hand/left/input/trigger/value"},   {trigger_, "/user/hand/right/input/trigger/value"},
        {squeeze_, "/user/hand/left/input/squeeze/click"},   {squeeze_, "/user/hand/right/input/squeeze/click"},
        {aim_, "/user/hand/left/input/aim/pose"},            {aim_, "/user/hand/right/input/aim/pose"},
        {x_, "/user/hand/left/input/trackpad/click"},        {menu_, "/user/hand/left/input/menu/click"},
        {a_, "/user/hand/right/input/trackpad/click"},       {b_, "/user/hand/right/input/menu/click"},
        {y_, "/user/hand/right/input/thumbstick/click"},
    };
    XrActionSuggestedBinding wb[sizeof wmr / sizeof wmr[0]];
    bool paths_ok = true;
    for (size_t i = 0; i < sizeof wmr / sizeof wmr[0]; i++) {
        wb[i].action = wmr[i].action;
        paths_ok = paths_ok && ok(xrStringToPath(instance_, wmr[i].path, &wb[i].binding), "xrStringToPath(wmr binding)");
    }
    XrInteractionProfileSuggestedBinding wp{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    if (paths_ok && ok(xrStringToPath(instance_, "/interaction_profiles/microsoft/motion_controller", &wp.interactionProfile),
                       "xrStringToPath(motion_controller)")) {
        wp.countSuggestedBindings = (uint32_t)(sizeof wb / sizeof wb[0]);
        wp.suggestedBindings = wb;
        ok(xrSuggestInteractionProfileBindings(instance_, &wp), "xrSuggestInteractionProfileBindings(motion_controller)");
    }
#endif

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = aim_;
        ci.subactionPath = hand_[h];
        ci.poseInActionSpace.orientation.w = 1.f;
        if (!ok(xrCreateActionSpace(session_, &ci, &aim_space_[h]), "xrCreateActionSpace(aim)", err)) return false;
    }
    return true;
}

void QxInput::attach() {
    if (attached_ || set_ == XR_NULL_HANDLE || session_ == XR_NULL_HANDLE) return;
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &set_;
    attached_ = ok(xrAttachSessionActionSets(session_, &ai), "xrAttachSessionActionSets");
    vos_log("AR_INPUT profile=touch attached=%d", attached_ ? 1 : 0);
}

void QxInput::clear(XrTime t, QxActionState *out) {
    *out = QxActionState{};
    out->left_aim.quat[3] = out->right_aim.quat[3] = 1.f;
    out->time_ns = (int64_t)t;
    publish(*out);
}

void QxInput::sync(XrSpace app_space, XrTime t, QxActionState *out) {
    if (!attached_) { clear(t, out); return; }
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    const XrResult r = xrSyncActions(session_, &si);
    // XR_SESSION_NOT_FOCUSED is a success code: the action states are all inactive then, so it reads as no input.
    if (r == XR_SESSION_NOT_FOCUSED || !ok(r, "xrSyncActions")) { clear(t, out); return; }

    QxActionState s{};
    s.left_aim.quat[3] = s.right_aim.quat[3] = 1.f;
    s.time_ns = (int64_t)t;
    bool hand_active[2] = {false, false};
    auto boolean = [&](XrAction a, XrPath sub, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (!ok(xrGetActionStateBoolean(session_, &gi, &st), "xrGetActionStateBoolean") || !st.isActive) return false;
        hand_active[hand] = true;
        return st.currentState == XR_TRUE;
    };
    auto value = [&](XrAction a, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = hand_[hand];
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        if (!ok(xrGetActionStateFloat(session_, &gi, &st), "xrGetActionStateFloat") || !st.isActive) return 0.f;
        hand_active[hand] = true;
        return st.currentState;
    };
    auto stick = [&](int hand, float *x, float *y) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = stick_;
        gi.subactionPath = hand_[hand];
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        *x = *y = 0.f;
        if (!ok(xrGetActionStateVector2f(session_, &gi, &st), "xrGetActionStateVector2f") || !st.isActive) return;
        hand_active[hand] = true;
        *x = st.currentState.x;
        *y = st.currentState.y;
    };
    auto aim = [&](int hand, ar_pose *pose) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = aim_;
        gi.subactionPath = hand_[hand];
        XrActionStatePose st{XR_TYPE_ACTION_STATE_POSE};
        if (!ok(xrGetActionStatePose(session_, &gi, &st), "xrGetActionStatePose") || !st.isActive) return false;
        hand_active[hand] = true;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (!ok(xrLocateSpace(aim_space_[hand], app_space, t, &loc), "xrLocateSpace(aim)")) return false;
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) != need) return false;
        *pose = qx_ar_pose(loc.pose);
        return true;
    };
    stick(kLeft, &s.lx, &s.ly);
    stick(kRight, &s.rx, &s.ry);
    s.lstick = boolean(stick_click_, hand_[kLeft], kLeft);
    s.rstick = boolean(stick_click_, hand_[kRight], kRight);
    s.lt = value(trigger_, kLeft);
    s.rt = value(trigger_, kRight);
    s.lg = value(squeeze_, kLeft);
    s.rg = value(squeeze_, kRight);
    s.x = boolean(x_, XR_NULL_PATH, kLeft);
    s.y = boolean(y_, XR_NULL_PATH, kLeft);
    s.menu = boolean(menu_, XR_NULL_PATH, kLeft);
    s.a = boolean(a_, XR_NULL_PATH, kRight);
    s.b = boolean(b_, XR_NULL_PATH, kRight);
    s.left_aim_valid = aim(kLeft, &s.left_aim);
    s.right_aim_valid = aim(kRight, &s.right_aim);
    s.left_active = hand_active[kLeft];
    s.right_active = hand_active[kRight];
    *out = s;
    publish(s);
}

void QxInput::publish(const QxActionState &s) {
    const ar_pad p = qx_pad_from_actions(s);
    std::lock_guard<std::mutex> g(m_);
    pad_ = p;
    active_ = s.left_active || s.right_active;
}

int QxInput::get_pad(ar_pad *out) {
    std::lock_guard<std::mutex> g(m_);
    *out = pad_;
    gate_.filter(out);
    return active_ ? 1 : 0;
}

void QxInput::hold_until_released(uint32_t mask) {
    std::lock_guard<std::mutex> g(m_);
    gate_.arm(mask);
}

void QxInput::destroy() {
    for (XrSpace &sp : aim_space_) {
        if (sp != XR_NULL_HANDLE) xrDestroySpace(sp);
        sp = XR_NULL_HANDLE;
    }
    if (set_ != XR_NULL_HANDLE) xrDestroyActionSet(set_);   // destroys its actions too
    set_ = XR_NULL_HANDLE;
    stick_ = stick_click_ = trigger_ = squeeze_ = aim_ = a_ = b_ = x_ = y_ = menu_ = XR_NULL_HANDLE;
    attached_ = false;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
}

}  // namespace ar
