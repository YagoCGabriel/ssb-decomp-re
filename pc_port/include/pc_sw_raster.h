#ifndef PC_SW_RASTER_H
#define PC_SW_RASTER_H

/*
 * ssb-decomp PC Port - CPU software rasterizer.
 *
 * The null/headless backend uses this to actually *rasterize* the translated
 * display lists into an RGBA8 framebuffer. This gives us:
 *   - a CI-verifiable renderer (pixel assertions in tests)
 *   - a reference implementation for the GPU backends (D3D10/D3D11/Vulkan)
 *     which run the same state model on real hardware.
 */

#include <stdint.h>
#include <stddef.h>
#include "gbi_interpreter.h"

typedef struct PcSwRaster {
    uint8_t *fb;        /* RGBA8, width*height*4 */
    float *zbuf;        /* depth buffer, 0..1, cleared to 1.0 */
    int width, height;
} PcSwRaster;

void pcswInit(PcSwRaster *r, uint8_t *fb, float *zbuf, int w, int h);
void pcswClear(PcSwRaster *r, uint32_t rgba8888, float depth);
void pcswFillRect(PcSwRaster *r, float x0, float y0, float x1, float y1, uint32_t rgba8888);
/* Draw one triangle in normalized device coords (-1..1 after ortho below).
 * `vp` is the N64 viewport (scale_x, trans_x, scale_y, trans_y) used to map
 * clip space to pixels exactly like the RDP did. */
void pcswDrawTri(PcSwRaster *r, const PcVertex *v0, const PcVertex *v1,
                 const PcVertex *v2, const PcDrawState *st, const float vp[4]);
/* Decode an N64 texture from virtual RDRAM into RGBA8 (caller frees). */
uint8_t *pcswDecodeTexture(const uint8_t *rdram, size_t rdram_size,
                           const PcTexInfo *tex, uint32_t *out_w, uint32_t *out_h);

#endif /* PC_SW_RASTER_H */
