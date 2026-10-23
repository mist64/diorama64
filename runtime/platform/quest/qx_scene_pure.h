// The room-scan table's portable half (host-tested in ar_qx_logic): a Scene API plane anchor turned into a +Y-up table,
// the nearest-table chooser, the level's placement on the table (an offset in table axes and a yaw), the grip drag
// step and the PlacementRecord mapping. No OpenXR calls here (only its plain structs): qx_scene.h (ar_quest only) adds
// QxScene, which queries the anchors, and qx_placer.h the per-frame placement state.
#pragma once
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include "vos_placement.h"
extern "C" {
#include "ar_math.h"
}

namespace ar {

struct QxSceneTable { std::string uuid; ar_pose tracking_from_table; float half_u, half_v; };   // +Y up, origin the plane centre
// Pure. The Scene API gives a plane anchor whose +Z (or -Z) is the surface normal and an XrRect2Df in the plane's XY; this
// turns it into a +Y-up table pose at the rect's centre with the rect's half sizes. The yaw is chosen so table +X is the
// rect's longer side; half_u / half_v are the half sizes along table +X / +Z.
QxSceneTable qx_table_from_scene(const std::string &uuid, const XrPosef &anchor, const XrRect2Df &rect);
// Pure. Index of the table nearest the head among those at least kMinTableAboveFloorM above floor_y (floor_known), else -1.
int qx_choose_table(const std::vector<QxSceneTable> &tables, const ar_pose &head, bool floor_known, float floor_y);
struct QxPlacement { std::string uuid; float offset_u = 0.f, offset_v = 0.f; float yaw_deg = 0.f; };
// tracking_from_level_table: the table pose, then the offset (offset_u, 0, offset_v) in table axes, then yaw_deg about +Y.
ar_pose qx_place(const QxSceneTable &t, const QxPlacement &p);
// Pure drag step: the controller ray hits the table plane; the level follows the hit, keeping the grab offset (grab_uv:
// the hit's table coordinates minus the offset when the drag began), clamped to the rect. False (p untouched) when the
// ray is parallel to the plane or points away from it.
bool qx_drag_update(const QxSceneTable &t, const float ray_o[3], const float ray_d[3], const float grab_uv[2], QxPlacement *p);
// The table coordinates (along table +X, +Z) where the ray hits the table plane; false on a miss (as qx_drag_update).
bool qx_table_hit_uv(const QxSceneTable &t, const float ray_o[3], const float ray_d[3], float uv[2]);
float qx_drag_yaw_step(float rx, float dt_s);   // 90 deg/s at full deflection, dead zone 0.2
// PlacementRecord mapping (reuses vos_placement.cpp's JSON): anchor = uuid, yaw_deg = yaw,
// tracking_from_table.pos = (offset_u, 0, offset_v), game = "quest" (the record belongs to the table, not to a game).
PlacementRecord qx_record_from(const QxPlacement &p);
bool qx_placement_from(const PlacementRecord &r, QxPlacement *out);   // false for an empty or "fixed" anchor
// The anchor's XrUuidEXT as 8-4-4-4-12 lowercase hex (vos_placement.cpp's anchor format).
std::string qx_uuid_string(const uint8_t data[16]);
// True when the Scene API's comma-separated label list holds label as a whole entry.
bool qx_has_label(const std::string &labels, const char *label);

}  // namespace ar
