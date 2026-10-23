// vos_api.h on the Quest: a port of apps/visionos/vos_app.mm, function by function, with QxPlatform / QxSession in
// place of VosPlatform / VosSession and prefs.json (QxPrefs) in place of UserDefaults. The Swift models' persistence
// (SM64ARApp.swift: ShellModel, LookModel, AudioModel, PacksModel, SM64LookModel) lives here too, so the panel calls the
// arvos_* setters only, and qx_app_startup() is SM64ARApp.init's load sequence.
//
// Differences from vos_app.mm: no pad timer (QxInput syncs in wait_frame); no render probe, safe mode, foveation,
// render quality, spatial events, ARKit hand tracking, .sarec replays or recordings (--sarec / --record are refused);
// arvos_layer_run(nullptr) makes, starts and owns the platform and runs the frame loop on the calling thread (pumping
// the Android looper through qx_app_set_pump's callback) until the platform stops or a quit is asked for.
#include "vos_api.h"
#include "qx_app.h"
#include <android_native_app_glue.h>
#include <sys/stat.h>
#include <time.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ar_adapter_sm64.h"
#include "sm64_ar_setup.h"
#include "sm64_packs.h"
#include "sm64_relevance.h"
#include "sm64_story.h"
#include "ar_audio_settings.h"
#include "ar_game_args.h"
#include "cube_adapter.h"
#include "look.h"
#include "panel_model.h"
#include "qx_platform.h"
#include "qx_prefs.h"
#include "qx_session.h"
#include "quest_prefs_glue.h"
#include "room_light.h"
#include "vos_log.h"
#include "vos_options.h"
#include "vos_scene_pos.h"

