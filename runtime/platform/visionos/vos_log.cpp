#include "vos_log.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
namespace ar {
namespace {
std::mutex g_m;
FILE *g_file = nullptr;
const auto g_t0 = std::chrono::steady_clock::now();
}
void vos_log_open(const char *path) {
    std::lock_guard<std::mutex> g(g_m);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    if (path && *path) g_file = std::fopen(path, "w");
}
void vos_log(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    std::lock_guard<std::mutex> g(g_m);
    std::fprintf(stderr, "[%8.3f] %s\n", t, msg);
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_INFO, "Diorama64", msg);
#endif
    if (g_file) { std::fprintf(g_file, "[%8.3f] %s\n", t, msg); std::fflush(g_file); }
}
void vos_exit_now(int code) {
    {
        std::lock_guard<std::mutex> g(g_m);
        if (g_file) { std::fflush(g_file); ::fsync(fileno(g_file)); }
    }
    std::fflush(nullptr);
    ::_exit(code);
}
}
