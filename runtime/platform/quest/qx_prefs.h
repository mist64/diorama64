// The Quest settings store: a flat JSON object of string values in one file, plus the visionOS load rules
// (LookLoad.swift loadLookValues, SM64ARApp.swift migrateSavedDefaults) ported over it. Pure C++ (host-tested).
#pragma once
#include <functional>
#include <mutex>
#include <map>
#include <string>
#include <vector>

namespace ar {

// Keys and values are UTF-8 strings. Any thread. save() is atomic: write path + ".tmp", fsync, rename over path.
class QxPrefs {
public:
    explicit QxPrefs(const std::string &path);
    // A missing file succeeds with no keys. A corrupt file fails with a message and leaves the store empty
    // (the caller decides whether to start fresh).
    bool load(std::string *err);
    bool save(std::string *err);
    bool has(const std::string &key) const;
    std::string get(const std::string &key, const std::string &fallback = "") const;
    void set(const std::string &key, const std::string &value);
    void remove(const std::string &key);
    // strtol; a missing or unparsable value gives the fallback. set_int formats with %d.
    int get_int(const std::string &key, int fallback) const;
    void set_int(const std::string &key, int v);

private:
    std::string path_;
    mutable std::mutex mu_;
    std::map<std::string, std::string> map_;
};

struct PrefLoadResult {
    std::string key, value;
    bool from_launch;
};

// Port of LookLoad.swift:15-40, minus the apply step. For each key: the stored revision is the max of
// prefix+"defaults_rev" and prefix+"defaults_version" (read once, before the loop); if it is older than new_rev and
// drop_stored(key, stored, stored_rev, has_launch) says so, the stored value is removed; the value is launch, else
// stored, else default. A launch value is written to prefix+key. Afterwards, if the revision was older,
// prefix+"defaults_rev" = new_rev.
// Split of duties: the caller applies each result (whether a value is accepted is its business). When the caller rejects
// a value it falls back to the default itself and, if !from_launch, calls p.remove(prefix + key). qx_load_values only
// changes the in-memory store; the caller save()s. A key whose default_of returns false has an empty default.
std::vector<PrefLoadResult> qx_load_values(QxPrefs &p, const std::string &prefix, const std::vector<std::string> &keys,
    const std::function<bool(const std::string &, std::string *)> &default_of, int new_rev,
    const std::function<bool(const std::string &, const std::string &, int, bool)> &drop_stored,
    const std::function<bool(const std::string &, std::string *)> &launch_of);

// Port of SM64ARApp.swift:200-212 for audio: for each version above audio.defaults_version (missing = 0), removes the
// stored values equal to that version's old defaults {1: ring_focus "1", 2: music "box"}, then sets it to 2.
void qx_migrate_audio_defaults(QxPrefs &p);

}  // namespace ar
