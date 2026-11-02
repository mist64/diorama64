// The room scan through Meta's Scene API (ar_quest only): XR_FB_spatial_entity, _query, _storage, XR_FB_scene and
// XR_FB_scene_capture. A query loads every scene anchor with semantic labels; the TABLE ones (with a 2D bound) become
// QxSceneTables and the FLOOR gives the floor height. Every anchor is re-located each frame in the app space, so a
// recentre moves the tables with the room. Render thread only (QxPlatform's wait_frame and poll_events).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include "qx_scene_pure.h"

namespace ar {

class QxScene {
public:
    QxScene() = default;
    ~QxScene() { destroy(); }
    QxScene(const QxScene &) = delete;
    QxScene &operator=(const QxScene &) = delete;
    // The extension entry points (the instance must have enabled the four scene extensions; capture is optional).
    bool start(XrInstance, XrSession, std::string *err);
    // xrQuerySpacesFB for the local anchors with a SEMANTIC_LABELS component, issued by the next poll (one in flight).
    void request_query();
    // From QxPlatform::poll_events: true when the event was the scene's (query results / complete, component status,
    // capture complete). Each event is offered once.
    bool handle_event(const XrEventDataBuffer &ev);
    // Once per frame after xrWaitFrame (t = its predicted display time): issues a pending query or capture, enables
    // LOCATABLE where needed and locates the anchors in app_space. A completed query's table list is published only
    // once every TABLE anchor of it is located, or 2 s after the query completed (then what is located; stragglers join
    // as they locate); until then tables() keeps the previous list. True when the published table list (its uuids) or
    // the floor's presence changed.
    bool poll(XrSpace app_space, XrTime t);
    const std::vector<QxSceneTable> &tables() const { return tables_; }
    bool floor_known() const { return floor_known_; }
    float floor_y() const { return floor_y_; }
    bool queried() const { return queried_ && published_; }   // a query's list is published (the scan's, not "not yet")
    bool tables_complete() const { return complete_; }        // the published list holds every TABLE anchor of the scan
    bool started() const { return session_ != XR_NULL_HANDLE; }
    // xrRequestSceneCaptureFB (Space Setup), issued by the next poll; a completed capture re-queries.
    void request_scene_capture();
    void destroy();                                      // the anchor spaces; before the session goes

private:
    struct Anchor {
        XrSpace space = XR_NULL_HANDLE;
        std::string uuid;
        bool is_table = false, is_floor = false;
        XrRect2Df rect{};
        bool locatable = false;        // LOCATABLE enabled
        bool enabling = false;         // xrSetSpaceComponentStatusFB in flight
        bool located = false;          // located at least once
        XrPosef pose{};
    };
    bool ok(XrResult r, const char *step);
    void retrieve(XrAsyncRequestIdFB id);
    void adopt_results();
    void classify(Anchor *a);
    void check_locatable(Anchor *a);
    void destroy_spaces(std::vector<XrSpaceQueryResultFB> *v);

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    PFN_xrQuerySpacesFB query_spaces_ = nullptr;
    PFN_xrRetrieveSpaceQueryResultsFB retrieve_results_ = nullptr;
    PFN_xrGetSpaceComponentStatusFB get_component_status_ = nullptr;
    PFN_xrSetSpaceComponentStatusFB set_component_status_ = nullptr;
    PFN_xrGetSpaceSemanticLabelsFB get_labels_ = nullptr;
    PFN_xrGetSpaceBoundingBox2DFB get_bbox2d_ = nullptr;
    PFN_xrRequestSceneCaptureFB request_capture_ = nullptr;
    uint32_t scene_version_ = 1;   // XR_FB_scene's spec version (2 adds XrSemanticLabelsSupportInfoFB, 3 desk-to-table)

    bool query_wanted_ = false, query_inflight_ = false, capture_wanted_ = false, capture_inflight_ = false;
    XrAsyncRequestIdFB query_id_ = 0, capture_id_ = 0;
    std::vector<XrSpaceQueryResultFB> pending_;   // this query's results so far (spaces owned here until adopted)
    std::vector<Anchor> anchors_;                  // the last completed query's table and floor anchors
    std::vector<QxSceneTable> tables_;
    bool floor_known_ = false;
    float floor_y_ = 0.f;
    bool queried_ = false;
    bool published_ = false;                       // the last completed query's list is published
    bool complete_ = false;                        // ... and every table anchor of it is located
    XrTime completed_at_ = 0;                      // the first poll time after the query completed (0: not yet)
    std::string last_uuids_;                       // the uuid list poll last reported, to detect a change
    bool last_floor_ = false;
    std::string last_error_;
};

}  // namespace ar
