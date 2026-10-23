// The Quest settings panel's model: the visionOS settings window (SM64ARApp.swift: Level, Look, Audio, World) as plain
// data built from a Bridge of std::functions (the app fills it with the arvos_* calls; the tests with a map), the panel
// layout's hit test and what a controller input does to the hovered control. Pure: no arvos_* symbol, no I/O.
#pragma once
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace ar {
namespace panel {

enum class Kind { segmented, toggle, slider, button, picker, stepper, text };
// options: segmented / picker values; a toggle's {off value, on value} ("0"/"1", "off"/"on", "show"/"hide", ...).
// labels: what is shown for each option (empty: the options themselves). value: the current value as the Bridge's get
// returns it. unit: shown after a slider's value. log_scale: a slider moved and drawn in log(value) (Mario size).
struct Control {
    Kind kind; std::string id, label; std::vector<std::string> options; float min = 0, max = 1, step = 0.05f; bool enabled = true;
    std::string note;
    std::string value; std::vector<std::string> labels; std::string unit; bool log_scale = false;
};
struct Tab { std::string name; std::vector<Control> controls; };
struct Bridge {   // the arvos_* calls behind the model, as function objects so tests can fake them
    std::function<std::string(const std::string &id)> get;             // current value as text ("1"/"0", option name, number)
    std::function<bool(const std::string &id, const std::string &v, bool final)> set;
    std::function<bool()> has_levels; std::function<bool()> locked;     // sm64 present; game lock (always false on Quest)
    // "levels", "level_sections", "packs_tex", "packs_models", "audio_presets", "look_keys", "audio_keys", "sm64_keys"
    std::function<std::vector<std::string>(const std::string &list)> names;
    std::function<std::string()> status; std::function<std::string()> hint; std::function<std::string()> now_line;
    // The buttons: level.go, level.play_from_start, look.neutral_lum (set as neutral), look.neutral_reset,
    // table.replace, table.scan, app.quit.
    std::function<bool(const std::string &id)> press;
};
// The four tabs (Level, Look, Audio, World; World only when has_levels) with the controls of SM64ARApp.swift in its
// order; ids are the setting keys with their prefixes (look.*, audio.*, sm64.*, world.*, level.*, table.*, app.*), text
// rows (headings, captions) "text.*". Each control carries its current value and enable rule.
std::vector<Tab> build_tabs(const Bridge &b);

struct Layout { uint32_t width = 1024, height = 768; float row_h = 36.f, margin = 24.f; };
// The layout's fixed parts, in texture pixels (y down): the tab strip on top, the rows, the scroll column at the right
// of the rows, the footer (status and hint) at the bottom; a row's widget area right of its label.
constexpr float kTabStripH = 56.f, kFooterH = 72.f, kScrollW = 44.f, kLabelW = 420.f, kValueW = 110.f;
float content_top(const Layout &L);
float content_bottom(const Layout &L);
int visible_rows(const Layout &L);
void widget_span(const Layout &L, float *x0, float *x1);       // a row's widget area
void slider_span(const Layout &L, float *x0, float *x1);       // a slider's bar (the value text right of it)
void scroll_span(const Layout &L, float *x0, float *x1);       // the scroll column
int max_scroll(const std::vector<Tab> &tabs, int tab, const Layout &L);

// t: 0..1 along a slider's bar / the widget area of the other kinds. scroll: -1 / +1 on the scroll column's up / down half.
struct Hit { int tab = -1; int control = -1; float t = 0.f; bool tab_strip = false; int scroll = 0; };
Hit hit_test(const std::vector<Tab> &tabs, int tab, const Layout &L, float u, float v, int scroll_rows);   // u, v in 0..1 panel space
bool same_hit(const Hit &a, const Hit &b);   // the same thing hovered (t ignored)

// set: value for id (final false while the stick slides; raw is the unrounded slider value to continue from).
// scroll: delta rows. select_tab: tab (and its name in value).
struct Action { enum Type { none, select_tab, set, press, scroll } kind = none; std::string id, value; int tab = -1;
                bool final = true; float raw = 0.f; int delta = 0; };
// trigger_edge: the trigger was pulled this frame. Toggle: flip; segmented: the option under t; picker / stepper: the
// left quarter steps back, the rest forward; slider: the value under t; button: press; tab strip: select it; scroll
// column: a page. stick_x (dead zone applied by the caller) moves a hovered slider by stick_x * (max - min) * 0.5 per
// second (in log space for log_scale) from its value, rounded to its step; raw is the unrounded result, which the caller
// passes back as the value next frame so a slow push accumulates. Disabled: none.
Action act_on(const std::vector<Tab> &tabs, int tab, const Hit &h, bool trigger_edge, float stick_x, float dt_s);

std::string format_value(float v);                       // SwiftUI's LookSlider.format: %g of v rounded to 1/1000
float snap_value(const Control &c, float v);              // clamped to min..max, rounded to the step from min
std::string option_label(const Control &c, size_t i);
int option_index(const Control &c);                       // the value's index in options, -1 when none

}  // namespace panel
}  // namespace ar
