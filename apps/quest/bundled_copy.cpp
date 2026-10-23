#include "bundled_copy.h"

#include <android/asset_manager.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bundled_copy_pure.h"

namespace ar {
namespace {

const char *const kManifests[] = {"Bundled/bundled.json", "res/res.json"};

bool read_asset(AAssetManager *am, const std::string &name, std::string *out) {
    AAsset *a = AAssetManager_open(am, name.c_str(), AASSET_MODE_STREAMING);
    if (!a) return false;
    out->clear();
    char buf[1 << 16];
    int n;
    while ((n = AAsset_read(a, buf, sizeof buf)) > 0) out->append(buf, (size_t)n);
    AAsset_close(a);
    return n == 0;
}

bool make_dirs(const std::string &path) {   // mkdir -p
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        if (mkdir(part.c_str(), 0771) != 0 && errno != EEXIST) return false;
    }
    return true;
}

void remove_tree(const std::string &path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(path.c_str())) {
            while (dirent *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                remove_tree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

bool exists(const std::string &p) { struct stat st; return lstat(p.c_str(), &st) == 0; }

bool safe_name(const std::string &n) {   // relative, no ".." component
    if (n.empty() || n[0] == '/') return false;
    size_t i = 0;
    while (i <= n.size()) {
        size_t j = n.find('/', i);
        if (j == std::string::npos) j = n.size();
        if (n.compare(i, j - i, "..") == 0) return false;
        i = j + 1;
    }
    return true;
}

// Runs plan steps over dest; false with *err on a failed rename.
bool run_steps(const std::vector<SwapStep> &steps, const std::string &dest, std::string *err) {
    const std::string tmp = dest + ".tmp", old = dest + ".old";
    for (SwapStep s : steps) {
        switch (s) {
            case SwapStep::restore_old: if (rename(old.c_str(), dest.c_str()) != 0) { *err = "cannot restore " + old + ": " + strerror(errno); return false; } break;
            case SwapStep::remove_old: remove_tree(old); break;
            case SwapStep::remove_tmp: remove_tree(tmp); break;
            case SwapStep::dest_to_old: remove_tree(old); if (rename(dest.c_str(), old.c_str()) != 0) { *err = "cannot move " + dest + " aside: " + strerror(errno); return false; } break;
            case SwapStep::tmp_to_dest: if (rename(tmp.c_str(), dest.c_str()) != 0) { *err = "cannot rename " + tmp + " to " + dest + ": " + strerror(errno); return false; } break;
        }
    }
    return true;
}

// bundled_copy's progress, throttled to one call per 50 ms.
struct Ticker {
    const BundledProgress &fn;
    int files = 0;
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
    void operator()() {
        if (!fn) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < next) return;
        next = now + std::chrono::milliseconds(50);
        fn(files);
    }
};

bool copy_one(AAssetManager *am, const std::string &asset, const std::string &dest, std::vector<char> &buf, std::string *err,
              Ticker &tick) {
    AAsset *a = AAssetManager_open(am, asset.c_str(), AASSET_MODE_STREAMING);
    if (!a) { *err = "asset missing: " + asset; return false; }
    const size_t slash = dest.rfind('/');
    if (slash != std::string::npos && !make_dirs(dest.substr(0, slash))) {
        AAsset_close(a);
        *err = "cannot create " + dest.substr(0, slash) + ": " + strerror(errno);
        return false;
    }
    FILE *f = fopen(dest.c_str(), "wb");
    if (!f) { AAsset_close(a); *err = "cannot write " + dest + ": " + strerror(errno); return false; }
    int n;
    bool ok = true;
    while ((n = AAsset_read(a, buf.data(), buf.size())) > 0) {
        if (fwrite(buf.data(), 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
        tick();
    }
    if (n < 0) ok = false;
    if (fclose(f) != 0) ok = false;
    AAsset_close(a);
    if (!ok) *err = "copy failed: " + asset + " -> " + dest;
    return ok;
}

}  // namespace

bool bundled_copy(AAssetManager *am, const std::string &dest_dir, std::string *err, int *copied, const BundledProgress &progress) {
    std::string e;
    if (!err) err = &e;
    Ticker tick{progress};
    const std::string tmp = dest_dir + ".tmp";
    remove_tree(tmp);   // a copy interrupted last time
    int &count = tick.files;
    std::vector<char> buf(1 << 18);
    for (const char *manifest : kManifests) {
        std::string json;
        if (!read_asset(am, manifest, &json)) { *err = std::string("asset missing: ") + manifest; remove_tree(tmp); return false; }
        const std::string prefix = std::string(manifest).substr(0, std::string(manifest).find('/') + 1);   // "Bundled/" | "res/"
        for (const std::string &rel : bundled_manifest_files(json)) {
            if (!safe_name(rel)) { *err = "bad name in " + std::string(manifest) + ": " + rel; remove_tree(tmp); return false; }
            if (!copy_one(am, prefix + rel, tmp + "/" + prefix + rel, buf, err, tick)) { remove_tree(tmp); return false; }
            count++;
            tick();
        }
        if (!copy_one(am, manifest, tmp + "/" + manifest, buf, err, tick)) { remove_tree(tmp); return false; }   // last: the marker of a whole copy
    }
    if (!run_steps(bundled_swap_plan(exists(dest_dir)), dest_dir, err)) return false;
    if (copied) *copied = count;
    return true;
}

bool bundled_sync(AAssetManager *am, const std::string &dest_dir, std::string *err, int *copied, const BundledProgress &progress) {
    std::string rerr;
    bool needed = !run_steps(bundled_recover_plan(exists(dest_dir), exists(dest_dir + ".tmp"), exists(dest_dir + ".old")), dest_dir, &rerr);
    for (const char *manifest : kManifests) {
        if (needed) break;
        std::string json;
        if (!read_asset(am, manifest, &json)) { needed = true; break; }   // bundled_copy reports the missing asset
        if (bundled_copy_needed(json, dest_dir + "/" + manifest)) { needed = true; break; }
    }
    if (!needed) { *copied = -1; return true; }
    return bundled_copy(am, dest_dir, err, copied, progress);
}

}  // namespace ar
