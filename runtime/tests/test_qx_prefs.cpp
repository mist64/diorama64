// QxPrefs (flat JSON string map, atomic save) and the visionOS load rules ported to it.
#include "check.h"
#include "qx_prefs.h"

#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace ar;

static std::string tmp_path(const char *name) {
    return std::string("/tmp/test_qx_prefs_") + std::to_string((int)getpid()) + "_" + name + ".json";
}
static void write_file(const std::string &p, const std::string &t) {
    FILE *f = fopen(p.c_str(), "wb");
    CHECK(f);
    fwrite(t.data(), 1, t.size(), f);
    fclose(f);
}
static std::string read_file(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    CHECK(f);
    std::string s;
    char b[256];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static bool exists(const std::string &p) { return access(p.c_str(), F_OK) == 0; }

static void round_trip_value(const char *name, const std::string &v) {
    std::string path = tmp_path(name), err;
    QxPrefs a(path);
    a.set("k", v);
    CHECK(a.save(&err));
    QxPrefs b(path);
    CHECK(b.load(&err));
    CHECK(b.has("k"));
    CHECK(b.get("k") == v);
    unlink(path.c_str());
}

static void test_round_trip() {
    std::string path = tmp_path("rt"), err;
    QxPrefs a(path);
    a.set("look.shadows", "objects");
    a.set_int("look.msaa", 4);
    CHECK(a.save(&err));
    CHECK(!exists(path + ".tmp"));
    QxPrefs b(path);
    CHECK(b.load(&err));
    CHECK(b.get("look.shadows") == "objects");
    CHECK(b.get_int("look.msaa", 0) == 4);
    CHECK(b.get("nope", "fb") == "fb");
    CHECK(b.get_int("nope", 7) == 7);
    b.set("bad", "xyz");
    CHECK(b.get_int("bad", 9) == 9);
    b.remove("look.msaa");
    CHECK(!b.has("look.msaa"));
    unlink(path.c_str());
}

static void test_missing_and_corrupt() {
    std::string path = tmp_path("miss"), err;
    unlink(path.c_str());
    QxPrefs a(path);
    CHECK(a.load(&err));
    CHECK(!a.has("anything"));

    const char *bad[] = {"{not json", "[1,2]", "{\"a\":1}", "{\"a\":\"b\"} trailing", "{\"a\":\"\\q\"}", "{\"a\":\"x\"", "{\"a\":\"b\",}", "{\"a\":{\"b\":\"c\"}}"};
    for (const char *t : bad) {
        write_file(path, t);
        QxPrefs c(path);
        c.set("stale", "1");
        err.clear();
        CHECK(!c.load(&err));
        CHECK(!err.empty());
        CHECK(!c.has("stale"));
    }
    write_file(path, "");
    QxPrefs z(path);
    CHECK(z.load(&err));
    write_file(path, "  { }  ");
    QxPrefs e(path);
    CHECK(e.load(&err));
    unlink(path.c_str());
}

static void test_save_failure_keeps_original() {
    std::string path = tmp_path("keep"), err;
    QxPrefs a(path);
    a.set("k", "v1");
    CHECK(a.save(&err));
    std::string before = read_file(path);
    // Make the temp path unwritable: a directory in its place.
    CHECK(mkdir((path + ".tmp").c_str(), 0700) == 0);
    a.set("k", "v2");
    err.clear();
    CHECK(!a.save(&err));
    CHECK(!err.empty());
    CHECK(read_file(path) == before);
    rmdir((path + ".tmp").c_str());
    unlink(path.c_str());
}

static void test_escapes() {
    round_trip_value("quote", "a\"b");
    round_trip_value("backslash", "a\\b");
    round_trip_value("newline", "a\nb");
    round_trip_value("tab", "a\tb");
    round_trip_value("ctl", std::string("a\x01" "b\x1f", 4));
    round_trip_value("utf8", "caf\xc3\xa9");
    // \uXXXX in input.
    std::string path = tmp_path("u"), err;
    write_file(path, "{\"k\":\"A\\u0042\\u0009\\/\"}");
    QxPrefs a(path);
    CHECK(a.load(&err));
    CHECK(a.get("k") == "AB\t/");
    write_file(path, "{\"k\":\"caf\\u00e9\"}");
    QxPrefs b(path);
    CHECK(b.load(&err));
    CHECK(b.get("k") == "caf\xc3\xa9");
    unlink(path.c_str());
}

static std::vector<std::string> keys() { return {"shadows", "msaa"}; }
static bool def_of(const std::string &k, std::string *v) {
    *v = k == "shadows" ? "full" : "2";
    return true;
}

static void test_drop_stored() {
    std::string path = tmp_path("drop");
    QxPrefs p(path);
    p.set("look.shadows", "objects");
    p.set_int("look.defaults_rev", 0);
    auto drop = [](const std::string &k, const std::string &s, int rev, bool) { return k == "shadows" && s == "objects" && rev == 0; };
    auto launch = [](const std::string &, std::string *) { return false; };
    auto r = qx_load_values(p, "look.", keys(), def_of, 2, drop, launch);
    CHECK(r.size() == 2);
    CHECK(r[0].key == "shadows" && r[0].value == "full" && !r[0].from_launch);
    CHECK(!p.has("look.shadows"));
    CHECK(p.get_int("look.defaults_rev", -1) == 2);
    // A second run at the same revision keeps stored values and does not drop.
    p.set("look.shadows", "objects");
    r = qx_load_values(p, "look.", keys(), def_of, 2, drop, launch);
    CHECK(r[0].value == "objects");
    CHECK(p.has("look.shadows"));
    // defaults_version also counts as the stored revision.
    QxPrefs q(path);
    q.set("look.shadows", "objects");
    q.set_int("look.defaults_version", 2);
    r = qx_load_values(q, "look.", keys(), def_of, 2, drop, launch);
    CHECK(r[0].value == "objects");
    CHECK(!q.has("look.defaults_rev"));
}

// arvos_quest_seed_look's sequence on a fresh store: the seed, then load_look's qx_load_values with the Look rule
// (look_drop_stored: shadows "objects" was superseded at revision 1; look_defaults_rev() is 2).
static void test_seed_survives_defaults_rule() {
    auto drop = [](const std::string &k, const std::string &s, int rev, bool has_launch) {
        return !has_launch && 1 > rev && k == "shadows" && s == "objects";
    };
    auto launch = [](const std::string &, std::string *) { return false; };
    QxPrefs p(tmp_path("seed"));
    CHECK(qx_seed_value(p, "look.", "shadows", "objects", 2));
    CHECK(p.get_int("look.defaults_rev", -1) == 2);
    auto r = qx_load_values(p, "look.", keys(), def_of, 2, drop, launch);
    CHECK(r[0].key == "shadows" && r[0].value == "objects");
    CHECK(p.get("look.shadows") == "objects");
    CHECK(!qx_seed_value(p, "look.", "shadows", "full", 2));   // stored: the player's value stays
    CHECK(p.get("look.shadows") == "objects");
    // Without the revision (the old seed) the same value is dropped: the bug this guards against.
    QxPrefs q(tmp_path("seed_old"));
    q.set("look.shadows", "objects");
    r = qx_load_values(q, "look.", keys(), def_of, 2, drop, launch);
    CHECK(r[0].value == "full" && !q.has("look.shadows"));
    // A store already loaded at an older revision keeps it, so the rule still applies to the player's older values.
    QxPrefs o(tmp_path("seed_rev0"));
    o.set_int("look.defaults_version", 0);
    o.set("look.msaa", "1");
    CHECK(qx_seed_value(o, "look.", "shadows", "objects", 2));
    CHECK(!o.has("look.defaults_rev"));
}

static void test_launch_value() {
    std::string path = tmp_path("launch");
    QxPrefs p(path);
    p.set("look.msaa", "1");
    auto drop = [](const std::string &, const std::string &, int, bool has_launch) { return has_launch; };
    auto launch = [](const std::string &k, std::string *v) {
        if (k != "msaa") return false;
        *v = "4";
        return true;
    };
    auto r = qx_load_values(p, "look.", keys(), def_of, 1, drop, launch);
    CHECK(r[1].key == "msaa" && r[1].value == "4" && r[1].from_launch);
    CHECK(p.get("look.msaa") != "4");  // not written by the function: the caller stores it after a successful apply
    CHECK(!p.has("look.msaa"));        // (stored "1" was dropped by drop_stored)
    CHECK(!r[0].from_launch && r[0].value == "full");
    CHECK(!p.has("look.shadows"));
    CHECK(p.get_int("look.defaults_rev", -1) == 1);
}

static void test_audio_migration() {
    std::string path = tmp_path("audio");
    QxPrefs p(path);
    p.set_int("audio.defaults_version", 1);
    p.set("audio.music", "box");
    p.set("audio.ring_focus", "1");
    qx_migrate_audio_defaults(p);
    CHECK(!p.has("audio.music"));
    CHECK(p.has("audio.ring_focus"));  // version 1 change was already applied
    CHECK(p.get_int("audio.defaults_version", 0) == 2);

    QxPrefs q(path);
    q.set_int("audio.defaults_version", 1);
    q.set("audio.music", "ring");
    qx_migrate_audio_defaults(q);
    CHECK(q.get("audio.music") == "ring");
    CHECK(q.get_int("audio.defaults_version", 0) == 2);

    QxPrefs r(path);  // fresh install: version 0 applies both
    r.set("audio.ring_focus", "1");
    r.set("audio.music", "box");
    r.set("audio.volume", "5");
    qx_migrate_audio_defaults(r);
    CHECK(!r.has("audio.ring_focus") && !r.has("audio.music") && r.get("audio.volume") == "5");
    r.set("audio.music", "box");  // already at 2: no second drop
    qx_migrate_audio_defaults(r);
    CHECK(r.get("audio.music") == "box");
}

int main() {
    test_round_trip();
    test_missing_and_corrupt();
    test_save_failure_keeps_original();
    test_escapes();
    test_drop_stored();
    test_seed_survives_defaults_rule();
    test_launch_value();
    test_audio_migration();
    printf("PASS\n");
    return 0;
}
