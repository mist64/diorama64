#include "check.h"
#include "qx_input_pure.h"

using namespace ar;

int main() {
    QxActionState s{}; s.lx = 0.5f; s.ly = -1.f; s.a = true; s.x = true; s.lt = 0.7f; s.rt = 0.2f; s.y = true;
    ar_pad p = qx_pad_from_actions(s);
    CHECK(p.lx == 0.5f && p.ly == -1.f && (p.buttons & AR_BTN_A) && (p.buttons & AR_BTN_X) && !(p.buttons & AR_BTN_B));
    CHECK((p.buttons & AR_BTN_LB) && !(p.buttons & AR_BTN_RB) && p.lt == 0.7f && (p.buttons & AR_BTN_START));
    QxActionState idle{}, trig{}; trig.rt = 1.f; trig.right_aim_valid = true;
    PointerDecision d = qx_route_pointer(PointerOwner::none, trig, idle);
    CHECK(d.owner == PointerOwner::game && d.begin);
    QxActionState grip = trig; grip.rg = 1.f;
    d = qx_route_pointer(PointerOwner::game, grip, trig);
    CHECK(d.owner == PointerOwner::placer && d.cancel && d.begin);
    d = qx_route_pointer(PointerOwner::placer, trig, grip);             // grip released, trigger still held: nothing begins
    CHECK(d.owner == PointerOwner::none && d.end && !d.begin);
    d = qx_route_pointer(PointerOwner::game, idle, trig);
    CHECK(d.owner == PointerOwner::none && d.end);
    return 0;
}
