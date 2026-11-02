// The pure parts of the Quest's APK-asset copy (apps/quest/bundled_copy_pure.h).
#include "check.h"
#include "bundled_copy_pure.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    const std::string m = R"({"files": {"rom.z64": "aa", "packs/p/x.png": "bb", "packs/p/.sm64ar-verified": "cc"}})";
    const std::vector<std::string> f = ar::bundled_manifest_files(m);
    CHECK(f.size() == 3);
    CHECK(f[0] == "rom.z64");
    CHECK(f[1] == "packs/p/x.png");
    CHECK(f[2] == "packs/p/.sm64ar-verified");

    const std::vector<std::string> r = ar::bundled_manifest_files(R"(["lang/English.ini", "mods/a b.lua"])");   // res.json
    CHECK(r.size() == 2 && r[0] == "lang/English.ini" && r[1] == "mods/a b.lua");
    const std::vector<std::string> r2 = ar::bundled_manifest_files(R"({"files": {"lang/English.ini": "ab", "mods/a b.lua": "cd"}})");
    CHECK(r2 == r);   // res.json as quest_bundle.py writes it now: {"files": {name: sha256}}
    CHECK(ar::bundled_manifest_files("").empty());
    CHECK(ar::bundled_manifest_files("{}").empty());

    const std::string path = "test_bundled_copy_installed.json";
    std::remove(path.c_str());
    CHECK(ar::bundled_copy_needed(m, path));                       // missing
    FILE *fp = fopen(path.c_str(), "wb");
    CHECK(fp);
    fwrite(m.data(), 1, m.size(), fp);
    fclose(fp);
    CHECK(!ar::bundled_copy_needed(m, path));                      // identical
    CHECK(ar::bundled_copy_needed(m + " ", path));                 // differs
    CHECK(ar::bundled_copy_needed(R"({"files": {"rom.z64": "zz"}})", path));
    std::remove(path.c_str());
    using ar::SwapStep;
    using V = std::vector<SwapStep>;
    CHECK(ar::bundled_recover_plan(true, false, false).empty());
    CHECK(ar::bundled_recover_plan(true, true, true) == (V{SwapStep::remove_old, SwapStep::remove_tmp}));
    CHECK(ar::bundled_recover_plan(false, true, true) == (V{SwapStep::restore_old, SwapStep::remove_tmp}));   // killed between the renames
    CHECK(ar::bundled_recover_plan(false, false, false).empty());
    CHECK(ar::bundled_swap_plan(true) == (V{SwapStep::dest_to_old, SwapStep::tmp_to_dest, SwapStep::remove_old}));
    CHECK(ar::bundled_swap_plan(false) == (V{SwapStep::tmp_to_dest}));
    printf("ok\n");
    return 0;
}
