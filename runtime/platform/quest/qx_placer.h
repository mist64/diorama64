// Where the level stands on the room scan's tables (ar_quest; render thread): the chosen table (by uuid), the offset and
// yaw on it, the grip drag and the record at files_dir/placement.json (vos_placement.cpp's JSON through
// qx_record_from / qx_placement_from). QxPlatform runs update() once per wait_frame, after the controllers are synced.
#pragma once
#include <string>
#include <vector>
#include "ar_platform.h"
#include "qx_input_pure.h"
#include "qx_scene_pure.h"

namespace ar {

class QxPlacer {
public:
    explicit QxPlacer(const std::string &record_path);   // loads the record (a missing or bad one: nothing remembered)
    // The scan's tables this frame, the floor, the head and the controllers, with the router's decision for this sample.
    // A table-list change uses the remembered table when it is in the list, else the nearest (offset reset); that
    // fallback is saved only when the list is complete (set_scene_status), so a table not yet located is never
    // forgotten on disk, and it comes back when a later list holds it. A placer BEGIN starts a drag: the grabbing hand's aim ray then moves the level over the table (clamped to
    // its rect) and the right stick turns it; the drag ends when the decision's owner is no longer the placer, and the
    // placement is saved.
    void update(const std::vector<QxSceneTable> &tables, bool floor_known, float floor_y, const ar_pose &head,
                const QxActionState &now, const QxActionState &before, PointerDecision d, float dt_s);
    bool valid() const { return chosen_ >= 0; }
    ar_pose tracking_from_table() const;               // the level's table pose (qx_place); identity when not valid
    void half_extent(float out[2]) const { out[0] = out[1] = 0.f; }   // unknown: the level's own extent is used
    // The nearest table, offset reset (now, or at the next table list); the current table stays when none qualifies.
    void replace();
    // DRAGGING while a grip drag runs, at the level's pose (qx_place): the shared renderer draws the level's footprint
    // there, as visionOS's drag. No outlines (the DRAGGING state does not draw them).
    bool preview(ar_preview *out) const;
    // "" or why there is no table: the permission hint, or the scan (once queried) has no table.
    std::string hint() const;
    // queried: a scan query completed. list_complete: every table anchor of that scan is located (QxScene::tables_complete).
    // perm_hint: "" or why the scene permission is missing.
    void set_scene_status(bool queried, bool list_complete, const std::string &perm_hint) {
        queried_ = queried; complete_ = list_complete; perm_hint_ = perm_hint;
    }
    bool dragging() const { return dragging_; }
    const QxPlacement &placement() const { return place_; }

private:
    void choose_nearest(const ar_pose &head, bool reset);
    void save();
    void log_choice() const;

    std::string path_;
    QxPlacement place_;                 // the placement in use (place_.uuid: the chosen table)
    QxPlacement record_;                // what placement.json holds (record_.uuid empty: nothing remembered)
    std::vector<QxSceneTable> tables_;
    bool floor_known_ = false;
    float floor_y_ = 0.f;
    int chosen_ = -1;                   // index into tables_
    std::string uuids_;                 // tables_'s uuid list, to see a change
    bool replace_pending_ = false;
    ar_pose head_ = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 1.f}};
    bool dragging_ = false, grabbed_ = false, drag_right_ = true;
    float grab_uv_[2] = {0.f, 0.f};
    bool queried_ = false, complete_ = false, seen_scan_ = false, was_complete_ = false;
    std::string perm_hint_;
};

}  // namespace ar
