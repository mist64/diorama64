#pragma once
// Copies the APK's bundled data (assets Bundled/ and res/) to the app's private files directory (Android only).
#include <functional>
#include <string>

struct AAssetManager;

namespace ar {

// Copies every file named in the assets' Bundled/bundled.json (to dest_dir/Bundled/...) and res/res.json (to
// dest_dir/res/...), plus the two manifests, into dest_dir + ".tmp", then removes an old dest_dir and renames the .tmp
// into place. An interrupted copy leaves only the .tmp, which the next call discards and redoes. False with *err set on
// a failure. *copied (if given) receives the file count.
// progress (if set) is called on the calling thread with the files copied so far, at most every 50 ms, between files
// and between the chunks of a large one, so the caller can keep its event loop alive (android_main pumps its looper).
using BundledProgress = std::function<void(int files_done)>;
bool bundled_copy(AAssetManager *am, const std::string &dest_dir, std::string *err, int *copied = nullptr,
                  const BundledProgress &progress = nullptr);

// bundled_copy only when an installed manifest in dest_dir is missing or differs from the APK's. *copied is -1 when
// everything was up to date. progress as bundled_copy's.
bool bundled_sync(AAssetManager *am, const std::string &dest_dir, std::string *err, int *copied,
                  const BundledProgress &progress = nullptr);

}  // namespace ar
