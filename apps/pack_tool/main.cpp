// sm64ar_pack_tool --rom ROM --res RESDIR [--generate-tex IN OUT] [--generate-actors DIR]
//
// DynOS's own generators (0007-DynOS-packs.patch) after a boot into an empty temporary save, as the macOS harness's
// run_generate does; nothing is ticked or drawn. scripts/packs.py runs it on Linux.
//
// Errors print "error: ..." and exit 1, checked in this order: the ROM file exists, the resource directory exists, IN is
// a directory holding at least one .png, the ROM is accepted (init). Nothing is written before all four pass. A
// --generate-tex run that leaves no .tex in OUT is an error too, and OUT is removed again if this run created it.
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

// Whether dir (searched recursively: the PNG tree has subfolders) holds a regular file named *ext.
bool has_ext(const std::string &dir, const char *ext) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ext && it->is_regular_file(ec)) return true;
    return false;
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
    if (!o.tex_in.empty() && !has_ext(o.tex_in, ".png")) {
        std::fprintf(stderr, "error: %s is not a directory holding any .png\n", o.tex_in.c_str());
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
    auto fail = [&](const std::string &why) {
        std::fprintf(stderr, "error: %s\n", why.c_str());
        std::filesystem::remove_all(save, ec);
        return 1;
    };
    if (!o.tex_in.empty()) {
        const bool created = !std::filesystem::exists(o.tex_out, ec);
        std::filesystem::create_directories(o.tex_out, ec);
        if (ec) return fail("cannot create " + o.tex_out + ": " + ec.message());
        sm64_host_generate_tex(o.tex_in.c_str(), o.tex_out.c_str());
        if (!has_ext(o.tex_out, ".tex")) {
            if (created) std::filesystem::remove_all(o.tex_out, ec);
            return fail("no .tex generated from " + o.tex_in + " into " + o.tex_out);
        }
    }
    if (!o.actors.empty()) {
        std::filesystem::create_directories(o.actors, ec);
        if (ec) return fail("cannot create " + o.actors + ": " + ec.message());
        sm64_host_generate_actors(o.actors.c_str());
    }
    std::printf("AR_GENERATED tex=%s actors=%s\n", o.tex_out.c_str(), o.actors.c_str());
    std::fflush(stdout);
    std::filesystem::remove_all(save, ec);
    return 0;
}
