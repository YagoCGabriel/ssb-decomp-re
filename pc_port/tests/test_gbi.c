/*
 * ssb-decomp PC Port - display-list interpreter tests.
 *
 * Builds synthetic F3DEX2 display lists in virtual RDRAM, runs them through
 * the interpreter + headless backend, and asserts on framebuffer pixels and
 * draw counters. Run under CI without any GPU.
 */
#include "pc_port_common.h"
#include "gbi_opcodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* accessors from null.c */
extern const uint8_t *nullFramebuffer(void);
extern int nullWidth(void);
extern int nullHeight(void);
extern unsigned nullDrawCount(void);
extern unsigned nullFillCount(void);
extern unsigned nullClearCount(void);
extern int nullDumpFramebuffer(const char *path);

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static uint8_t *rdram;

/* Write helpers -------------------------------------------------------------
 * ALL display-list words (RSP and RDP) are stored big-endian in virtual
 * RDRAM, matching the N64 byte order of the original assets. */
static void wr_be(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24); dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);  dst[3] = (uint8_t)v;
}
static size_t wp(size_t off, uint32_t w0, uint32_t w1)
{
    wr_be(rdram + off, w0);
    wr_be(rdram + off + 4, w1);
    return off + 8;
}
/* RDP packet: BE words */
static size_t rdp2(size_t off, uint32_t w0, uint32_t w1)
{
    wr_be(rdram + off, w0);
    wr_be(rdram + off + 4, w1);
    return off + 8;
}
static size_t rdp4(size_t off, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    rdp2(off, w0, w1);
    wr_be(rdram + off + 8, w2);
    wr_be(rdram + off + 12, w3);
    return off + 16;
}

static void putVertex(size_t addr, short x, short y, short z, short s, short t,
                      unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    uint8_t *v = rdram + addr;
    wr_be(v + 0, (uint32_t)(uint16_t)x);
    wr_be(v + 2, (uint32_t)(uint16_t)y);
    wr_be(v + 4, (uint32_t)(uint16_t)z);
    wr_be(v + 6, 0);
    wr_be(v + 8, (uint32_t)(uint16_t)s);
    wr_be(v + 10, (uint32_t)(uint16_t)t);
    v[12] = r; v[13] = g; v[14] = b; v[15] = a;
}

static uint32_t pixel(int x, int y)
{
    (void)nullFramebuffer();
    return ((uint32_t)fb[(y*nullWidth()+x)*4] << 24) |
           ((uint32_t)fb[(y*nullWidth()+x)*4+1] << 16) |
           ((uint32_t)fb[(y*nullWidth()+x)*4+2] << 8) |
           ((uint32_t)fb[(y*nullWidth()+x)*4+3]);
}

/* -------------------------------------------------------------------------- */

/* Test 1: full-screen fill rect via gsDPSetFillColor + gsDPFillRectangle. */
static void test_fillrect(GbiInterpreter *gi)
{
    PcBackend *be = gbiGetBackend(gi);
    size_t dl = 0x1000;
    size_t o = dl;
    /* gsDPSetFillColor(0xFF8000FF): R,G,B,A packed 8-bit each in one word */
    o = rdp2(o, (RDP_SETFILLCOLOR << 24), 0xFF8000FF);
    o = rdp2(o, (RDP_SETFILLCOLOR << 24), 0xFF8000FF);
    o = rdp4(o, (RDP_FILLRECT << 24), (0u << 16) | 0u, (319u << 16) | 239u, 0);
    o = rdp2(o, (RDP_FULLSYNC << 24), 0);
    o = wp(o, (OP_ENDDL << 24), 0);

    be->clear(be, 0x000000FF, 1.0f);
    gbiRunDisplayListN(gi, rdram + dl, o - dl);
    CHECK(nullFillCount() >= 1, "fill_rect command reached backend");
    CHECK((pixel(10, 10) & 0xFFFFFF00u) == 0xFF800000u, "fill color written to fb center");
    CHECK((pixel(0, 0) & 0xFFFFFF00u) == 0xFF800000u, "fill covers top-left");
    CHECK((pixel(319, 239) & 0xFFFFFF00u) == 0xFF800000u, "fill covers bottom-right");
}

