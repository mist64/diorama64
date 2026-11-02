// The Quest bridge's persistence (quest_app.cpp's half that stands in for UserDefaults and the Swift models' load
// functions): prefs.json through QxPrefs, and SM64ARApp.init's load sequence over it. android_main thread.
#pragma once
#include <string>
#include "qx_prefs.h"

namespace qxapp {

// prefs.json. pref_* change the store and mark it dirty; prefs_tick (the frame loop) saves at most every 0.5 s,
// prefs_save at once, prefs_flush only when dirty. Bools are stored "1" / "0" ("true" / "false" read too).
void prefs_open(const std::string &path);   // a corrupt file is logged and the store starts empty
ar::QxPrefs *prefs();                        // nullptr before prefs_open
void pref_set(const std::string &key, const std::string &value);
void pref_remove(const std::string &key);
void pref_set_bool(const std::string &key, bool v);
void pref_set_int(const std::string &key, int v);
void pref_set_double(const std::string &key, double v);
bool pref_has(const std::string &key);
bool pref_bool(const std::string &key, bool *out);     // false when missing or not of that type (as? Bool)
bool pref_int(const std::string &key, int *out);
bool pref_double(const std::string &key, double *out);
void prefs_save(const char *why);
void prefs_flush(const char *why);
void prefs_tick();
const char *pack_on_key(int kind);     // packTexturesOn / packModelsOn
const char *pack_name_key(int kind);   // packTextures / packModels

// The bridge's setters without their save (quest_app.cpp): the load sequence applies through these and does the
// caller's part of qx_load_values itself.
int look_apply(const char *key, const char *value, int final_value);
int sm64_apply(const char *key, const char *value);
int audio_apply(const char *key, const char *value, int final_value);

// SM64ARApp.init's loads, in its order: ShellModel.loadSM64World (load_start_in .. load_cannon_visible,
// load_sm64_settings), LookModel.load (load_look), PacksModel.load (load_packs, once the pack list is known) and
// AudioModel.load (load_audio).
void load_start_in();
void load_draw_all_rooms();
void load_hide_outside();
void load_other_rooms();
void load_tree_style();
void load_mario_cm();
void load_billboards();
void load_cannon_visible();
void load_sm64_settings();
void load_look();
void load_packs();
void load_audio();

}  // namespace qxapp
