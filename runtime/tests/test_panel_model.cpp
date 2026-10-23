// The Quest settings panel's model (apps/quest/panel/panel_model.h) over a fake Bridge.
#include "check.h"
#include "panel_model.h"

#include <map>
#include <string>
#include <vector>

using namespace ar::panel;

namespace {
const std::vector<std::string> kLookKeys = {
    "mode", "key_az", "key_el", "key_power", "warmth", "source", "match_brightness", "match_colour", "neutral_lum",
    "light_response", "contact", "contact_strength", "shadows", "blob", "softness", "directional", "table_cast", "relight",
    "relight_strength", "ao", "ao_strength", "toy", "gloss", "rim", "reflections", "msaa", "foveation", "render_quality",
    "tex_filter", "decal_fix", "edge_aa", "colour"};
const std::vector<std::string> kAudioKeys = {"head_focus", "player_focus", "object_focus", "music", "ring_radius", "ring_rotate",
                                             "ring_height", "ring_focus", "spotlight", "spotlight_cone", "widen", "sfx_db",
                                             "music_db", "spatializer", "show_sources"};
const std::vector<std::string> kSm64Keys = {
    "menu_size", "pack_tilt", "menu_backdrop", "gameover_text", "splash_lift", "title_light", "menu_backdrop_white",
    "letter_front", "score_float", "intro_pip", "title_eye_level", "title_touch", "title_eyes", "pip_follow", "room_reveal",
    "below_table", "trim_level", "liquids", "water_trim", "behind_walls", "transitions", "trans_fade", "fine_textures",
    "score_seated", "dialog_plate", "dialog_size", "act_star_solid", "sound", "audio", "reverb", "event_music", "resampler"};

struct Fake {
    std::map<std::string, std::string> v;
    bool levels = true;
    Bridge bridge() {
        Bridge b;
        b.get = [this](const std::string &id) { auto it = v.find(id); return it == v.end() ? std::string() : it->second; };
        b.set = [this](const std::string &id, const std::string &x, bool) { v[id] = x; return true; };
        b.has_levels = [this] { return levels; };
        b.locked = [] { return false; };
        b.names = [this](const std::string &list) -> std::vector<std::string> {
            if (list == "levels") return {"Castle grounds", "Bob-omb Battlefield", "Bowser in the Dark World"};
            if (list == "level_sections") return {"0", "0", "1"};
            if (list == "packs_tex") return {"SM64 Reloaded", "Other HD"};
            if (list == "packs_models") return {"Render96 Models"};
            if (list == "audio_presets") return {"off", "gentle", "strong"};
            if (list == "look_keys") return kLookKeys;
            if (list == "audio_keys") return kAudioKeys;
            if (list == "sm64_keys") return levels ? kSm64Keys : std::vector<std::string>{};
            return {};
        };
        b.status = [] { return std::string("sm64: tick 1"); };
        b.hint = [] { return std::string(); };
        b.now_line = [] { return std::string("Now: Castle grounds"); };
        b.press = [](const std::string &) { return true; };
        return b;
    }
    Fake() {
        v = {{"look.mode", "roomlit"}, {"look.msaa", "4"}, {"look.shadows", "all"}, {"look.source", "room"},
             {"look.contact", "1"}, {"look.relight", "1"}, {"look.ao", "1"}, {"look.toy", "1"}, {"look.tex_filter", "smooth"},
             {"look.edge_aa", "1"}, {"look.decal_fix", "1"}, {"audio.music", "ring"}, {"audio.widen", "1"},
             {"world.draw_all_rooms", "1"}, {"world.mario_cm", "2.5"}, {"sm64.menu_backdrop", "on"}, {"sm64.liquids", "on"},
             {"sm64.transitions", "on"}, {"sm64.score_float", "on"}, {"sm64.intro_pip", "on"}, {"sm64.audio_state", "1"},
             {"sm64.sound", "spatial"}, {"look.packs_tex_on", "1"}, {"look.packs_models_on", "1"}};
    }
};

const Control *find(const std::vector<Tab> &tabs, const std::string &id, int *tab_out = nullptr, int *idx_out = nullptr) {
    for (size_t t = 0; t < tabs.size(); t++)
        for (size_t i = 0; i < tabs[t].controls.size(); i++)
            if (tabs[t].controls[i].id == id) {
                if (tab_out) *tab_out = (int)t;
                if (idx_out) *idx_out = (int)i;
                return &tabs[t].controls[i];
            }
    return nullptr;
}
int tab_index(const std::vector<Tab> &tabs, const std::string &name) {
    for (size_t t = 0; t < tabs.size(); t++) if (tabs[t].name == name) return (int)t;
    return -1;
}
}  // namespace

