#include "quest_prefs_glue.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>
#include "ar_adapter_sm64.h"
#include "ar_audio_settings.h"
#include "look.h"
#include "vos_api.h"
#include "vos_log.h"

namespace qxapp {
namespace {
// Plain globals: everything here runs on the android_main thread (configure, startup, the frame loop and the panel
// inside it); only the dirty flag is atomic. QxPrefs itself is thread-safe.
std::unique_ptr<ar::QxPrefs> g_prefs;
std::atomic<bool> g_dirty{false};
std::chrono::steady_clock::time_point g_saved_at{};
constexpr double kPrefsSaveEveryS = 0.5;
}  // namespace

void prefs_open(const std::string &path) {
    g_prefs.reset(new ar::QxPrefs(path));
    std::string err;
    if (!g_prefs->load(&err)) ar::vos_log("AR_PREFS unreadable, starting fresh: %s", err.c_str());   // the store is empty
    ar::vos_log("AR_PREFS path=%s", path.c_str());
}
ar::QxPrefs *prefs() { return g_prefs.get(); }
void pref_set(const std::string &key, const std::string &value) {
    if (!g_prefs) return;
    g_prefs->set(key, value);
    g_dirty = true;
}
void pref_remove(const std::string &key) {
    if (!g_prefs || !g_prefs->has(key)) return;
    g_prefs->remove(key);
    g_dirty = true;
}
void pref_set_bool(const std::string &key, bool v) { pref_set(key, v ? "1" : "0"); }
void pref_set_int(const std::string &key, int v) { pref_set(key, std::to_string(v)); }
void pref_set_double(const std::string &key, double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    pref_set(key, b);
}
bool pref_has(const std::string &key) { return g_prefs && g_prefs->has(key); }
// UserDefaults' object(forKey:) as? Bool / Int / Double: false when missing or not of that type.
bool pref_bool(const std::string &key, bool *out) {
    if (!pref_has(key)) return false;
    const std::string v = g_prefs->get(key);
    if (v == "1" || v == "true") { *out = true; return true; }
    if (v == "0" || v == "false") { *out = false; return true; }
    return false;
}
bool pref_int(const std::string &key, int *out) {
    if (!pref_has(key)) return false;
    const std::string v = g_prefs->get(key);
    char *end = nullptr;
    const long n = v.empty() ? 0 : std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end != 0) return false;
    *out = (int)n;
    return true;
}
bool pref_double(const std::string &key, double *out) {
    if (!pref_has(key)) return false;
    const std::string v = g_prefs->get(key);
    char *end = nullptr;
    const double d = v.empty() ? 0.0 : std::strtod(v.c_str(), &end);
    if (v.empty() || *end != 0 || !std::isfinite(d)) return false;
    *out = d;
    return true;
}
void prefs_save(const char *why) {
    if (!g_prefs) return;
    g_dirty = false;
    g_saved_at = std::chrono::steady_clock::now();
    std::string err;
    if (!g_prefs->save(&err)) {
        g_dirty = true;   // tried again at the next chance
        ar::vos_log("AR_PREFS save failed (%s): %s", why, err.c_str());
    }
}
void prefs_flush(const char *why) { if (g_dirty.load()) prefs_save(why); }
// The frame loop's debounce: at most one save per kPrefsSaveEveryS.
void prefs_tick() {
    if (!g_dirty.load()) return;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - g_saved_at).count() < kPrefsSaveEveryS) return;
    prefs_save("changed");
}

const char *pack_on_key(int kind) { return kind == SM64_PACK_TEXTURES ? "packTexturesOn" : "packModelsOn"; }
const char *pack_name_key(int kind) { return kind == SM64_PACK_TEXTURES ? "packTextures" : "packModels"; }

