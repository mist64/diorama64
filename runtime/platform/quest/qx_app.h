// The Quest side of the app bridge (apps/quest/quest_app.cpp): what vos_api.h does not cover. Everything runs on the
// android_main thread, which also runs arvos_layer_run's frame loop (and the panel inside it), unless noted.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct android_app;
// Before arvos_layer_run: the activity the platform is made for. The bridge sets app->userData to the live QxPlatform
// (nullptr when there is none), so android_main's onAppCmd can forward to QxPlatform::handle_cmd.
void qx_app_set_android(struct android_app *app);
// Called once per frame by arvos_layer_run's loop, before the frame (and while it waits after a failed start): pumps
// the Android looper without blocking.
void qx_app_set_pump(void (*fn)(void *ctx), void *ctx);
// After arvos_configure: SM64ARApp.init's sequence (loadGame, the world settings, the sm64 settings and the Look values
// through qx_load_values, the packs, the audio values) over prefs.json, then one save. 0 when arvos_configure failed.
int  qx_app_startup(void);
// The placer's hint ("" when the table is placed or nothing is wrong); 1 when non-empty.
int  arvos_quest_hint(char *buf, int cap);
// Stores look.<key> = value only when look.<key> is not stored yet (a first-run value that the player's own choice
// always beats); before qx_app_startup loads the Look. 1 when written.
int  arvos_quest_seed_look(const char *key, const char *value);
// Space Setup (scene capture), then the room scan is read again; now or once the platform runs.
void arvos_quest_request_scan(void);
// The panel's tab, persisted as ui.tab (--ui-tab NAME wins at launch, as StatusTab.initial). 1 when known.
int  arvos_quest_ui_tab(char *buf, int cap);
void arvos_quest_set_ui_tab(const char *tab);
// Safe from inside the frame loop (the panel's Quit): the loop ends after this frame, the session stops, the
// preferences are flushed and arvos_layer_run returns; android_main then finishes the activity. arvos_prepare_quit
// waits for the loop and so must not be called from the loop's own thread (it returns 0 at once there).
void arvos_quest_request_quit(void);
#ifdef __cplusplus
}
#endif
