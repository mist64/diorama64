#include <android_native_app_glue.h>
#include <android/looper.h>
#include <jni.h>
#include <sys/stat.h>
#include <time.h>
#include <cstring>
#include <string>
#include "ar_adapter_sm64.h"
#include "ar_game_args.h"
#include "cube_adapter.h"
#include "look.h"
#include "sm64_ar_setup.h"
#include "sm64_story.h"
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

// The launch intent's extras (adb shell am start -S -n ... --es game sm64 --es start story --ez fixed_table true
// --es look msaa=2,shadows=0 --ez replace true --ez scan true). The activity is singleTask and NativeActivity reads only
// the launch intent, so an extra applies to a fresh start (-S stops the running app first).
struct LaunchExtras {
    std::string game = "sm64";
    std::string start = "level";
    bool fixed_table = false;   // debug: the fixed table instead of the room scan's
    bool replace = false;       // "Re-place table" until plan 3's panel: the nearest table, offset reset
    bool scan = false;          // run Space Setup (scene capture), then read the scan again
    std::string look;   // launch look overrides: key=value,key=value (look.h's keys)
};

LaunchExtras read_extras(ANativeActivity *activity) {
    LaunchExtras out;
    JNIEnv *env = nullptr;
    if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        ar::vos_log("AR_QUEST_INTENT no JNI env; defaults");
        return out;
    }
    jobject intent = nullptr;
    jclass act_cls = env->GetObjectClass(activity->clazz);
    jmethodID get_intent = env->GetMethodID(act_cls, "getIntent", "()Landroid/content/Intent;");
    if (get_intent) intent = env->CallObjectMethod(activity->clazz, get_intent);
    if (env->ExceptionCheck()) { env->ExceptionClear(); intent = nullptr; }
    if (intent) {
        jclass intent_cls = env->GetObjectClass(intent);
        jmethodID get_str = env->GetMethodID(intent_cls, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
        jmethodID get_bool = env->GetMethodID(intent_cls, "getBooleanExtra", "(Ljava/lang/String;Z)Z");
        auto str_extra = [&](const char *key, std::string *dst) {
            if (!get_str) return;
            jstring k = env->NewStringUTF(key);
            jstring v = (jstring)env->CallObjectMethod(intent, get_str, k);
            if (env->ExceptionCheck()) { env->ExceptionClear(); v = nullptr; }
            if (v) {
                const char *c = env->GetStringUTFChars(v, nullptr);
                if (c) { *dst = c; env->ReleaseStringUTFChars(v, c); }
                env->DeleteLocalRef(v);
            }
            env->DeleteLocalRef(k);
        };
        str_extra("game", &out.game);
        str_extra("start", &out.start);
        str_extra("look", &out.look);
        auto bool_extra = [&](const char *key, bool *dst) {
            if (!get_bool) return;
            jstring k = env->NewStringUTF(key);
            const bool v = env->CallBooleanMethod(intent, get_bool, k, (jboolean)JNI_FALSE) != JNI_FALSE;
            if (env->ExceptionCheck()) env->ExceptionClear();
            else *dst = v;
            env->DeleteLocalRef(k);
        };
        bool_extra("fixed_table", &out.fixed_table);
        bool_extra("replace", &out.replace);
        bool_extra("scan", &out.scan);
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(intent);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();   // a missing method leaves NoSuchMethodError pending
    env->DeleteLocalRef(act_cls);
    activity->vm->DetachCurrentThread();
    return out;
}

bool file_exists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// The sm64 configuration of vos_app.mm's pick_adapter, reading the data quest_build.py --rom pushed to the app's
// external files directory: res/ (lang, dynos, palettes, mods) and packs/ (stamped). nullptr (and *err) when the
// resources are missing or the game is unknown.
const ar_adapter *pick_adapter(const std::string &game, const std::string &external, std::string *err) {
    if (game == "cube") return cube_adapter();
    if (game == "sm64") {
        const std::string res = external + "/res";
        if (!file_exists(res + "/lang/English.ini")) {
            *err = "sm64 resources missing at " + res + "; run quest_build.py --rom";
            return nullptr;
        }
        ar_adapter_sm64_set_resource_dir(res.c_str());   // before the game boots (0002-Library-seams-and-host-hooks.patch)
        ar_adapter_sm64_set_pack_stamps_required(1);      // only packs staged by quest_build.py (stamp + sha256) load
        ar_adapter_sm64_set_bundled_pack_dir((external + "/packs").c_str());
        ar_adapter_sm64_set_screen_log([](int screen, int story, uint32_t tick) {
            const char *name = sm64_screen_name(screen);
            ar::vos_log("AR_SCREEN name=%s story=%d tick=%u", name ? name : "?", story, tick);
        });
        ar_adapter_sm64_set_dropped_log([](const char *what, const char *why, uint32_t tick) {
            ar::vos_log("AR_LEVEL dropped %s: %s tick=%u", what, why, tick);
        });
        ar_adapter_sm64_set_touch_log([](const char *line, uint32_t tick) { ar::vos_log("AR_TOUCH %s tick=%u", line, tick); });
        ar_adapter_sm64_set_title_log([](const char *line, uint32_t tick) { ar::vos_log("AR_TITLE %s tick=%u", line, tick); });
        ar_adapter_sm64_set_title_place_on_request(1);
        ar_adapter_sm64_set_eyes_log([](const char *line, uint32_t tick) { ar::vos_log("AR_EYES %s tick=%u", line, tick); });
        ar_adapter_sm64_set_title_eyes_active(1);
        return ar_adapter_sm64();
    }
    *err = "unknown game " + game;
    return nullptr;
}

// The setup text vos_app.mm hands ar_adapter_sm64_setup for a launch with no --game-arg: empty for a level start,
// start=story for a story start. False (and *err) when the setup is refused.
bool setup_sm64(const std::string &start, std::string *err) {
    if (start != "level" && start != "story") {
        *err = "start must be level or story, not " + start;
        return false;
    }
    std::string setup;
    const char *src = "saved";
    const int story = sm64_start_resolve(setup.c_str(), start == "story" ? 1 : 0, &src);
    if (story && sm64_setup_start(setup.c_str()) < 0) setup = ar::setup_with_items(setup, "start=story");
    ar::vos_log("AR_STORY start=%s source=%s", story ? "story" : "level", src);
    char why[160];
    if (!ar_adapter_sm64_setup(setup.c_str(), why, sizeof why)) {
        *err = std::string("setup: ") + why;
        return false;
    }
    ar::vos_log("AR_SETUP %s", setup.c_str());
    return true;
}

// The look overrides of the launch extra "look" (key=value,key=value) into the store, each applied one logged
// (AR_LOOK key=value) and each refused one logged with why. Until plan 3's panel the only way to change the look.
void apply_look_overrides(const std::string &spec, ar::LookStore *store) {
    size_t at = 0;
    while (at <= spec.size()) {
        size_t end = spec.find(',', at);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        std::string why;
        if (eq == std::string::npos || eq == 0) {
            ar::vos_log("AR_LOOK refused %s: not key=value", item.c_str());
        } else if (!store->set(item.substr(0, eq), item.substr(eq + 1), &why)) {
            ar::vos_log("AR_LOOK refused %s: %s", item.c_str(), why.c_str());
        } else {
            ar::vos_log("AR_LOOK %s", item.c_str());
        }
    }
}

}  // namespace

