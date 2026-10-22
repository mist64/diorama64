#include <android_native_app_glue.h>
#include <android/looper.h>
#include <time.h>
#include <string>
#include "cube_adapter.h"
#include "qx_platform.h"
#include "qx_session.h"
#include "vos_log.h"

namespace {

double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void on_cmd(android_app *app, int32_t cmd) {
    ar::vos_log("APP_CMD_%d", (int)cmd);
    if (app->userData) static_cast<ar::QxPlatform *>(app->userData)->handle_cmd(cmd);
}

void pump(android_app *app, int timeout_ms) {
    for (;;) {
        android_poll_source *src = nullptr;
        int events = 0;
        if (ALooper_pollOnce(timeout_ms, nullptr, &events, (void **)&src) < 0) break;
        if (src) src->process(app, src);
        if (timeout_ms != 0) break;
    }
}

}  // namespace

extern "C" void android_main(android_app *app) {
    const std::string internal = app->activity->internalDataPath;
    ar::vos_log_open((internal + "/sm64ar.log").c_str());
    ar::vos_log("AR_QUEST_START");
    app->onAppCmd = on_cmd;

    ar::QxPlatform::Config cfg;
    cfg.fixed_table = ar_pose_identity();
    cfg.fixed_table.pos[1] = 0.75f;   // table height above the floor; in LOCAL space the platform moves it 1.5 m down
    cfg.fixed_table.pos[2] = -1.0f;
    cfg.fixed_table_valid = true;
    ar::QxPlatform platform(app, cfg);
    app->userData = &platform;
    ar::QxSession session(cube_adapter(), platform, internal);

    // running: the platform frame loop runs; game: the session drives it (else passthrough only, after a failed start).
    std::string err;
    bool running = platform.start(&err);
    if (!running) ar::vos_log("AR_QUEST_FAIL %s", err.c_str());
    bool game = running && session.start("", internal + "/save", &err);
    if (running && !game) ar::vos_log("AR_QUEST_FAIL %s", err.c_str());

    const ar_platform *api = platform.api();
    double last_head_log = 0.0;
    while (!app->destroyRequested) {
        pump(app, running ? 0 : -1);   // a stopped loop keeps the activity (and its log) alive, waiting on events
        if (app->destroyRequested || !running) continue;
        bool more;
        if (game) {
            more = !platform.exiting() && session.frame();
        } else {   // no game: keep the XR frame loop going so the passthrough layer is shown
            ar_frame_info fi;
            more = !platform.exiting() && api->wait_frame(api->user, &fi);
            if (more) {
                api->begin_frame(api->user);
                platform.end_submission();
            }
        }
        if (!more) {   // the session is exiting: finish the activity, then wait for APP_CMD_DESTROY
            ar::vos_log("AR_QUEST_STOP ticks=%llu", (unsigned long long)session.ticks_done());
            session.stop();
            game = false;
            running = false;
            ANativeActivity_finish(app->activity);
            continue;
        }
        const ar_frame_info &fi = platform.last_frame();
        if (fi.head_tracked && now_s() - last_head_log >= 1.0) {
            last_head_log = now_s();
            ar::vos_log("AR_QUEST_HEAD %.3f %.3f %.3f", fi.tracking_from_head.pos[0], fi.tracking_from_head.pos[1],
                        fi.tracking_from_head.pos[2]);
        }
    }
    const unsigned long long ticks = session.ticks_done();
    session.stop();   // before the platform (its Vulkan device and XR session) is destroyed
    app->userData = nullptr;
    ar::vos_log("AR_QUEST_EXIT ticks=%llu", ticks);
}
