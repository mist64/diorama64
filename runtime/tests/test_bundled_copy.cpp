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
    printf("ok\n");
    return 0;
}
