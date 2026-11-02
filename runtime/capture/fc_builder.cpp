#include "fc_builder.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <condition_variable>
#include <mutex>
#endif
static_assert(sizeof(fc_vertex) == 32, "fc_vertex is the fixed 32-byte vertex");
static_assert(sizeof(fc_draw) == 96, "fc_draw layout (FC_ABI_VERSION 5)");
static_assert(offsetof(fc_draw, clip) == 72 && offsetof(fc_draw, focus) == 88 && offsetof(fc_draw, light_flags) == 89 &&
              offsetof(fc_draw, light_role) == 90, "fc_draw field offsets");
static_assert(sizeof(fc_overlay_group) == 44 && offsetof(fc_overlay_group, flags) == 2 && offsetof(fc_overlay_group, box) == 28,
              "fc_overlay_group layout (FC_ABI_VERSION 5)");
static_assert(sizeof(fc_polyline) == 16, "fc_polyline layout");
static_assert(sizeof(fc_frame) == 216 && offsetof(fc_frame, game_view) == 184 && offsetof(fc_frame, focus_styles) == 192 &&
              offsetof(fc_frame, focus_style_count) == 200 && offsetof(fc_frame, look) == 208,
              "fc_frame layout (FC_ABI_VERSION 5, 64-bit pointers)");
static_assert(sizeof(fc_look) == 248 && offsetof(fc_look, cut_target_prev) == 236, "fc_look layout");
static_assert(sizeof(fc_focus_style) == 8, "fc_focus_style layout");
static_assert(sizeof(fc_game_view) == 84, "fc_game_view layout");
static_assert(offsetof(fc_frame, prev_pos) == 136 && offsetof(fc_frame, has_level) == 144 && offsetof(fc_frame, level) == 148,
              "fc_frame ABI 5 offsets");
static_assert(offsetof(fc_frame, seq) == 8 && offsetof(fc_frame, verts) == 24 && offsetof(fc_frame, world) == 40 &&
              offsetof(fc_frame, overlay) == 56 && offsetof(fc_frame, discarded_draws) == 68 &&
              offsetof(fc_frame, groups) == 80 && offsetof(fc_frame, line_points) == 96 &&
              offsetof(fc_frame, lines) == 112 && offsetof(fc_frame, line_count) == 120 &&
              offsetof(fc_frame, normals) == 128, "fc_frame field offsets");
constexpr uint32_t kParallelVerts = 60000, kWorkers = 4, kSplitVerts = 16384;
struct fc_weld_scratch {
    std::vector<uint64_t> ckey;
    std::vector<uint32_t> cvert, cgroup;
    std::vector<uint32_t> gstart, gvert;
    std::vector<uint64_t> hkey;
    std::vector<uint32_t> hgroup, hgen;
    uint32_t hstamp = 0;
    std::vector<uint32_t> gface, grep;
    std::vector<uint32_t> rface, groot;
    std::vector<float> gsum;
};
// thread_local: the snapshot ring's builders are filled one at a time on the game thread, so they share it (a frame
// allocates nothing in steady state). Clusters own disjoint vertices, so workers write face entries without sharing an
// element.
struct fc_derive_scratch {
    std::vector<float> face_n;
    std::vector<float> face_u;
    std::vector<uint64_t> vkey;
    struct KeyBox { uint32_t lo[3], hi[3]; };
    struct Segment { uint32_t first, count; };
    std::vector<Segment> sseg;
    std::vector<KeyBox> sbox;
    std::vector<uint32_t> sorder, sroot;
    std::vector<uint32_t> cseg, cstart, cvcount;
    struct Item { uint32_t cluster, part, nparts; };
    std::vector<Item> items;
    std::vector<uint32_t> fseg;
    std::vector<uint8_t> vpart;
    fc_weld_scratch weld[kWorkers];
};
struct fc_builder {
    std::vector<fc_vertex> verts;
    std::vector<fc_draw> world, overlay;
    std::vector<fc_overlay_group> groups;
    std::vector<float> points;
    std::vector<fc_polyline> lines;
    std::vector<int8_t> normals;
    std::vector<float> prev;
    bool prev_on = false;
    bool gview_on = false;
    fc_game_view gview{};
    std::vector<fc_focus_style> focus_styles;
    bool look_on = false;
    fc_look look{};
    uint32_t prev_dropped = 0;
    struct NormalCacheEntry { uint64_t hash = 0, used = 0; uint32_t count = 0; std::vector<int8_t> n; };
    NormalCacheEntry ncache[4];
    uint64_t nuse = 0;
    uint32_t nhits = 0, nmiss_run = 0, nskip = 0, npar = 0;
    bool ncaching = true;
    fc_frame frame{};
    uint64_t seq = 0;
    uint32_t screen_w = 320, screen_h = 240, discarded = 0, unsupported = 0;
};
static void derive_cluster(fc_builder *b, fc_derive_scratch &ds, fc_weld_scratch &w, const uint32_t *segs, size_t nsegs,
                           uint32_t part, uint32_t nparts, float cos_crease, float cos_half);