// The world settings of ShellModel.loadSM64World: the launch option, else the saved value, else the default; then the
// setter (which saves, as the Swift apply functions do).
void load_start_in() {
    const int option = arvos_start_option();
    bool saved = true;
    const bool have = pref_bool("startIn", &saved);
    const bool story = option >= 0 ? option != 0 : (have ? saved : true);
    arvos_set_start_in(story ? 1 : 0);
    char source[32];
    const int effective = arvos_start_effective(source, sizeof source);
    if (effective >= 0 && (effective != 0) != story)
        ar::vos_log("AR_SETTING start_in shown=%s saved=%s source=%s", effective ? "story" : "level", story ? "story" : "level", source);
}
void load_draw_all_rooms() {
    const int option = arvos_draw_all_rooms_option();
    bool saved = true;
    const bool have = pref_bool("drawAllRooms", &saved);
    arvos_set_draw_all_rooms(option >= 0 ? option != 0 : (have ? saved : true));
}
void load_hide_outside() {
    const int option = arvos_hide_outside_option();
    bool saved = true;
    const bool have = pref_bool("hideOutside", &saved);
    arvos_set_hide_outside(option >= 0 ? option != 0 : (have ? saved : true));
}
void load_other_rooms() {
    const int option = arvos_other_rooms_option();
    int saved = 2;
    const bool have = pref_int("otherRoomObjects", &saved);
    int mode = option >= 0 ? option : (have ? saved : 2);
    if (mode < 0 || mode > 2) mode = 2;
    arvos_set_other_rooms(mode);
}
void load_tree_style() {
    const int option = arvos_tree_style_option();
    int saved = 1;
    const bool have = pref_int("treeStyle", &saved);
    int style = option >= 0 ? option : (have ? saved : 1);
    if (style < 0 || style > 1) style = 1;
    arvos_set_tree_style(style);
}
void load_mario_cm() {
    const double option = arvos_mario_cm_option();
    double saved = 2.0;
    const bool have = pref_double("marioCm", &saved);
    arvos_set_mario_cm(option > 0 ? option : (have ? saved : 2.0), 1);
}
void load_billboards() {
    const int option = arvos_billboards_option();
    int saved = 1;
    const bool have = pref_int("billboards", &saved);
    int style = option >= 0 ? option : (have ? saved : 1);
    if (style < 0 || style > 1) style = 1;
    arvos_set_billboards(style);
}
void load_cannon_visible() {
    const int option = arvos_cannon_visible_option();
    bool saved = true;
    const bool have = pref_bool("cannonVisible", &saved);
    arvos_set_cannon_visible(option >= 0 ? option != 0 : (have ? saved : true));
}
static std::string text_of(int (*fill)(const char *, char *, int), const std::string &key) {
    char b[256];
    return fill(key.c_str(), b, (int)sizeof b) ? std::string(b) : std::string();
}
static bool has_text(int (*fill)(const char *, char *, int), const std::string &key, std::string *out) {
    char b[256];
    if (!fill(key.c_str(), b, (int)sizeof b)) return false;
    *out = b;
    return true;
}
// LookLoad.swift's loadLookValues over prefs.json: qx_load_values, then the caller's part (qx_prefs.h): apply each
// value; a launch value that applied is stored; one that did not falls back to the default, and a stored value that
// did not is removed. The drops qx_load_values made are logged as LookLoad.swift logs them.
static void load_values(const char *prefix_dot, const std::vector<std::string> &keys, int (*default_fill)(const char *, char *, int),
                        int new_rev, const std::function<bool(const std::string &, const std::string &, int, bool)> &drop,
                        int (*launch_fill)(const char *, char *, int), int (*apply)(const char *, const char *)) {
    const std::string p = prefix_dot;
    std::vector<std::pair<bool, std::string>> before;
    for (const auto &k : keys) before.emplace_back(g_prefs->has(p + k), g_prefs->get(p + k));
    const auto results = ar::qx_load_values(*g_prefs, p, keys,
        [&](const std::string &k, std::string *v) { return has_text(default_fill, k, v); }, new_rev, drop,
        [&](const std::string &k, std::string *v) { return has_text(launch_fill, k, v); });
    for (size_t i = 0; i < keys.size(); i++)
        if (before[i].first && !g_prefs->has(p + keys[i]))
            ar::vos_log("AR_SETTING %s%s stored %s was the old default: dropped (defaults rev %d)", prefix_dot, keys[i].c_str(),
                        before[i].second.c_str(), new_rev);
    for (const auto &r : results) {
        if (apply(r.key.c_str(), r.value.c_str())) {
            if (r.from_launch) g_prefs->set(p + r.key, r.value);
            continue;
        }
        if (!r.from_launch) g_prefs->remove(p + r.key);
        apply(r.key.c_str(), text_of(default_fill, r.key).c_str());
    }
    g_dirty = true;   // qx_app_startup saves once at its end
}
static int look_apply_final(const char *k, const char *v) { return look_apply(k, v, 1); }
void load_sm64_settings() {   // SM64LookModel.load
    std::vector<std::string> keys;
    for (int i = 0; i < arvos_sm64_setting_count(); i++) {
        char b[128];
        if (arvos_sm64_setting_key(i, b, sizeof b)) keys.push_back(b);
    }
    load_values("sm64.", keys, arvos_sm64_setting_default, arvos_sm64_settings_revision(),
                [](const std::string &k, const std::string &st, int rev, bool launch) {
                    return arvos_sm64_setting_drop_stored(k.c_str(), st.c_str(), rev, launch ? 1 : 0) != 0;
                },
                arvos_game_setting_option, sm64_apply);
}
void load_look() {   // LookModel.load
    load_values("look.", ar::look_keys(), arvos_look_default, arvos_look_defaults_rev(),
                [](const std::string &k, const std::string &st, int rev, bool launch) {
                    return arvos_look_drop_stored(k.c_str(), st.c_str(), rev, launch ? 1 : 0) != 0;
                },
                arvos_look_option, look_apply_final);
}
// PacksModel.load for both kinds, once the pack list is known (the game lists them once it booted): the launch choice,
// else the saved one (on by default, no name), then PacksModel.apply (saves, arvos_set_packs).
void load_packs() {
    for (int kind : {SM64_PACK_TEXTURES, SM64_PACK_MODELS}) {
        char buf[160];
        bool on = true;
        std::string name;
        if (arvos_pack_option(kind, buf, sizeof buf)) {
            const std::string v = buf;
            on = v != "original";
            const size_t c = v.find(':');
            if (c != std::string::npos) name = v.substr(c + 1);
        } else {
            bool saved = true;
            if (pref_bool(pack_on_key(kind), &saved)) on = saved;
            if (g_prefs) name = g_prefs->get(pack_name_key(kind));
        }
        arvos_set_packs(kind, on ? 1 : 0, name.c_str());
    }
}
void load_audio() {   // AudioModel.load
    std::vector<std::pair<std::string, std::string>> before;
    for (const auto &k : ar::audio_keys()) before.emplace_back(k, g_prefs->get("audio." + k));
    const int have = g_prefs->get_int("audio.defaults_version", 0);
    ar::qx_migrate_audio_defaults(*g_prefs);
    if (g_prefs->get_int("audio.defaults_version", 0) != have) g_dirty = true;
    for (const auto &b : before)
        if (!b.second.empty() && !g_prefs->has("audio." + b.first))
            ar::vos_log("AR_SETTING audio.%s=%s was the old default: now the new one (defaults_version %d)", b.first.c_str(),
                        b.second.c_str(), b.first == "ring_focus" ? 1 : 2);   // qx_migrate_audio_defaults' changes: 1 ring_focus, 2 music
    for (const auto &key : ar::audio_keys()) {
        const std::string def = text_of(arvos_audio_default, key);
        std::string value;
        if (!has_text(arvos_audio_option, key, &value)) value = g_prefs->has("audio." + key) ? g_prefs->get("audio." + key) : def;
        if (!audio_apply(key.c_str(), value.c_str(), 1)) { value = def; audio_apply(key.c_str(), value.c_str(), 1); }
        pref_set("audio." + key, value);
    }
}

}  // namespace qxapp
