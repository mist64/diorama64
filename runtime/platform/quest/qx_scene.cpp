#include "qx_scene.h"
#include <stdio.h>
#include <string.h>
#include <type_traits>
#include "vos_log.h"

namespace ar {

bool QxScene::ok(XrResult r, const char *step) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE];
    if (instance_ == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance_, r, name))) snprintf(name, sizeof name, "%d", (int)r);
    const std::string line = std::string(step) + " failed: " + name;
    if (line != last_error_) {   // a call failing every frame logs once
        vos_log("AR_QUEST_SCENE %s", line.c_str());
        last_error_ = line;
    }
    return false;
}

bool QxScene::start(XrInstance instance, XrSession session, std::string *err) {
    instance_ = instance;
    auto load = [&](const char *name, auto *out) {
        PFN_xrVoidFunction fn = nullptr;
        if (!ok(xrGetInstanceProcAddr(instance, name, &fn), name) || !fn) {
            if (err) *err = std::string(name) + " unavailable";
            return false;
        }
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(fn);
        return true;
    };
    if (!load("xrQuerySpacesFB", &query_spaces_) || !load("xrRetrieveSpaceQueryResultsFB", &retrieve_results_) ||
        !load("xrGetSpaceComponentStatusFB", &get_component_status_) ||
        !load("xrSetSpaceComponentStatusFB", &set_component_status_) || !load("xrGetSpaceSemanticLabelsFB", &get_labels_) ||
        !load("xrGetSpaceBoundingBox2DFB", &get_bbox2d_)) {
        instance_ = XR_NULL_HANDLE;
        return false;
    }
    // Optional: without XR_FB_scene_capture the scan cannot be started from here (the user runs Space Setup).
    PFN_xrVoidFunction fn = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrRequestSceneCaptureFB", &fn)) && fn)
        request_capture_ = reinterpret_cast<PFN_xrRequestSceneCaptureFB>(fn);
    uint32_t n = 0;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) {
        std::vector<XrExtensionProperties> exts(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data())))
            for (uint32_t i = 0; i < n; i++)
                if (strcmp(exts[i].extensionName, XR_FB_SCENE_EXTENSION_NAME) == 0) scene_version_ = exts[i].extensionVersion;
    }
    session_ = session;
    vos_log("AR_QUEST_SCENE started scene_version=%u capture=%d", scene_version_, request_capture_ ? 1 : 0);
    return true;
}

void QxScene::request_query() { query_wanted_ = true; }

void QxScene::request_scene_capture() { capture_wanted_ = true; }

void QxScene::destroy_spaces(std::vector<XrSpaceQueryResultFB> *v) {
    for (auto &r : *v)
        if (r.space != XR_NULL_HANDLE) xrDestroySpace(r.space);
    v->clear();
}

void QxScene::destroy() {
    destroy_spaces(&pending_);
    for (auto &a : anchors_)
        if (a.space != XR_NULL_HANDLE) xrDestroySpace(a.space);
    anchors_.clear();
    tables_.clear();
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    query_inflight_ = capture_inflight_ = false;
}

// XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB: the two-call idiom; may arrive more than once per query.
void QxScene::retrieve(XrAsyncRequestIdFB id) {
    XrSpaceQueryResultsFB res{XR_TYPE_SPACE_QUERY_RESULTS_FB};
    if (!ok(retrieve_results_(session_, id, &res), "xrRetrieveSpaceQueryResultsFB")) return;
    std::vector<XrSpaceQueryResultFB> got(res.resultCountOutput);
    res.resultCapacityInput = (uint32_t)got.size();
    res.results = got.data();
    if (!got.empty() && !ok(retrieve_results_(session_, id, &res), "xrRetrieveSpaceQueryResultsFB")) return;
    got.resize(res.resultCountOutput);
    pending_.insert(pending_.end(), got.begin(), got.end());
}

