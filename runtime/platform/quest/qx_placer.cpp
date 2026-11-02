#include "qx_placer.h"
#include <cmath>
#include <string.h>
#include "vos_log.h"

namespace ar {

namespace {
constexpr float kPressed = 0.5f;
bool rose(float now, float before) { return now > kPressed && !(before > kPressed); }
// The OpenXR aim pose points down its -Z.
void aim_ray(const ar_pose &aim, float o[3], float d[3]) {
    const float fwd[3] = {0.f, 0.f, -1.f};
    for (int k = 0; k < 3; k++) o[k] = aim.pos[k];
    ar_quat_rotate(aim.quat, fwd, d);
}
float wrap_deg(float a) {
    while (a > 180.f) a -= 360.f;
    while (a <= -180.f) a += 360.f;
    return a;
}
}  // namespace

QxPlacer::QxPlacer(const std::string &record_path) : path_(record_path) {
    PlacementRecord r;
    std::string err;
    if (placement_load(path_, &r, &err) && qx_placement_from(r, &record_)) {
        place_ = record_;
        vos_log("AR_QUEST_PLACE record uuid=%s offset=(%.2f,%.2f) yaw=%.1f", place_.uuid.c_str(), (double)place_.offset_u,
                (double)place_.offset_v, (double)place_.yaw_deg);
    } else {
        vos_log("AR_QUEST_PLACE record none (%s)", err.empty() ? "not a scene table" : err.c_str());
    }
}

void QxPlacer::save() {
    std::string err;
    if (placement_save(path_, qx_record_from(place_), &err)) record_ = place_;
    else vos_log("AR_QUEST_PLACE save failed: %s", err.c_str());
}

void QxPlacer::log_choice() const {
    vos_log("AR_QUEST_SCENE tables=%d floor=%d chosen=%s", (int)tables_.size(), floor_known_ ? 1 : 0,
            chosen_ >= 0 ? tables_[chosen_].uuid.c_str() : "none");
}

// The nearest candidate, offset reset (the yaw is kept: it is the player's, not the table's). No candidate: chosen_
// is left as it was. Saved only when save_it (an explicit replace, or a complete list without the remembered table).
void QxPlacer::choose_nearest(const ar_pose &head, bool save_it) {
    const int i = qx_choose_table(tables_, head, floor_known_, floor_y_);
    if (i < 0) return;
    chosen_ = i;
    place_.uuid = tables_[i].uuid;
    place_.offset_u = place_.offset_v = 0.f;
    if (save_it) save();
}

void QxPlacer::replace() {
    vos_log("AR_QUEST_PLACE replace");
    if (tables_.empty()) { replace_pending_ = true; return; }
    dragging_ = grabbed_ = false;
    choose_nearest(head_, true);   // the player asked: saved even from an incomplete list
    log_choice();
}

void QxPlacer::update(const std::vector<QxSceneTable> &tables, bool floor_known, float floor_y, const ar_pose &head,
                      const QxActionState &now, const QxActionState &before, PointerDecision d, float dt_s) {
    head_ = head;
    floor_known_ = floor_known;
    floor_y_ = floor_y;
    std::string uuids;
    for (auto &t : tables) uuids += t.uuid + ",";
    tables_ = tables;   // every frame: the poses follow the re-located anchors
    const bool first_scan = queried_ && !seen_scan_;   // a scan with no table is a list too: log it once
    seen_scan_ = seen_scan_ || queried_;
    const bool became_complete = complete_ && !was_complete_;   // the list now confirms what it lacks
    was_complete_ = complete_;
    if (uuids != uuids_ || first_scan || became_complete) {
        uuids_ = uuids;
        const std::string current = chosen_ >= 0 ? place_.uuid : std::string();
        chosen_ = -1;
        int recorded = -1, kept = -1;
        for (size_t i = 0; i < tables_.size(); i++) {
            if (!record_.uuid.empty() && tables_[i].uuid == record_.uuid) recorded = (int)i;
            if (!current.empty() && tables_[i].uuid == current) kept = (int)i;
        }
        if (replace_pending_ && !tables_.empty()) {
            chosen_ = recorded >= 0 ? recorded : kept;   // stays when no table qualifies
            choose_nearest(head, true);
            replace_pending_ = false;
        } else if (recorded >= 0) {
            chosen_ = recorded;
            if (place_.uuid != record_.uuid) place_ = record_;   // the remembered table is (back) in the list
        } else if (kept >= 0) {
            chosen_ = kept;                                      // a fallback still in the list stays put
            if (complete_ && record_.uuid != place_.uuid) save();   // and the complete list confirms the old one is gone
        } else if (!tables_.empty()) {
            // The remembered table is absent: saved only when the list is complete (it is really gone).
            choose_nearest(head, complete_);
        }
        if (chosen_ < 0 && dragging_) dragging_ = grabbed_ = false;
        log_choice();
    }

    // The drag. Begin: the grabbing hand (the grip that rose this sample; the right one when both did).
    if (d.owner == PointerOwner::placer && d.begin && !dragging_) {
        dragging_ = true;
        grabbed_ = false;
        drag_right_ = rose(now.rg, before.rg) || !rose(now.lg, before.lg);
        vos_log("AR_QUEST_PLACE drag=begin offset=(%.2f,%.2f) yaw=%.1f", (double)place_.offset_u, (double)place_.offset_v,
                (double)place_.yaw_deg);
    }
    if (dragging_ && d.owner != PointerOwner::placer) {
        dragging_ = grabbed_ = false;
        if (chosen_ >= 0) save();
        vos_log("AR_QUEST_PLACE drag=end offset=(%.2f,%.2f) yaw=%.1f", (double)place_.offset_u, (double)place_.offset_v,
                (double)place_.yaw_deg);
        return;
    }
    if (!dragging_ || chosen_ < 0) return;
    const QxSceneTable &t = tables_[chosen_];
    const bool aim_valid = drag_right_ ? now.right_aim_valid : now.left_aim_valid;
    if (aim_valid) {
        float o[3], dir[3];
        aim_ray(drag_right_ ? now.right_aim : now.left_aim, o, dir);
        if (!grabbed_) {   // the first hit fixes where on the level the ray holds it
            float uv[2];
            if (qx_table_hit_uv(t, o, dir, uv)) {
                grab_uv_[0] = uv[0] - place_.offset_u;
                grab_uv_[1] = uv[1] - place_.offset_v;
                grabbed_ = true;
            }
        } else {
            qx_drag_update(t, o, dir, grab_uv_, &place_);
        }
    }
    // Right stick to the right turns the level clockwise seen from above (a negative turn about +Y).
    place_.yaw_deg = wrap_deg(place_.yaw_deg - qx_drag_yaw_step(now.rx, dt_s));
}

ar_pose QxPlacer::tracking_from_table() const {
    if (chosen_ < 0) return ar_pose_identity();
    return qx_place(tables_[chosen_], place_);
}

bool QxPlacer::preview(ar_preview *out) const {
    if (!dragging_ || chosen_ < 0) return false;
    memset(out, 0, sizeof *out);
    const QxSceneTable &t = tables_[chosen_];
    out->state = AR_PREVIEW_DRAGGING;
    out->on_plane = 1;
    out->outlines_only = 1;
    out->tracking_from_preview = qx_place(t, place_);   // the footprint follows the level (vos_platform.mm's drag)
    out->outline_count = 0;
    return true;
}

std::string QxPlacer::hint() const {
    if (!perm_hint_.empty()) return perm_hint_;
    if (queried_ && chosen_ < 0) return "No table in your room scan. Run Space Setup, then come back.";
    return "";
}

}  // namespace ar