static void tabs_in_order() {
    Fake f;
    std::vector<Tab> tabs = build_tabs(f.bridge());
    CHECK(tabs.size() == 4);
    CHECK(tabs[0].name == "Level" && tabs[1].name == "Look" && tabs[2].name == "Audio" && tabs[3].name == "World");
    f.levels = false;
    tabs = build_tabs(f.bridge());
    CHECK(tabs.size() == 3);
    CHECK(tabs[0].name == "Level" && tabs[1].name == "Look" && tabs[2].name == "Audio");
    for (const Tab &t : tabs)
        for (const Control &c : t.controls) {
            CHECK(c.id.compare(0, 5, "sm64.") != 0);
            CHECK(c.id.compare(0, 6, "world.") != 0);
        }
}

static void every_key_is_a_control() {
    Fake f;
    const std::vector<Tab> tabs = build_tabs(f.bridge());
    const int look = tab_index(tabs, "Look"), audio = tab_index(tabs, "Audio");
    CHECK(kLookKeys.size() == 32);
    for (const std::string &k : kLookKeys) {
        int t = -1;
        CHECK(find(tabs, "look." + k, &t));
        CHECK(t == look);
    }
    CHECK(find(tabs, "look.packs_tex_on"));
    CHECK(find(tabs, "look.packs_tex"));
    CHECK(find(tabs, "look.packs_models_on"));
    CHECK(kSm64Keys.size() == 32);
    for (const std::string &k : kSm64Keys) {
        int t = -1;
        CHECK(find(tabs, "sm64." + k, &t));
        const bool sound = k == "sound" || k == "audio" || k == "reverb" || k == "event_music" || k == "resampler";
        CHECK(t == (sound ? audio : look));
    }
    for (const std::string &k : kAudioKeys) {
        int t = -1;
        CHECK(find(tabs, "audio." + k, &t));
        CHECK(t == audio);
    }
    for (const char *id : {"level.start_in", "level.play_from_start", "level.level", "level.go", "table.replace", "table.scan",
                           "app.quit", "audio.preset", "world.mario_cm", "world.draw_all_rooms", "world.hide_outside",
                           "world.other_rooms", "world.billboards", "world.cannon_visible", "world.tree_style"})
        CHECK(find(tabs, id));
    // ids are unique
    for (const Tab &t : tabs)
        for (const Control &c : t.controls) {
            int n = 0;
            for (const Tab &u : tabs) for (const Control &d : u.controls) n += d.id == c.id;
            CHECK(n == 1);
        }
}

