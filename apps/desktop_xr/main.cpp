// The Quest app as a Linux desktop OpenXR program, for testing without a headset on Monado (scripts/desktop_xr.py).
// The same bridge (apps/quest/quest_app.cpp), platform and session run here as on the Quest; only the Android pieces
// differ: no activity (Ctrl-C quits), no APK assets (the staged data directory is the bundle), AAudio over PulseAudio,
// and no room scan, so the table is the fixed one unless --scan-table is given.
//
//   diorama64_desktop --bundle DIR --docs DIR [--scan-table] [app arguments, e.g. --game sm64 --start story]
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "qx_app.h"
#include "vos_api.h"
#include "vos_log.h"

namespace {

std::atomic<bool> g_interrupted{false};
void on_signal(int) { g_interrupted = true; }

// arvos_layer_run calls this once per frame on the loop thread, where arvos_quest_request_quit is safe.
void pump(void *) {
    static bool asked = false;
    if (g_interrupted && !asked) {
        asked = true;
        arvos_quest_request_quit();
    }
}

int usage(const char *why) {
    std::fprintf(stderr, "diorama64_desktop: %s\nusage: diorama64_desktop --bundle DIR --docs DIR [--scan-table] [app arguments]\n", why);
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    std::string bundle, docs;
    bool scan_table = false, has_fixed = false;
    std::vector<std::string> args{"diorama64"};
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--bundle" && i + 1 < argc) bundle = argv[++i];
        else if (a == "--docs" && i + 1 < argc) docs = argv[++i];
        else if (a == "--scan-table") scan_table = true;
        else {
            if (a == "--fixed-table") has_fixed = true;
            args.push_back(a);
        }
    }
    if (bundle.empty()) return usage("--bundle DIR is required (the staged data: Bundled/ and res/)");
    if (docs.empty()) return usage("--docs DIR is required (prefs.json, placement.json, save/, sm64ar.log)");
    ::mkdir(docs.c_str(), 0755);
    // Monado has no room scan: without a fixed table the level would never appear.
    if (!scan_table && !has_fixed) args.push_back("--fixed-table");

    // SM64AR_PTRACE=1: any process of this user may attach a debugger (gdb -p) despite Yama's ptrace_scope=1.
    if (getenv("SM64AR_PTRACE")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::vector<const char *> cargv;
    for (const auto &a : args) cargv.push_back(a.c_str());
    qx_app_set_pump(pump, nullptr);
    const int configured = arvos_configure((int)cargv.size(), cargv.data(), bundle.c_str(), docs.c_str());
    std::string joined;
    for (size_t i = 1; i < args.size(); i++) { joined += ' '; joined += args[i]; }
    ar::vos_log("AR_QUEST_START desktop");
    ar::vos_log("AR_ARGS%s", joined.c_str());
    if (configured) qx_app_startup();
    arvos_layer_run(nullptr);   // returns when the session ends, a quit is asked for (Ctrl-C), or the start failed
    ar::vos_log("AR_QUEST_EXIT");
    return configured ? 0 : 1;
}
