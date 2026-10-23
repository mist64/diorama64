#include "qx_prefs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace ar {

namespace {

void put_escaped(std::string &o, const std::string &s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += (char)c;
            }
        }
    }
    o += '"';
}

struct Reader {
    const std::string &t;
    size_t i = 0;
    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
    }
    bool eat(char c) {
        ws();
        if (i < t.size() && t[i] == c) {
            i++;
            return true;
        }
        return false;
    }
    bool str(std::string *out) {
        if (!eat('"')) return false;
        out->clear();
        while (i < t.size()) {
            unsigned char c = (unsigned char)t[i++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                *out += (char)c;
                continue;
            }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
            case '"': *out += '"'; break;
            case '\\': *out += '\\'; break;
            case '/': *out += '/'; break;
            case 'n': *out += '\n'; break;
            case 't': *out += '\t'; break;
            case 'r': *out += '\r'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'u': {
                if (i + 4 > t.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = t[i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                    else return false;
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) return false;
                if (cp < 0x80) {
                    *out += (char)cp;
                } else if (cp < 0x800) {
                    *out += (char)(0xC0 | (cp >> 6));
                    *out += (char)(0x80 | (cp & 0x3F));
                } else {
                    *out += (char)(0xE0 | (cp >> 12));
                    *out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    *out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }
};

bool parse_flat(const std::string &t, std::map<std::string, std::string> *out, std::string *err) {
    Reader r{t};
    std::map<std::string, std::string> m;
    auto fail = [&](const char *why) {
        if (err) *err = std::string("preferences: ") + why + " at byte " + std::to_string(r.i);
        return false;
    };
    if (!r.eat('{')) return fail("expected '{'");
    if (!r.eat('}')) {
        for (;;) {
            std::string k, v;
            if (!r.str(&k)) return fail("expected a string key");
            if (!r.eat(':')) return fail("expected ':'");
            if (!r.str(&v)) return fail("expected a string value");
            m[k] = v;
            if (r.eat(',')) continue;
            if (r.eat('}')) break;
            return fail("expected ',' or '}'");
        }
    }
    r.ws();
    if (r.i != t.size()) return fail("trailing data");
    *out = std::move(m);
    return true;
}

}  // namespace

QxPrefs::QxPrefs(const std::string &path) : path_(path) {}

bool QxPrefs::load(std::string *err) {
    std::lock_guard<std::mutex> g(mu_);
    map_.clear();
    FILE *f = fopen(path_.c_str(), "rb");
    if (!f) {
        if (errno == ENOENT) return true;
        if (err) *err = "preferences: cannot open " + path_ + ": " + strerror(errno);
        return false;
    }
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) {
        if (err) *err = "preferences: read error on " + path_;
        return false;
    }
    return parse_flat(text, &map_, err);
}

bool QxPrefs::save(std::string *err) {
    std::string text = "{";
    {
        std::lock_guard<std::mutex> g(mu_);
        bool first = true;
        for (const auto &kv : map_) {
            text += first ? "\n  " : ",\n  ";
            first = false;
            put_escaped(text, kv.first);
            text += ": ";
            put_escaped(text, kv.second);
        }
        text += first ? "}\n" : "\n}\n";
    }
    const std::string tmp = path_ + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        if (err) *err = "preferences: cannot write " + tmp + ": " + strerror(errno);
        return false;
    }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0 && fsync(fileno(f)) == 0;
    int e = errno;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        if (err) *err = "preferences: write failed on " + tmp + ": " + strerror(e);
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path_.c_str()) != 0) {
        if (err) *err = "preferences: rename onto " + path_ + " failed: " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool QxPrefs::has(const std::string &key) const {
    std::lock_guard<std::mutex> g(mu_);
    return map_.count(key) != 0;
}
std::string QxPrefs::get(const std::string &key, const std::string &fallback) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = map_.find(key);
    return it == map_.end() ? fallback : it->second;
}
void QxPrefs::set(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> g(mu_);
    map_[key] = value;
}
void QxPrefs::remove(const std::string &key) {
    std::lock_guard<std::mutex> g(mu_);
    map_.erase(key);
}
int QxPrefs::get_int(const std::string &key, int fallback) const {
    std::string s;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return fallback;
        s = it->second;
    }
    if (s.empty()) return fallback;
    char *end = nullptr;
    errno = 0;
    long v = strtol(s.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || v < -2147483647L - 1 || v > 2147483647L) return fallback;
    return (int)v;
}
void QxPrefs::set_int(const std::string &key, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    set(key, b);
}

std::vector<PrefLoadResult> qx_load_values(QxPrefs &p, const std::string &prefix, const std::vector<std::string> &keys,
    const std::function<bool(const std::string &, std::string *)> &default_of, int new_rev,
    const std::function<bool(const std::string &, const std::string &, int, bool)> &drop_stored,
    const std::function<bool(const std::string &, std::string *)> &launch_of) {
    std::vector<PrefLoadResult> out;
    const int a = p.get_int(prefix + "defaults_rev", 0), b = p.get_int(prefix + "defaults_version", 0);
    const int rev = a > b ? a : b;
    for (const std::string &key : keys) {
        std::string def, launch;
        if (!default_of(key, &def)) def.clear();
        const bool has_launch = launch_of(key, &launch);
        const std::string full = prefix + key;
        bool has_stored = p.has(full);
        std::string stored = p.get(full);
        if (rev < new_rev && has_stored && drop_stored(key, stored, rev, has_launch)) {
            p.remove(full);
            has_stored = false;
        }
        PrefLoadResult r{key, has_launch ? launch : has_stored ? stored : def, has_launch};
        if (has_launch) p.set(full, r.value);
        out.push_back(std::move(r));
    }
    if (rev < new_rev) p.set_int(prefix + "defaults_rev", new_rev);
    return out;
}

void qx_migrate_audio_defaults(QxPrefs &p) {
    static const struct {
        int version;
        const char *key, *old;
    } kChanged[] = {{1, "ring_focus", "1"}, {2, "music", "box"}};
    const int latest = 2;
    const int have = p.get_int("audio.defaults_version", 0);
    if (have >= latest) return;
    for (const auto &c : kChanged) {
        if (c.version <= have) continue;
        const std::string k = std::string("audio.") + c.key;
        if (p.has(k) && p.get(k) == c.old) p.remove(k);
    }
    p.set_int("audio.defaults_version", latest);
}

}  // namespace ar
