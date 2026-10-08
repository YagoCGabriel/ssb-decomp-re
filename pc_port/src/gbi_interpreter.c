/*
 * ssb-decomp PC Port - GBI display-list interpreter.
 *
 * Walks N64 RSP/RDP display lists produced by the decompiled game code and
 * translates them into calls on a PcBackend (Direct3D 10 / Direct3D 11 /
 * Vulkan). This lets the original game logic run unmodified: every scene's
 * gSPDisplayList / gSPVertex / gSP1Triangle stream is consumed here.
 *
 * SSB uses the F3DEX2 ucode family (dSYTaskmanUcodes in src/sys/taskman.c
 * loads gspF3DEX2_fifo), so opcode values follow the F3DEX_GBI_2 table from
 * include/PR/gbi.h (mirrored in gbi_opcodes.h).
 */

#include "gbi_interpreter.h"
#include "gbi_opcodes.h"
#include "pc_port_common.h"

#include <stdlib.h>
#include <string.h>

/* Display-list words are stored in N64 big-endian order regardless of host
 * architecture (the original game ran on a BE MIPS CPU, and the port's
 * virtual RDRAM keeps assets byte-identical). Decode every Gfx word with
 * be32(); do NOT rely on host endianness. */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

#define GB_G(w0) (((w0) >> 24) & 0xFF) /* command id in top byte */

#define GBI_MAX_VERTS 256
#define GBI_SEGMENTS 16
#define GBI_MAX_STACK 32
#define GBI_DL_DEPTH 8

/* Geometry-mode bits we map onto backend state (values from gbi.h) */
#define GB_G_ZBUFFER    0x00000001u
#define GB_G_CULL_FRONT 0x00000200u
#define GB_G_CULL_BACK  0x00000400u
#define GB_G_LIGHTING   0x00020000u

struct GbiInterpreter {
    PcBackend *backend;
    uint8_t *rdram;
    size_t rdram_size;

    uint32_t segments[GBI_SEGMENTS];

    /* vertex buffer (gSPVertex writes here, triangles index into it) */
    struct {
        short ob[3];
        unsigned short flag;
        short tc[2];
        unsigned char cn[4];
    } verts[GBI_MAX_VERTS];
    uint32_t nverts;

    /* tile state: per-tile dimensions recorded at load time */
    struct {
        uint32_t width, height;
    } tiles[8];

    PcMatrix model;
    PcMatrix proj;
    PcMatrix model_stack[GBI_MAX_STACK];
    int sp;

    PcDrawState state;
    uint32_t fill_color_packed; /* gDPSetFillColor */
    int dl_depth;

    /* RDPHALF_1 / RDPHALF_2 continuation staging (see runRdpPacket).
     * half1_valid distinguishes "no HALF1 pending" from a legitimate
     * payload word of 0. */
    uint32_t last_half1;   /* payload stashed by gsSPHalf1 */
    int half1_valid;       /* last_half1 holds an unconsumed payload */
    int pending4w;         /* waiting for the tail word of a split packet */
    uint32_t pend_w[4];    /* first three words of the pending packet */
};

/* Big-endian load helpers: virtual RDRAM stores N64 data in BE order. */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

void *gbiResolveAddr(GbiInterpreter *gi, uint32_t segaddr)
{
    uint32_t seg = (segaddr >> 28) & 0xF;
    uint32_t off = segaddr & 0x0FFFFFFF;
    uint32_t base;

    if (seg == 0) {
        base = 0; /* direct physical address */
    } else {
        base = gi->segments[seg];
    }

    /* Game code stores raw VRAM pointers (0x80XXXXXX) in some paths; mask to
     * the RDRAM window. */
    if (base >= 0x80000000u) {
        base &= 0x0FFFFFFFu;
    }
    if ((off + base) < gi->rdram_size) {
        return gi->rdram + off + base;
    }
    pclog("gbi: resolve out of range seg=%u addr=0x%08X\n", seg, segaddr);
    return NULL;
}

void gbiSetSegment(GbiInterpreter *gi, int index, uint32_t base)
{
    if (index >= 0 && index < GBI_SEGMENTS) {
        gi->segments[index] = base;
    }
}

const PcDrawState *gbiGetState(GbiInterpreter *gi)
{
    return &gi->state;
}

PcBackend *gbiGetBackend(GbiInterpreter *gi)
{
    return gi->backend;
}

/* Convert an N64 fixed-point matrix (int16 integer part + u16 fraction part
 * interleaved, i.e. the standard Mtx union layout) to float row-major. */
