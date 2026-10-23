// sm64ar_pack_tool --rom ROM --res RESDIR [--generate-tex IN OUT] [--generate-actors DIR]
//
// DynOS's own generators (0007-DynOS-packs.patch) after a boot into an empty temporary save, as the macOS harness's
// run_generate does; nothing is ticked or drawn. scripts/packs.py runs it on Linux. On a missing or refused ROM it
// exits 1 before anything is written, so OUT is left untouched.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "ar_adapter_sm64.h"
#include "sm64_host.h"

namespace {

struct Options {
    std::string rom, res, tex_in, tex_out, actors;
};

int usage(const char *why) {
    std::fprintf(stderr, "error: %s\nusage: sm64ar_pack_tool --rom ROM --res RESDIR [--generate-tex IN OUT] "
                         "[--generate-actors DIR]\n", why);
    return 2;
}

// Returns nullptr on success, else what is wrong.
const char *parse(int argc, char **argv, Options &o) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const int left = argc - i - 1;
        if (!std::strcmp(a, "--rom") && left >= 1) o.rom = argv[++i];
        else if (!std::strcmp(a, "--res") && left >= 1) o.res = argv[++i];
        else if (!std::strcmp(a, "--generate-tex") && left >= 2) { o.tex_in = argv[++i]; o.tex_out = argv[++i]; }
        else if (!std::strcmp(a, "--generate-actors") && left >= 1) o.actors = argv[++i];
        else return "unknown or incomplete argument";
    }
    if (o.rom.empty()) return "--rom is required";
    if (o.res.empty()) return "--res is required";
    if (o.tex_in.empty() && o.actors.empty()) return "nothing to do: give --generate-tex and/or --generate-actors";
    return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    if (const char *why = parse(argc, argv, o)) return usage(why);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(o.rom, ec)) {
        std::fprintf(stderr, "error: no ROM at %s\n", o.rom.c_str());
        return 1;
    }
    if (!std::filesystem::is_directory(o.res, ec)) {
        std::fprintf(stderr, "error: no resource directory at %s (run python3 scripts/sm64_oracle.py)\n", o.res.c_str());
        return 1;
    }
    ar_adapter_sm64_set_resource_dir(o.res.c_str());
    char save[] = "/tmp/sm64ar-gen-XXXXXX";
    if (!mkdtemp(save)) {
        std::fprintf(stderr, "error: cannot create a temporary save\n");
        return 1;
    }
    if (!ar_adapter_sm64()->init(o.rom.c_str(), save)) {
        std::fprintf(stderr, "error: %s\n", ar_adapter_sm64()->last_error());
        std::filesystem::remove_all(save, ec);
        return 1;
    }
    if (!o.tex_in.empty()) {
        std::filesystem::create_directories(o.tex_out, ec);
        if (ec) {
            std::fprintf(stderr, "error: cannot create %s: %s\n", o.tex_out.c_str(), ec.message().c_str());
            std::filesystem::remove_all(save, ec);
            return 1;
        }
        sm64_host_generate_tex(o.tex_in.c_str(), o.tex_out.c_str());
    }
    if (!o.actors.empty()) sm64_host_generate_actors(o.actors.c_str());
    std::printf("AR_GENERATED tex=%s actors=%s\n", o.tex_out.c_str(), o.actors.c_str());
    std::fflush(stdout);
    std::filesystem::remove_all(save, ec);
    return 0;
}
