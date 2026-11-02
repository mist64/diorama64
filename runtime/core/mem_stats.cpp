#include "ar_mem_stats.h"
#if defined(__APPLE__)
#include <mach/mach.h>
double ar_phys_footprint_mb(void) {
    task_vm_info_data_t info{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &n) != KERN_SUCCESS) return 0.0;
    return double(info.phys_footprint) / (1024.0 * 1024.0);
}
#elif defined(__linux__)
#include <unistd.h>
#include <cstdio>
// Linux and Android: resident set from /proc/self/statm (field 2, in pages).
double ar_phys_footprint_mb(void) {
    FILE *f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0.0;
    unsigned long long size = 0, resident = 0;
    const int n = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    const long page = sysconf(_SC_PAGESIZE);
    if (n != 2 || page <= 0) return 0.0;
    return double(resident) * double(page) / (1024.0 * 1024.0);
}
#else
double ar_phys_footprint_mb(void) { return 0.0; }
#endif
