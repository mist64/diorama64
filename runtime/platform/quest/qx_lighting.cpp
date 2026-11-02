// runtime/platform/quest/qx_lighting.cpp — the visionOS session's update_lighting (RoomLightDriver::begin) without the
// device's environment reduction: the Quest never has a probe, so the driver runs in its fixed-light mode.
#include "qx_lighting.h"
namespace ar {
LookSettings qx_fixed_look(const LookSettings &look) {
    LookSettings l = look;
    l.source = 1;   // "fixed" (look.cpp: LOOK_PICK(source, "room", "fixed"))
    return l;
}
LightingFrame qx_lighting_step(RoomLightDriver &driver, const LookSettings &look, const ar_render_frame &rf) {
    LightingFrame lf = driver.step(qx_fixed_look(look), rf, nullptr, 0);
    lf.reflections = false;   // nothing to reflect: no environment cube
    return lf;
}
LightingFrame qx_lighting_frame(const LookSettings &look, const ar_render_frame &rf) {
    RoomLightDriver driver;
    return qx_lighting_step(driver, look, rf);
}
}
