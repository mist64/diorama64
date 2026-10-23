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

// The install directory dest is replaced by renames only, so a kill never leaves a half-deleted dest that the
// manifest check would call up to date. States (dest, dest.tmp, dest.old):
//   copy:    files go to dest.tmp; a kill leaves dest untouched (and a stale .tmp)
//   swap:    dest -> dest.old, dest.tmp -> dest, remove dest.old; a kill between the renames leaves .old and no dest
//   startup: bundled_recover_plan restores .old when dest is missing, then discards leftover .old and .tmp
enum class SwapStep { restore_old, remove_old, remove_tmp, dest_to_old, tmp_to_dest };

std::vector<SwapStep> bundled_recover_plan(bool dest_exists, bool tmp_exists, bool old_exists);
std::vector<SwapStep> bundled_swap_plan(bool dest_exists);

}  // namespace ar