static void enable_rules() {
    Fake f;
    CHECK(find(build_tabs(f.bridge()), "look.edge_aa")->enabled);
    f.v["look.msaa"] = "1";
    CHECK(!find(build_tabs(f.bridge()), "look.edge_aa")->enabled);
    CHECK(find(build_tabs(f.bridge()), "look.source")->enabled);
    f.v["look.mode"] = "original";
    std::vector<Tab> tabs = build_tabs(f.bridge());
    CHECK(!find(tabs, "look.source")->enabled);
    CHECK(!find(tabs, "look.packs_tex_on")->enabled);
    CHECK(!find(tabs, "look.key_az")->enabled);
    CHECK(find(tabs, "look.msaa")->enabled);
    f.v["look.mode"] = "roomlit";
    f.v["look.shadows"] = "off";
    tabs = build_tabs(f.bridge());
    CHECK(find(tabs, "look.shadows")->enabled);
    CHECK(!find(tabs, "look.blob")->enabled && !find(tabs, "look.softness")->enabled && !find(tabs, "look.table_cast")->enabled);
    CHECK(find(build_tabs(f.bridge()), "audio.ring_radius")->enabled);
    f.v["audio.music"] = "box";
    tabs = build_tabs(f.bridge());
    CHECK(!find(tabs, "audio.ring_radius")->enabled && !find(tabs, "audio.ring_rotate")->enabled);
    CHECK(!find(tabs, "audio.ring_height")->enabled && !find(tabs, "audio.ring_focus")->enabled);
    CHECK(find(tabs, "world.hide_outside")->enabled);
    f.v["world.draw_all_rooms"] = "0";
    CHECK(!find(build_tabs(f.bridge()), "world.hide_outside")->enabled);
    CHECK(find(build_tabs(f.bridge()), "world.tree_style")->enabled);
    f.v["world.trees_3d"] = "1";
    CHECK(!find(build_tabs(f.bridge()), "world.tree_style")->enabled);
    tabs = build_tabs(f.bridge());
    CHECK(find(tabs, "look.foveation")->note == "no effect on this headset");
    CHECK(find(tabs, "audio.spatializer")->note == "binaural: panning on this headset");
}

static void ranges() {
    Fake f;
    const std::vector<Tab> tabs = build_tabs(f.bridge());
    const Control *s = find(tabs, "audio.spotlight");
    CHECK(s->kind == Kind::slider && s->min == -30.f && s->max == 0.f && s->step == 1.f);
    const Control *az = find(tabs, "look.key_az");
    CHECK(az->kind == Kind::slider && az->min == 0.f && az->max == 360.f && az->step == 5.f);
    const Control *m = find(tabs, "world.mario_cm");
    CHECK(m->kind == Kind::slider && m->min == 0.5f && m->max == 10.f && m->log_scale);
    const Control *w = find(tabs, "audio.widen");
    CHECK(w->min == 1.f && w->max == 2.f && w->step == 0.05f);
    CHECK(find(tabs, "look.msaa")->kind == Kind::segmented);
    CHECK(find(tabs, "look.contact")->kind == Kind::toggle);
    CHECK(find(tabs, "level.level")->kind == Kind::picker);
    CHECK(find(tabs, "level.level")->options.size() == 3);
}

