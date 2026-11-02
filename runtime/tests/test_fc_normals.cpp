// Smooth-normal derivation on a mesh large enough for the parallel path (the worker pool off Apple, dispatch on Apple):
// a finely tessellated sphere must come out with radial normals, the parallel path must have run, and a second
// derivation must give byte-identical normals.
#include <cmath>
#include <cstring>
#include <vector>
#include "check.h"
#include "fc_builder.h"

namespace {

constexpr float kR = 1000.f;

void add_sphere(fc_builder *b, int lat, int lon) {
    fc_draw d;
    std::memset(&d, 0, sizeof d);
    d.light_flags = FC_LF_GAME_LIT;   // dynamic (not FC_LF_STATIC): the set the HD models land in
    const uint32_t n = (uint32_t)(lat * lon * 6);
    fc_vertex *v = fc_builder_add_draw(b, &d, n);
    CHECK(v != nullptr);
    auto at = [](int i, int j, int lat_, int lon_, float out[3]) {
        const float th = 3.14159265f * (float)i / (float)lat_, ph = 6.2831853f * (float)j / (float)lon_;
        out[0] = kR * std::sin(th) * std::cos(ph);
        out[1] = kR * std::cos(th);
        out[2] = kR * std::sin(th) * std::sin(ph);
    };
    uint32_t k = 0;
    for (int i = 0; i < lat; i++)
        for (int j = 0; j < lon; j++) {
            const int q[6][2] = {{i, j}, {i + 1, j + 1}, {i + 1, j}, {i, j}, {i, j + 1}, {i + 1, j + 1}};
            for (const auto &c : q) {
                std::memset(&v[k], 0, sizeof v[k]);
                at(c[0], c[1], lat, lon, v[k].pos);
                k++;
            }
        }
}

std::vector<int8_t> derive(fc_builder *b, int lat, int lon, uint32_t *nverts) {
    fc_builder_reset(b, 1, 1, 1);
    add_sphere(b, lat, lon);
    fc_builder_derive_normals(b, FC_LF_GAME_LIT, 50.f, 1.f);
    const fc_frame *f = fc_builder_finish(b);
    CHECK(f->normals != nullptr);
    *nverts = f->vert_count;
    for (uint32_t i = 0; i < f->vert_count; i++) {
        const float *p = f->verts[i].pos;
        // A degenerate triangle's corners get no normal (the poles' rows): skip them.
        const float *a = f->verts[i - i % 3].pos, *b1 = f->verts[i - i % 3 + 1].pos, *c = f->verts[i - i % 3 + 2].pos;
        const float e1[3] = {b1[0] - a[0], b1[1] - a[1], b1[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        if (cx * cx + cy * cy + cz * cz < 1e-3f) continue;
        const int8_t *nm = &f->normals[(size_t)i * 4];
        const float dot = (nm[0] * p[0] + nm[1] * p[1] + nm[2] * p[2]) / (127.f * kR);
        if (dot < 0.97f) { printf("vertex %u: normal %d %d %d at %g %g %g (dot %g)\n", i, nm[0], nm[1], nm[2], p[0], p[1], p[2], dot); CHECK(false); }
    }
    return std::vector<int8_t>(f->normals, f->normals + (size_t)f->vert_count * 4);
}

}  // namespace

int main() {
    fc_builder *b = fc_builder_create();
    uint32_t nv = 0;
    // Serial: under kParallelVerts.
    derive(b, 20, 40, &nv);
    CHECK(fc_builder_normal_parallel_runs(b) == 0);
    // Parallel: 172800 vertices in one cluster, split across the workers.
    const std::vector<int8_t> first = derive(b, 120, 240, &nv);
    CHECK(nv == 120u * 240u * 6u);
    CHECK(fc_builder_normal_parallel_runs(b) >= 1);
    for (int run = 0; run < 3; run++) {
        const std::vector<int8_t> again = derive(b, 120, 240, &nv);
        CHECK(again == first);
    }
    fc_builder_destroy(b);
    printf("test_fc_normals OK\n");
    return 0;
}