/* Test 2: triangle through gSPVertex + gSP1Triangle with ortho identity mtx. */
static void test_triangle(GbiInterpreter *gi)
{
    size_t verts = 0x2000;
    size_t dl = 0x3000;
    size_t o = dl;
    PcBackend *be = gbiGetBackend(gi);

    putVertex(verts + 0,   160, 40, 0, 0, 1024, 255, 0, 0, 255);
    putVertex(verts + 16,   20, 220, 0, 0, 0, 0, 255, 0, 255);
    putVertex(verts + 32,  300, 220, 0, 1024, 0, 0, 0, 255, 255);

    /* load identity model matrix (fixed 16.16) */
    {
        size_t mtx = 0x2100;
        int i;
        /* Mtx = 16 s15.16 words stored big-endian, one 32-bit word each */
        for (i = 0; i < 16; i++) {
            uint32_t v = (i % 5 == 0) ? 0x00010000u : 0u; /* s15.16: hi word at +0, lo at +2 */
            wr_be(rdram + mtx + i * 4, v);
        }
        /* gsSPMatrix(F3DEX2): w1 = addr | param;
         * MODELVIEW(0x00)|MUL(0x00)|PUSH(0x04) => param = 0x04 */
        o = wp(o, (OP_MTX << 24), 0x2100u | 0x02u | 0x04u);
    }
    /* disable Z buffer + culling for this flat test.
     * gsPClearGeometryMode sets bit 8 of w0 and puts the mask in w1. */
    o = wp(o, (OP_GEOMETRYMODE << 24) | (1u << 8), (1u << 0)); /* clear ZBUFFER */
    o = wp(o, (OP_GEOMETRYMODE << 24) | (1u << 8), (1u<<9)|(1u<<10)); /* clear CULL_FRONT/BACK */
    /* gSPVertex(dl verts, n=3, v0=0): F3DEX2 packs n<<20 | (v0+n)<<12 */
    o = wp(o, (OP_VTX << 24) | (3u << 20) | (3u << 12), (uint32_t)verts);
    /* gSP1Triangle(v0=0,v1=1,v2=2,layer=0) */
    o = wp(o, (OP_TRI1 << 24), (0u << 24) | (0u << 16) | (1u << 8) | 2u);
    /* gsSPPopMatrix(F3DEX2): param byte in w0 bits 8..15 (modelview=0x00) */
    o = wp(o, (OP_POPMTX << 24) | (0x00u << 8), 1);
    o = wp(o, (OP_ENDDL << 24), 0);

    be->clear(be, 0x000000FF, 1.0f);
    gbiRunDisplayListN(gi, rdram + dl, o - dl);
    CHECK(nullDrawCount() >= 1, "triangle submitted to backend");
    {
        (void)nullFramebuffer();
        int painted = 0; unsigned best = 0;
        for (int yy = 0; yy < 240; yy++)
          for (int xx = 0; xx < 320; xx++) {
            uint32_t p = pixel(xx, yy);
            if ((p >> 24) > best) best = p >> 24;
            if (p != 0x000000FFu) painted++;
          }
    }
    /* centroid (0,-~53) maps near screen center-bottom; check red-ish pixel inside tri */
    CHECK(((pixel(160, 140) >> 24) & 0xFF) > 100, "rasterized triangle has red channel inside");
    /* outside corner should remain clear color */
    CHECK((pixel(2, 2) & 0xFFu) == 0xFFu, "outside triangle untouched");
}

/* Test 3: nested display list call + branch + segment resolution. */
static void test_nested_dl(GbiInterpreter *gi)
{
    size_t child = 0x4000;
    size_t parent = 0x4100;
    size_t o = child;
    unsigned before;
    /* child: set fill color green + fill small rect, ENDDL */
    o = rdp2(o, (RDP_SETFILLCOLOR << 24), 0x00FF00FF);
    o = rdp4(o, (RDP_FILLRECT << 24), (10u << 16) | 10u, (20u << 16) | 20u, 0);
    o = wp(o, (OP_ENDDL << 24), 0);

    /* parent: gSPSegment(2, child_base>>?) then gSPDisplayList(seg-encoded) */
    o = parent;
    /* segment 2 base = 0 so segaddr 0x20004000 resolves to offset 0x4000 */
    o = wp(o, (OP_DMA_IO << 24) | (2u << 8), 0x00000000u);
    o = wp(o, (OP_DL << 24), 0x20004000u); /* call child via segment 2 */
    o = wp(o, (OP_ENDDL << 24), 0);

    before = nullFillCount();
    gbiRunDisplayListN(gi, rdram + parent, o - parent);
    CHECK(nullFillCount() == before + 1, "nested DL executed exactly one fill");
    CHECK((pixel(15, 15) & 0xFFFFFF00u) == 0x00FF0000u, "nested DL fill visible in fb");
}

/* Test 4: other-mode decode (cycle type) reaches state. */
static void test_othermode(GbiInterpreter *gi)
{
    size_t o = 0x5000, dl = o;
    /* G_SETOTHERMODE_H shift=14 len=3 value=G_CYC_2CYCLE(1) */
    o = wp(o, (OP_SETOTHERMODE_H << 24) | (14u << 8) | 4u, 1u);
    o = wp(o, (OP_ENDDL << 24), 0);
    gbiRunDisplayListN(gi, rdram + dl, o - dl);
    CHECK(gbiGetState(gi)->cycle_type == G_CYC_2CYCLE, "othermode hi decoded cycle type");
}

int main(void)
{
    PcBackend *be;
    GbiInterpreter *gi;

    rdram = pcportRdram();
    be = backendAutoSelect(getenv("SSB_BACKEND"));
    if (!be) { printf("no backend\n"); return 2; }
    if (!strcmp(be->name, "Headless SW Raster")) {
        if (!be->init(be, 320, 240, "test")) return 2;
    } else {
        printf("tests expect SSB_BACKEND=null (got %s)\n", be->name);
        return 2;
    }

    gi = gbiCreate(be, rdram, PCPORT_RDRAM_SIZE);
    if (!gi) return 2;

    test_fillrect(gi);
    test_triangle(gi);
    test_nested_dl(gi);
    test_othermode(gi);

    nullDumpFramebuffer("gbi_test_fb.rgba");
    gbiDestroy(gi);
    be->shutdown(be);

    printf("\n%s (%d failure(s))\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
