// The Quest session's lighting (QxSession::update_lighting), portable so the host tests run it. The Quest has no
// environment probe (no has_environment frames, no cube): the key light is always the fixed one.
#pragma once
extern "C" {
#include "ar_runtime.h"
}
#include "lighting.h"
#include "look.h"
#include "room_light_driver.h"
namespace ar {
// The look the Quest lights with: source forced to fixed (LookSettings::source 1), whatever the store says.
LookSettings qx_fixed_look(const LookSettings &look);
// One frame of the session's lighting through its driver (the key eases towards a changed key_az / key_el, the
// key-light callback fires on a change): RoomLightDriver::step with no environment, source fixed, no reflections.
LightingFrame qx_lighting_step(RoomLightDriver &driver, const LookSettings &look, const ar_render_frame &rf);
// Pure: the lighting a fresh session's first frame gets (the fixed key at key_az / key_el, unit).
LightingFrame qx_lighting_frame(const LookSettings &look, const ar_render_frame &rf);
}
