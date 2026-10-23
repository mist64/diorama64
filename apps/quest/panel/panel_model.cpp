// The settings panel's tabs, from SM64ARApp.swift's LevelPicker / StatusView (Level), LookSection + PacksGroup (Look),
// SM64SoundRows + AudioSection (Audio) and StatusView.worldControls (World), in their order and with their enable rules.
#include "panel_model.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ar {
namespace panel {
namespace {
using Opts = std::vector<std::string>;

// Appends the controls of one tab; every control's value is the Bridge's get(id) at build time.
struct Rows {
    const Bridge &b;
    std::vector<Control> out;
    explicit Rows(const Bridge &br) : b(br) {}
    std::string get(const std::string &id) const { return b.get ? b.get(id) : std::string(); }
    Control &add(Control c) {
        if (c.kind != Kind::text && c.kind != Kind::button) c.value = get(c.id);
        out.push_back(std::move(c));
        return out.back();
    }
    Control &text(const std::string &id, const std::string &label) { return add(Control{Kind::text, "text." + id, label}); }
    Control &heading(const std::string &label) {
        std::string id = label;
        for (char &ch : id) ch = std::isalnum((unsigned char)ch) ? (char)std::tolower((unsigned char)ch) : '_';
        return text("head." + id, label);
    }
    Control &seg(const std::string &id, const std::string &label, Opts values, Opts labels = {}) {
        Control c{Kind::segmented, id, label, std::move(values)};
        c.labels = std::move(labels);
        return add(std::move(c));
    }
    Control &toggle(const std::string &id, const std::string &label, const char *off = "0", const char *on = "1") {
        return add(Control{Kind::toggle, id, label, {off, on}});
    }
    Control &slider(const std::string &id, const std::string &label, float lo, float hi, float step, const char *unit = "") {
        Control c{Kind::slider, id, label, {}, lo, hi, step};
        c.unit = unit;
        return add(std::move(c));
    }
    Control &button(const std::string &id, const std::string &label) { return add(Control{Kind::button, id, label}); }
    Control &picker(const std::string &id, const std::string &label, Opts values, Opts labels = {}) {
        Control c{Kind::picker, id, label, std::move(values)};
        c.labels = std::move(labels);
        return add(std::move(c));
    }
};
Opts names_of(const Bridge &b, const std::string &list) { return b.names ? b.names(list) : Opts{}; }
bool flag(const std::function<bool()> &f) { return f && f(); }
std::string capitalized(std::string s) {
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

std::vector<Control> level_tab(const Bridge &b) {
    Rows r(b);
    const bool levels = flag(b.has_levels), locked = flag(b.locked);
    const std::string now = b.now_line ? b.now_line() : std::string();
    if (!now.empty()) r.text("now", now);
    if (levels) {
        r.seg("level.start_in", "Start in", {"story", "level"}, {"Story", "Level"}).enabled = !locked;
        const std::string note = r.get("level.start_note");
        r.text("start_in", !note.empty() ? note : "Story: title, file select and the normal game. Applies at the next launch.");
        r.button("level.play_from_start", "Play from the start").enabled = !locked;
        const Opts names = names_of(b, "levels"), sections = names_of(b, "level_sections");
        Opts values, labels;
        for (size_t i = 0; i < names.size(); i++) {   // section 1 is the menu's "Secret & Bowser" section
            values.push_back(std::to_string(i));
            labels.push_back(i < sections.size() && sections[i] == "1" ? "Secret & Bowser: " + names[i] : names[i]);
        }
        r.picker("level.level", "Level", values, labels);
        if (r.get("level.has_acts") == "1") {
            Control c{Kind::stepper, "level.act", "Act", {"0", "1", "2", "3", "4", "5", "6"}, 0.f, 6.f, 1.f};
            c.labels = {"First without a star", "Act 1", "Act 2", "Act 3", "Act 4", "Act 5", "Act 6"};
            r.add(c);
        }
        r.button("level.go", "Go");
        const std::string went = r.get("level.note");
        if (!went.empty()) r.text("level_note", went);
    }
    r.heading("Table");
    r.button("table.replace", "Re-place table");
    r.button("table.scan", "Scan the room (Space Setup)");
    r.button("app.quit", "Quit");
    return r.out;
}

// The sm64 toggles of the Look tab: key, label, off / on values, the enable rule's dependency (another sm64 key that
// must be "on"; "" none) and whether Room-lit is required.
struct Sm64Row { const char *key, *label, *off, *on, *needs; bool roomlit; };
const Sm64Row kStory[] = {
    {"pack_tilt", "3D models follow the floor", "off", "on", "", false},
    {"menu_size", nullptr, "normal", "large", "", false},
    {"menu_backdrop", "Menu backdrop", "off", "on", "", false},
    {"menu_backdrop_white", "Menu backdrop white (off: dark glass)", "off", "on", "menu_backdrop", false},
    {"letter_front", "Peach's letter in front of the castle", "off", "on", "", false},
    {"score_float", "Score floats above the level", "off", "on", "", false},
    {"score_seated", "Score at a seated height, larger", "off", "on", "score_float", false},
    {"intro_pip", "Show Lakitu's view during the intro", "off", "on", "", false},
    {"pip_follow", "Lakitu's view rides over Lakitu", "off", "on", "intro_pip", false},
    {"title_eye_level", "Title at eye level", "off", "on", "", false},
    {"title_touch", nullptr, "", "", "", false},
    {"title_eyes", nullptr, "", "", "", false},
    {"room_reveal", "Reveal rooms as you visit", "off", "on", "", false},
    {"gameover_text", "GAME OVER lettering (Story)", "off", "on", "", false},
    {"splash_lift", "Splash logo stays above the table", "off", "on", "", false},
    {"title_light", "Title head lit from the front", "room", "front", "", true},
};
const Sm64Row kDiorama[] = {
    {"below_table", "Hide what reaches below the table", "show", "hide", "", true},
    {"trim_level", "Trim the level at its edges", "off", "on", "", true},
    {"liquids", "Water and lava look like liquids", "off", "on", "", true},
    {"water_trim", "Water ends at the shore", "off", "on", "liquids", true},
    {"behind_walls", nullptr, "", "", "", false},
    {"transitions", "Warps fade and iris on the table", "off", "on", "", true},
    {"trans_fade", "Fades: the level turns see-through", "colour", "clear", "transitions", true},
    {"fine_textures", "Sharper textures (anisotropy 16)", "off", "on", "", true},
    {"dialog_plate", "Dialogs: dark text on a white plate", "off", "on", "", false},
    {"dialog_size", nullptr, "", "", "", false},
    {"act_star_solid", "Act selector: a solid star for the next act", "off", "on", "", false},
};
// The segmented sm64 rows (label nullptr above).
struct Sm64Seg { const char *key, *label; Opts values, labels; };
const std::vector<Sm64Seg> &sm64_segs() {
    static const std::vector<Sm64Seg> s = {
        {"menu_size", "Menu size", {"normal", "large"}, {"Normal (50 cm)", "Large (70 cm)"}},
        {"title_touch", "Pinch the head", {"both", "hands", "pinch"}, {"At the face + look", "At the face", "Look"}},
        {"title_eyes", "Eyes follow", {"both", "finger", "off"}, {"Finger + you", "Finger", "Off"}},
        {"behind_walls", "Mario behind walls", {"silhouette", "cutaway", "both", "off"}, {"Silhouette", "Cutaway", "Both", "Off"}},
        {"dialog_size", "Dialog size", {"normal", "large"}, {"Normal", "Large (1.5x)"}},
    };
    return s;
}
template <size_t N>
void sm64_rows(Rows &r, const Sm64Row (&rows)[N], bool roomlit, bool tex_smooth) {
    for (const Sm64Row &row : rows) {
        const std::string id = std::string("sm64.") + row.key;
        Control *c = nullptr;
        if (!row.label) {
            for (const Sm64Seg &s : sm64_segs())
                if (std::string(s.key) == row.key) c = &r.seg(id, s.label, s.values, s.labels);
        } else {
            c = &r.toggle(id, row.label, row.off, row.on);
        }
        if (!c) continue;
        bool on = !row.roomlit || roomlit;
        if (*row.needs) on = on && r.get(std::string("sm64.") + row.needs) == "on";
        if (std::string(row.key) == "fine_textures") on = on && tex_smooth;
        c->enabled = on;
    }
}

std::vector<Control> look_tab(const Bridge &b) {
    Rows r(b);
    const bool levels = flag(b.has_levels);
    r.seg("look.mode", "Look", {"original", "roomlit"}, {"Original N64", "Room-lit"});
    const bool rl = r.get("look.mode") == "roomlit";
    if (levels) {   // PacksGroup
        struct Kind2 { const char *list, *key, *label, *on_label; };
        const Kind2 kinds[] = {{"packs_tex", "look.packs_tex", "Textures", "HD"}, {"packs_models", "look.packs_models", "Models", "3D pack"}};
        bool any = false;
        for (const Kind2 &k : kinds) {
            const Opts names = names_of(b, k.list);
            if (names.empty()) continue;
            if (!any) r.heading("Packs");
            any = true;
            const std::string on_id = std::string(k.key) + "_on";
            r.seg(on_id, k.label, {"0", "1"}, {"Original", k.on_label}).enabled = rl;
            if (names.size() > 1) {
                Control &p = r.picker(k.key, std::string(k.label) + " pack", names);
                if (std::find(names.begin(), names.end(), p.value) == names.end()) p.value = names[0];
                p.enabled = rl && p.value.size() && r.get(on_id) == "1";
            }
        }
        if (any && !rl) r.text("packs", "Original N64 draws the original textures and models.");
    }
    r.seg("look.source", "Light source", {"room", "fixed"}, {"Room", "Fixed key light"}).enabled = rl;
    r.seg("look.light_response", "Room light", {"steady", "responsive"}, {"Steady", "Responsive"}).enabled =
        rl && r.get("look.source") != "fixed";
    const std::string light = r.get("look.room_light");
    r.text("room_light", light.empty() ? "room light: -" : light);
    r.slider("look.match_brightness", "Match brightness", 0.f, 1.f, 0.05f).enabled = rl;
    r.slider("look.match_colour", "Match colour", 0.f, 1.f, 0.05f).enabled = rl;
    r.button("look.neutral_lum", "Set as neutral").enabled = rl;
    r.button("look.neutral_reset", "Reset neutral").enabled = rl;
    r.toggle("look.contact", "Contact shadow on the table").enabled = rl;
    r.slider("look.contact_strength", "Contact strength", 0.f, 1.f, 0.05f).enabled = rl && r.get("look.contact") == "1";
    r.seg("look.shadows", "Shadows", {"off", "objects", "all"}, {"Off", "Objects", "Objects + level"}).enabled = rl;
    const bool shadows = rl && r.get("look.shadows") != "off";
    r.seg("look.blob", "Blob shadows", {"keep", "hide"}, {"Keep", "Hide"}).enabled = shadows;
    r.slider("look.softness", "Shadow softness", 0.f, 1.f, 0.05f).enabled = shadows;
    r.slider("look.table_cast", "Shadow on table", 0.f, 1.f, 0.05f).enabled = shadows;
    r.heading("Detail");
    r.toggle("look.relight", "Terrain light from its shape").enabled = rl;
    r.slider("look.relight_strength", "Relight strength", 0.f, 1.f, 0.05f).enabled = rl && r.get("look.relight") == "1";
    r.toggle("look.ao", "Ambient occlusion").enabled = rl;
    r.slider("look.ao_strength", "AO strength", 0.f, 1.f, 0.05f).enabled = rl && r.get("look.ao") == "1";
    r.toggle("look.toy", "Toy material (painted plastic)").enabled = rl;
    const bool toy = rl && r.get("look.toy") == "1";
    r.slider("look.gloss", "Gloss", 0.f, 1.f, 0.05f).enabled = toy;
    r.slider("look.rim", "Rim light", 0.f, 1.f, 0.05f).enabled = toy;
    r.toggle("look.reflections", "Room reflections (metal cap)").enabled = toy;
    r.heading("Fixed key light (and fallback)");
    r.slider("look.key_az", "Direction", 0.f, 360.f, 5.f, " deg").enabled = rl;
    r.slider("look.key_el", "Height", 10.f, 90.f, 5.f, " deg").enabled = rl;
    r.slider("look.key_power", "Brightness", 0.3f, 1.5f, 0.05f, "x").enabled = rl;
    r.slider("look.warmth", "Warmth", -1.f, 1.f, 0.05f).enabled = rl;
    r.slider("look.directional", "Directional", 0.f, 1.f, 0.05f).enabled = rl;
    r.heading("Graphics");
    r.seg("look.msaa", "Anti-aliasing (MSAA)", {"1", "2", "4"}, {"Off", "2x", "4x"});
    const std::string msaa = r.get("look.msaa");
    r.toggle("look.edge_aa", "Smooth cut-out edges (with MSAA)").enabled = (msaa.empty() ? "4" : msaa) != "1";
    r.toggle("look.foveation", "Foveation (sharper centre)").note = "no effect on this headset";
    Opts q = {"0", "0.6", "0.8", "0.9", "1"};
    const std::string rq = r.get("look.render_quality");
    if (!rq.empty() && std::find(q.begin(), q.end(), rq) == q.end()) q.push_back(rq);   // RenderQualityChoices.tags
    Opts ql = q;
    ql[0] = "System";
    r.seg("look.render_quality", "Render quality", q, ql).note = "no effect on this headset";
    r.seg("look.tex_filter", "Textures", {"smooth", "vanilla"}, {"Smooth (mipmaps)", "Original sampling"});
    r.toggle("look.decal_fix", "Shadow flicker fix (decal offset)");
    r.seg("look.colour", "Colour", {"auto", "linear", "raw"}, {"Auto", "Linear", "Raw"});
    if (levels && !names_of(b, "sm64_keys").empty()) {
        const std::string tf = r.get("look.tex_filter");
        r.heading("Story and 3D models");
        sm64_rows(r, kStory, rl, tf.empty() || tf == "smooth");
        r.heading("Diorama");
        sm64_rows(r, kDiorama, rl, tf.empty() || tf == "smooth");
        r.text("diorama", "Room-lit only (except dialogs and the act star).");
    }
    return r.out;
}

std::vector<Control> audio_tab(const Bridge &b) {
    Rows r(b);
    if (flag(b.has_levels) && !names_of(b, "sm64_keys").empty()) {   // SM64SoundRows
        const bool stereo = r.get("sm64.sound") == "stereo";
        const std::string st = r.get("sm64.audio_state");   // -1 not decided yet, 0 stereo only, 1 spatial
        r.heading("Sound");
        r.seg("sm64.sound", "Sound", {"spatial", "stereo"}, {"Spatial", "Stereo"});
        r.seg("sm64.audio", "Game sound", {"on", "off"}, {"On", "Off"});
        r.seg("sm64.reverb", "Game reverb", {"neutral", "original"}, {"Neutral", "Original"}).enabled = !(stereo || st == "0");
        r.seg("sm64.event_music", "Event music", {"ring", "bed"}, {"Around the table", "With the game's mix"}).enabled =
            !(stereo || st == "0" || r.get("audio.music") != "ring");
        r.seg("sm64.resampler", "Resampler", {"sinc", "linear"}, {"Smooth", "Linear"}).enabled = st != "0";
    }
    r.heading("Audio");
    Opts presets = names_of(b, "audio_presets"), labels;
    for (const std::string &p : presets) labels.push_back(capitalized(p));
    Control &pr = r.seg("audio.preset", "Preset", presets, labels);
    if (option_index(pr) < 0) pr.note = "Custom";
    struct S { const char *key, *label; float lo, hi, step; const char *unit; };
    const S focus[] = {
        {"head_focus", "Head focus", 0.f, 1.f, 0.05f, ""},          {"player_focus", "Mario focus", 0.f, 1.f, 0.05f, ""},
        {"object_focus", "Object focus", 0.f, 1.f, 0.05f, ""},      {"spotlight", "Spotlight", -30.f, 0.f, 1.f, " dB"},
        {"spotlight_cone", "Spotlight cone", 5.f, 90.f, 1.f, " deg"}, {"widen", "Wider left/right", 1.f, 2.f, 0.05f, "x"},
        {"sfx_db", "Effects", -12.f, 6.f, 0.5f, " dB"},             {"music_db", "Music", -24.f, 6.f, 0.5f, " dB"},
    };
    for (const S &s : focus) r.slider(std::string("audio.") + s.key, s.label, s.lo, s.hi, s.step, s.unit);
    r.seg("audio.music", "Music", {"box", "ring"}, {"Music box", "Orchestra ring"});
    const bool ring = r.get("audio.music") == "ring";
    r.slider("audio.ring_radius", "Ring size", 0.5f, 2.f, 0.05f, "x").enabled = ring;
    r.slider("audio.ring_rotate", "Ring rotation", 0.f, 30.f, 1.f, " deg/s").enabled = ring;
    r.seg("audio.ring_height", "Ring height", {"head", "table"}, {"Head", "Table"}).enabled = ring;
    r.slider("audio.ring_focus", "Ring focus", 0.f, 1.f, 0.05f).enabled = ring;
    r.seg("audio.spatializer", "Spatial audio", {"binaural", "automatic", "channel"}, {"Binaural", "Automatic", "Channel"}).note =
        "binaural: panning on this headset";
    r.toggle("audio.show_sources", "Show sound sources");
    return r.out;
}

std::vector<Control> world_tab(const Bridge &b) {
    Rows r(b);
    const bool locked = flag(b.locked);
    Control &m = r.slider("world.mario_cm", "Mario size", 0.5f, 10.f, 0.1f, " cm");
    m.log_scale = true;
    m.enabled = !locked;
    m.note = r.get("world.mario_cm_readout");
    r.toggle("world.draw_all_rooms", "Draw all rooms").enabled = !locked;
    const bool all = r.get("world.draw_all_rooms") == "1";
    r.toggle("world.hide_outside", "Hide outside while inside").enabled = all;
    r.seg("world.other_rooms", "Objects in other rooms", {"0", "1", "2"}, {"Hidden", "Frozen", "Live (experimental)"}).enabled =
        all && !locked;
    r.seg("world.billboards", "Billboards", {"1", "0"}, {"Upright", "Face the head"});
    r.toggle("world.cannon_visible", "Cannon visible while aiming").enabled = !locked;
    const bool trees3d = r.get("world.trees_3d") == "1";
    Control &t = r.seg("world.tree_style", "Tree style", {"0", "1"}, {"Original card", "Crossed cards"});
    t.enabled = !trees3d;
    if (trees3d) t.note = "3D trees from the model pack";
    return r.out;
}

float parse(const std::string &s, float fallback) {
    if (s.empty()) return fallback;
    char *end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(v) ? v : fallback;
}
float clamp01(float t) { return t < 0.f ? 0.f : t > 1.f ? 1.f : t; }
}  // namespace

std::vector<Tab> build_tabs(const Bridge &b) {
    std::vector<Tab> tabs;
    tabs.push_back(Tab{"Level", level_tab(b)});
    tabs.push_back(Tab{"Look", look_tab(b)});
    tabs.push_back(Tab{"Audio", audio_tab(b)});
    if (b.has_levels && b.has_levels()) tabs.push_back(Tab{"World", world_tab(b)});
    return tabs;
}

float content_top(const Layout &) { return kTabStripH + 8.f; }
float content_bottom(const Layout &L) { return (float)L.height - kFooterH; }
int visible_rows(const Layout &L) {
    const float h = content_bottom(L) - content_top(L);
    return L.row_h > 0.f && h > 0.f ? (int)std::floor(h / L.row_h) : 0;
}
void widget_span(const Layout &L, float *x0, float *x1) {
    *x0 = L.margin + kLabelW;
    *x1 = (float)L.width - L.margin - kScrollW - 12.f;
}
void slider_span(const Layout &L, float *x0, float *x1) {
    widget_span(L, x0, x1);
    *x1 -= kValueW;
}
void scroll_span(const Layout &L, float *x0, float *x1) {
    *x1 = (float)L.width - L.margin;
    *x0 = *x1 - kScrollW;
}
int max_scroll(const std::vector<Tab> &tabs, int tab, const Layout &L) {
    if (tab < 0 || tab >= (int)tabs.size()) return 0;
    return std::max(0, (int)tabs[(size_t)tab].controls.size() - visible_rows(L));
}

Hit hit_test(const std::vector<Tab> &tabs, int tab, const Layout &L, float u, float v, int scroll_rows) {
    Hit h;
    if (!(u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f) || tabs.empty()) return h;
    const float x = u * (float)L.width, y = v * (float)L.height;
    if (y < kTabStripH) {
        h.tab_strip = true;
        h.tab = std::min((int)tabs.size() - 1, (int)(x / ((float)L.width / (float)tabs.size())));
        return h;
    }
    if (tab < 0 || tab >= (int)tabs.size()) return h;
    h.tab = tab;
    const float top = content_top(L), bottom = top + (float)visible_rows(L) * L.row_h;
    if (y < top || y >= bottom) return h;
    float sx0, sx1;
    scroll_span(L, &sx0, &sx1);
    if (x >= sx0 && x <= sx1) {
        if (max_scroll(tabs, tab, L) > 0) h.scroll = y < 0.5f * (top + bottom) ? -1 : 1;
        return h;
    }
    const int idx = scroll_rows + (int)((y - top) / L.row_h);
    const std::vector<Control> &cs = tabs[(size_t)tab].controls;
    if (idx < 0 || idx >= (int)cs.size()) return h;
    h.control = idx;
    float x0, x1;
    if (cs[(size_t)idx].kind == Kind::slider) slider_span(L, &x0, &x1);
    else widget_span(L, &x0, &x1);
    h.t = clamp01((x - x0) / (x1 - x0));
    return h;
}

bool same_hit(const Hit &a, const Hit &b) {
    return a.tab == b.tab && a.control == b.control && a.tab_strip == b.tab_strip && a.scroll == b.scroll;
}

std::string format_value(float v) {
    char b[32];
    double r = std::round((double)v * 1000.0) / 1000.0;
    if (r == 0.0) r = 0.0;   // no "-0"
    std::snprintf(b, sizeof b, "%g", r);
    return b;
}

float snap_value(const Control &c, float v) {
    v = std::min(std::max(v, c.min), c.max);
    if (c.step > 0.f) v = c.min + std::round((v - c.min) / c.step) * c.step;
    return std::min(std::max(v, c.min), c.max);
}

std::string option_label(const Control &c, size_t i) {
    if (i < c.labels.size()) return c.labels[i];
    return i < c.options.size() ? c.options[i] : std::string();
}

int option_index(const Control &c) {
    for (size_t i = 0; i < c.options.size(); i++) if (c.options[i] == c.value) return (int)i;
    return -1;
}

Action act_on(const std::vector<Tab> &tabs, int tab, const Hit &h, bool trigger_edge, float stick_x, float dt_s) {
    Action a;
    if (h.tab_strip) {
        if (!trigger_edge || h.tab < 0 || h.tab >= (int)tabs.size()) return a;
        a.kind = Action::select_tab; a.tab = h.tab; a.value = tabs[(size_t)h.tab].name;
        return a;
    }
    if (h.scroll != 0) {
        if (!trigger_edge) return a;
        a.kind = Action::scroll; a.tab = tab; a.delta = h.scroll * 8;
        return a;
    }
    if (tab < 0 || tab >= (int)tabs.size() || h.control < 0 || h.control >= (int)tabs[(size_t)tab].controls.size()) return a;
    const Control &c = tabs[(size_t)tab].controls[(size_t)h.control];
    if (!c.enabled) return a;
    a.id = c.id; a.tab = tab;
    const int n = (int)c.options.size();
    switch (c.kind) {
    case Kind::text: return Action{};
    case Kind::button:
        if (trigger_edge) a.kind = Action::press;
        return a;
    case Kind::toggle:
        if (!trigger_edge || n != 2) return Action{};
        a.kind = Action::set; a.value = c.value == c.options[1] ? c.options[0] : c.options[1];
        return a;
    case Kind::segmented:
        if (!trigger_edge || n == 0) return Action{};
        a.kind = Action::set; a.value = c.options[(size_t)std::min(n - 1, (int)(clamp01(h.t) * (float)n))];
        return a;
    case Kind::picker: {
        if (!trigger_edge || n == 0) return Action{};
        const int i = option_index(c), d = h.t < 0.25f ? -1 : 1;
        a.kind = Action::set; a.value = c.options[(size_t)(((std::max(i, 0) + d) % n + n) % n)];
        return a;
    }
    case Kind::stepper: {
        if (!trigger_edge) return Action{};
        const float v = snap_value(c, parse(c.value, c.min) + (h.t < 0.25f ? -c.step : c.step));
        a.kind = Action::set; a.value = format_value(v);
        return a;
    }
    case Kind::slider: {
        const float cur = parse(c.value, c.min);
        const bool lg = c.log_scale && c.min > 0.f;
        auto from_t = [&](float t) { return lg ? std::exp(std::log(c.min) + t * (std::log(c.max) - std::log(c.min))) : c.min + t * (c.max - c.min); };
        if (trigger_edge) {
            const float v = snap_value(c, from_t(clamp01(h.t)));
            a.kind = Action::set; a.value = format_value(v); a.raw = v;
            return a;
        }
        if (stick_x == 0.f || !(dt_s > 0.f)) return Action{};
        float raw;
        if (lg) raw = std::exp(std::log(std::max(cur, c.min)) + stick_x * (std::log(c.max) - std::log(c.min)) * 0.5f * dt_s);
        else raw = cur + stick_x * (c.max - c.min) * 0.5f * dt_s;
        raw = std::min(std::max(raw, c.min), c.max);
        // Every frame of a push reports its value (often the same one): the caller keeps raw to continue from, so a slow
        // push accumulates across frames, and skips a value it already sent.
        a.kind = Action::set; a.value = format_value(snap_value(c, raw)); a.final = false; a.raw = raw;
        return a;
    }
    }
    return Action{};
}

}  // namespace panel
}  // namespace ar
