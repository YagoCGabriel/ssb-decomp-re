/*
 * ssb-decomp PC Port - CPU software rasterizer (reference renderer).
 * Mirrors the fixed-function RDP pipeline closely enough for pixel-exact
 * tests and as a spec for the D3D10/D3D11/Vulkan GPU backends.
 */
#include "pc_sw_raster.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void pcswInit(PcSwRaster *r, uint8_t *fb, float *zbuf, int w, int h)
{
    r->fb = fb;
    r->zbuf = zbuf;
    r->width = w;
    r->height = h;
}

void pcswClear(PcSwRaster *r, uint32_t rgba, float depth)
{
    int x, y;
    for (y = 0; y < r->height; y++) {
        for (x = 0; x < r->width; x++) {
            uint8_t *p = r->fb + (size_t)(y * r->width + x) * 4;
            p[0] = (uint8_t)(rgba >> 24);
            p[1] = (uint8_t)(rgba >> 16);
            p[2] = (uint8_t)(rgba >> 8);
            p[3] = (uint8_t)(rgba);
            if (r->zbuf) r->zbuf[y * r->width + x] = depth;
        }
    }
}

void pcswFillRect(PcSwRaster *r, float x0, float y0, float x1, float y1, uint32_t rgba)
{
    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    int x, y;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > r->width - 1) ix1 = r->width - 1;
    if (iy1 > r->height - 1) iy1 = r->height - 1;
    for (y = iy0; y <= iy1; y++) {
        for (x = ix0; x <= ix1; x++) {
            uint8_t *p = r->fb + (size_t)(y * r->width + x) * 4;
            p[0] = (uint8_t)(rgba >> 24);
            p[1] = (uint8_t)(rgba >> 16);
            p[2] = (uint8_t)(rgba >> 8);
            p[3] = (uint8_t)(rgba);
        }
    }
}

/* ---- N64 texture decoding ------------------------------------------------ */

static uint32_t cvt555to8888(uint32_t c)
{
    uint32_t rr = (c >> 11) & 0x1F, gg = (c >> 6) & 0x1F, bb = (c >> 1) & 0x1F;
    uint32_t aa = (c & 1) ? 255 : 0;
    rr = (rr << 3) | (rr >> 2);
    gg = (gg << 3) | (gg >> 2);
    bb = (bb << 3) | (bb >> 2);
    return (rr << 24) | (gg << 16) | (bb << 8) | aa;
}

/* Read a 16-bit TLUT entry (RGBA5551 / IA16 stored big-endian). */
static uint32_t tlut_entry(const uint8_t *tlut, uint32_t idx)
{
    uint32_t c = ((uint32_t)tlut[idx * 2] << 8) | tlut[idx * 2 + 1];
    return cvt555to8888(c);
}

uint8_t *pcswDecodeTexture(const uint8_t *rdram, size_t rdram_size,
                           const PcTexInfo *tex, uint32_t *out_w, uint32_t *out_h)
{
    uint32_t w = tex->width ? tex->width : 1;
    uint32_t h = tex->height ? tex->height : 1;
    uint8_t *rgba;
    uint32_t x, y;
    uintptr_t off;
    const uint8_t *src;
    const uint8_t *tlut = NULL;

    if (!rdram || !tex->addr) {
        return NULL;
    }
    rgba = (uint8_t *)calloc((size_t)w * h * 4, 1);
    if (!rgba) return NULL;
    *out_w = w; *out_h = h;

    src = rdram + (tex->addr % rdram_size);
    if (tex->tlut && tex->format == G_IM_FMT_CI) {
        tlut = rdram + (tex->tlut % rdram_size);
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c = 0;
            switch (tex->size) {
            case G_IM_SIZ_32b: {
                uint32_t v;
                off = ((y * w + x) * 4) % (rdram_size - 4);
                v = ((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) |
                    ((uint32_t)src[off+2] << 8) | src[off+3];
                if (tex->format == G_IM_FMT_RGBA) {
                    c = v;
                } else { /* IA / I */
                    uint32_t i = tex->format == G_IM_FMT_IA ? v >> 8 : v >> 8;
                    uint32_t a = tex->format == G_IM_FMT_IA ? (v & 0xFF) : 255;
                    c = (i << 24) | (i << 16) | (i << 8) | a;
                }
                break;
            }
            case G_IM_SIZ_16b: {
                uint16_t v;
                off = ((y * w + x) * 2) % (rdram_size - 2);
                v = (uint16_t)((src[off] << 8) | src[off+1]);
                if (tex->format == G_IM_FMT_RGBA) {
                    c = cvt555to8888(v);
                } else if (tex->format == G_IM_FMT_IA) {
                    uint32_t i = (v >> 8) & 0xFF, a = (v & 0xFF) ? 255 : 0;
                    i = (i << 1) | (i >> 7);
                    c = (i << 24) | (i << 16) | (i << 8) | a;
                } else { /* CI16 index stored in low bits of 16b tile */
                    uint32_t idx = v & 0x3FF;
                    c = tlut ? tlut_entry(tlut, idx) : 0xFFFFFFFFu;
                }
                break;
            }
            case G_IM_SIZ_8b: {
                off = (y * w + x) % rdram_size;
                if (tex->format == G_IM_FMT_CI) {
                    uint32_t idx = src[off];
                    c = tlut ? tlut_entry(tlut, idx) : 0xFFFFFFFFu;
                } else { /* I8 / IA8-ish */
                    uint32_t i = src[off];
                    c = (i << 24) | (i << 16) | (i << 8) | 0xFF;
                }
                break;
            }
            case G_IM_SIZ_4b: {
                uint32_t byte = (y * w + x) / 2;
                uint32_t nib = ((y * w + x) & 1) ? (byte < rdram_size ? src[byte % rdram_size] & 0xF : 0)
                                                 : (byte < rdram_size ? src[byte % rdram_size] >> 4 : 0);
                if (tex->format == G_IM_FMT_CI) {
                    c = tlut ? tlut_entry(tlut, nib) : 0xFFFFFFFFu;
                } else {
                    uint32_t i = nib * 17;
                    c = (i << 24) | (i << 16) | (i << 8) | 0xFF;
                }
                break;
            }
            default:
                c = 0xFF00FF00;
                break;
            }
            {
                uint8_t *p = rgba + ((size_t)(y * w + x)) * 4;
                p[0] = (uint8_t)(c >> 24);
                p[1] = (uint8_t)(c >> 16);
                p[2] = (uint8_t)(c >> 8);
                p[3] = (uint8_t)(c);
            }
        }
    }
    return rgba;
}

