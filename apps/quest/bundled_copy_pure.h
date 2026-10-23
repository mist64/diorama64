#pragma once
// The pure half of the Quest's copy of the APK's bundled data to the app's private files directory (the AAssetManager
// half is bundled_copy.h, Android only).
#include <string>
#include <vector>

namespace ar {

// The file names in a manifest, in order. Two shapes: Bundled/bundled.json is {"files": {"<relative path>": "<sha256>",
// ...}} (the keys are returned); res/res.json is a JSON array of relative paths ["lang/English.ini", ...] (the strings
// are returned). Anything else gives an empty list.
std::vector<std::string> bundled_manifest_files(const std::string &json);

// True when the manifest installed at installed_manifest_path is missing or differs byte for byte from the APK's.
bool bundled_copy_needed(const std::string &asset_manifest_json, const std::string &installed_manifest_path);

}  // namespace ar
