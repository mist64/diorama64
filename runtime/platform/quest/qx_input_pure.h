// The Touch controllers' portable half (host-tested in ar_qx_logic): one synced controller sample, the game pad it
// makes and the router that gives the right aim ray to the game pointer or the grip-drag placer. No OpenXR here:
// qx_input.h (ar_quest only) adds QxInput, which fills QxActionState from the OpenXR action set.
#pragma once
#include <stdint.h>
extern "C" {
#include "ar_types.h"
}

namespace ar {

struct QxActionState {   // one controller sample, synced once per wait_frame
    bool left_active, right_active;
    float lx, ly, rx, ry;             // thumbsticks, +y up
    bool a, b, x, y, menu;            // buttons (menu = left controller's menu button)
    bool lstick, rstick;              // thumbstick clicks
    float lt, rt, lg, rg;             // index triggers and grips, 0..1
    bool left_aim_valid, right_aim_valid; ar_pose left_aim, right_aim;   // aim poses in tracking space
    int64_t time_ns;
};

// lx/ly -> lx/ly; rx/ry -> rx/ry; a->AR_BTN_A, b->AR_BTN_B, x->AR_BTN_X, y->AR_BTN_START, lt->lt (and AR_BTN_LB when
// > 0.5), rt->rt (and AR_BTN_RB when > 0.5), left stick click -> AR_BTN_LSTICK. Raw stick values: dead zones are the
// adapter's.
ar_pad qx_pad_from_actions(const QxActionState &s);

enum class PointerOwner { none, game, placer };
// begin: the new owner's gesture begins; end: the previous owner's gesture ends normally; cancel: the game pointer is
// taken by the placer.
struct PointerDecision { PointerOwner owner; bool begin, end, cancel; };
// Trigger (> 0.5, pressed this sample) begins a game pointer on the right controller when its aim is valid; a grip
// (> 0.5, pressed this sample) on either controller begins a placer drag and cancels any game pointer; while a drag is
// active the trigger is ignored. The drag ends when both grips are released, the game pointer when the trigger is;
// nothing begins on the sample a drag ends. Pure: previous owner in, decision out.
PointerDecision qx_route_pointer(PointerOwner prev, const QxActionState &now, const QxActionState &before);

}  // namespace ar