static void mtxToFloat(const uint8_t *raw, PcMatrix *out)
{
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int idx = i * 4 + j;
            int16_t hi = (int16_t)be16(raw + idx * 4);
            uint16_t lo = be16(raw + idx * 4 + 2);
            out->m[i][j] = (float)hi + (float)lo / 65536.0f;
        }
    }
}

static void mat4Identity(PcMatrix *m)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            m->m[i][j] = (i == j) ? 1.0f : 0.0f;
}

static void mat4Mul(const PcMatrix *a, const PcMatrix *b, PcMatrix *out)
{
    int i, j, k;
    PcMatrix t;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) {
                s += a->m[i][k] * b->m[k][j];
            }
            t.m[i][j] = s;
        }
    }
    *out = t;
}

static void transformVert(const PcMatrix *m, const short ob[3], float out[3])
{
    float x = (float)ob[0], y = (float)ob[1], z = (float)ob[2];
    out[0] = m->m[0][0]*x + m->m[0][1]*y + m->m[0][2]*z + m->m[0][3];
    out[1] = m->m[1][0]*x + m->m[1][1]*y + m->m[1][2]*z + m->m[1][3];
    out[2] = m->m[2][0]*x + m->m[2][1]*y + m->m[2][2]*z + m->m[2][3];
}

GbiInterpreter *gbiCreate(PcBackend *backend, uint8_t *rdram, size_t rdram_size)
{
    GbiInterpreter *gi = (GbiInterpreter *)calloc(1, sizeof(*gi));
    if (!gi) {
        return NULL;
    }
    gi->backend = backend;
    gi->rdram = rdram;
    gi->rdram_size = rdram_size;
    mat4Identity(&gi->model);
    mat4Identity(&gi->proj);
    gi->state.depth_test = 1;
    gi->state.depth_write = 1;
    gi->state.cull_face = 2;
    if (backend && backend->bind_rdram) {
        backend->bind_rdram(backend, rdram, rdram_size);
    }
    return gi;
}

void gbiDestroy(GbiInterpreter *gi)
{
    free(gi);
}

/* ---------------------------------------------------------------------------
 * State helpers
 * ------------------------------------------------------------------------- */

static void pushMatrices(GbiInterpreter *gi)
{
    if (gi->backend && gi->backend->set_matrices) {
        gi->backend->set_matrices(gi->backend, (const float *)&gi->model.m[0][0],
                                  (const float *)&gi->proj.m[0][0]);
    }
}

static void refreshGeomDecoded(GbiInterpreter *gi)
{
    gi->state.depth_write = !!(gi->state.geom_mode & GB_G_ZBUFFER);
    gi->state.depth_test = !!(gi->state.geom_mode & GB_G_ZBUFFER);
    gi->state.lighting = !!(gi->state.geom_mode & GB_G_LIGHTING);
    gi->state.cull_face = (gi->state.geom_mode & GB_G_CULL_BACK) ? 2
                          : ((gi->state.geom_mode & GB_G_CULL_FRONT) ? 1 : 0);
    if (gi->backend && gi->backend->set_geom_mode) {
        gi->backend->set_geom_mode(gi->backend, gi->state.geom_mode);
    }
}