namespace {
const std::vector<ar::GameSpec> &games() {
    static const std::vector<ar::GameSpec> g = {
        {"sm64", ar::PlaceParams{1.9f, 0.7f, 0.f}},
        {"cube", ar::PlaceParams{1.2f, 0.5f, 0.f}},
    };
    return g;
}
struct AppState {
    std::mutex m;                  // status, error, refusal, hint
    std::mutex run;                // one frame loop at a time
    std::string bundle, docs, status = "not configured", error, hint;
    ar::GameSettingLock game_lock;
    int start_in = 1;
    std::string refusal;
    std::chrono::steady_clock::time_point refusal_at;
    ar::VosOptions opts;
    bool options_ok = false;
    // The Quest-only launch flags (taken out of argv before parse_vos_options).
    bool fixed_table = false;
    std::atomic<bool> replace_pending{false}, scan_pending{false};
    std::string ui_tab_arg;
    android_app *app = nullptr;
    void (*pump)(void *) = nullptr;
    void *pump_ctx = nullptr;
    std::unique_ptr<ar::QxPlatform> platform;   // made by arvos_layer_run, destroyed when its loop ends
    std::unique_ptr<ar::QxSession> session;
    ar::LookStore look;
    ar::AudioStore audio;
    ar::RoomLightStatusBox light_status;
    std::atomic<ar::QxPlatform *> live{nullptr};
    std::atomic<bool> background{false};
    std::atomic<bool> quit{false};              // arvos_space_closed / arvos_prepare_quit / arvos_quest_request_quit
    std::atomic<std::thread::id> loop_thread{std::thread::id()};
    bool packs_known = false;
};
AppState &state() { static AppState s; return s; }
constexpr double kRefusalShowS = 8.0;
void set_status(const std::string &s) { std::lock_guard<std::mutex> g(state().m); state().status = s; }
bool file_exists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// The data the game reads: the bundled copy (internalDataPath/bundled, copied out of the APK's assets by main.cpp):
// Bundled/rom.z64, Bundled/packs/<name>/ and res/{lang,dynos,palettes,mods}.
struct DataDirs { std::string rom, res, packs, source; };
DataDirs data_dirs() {
    DataDirs d;
    d.rom = ar::bundled_data(state().bundle, "rom.z64"); d.res = state().bundle + "/res";
    d.packs = ar::bundled_data(state().bundle, "packs"); d.source = "bundled";
    return d;
}
const ar_adapter *pick_adapter(const std::string &game, const DataDirs &d, std::string *err) {
    if (game == "cube") return cube_adapter();
    if (game == "sm64") {
        if (!file_exists(d.res + "/lang/English.ini")) {
            *err = "sm64 resources missing at " + d.res + " (no res/lang/English.ini; rebuild the APK with quest_build.py)";
            return nullptr;
        }
        ar_adapter_sm64_set_resource_dir(d.res.c_str());   // before the game boots (0002-Library-seams-and-host-hooks.patch)
        ar_adapter_sm64_set_pack_stamps_required(1);        // only staged packs (stamp + sha256) load
        ar_adapter_sm64_set_bundled_pack_dir(d.packs.c_str());
        ar_adapter_sm64_set_screen_log([](int screen, int story, uint32_t tick) {
            const char *name = sm64_screen_name(screen);
            ar::vos_log("AR_SCREEN name=%s story=%d tick=%u", name ? name : "?", story, tick);
        });
        ar_adapter_sm64_set_dropped_log([](const char *what, const char *why, uint32_t tick) {
            ar::vos_log("AR_LEVEL dropped %s: %s tick=%u", what, why, tick);
            AppState &st = state();
            std::lock_guard<std::mutex> g(st.m);
            st.refusal = std::string(what) + " dropped: " + why; st.refusal_at = std::chrono::steady_clock::now();
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
// sm64's audio settings for QxSession: "sound" spatial|stereo, "resampler" sinc|linear (feed_choice 0 | 1).
ar::QxSession::AudioChoice sm64_audio_choice() {
    ar::QxSession::AudioChoice c;
    char v[16];
    c.spatial = ar_adapter_sm64_get_setting("sound", v, sizeof v) && std::strcmp(v, "spatial") == 0;
    c.resampler = ar_adapter_sm64_get_setting("resampler", v, sizeof v) && std::strcmp(v, "linear") == 0 ? 1 : 0;
    return c;
}
double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
}  // namespace
using qxapp::pref_set; using qxapp::pref_set_bool; using qxapp::pref_set_int; using qxapp::pref_set_double;
using qxapp::pref_has; using qxapp::prefs_flush; using qxapp::prefs_save; using qxapp::prefs_tick;
using qxapp::pack_on_key; using qxapp::pack_name_key;

static void apply_audio_to_game(const ar::AudioSettings &a);

// SM64LookModel.load() iterates only the table's keys, so an unknown --game-set KEY would be ignored silently: log it.
static void log_unknown_game_set_keys() {
    if (state().options_ok && state().opts.game == "sm64")
        for (const auto &kv : state().opts.game_set) {
            char b[32];
            if (!ar_adapter_sm64_get_setting(kv.first.c_str(), b, sizeof b))
                ar::vos_log("AR_SETTING sm64.%s rejected (unknown key)", kv.first.c_str());
        }
}
// The Quest-only flags out of args (the shared parser would refuse them): --fixed-table, --replace, --scan; and
// --start story|level, which becomes the visionOS launch form --game-arg start=story|level. --ui-tab stays (the shared
// parser takes it and ignores it); its value is remembered for arvos_quest_ui_tab.
static bool quest_preparse(std::vector<std::string> *args, std::string *err) {
    AppState &s = state();
    std::vector<std::string> out;
    for (size_t i = 0; i < args->size(); i++) {
        const std::string &a = (*args)[i];
        if (i == 0) { out.push_back(a); continue; }
        if (a == "--fixed-table") s.fixed_table = true;
        else if (a == "--replace") s.replace_pending = true;
        else if (a == "--scan") s.scan_pending = true;
        else if (a == "--start") {
            if (i + 1 >= args->size()) { *err = "--start needs a value"; return false; }
            const std::string v = (*args)[++i];
            if (v != "story" && v != "level") { *err = "bad --start " + v + " (story or level)"; return false; }
            out.push_back("--game-arg");
            out.push_back("start=" + v);
        } else {
            if (a == "--ui-tab" && i + 1 < args->size()) s.ui_tab_arg = (*args)[i + 1];
            out.push_back(a);
        }
    }
    *args = out;
    return true;
}
extern "C" int arvos_configure(int argc, const char *const *argv, const char *bundle_dir, const char *documents_dir) {
    AppState &s = state();
    s.bundle = bundle_dir ? bundle_dir : "";
    s.docs = documents_dir ? documents_dir : "";
    std::vector<std::string> args(argv, argv + argc);
    if (args.empty()) args.push_back("diorama64");
    ar::vos_log_open(s.docs.empty() ? nullptr : (s.docs + "/sm64ar.log").c_str());
    std::string joined;
    for (size_t i = 1; i < args.size(); i++) { joined += ' '; joined += args[i]; }
    ar::vos_log("AR_CONFIG args=%s", joined.c_str());
    ar::vos_log("AR_ADAPTERS sm64=%s abi=%u", ar_adapter_sm64()->name, ar_adapter_sm64()->abi_version);
    std::string err;
    s.options_ok = quest_preparse(&args, &err) && ar::parse_vos_options(args, games(), &s.opts, &err);
    if (s.options_ok && (!s.opts.sarec.empty() || !s.opts.record.empty())) {
        err = "--sarec and --record are not supported on the Quest";
        s.options_ok = false;
    }
    if (!s.options_ok) {
        s.error = "bad arguments: " + err;
        ar::vos_log("AR_ERROR %s", s.error.c_str());
        set_status(s.error);
        return 0;
    }
    s.opts.rom = ar::bundled_data(s.bundle, "rom.z64");
    if (s.opts.reset_placement) s.replace_pending = true;
    if (s.opts.fake_table == ar::FakeTable::on) s.fixed_table = true;
    log_unknown_game_set_keys();
    // Open here, so arvos_quest_seed_look works between arvos_configure and qx_app_startup (which loads from it).
    if (!qxapp::prefs()) qxapp::prefs_open(s.docs + "/prefs.json");
    apply_audio_to_game(s.audio.get());
    set_status("ready: game " + s.opts.game);
    return 1;
}
extern "C" int arvos_autostart(void) { return 1; }   // the Quest has no window: the table always opens
extern "C" int arvos_lifecycle_test(double *, double *) { return 0; }
extern "C" int arvos_hide_status(void) {
    return state().options_ok && (!state().opts.hold_ticks.empty() || state().opts.hide_status) ? 1 : 0;
}
extern "C" int arvos_status(char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    AppState &s = state();
    std::lock_guard<std::mutex> g(s.m);
    const bool show_refusal = !s.refusal.empty() &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - s.refusal_at).count() < kRefusalShowS;
    std::snprintf(buf, (size_t)cap, "%s%s%s%s%s%s", show_refusal ? "level not changed: " : "", show_refusal ? s.refusal.c_str() : "",
                  show_refusal ? "\n" : "", s.status.c_str(), s.hint.empty() ? "" : "\n", s.hint.c_str());
    return 1;
}
extern "C" void arvos_log(const char *line) { ar::vos_log("%s", line ? line : ""); }
extern "C" void arvos_space_closed(void) {
    state().quit = true;
    ar::vos_log("AR_SPACE closed");
}
extern "C" int arvos_prepare_quit(double wait_s) {
    AppState &s = state();
    s.quit = true;
    prefs_flush("quit");
    if (s.loop_thread.load() == std::this_thread::get_id()) {   // the loop cannot end while its own thread waits here
        ar::vos_log("AR_APP quit loop_ended=0 (asked from the frame loop: it ends after this frame)");
        return 0;
    }
    // The loop holds `run` while it runs; the session's stop (its pipeline list) runs before it lets go.
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(wait_s);
    bool ended = false;
    while (!(ended = s.run.try_lock()) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (ended) s.run.unlock();
    ar::vos_log("AR_APP quit loop_ended=%d", ended ? 1 : 0);
    return ended ? 1 : 0;
}
// Exit without teardown, as vos_app.mm: the session, the platform and the audio stream may still run on other threads.
extern "C" void arvos_quit_now(void) {
    prefs_flush("quit");
    ar::vos_log("AR_APP quit exit (no teardown)");
    ar::vos_exit_now(0);
}
extern "C" int arvos_replace_test(double *) { return 0; }
extern "C" void arvos_replace_table(void) {
    ar::vos_log("AR_UI replace table");
    if (ar::QxPlatform *p = state().live.load()) p->replace_table();
    else state().replace_pending = true;   // once the platform runs
}
static int copy_out(const std::string &v, char *buf, int cap);
extern "C" int arvos_game(char *buf, int cap) { return state().options_ok ? copy_out(state().opts.game, buf, cap) : 0; }
extern "C" int arvos_game_setting_option(const char *key, char *buf, int cap) {
    if (!key || !state().options_ok) return 0;
    const std::string *last = nullptr;
    for (const auto &kv : state().opts.game_set) if (kv.first == key) last = &kv.second;
    return last ? copy_out(*last, buf, cap) : 0;
}
static bool has_levels() { return state().options_ok && state().opts.game == "sm64"; }
static bool is_sm64_session() { return has_levels(); }
extern "C" int arvos_sm64_setting_count(void) { return has_levels() ? ar_adapter_sm64_setting_count() : 0; }
extern "C" int arvos_sm64_setting_key(int i, char *buf, int cap) {
    const char *k = has_levels() ? ar_adapter_sm64_setting_key(i) : nullptr;
    return k ? copy_out(k, buf, cap) : 0;
}
extern "C" int arvos_sm64_setting_default(const char *key, char *buf, int cap) {
    const char *d = has_levels() && key ? ar_adapter_sm64_setting_default(key) : nullptr;
    return d ? copy_out(d, buf, cap) : 0;
}
// vos_app.mm's arvos_sm64_setting_set (no hand tracking to start for title_eyes on the Quest).
static int apply_sm64_setting(const char *key, const char *value) {
    if (!has_levels() || !key || !value) return 0;
    char why[96];
    if (!ar_adapter_sm64_set_setting(key, value, why, sizeof why)) {
        ar::vos_log("AR_SETTING sm64.%s rejected value=%s (%s)", key, value, why);
        return 0;
    }
    ar::vos_log("AR_SETTING sm64.%s=%s", key, value);
    // resampler: QxSession polls the setting once a second (set_audio_choice), so no feed filter is kept here.
    return 1;
}
extern "C" int arvos_sm64_setting_set(const char *key, const char *value) {   // + SM64LookModel.set's save
    if (!apply_sm64_setting(key, value)) return 0;
    pref_set(std::string("sm64.") + key, value);
    return 1;
}
extern "C" int arvos_sm64_settings_revision(void) { return has_levels() ? ar_adapter_sm64_settings_revision() : 0; }
extern "C" int arvos_sm64_setting_drop_stored(const char *key, const char *stored, int stored_rev, int has_launch_override) {
    return has_levels() ? ar_adapter_sm64_settings_drop_stored(key, stored, stored_rev, has_launch_override) : 0;
}
extern "C" int arvos_level_count(void) { return has_levels() ? ar_adapter_sm64_level_count() : 0; }
extern "C" int arvos_level_name(int index, char *buf, int cap) {
    const sm64_level_entry *e = has_levels() ? ar_adapter_sm64_level(index) : nullptr;
    if (!e || !buf || cap <= 0) return 0;
    std::snprintf(buf, (size_t)cap, "%s", e->name);
    return 1;
}
extern "C" int arvos_level_has_acts(int index) {
    const sm64_level_entry *e = has_levels() ? ar_adapter_sm64_level(index) : nullptr;
    return e && e->course > 0 ? 1 : 0;
}
extern "C" int arvos_level_section(int index) {
    const sm64_level_entry *e = has_levels() ? ar_adapter_sm64_level(index) : nullptr;
    return e ? e->section : -1;
}
extern "C" int arvos_play_from_start(void) {
    AppState &s = state();
    std::string why;
    if (!has_levels()) why = "no levels in this game";
    else if (!ar::level_switch_allowed(s.opts, &why)) {}
    else if (!ar_adapter_sm64_play_from_start()) why = ar_adapter_sm64_request_refusal() ? ar_adapter_sm64_request_refusal() : "refused";
    if (!why.empty()) {
        ar::vos_log("AR_STORY play_from_start refused: %s", why.c_str());
        std::lock_guard<std::mutex> g(s.m);
        s.refusal = why; s.refusal_at = std::chrono::steady_clock::now();
        return 0;
    }
    ar::vos_log("AR_STORY play_from_start");
    return 1;
}
extern "C" int arvos_select_level(int index, int act) {
    AppState &s = state();
    std::string why;
    const sm64_level_entry *e = has_levels() ? ar_adapter_sm64_level(index) : nullptr;
    if (!e) why = "no such level";
    else if (!ar::level_switch_allowed(s.opts, &why)) {}
    else if (act < 0 || act > 6) why = "bad act " + std::to_string(act) + " (0..6)";
    else if (!ar_adapter_sm64_request_level(index, act >= 1 ? act : -1))
        why = ar_adapter_sm64_request_refusal() ? ar_adapter_sm64_request_refusal() : "the game did not take the pick";
    if (!why.empty()) {
        ar::vos_log("AR_LEVEL refused index=%d act=%d: %s", index, act, why.c_str());
        std::lock_guard<std::mutex> g(s.m);
        s.refusal = why;
        s.refusal_at = std::chrono::steady_clock::now();
        return 0;
    }
    ar::vos_log("AR_LEVEL request key=%s act=%d", e->key, act);
    std::lock_guard<std::mutex> g(s.m);
    s.refusal.clear();
    return 1;
}
extern "C" int arvos_level_test(double *after_s, int *index, int *act) {
    const AppState &s = state();
    if (!has_levels() || !(s.opts.select_test_s > 0)) return 0;
    const int i = sm64_level_find(s.opts.select_test_key.c_str());
    if (i < 0) { ar::vos_log("AR_ERROR --select-test: unknown level %s", s.opts.select_test_key.c_str()); return 0; }
    *after_s = s.opts.select_test_s; *index = i; *act = s.opts.select_test_n;
    return 1;
}
extern "C" int arvos_level_view_get(arvos_level_view *out) {
    sm64_level_state ls;
    if (!out || !has_levels() || !ar_adapter_sm64_level_state(&ls)) return 0;
    out->current = ls.index; out->home = ls.home_index; out->pending = ls.pending; out->act = ls.act; out->story = ls.story;
    sm64_level_current_name(&ls, out->current_name, (int)sizeof out->current_name);
    out->screen = ls.screen;
    if (const char *title = ls.screen != 0 ? sm64_screen_title(ls.screen) : nullptr) {
        std::snprintf(out->current_name, sizeof out->current_name, "%s", title); out->current = -1;
    }
    return 1;
}
extern "C" int arvos_level_label(int index, char *buf, int cap) {
    const sm64_level_entry *e = has_levels() ? ar_adapter_sm64_level(index) : nullptr;
    if (!e || !buf || cap <= 0) return 0;
    sm64_level_state ls{};
    const bool have = ar_adapter_sm64_level_state(&ls) != 0;
    sm64_level_label(buf, cap, e->name, have ? ls.stars[index] : 0, e->star_max, have && ls.index == index,
                     have && ls.home_index == index);
    return 1;
}
extern "C" int arvos_has_draw_all_rooms(void) { return has_levels() ? 1 : 0; }
extern "C" int arvos_draw_all_rooms_option(void) {
    const AppState &s = state();
    if (!s.options_ok) return -1;
    return s.opts.draw_all_rooms >= 0 || !has_levels() ? s.opts.draw_all_rooms : sm64_setup_rooms(ar::join_game_args(s.opts.game_args).c_str());
}
extern "C" int arvos_draw_all_rooms(void) { return has_levels() ? ar_adapter_sm64_draw_all_rooms() : -1; }
extern "C" void arvos_set_draw_all_rooms(int on) {
    // ShellModel.applyDrawAllRooms: nothing (not even the save) while the game's settings are locked.
    if (state().game_lock.why().empty()) pref_set_bool("drawAllRooms", on != 0);
    if (!has_levels()) return;
    std::string why;
    if (!state().game_lock.change([&] { ar_adapter_sm64_set_draw_all_rooms(on ? 1 : 0); }, &why)) {
        ar::vos_log("AR_SETTING draw_all_rooms locked (%s): kept %d", why.c_str(), ar_adapter_sm64_draw_all_rooms());
        return;
    }
    ar::vos_log("AR_SETTING draw_all_rooms=%d", on ? 1 : 0);
}
extern "C" int arvos_hide_outside_option(void) { return state().options_ok ? state().opts.hide_outside : -1; }
extern "C" void arvos_set_hide_outside(int on) {
    pref_set_bool("hideOutside", on != 0);
    if (!has_levels()) return;
    ar_adapter_sm64_set_hide_outside(on ? 1 : 0);
    ar::vos_log("AR_SETTING hide_outside=%d", on ? 1 : 0);
}
extern "C" double arvos_mario_cm_option(void) {
    if (!has_levels()) return -1.0;
    const float cm = sm64_setup_mario_cm(ar::join_game_args(state().opts.game_args).c_str());
    return std::isnan(cm) ? -1.0 : (double)cm;
}
extern "C" int arvos_start_option(void) {
    return has_levels() ? sm64_setup_start(ar::join_game_args(state().opts.game_args).c_str()) : -1;
}
extern "C" int arvos_set_start_in(int story) {
    if (!has_levels()) return 0;
    AppState &s = state();
    const std::string why = s.game_lock.why();
    if (!why.empty()) { ar::vos_log("AR_SETTING start_in locked (%s)", why.c_str()); return 0; }
    s.start_in = story ? 1 : 0;
    ar::vos_log("AR_SETTING start_in=%s%s", story ? "story" : "level", s.session ? " (applies at the next launch)" : "");
    pref_set_bool("startIn", story != 0);   // ShellModel.applyStartIn: saved when taken
    return 1;
}
extern "C" int arvos_start_effective(char *source, int cap) {
    const AppState &s = state();
    if (source && cap > 0) source[0] = 0;
    if (!has_levels() || !s.options_ok || !s.opts.sarec.empty()) return -1;
    const char *src = "saved";
    const int story = sm64_start_resolve(ar::join_game_args(s.opts.game_args).c_str(), s.start_in, &src);
    if (source && cap > 0) std::snprintf(source, (size_t)cap, "%s", src);
    return story;
}
extern "C" double arvos_mario_cm(void) { return has_levels() ? (double)ar_adapter_sm64_mario_cm() : -1.0; }
extern "C" int arvos_mario_cm_lock(char *buf, int cap) {
    const std::string why = state().game_lock.why();
    if (why.empty()) return 0;
    if (buf && cap > 0) std::snprintf(buf, (size_t)cap, "%s", why.c_str());
    return 1;
}
extern "C" void arvos_set_mario_cm(double cm, int final_value) {
    // ShellModel.applyMarioCm: clamped to 0.5...10, saved when final.
    cm = std::min(std::max(cm, 0.5), 10.0);
    if (final_value) pref_set_double("marioCm", cm);
    if (!has_levels()) return;
    std::string why;
    if (!state().game_lock.change([&] { ar_adapter_sm64_set_mario_cm((float)cm); }, &why)) {
        if (final_value) ar::vos_log("AR_SETTING mario_cm locked (%s): kept %.2f", why.c_str(), (double)ar_adapter_sm64_mario_cm());
        return;
    }
    if (final_value) {
        const float c = ar_adapter_sm64_mario_cm();
        ar::vos_log("AR_SETTING mario_cm=%.2f upm=%.0f", (double)c, (double)sm64_units_per_meter_for_mario_cm(c));
    }
}
extern "C" int arvos_scale_readout(double cm, char *buf, int cap) {
    if (!has_levels() || !buf || cap <= 0) return 0;
    sm64_level_state ls{};
    const float extent = ar_adapter_sm64_level_state(&ls) ? ls.extent_units : 0.f;
    sm64_scale_readout(buf, cap, (float)cm, extent);
    return 1;
}
extern "C" double arvos_mario_focus_far_m(double cm) {
    return has_levels() ? (double)(SM64_RELEVANCE_FAR / sm64_units_per_meter_for_mario_cm((float)cm)) : 0.0;
}
extern "C" int arvos_other_rooms_option(void) {
    const AppState &s = state();
    if (!s.options_ok) return -1;
    return s.opts.other_rooms >= 0 || !has_levels() ? s.opts.other_rooms : sm64_setup_other_rooms(ar::join_game_args(s.opts.game_args).c_str());
}
extern "C" int arvos_other_rooms(void) { return has_levels() ? ar_adapter_sm64_other_room_objects() : -1; }
extern "C" void arvos_set_other_rooms(int mode) {
    const char *name = ar_adapter_sm64_other_rooms_name(mode);
    if (name && state().game_lock.why().empty()) pref_set_int("otherRoomObjects", mode);   // ShellModel.applyOtherRooms
    if (!has_levels() || !name) return;
    std::string why;
    if (!state().game_lock.change([&] { ar_adapter_sm64_set_other_room_objects(mode); }, &why)) {
        ar::vos_log("AR_SETTING other_rooms locked (%s): kept %s", why.c_str(),
                    ar_adapter_sm64_other_rooms_name(ar_adapter_sm64_other_room_objects()));
        return;
    }
    ar::vos_log("AR_SETTING other_rooms=%s", name);
}
extern "C" int arvos_tree_style_option(void) { return state().options_ok ? state().opts.tree_style : -1; }
extern "C" void arvos_set_tree_style(int style) {
    const char *name = ar_adapter_sm64_tree_style_name(style);
    if (name) pref_set_int("treeStyle", style);
    if (!has_levels() || !name) return;
    ar_adapter_sm64_set_tree_style(style);
    ar::vos_log("AR_SETTING tree_style=%s", name);
}
extern "C" int arvos_billboards_option(void) { return state().options_ok ? state().opts.billboards : -1; }
extern "C" void arvos_set_billboards(int style) {
    const char *name = ar_adapter_sm64_billboard_style_name(style);
    if (name) pref_set_int("billboards", style);
    if (!has_levels() || !name) return;
    ar_adapter_sm64_set_billboard_style(style);
    ar::vos_log("AR_SETTING billboards=%s", name);
}
extern "C" int arvos_cannon_visible_option(void) {
    return has_levels() ? sm64_setup_cannon(ar::join_game_args(state().opts.game_args).c_str()) : -1;
}
extern "C" int arvos_cannon_visible(void) { return has_levels() ? ar_adapter_sm64_cannon_visible() : -1; }
extern "C" void arvos_set_cannon_visible(int on) {
    pref_set_bool("cannonVisible", on != 0);   // ShellModel.applyCannonVisible saves before the lock check
    if (!has_levels()) return;
    std::string why;
    if (!state().game_lock.change([&] { ar_adapter_sm64_set_cannon_visible(on ? 1 : 0); }, &why)) {
        ar::vos_log("AR_SETTING cannon_visible locked (%s): kept %d", why.c_str(), ar_adapter_sm64_cannon_visible());
        return;
    }
    ar::vos_log("AR_SETTING cannon_visible=%d", on ? 1 : 0);
}
static int copy_out(const std::string &v, char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    std::snprintf(buf, (size_t)cap, "%s", v.c_str());
    return 1;
}
extern "C" int arvos_look_key_count(void) { return (int)ar::look_keys().size(); }
extern "C" int arvos_look_key(int i, char *buf, int cap) {
    const auto &k = ar::look_keys();
    return i >= 0 && i < (int)k.size() ? copy_out(k[(size_t)i], buf, cap) : 0;
}
extern "C" int arvos_look_default(const char *key, char *buf, int cap) {
    std::string v;
    return key && ar::look_default(key, &v) ? copy_out(v, buf, cap) : 0;
}
extern "C" int arvos_look_defaults_rev(void) { return ar::look_defaults_rev(); }
extern "C" int arvos_look_drop_stored(const char *key, const char *stored, int stored_rev, int has_launch_override) {
    return key && stored && ar::look_drop_stored(key, stored, stored_rev, has_launch_override != 0) ? 1 : 0;
}
extern "C" int arvos_look_option(const char *key, char *buf, int cap) {
    if (!key || !state().options_ok) return 0;
    const std::string *last = nullptr;
    for (const auto &kv : state().opts.look) if (kv.first == key) last = &kv.second;
    return last ? copy_out(*last, buf, cap) : 0;
}
static struct PackChoice { int on = 0; std::string name; bool set = false; } g_packs[3];
static void log_pack_load_if_changed(void *) {
    static double logged = 0;   // frame loop thread only
    const double ms = ar_adapter_sm64_pack_load_ms();
    if (ms == logged) return;
    logged = ms;
    uint64_t up = 0, peak = 0;
    ar_adapter_sm64_texfifo_stats(&up, &peak);
    ar::vos_log("AR_PACKS load_ms=%.1f tex_up_mb=%.1f tex_peak_mb=%.1f", ms, up / 1048576.0, peak / 1048576.0);
}
static void apply_packs(int only_kind = 0) {
    if (!has_levels()) return;
    const bool roomlit = state().look.get().roomlit;
    for (int kind : {SM64_PACK_TEXTURES, SM64_PACK_MODELS}) {
        if (!g_packs[kind].set || (only_kind && kind != only_kind)) continue;
        const int on = g_packs[kind].on;
        const int idx = ar_adapter_sm64_set_packs(kind, on && roomlit, g_packs[kind].name.c_str());
        char used[128] = "", choice[160];
        if (on && roomlit && idx >= 0) ar_adapter_sm64_pack_info(idx, used, sizeof used, nullptr, nullptr, nullptr, nullptr);
        sm64_packs_choice_string(kind, on, on && roomlit && idx >= 0 ? used : g_packs[kind].name.c_str(), choice, sizeof choice);
        ar::vos_log("AR_SETTING %s=%s%s", sm64_packs_kind_name(kind), choice,
                    !on ? "" : !roomlit ? " (off: Look is Original N64)" : idx < 0 ? " (none installed)" : "");
    }
}
extern "C" int arvos_pack_count(void) {
    if (!has_levels()) return -1;
    const int n = ar_adapter_sm64_pack_count();
    static bool logged = false;
    if (n >= 0 && !logged) {
        logged = true;
        ar::vos_log("AR_PACKS n=%d", n);
        for (int i = 0; i < n; i++) {
            char name[128]; int kind = 0, tex = 0, bins = 0; double mb = 0;
            if (!ar_adapter_sm64_pack_info(i, name, sizeof name, &kind, &tex, &bins, &mb)) continue;
            ar::vos_log("AR_PACK i=%d kind=%s name=%s tex=%d bins=%d mb=%.1f", i,
                        sm64_packs_kind_name(kind) ? sm64_packs_kind_name(kind) : "none", name, tex, bins, mb);
            if (const char *why = ar_adapter_sm64_pack_refusal(i)) ar::vos_log("AR: refusing unstaged pack \"%s\" (%s)", name, why);
        }
        int refused = 0; double ms = 0;
        const int ok = ar_adapter_sm64_pack_stamps(&refused, &ms);
        if (ok >= 0) ar::vos_log("AR_PACKS_STAMPED ok=%d refused=%d ms=%.1f", ok, refused, ms);
    }
    return n;
}
extern "C" int arvos_pack_info(int index, char *name, int cap) {
    int kind = 0;
    if (!has_levels() || !name || cap <= 0 || !ar_adapter_sm64_pack_info(index, name, cap, &kind, nullptr, nullptr, nullptr)) return 0;
    return kind;
}
extern "C" int arvos_pack_option(int kind, char *buf, int cap) {
    if (!state().options_ok || !buf || cap <= 0) return 0;
    const std::string empty;
    const std::string &v = kind == SM64_PACK_TEXTURES ? state().opts.textures : kind == SM64_PACK_MODELS ? state().opts.models : empty;
    if (v.empty() || (int)v.size() >= cap) return 0;
    std::memcpy(buf, v.c_str(), v.size() + 1);
    return 1;
}
extern "C" void arvos_set_packs(int kind, int on, const char *name) {
    if (kind != SM64_PACK_TEXTURES && kind != SM64_PACK_MODELS) return;
    g_packs[kind].on = on ? 1 : 0; g_packs[kind].name = name ? name : ""; g_packs[kind].set = true;
    pref_set_bool(pack_on_key(kind), on != 0);   // PacksModel.apply
    pref_set(pack_name_key(kind), g_packs[kind].name);
    apply_packs(kind);
}
extern "C" int arvos_trees_3d(void) { return has_levels() ? ar_adapter_sm64_trees_3d() : 0; }

static int apply_look(const char *key, const char *value, int final_value) {
    if (!key || !value) return 0;
    std::string err;
    if (!state().look.set(key, value, &err)) {
        ar::vos_log("AR_SETTING look.%s rejected value=%s (%s)", key, value, err.c_str());
        return 0;
    }
    if (final_value) ar::vos_log("AR_SETTING look.%s=%s", key, value);
    if (has_levels()) ar_adapter_sm64_set_normal_flags(ar::look_normal_flags(state().look.get()));
    if (std::strcmp(key, "mode") == 0) apply_packs();
    return 1;
}
extern "C" int arvos_set_look(const char *key, const char *value, int final_value) {   // + LookModel.set's save
    if (!apply_look(key, value, final_value)) return 0;
    if (final_value) pref_set(std::string("look.") + key, value);
    return 1;
}
extern "C" int arvos_foveation(void) { return 0; }   // no foveated rate map on the Quest
extern "C" void arvos_render_probe_begin(void) {}
extern "C" void arvos_render_probe_end(void) {}
extern "C" int arvos_render_quality(double *) { return 0; }
static void apply_audio_to_game(const ar::AudioSettings &a) {
    if (!has_levels()) return;
    ar_adapter_sm64_set_player_focus(a.player_focus);
    ar_adapter_sm64_set_object_focus(a.object_focus);
    ar_adapter_sm64_set_music_ring(a.music_ring, a.ring_radius, a.ring_rotate);
    ar_adapter_sm64_set_ring_listener(a.ring_at_head, a.ring_focus);
}
// A game booted in SOURCES (1) whose stream then fell back to stereo shows 0.
static int shown_game_audio_state(int game_state) {
    AppState &s = state();
    return game_state == 1 && s.session ? ar::shown_audio_state(game_state, s.session->audio_spatial()) : game_state;
}
extern "C" int arvos_sm64_audio_state(void) { return is_sm64_session() ? shown_game_audio_state(ar_adapter_sm64_audio_state()) : -1; }
extern "C" int arvos_audio_key_count(void) { return (int)ar::audio_keys().size(); }
extern "C" int arvos_audio_key(int i, char *buf, int cap) {
    const auto &k = ar::audio_keys();
    return i >= 0 && i < (int)k.size() ? copy_out(k[(size_t)i], buf, cap) : 0;
}
extern "C" int arvos_audio_default(const char *key, char *buf, int cap) {
    std::string v;
    return key && ar::audio_default(key, &v) ? copy_out(v, buf, cap) : 0;
}
extern "C" int arvos_audio_option(const char *key, char *buf, int cap) {
    if (!key || !state().options_ok) return 0;
    const std::string *last = nullptr;
    for (const auto &kv : state().opts.audio_set) if (kv.first == key) last = &kv.second;
    return last ? copy_out(*last, buf, cap) : 0;
}
extern "C" int arvos_audio_preset_count(void) { return (int)ar::audio_preset_names().size(); }
extern "C" int arvos_audio_preset_name(int i, char *buf, int cap) {
    const auto &n = ar::audio_preset_names();
    return i >= 0 && i < (int)n.size() ? copy_out(n[(size_t)i], buf, cap) : 0;
}
extern "C" int arvos_audio_preset_value(const char *preset, const char *key, char *buf, int cap) {
    std::vector<std::pair<std::string, std::string>> kv;
    if (!preset || !key || !ar::audio_preset_values(preset, &kv)) return 0;
    for (const auto &p : kv) if (p.first == key) return copy_out(p.second, buf, cap);
    return 0;
}
extern "C" int arvos_apply_audio_preset(const char *preset) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::string err, line;
    if (!preset || !ar::audio_preset_values(preset, &kv) || !state().audio.set_many(kv, &err)) {
        ar::vos_log("AR_SETTING audio.preset rejected value=%s%s", preset ? preset : "(null)", err.empty() ? "" : (" (" + err + ")").c_str());
        return 0;
    }
    apply_audio_to_game(state().audio.get());
    for (const auto &p : kv) line += " " + p.first + "=" + p.second;
    ar::vos_log("AR_SETTING audio.preset=%s%s", preset, line.c_str());
    for (const auto &p : kv) pref_set("audio." + p.first, p.second);   // AudioModel.applyPreset
    return 1;
}
static int apply_audio(const char *key, const char *value, int final_value) {
    if (!key || !value) return 0;
    std::string err;
    if (!state().audio.set(key, value, &err)) {
        ar::vos_log("AR_SETTING audio.%s rejected value=%s (%s)", key, value, err.c_str());
        return 0;
    }
    apply_audio_to_game(state().audio.get());
    if (final_value) ar::vos_log("AR_SETTING audio.%s=%s", key, value);
    return 1;
}
extern "C" int arvos_set_audio(const char *key, const char *value, int final_value) {   // + AudioModel.set's save
    if (!apply_audio(key, value, final_value)) return 0;
    if (final_value) pref_set(std::string("audio.") + key, value);
    return 1;
}
extern "C" int arvos_room_light_get(arvos_room_light *out) {
    if (!out || !state().light_status.seen()) return 0;
    const ar::RoomLightStatus st = state().light_status.get();
    out->room = st.room ? 1 : 0; out->abs_lum = st.abs_lum; out->neutral = st.neutral; out->gain = st.gain;
    for (int k = 0; k < 3; k++) { out->dir[k] = st.dir[k]; out->tint[k] = st.tint[k]; }
    out->probes = st.probes;
    return 1;
}
extern "C" void arvos_push_spatial_event(const arvos_spatial_event *) {}   // the Touch controllers come through QxInput
extern "C" void arvos_set_background(int background) {
    AppState &s = state();
    s.background = background != 0;
    if (ar::QxPlatform *p = s.live.load()) p->set_paused(background != 0);
    ar::vos_log("AR_SCENE background=%d", background != 0);
}

// ---- Quest-only (qx_app.h) ----
extern "C" void qx_app_set_android(android_app *app) { state().app = app; }
extern "C" void qx_app_set_pump(void (*fn)(void *), void *ctx) { state().pump = fn; state().pump_ctx = ctx; }
extern "C" int arvos_quest_hint(char *buf, int cap) {
    AppState &s = state();
    std::string h;
    { std::lock_guard<std::mutex> g(s.m); h = s.hint; }
    if (buf && cap > 0) copy_out(h, buf, cap);
    return h.empty() ? 0 : 1;
}
extern "C" int arvos_quest_seed_look(const char *key, const char *value) {
    if (!key || !value || !qxapp::prefs()) return 0;
    const std::string k = std::string("look.") + key;
    if (pref_has(k)) return 0;
    pref_set(k, value);
    ar::vos_log("AR_SETTING look.%s seeded=%s (first run on this headset)", key, value);
    return 1;
}
extern "C" void arvos_quest_request_scan(void) {
    ar::vos_log("AR_UI scan room");
    if (ar::QxPlatform *p = state().live.load()) p->request_scene_capture();
    else state().scan_pending = true;
}
static const char *const kTabs[] = {"Level", "Look", "Audio", "World"};
static const char *tab_named(const std::string &name) {   // StatusTab's raw values; --ui-tab is matched without case
    for (const char *t : kTabs) {
        if (name.size() != std::strlen(t)) continue;
        bool same = true;
        for (size_t i = 0; i < name.size() && same; i++) same = std::tolower((unsigned char)name[i]) == std::tolower((unsigned char)t[i]);
        if (same) return t;
    }
    return nullptr;
}
extern "C" int arvos_quest_ui_tab(char *buf, int cap) {
    const AppState &s = state();
    const char *t = tab_named(s.ui_tab_arg.substr(0, s.ui_tab_arg.find(':')));
    if (!t && qxapp::prefs()) {   // StatusTab(rawValue:) is exact
        const std::string stored = qxapp::prefs()->get("ui.tab");
        for (const char *x : kTabs) if (stored == x) t = x;
    }
    copy_out(t ? t : "Level", buf, cap);   // the default shown when none is stored
    return t ? 1 : 0;
}
extern "C" void arvos_quest_set_ui_tab(const char *tab) {
    for (const char *x : kTabs) if (tab && std::strcmp(tab, x) == 0) { pref_set("ui.tab", x); return; }
}
extern "C" void arvos_quest_request_quit(void) {
    ar::vos_log("AR_APP quit requested");
    state().quit = true;
}

// ---- SM64ARApp.init's load sequence (the loads are in quest_prefs_glue.cpp) ----
int qxapp::look_apply(const char *key, const char *value, int final_value) { return apply_look(key, value, final_value); }
int qxapp::sm64_apply(const char *key, const char *value) { return apply_sm64_setting(key, value); }
int qxapp::audio_apply(const char *key, const char *value, int final_value) { return apply_audio(key, value, final_value); }
static void poll_packs() {   // PacksModel.startPolling's refresh
    AppState &s = state();
    if (s.packs_known || !has_levels() || arvos_pack_count() < 0) return;
    s.packs_known = true;
    qxapp::load_packs();
}
extern "C" int qx_app_startup(void) {
    AppState &s = state();
    if (!s.options_ok) { ar::vos_log("AR_ERROR startup skipped: %s", s.error.c_str()); return 0; }
    if (!qxapp::prefs()) qxapp::prefs_open(s.docs + "/prefs.json");   // normally opened by arvos_configure
    // ShellModel.loadGame has nothing to load here (arvos_game, arvos_has_draw_all_rooms are read by the panel).
    // ShellModel.loadSM64World:
    using namespace qxapp;
    load_start_in(); load_draw_all_rooms(); load_hide_outside(); load_other_rooms(); load_tree_style(); load_mario_cm();
    load_billboards(); load_cannon_visible(); load_sm64_settings();
    load_look();       // LookModel.load
    poll_packs();      // PacksModel.startPolling (the frame loop keeps polling until the list is known)
    load_audio();      // AudioModel.load
    prefs_save("startup");
    return 1;
}

// ---- The settings panel's bridge (QxSession draws the panel; panel_model.h builds it from these) ----
// The Level tab's own state (LevelPicker's @State): the picked level (-1: follow the current one) and act, and the note
// under Go / Play from the start.
static struct PanelLevelUi { int level = -1, act = 0, going = -1; bool saw_pending = false; std::string note; } g_level_ui;
static std::string text_of(int (*fn)(char *, int)) {
    char b[512];
    return fn(b, (int)sizeof b) ? std::string(b) : std::string();
}
static int picked_level() {
    if (g_level_ui.level >= 0) return g_level_ui.level;
    arvos_level_view v{};
    return arvos_level_view_get(&v) && v.current >= 0 ? v.current : 0;
}
static std::string level_note() {   // LevelPicker.follow: the warp's note clears on arrival
    arvos_level_view v{};
    if (g_level_ui.going >= 0 && arvos_level_view_get(&v)) {
        if (v.pending) g_level_ui.saw_pending = true;
        else if (v.current == g_level_ui.going || g_level_ui.saw_pending) {
            g_level_ui.note = v.current == g_level_ui.going ? "" : "landed elsewhere";
            g_level_ui.going = -1;
        }
    }
    return g_level_ui.note;
}
static std::string now_line() {   // LevelNowLine
    if (!has_levels()) return "";
    arvos_level_view v{};
    if (!arvos_level_view_get(&v)) return "Now: starting";
    if (v.pending) return "Now: warping...";
    const bool acts = v.current >= 0 && arvos_level_has_acts(v.current);
    return "Now: " + std::string(v.current_name) + (v.act > 0 && acts ? ", act " + std::to_string(v.act) : "");
}
static std::string num(double v) { char b[32]; std::snprintf(b, sizeof b, "%g", v); return b; }
static bool same_value(const std::string &a, const std::string &b) {   // AudioModel.currentPreset's comparison
    if (a == b) return true;
    char *ea = nullptr, *eb = nullptr;
    const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
    return ea != a.c_str() && eb != b.c_str() && std::fabs(x - y) < 1e-6;
}
static std::vector<std::string> pack_names(int kind) {   // PacksModel.refresh's order
    static const char *const preferred[] = {"SM64 Reloaded", "Render96 HD", "Render96 Models (Balanced)", "Render96 Models", "SwagSkeleton95"};
    std::vector<std::pair<int, std::string>> list;
    const int n = arvos_pack_count();
    for (int i = 0; i < n; i++) {
        char name[128];
        if (arvos_pack_info(i, name, sizeof name) != kind) continue;
        int rank = 5;
        for (int r = 0; r < 5; r++) if (std::strcmp(name, preferred[r]) == 0) rank = r;
        list.emplace_back(rank * 1000 + (int)list.size(), name);
    }
    std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::string> out;
    for (const auto &p : list) out.push_back(p.second);
    return out;
}
static std::string panel_get(const std::string &id) {
    AppState &s = state();
    auto has = [&](const char *prefix) { return id.compare(0, std::strlen(prefix), prefix) == 0; };
    if (id == "ui.tab") { char b[16]; arvos_quest_ui_tab(b, sizeof b); return b; }
    if (id == "level.start_in") return s.start_in ? "story" : "level";
    if (id == "level.level") return std::to_string(picked_level());
    if (id == "level.act") return std::to_string(g_level_ui.act);
    if (id == "level.has_acts") return arvos_level_has_acts(picked_level()) ? "1" : "0";
    if (id == "level.note") return level_note();
    if (id == "look.room_light") {   // RoomLightText
        arvos_room_light i{};
        if (!arvos_room_light_get(&i)) return "room light: -";
        char b[128];
        std::snprintf(b, sizeof b, "%s - brightness %.2fx - probes %llu", i.room ? "room" : "fixed key light", (double)i.gain, i.probes);
        return b;
    }
    if (id == "look.packs_tex_on" || id == "look.packs_models_on") return g_packs[id == "look.packs_tex_on" ? SM64_PACK_TEXTURES : SM64_PACK_MODELS].on ? "1" : "0";
    if (id == "look.packs_tex" || id == "look.packs_models") return g_packs[id == "look.packs_tex" ? SM64_PACK_TEXTURES : SM64_PACK_MODELS].name;
    if (id == "audio.preset") {
        const ar::AudioSettings a = s.audio.get();
        for (const std::string &p : ar::audio_preset_names()) {
            std::vector<std::pair<std::string, std::string>> kv;
            if (!ar::audio_preset_values(p, &kv)) continue;
            bool all = true;
            for (const auto &x : kv) all = all && same_value(ar::audio_get(a, x.first), x.second);
            if (all) return p;
        }
        return "";
    }
    if (id == "sm64.audio_state") return std::to_string(arvos_sm64_audio_state());
    if (has("look.")) return ar::look_get(s.look.get(), id.substr(5));
    if (has("audio.")) return ar::audio_get(s.audio.get(), id.substr(6));
    if (has("sm64.")) {
        char b[64];
        return has_levels() && ar_adapter_sm64_get_setting(id.c_str() + 5, b, sizeof b) ? std::string(b) : std::string();
    }
    if (!has_levels()) return "";
    if (id == "world.mario_cm") return num(std::round(arvos_mario_cm() * 10.0) / 10.0);
    if (id == "world.mario_cm_readout") { char b[96]; return arvos_scale_readout(arvos_mario_cm(), b, sizeof b) ? std::string(b) : std::string(); }
    if (id == "world.draw_all_rooms") return arvos_draw_all_rooms() == 1 ? "1" : "0";
    if (id == "world.hide_outside") return ar_adapter_sm64_hide_outside() ? "1" : "0";
    if (id == "world.other_rooms") return std::to_string(arvos_other_rooms());
    if (id == "world.billboards") return std::to_string(ar_adapter_sm64_billboard_style());
    if (id == "world.cannon_visible") return arvos_cannon_visible() == 1 ? "1" : "0";
    if (id == "world.tree_style") return std::to_string(ar_adapter_sm64_tree_style());
    if (id == "world.trees_3d") return arvos_trees_3d() ? "1" : "0";
    return "";
}
static bool panel_set(const std::string &id, const std::string &v, bool final_value) {
    auto has = [&](const char *prefix) { return id.compare(0, std::strlen(prefix), prefix) == 0; };
    const int iv = std::atoi(v.c_str());
    if (id == "ui.tab") { arvos_quest_set_ui_tab(v.c_str()); return true; }
    if (id == "level.start_in") return arvos_set_start_in(v == "story") != 0;
    if (id == "level.level") { g_level_ui.level = iv; g_level_ui.note.clear(); return true; }
    if (id == "level.act") { g_level_ui.act = std::min(std::max(iv, 0), 6); return true; }
    if (id == "look.packs_tex_on" || id == "look.packs_models_on") {
        const int kind = id == "look.packs_tex_on" ? SM64_PACK_TEXTURES : SM64_PACK_MODELS;
        arvos_set_packs(kind, v == "1", g_packs[kind].name.c_str());
        return true;
    }
    if (id == "look.packs_tex" || id == "look.packs_models") {
        const int kind = id == "look.packs_tex" ? SM64_PACK_TEXTURES : SM64_PACK_MODELS;
        arvos_set_packs(kind, g_packs[kind].on, v.c_str());
        return true;
    }
    if (id == "audio.preset") return arvos_apply_audio_preset(v.c_str()) != 0;
    if (has("look.")) return arvos_set_look(id.c_str() + 5, v.c_str(), final_value ? 1 : 0) != 0;
    if (has("audio.")) return arvos_set_audio(id.c_str() + 6, v.c_str(), final_value ? 1 : 0) != 0;
    if (has("sm64.")) return arvos_sm64_setting_set(id.c_str() + 5, v.c_str()) != 0;
    if (id == "world.mario_cm") { arvos_set_mario_cm(std::atof(v.c_str()), final_value ? 1 : 0); return true; }
    if (id == "world.draw_all_rooms") { arvos_set_draw_all_rooms(v == "1"); return true; }
    if (id == "world.hide_outside") { arvos_set_hide_outside(v == "1"); return true; }
    if (id == "world.other_rooms") { arvos_set_other_rooms(iv); return true; }
    if (id == "world.billboards") { arvos_set_billboards(iv); return true; }
    if (id == "world.cannon_visible") { arvos_set_cannon_visible(v == "1"); return true; }
    if (id == "world.tree_style") { arvos_set_tree_style(iv); return true; }
    ar::vos_log("AR_PANEL unknown setting %s", id.c_str());
    return false;
}
static bool panel_press(const std::string &id) {
    if (id == "level.go") {   // LevelPicker's Go
        const int i = picked_level(), act = arvos_level_has_acts(i) ? g_level_ui.act : 0;
        char name[64] = "";
        arvos_level_name(i, name, sizeof name);
        if (arvos_select_level(i, act)) {
            g_level_ui.going = i; g_level_ui.saw_pending = false; g_level_ui.level = -1;
            g_level_ui.note = std::string("warping to ") + name + "...";
            return true;
        }
        g_level_ui.going = -1;
        g_level_ui.note = "not changed (reason in the status below)";
        return false;
    }
    if (id == "level.play_from_start") {
        const bool ok = arvos_play_from_start() != 0;
        g_level_ui.note = ok ? "to the title..." : "not now (reason in the status below)";
        return ok;
    }
    if (id == "look.neutral_lum") {   // "Set as neutral"
        arvos_room_light i{};
        return arvos_room_light_get(&i) && i.abs_lum > 0.f && arvos_set_look("neutral_lum", num(i.abs_lum).c_str(), 1);
    }
    if (id == "look.neutral_reset") return arvos_set_look("neutral_lum", "0", 1) != 0;
    if (id == "table.replace") { arvos_replace_table(); return true; }
    if (id == "table.scan") { arvos_quest_request_scan(); return true; }
    if (id == "app.quit") { arvos_quest_request_quit(); return true; }   // the loop ends after this frame
    ar::vos_log("AR_PANEL unknown button %s", id.c_str());
    return false;
}
static ar::panel::Bridge make_panel_bridge() {
    ar::panel::Bridge b;
    b.get = panel_get;
    b.set = panel_set;
    b.press = panel_press;
    b.has_levels = [] { return has_levels(); };
    b.locked = [] { return !state().game_lock.why().empty(); };
    b.names = [](const std::string &list) {
        std::vector<std::string> out;
        auto keys = [&](int n, int (*key)(int, char *, int)) {
            for (int i = 0; i < n; i++) { char k[64]; if (key(i, k, sizeof k)) out.push_back(k); }
        };
        if (list == "levels" || list == "level_sections") {
            for (int i = 0, n = arvos_level_count(); i < n; i++) {
                char b[96] = "";
                if (list == "level_sections") out.push_back(std::to_string(arvos_level_section(i)));
                else out.push_back(arvos_level_label(i, b, sizeof b) || arvos_level_name(i, b, sizeof b) ? b : "?");
            }
        } else if (list == "packs_tex") out = pack_names(SM64_PACK_TEXTURES);
        else if (list == "packs_models") out = pack_names(SM64_PACK_MODELS);
        else if (list == "audio_presets") out = ar::audio_preset_names();
        else if (list == "look_keys") out = ar::look_keys();
        else if (list == "audio_keys") out = ar::audio_keys();
        else if (list == "sm64_keys") keys(arvos_sm64_setting_count(), arvos_sm64_setting_key);
        return out;
    };
    b.status = [] {   // one line: the refusal, the status and the hint arvos_status puts on lines of their own
        std::string t = text_of(arvos_status);
        for (size_t i; (i = t.find('\n')) != std::string::npos;) t.replace(i, 1, " | ");
        return t;
    };
    b.hint = [] { return text_of(arvos_quest_hint); };
    b.now_line = now_line;
    return b;
}

// ---- The frame loop ----
extern "C" void arvos_layer_run(void *) {
    AppState &s = state();
    std::lock_guard<std::mutex> run(s.run);
    if (!s.options_ok) { ar::vos_log("AR_ERROR not starting: %s", s.error.c_str()); return; }
    if (!s.app) { ar::vos_log("AR_ERROR not starting: no android_app (qx_app_set_android)"); return; }
    s.loop_thread = std::this_thread::get_id();
    struct LoopEnd { ~LoopEnd() { state().loop_thread = std::thread::id(); } } loop_end;
    s.quit = false;
    auto pump = [&] { if (s.pump) s.pump(s.pump_ctx); };
    if (!s.platform) {
        ar::QxPlatform::Config pc;
        pc.passthrough = true;
        pc.refresh_hz = 90.f;
        // The room scan's table (remembered at placement.json) unless --fixed-table asks for plan 1's fixed one.
        pc.fixed_table = ar_pose_identity();
        pc.fixed_table.pos[1] = 0.75f;   // table height above the floor; in LOCAL space the platform moves it 1.5 m down
        pc.fixed_table.pos[2] = -1.0f;
        pc.fixed_table_valid = s.fixed_table;
        pc.record_path = s.docs + "/placement.json";
        s.platform.reset(new ar::QxPlatform(s.app, pc));
        s.app->userData = s.platform.get();
    }
    s.platform->set_paused(s.background);
    std::string err;
    const bool running = s.platform->start(&err);
    if (!running) {
        ar::vos_log("AR_QUEST_FAIL %s", err.c_str());
        set_status("error: " + err);
    } else {
        s.live = s.platform.get();
        if (s.replace_pending.exchange(false)) s.platform->replace_table();
        if (s.scan_pending.exchange(false)) s.platform->request_scene_capture();
    }
    // The adapter and its setup, as vos_app.mm; a refused game falls back to the cube (plan 2), so passthrough and the
    // log line saying why stay up.
    const DataDirs data = data_dirs();
    ar::vos_log("AR_DATA source=%s rom=%s res=%s packs=%s", data.source.c_str(), data.rom.c_str(), data.res.c_str(), data.packs.c_str());
    std::string aerr, setup;
    const ar_adapter *a = running ? pick_adapter(s.opts.game, data, &aerr) : nullptr;
    if (a && !ar::resolve_setup(ar::join_game_args(s.opts.game_args), "", &setup, &aerr, false)) a = nullptr;
    if (a && s.opts.game == "sm64") {
        std::string lock;
        const bool locked = !ar::game_setting_change_allowed(s.opts, &lock);   // never on the Quest (no --sarec / --record)
        if (locked) s.game_lock.lock(lock);
        const char *src = "saved";
        const int story = sm64_start_resolve(ar::join_game_args(s.opts.game_args).c_str(), s.start_in, &src);
        if (story && sm64_setup_start(setup.c_str()) < 0) setup = ar::setup_with_items(setup, "start=story");
        ar::vos_log("AR_STORY start=%s source=%s", story ? "story" : "level", src);
        char why[160];
        if (!ar_adapter_sm64_setup(setup.c_str(), why, sizeof why)) { aerr = std::string("setup: ") + why; a = nullptr; }
        if (a && locked) {
            ar::vos_log("AR_SETTING draw_all_rooms=%d locked (%s)", ar_adapter_sm64_draw_all_rooms(), lock.c_str());
            ar::vos_log("AR_SETTING other_rooms=%s locked (%s)", ar_adapter_sm64_other_rooms_name(ar_adapter_sm64_other_room_objects()), lock.c_str());
            ar::vos_log("AR_SETTING cannon_visible=%d locked (%s)", ar_adapter_sm64_cannon_visible(), lock.c_str());
            const float c = ar_adapter_sm64_mario_cm();
            ar::vos_log("AR_SETTING mario_cm=%.2f upm=%.0f locked (%s)", (double)c, (double)sm64_units_per_meter_for_mario_cm(c), lock.c_str());
        }
    } else if (a && !setup.empty()) {
        aerr = "game " + s.opts.game + " takes no --game-arg";
        a = nullptr;
    }
    if (running && !a) {
        set_status("error: " + aerr);
        ar::vos_log("AR_ERROR %s", aerr.c_str());
        ar::vos_log("AR_QUEST_FAIL %s; showing the cube", aerr.c_str());
        s.opts.game = "cube";   // has_levels() is false from here on: no sm64 calls into a game that never booted
        setup.clear();
        a = cube_adapter();
    }
    if (a) ar::vos_log("AR_SETUP %s", setup.c_str());
    bool game = false;
    if (a) {
        s.session.reset(new ar::QxSession(a, *s.platform, s.docs));
        s.session->set_panel_bridge(make_panel_bridge());   // before the first frame: the menu button opens the panel
        const ar::SaveFolderChoice sf = ar::save_folder_for(s.opts.game, s.opts.game == "sm64" && sm64_setup_start(setup.c_str()) == 1,
                                                            false, false);
        std::string folder, ferr;
        bool deleted = false;
        if (!ar::prepare_save_folder(s.docs, sf.name, sf.fresh, &folder, &deleted, &ferr)) {
            set_status("error: " + ferr); ar::vos_log("AR_ERROR %s", ferr.c_str());
        } else {
            if (sf.fresh) ar::vos_log("AR_STORY scratch_save=Documents/%s", folder.c_str());
            s.session->set_look_store(&s.look);
            s.session->set_audio_store(&s.audio);
            s.session->set_room_light_status(&s.light_status);
            if (s.opts.game == "sm64") {
                s.session->set_audio_choice(sm64_audio_choice);
                s.session->set_perf_window_callback(log_pack_load_if_changed, nullptr);
                s.session->set_key_light_callback([](void *, int on, const float d[3]) { ar_adapter_sm64_set_key_light(on, d); }, nullptr);
                ar_adapter_sm64_set_normal_flags(ar::look_normal_flags(s.look.get()));
                std::string lock;
                ar_adapter_sm64_set_pointer_lock(!ar::game_setting_change_allowed(s.opts, &lock));
                // The Touch trigger's ray drives sm64's title screen and menus. Logged at begin / end / cancel, never per move.
                s.session->set_pointer_claim([](const ar::PointerEvent &e) {
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
            const std::string rom = s.opts.game == "sm64" ? data.rom : std::string();
            s.opts.rom = rom;
            ar::vos_log("AR_DATA rom=%s", rom.c_str());
            game = s.session->start(rom, s.docs + "/" + folder, &err);
            if (!game) {
                if (ar::is_rom_error(err)) err = ar::rom_error_hint(err);
                set_status("error: " + err);
                ar::vos_log("AR_QUEST_FAIL %s", err.c_str());
            } else {
                ar::vos_log("AR_LAYER attached ticks=%llu", (unsigned long long)s.session->ticks_done());
            }
        }
    }

    uint64_t frames = 0;
    int last_level = -1, last_act = -1, last_pending = 1;
    int last_forced = 0, last_save = 0;
    int last_wait_s = 0;
    double last_head_log = 0.0;
    const auto opened = std::chrono::steady_clock::now();
    size_t look_tests = 0;
    std::vector<ar::VosOptions::LookTest> tests = s.opts.look_test;
    std::stable_sort(tests.begin(), tests.end(), [](const auto &x, const auto &y) { return x.at_s < y.at_s; });
    const ar_platform *api = s.platform->api();
    while (running) {
        pump();
        bool more = false;
        if (s.quit.load() || s.platform->exiting()) {
            more = false;
        } else if (game) {
            more = s.session->frame();
        } else {   // no game: keep the XR frame loop going so the passthrough layer is shown
            ar_frame_info fi;
            more = api->wait_frame(api->user, &fi);
            if (more) {
                api->begin_frame(api->user);
                s.platform->end_submission();
            }
        }
        if (!more) break;
        prefs_tick();
        const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - opened).count();
        for (; look_tests < tests.size() && since >= tests[look_tests].at_s; look_tests++) {
            const auto &t = tests[look_tests];
            std::string terr;
            const bool ok = s.look.set(t.key, t.value, &terr);
            ar::vos_log("AR_SETTING look.%s=%s reason=look-test ok=%d", t.key.c_str(), t.value.c_str(), ok ? 1 : 0);
        }
        if (++frames % 30 == 0) {
            if (game) set_status(s.session->status());
            const std::string h = s.platform->scene_hint();
            std::lock_guard<std::mutex> g(s.m);
            s.hint = h;
        }
        if (frames % 8 == 0) poll_packs();
        const ar_frame_info &fi = s.platform->last_frame();
        if (fi.head_tracked && now_s() - last_head_log >= 1.0) {
            last_head_log = now_s();
            ar::vos_log("AR_QUEST_HEAD %.3f %.3f %.3f", fi.tracking_from_head.pos[0], fi.tracking_from_head.pos[1],
                        fi.tracking_from_head.pos[2]);
        }
        sm64_level_state ls;
        if (game && s.opts.game == "sm64" && ar_adapter_sm64_level_state(&ls)) {
            if (!ls.pending && (last_pending || ls.level_num != last_level || ls.act != last_act)) {
                const sm64_level_entry *cur = sm64_level_at(ls.index), *home = sm64_level_at(ls.home_index);
                ar::vos_log("AR_LEVEL now level=%d key=%s act=%d home=%s", ls.level_num, cur ? cur->key : "-", ls.act,
                            home ? home->key : "-");
                last_level = ls.level_num; last_act = ls.act;
            }
            last_pending = ls.pending;
            const int wait_s = (ls.wait_ticks + 29) / 30;
            if (wait_s != last_wait_s && wait_s > 0)
                ar::vos_log("AR_LEVEL waiting reason=%s ticks=%d", ls.wait_reason, ls.wait_ticks);
            last_wait_s = wait_s;
            if (ls.save_count != last_save) {
                ar::vos_log("AR_SAVE course=%d star=%d", ls.save_course, ls.save_star);
                last_save = ls.save_count;
            }
            if (ls.forced_count != last_forced) {
                ar::vos_log("AR_LEVEL forced reason=%s", ls.forced_reason);
                last_forced = ls.forced_count;
            }
        }
    }
    const unsigned long long ticks = s.session ? (unsigned long long)s.session->ticks_done() : 0ull;
    ar::vos_log("AR_QUEST_STOP ticks=%llu", ticks);
    if (game) ar::vos_log("AR_STATUS %s", s.session->status().c_str());
    ar::vos_log("AR_LAYER invalidated frames=%llu ticks=%llu", (unsigned long long)frames, ticks);
    if (s.session) {
        s.session->stop();   // before the platform (its Vulkan device and XR session) goes
        if (game) set_status("table closed; " + s.session->status());
    }
    prefs_flush("loop end");
    s.live = nullptr;
    s.session.reset();
    s.app->userData = nullptr;
    s.platform.reset();
}