constexpr uint8_t kNoFace = 0xFF;
static_assert(kWorkers < kNoFace, "a part id fits a byte, below kNoFace");
static inline uint32_t weld_part(uint64_t k, uint32_t nparts) {
    return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 56) % nparts;
}
static inline bool face_normal(const fc_builder *b, fc_derive_scratch &ds, uint32_t t) {
    const float *p0 = b->verts[t].pos, *p1 = b->verts[t + 1].pos, *p2 = b->verts[t + 2].pos;
    const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    float *n = &ds.face_n[t], *u = &ds.face_u[t];
    n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    const float l2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    if (!std::isfinite(l2) || !(l2 > 1e-12f)) { n[0] = n[1] = n[2] = 0.f; return false; }
    const float il = 1.f / std::sqrt(l2);
    for (int k = 0; k < 3; k++) u[k] = n[k] * il;
    return true;
}
struct derive_job {
    fc_builder *b;
    fc_derive_scratch *ds;
    float cos_crease, cos_half;
    uint32_t nfaces, count;
    std::atomic<uint32_t> next{0}, faces_done{0};
};
#if !defined(__APPLE__)
// dispatch_apply_f's contract off Apple (the Quest, the desktop build): fn(ctx, i) for every i < n, concurrently, and
// back once all have returned. The caller runs i = 0; kWorkers - 1 persistent threads (started at the first call, never
// joined: they sleep between jobs and live as long as the process) take the rest. One job at a time (a mutex), which
// derive_set's single caller (the game thread) never contends.
static void parallel_apply(size_t n, void *ctx, void (*fn)(void *, size_t)) {
    struct Pool {
        std::mutex job_m, m;
        std::condition_variable cv, done_cv;
        uint64_t gen = 0;
        size_t n = 0, done = 0;
        void *ctx = nullptr;
        void (*fn)(void *, size_t) = nullptr;
        Pool() {
            for (uint32_t w = 1; w < kWorkers; w++)
                std::thread([this, w] {
                    uint64_t seen = 0;
                    for (;;) {
                        std::unique_lock<std::mutex> l(m);
                        cv.wait(l, [&] { return gen != seen; });
                        seen = gen;
                        if (w >= n) continue;
                        void *c = ctx; void (*f)(void *, size_t) = fn;
                        l.unlock();
                        f(c, w);
                        l.lock();
                        if (++done == n - 1) done_cv.notify_one();
                    }
                }).detach();
        }
    };
    static Pool &pool = *new Pool;   // never destroyed: its threads outlive static destruction
    if (n <= 1) { if (n == 1) fn(ctx, 0); return; }
    n = std::min<size_t>(n, kWorkers);
    std::lock_guard<std::mutex> job(pool.job_m);
    {
        std::lock_guard<std::mutex> l(pool.m);
        pool.n = n; pool.done = 0; pool.ctx = ctx; pool.fn = fn; pool.gen++;
    }
    pool.cv.notify_all();
    fn(ctx, 0);
    std::unique_lock<std::mutex> l(pool.m);
    pool.done_cv.wait(l, [&] { return pool.done == n - 1; });
}
#endif
static void derive_worker(void *ctx, size_t wi) {
    derive_job &j = *static_cast<derive_job *>(ctx);
    fc_builder *b = j.b;
    fc_derive_scratch &ds = *j.ds;
    for (uint32_t i; (i = j.next.fetch_add(1, std::memory_order_relaxed)) < j.count;) {
        if (i < j.nfaces) {
            const fc_derive_scratch::Segment &sg = ds.sseg[ds.fseg[i]];
            for (uint32_t t = sg.first; t < sg.first + sg.count; t += 3) {
                const bool ok = face_normal(b, ds, t);
                for (uint32_t v = t; v < t + 3; v++) ds.vpart[v] = ok ? (uint8_t)weld_part(ds.vkey[v], kWorkers) : kNoFace;
            }
            j.faces_done.fetch_add(1, std::memory_order_release);
            continue;
        }
        const fc_derive_scratch::Item &it = ds.items[i - j.nfaces];
        if (it.nparts > 1)
            while (j.faces_done.load(std::memory_order_acquire) < j.nfaces) std::this_thread::yield();
        const uint32_t c = it.cluster;
        derive_cluster(b, ds, ds.weld[wi], &ds.cseg[ds.cstart[c]], ds.cstart[c + 1] - ds.cstart[c], it.part, it.nparts,
                       j.cos_crease, j.cos_half);
    }
}
// One welded-key component. lround on a NaN, an infinity or a value past int64 is undefined, so those take cell 0 (a face
// with a non-finite corner is skipped by derive_cluster, so the cell only has to be harmless); finite values far outside
// the 21-bit window are clamped before rounding and wrap exactly as before.
static inline uint32_t weld_cell(float v) {
    if (!(std::fabs(v) < 1e18f)) return (uint32_t)(1 << 20) & 0x1FFFFF;
    return (uint32_t)((uint64_t)((int64_t)std::lround(v) + (1 << 20)) & 0x1FFFFF);
}
template <class Sel>
static void derive_set(fc_builder *b, fc_derive_scratch &ds, const Sel &selected, bool stat, float cos_crease, float q) {
    using KeyBox = fc_derive_scratch::KeyBox;
    const float cos_half = std::sqrt(std::max(0.f, (1.f + cos_crease) * 0.5f));
    ds.sseg.clear(); ds.sbox.clear();
    uint32_t total = 0;
    for (const fc_draw &d : b->world) {
        if (!selected(d, stat)) continue;
        assert(d.first_vertex % 3 == 0);              // every draw adds whole triangles: a face is known by its first vertex
        const uint32_t end = d.first_vertex + d.vertex_count / 3 * 3;
        KeyBox box{};
        for (uint32_t t = d.first_vertex; t < end; t += 3) {
            KeyBox tb{{0x1FFFFF, 0x1FFFFF, 0x1FFFFF}, {0, 0, 0}};
            for (uint32_t v = t; v < t + 3; v++) {
                const float *p = b->verts[v].pos;
                uint64_t k = 0;
                for (int a = 0; a < 3; a++) {
                    const uint32_t c = weld_cell(p[a] * q);
                    tb.lo[a] = std::min(tb.lo[a], c); tb.hi[a] = std::max(tb.hi[a], c);
                    k = (k << 21) | c;
                }
                ds.vkey[v] = k;
            }
            // The run continues while the triangle touches its box grown by the box's own size (plus a little); the cut only
            // decides how finely the work splits, never the result.
            bool near = t > d.first_vertex;
            for (int a = 0; near && a < 3; a++) {
                const int64_t grow = (int64_t)(box.hi[a] - box.lo[a]) + 4;
                near = (int64_t)tb.lo[a] <= (int64_t)box.hi[a] + grow && (int64_t)tb.hi[a] + grow >= (int64_t)box.lo[a];
            }
            if (near) {
                ds.sseg.back().count += 3;
                for (int a = 0; a < 3; a++) { box.lo[a] = std::min(box.lo[a], tb.lo[a]); box.hi[a] = std::max(box.hi[a], tb.hi[a]); }
            } else {
                if (t > d.first_vertex) ds.sbox.push_back(box);
                ds.sseg.push_back({t, 3});
                box = tb;
            }
        }
        if (end > d.first_vertex) ds.sbox.push_back(box);
        total += end - d.first_vertex;
    }
    const uint32_t ns = (uint32_t)ds.sseg.size();
    ds.sorder.resize(ns); ds.sroot.resize(ns);
    for (uint32_t i = 0; i < ns; i++) { ds.sorder[i] = i; ds.sroot[i] = i; }
    std::sort(ds.sorder.begin(), ds.sorder.end(), [&](uint32_t x, uint32_t y) { return ds.sbox[x].lo[0] < ds.sbox[y].lo[0]; });
    auto root = [&](uint32_t a) { while (ds.sroot[a] != a) a = ds.sroot[a] = ds.sroot[ds.sroot[a]]; return a; };
    for (uint32_t i = 0; i < ns; i++) {
        const KeyBox &x = ds.sbox[ds.sorder[i]];
        for (uint32_t j = i + 1; j < ns && ds.sbox[ds.sorder[j]].lo[0] <= x.hi[0]; j++) {
            const KeyBox &y = ds.sbox[ds.sorder[j]];
            if (y.lo[1] > x.hi[1] || x.lo[1] > y.hi[1] || y.lo[2] > x.hi[2] || x.lo[2] > y.hi[2]) continue;
            const uint32_t rx = root(ds.sorder[i]), ry = root(ds.sorder[j]);
            if (rx != ry) ds.sroot[std::max(rx, ry)] = std::min(rx, ry);
        }
    }
    ds.cstart.assign((size_t)ns + 1, 0);
    ds.cvcount.assign(ns, 0);
    for (uint32_t i = 0; i < ns; i++) {
        const uint32_t r = root(i);
        ds.cstart[r + 1]++; ds.cvcount[r] += ds.sseg[i].count;
    }
    for (uint32_t i = 0; i < ns; i++) ds.cstart[i + 1] += ds.cstart[i];
    ds.cseg.resize(ns);
    {
        std::vector<uint32_t> &fill = ds.sorder;
        fill.assign(ds.cstart.begin(), ds.cstart.end() - 1);
        for (uint32_t i = 0; i < ns; i++) ds.cseg[fill[root(i)]++] = i;
    }
    const bool par = total >= kParallelVerts;
    ds.items.clear(); ds.fseg.clear();
    for (uint32_t c = 0; c < ns; c++) {
        if (ds.cstart[c + 1] == ds.cstart[c]) continue;
        const uint32_t nparts = par && ds.cvcount[c] >= kSplitVerts ? kWorkers : 1;
        if (nparts > 1) ds.fseg.insert(ds.fseg.end(), ds.cseg.begin() + ds.cstart[c], ds.cseg.begin() + ds.cstart[c + 1]);
        for (uint32_t p = 0; p < nparts; p++) ds.items.push_back({c, p, nparts});
    }
    std::sort(ds.items.begin(), ds.items.end(), [&](const fc_derive_scratch::Item &x, const fc_derive_scratch::Item &y) {
        const uint32_t sx = ds.cvcount[x.cluster] / x.nparts, sy = ds.cvcount[y.cluster] / y.nparts;
        return sx != sy ? sx > sy : x.cluster != y.cluster ? x.cluster < y.cluster : x.part < y.part;
    });
    derive_job job;
    job.b = b; job.ds = &ds; job.cos_crease = cos_crease; job.cos_half = cos_half;
    job.nfaces = (uint32_t)ds.fseg.size(); job.count = job.nfaces + (uint32_t)ds.items.size();
    if (par && ds.items.size() > 1) {
        b->npar++;
#if defined(__APPLE__)
        dispatch_apply_f(std::min<size_t>(kWorkers, ds.items.size()), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                         &job, derive_worker);
#else
        parallel_apply(std::min<size_t>(kWorkers, ds.items.size()), &job, derive_worker);
#endif
        return;
    }
    derive_worker(&job, 0);
}
static void derive_cluster(fc_builder *b, fc_derive_scratch &ds, fc_weld_scratch &w, const uint32_t *segs, size_t nsegs,
                           uint32_t part, uint32_t nparts, float cos_crease, float cos_half) {
    w.ckey.clear(); w.cvert.clear();
    if (nparts == 1) {
        for (size_t si = 0; si < nsegs; si++) {
            const fc_derive_scratch::Segment &sg = ds.sseg[segs[si]];
            for (uint32_t t = sg.first; t < sg.first + sg.count; t += 3) {
                if (!face_normal(b, ds, t)) continue;
                for (uint32_t c = 0; c < 3; c++) { w.ckey.push_back(ds.vkey[t + c]); w.cvert.push_back(t + c); }
            }
        }
    } else {
        size_t n = 0;
        for (size_t si = 0; si < nsegs; si++) n += ds.sseg[segs[si]].count;
        w.ckey.resize(n); w.cvert.resize(n);
        uint64_t *const ckey = w.ckey.data();
        uint32_t *const cvert = w.cvert.data();
        const uint8_t *const vpart = ds.vpart.data();
        const uint64_t *const vkey = ds.vkey.data();
        n = 0;
        for (size_t si = 0; si < nsegs; si++) {
            const fc_derive_scratch::Segment &sg = ds.sseg[segs[si]];
            for (uint32_t v = sg.first; v < sg.first + sg.count; v++) {
                ckey[n] = vkey[v]; cvert[n] = v;
                n += vpart[v] == part;
            }
        }
        w.ckey.resize(n); w.cvert.resize(n);
    }
    const size_t nc = w.ckey.size();
    size_t cap = 64;
    while (cap < nc * 2) cap *= 2;
    if (w.hkey.size() < cap) { w.hkey.assign(cap, 0); w.hgroup.assign(cap, 0); w.hgen.assign(cap, 0); w.hstamp = 0; }
    if (++w.hstamp == 0) { std::fill(w.hgen.begin(), w.hgen.end(), 0u); w.hstamp = 1; }
    const size_t mask = cap - 1;
    uint32_t ng = 0;
    w.cgroup.resize(nc);
    for (size_t i = 0; i < nc; i++) {
        const uint64_t k = w.ckey[i];
        size_t slot = (size_t)((k * 0x9E3779B97F4A7C15ull) >> 20) & mask;
        while (w.hgen[slot] == w.hstamp && w.hkey[slot] != k) slot = (slot + 1) & mask;
        if (w.hgen[slot] != w.hstamp) { w.hgen[slot] = w.hstamp; w.hkey[slot] = k; w.hgroup[slot] = ng++; }
        w.cgroup[i] = w.hgroup[slot];
    }
    w.gstart.assign((size_t)ng + 1, 0);
    for (size_t i = 0; i < nc; i++) w.gstart[w.cgroup[i] + 1]++;
    for (uint32_t g = 0; g < ng; g++) w.gstart[g + 1] += w.gstart[g];
    w.gvert.resize(nc);
    {
        std::vector<uint32_t> &fill = w.groot;
        fill.assign(w.gstart.begin(), w.gstart.end() - 1);
        for (size_t i = 0; i < nc; i++) w.gvert[fill[w.cgroup[i]]++] = w.cvert[i];
    }
    uint32_t maxg = 0;
    for (uint32_t g = 0; g < ng; g++) maxg = std::max(maxg, w.gstart[g + 1] - w.gstart[g]);
    w.gface.resize(maxg); w.grep.resize(maxg); w.rface.resize(maxg); w.groot.resize(maxg); w.gsum.resize((size_t)maxg * 3);
    uint32_t *const gface = w.gface.data(), *const grep = w.grep.data(), *const rface = w.rface.data();
    uint32_t *const groot = w.groot.data();
    float *const gsum = w.gsum.data();
    const float *const face_u = ds.face_u.data(), *const face_n = ds.face_n.data();
    const uint32_t *const gvert = w.gvert.data();
    int8_t *const normals = b->normals.data();
    auto root = [groot](uint32_t a) { while (groot[a] != a) a = groot[a] = groot[groot[a]]; return a; };
    for (uint32_t g = 0; g < ng; g++) {
        const uint32_t i = w.gstart[g], j = w.gstart[g + 1];
        uint32_t nf = 0;
        for (uint32_t c = i; c < j; c++) {
            const uint32_t f = gvert[c] - gvert[c] % 3;
            if (nf == 0 || gface[nf - 1] != f) gface[nf++] = f;
        }
        uint32_t nr = 0;
        for (uint32_t f = 0; f < nf; f++) {
            const float *uf = &face_u[gface[f]];
            uint32_t r = 0;
            for (; r < nr; r++) {
                const float *ur = &face_u[rface[r]];
                if (uf[0] * ur[0] + uf[1] * ur[1] + uf[2] * ur[2] > 0.99999f) break;
            }
            if (r == nr) rface[nr++] = gface[f];
            grep[f] = r;
        }
        // Fast path (smooth terrain): planes all within half the crease of their mean direction are pairwise within the
        // crease, so they form one group. Otherwise group them pairwise (transitively).
        float m[3] = {0.f, 0.f, 0.f};
        for (uint32_t r = 0; r < nr; r++) for (int k = 0; k < 3; k++) m[k] += face_u[rface[r] + k];
        const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        bool one = ml > 0.f;
        for (uint32_t r = 0; one && r < nr; r++) {
            const float *ur = &face_u[rface[r]];
            one = ur[0] * m[0] + ur[1] * m[1] + ur[2] * m[2] >= cos_half * ml;
        }
        for (uint32_t r = 0; r < nr; r++) groot[r] = one ? 0u : r;
        for (uint32_t r = 0; !one && r < nr; r++) {
            const float *ua = &face_u[rface[r]];
            for (uint32_t c = r + 1; c < nr; c++) {
                const float *uc = &face_u[rface[c]];
                if (ua[0] * uc[0] + ua[1] * uc[1] + ua[2] * uc[2] < cos_crease) continue;
                const uint32_t ra = root(r), rc = root(c);
                if (ra != rc) groot[rc] = ra;
            }
        }
        std::fill(gsum, gsum + (size_t)nr * 3, 0.f);
        for (uint32_t f = 0; f < nf; f++) {
            const uint32_t r = root(grep[f]);
            for (int k = 0; k < 3; k++) gsum[r * 3 + k] += face_n[gface[f] + k];
        }
        for (uint32_t r = 0; r < nr; r++) {
            if (root(r) != r) continue;
            float *gs = &gsum[r * 3];
            const float l = std::sqrt(gs[0] * gs[0] + gs[1] * gs[1] + gs[2] * gs[2]);
            for (int k = 0; k < 3; k++) gs[k] = l > 0.f ? gs[k] / l : 0.f;
        }
        uint32_t fi = 0;
        for (uint32_t c = i; c < j; c++) {
            const uint32_t v = gvert[c];
            if (gface[fi] != v - v % 3) fi++;
            const float *gs = &gsum[root(grep[fi]) * 3];
            for (int k = 0; k < 3; k++) normals[(size_t)v * 4 + k] = (int8_t)std::lround(gs[k] * 127.f);
        }
    }
}
extern "C" {
fc_builder *fc_builder_create(void) { return new fc_builder(); }
void fc_builder_destroy(fc_builder *b) { delete b; }
void fc_builder_reset(fc_builder *b, uint64_t seq, uint32_t w, uint32_t h) {
    b->verts.clear(); b->world.clear(); b->overlay.clear();
    b->groups.clear(); b->points.clear(); b->lines.clear(); b->normals.clear();
    b->seq = seq; b->screen_w = w; b->screen_h = h; b->discarded = b->unsupported = 0;
    b->prev_on = false; b->frame.prev_pos = nullptr; b->frame.has_level = 0; b->frame.level = ar_level_info{};
    b->gview_on = false; b->frame.game_view = nullptr;
    b->focus_styles.clear(); b->frame.focus_styles = nullptr; b->frame.focus_style_count = 0;
    b->look_on = false; b->frame.look = nullptr;
}
void fc_builder_set_focus_styles(fc_builder *b, const fc_focus_style *s, uint32_t n) {
    if (s && n) b->focus_styles.assign(s, s + n); else b->focus_styles.clear();
}
void fc_builder_set_game_view(fc_builder *b, const fc_game_view *v) {
    b->gview_on = v != nullptr;
    b->gview = v ? *v : fc_game_view{};
}
void fc_builder_set_look(fc_builder *b, const fc_look *l) {
    b->look_on = l != nullptr;
    b->look = l ? *l : fc_look{};
}
uint32_t fc_builder_vertex_count(const fc_builder *b) { return (uint32_t)b->verts.size(); }
uint32_t fc_builder_prev_dropped(const fc_builder *b) { return b->prev_dropped; }
float *fc_builder_prev_positions(fc_builder *b) {
    const size_t n = b->verts.size();
    b->prev.resize(n * 3);
    for (size_t i = 0; i < n; i++) std::memcpy(&b->prev[i * 3], b->verts[i].pos, sizeof(float) * 3);
    b->prev_on = true;
    return b->prev.data();
}
void fc_builder_translate_world(fc_builder *b, const float d[3]) {
    const bool prev = b->prev_on && b->prev.size() == b->verts.size() * 3;
    for (const fc_draw &w : b->world) {
        for (uint32_t i = w.first_vertex; i < w.first_vertex + w.vertex_count && i < b->verts.size(); i++) {
            for (int k = 0; k < 3; k++) {
                b->verts[i].pos[k] += d[k];
                if (prev) b->prev[(size_t)i * 3 + k] += d[k];
            }
        }
    }
}
void fc_builder_set_level(fc_builder *b, const ar_level_info *level) {
    b->frame.has_level = level ? 1u : 0u;
    b->frame.level = level ? *level : ar_level_info{};
}
fc_vertex *fc_builder_add_draw(fc_builder *b, const fc_draw *proto, uint32_t n) {
    fc_draw d = *proto;
    d.first_vertex = (uint32_t)b->verts.size();
    d.vertex_count = n;
    (d.space == FC_SPACE_WORLD ? b->world : b->overlay).push_back(d);
    b->verts.resize(b->verts.size() + n);
    return b->verts.data() + d.first_vertex;
}
void fc_builder_count(fc_builder *b, uint32_t discarded, uint32_t unsupported) {
    b->discarded += discarded; b->unsupported += unsupported;
}
void fc_builder_set_group(fc_builder *b, const fc_overlay_group *g) {
    for (auto &x : b->groups) {
        if (x.id == g->id) { x = *g; return; }
    }
    b->groups.push_back(*g);
}
void fc_builder_add_polyline(fc_builder *b, const float *xyz, uint32_t n, const uint8_t rgba[4], float width_m) {
    if (!xyz || !rgba || n < 2) return;
    if (n > UINT32_MAX - b->points.size() / 3) return;
    fc_polyline l{};
    l.first_point = (uint32_t)(b->points.size() / 3);
    l.point_count = n;
    for (int k = 0; k < 4; k++) l.rgba[k] = rgba[k];
    l.width_m = width_m;
    b->points.insert(b->points.end(), xyz, xyz + (size_t)n * 3);
    b->lines.push_back(l);
}
uint32_t fc_builder_normal_cache_hits(const fc_builder *b) { return b->nhits; }
uint32_t fc_builder_normal_parallel_runs(const fc_builder *b) { return b->npar; }
void fc_builder_derive_normals(fc_builder *b, uint8_t mask, float crease_deg, float weld) {
    fc_builder_derive_normals_keyed(b, mask, crease_deg, weld, 0);
}
void fc_builder_derive_normals_keyed(fc_builder *b, uint8_t mask, float crease_deg, float weld, uint64_t static_key) {
    const size_t nv = b->verts.size();
    b->normals.assign(nv * 4, 0);
    static thread_local fc_derive_scratch ds;
    // Written before read: a cluster reads only its own non-degenerate faces' entries and its corners' keys (and a split
    // cluster its corners' parts, written by its face jobs first).
    if (ds.face_n.size() < nv) { ds.face_n.resize(nv); ds.face_u.resize(nv); ds.vkey.resize(nv); ds.vpart.resize(nv); }
    const float q = weld > 0.f ? 1.f / weld : 1.f;
    const float cos_crease = std::cos(crease_deg * 3.14159265f / 180.f);
    auto selected = [&](const fc_draw &d, bool stat) {
        return (d.light_flags & mask) != 0 && ((d.light_flags & FC_LF_STATIC) != 0) == stat && d.first_vertex + d.vertex_count <= nv;
    };
    // The static set: unchanged geometry (positions and each draw's count and flags, in draw order; never the vertex
    // offsets, which dynamic draws shift) takes its normals from the cache. Static geometry that keeps moving misses every
    // frame: after kGiveUp misses in a row the hash is skipped, and probed again every kRetry frames.
    constexpr uint32_t kGiveUp = 8, kRetry = 256;
    const bool probe = b->ncaching || ++b->nskip >= kRetry;
    uint64_t h = 0x243F6A8885A308D3ull;
    uint32_t count = 0;
    auto mix = [&h](uint32_t w) { h ^= w; h *= 0x9E3779B97F4A7C15ull; h ^= h >> 29; };
    if (probe) {
        b->nskip = 0;
        uint32_t bits;
        mix(mask); std::memcpy(&bits, &crease_deg, 4); mix(bits); std::memcpy(&bits, &weld, 4); mix(bits);
        if (static_key) { mix(0x4B455944u); mix((uint32_t)static_key); mix((uint32_t)(static_key >> 32)); }
        for (const fc_draw &d : b->world) {
            if (!selected(d, true)) continue;
            mix(d.vertex_count); mix(d.light_flags);
            if (!static_key)
                for (uint32_t v = d.first_vertex; v < d.first_vertex + d.vertex_count; v++)
                    for (int k = 0; k < 3; k++) { std::memcpy(&bits, &b->verts[v].pos[k], 4); mix(bits); }
            count += d.vertex_count;
        }
    }
    fc_builder::NormalCacheEntry *hit = nullptr;
    if (probe)
        for (auto &e : b->ncache) if (e.used != 0 && e.hash == h && e.count == count) hit = &e;
    if (hit) {
        size_t i = 0;
        for (const fc_draw &d : b->world) {
            if (!selected(d, true)) continue;
            for (uint32_t v = d.first_vertex; v < d.first_vertex + d.vertex_count; v++, i += 3)
                for (int k = 0; k < 3; k++) b->normals[(size_t)v * 4 + k] = hit->n[i + k];
        }
        hit->used = ++b->nuse;
        b->nhits++; b->nmiss_run = 0; b->ncaching = true;
    } else {
        derive_set(b, ds, selected, true, cos_crease, q);
        if (probe) {
            fc_builder::NormalCacheEntry *e = &b->ncache[0];
            for (auto &c : b->ncache) if (c.used < e->used) e = &c;
            e->hash = h; e->count = count; e->used = ++b->nuse;
            e->n.resize((size_t)count * 3);
            size_t i = 0;
            for (const fc_draw &d : b->world) {
                if (!selected(d, true)) continue;
                for (uint32_t v = d.first_vertex; v < d.first_vertex + d.vertex_count; v++, i += 3)
                    for (int k = 0; k < 3; k++) e->n[i + k] = b->normals[(size_t)v * 4 + k];
            }
            if (++b->nmiss_run >= kGiveUp) b->ncaching = false;
        }
    }
    derive_set(b, ds, selected, false, cos_crease, q);
}
const fc_frame *fc_builder_finish(fc_builder *b) {
    fc_frame &f = b->frame;
    f.magic = FC_MAGIC; f.abi_version = FC_ABI_VERSION; f.seq = b->seq;
    f.screen_w = b->screen_w; f.screen_h = b->screen_h;
    f.verts = b->verts.data(); f.vert_count = (uint32_t)b->verts.size();
    f.world = b->world.data(); f.world_count = (uint32_t)b->world.size();
    f.overlay = b->overlay.data(); f.overlay_count = (uint32_t)b->overlay.size();
    f.discarded_draws = b->discarded; f.unsupported_draws = b->unsupported;
    f.groups = b->groups.data(); f.group_count = (uint32_t)b->groups.size();
    f.line_points = b->points.data(); f.line_point_count = (uint32_t)(b->points.size() / 3);
    f.lines = b->lines.data(); f.line_count = (uint32_t)b->lines.size();
    f.normals = b->normals.size() == b->verts.size() * 4 && !b->normals.empty() ? b->normals.data() : nullptr;
    // publish prev_pos only when it covers every vertex (an add_draw after fc_builder_prev_positions would leave it short,
// and the renderer reads 3 floats per vertex); otherwise drop it for this frame and count it
    const bool prev_ok = b->prev.size() == b->verts.size() * 3 && !b->prev.empty();
    if (b->prev_on && !prev_ok) b->prev_dropped++;
    f.prev_pos = b->prev_on && prev_ok ? b->prev.data() : nullptr;
    f.game_view = b->gview_on ? &b->gview : nullptr;
    f.focus_styles = b->focus_styles.empty() ? nullptr : b->focus_styles.data();
    f.focus_style_count = (uint32_t)b->focus_styles.size();
    f.look = b->look_on ? &b->look : nullptr;
    return &f;
}
}
