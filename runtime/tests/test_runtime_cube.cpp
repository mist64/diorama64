#include "check.h"
#include <cmath>
#include "ar_runtime.h"
#include "ar_mem_stats.h"
#include "cube_adapter.h"
#include "fake_platform.h"

int main() {
#if defined(__linux__) || defined(__APPLE__)
    CHECK(ar_phys_footprint_mb() > 0.0);
#endif
    fake_platform fp;
    fake_platform_init(&fp, 90.0);
    fp.stop_after = 10;
    ar_runtime *rt = ar_runtime_create(cube_adapter(), &fp.api);
    CHECK(rt != nullptr);
    CHECK(ar_runtime_init_game(rt, "", "") == 1);
    ar_render_frame rf{};
    ar_render_frame last{};
    int frames = 0;
    while (ar_runtime_begin_frame(rt, &rf)) {
        while (ar_runtime_ticks_due(rt) > 0) ar_runtime_step_game(rt);
        ar_runtime_submit_frame(rt, &rf);
        ar_runtime_end_frame(rt, &rf);
        last = rf;
        frames++;
    }
    CHECK(frames == 10);
    CHECK(fp.submitted == 10);
    CHECK(cube_ticks() >= 3);
    CHECK(last.snapshot != nullptr);
    CHECK(last.view_count == 1);
    for (int i = 0; i < 16; i++) CHECK(std::isfinite(last.views[0].clip_from_level.m[i]));
    ar_runtime_destroy(rt);
    return 0;
}
