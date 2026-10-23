#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/looper.h>
#include <jni.h>
#include <string>
#include <vector>
#include <chrono>
#include "bundled_copy.h"
#include "qx_app.h"
#include "qx_platform.h"
#include "vos_api.h"
#include "vos_log.h"

namespace {

void on_cmd(android_app *app, int32_t cmd) {
    ar::vos_log("APP_CMD_%d", (int)cmd);
    if (app->userData) static_cast<ar::QxPlatform *>(app->userData)->handle_cmd(cmd);   // set by the bridge
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
void pump_now(void *ctx) { pump(static_cast<android_app *>(ctx), 0); }

// The launch intent's extras as arvos_configure's argv (adb shell am start -S -n ... --es args "--game sm64 --look
// msaa=2 --start story", or plan 2's extras: --es game sm64 --es start story --es look msaa=2,shadows=0 --es audio
// head_focus=0.8,widen=1.5 --ez fixed_table true --ez replace true --ez scan true). The activity is singleTask and
// NativeActivity reads only the launch intent, so an extra applies to a fresh start (-S stops the running app first).
// Each extra becomes its flag (game → --game, start → --start, each look pair → --look k=v, each audio pair →
// --audio-set k=v, fixed_table → --fixed-table, replace → --replace, scan → --scan), then the words of "args" follow.
std::vector<std::string> read_launch_args(ANativeActivity *activity, std::string *note) {
    std::vector<std::string> out{"diorama64"};
    JNIEnv *env = nullptr;
    bool attached = false;   // detach only what this call attached
    if (activity->vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) {
        env = nullptr;
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
            *note = "no JNI env; no launch extras";
            return out;
        }
        attached = true;
    }
    auto failed = [&] {   // a missing method or a throwing call leaves an exception pending: clear it
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    };
    jobject intent = nullptr;
    jclass act_cls = env->GetObjectClass(activity->clazz);
    jmethodID get_intent = act_cls ? env->GetMethodID(act_cls, "getIntent", "()Landroid/content/Intent;") : nullptr;
    if (failed()) get_intent = nullptr;
    if (get_intent) {
        intent = env->CallObjectMethod(activity->clazz, get_intent);
        if (failed()) intent = nullptr;
    }
    std::vector<std::string> flags, words;
    if (intent) {
        jclass intent_cls = env->GetObjectClass(intent);
        jmethodID get_str = env->GetMethodID(intent_cls, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
        if (failed()) get_str = nullptr;
        jmethodID get_bool = env->GetMethodID(intent_cls, "getBooleanExtra", "(Ljava/lang/String;Z)Z");
        if (failed()) get_bool = nullptr;
        auto str_extra = [&](const char *key, std::string *dst) {
            if (!get_str) return false;
            jstring k = env->NewStringUTF(key);
            jstring v = (jstring)env->CallObjectMethod(intent, get_str, k);
            if (failed()) v = nullptr;
            bool have = false;
            if (v) {
                const char *c = env->GetStringUTFChars(v, nullptr);
                if (failed()) c = nullptr;
                if (c) { *dst = c; have = true; env->ReleaseStringUTFChars(v, c); }
                env->DeleteLocalRef(v);
            }
            env->DeleteLocalRef(k);
            return have;
        };
        auto bool_extra = [&](const char *key) {
            if (!get_bool) return false;
            jstring k = env->NewStringUTF(key);
            const bool v = env->CallBooleanMethod(intent, get_bool, k, (jboolean)JNI_FALSE) != JNI_FALSE;
            const bool ok = !failed();
            env->DeleteLocalRef(k);
            return ok && v;
        };
        auto split = [](const std::string &s, char sep, bool any_space) {
            std::vector<std::string> parts;
            std::string cur;
            for (char c : s) {
                const bool cut = any_space ? (c == ' ' || c == '\t' || c == '\n') : c == sep;
                if (!cut) { cur += c; continue; }
                if (!cur.empty()) parts.push_back(cur);
                cur.clear();
            }
            if (!cur.empty()) parts.push_back(cur);
            return parts;
        };
        std::string v;
        if (str_extra("game", &v)) { flags.push_back("--game"); flags.push_back(v); }
        if (str_extra("start", &v)) { flags.push_back("--start"); flags.push_back(v); }
        if (str_extra("look", &v)) for (const auto &kv : split(v, ',', false)) { flags.push_back("--look"); flags.push_back(kv); }
        if (str_extra("audio", &v)) for (const auto &kv : split(v, ',', false)) { flags.push_back("--audio-set"); flags.push_back(kv); }
        if (bool_extra("fixed_table")) flags.push_back("--fixed-table");
        if (bool_extra("replace")) flags.push_back("--replace");
        if (bool_extra("scan")) flags.push_back("--scan");
        if (str_extra("args", &v)) words = split(v, ' ', true);
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(intent);
    }
    failed();
    if (act_cls) env->DeleteLocalRef(act_cls);
    if (attached) activity->vm->DetachCurrentThread();
    out.insert(out.end(), flags.begin(), flags.end());
    out.insert(out.end(), words.begin(), words.end());
    return out;
}

}  // namespace

// The bridge (quest_app.cpp) owns the platform, the session and the frame loop; android_main only feeds it the launch
// arguments and the Android events.
extern "C" void android_main(android_app *app) {
    const std::string internal = app->activity->internalDataPath;
    app->onAppCmd = on_cmd;
    std::string note;
    const std::vector<std::string> args = read_launch_args(app->activity, &note);
    std::vector<const char *> argv;
    for (const auto &a : args) argv.push_back(a.c_str());
    qx_app_set_android(app);
    qx_app_set_pump(pump_now, app);
    // The ROM, packs and resources ship as APK assets; copy them to internalDataPath/bundled on the first launch and
    // whenever the APK's manifests change (atomic: written to bundled.tmp, renamed into place).
    const auto t0 = std::chrono::steady_clock::now();
    int copied = 0;
    std::string copy_err;
    const bool copy_ok = ar::bundled_sync(app->activity->assetManager, internal + "/bundled", &copy_err, &copied);
    const double copy_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!copy_ok) { ar::vos_log("AR_BUNDLED failed %s", copy_err.c_str()); ar::vos_log("AR_QUEST_FAIL bundled data: %s", copy_err.c_str()); }
    else if (copied < 0) ar::vos_log("AR_BUNDLED up_to_date");
    else ar::vos_log("AR_BUNDLED copied=%d ms=%.0f", copied, copy_ms);
    // bundle_dir: the bundled data copied out of the APK; documents: the files directory (prefs.json,
    // placement.json, pipelines.txt, save/, sm64ar.log).
    const int configured = arvos_configure((int)argv.size(), argv.data(), (internal + "/bundled").c_str(), internal.c_str());
    std::string joined;
    for (size_t i = 1; i < args.size(); i++) { joined += ' '; joined += args[i]; }
    ar::vos_log("AR_QUEST_START");
    ar::vos_log("AR_ARGS%s", joined.c_str());
    if (!note.empty()) ar::vos_log("AR_QUEST_INTENT %s", note.c_str());
    if (configured) qx_app_startup();
    arvos_layer_run(nullptr);   // returns when the platform stops, a quit is asked for, or the start failed
    ar::vos_log("AR_QUEST_EXIT");
    if (!app->destroyRequested) ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) pump(app, -1);
}
