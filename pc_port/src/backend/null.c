/*
 * ssb-decomp PC Port - headless ("null") backend.
 *
 * Implements PcBackend on top of the CPU software rasterizer so display-list
 * translation can be validated pixel-for-pixel without any GPU API. This is
 * the reference target used by the test suite and CI, and it also serves as
 * the fallback renderer when neither Vulkan nor Direct3D are available.
 */
#include "pc_port_common.h"
#include "pc_sw_raster.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct NullBackend {
    PcBackend base;
    PcSwRaster raster;
    uint8_t *fb;
    float *zbuf;
    const uint8_t *rdram;
    size_t rdram_size;
    float viewport[4];
    /* stats for tests */
    unsigned int draws, fills, clears, tex_uploads;
} NullBackend;

static NullBackend g_null;

static int nullInit(PcBackend *self, int width, int height, const char *title)
{
    NullBackend *nb = (NullBackend *)self;
    (void)title;
    free(nb->fb);
    free(nb->zbuf);
    nb->fb = (uint8_t *)calloc((size_t)width * height * 4, 1);
    nb->zbuf = (float *)calloc((size_t)width * height, sizeof(float));
    if (!nb->fb || !nb->zbuf) {
        return 0;
    }
    pcswInit(&nb->raster, nb->fb, nb->zbuf, width, height);
    nb->viewport[0] = 1.0f;  /* fb-space -> rt identity at native res */
    nb->viewport[1] = 0.0f;
    nb->viewport[2] = 1.0f;
    nb->viewport[3] = 0.0f;
    pclog("null: initialized %dx%d (%s)\n", width, height, self->name);
    return 1;
}

static void nullShutdown(PcBackend *self)
{
    NullBackend *nb = (NullBackend *)self;
    free(nb->fb); nb->fb = NULL;
    free(nb->zbuf); nb->zbuf = NULL;
}

static void nullBeginFrame(PcBackend *self) { (void)self; }
static void nullEndFrame(PcBackend *self) { (void)self; }

static void nullBindRdram(PcBackend *self, const uint8_t *rdram, size_t size)
{
    NullBackend *nb = (NullBackend *)self;
    nb->rdram = rdram;
    nb->rdram_size = size;
}

static void nullSetViewport(PcBackend *self, float lx, float ly, float sx, float sy)
{
    NullBackend *nb = (NullBackend *)self;
    /* map the N64 fb-space box (lx..lx+sx, ly..ly+sy) onto the current
     * render target size */
    float rw = (float)nb->raster.width, rh = (float)nb->raster.height;
    nb->viewport[0] = sx > 0.0f ? rw / sx : 1.0f;   /* x scale rt/native */
    nb->viewport[1] = lx * (rw / (sx > 0.0f ? sx : 1.0f));
    nb->viewport[2] = sy > 0.0f ? rh / sy : 1.0f;
    nb->viewport[3] = ly * (rh / (sy > 0.0f ? sy : 1.0f));
}

static void nullNoop(void) {}
static void nullSetScissorFn(PcBackend *s, float l, float t, float r, float b) { (void)s;(void)l;(void)t;(void)r;(void)b; }
static void nullSetOthermode(PcBackend *s, uint64_t h, uint64_t l) { (void)s;(void)h;(void)l; }
static void nullSetGeomMode(PcBackend *s, uint32_t m) { (void)s;(void)m; }
static void nullSetCombine(PcBackend *s, const uint32_t a[4], const uint32_t b[4],
                           const uint32_t c[4], const uint32_t d[4])
{ (void)s;(void)a;(void)b;(void)c;(void)d; }
static void nullSetTexture(PcBackend *s, const PcTexInfo *t)
{
    NullBackend *nb = (NullBackend *)s;
    (void)t;
    nb->tex_uploads++;
}
static void nullSetPrimColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void nullSetEnvColor(PcBackend *s, uint32_t c) { (void)s;(void)c; }
static void nullSetMatrices(PcBackend *s, const float m[16], const float p[16]) { (void)s;(void)m;(void)p; }

static void nullDrawTri(PcBackend *self, const PcVertex *v0, const PcVertex *v1,
                        const PcVertex *v2, const PcDrawState *st)
{
    NullBackend *nb = (NullBackend *)self;
    nb->draws++;
    pcswDrawTri(&nb->raster, v0, v1, v2, st, nb->viewport);
}

static void nullFillRect(PcBackend *self, float x0, float y0, float x1, float y1,
                         uint32_t color, const PcDrawState *st)
{
    NullBackend *nb = (NullBackend *)self;
    (void)st;
    nb->fills++;
    pcswFillRect(&nb->raster, x0, y0, x1, y1, color);
}

static void nullClear(PcBackend *self, uint32_t color, float depth)
{
    NullBackend *nb = (NullBackend *)self;
    nb->clears++;
    pcswClear(&nb->raster, color, depth);
}

PcBackend *backendCreateNull(void)
{
    static PcBackend iface;
    memset(&g_null, 0, sizeof(g_null));
    iface.name = "Headless SW Raster";
    iface.init = nullInit;
    iface.shutdown = nullShutdown;
    iface.begin_frame = nullBeginFrame;
    iface.end_frame = nullEndFrame;
    iface.bind_rdram = nullBindRdram;
    iface.set_viewport = nullSetViewport;
    iface.set_scissor = nullSetScissorFn;
    iface.set_othermode = nullSetOthermode;
    iface.set_geom_mode = nullSetGeomMode;
    iface.set_combine = nullSetCombine;
    iface.set_texture = nullSetTexture;
    iface.set_prim_color = nullSetPrimColor;
    iface.set_env_color = nullSetEnvColor;
    iface.set_matrices = nullSetMatrices;
    iface.draw_triangles = nullDrawTri;
    iface.fill_rect = nullFillRect;
    iface.clear = nullClear;
    g_null.base = iface;
    (void)nullNoop;
    return &g_null.base;
}

/* Accessors for the test harness */
const uint8_t *nullFramebuffer(void) { return g_null.fb; }
int nullWidth(void) { return g_null.raster.width; }
int nullHeight(void) { return g_null.raster.height; }
unsigned nullDrawCount(void) { return g_null.draws; }
unsigned nullFillCount(void) { return g_null.fills; }
unsigned nullClearCount(void) { return g_null.clears; }

/* Write framebuffer as binary RGBA (for golden-image diffs) */
int nullDumpFramebuffer(const char *path)
{
    FILE *f;
    if (!g_null.fb) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(&g_null.raster.width, 4, 1, f);
    fwrite(&g_null.raster.height, 4, 1, f);
    fwrite(g_null.fb, 1, (size_t)g_null.raster.width * g_null.raster.height * 4, f);
    fclose(f);
    return 1;
}
