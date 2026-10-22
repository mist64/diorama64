// The Vulkan device draws the cube through the shared Renderer (draw_ar_layer) under lavapipe and reads it back.
#include "check.h"
#include <string>
#include <vector>
#include "ar_layer.h"
#include "cube_adapter.h"
#include "fake_platform.h"
#include "renderer.h"
#include "vk_host_context.h"
#include "vulkan_device.h"

using namespace ar;

int main() {
    VulkanDeviceInit init{}; std::string err;
    if (!vk_host_create(&init, &err)) SKIP(err.c_str());
    auto dev = create_vulkan_device(init);
    CHECK(dev);
    Renderer renderer(*dev);
    fake_platform fp; fake_platform_init(&fp, 90.0); fp.width = 256; fp.height = 256; fp.view_count = 2; fp.stop_after = 4;
    ar_runtime *rt = ar_runtime_create(cube_adapter(), &fp.api);
    CHECK(ar_runtime_init_game(rt, "", ""));
    ar_runtime_set_texture_sink(rt, [](void *u, uint32_t id, uint32_t w, uint32_t h, const uint8_t *px) { static_cast<IRenderDevice *>(u)->upload_texture(id, w, h, px); }, dev.get());
    const uint32_t target = dev->create_target(256, 256, 2, 1);
    ar_render_frame rf{};
    for (int i = 0; i < 4; i++) {
        CHECK(ar_runtime_begin_frame(rt, &rf));
        while (ar_runtime_ticks_due(rt)) ar_runtime_step_game(rt);
        if (rf.snapshot) {
            LightingFrame lf; ArLayerOptions lo; lo.layered = true; lo.lighting = &lf; lo.overlays = false;
            dev->begin_frame();
            draw_ar_layer(renderer, target, rf, lo, 1.f / 90.f);
            ar_runtime_submit_frame(rt, &rf);
            dev->end_frame([](void *) {}, nullptr, nullptr);
        } else { ar_runtime_submit_frame(rt, &rf); }
        ar_runtime_end_frame(rt, &rf);
    }
    dev->wait_idle();
    std::vector<uint8_t> rgba; uint32_t w = 0, h = 0;
    CHECK(dev->read_target(target, rgba, w, h) && w == 256 && h == 256 && rgba.size() == 256u * 256u * 4u);
    // the cube (40 cm at 1:8000 on a table 0.75 m up, head 2 m back at 1.5 m) covers the middle columns below the
    // horizon, not the corners: its vertices project to x 104..161, y 169..235 (top-left origin), so row 200 is inside
    auto px = [&](uint32_t x, uint32_t y) { return &rgba[(y * 256 + x) * 4]; };
    CHECK(px(128, 200)[3] == 255 && (px(128, 200)[0] + px(128, 200)[1] + px(128, 200)[2]) > 0);
    CHECK(px(2, 2)[3] == 0 && px(253, 253)[3] == 0);
    std::vector<float> depth; CHECK(dev->read_target_depth(target, depth, w, h) && depth[200 * 256 + 128] > 0.f && depth[2 * 256 + 2] == 0.f);
    // a pipeline error is reported, not fatal
    fc_combiner_key bad{}; bad.slots = ~0ull; std::string why;
    CHECK(!dev->compile_combiner(bad, &why) || dev->pipeline_count() > 0);
    std::string perr; (void)dev->take_pipeline_error(&perr);
    CHECK(dev->pipeline_count() >= 1);
    ar_runtime_destroy(rt); dev.reset(); vk_host_destroy(init);
    return 0;
}
