#include "cube_adapter.h"
#include "ar_snapshot_ring.h"
#include <string.h>
static ar_snapshot_ring *sRing;
static ar_texture_fifo *sFifo;
static ar_player_input sLast;
static uint32_t sTicks;
static float sPos[2];
static int sPublish = 1;
static const float C[8][3] = {{-1,0,-1},{1,0,-1},{1,2,-1},{-1,2,-1},{-1,0,1},{1,0,1},{1,2,1},{-1,2,1}};
static const int F[6][4] = {{4,5,6,7},{1,0,3,2},{5,1,2,6},{0,4,7,3},{7,6,2,3},{0,1,5,4}};
static const uint8_t COL[6][4] = {{220,60,60,255},{60,220,60,255},{60,60,220,255},{220,220,60,255},{240,240,240,255},{90,90,90,255}};
static int c_init(const char *rom, const char *save) {
    (void)rom; (void)save;
    if (!sRing) { sRing = ar_ring_create(); sFifo = ar_texfifo_create(); }
    sTicks = 0; sPos[0] = sPos[1] = 0.f; memset(&sLast, 0, sizeof sLast);
    return 1;
}
static const char *c_err(void) { return ""; }
static void c_tick(const ar_player_input *p, uint32_t n) {
    if (n) sLast = p[0];
    sTicks++;
    if (n && p[0].pad_connected) {
        sPos[0] += p[0].pad.lx * CUBE_SPEED_UNITS;
        sPos[1] -= p[0].pad.ly * CUBE_SPEED_UNITS;
    }
    if (sTicks == 1) { const uint8_t px[4] = {255, 255, 255, 255}; ar_texfifo_push(sFifo, sTicks, 1, 1, 1, px); }
    if (!sPublish) return;
    fc_builder *b = ar_ring_begin_write(sRing);
    if (!b) return;
    fc_builder_reset(b, sTicks, 320, 240);
    fc_draw d; memset(&d, 0, sizeof d);
    d.space = FC_SPACE_WORLD; d.render_flags = FC_RS_DEPTH_TEST | FC_RS_DEPTH_WRITE;
    d.light_flags = FC_LF_STATIC | FC_LF_CASTER;   /* static scenery: casts, and the AO bake and underside map see it */
    d.cc.slots = ((uint64_t)FC_SRC_SHADE << 12) | ((uint64_t)FC_SRC_SHADE << 28);
    fc_vertex *v = fc_builder_add_draw(b, &d, 36);
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int f = 0; f < 6; f++)
        for (int i = 0; i < 6; i++) {
            fc_vertex *o = &v[f * 6 + i]; memset(o, 0, sizeof *o);
            o->pos[0] = C[F[f][idx[i]]][0] * 1600.f + sPos[0];
            o->pos[1] = C[F[f][idx[i]]][1] * 1600.f;
            o->pos[2] = C[F[f][idx[i]]][2] * 1600.f + sPos[1];
            memcpy(o->shade, COL[f], 4);
        }
    ar_ring_publish(sRing);
}
static const fc_frame *c_acquire(void) { return ar_ring_acquire_latest(sRing); }
static void c_release(const fc_frame *f) { ar_ring_release(sRing, f); }
static uint32_t c_drain(uint64_t up_to, ar_texture_upload_fn fn, void *user) { return ar_texfifo_drain(sFifo, up_to, fn, user); }
static ar_level_info c_level(void) { ar_level_info l = { 8000.f, 0.f, {-1600.f, 0.f, -1600.f}, {1600.f, 3200.f, 1600.f} }; return l; }
static uint32_t c_audio(ar_audio_pull *p) { (void)p; return 0; }
static uint64_t c_hash(void) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char *b = (const unsigned char *)&sTicks;
    for (size_t i = 0; i < sizeof sTicks; i++) { h ^= b[i]; h *= 1099511628211ull; }
    b = (const unsigned char *)sPos;
    for (size_t i = 0; i < sizeof sPos; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static const ar_adapter sCube = { AR_ADAPTER_ABI_VERSION, "cube", c_init, c_err, c_tick, c_acquire, c_release, c_drain, c_level, c_audio, c_hash };
const ar_adapter *cube_adapter(void) { return &sCube; }
ar_player_input cube_last_input(void) { return sLast; }
uint32_t cube_ticks(void) { return sTicks; }
void cube_set_publish(int on) { sPublish = on; }
void cube_position(float out[2]) { out[0] = sPos[0]; out[1] = sPos[1]; }
