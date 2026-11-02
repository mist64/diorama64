#include "qx_input_pure.h"

namespace ar {

namespace {
constexpr float kPressed = 0.5f;
bool rose(float now, float before) { return now > kPressed && !(before > kPressed); }
}  // namespace

ar_pad qx_pad_from_actions(const QxActionState &s) {
    ar_pad p{};
    p.lx = s.lx; p.ly = s.ly; p.rx = s.rx; p.ry = s.ry;
    p.lt = s.lt; p.rt = s.rt;
    if (s.a) p.buttons |= AR_BTN_A;
    if (s.b) p.buttons |= AR_BTN_B;
    if (s.x) p.buttons |= AR_BTN_X;
    if (s.y) p.buttons |= AR_BTN_START;
    if (s.lt > kPressed) p.buttons |= AR_BTN_LB;
    if (s.rt > kPressed) p.buttons |= AR_BTN_RB;
    if (s.lstick) p.buttons |= AR_BTN_LSTICK;
    return p;
}

PointerDecision qx_route_pointer(PointerOwner prev, const QxActionState &now, const QxActionState &before) {
    PointerDecision d{prev, false, false, false};
    const bool grip_held = now.lg > kPressed || now.rg > kPressed;
    const bool grip_rose = rose(now.lg, before.lg) || rose(now.rg, before.rg);
    switch (prev) {
    case PointerOwner::placer:
        if (!grip_held) { d.owner = PointerOwner::none; d.end = true; }
        break;
    case PointerOwner::game:
        if (grip_rose) { d.owner = PointerOwner::placer; d.cancel = true; d.begin = true; }
        else if (!(now.rt > kPressed)) { d.owner = PointerOwner::none; d.end = true; }
        break;
    case PointerOwner::none:
        if (grip_rose) { d.owner = PointerOwner::placer; d.begin = true; }
        else if (rose(now.rt, before.rt) && now.right_aim_valid) { d.owner = PointerOwner::game; d.begin = true; }
        break;
    }
    return d;
}

}  // namespace ar