// Labels and the 2D bound of one result: TABLE (with a bound) or FLOOR, else not kept.
void QxScene::classify(Anchor *a) {
    XrSemanticLabelsFB labels{XR_TYPE_SEMANTIC_LABELS_FB};
    XrSemanticLabelsSupportInfoFB support{XR_TYPE_SEMANTIC_LABELS_SUPPORT_INFO_FB};
    support.flags = XR_SEMANTIC_LABELS_SUPPORT_MULTIPLE_SEMANTIC_LABELS_BIT_FB;
    if (scene_version_ >= 3) support.flags |= XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_DESK_TO_TABLE_MIGRATION_BIT_FB;
    support.recognizedLabels = "TABLE,FLOOR";
    if (scene_version_ >= 2) labels.next = &support;
    if (!ok(get_labels_(session_, a->space, &labels), "xrGetSpaceSemanticLabelsFB")) return;
    std::string text(labels.bufferCountOutput, '\0');
    labels.bufferCapacityInput = (uint32_t)text.size();
    labels.buffer = text.empty() ? nullptr : &text[0];
    if (!text.empty() && !ok(get_labels_(session_, a->space, &labels), "xrGetSpaceSemanticLabelsFB")) return;
    text.resize(strnlen(text.c_str(), text.size()));
    a->is_floor = qx_has_label(text, "FLOOR");
    a->is_table = qx_has_label(text, "TABLE") || qx_has_label(text, "DESK");
    if (!a->is_table) return;
    XrSpaceComponentStatusFB st{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
    if (!ok(get_component_status_(a->space, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB, &st), "xrGetSpaceComponentStatusFB(BOUNDED_2D)") ||
        !st.enabled || !ok(get_bbox2d_(session_, a->space, &a->rect), "xrGetSpaceBoundingBox2DFB"))
        a->is_table = false;   // a table without its rectangle cannot hold the level
}

// LOCATABLE must be enabled before xrLocateSpace: scene anchors normally come with it on; otherwise ask once.
void QxScene::check_locatable(Anchor *a) {
    if (a->locatable || a->enabling) return;
    XrSpaceComponentStatusFB st{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
    if (!ok(get_component_status_(a->space, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, &st), "xrGetSpaceComponentStatusFB(LOCATABLE)"))
        return;
    if (st.enabled) { a->locatable = true; return; }
    if (st.changePending) return;
    XrSpaceComponentStatusSetInfoFB si{XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB};
    si.componentType = XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB;
    si.enabled = XR_TRUE;
    si.timeout = 0;
    XrAsyncRequestIdFB id = 0;
    const XrResult r = set_component_status_(a->space, &si, &id);
    if (r == XR_ERROR_SPACE_COMPONENT_STATUS_ALREADY_SET_FB) { a->locatable = true; return; }
    if (ok(r, "xrSetSpaceComponentStatusFB(LOCATABLE)")) a->enabling = true;
}

// XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB: the query's results replace the last ones.
void QxScene::adopt_results() {
    for (auto &a : anchors_)
        if (a.space != XR_NULL_HANDLE) xrDestroySpace(a.space);
    anchors_.clear();
    int seen = 0;
    for (auto &r : pending_) {
        seen++;
        Anchor a;
        a.space = r.space;
        a.uuid = qx_uuid_string(r.uuid.data);
        classify(&a);
        if (a.is_table || a.is_floor) {
            anchors_.push_back(a);
        } else {
            xrDestroySpace(r.space);
        }
        r.space = XR_NULL_HANDLE;
    }
    pending_.clear();
    int t = 0, f = 0;
    for (auto &a : anchors_) { t += a.is_table; f += a.is_floor; }
    vos_log("AR_QUEST_SCENE query=complete anchors=%d tables=%d floors=%d", seen, t, f);
}

bool QxScene::handle_event(const XrEventDataBuffer &ev) {
    switch (ev.type) {
    case XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB: {
        const auto *e = reinterpret_cast<const XrEventDataSpaceQueryResultsAvailableFB *>(&ev);
        if (session_ != XR_NULL_HANDLE && query_inflight_ && e->requestId == query_id_) retrieve(e->requestId);
        return true;
    }
    case XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB: {
        const auto *e = reinterpret_cast<const XrEventDataSpaceQueryCompleteFB *>(&ev);
        if (session_ == XR_NULL_HANDLE || !query_inflight_ || e->requestId != query_id_) return true;
        query_inflight_ = false;
        if (XR_SUCCEEDED(e->result)) {
            adopt_results();
            published_ = complete_ = false;   // the new anchors' list waits until they are located
            completed_at_ = 0;
        } else {   // the last scan's anchors stay; the hint reports no table when there is none
            ok(e->result, "xrQuerySpacesFB (complete)");
            destroy_spaces(&pending_);
        }
        queried_ = true;
        return true;
    }
    case XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB: {
        const auto *e = reinterpret_cast<const XrEventDataSpaceSetStatusCompleteFB *>(&ev);
        for (auto &a : anchors_)
            if (a.space == e->space && e->componentType == XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB) {
                a.enabling = false;
                a.locatable = XR_SUCCEEDED(e->result) && e->enabled;
                if (!a.locatable) ok(e->result, "xrSetSpaceComponentStatusFB (complete)");
            }
        return true;
    }
    case XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB: {
        const auto *e = reinterpret_cast<const XrEventDataSceneCaptureCompleteFB *>(&ev);
        if (!capture_inflight_ || e->requestId != capture_id_) return true;
        capture_inflight_ = false;
        vos_log("AR_QUEST_SCENE capture=complete result=%d", (int)e->result);
        query_wanted_ = true;   // whatever Space Setup changed (or not), read the scan again
        return true;
    }
    default: return false;
    }
}

bool QxScene::poll(XrSpace app_space, XrTime t) {
    if (session_ == XR_NULL_HANDLE) return false;
    if (capture_wanted_ && !capture_inflight_) {
        capture_wanted_ = false;
        if (!request_capture_) {
            vos_log("AR_QUEST_SCENE capture unavailable: runtime lacks %s", XR_FB_SCENE_CAPTURE_EXTENSION_NAME);
        } else {
            XrSceneCaptureRequestInfoFB ci{XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB};
            if (ok(request_capture_(session_, &ci, &capture_id_), "xrRequestSceneCaptureFB")) {
                capture_inflight_ = true;
                vos_log("AR_QUEST_SCENE capture=requested");
            }
        }
    }
    if (query_wanted_ && !query_inflight_) {
        query_wanted_ = false;
        destroy_spaces(&pending_);
        XrSpaceStorageLocationFilterInfoFB where{XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB};
        where.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
        XrSpaceComponentFilterInfoFB which{XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB};
        which.next = &where;
        which.componentType = XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB;
        XrSpaceQueryInfoFB qi{XR_TYPE_SPACE_QUERY_INFO_FB};
        qi.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB;
        qi.maxResultCount = 256;
        qi.timeout = 0;
        qi.filter = reinterpret_cast<const XrSpaceFilterInfoBaseHeaderFB *>(&which);
        qi.excludeFilter = nullptr;
        if (ok(query_spaces_(session_, reinterpret_cast<const XrSpaceQueryInfoBaseHeaderFB *>(&qi), &query_id_), "xrQuerySpacesFB")) {
            query_inflight_ = true;
            vos_log("AR_QUEST_SCENE query=requested");
        }
    }

    std::vector<QxSceneTable> located;
    bool floor = false, all = true;
    float fy = 0.f;
    for (auto &a : anchors_) {
        check_locatable(&a);
        if (a.locatable) {
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            if (ok(xrLocateSpace(a.space, app_space, t, &loc), "xrLocateSpace(anchor)") &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                a.pose = loc.pose;
                a.located = true;
            }
        }
        if (!a.located) {   // a lost anchor keeps its last pose once it was located
            if (a.is_table) all = false;
            continue;
        }
        if (a.is_floor && (!floor || a.pose.position.y < fy)) { floor = true; fy = a.pose.position.y; }
        if (a.is_table) located.push_back(qx_table_from_scene(a.uuid, a.pose, a.rect));
    }
    constexpr XrTime kPublishTimeout = 2000000000;   // 2 s
    if (queried_ && !published_) {
        if (completed_at_ == 0) completed_at_ = t;
        if (all || t - completed_at_ >= kPublishTimeout) {
            published_ = true;
            vos_log("AR_QUEST_SCENE list=published tables=%d complete=%d", (int)located.size(), all ? 1 : 0);
        }
    }
    if (published_ || !queried_) tables_ = std::move(located);   // before any query: empty
    complete_ = published_ && all;
    floor_known_ = floor;
    floor_y_ = fy;
    std::string uuids;
    for (auto &tb : tables_) uuids += tb.uuid + ",";
    const bool changed = uuids != last_uuids_ || floor != last_floor_;
    last_uuids_ = uuids;
    last_floor_ = floor;
    return changed;
}

}  // namespace ar