static void pushTexture(GbiInterpreter *gi)
{
    if (gi->backend && gi->backend->set_texture) {
        PcTexInfo ti;
        uint32_t tile = gi->state.tex_tile & 7;
        memset(&ti, 0, sizeof(ti));
        ti.addr = gi->state.timg_addr;
        ti.width = gi->tiles[tile].width ? gi->tiles[tile].width : gi->state.tex_width;
        ti.height = gi->tiles[tile].height ? gi->tiles[tile].height : 1;
        ti.format = gi->state.tex_format;
        ti.size = gi->state.tex_size;
        ti.tlut = gi->state.tlut_addr;
        ti.mask_s = gi->state.mask_s; ti.mask_t = gi->state.mask_t;
        ti.mirror_s = gi->state.mirror_s; ti.mirror_t = gi->state.mirror_t;
        ti.shift_s = gi->state.shift_s; ti.shift_t = gi->state.shift_t;
        gi->backend->set_texture(gi->backend, &ti);
    }
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * ------------------------------------------------------------------------- */

/* gSPMatrix/gSPPopMatrix param bits. These match the F3DEX_GBI_2 (F3DEX2)
 * branch of include/PR/gbi.h (SSB loads gspF3DEX2_fifo), where libultra
 * defines G_MTX_PROJECTION=0x01, G_MTX_LOAD=0x02, G_MTX_PUSH=0x04. The
 * legacy F3D/F3DEX headers swap PROJECTION/PUSH; do not use those values. */
#define GB_MTX_PROJECTION 0x01u /* else modelview */
#define GB_MTX_LOAD       0x02u /* else multiply */
#define GB_MTX_PUSH       0x04u /* else no push   */

static void cmdVtx(GbiInterpreter *gi, const uint32_t *w)
{
    /* F3DEX2 gSPVertex packing:
     *   w0 = (OP_VTX<<24) | (n << 20) | ((v0 + n) << 12)
     *   w1 = vertex address
     */
    uint32_t n = (w[0] >> 20) & 0xF;
    uint32_t v0 = ((w[0] >> 12) & 0xFF) - n;
    const uint8_t *src;
    uint32_t i;

    if (n == 0 || n > GBI_MAX_VERTS) {
        return;
    }
    src = (const uint8_t *)gbiResolveAddr(gi, w[1]);
    if (!src) {
        return;
    }
    if (v0 + n > GBI_MAX_VERTS) {
        n = GBI_MAX_VERTS - v0;
    }
    for (i = 0; i < n; i++) {
        /* Vtx is 16 bytes big-endian in RDRAM:
         * [0..5]=ob xyz (s16) [6..7]=flag [8..11]=tc st (s16 fx10.2)
         * [12..15]=cn rgba (u8) */
        const uint8_t *v = src + i * 16;
        typeof(gi->verts[0]) *d = &gi->verts[v0 + i];
        d->ob[0] = (short)be16(v + 0);
        d->ob[1] = (short)be16(v + 2);
        d->ob[2] = (short)be16(v + 4);
        d->flag = be16(v + 6);
        d->tc[0] = (short)be16(v + 8);
        d->tc[1] = (short)be16(v + 10);
        d->cn[0] = v[12];
        d->cn[1] = v[13];
        d->cn[2] = v[14];
        d->cn[3] = v[15];
    }
    if (v0 + n > gi->nverts) {
        gi->nverts = v0 + n;
    }
}

static void emitTri(GbiInterpreter *gi, uint32_t a, uint32_t b, uint32_t c)
{
    PcVertex pv[3];
    uint32_t idx[3] = { a, b, c };
    int i;

    if (a >= gi->nverts || b >= gi->nverts || c >= gi->nverts) {
        return;
    }
    for (i = 0; i < 3; i++) {
        const typeof(gi->verts[0]) *v = &gi->verts[idx[i]];
        transformVert(&gi->model, v->ob, pv[i].pos);
        pv[i].color[0] = v->cn[0] / 255.0f;
        pv[i].color[1] = v->cn[1] / 255.0f;
        pv[i].color[2] = v->cn[2] / 255.0f;
        pv[i].color[3] = v->cn[3] / 255.0f;
        pv[i].uv[0] = (float)v->tc[0] / 4.0f; /* fx10.2 -> float */
        pv[i].uv[1] = (float)v->tc[1] / 4.0f;
        memcpy(pv[i].light, pv[i].color, sizeof(pv[i].light));
        pv[i].flags = (gi->state.lighting ? PCV_FLAG_LIGHTEN : 0u);
    }
    if (gi->backend && gi->backend->draw_triangles) {
        gi->backend->draw_triangles(gi->backend, &pv[0], &pv[1], &pv[2], &gi->state);
    }
}

static void cmdTri1(GbiInterpreter *gi, const uint32_t *w)
{
    /* F3DEX2: w1 = flags<<24 | v1<<16 | v2<<8 | v3 */
    uint32_t w1 = w[1];
    emitTri(gi, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF);
}

static void cmdTri2(GbiInterpreter *gi, const uint32_t *w)
{
    uint32_t w1 = w[1];
    uint32_t w3 = w[3];
    emitTri(gi, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF);
    emitTri(gi, (w3 >> 16) & 0xFF, (w3 >> 8) & 0xFF, w3 & 0xFF);
}

static void cmdMatrix(GbiInterpreter *gi, const uint32_t *w)
{
    /* gsSPMatrix: w1 = (address & ~0xF) | param, where the low 4 bits of the
     * address field are replaced by the param byte:
     *   G_MTX_PROJECTION=0x01 (else modelview), G_MTX_LOAD=0x02, G_MTX_PUSH=0x04 */
    uint32_t param = w[1] & 0xF;
    const uint8_t *raw = (const uint8_t *)gbiResolveAddr(gi, w[1] & 0xFFFFFFF0u);
    PcMatrix m, *cur;

    if (!raw) {
        return;
    }
    mtxToFloat(raw, &m);
    cur = (param & GB_MTX_PROJECTION) ? &gi->proj : &gi->model;

    if (param & GB_MTX_PUSH) { /* push */
        if (gi->sp < GBI_MAX_STACK - 1) {
            gi->model_stack[gi->sp++] = *cur;
        }
    }
    if (param & GB_MTX_LOAD) { /* load */
        *cur = m;
    } else { /* multiply: cur = cur * m */
        PcMatrix r;
        mat4Mul(cur, &m, &r);
        *cur = r;
    }
    pushMatrices(gi);
}

static void cmdPopMtx(GbiInterpreter *gi, const uint32_t *w)
{
    /* gsSPPopMatrix(F3DEX2): param byte in bits 8..15 of w0
     * (G_MTX_PROJECTION=0x01 selects the projection stack). */
    uint32_t param = (w[0] >> 8) & 0xFF;
    uint32_t n = w[1];
    if (param & GB_MTX_PROJECTION) {
        return; /* projection matrix stack unused by SSB */
    }
    while (n-- > 0 && gi->sp > 0) {
        gi->model = gi->model_stack[--gi->sp];
    }
    pushMatrices(gi);
}

static void cmdGeometryMode(GbiInterpreter *gi, const uint32_t *w, int set)
{
    /* F3DEX2 G_GEOMETRYMODE: mode bits live in w1 (low word); the high word
     * carries only the mask-length fields. */
    uint32_t mask = w[1];
    if (set) {
        gi->state.geom_mode |= mask;
    } else {
        gi->state.geom_mode &= ~mask;
    }
    refreshGeomDecoded(gi);
}

static void cmdSetOtherMode(GbiInterpreter *gi, const uint32_t *w, int hi)
{
    uint32_t shift = (w[0] >> 8) & 0xFF;
    uint32_t len = (w[0] & 0xFF) ? ((w[0] & 0xFF) - 1) : 31;
    uint64_t value = w[1];
    uint64_t mask = ((len + 1) >= 32) ? 0xFFFFFFFFull : ((1ull << (len + 1)) - 1);
    uint64_t *om = hi ? &gi->state.othermode_hi : &gi->state.othermode_lo;
    uint64_t clear = ~(mask << shift);
    *om = (*om & clear) | ((value & mask) << shift);

    gi->state.cycle_type = (int)((gi->state.othermode_hi >> G_MDSFT_CYCLETYPE) & 3);
    gi->state.alpha_comp = (int)(gi->state.othermode_lo & 3);
    gi->state.zsrc_sel = (int)((gi->state.othermode_lo >> G_MDSFT_ZSRCSEL) & 1);
    if (gi->backend && gi->backend->set_othermode) {
        gi->backend->set_othermode(gi->backend, gi->state.othermode_hi, gi->state.othermode_lo);
    }
}

static void cmdTexture(GbiInterpreter *gi, const uint32_t *w)
{
    /* gsSPTexture: w0 carries tile/on/mask/mirror/shift fields */
    uint32_t tile = (w[0] >> 18) & 7;
    uint32_t on = (w[0] >> 14) & 1;
    uint32_t sshift = (w[0] >> 8) & 0x3F;
    uint32_t tshift = (w[0] >> 2) & 0x3F;
    uint32_t smask = (w[0] >> 26) & 0xF;
    uint32_t tmask = (w[0] >> 22) & 0xF;
    uint32_t smirror = (w[0] >> 21) & 1;
    uint32_t tmirror = (w[0] >> 20) & 1;

    gi->state.tex_tile = tile;
    gi->state.mask_s = smask; gi->state.mirror_s = smirror; gi->state.shift_s = sshift;
    gi->state.mask_t = tmask; gi->state.mirror_t = tmirror; gi->state.shift_t = tshift;
    gi->state.texture_en = (int)on;
    pushTexture(gi);
}

static void cmdSetTile(GbiInterpreter *gi, const uint32_t *w)
{
    /* gsDPSetTile: w0 = op | palette<<20 | fmt<<18 | siz<<16 | line<<8 | tile
     * w1 = cmt_s<<30 | mask_s<<26 | shift_s<<20 | cmt_t<<18 | mask_t<<14 | shift_t<<8 */
    uint32_t tile = w[0] & 7;
    uint32_t line = (w[0] >> 8) & 0xFFF;
    uint32_t fmt = (w[0] >> 18) & 3;
    uint32_t pal = (w[0] >> 20) & 0x1F;
    uint32_t siz = (w[0] >> 16) & 3;
    uint32_t cmts = (w[1] >> 30) & 3;
    uint32_t masks = (w[1] >> 26) & 0xF;
    uint32_t shifts = (w[1] >> 20) & 0x3F;
    uint32_t cmtt = (w[1] >> 18) & 3;
    uint32_t maskt = (w[1] >> 14) & 0xF;
    uint32_t shiftt = (w[1] >> 8) & 0x3F;
    static const uint32_t bpp[4] = { 4, 8, 16, 32 };

    gi->state.tex_tile = tile;
    gi->state.tex_format = fmt;
    gi->state.tex_size = siz;
    gi->state.tlut_addr = pal;
    gi->tiles[tile].width = line ? (line * 8 / bpp[siz]) : gi->state.tex_width;
    gi->state.mask_s = masks; gi->state.shift_s = shifts;
    gi->state.mask_t = maskt; gi->state.shift_t = shiftt;
    gi->state.mirror_s = (cmts & 1);
    gi->state.mirror_t = (cmtt & 1);
    pushTexture(gi);
}

static void cmdLoadBlock(GbiInterpreter *gi, const uint32_t *w)
{
    uint32_t tile = (w[0] >> 24) & 7;
    uint32_t us = (w[1] >> 16) & 0xFFF;
    uint32_t vs = w[1] & 0xFFF;
    uint32_t cl = ((w[2] >> 16) & 0xFFF) - us;
    uint32_t rw = (w[2] & 0xFFF) - vs;
    gi->tiles[tile].width = cl + 1;
    gi->tiles[tile].height = rw + 1;
    pushTexture(gi);
}

static void cmdSetTileSize(GbiInterpreter *gi, const uint32_t *w)
{
    uint32_t tile = (w[0] >> 24) & 7;
    uint32_t uls = (w[1] >> 16) & 0xFFF;
    uint32_t ult = w[1] & 0xFFF;
    uint32_t lrs = (w[2] >> 16) & 0xFFF;
    uint32_t lrt = w[2] & 0xFFF;
    gi->tiles[tile].width = ((lrs - uls) >> 2) + 1; /* fx10.2 -> pixels */
    gi->tiles[tile].height = ((lrt - ult) >> 2) + 1;
    pushTexture(gi);
}

static void cmdLoadTile(GbiInterpreter *gi, const uint32_t *w)
{
    uint32_t tile = (w[0] >> 24) & 7;
    uint32_t sl = (w[0] >> 19) & 0x1F;
    uint32_t tl = (w[0] >> 14) & 0x1F;
    uint32_t sh = (w[0] >> 9) & 0x1F;
    uint32_t th = (w[0] >> 4) & 0x1F;
    gi->tiles[tile].width = sh - sl + 1;
    gi->tiles[tile].height = th - tl + 1;
    pushTexture(gi);
}

static void cmdLoadTlut(GbiInterpreter *gi, const uint32_t *w)
{
    uint32_t count = (w[1] >> 20) & 0xFF;
    uint32_t tile = (w[1] >> 16) & 7;
    gi->state.tlut_addr = (uint32_t)gi->state.timg_addr;
    if (count && !gi->tiles[tile].width) {
        gi->tiles[tile].width = count;
    }
    pushTexture(gi);
}

static void cmdSetCombine(GbiInterpreter *gi, const uint32_t *w)
{
    uint32_t mode = w[1];
    gi->state.combine_a[0] = (mode >> 28) & 0xF;
    gi->state.combine_b[0] = (mode >> 25) & 7;
    gi->state.combine_c[0] = (mode >> 22) & 7;
    gi->state.combine_d[0] = (mode >> 19) & 7;
    gi->state.combine_a[1] = (mode >> 16) & 7;
    gi->state.combine_b[1] = (mode >> 13) & 7;
    gi->state.combine_c[1] = (mode >> 10) & 7;
    gi->state.combine_d[1] = (mode >> 7) & 7;
    gi->state.combine_a[2] = (w[2] >> 28) & 0xF;
    gi->state.combine_b[2] = (w[2] >> 25) & 7;
    gi->state.combine_c[2] = (w[2] >> 22) & 7;
    gi->state.combine_d[2] = (w[2] >> 19) & 7;
    gi->state.combine_a[3] = (w[2] >> 16) & 7;
    gi->state.combine_b[3] = (w[2] >> 13) & 7;
    gi->state.combine_c[3] = (w[2] >> 10) & 7;
    gi->state.combine_d[3] = (w[2] >> 7) & 7;
    if (gi->backend && gi->backend->set_combine) {
        gi->backend->set_combine(gi->backend, gi->state.combine_a, gi->state.combine_b,
                                 gi->state.combine_c, gi->state.combine_d);
    }
}

static void cmdSetPrimColor(GbiInterpreter *gi, const uint32_t *w)
{
    uint32_t r = (w[1] >> 24) & 0xFF;
    uint32_t g = (w[1] >> 16) & 0xFF;
    uint32_t b = (w[1] >> 8) & 0xFF;
    uint32_t a = w[1] & 0xFF;
    gi->state.prim_color = (r << 24) | (g << 16) | (b << 8) | a;
    if (gi->backend && gi->backend->set_prim_color) {
        gi->backend->set_prim_color(gi->backend, gi->state.prim_color);
    }
}

static void cmdSetEnvColor(GbiInterpreter *gi, const uint32_t *w)
{
    gi->state.env_color = w[1] & 0xFFFFFFFFu;
    if (gi->backend && gi->backend->set_env_color) {
        gi->backend->set_env_color(gi->backend, gi->state.env_color);
    }
}

static void cmdSetScissor(GbiInterpreter *gi, const uint32_t *w)
{
    gi->state.scissor[0] = (float)((w[1] >> 16) & 0xFFF) / 4.0f;
    gi->state.scissor[1] = (float)(w[1] & 0xFFF) / 4.0f;
    gi->state.scissor[2] = (float)((w[2] >> 16) & 0xFFF) / 4.0f;
    gi->state.scissor[3] = (float)(w[2] & 0xFFF) / 4.0f;
    if (gi->backend && gi->backend->set_scissor) {
        gi->backend->set_scissor(gi->backend, gi->state.scissor[0], gi->state.scissor[1],
                                 gi->state.scissor[2], gi->state.scissor[3]);
    }
}

static void cmdFillRect(GbiInterpreter *gi, const uint32_t *w)
{
    float xl = (float)(w[1] >> 16) / 32.0f;
    float yl = (float)(w[1] & 0xFFFF) / 32.0f;
    float xh = (float)(w[2] >> 16) / 32.0f;
    float yh = (float)(w[2] & 0xFFFF) / 32.0f;
    if (gi->backend && gi->backend->fill_rect) {
        gi->backend->fill_rect(gi->backend, xl, yl, xh, yh, gi->fill_color_packed, &gi->state);
    }
}

/* Handle a pure-RDP packet (opcode in top byte, DP command space).
 * Returns the number of Gfx words consumed.
 *
 * In F3DEX2 display lists every command is exactly two Gfx words (8 bytes);
 * multi-word RDP operations are split across consecutive commands using the
 * gSPHalf1/gSPHalf2 (0xE1/0xF1) helpers. The interpreter keeps the payload
 * halves in `last_half1`/`pending4w` so that e.g. gsDPSetCombineLODs and
 * gsDPFillRectangle (emitted as SETHALF1 + SETCOMBINE/FILLRECT) reassemble
 * into full 4-word packets before hitting the backend. */
static int runRdpPacket(GbiInterpreter *gi, const uint8_t *bytes)
{
    uint32_t w[4];
    uint8_t rdp;

    w[0] = be32(bytes);
    w[1] = be32(bytes + 4);
    rdp = bytes[0];

    if (rdp == OP_RDPHALF_1) {
        gi->last_half1 = w[1];
        gi->half1_valid = 1;
        return 1;
    }

    /* Commands whose semantics need the second half of the packet. In real
     * F3DEX2 lists the layout is [RDPHALF_1][CMD w0/w1][RDPHALF_2][tail];
     * synthetic/test lists may pack all four words adjacently instead. */
    switch (rdp) {
    case RDP_SETCOMBINE:
    case RDP_SETSCISSOR:
    case RDP_FILLRECT:
    case RDP_TEXRECT:
    case RDP_TEXRECTFLIP:
    case RDP_LOADBLOCK:
    case RDP_LOADTILE:
    case RDP_SETTILESIZE:
    case RDP_LOADTLUT:
    case RDP_SETKEYGB:
    case RDP_SETKEYR:
    case RDP_SETCONVERT: {
        if (gi->half1_valid) {
            /* classic F3DEX2 layout: HALF1 carried the high word; stash the
             * first three words and wait for the tail command. */
            gi->pending4w = 1;
            gi->pend_w[0] = w[0];           /* command header */
            gi->pend_w[1] = w[1];           /* command low half */
            gi->pend_w[2] = gi->last_half1; /* HALF1 stash (unused here) */
            gi->half1_valid = 0;
            return 1;
        }
        /* adjacent packing (tests): bytes+8/+12 hold the continuation */
        w[2] = be32(bytes + 8);
        w[3] = be32(bytes + 12);
        break;
    }
    default:
        if (rdp >= RDP_TRI_GEN_BASE && rdp <= RDP_TRI_GEN_TOP) {
            /* generated triangle packets are 4 words (edge + shade) */
            w[2] = be32(bytes + 8);
            w[3] = be32(bytes + 12);
        }
        break;
    }

    switch (rdp) {
    case RDP_SETCIMG:
        gi->state.tex_format = (w[0] >> 21) & 3;
        gi->state.tex_size = (w[0] >> 19) & 3;
        gi->state.tex_width = ((w[0] >> 9) & 0x3FF) + 1;
        break;
    case RDP_SETZIMG:       break;
    case RDP_SETTIMG:
        gi->state.timg_addr = (uintptr_t)gbiResolveAddr(gi, w[1]);
        pushTexture(gi);
        break;
    case RDP_SETCOMBINE:    cmdSetCombine(gi, w); break;
    case RDP_SETENVCOLOR:   cmdSetEnvColor(gi, w); break;
    case RDP_SETPRIMCOLOR:  cmdSetPrimColor(gi, w); break;
    case RDP_SETBLENDCOLOR: break;
    case RDP_SETFOGCOLOR:   break;
    case RDP_SETFILLCOLOR:  gi->fill_color_packed = w[1]; break;
    case RDP_FILLRECT:      cmdFillRect(gi, w); break;
    case RDP_SETTILE:       cmdSetTile(gi, w); break;
    case RDP_LOADTILE:      cmdLoadTile(gi, w); break;
    case RDP_LOADBLOCK:     cmdLoadBlock(gi, w); break;
    case RDP_SETTILESIZE:   cmdSetTileSize(gi, w); break;
    case RDP_LOADTLUT:      cmdLoadTlut(gi, w); break;
    case RDP_RDPSETOTHERMODE:
        gi->state.othermode_hi = ((uint64_t)(w[0] & 0xFFFFFF) << 32) | w[1];
        gi->state.cycle_type = (int)((gi->state.othermode_hi >> G_MDSFT_CYCLETYPE) & 3);
        break;
    case RDP_SETPRIMDEPTH:  break;
    case RDP_SETSCISSOR:    cmdSetScissor(gi, w); break;
    case RDP_SETCONVERT:    break;
    case RDP_SETKEYR:       break;
    case RDP_SETKEYGB:      break;
    case RDP_FULLSYNC:
    case RDP_TILESYNC:
    case RDP_PIPESYNC:
    case RDP_LOADSYNC:      break;
    case RDP_TEXRECT:
    case RDP_TEXRECTFLIP:   break;
    default:
        if (rdp >= RDP_TRI_GEN_BASE && rdp <= RDP_TRI_GEN_TOP) {
            /* generated triangle packet: indices packed like TRI1 in w1 */
            cmdTri1(gi, w);
        }
        break;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Main loop
 * ------------------------------------------------------------------------- */

void gbiRunDisplayList(GbiInterpreter *gi, const void *dlptr)
{
    const uint8_t *dlb = (const uint8_t *)dlptr;
    int guard = 0;

    if (!dlb || gi->dl_depth >= GBI_DL_DEPTH) {
        return;
    }
    gi->dl_depth++;

    while (guard++ < 200000) {
        uint32_t w[4];
        uint8_t op;

        /* Every display-list command occupies 8 bytes. Gfx words are stored
         * big-endian in virtual RDRAM (N64 byte order); decode with be32()
         * regardless of host endianness. */
        op = dlb[0];
        w[0] = be32(dlb);
        w[1] = be32(dlb + 4);

        if (gi->pending4w) {
            /* This command is the tail word of a multi-word RDP packet whose
             * first half was already dispatched by runRdpPacket. */
            uint32_t pw[4];
            pw[0] = gi->pend_w[0];          /* command header word */
            pw[1] = gi->pend_w[1];          /* low half from the command */
            pw[2] = w[1];                   /* high half from RDPHALF_2 */
            pw[3] = 0;
            gi->pending4w = 0;
            /* Re-enter the DP dispatcher with the assembled 4-word packet. */
            switch ((uint8_t)(pw[0] >> 24)) {
            case RDP_SETCOMBINE:  cmdSetCombine(gi, pw); break;
            case RDP_SETSCISSOR:  cmdSetScissor(gi, pw); break;
            case RDP_FILLRECT:    cmdFillRect(gi, pw); break;
            case RDP_LOADBLOCK:   cmdLoadBlock(gi, pw); break;
            case RDP_LOADTILE:    cmdLoadTile(gi, pw); break;
            case RDP_SETTILESIZE: cmdSetTileSize(gi, pw); break;
            case RDP_LOADTLUT:    cmdLoadTlut(gi, pw); break;
            default:              break;
            }
            dlb += 8;
            continue;
        }

        if (op <= 0x0F || op == OP_SETOTHERMODE_H || op == OP_SETOTHERMODE_L ||
            op == OP_SPNOOP || op == OP_RDPHALF_1 || op == OP_RDPHALF_2) {
            /* ---- RSP commands: microcode ops 0x00..0x0F plus the F3DEX2
             * high-window members SPNOOP(0xE0)/RDPHALF_1(0xE1)/
             * SETOTHERMODE_L(0xE2)/SETOTHERMODE_H(0xE3)/RDPHALF_2(0xF1).
             * The remaining E4..EF values are DP-only and fall through to
             * the RDP dispatcher below. ---- */
            switch (op) {
        case OP_NOOP:
        case OP_SPNOOP:
            dlb += 8;
            break;
        case OP_VTX:
            cmdVtx(gi, w);
            dlb += 8;
            break;
        case OP_MODIFYVTX:
        case OP_BRANCH_Z:
        case OP_LINE3D:
            dlb += 8;
            break;
        case OP_CULLDL:
            dlb += 8;
            break;
        case OP_TRI1:
            cmdTri1(gi, w);
            dlb += 8;
            break;
        case OP_TRI2: {
            w[2] = be32(dlb + 8);
            w[3] = be32(dlb + 12);
            cmdTri2(gi, w);
            dlb += 16;
            break;
        }
        case OP_SETOTHERMODE_H:
            cmdSetOtherMode(gi, w, 1);
            dlb += 8;
            break;
        case OP_SETOTHERMODE_L:
            cmdSetOtherMode(gi, w, 0);
            dlb += 8;
            break;
        case OP_RDPHALF_1:
            /* payload for the next multi-word RDP command */
            gi->last_half1 = w[1];
            gi->half1_valid = 1;
            dlb += 8;
            break;
        case OP_RDPHALF_2:
            /* third word of a split packet; the fourth follows as the next
             * command in the list (handled by the pending4w path above) */
            gi->pend_w[2] = w[1];
            dlb += 8;
            break;
        case OP_ENDDL:
            goto done;
        case OP_DL: {
            /* mode bit 0 of w1: 0 = call (push), 1 = branch/jump */
            uint32_t target = w[1] & 0xFFFFFFF0u;
            if (w[1] & 1) {
                dlb = (const uint8_t *)gbiResolveAddr(gi, target);
                if (!dlb) goto done;
                continue;
            } else {
                const void *child = gbiResolveAddr(gi, target);
                dlb += 8;
                gbiRunDisplayList(gi, child);
            }
            break;
        }
        case OP_LOAD_UCODE:
        case OP_MOVEMEM:
            dlb += 16;
            break;
        case OP_MOVEWORD:
            /* gSPViewport encodes its payload via MoveWord into RSP dmem;
             * the port supplies viewports through the backend directly. */
            dlb += 8;
            break;
        case OP_MTX:
            cmdMatrix(gi, w);
            dlb += 8;
            break;
        case OP_GEOMETRYMODE:
            /* gSPSetGeometryMode vs gSPClearGeometryMode share the opcode;
             * gsPClearGeometryMode sets bit 8 of w0 (gsSPSetGeometryMode
             * leaves it clear). The command is a single Gfx (8 bytes): the
             * mask lives in w1. */
            cmdGeometryMode(gi, w, !(w[0] & 0x100u));
            dlb += 8;
            break;
        case OP_POPMTX:
            cmdPopMtx(gi, w);
            dlb += 8;
            break;
        case OP_TEXTURE:
            cmdTexture(gi, w);
            dlb += 16;
            break;
        case OP_DMA_IO:
            /* gSPSegment: io field (bits 8..11) = segment index */
            gbiSetSegment(gi, (w[0] >> 8) & 0xF, w[1]);
            dlb += 8;
            break;
        case OP_SPECIAL_1:
        case OP_SPECIAL_2:
        case OP_SPECIAL_3:
            dlb += 8;
            break;
        default:
            pclog("gbi: unknown RSP op 0x%02X at %p\n", op, (const void *)dlb);
            dlb += 8;
            break;
        }
            continue;
        }

        /* ---- Pure-RDP packets and generated triangle packets
         * (big-endian, DP command window D0..FF except the RSP half of
         *  E0..EF handled above) ---- */
        dlb += (size_t)runRdpPacket(gi, dlb) * 8u;
    }
    pclog("gbi: DL runaway guard hit\n");
done:
    gi->dl_depth--;
}