/* ---- triangle rasterization --------------------------------------------- */

void pcswDrawTri(PcSwRaster *r, const PcVertex *v0, const PcVertex *v1,
                 const PcVertex *v2, const PcDrawState *st, const float vp[4])
{
    float sx[3], sy[3], sz[3];
    int i, x, y;
    float minx, maxx, miny, maxy;
    float area;
    int ix0, ix1, iy0, iy1;

    /* pos[] arrives in N64 framebuffer pixel space (ortho projection is an
     * identity transform there); vp maps fb-space to render-target pixels. */
    for (i = 0; i < 3; i++) {
        const PcVertex *v = (i == 0) ? v0 : (i == 1) ? v1 : v2;
        sx[i] = v->pos[0] * vp[0] + vp[1];
        sy[i] = v->pos[1] * vp[2] + vp[3];
        sz[i] = (v->pos[2] * 0.5f + 0.5f);
        if (sz[i] < 0.f) sz[i] = 0.f;
        if (sz[i] > 1.f) sz[i] = 1.f;
    }

    /* cull by signed area (back/front) */
    area = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sy[1]-sy[0])*(sx[2]-sx[0]);
    if (area == 0.0f) return;
    if (st->cull_face == 2 && area < 0.0f) return; /* back */
    if (st->cull_face == 1 && area > 0.0f) return; /* front */

    minx = fminf(fminf(sx[0], sx[1]), sx[2]);
    maxx = fmaxf(fmaxf(sx[0], sx[1]), sx[2]);
    miny = fminf(fminf(sy[0], sy[1]), sy[2]);
    maxy = fmaxf(fmaxf(sy[0], sy[1]), sy[2]);

    ix0 = (int)fmaxf(floorf(minx), 0.0f);
    ix1 = (int)fminf(ceilf(maxx), (float)(r->width - 1));
    iy0 = (int)fmaxf(floorf(miny), 0.0f);
    iy1 = (int)fminf(ceilf(maxy), (float)(r->height - 1));
    /* scissor */
    if (st->scissor[2] > st->scissor[0]) {
        ix0 = (int)fmaxf((float)ix0, floorf(st->scissor[0]));
        iy0 = (int)fmaxf((float)iy0, floorf(st->scissor[1]));
        ix1 = (int)fminf((float)ix1, ceilf(st->scissor[2]) - 1);
        iy1 = (int)fminf((float)iy1, ceilf(st->scissor[3]) - 1);
    }

    for (y = iy0; y <= iy1; y++) {
        for (x = ix0; x <= ix1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0, w1, w2, z;
            uint32_t color;
            const float *c0, *c1, *c2;
            float cr, cg, cb, ca;
            float denom = (sy[1]-sy[2])*(sx[0]-sx[2]) + (sx[2]-sx[1])*(sy[0]-sy[2]);
            uint8_t *p;

            if (denom == 0.0f) continue;
            w0 = ((sy[1]-sy[2])*(px-sx[2]) + (sx[2]-sx[1])*(py-sy[2])) / denom;
            w1 = ((sy[2]-sy[0])*(px-sx[2]) + (sx[0]-sx[2])*(py-sy[2])) / denom;
            w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            z = w0*sz[0] + w1*sz[1] + w2*sz[2];
            if (st->depth_test && r->zbuf && z > r->zbuf[y*r->width+x]) continue;
            if (st->depth_write && r->zbuf) r->zbuf[y*r->width+x] = z;

            c0 = st->lighting ? v0->light : v0->color;
            c1 = st->lighting ? v1->light : v1->color;
            c2 = st->lighting ? v2->light : v2->color;
            cr = w0*c0[0] + w1*c1[0] + w2*c2[0];
            cg = w0*c0[1] + w1*c1[1] + w2*c2[1];
            cb = w0*c0[2] + w1*c1[2] + w2*c2[2];
            ca = w0*c0[3] + w1*c1[3] + w2*c2[3];
#define CLAMP01(v) do { if ((v) < 0.0f) (v) = 0.0f; else if ((v) > 1.0f) (v) = 1.0f; } while (0)
            CLAMP01(cr);
            CLAMP01(cg);
            CLAMP01(cb);
            CLAMP01(ca);
#undef CLAMP01
            color = ((uint32_t)(cr*255) << 24) | ((uint32_t)(cg*255) << 16) |
                    ((uint32_t)(cb*255) << 8) | (uint32_t)(ca*255);

            p = r->fb + (size_t)(y*r->width + x) * 4;
            p[0] = (uint8_t)(color >> 24);
            p[1] = (uint8_t)(color >> 16);
            p[2] = (uint8_t)(color >> 8);
            p[3] = (uint8_t)(color);
        }
    }
}
