#include <android_native_app_glue.h>
#include <android/looper.h>
#include <string>
#include "vos_log.h"

static void on_cmd(android_app *, int32_t cmd) {
    ar::vos_log("APP_CMD_%d", (int)cmd);
}

extern "C" void android_main(android_app *app) {
    ar::vos_log_open((std::string(app->activity->internalDataPath) + "/sm64ar.log").c_str());
    ar::vos_log("AR_QUEST_START");
    app->onAppCmd = on_cmd;
    while (!app->destroyRequested) {
        android_poll_source *src = nullptr;
        int events = 0;
        if (ALooper_pollOnce(-1, nullptr, &events, (void **)&src) >= 0 && src)
            src->process(app, src);
    }
    ar::vos_log("AR_QUEST_EXIT");
}
