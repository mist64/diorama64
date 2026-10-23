// The Vulkan device draws the cube room-lit through the shared Renderer (draw_ar_layer) under lavapipe: the shadow and
// underside maps, the AO bake and the contact shadow on the table.
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
    fake_platform fp; fake_platform_init(&fp, 90.0); fp.width = 256; fp.height = 256; fp.view_count = 2; fp.stop_after = 64;
    ar_runtime *rt = ar_runtime_create(cube_adapter(), &fp.api);
    CHECK(ar_runtime_init_game(rt, "", ""));
    ar_runtime_set_texture_sink(rt, [](void *u, uint32_t id, uint32_t w, uint32_t h, const uint8_t *px) { static_cast<IRenderDevice *>(u)->upload_texture(id, w, h, px); }, dev.get());
    const uint32_t target = dev->create_target(256, 256, 2, 1);
    const ar_level_info level = cube_adapter()->level_info();
    ar_render_frame rf{};
    auto run = [&](int frames) {
        for (int i = 0; i < frames; i++) {
            CHECK(ar_runtime_begin_frame(rt, &rf));
            while (ar_runtime_ticks_due(rt)) ar_runtime_step_game(rt);
            if (rf.snapshot) {
                LightingFrame lf; lf.on = true; lf.shadows = 2; lf.contact = true; lf.contact_strength = 0.5f;
                lf.ao = true; lf.ao_strength = 0.6f; lf.upm = 8000.f;
                for (int k = 0; k < 3; k++) { lf.bounds_min[k] = level.bounds_min[k]; lf.bounds_max[k] = level.bounds_max[k]; }
                ArLayerOptions lo; lo.layered = true; lo.lighting = &lf; lo.overlays = true;
                dev->begin_frame();
                draw_ar_layer(renderer, target, rf, lo, 1.f / 90.f);
                ar_runtime_submit_frame(rt, &rf);
                dev->end_frame([](void *) {}, nullptr, nullptr);
            } else { ar_runtime_submit_frame(rt, &rf); }
            ar_runtime_end_frame(rt, &rf);
        }
        dev->wait_idle();
    };
    run(30);
    std::vector<uint8_t> rgba; uint32_t w = 0, h = 0;
    CHECK(dev->read_target(target, rgba, w, h) && w == 256 && h == 256);
    auto px = [&](uint32_t x, uint32_t y) { return &rgba[(y * 256 + x) * 4]; };
    uint64_t b0 = 0; double ms0 = 0.0;
    dev->ao_bake_time(&b0, &ms0);   // printed with the luminances, checked below
    // the contact shadow: the table at the cube's base is darker than the table far from it. The cube covers x 104..160,
    // rows 172..234 here; the ring in front of its front face is under a pixel deep at this angle (one row is ~160 level
    // units of table, the blur radius 120), so the base is read beside its bottom edges, on both sides. The layer is
    // premultiplied and the table shadow is black with alpha (it darkens passthrough), so luminance is taken over white:
    // what the passthrough behind the pixel shows through it.
    auto lum = [&](uint32_t x, uint32_t y) {
        const uint8_t *p = px(x, y);
        const float a = p[3] / 255.f;
        return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] + 255.f * (1.f - a);
    };
    printf("luminance at the cube's base %.1f (left) %.1f (right), far from it %.1f; ao bake %llu in %.3f ms\n", lum(103, 232),
           lum(161, 232), lum(20, 232), (unsigned long long)b0, ms0);
    CHECK(px(128, 200)[3] == 255);
    CHECK(renderer.ao_bakes() >= 1);
    uint64_t b = 0; double ms = 0.0;
    CHECK(dev->ao_bake_time(&b, &ms) && b >= 1);
    std::string why;
    CHECK(dev->take_pipeline_error(&why) == false);
    CHECK(lum(103, 232) < lum(20, 232) && lum(161, 232) < lum(20, 232));
    ar_runtime_destroy(rt); dev.reset(); vk_host_destroy(init);
    return 0;
}
