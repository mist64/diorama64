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
    // A missing or empty file succeeds with no keys. A corrupt file fails with a message and leaves the store empty
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
    std::mutex save_mu_;
    std::map<std::string, std::string> map_;
};

struct PrefLoadResult {
    std::string key, value;
    bool from_launch;
};

// Port of LookLoad.swift:15-40, minus the apply step. For each key: the stored revision is the max of
// prefix+"defaults_rev" and prefix+"defaults_version" (read once, before the loop); if it is older than new_rev and
// drop_stored(key, stored, stored_rev, has_launch) says so, the stored value is removed; the value is launch, else
// stored, else default. Afterwards, if the revision was older, prefix+"defaults_rev" = new_rev.
// The caller applies each value; on success and from_launch it stores prefix+key = value; on failure it applies the
// default and removes the stored value unless from_launch; then it saves. qx_load_values only changes the in-memory
// store and never writes a launch value itself. A key whose default_of returns false has an empty default.
std::vector<PrefLoadResult> qx_load_values(QxPrefs &p, const std::string &prefix, const std::vector<std::string> &keys,
    const std::function<bool(const std::string &, std::string *)> &default_of, int new_rev,
    const std::function<bool(const std::string &, const std::string &, int, bool)> &drop_stored,
    const std::function<bool(const std::string &, std::string *)> &launch_of);

// Port of SM64ARApp.swift:200-212 for audio: for each version above audio.defaults_version (missing = 0), removes the
// stored values equal to that version's old defaults {1: ring_focus "1", 2: music "box"}, then sets it to 2.
void qx_migrate_audio_defaults(QxPrefs &p);

}  // namespace ar