static void hit_and_act() {
    Fake f;
    std::vector<Tab> tabs = build_tabs(f.bridge());
    const Layout L;
    const int look = tab_index(tabs, "Look");
    // the first row's centre
    Hit h = hit_test(tabs, look, L, 0.5f, (content_top(L) + 0.5f * L.row_h) / (float)L.height, 0);
    CHECK(h.control == 0 && h.tab == look && !h.tab_strip);
    // the second row with two rows scrolled is control 3
    h = hit_test(tabs, look, L, 0.5f, (content_top(L) + 1.5f * L.row_h) / (float)L.height, 2);
    CHECK(h.control == 3);
    // the tab strip
    h = hit_test(tabs, look, L, 0.9f, 10.f / (float)L.height, 0);
    CHECK(h.tab_strip && h.tab == 3);
    Action a = act_on(tabs, look, h, true, 0.f, 0.f);
    CHECK(a.kind == Action::select_tab && a.tab == 3 && a.value == "World");

    // toggle: flipped
    int t = -1, i = -1;
    find(tabs, "look.contact", &t, &i);
    Hit th; th.tab = t; th.control = i; th.t = 0.5f;
    a = act_on(tabs, t, th, true, 0.f, 0.f);
    CHECK(a.kind == Action::set && a.id == "look.contact" && a.value == "0");
    CHECK(act_on(tabs, t, th, false, 0.f, 0.f).kind == Action::none);
    // segmented: the next option (the pill under t)
    find(tabs, "look.shadows", &t, &i);   // off, objects, all; current all
    const Control &sh = tabs[t].controls[i];
    CHECK(sh.options.size() == 3 && sh.value == "all");
    Hit sg; sg.tab = t; sg.control = i; sg.t = 0.1f;
    a = act_on(tabs, t, sg, true, 0.f, 0.f);
    CHECK(a.kind == Action::set && a.value == "off");
    f.v["look.shadows"] = "off";
    tabs = build_tabs(f.bridge());
    sg.t = 0.5f;
    a = act_on(tabs, t, sg, true, 0.f, 0.f);
    CHECK(a.kind == Action::set && a.value == "objects");
    // slider: the stick moves it by half its range per second, rounded to the step
    find(tabs, "audio.widen", &t, &i);
    Hit sl; sl.tab = t; sl.control = i; sl.t = 0.f;
    a = act_on(tabs, t, sl, false, 1.f, 1.f);
    CHECK(a.kind == Action::set && a.id == "audio.widen" && a.value == "1.5" && !a.final);
    CHECK(a.raw == 1.5f);
    // a slow push accumulates through raw: one frame stays at 1, the value passed back moves on from there
    a = act_on(tabs, t, sl, false, 0.3f, 1.f / 90.f);
    CHECK(a.kind == Action::set && a.value == "1" && a.raw > 1.f && a.raw < 1.01f);
    tabs[t].controls[i].value = "1.024";
    a = act_on(tabs, t, sl, false, 0.3f, 1.f / 90.f);
    CHECK(a.kind == Action::set && a.value == "1.05");
    tabs[t].controls[i].value = "1";
    // the trigger sets the value under t, final
    sl.t = 1.f;
    a = act_on(tabs, t, sl, true, 0.f, 0.f);
    CHECK(a.kind == Action::set && a.value == "2" && a.final);
    // a disabled control does nothing
    f.v["audio.music"] = "box";
    tabs = build_tabs(f.bridge());
    find(tabs, "audio.ring_radius", &t, &i);
    Hit dh; dh.tab = t; dh.control = i; dh.t = 0.5f;
    CHECK(act_on(tabs, t, dh, true, 1.f, 1.f).kind == Action::none);
    // a button presses
    find(tabs, "level.go", &t, &i);
    Hit bh; bh.tab = t; bh.control = i;
    a = act_on(tabs, t, bh, true, 0.f, 0.f);
    CHECK(a.kind == Action::press && a.id == "level.go");
    // picker: forward from the right part, back from the left quarter (wrapping)
    find(tabs, "level.level", &t, &i);
    Hit ph; ph.tab = t; ph.control = i; ph.t = 0.9f;
    a = act_on(tabs, t, ph, true, 0.f, 0.f);
    CHECK(a.kind == Action::set && a.id == "level.level" && a.value == "1");
    ph.t = 0.1f;
    a = act_on(tabs, t, ph, true, 0.f, 0.f);
    CHECK(a.kind == Action::set && a.value == "2");
}

static void formatting() {
    CHECK(format_value(1.5f) == "1.5");
    CHECK(format_value(0.30000001f) == "0.3");
    CHECK(format_value(-9.f) == "-9");
    Control c{Kind::slider, "x", "x", {}, 0.3f, 1.5f, 0.05f};
    CHECK(format_value(snap_value(c, 0.62f)) == "0.6");
    CHECK(format_value(snap_value(c, 9.f)) == "1.5");
}

int main() {
    tabs_in_order();
    every_key_is_a_control();
    enable_rules();
    ranges();
    hit_and_act();
    formatting();
    printf("test_panel_model ok\n");
    return 0;
}
