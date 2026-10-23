#include "bundled_copy_pure.h"

#include <cctype>
#include <fstream>
#include <iterator>

namespace ar {
namespace {

void skip_ws(const std::string &s, size_t &i) {
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
}

// A JSON string starting at s[i] == '"'; i ends after the closing quote. False when unterminated.
bool read_string(const std::string &s, size_t &i, std::string *out) {
    out->clear();
    i++;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return true;
        if (c == '\\' && i < s.size()) {
            const char e = s[i++];
            out->push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
        } else {
            out->push_back(c);
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> bundled_manifest_files(const std::string &json) {
    std::vector<std::string> out;
    size_t i = 0;
    skip_ws(json, i);
    if (i >= json.size()) return out;
    if (json[i] == '[') {   // res.json
        for (i++; i < json.size() && json[i] != ']';) {
            skip_ws(json, i);
            if (i < json.size() && json[i] == '"') {
                std::string v;
                if (!read_string(json, i, &v)) break;
                out.push_back(v);
            } else {
                i++;
            }
        }
        return out;
    }
    if (json[i] != '{') return out;
    const size_t at = json.find("\"files\"", i);   // bundled.json
    if (at == std::string::npos) return out;
    i = json.find('{', at);
    if (i == std::string::npos) return out;
    i++;
    bool key = true;   // strings alternate key, value
    while (i < json.size() && json[i] != '}') {
        if (json[i] == '"') {
            std::string v;
            if (!read_string(json, i, &v)) break;
            if (key) out.push_back(v);
            key = !key;
        } else {
            i++;
        }
    }
    return out;
}

bool bundled_copy_needed(const std::string &asset_manifest_json, const std::string &installed_manifest_path) {
    std::ifstream f(installed_manifest_path, std::ios::binary);
    if (!f) return true;
    const std::string have((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return have != asset_manifest_json;
}

}  // namespace ar
