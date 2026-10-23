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
    // A table-list change keeps the remembered table when it is still in the list, else chooses the nearest (offset
    // reset). A placer BEGIN starts a drag: the grabbing hand's aim ray then moves the level over the table (clamped to
    // its rect) and the right stick turns it; the drag ends when the decision's owner is no longer the placer, and the
    // placement is saved.
    void update(const std::vector<QxSceneTable> &tables, bool floor_known, float floor_y, const ar_pose &head,
                const QxActionState &now, const QxActionState &before, PointerDecision d, float dt_s);
    bool valid() const { return chosen_ >= 0; }
    ar_pose tracking_from_table() const;               // the level's table pose (qx_place); identity when not valid
    void half_extent(float out[2]) const { out[0] = out[1] = 0.f; }   // unknown: the level's own extent is used
    void replace();                                    // the nearest table, offset reset (now, or at the next table list)
    bool preview(ar_preview *out) const;               // DRAGGING while a grip drag runs: the table rect's outline
    // "" or why there is no table: the scene permission denied, or the scan (once queried) has no table.
    std::string hint() const;
    void set_scene_status(bool queried, bool permission_denied) { queried_ = queried; denied_ = permission_denied; }
    bool dragging() const { return dragging_; }
    const QxPlacement &placement() const { return place_; }

private:
    void choose_nearest(const ar_pose &head, bool reset);
    void save();
    void log_choice() const;

    std::string path_;
    QxPlacement place_;                 // place_.uuid: the remembered (or chosen) table
    std::vector<QxSceneTable> tables_;
    bool floor_known_ = false;
    float floor_y_ = 0.f;
    int chosen_ = -1;                   // index into tables_
    std::string uuids_;                 // tables_'s uuid list, to see a change
    bool replace_pending_ = false;
    ar_pose head_ = {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 1.f}};
    bool dragging_ = false, grabbed_ = false, drag_right_ = true;
    float grab_uv_[2] = {0.f, 0.f};
    bool queried_ = false, denied_ = false, seen_scan_ = false;
};

}  // namespace ar