extern "C" void android_main(android_app *app) {
    const std::string internal = app->activity->internalDataPath;
    // The app's own external files directory: adb pushes the ROM, res/ and packs/ there (quest_build.py --rom).
    const std::string external = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    ar::vos_log_open((internal + "/sm64ar.log").c_str());
    ar::vos_log("AR_QUEST_START");
    ar::vos_log("AR_ADAPTERS sm64=%s abi=%u", ar_adapter_sm64()->name, ar_adapter_sm64()->abi_version);
    app->onAppCmd = on_cmd;
    const LaunchExtras extras = read_extras(app->activity);
    ar::vos_log("AR_QUEST_INTENT game=%s start=%s fixed_table=%d replace=%d scan=%d look=%s", extras.game.c_str(),
                extras.start.c_str(), extras.fixed_table ? 1 : 0, extras.replace ? 1 : 0, extras.scan ? 1 : 0, extras.look.c_str());
    ar::LookStore look;   // outlives the session (declared before it)
    apply_look_overrides(extras.look, &look);

    // Fallback: the cube, so a missing or refused game still leaves passthrough (and a log line saying why) up.
    std::string game_name = extras.game, rom;
    std::string pick_err;
    const ar_adapter *adapter = pick_adapter(game_name, external, &pick_err);
    if (adapter && game_name == "sm64" && !setup_sm64(extras.start, &pick_err)) adapter = nullptr;
    if (!adapter) {
        ar::vos_log("AR_QUEST_FAIL %s", pick_err.c_str());
        game_name = "cube";
        adapter = cube_adapter();
    }
    if (game_name == "sm64") rom = external + "/rom.z64";

    ar::QxPlatform::Config cfg;
    // The room scan's table (the platform's placer, remembered at placement.json) unless fixed_table asks for plan 1's
    // fixed one (debugging without a scan).
    cfg.fixed_table = ar_pose_identity();
    cfg.fixed_table.pos[1] = 0.75f;   // table height above the floor; in LOCAL space the platform moves it 1.5 m down
    cfg.fixed_table.pos[2] = -1.0f;
    cfg.fixed_table_valid = extras.fixed_table;
    cfg.record_path = internal + "/placement.json";
    ar::QxPlatform platform(app, cfg);
    app->userData = &platform;
    ar::QxSession session(adapter, platform, internal);
    session.set_look_store(&look);
    if (game_name == "sm64") {
        // vos_app.mm's pointer claim: the Touch trigger's ray drives sm64's title screen and menus. The Quest never
        // records or replays, so the pointer lock stays off. Logged at begin / end / cancel, never per move.
        ar_adapter_sm64_set_pointer_lock(0);
        // sm64's "audio" setting (on|off) starts and stops the AAudio stream; the session re-reads it once a second.
        session.set_audio_setting([] {
            char v[8];
            return !ar_adapter_sm64_get_setting("audio", v, sizeof v) || std::strcmp(v, "off") != 0;
        });
        session.set_pointer_claim([](const ar::PointerEvent &e) {
            sm64_pointer p{};
            p.id = e.id; p.phase = (int)e.phase; p.has_hand = e.has_hand ? 1 : 0; p.head = e.head; p.no_ray = e.has_ray ? 0 : 1;
            for (int k = 0; k < 3; k++) { p.ray_o[k] = e.ray_o[k]; p.ray_d[k] = e.ray_d[k]; p.hand[k] = e.hand[k]; }
            const int claimed = ar_adapter_sm64_pointer(&p);
            if (e.phase == ar::PointerEvent::begin)
                ar::vos_log("AR_POINTER begin id=%llu claimed=%d target=%d ndc=%.3f,%.3f", (unsigned long long)e.id, claimed,
                            p.target, (double)p.ndc[0], (double)p.ndc[1]);
            else if (e.phase == ar::PointerEvent::end || e.phase == ar::PointerEvent::cancel)
                ar::vos_log("AR_POINTER %s id=%llu", e.phase == ar::PointerEvent::end ? "end" : "cancel", (unsigned long long)e.id);
            return claimed != 0;
        });
    }

    // running: the platform frame loop runs; game: the session drives it (else passthrough only, after a failed start).
    std::string err;
    bool running = platform.start(&err);
    if (!running) ar::vos_log("AR_QUEST_FAIL %s", err.c_str());
    if (running && extras.replace) platform.replace_table();
    if (running && extras.scan) platform.request_scene_capture();
    bool game = running && session.start(rom, internal + "/save", &err);
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
            if (game) ar::vos_log("AR_STATUS %s", session.status().c_str());
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
