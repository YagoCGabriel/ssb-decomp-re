/*
 * ssb-decomp PC Port - entry point / frame harness.
 *
 * Stage 4-5 glue: creates a backend (Vulkan -> D3D11 -> D3D10 -> null),
 * builds the GBI interpreter over the virtual RDRAM, and runs a demo
 * display list each frame. On the null backend it exits after one frame
 * (CI/headless); on GPU backends it pumps frames until the window closes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pc_port_common.h"
#include "gbi_interpreter.h"
#include "gbi_opcodes.h"
#include "pc_libultra.h"
#include "pc_rom.h"

/* Helpers exported by the null backend for headless validation. */
const uint8_t *nullFramebuffer(void);
unsigned nullDrawCount(void);
unsigned nullFillCount(void);

/* --------------------------------------------------------------------------
 * Big-endian writers: N64 assets/display lists live in RDRAM as BE data.
 * ----------------------------------------------------------------------- */
static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF);
}


static uint8_t *rdram_g;

/* Append one 8-byte Gfx slot (two big-endian u32 words), mirroring
 * tests/test_gbi.c's wp() helper. */
static size_t wp(size_t off, uint32_t w0, uint32_t w1)
{
    wr_be32(rdram_g + off + 0, w0);
    wr_be32(rdram_g + off + 4, w1);
    return off + 8;
}
/* Four-word (16-byte) RDP packet. */
static size_t rdp4(size_t off, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    off = wp(off, w0, w1);
    wr_be32(rdram_g + off + 0, w2);
    wr_be32(rdram_g + off + 4, w3);
    return off + 8;
}

static void putVtx16(size_t addr, short x, short y, short z,
                     unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    uint8_t *v = rdram_g + addr;
    put16(v + 0, (uint16_t)x);
    put16(v + 2, (uint16_t)y);
    put16(v + 4, (uint16_t)z);
    put16(v + 6, 0);            /* s */
    put16(v + 8, 0);            /* t */
    v[10] = 0; v[11] = 0;       /* Clr -> Cb/Cr hi */
    v[12] = r; v[13] = g; v[14] = b; v[15] = a;
}

/* --------------------------------------------------------------------------
 * Demo scene: clear + fullscreen fill, then a big triangle through
 * gSPMatrix/gSPVertex/gSP1Triangle (same encodings validated by test_gbi.c).
 * ----------------------------------------------------------------------- */
#define VW 320
#define VH 240

static size_t buildDemoList(size_t vtx_off, size_t dl_off)
{
    uint8_t *v = rdram_g + vtx_off;
    (void)v;
    putVtx16(vtx_off +  0, 160 * 32,  20 * 32, 0, 0xE0, 0x30, 0x30, 0xFF);
    putVtx16(vtx_off + 16,  20 * 32, 220 * 32, 0, 0x30, 0x60, 0xE0, 0xFF);
    putVtx16(vtx_off + 32, 300 * 32, 220 * 32, 0, 0x30, 0xC0, 0x60, 0xFF);

    size_t o = dl_off;

    /* Identity model matrix (s15.16 fixed, BE) */
    size_t mtx_off = 0x7E00;
    for (int i = 0; i < 16; i++)
        wr_be32(rdram_g + mtx_off + (size_t)i * 4, (i % 5 == 0) ? 0x00010000u : 0u);

    /* gsSPMatrix(F3DEX2): w1 = addr | param; MUL|LOAD|PUSH => 0x04 */
    o = wp(o, (OP_MTX << 24), (uint32_t)mtx_off | 0x04u);

    /* Clear ZBUFFER + culling bits (gsPClearGeometryMode: bit8 of w0 set) */
    o = wp(o, (OP_GEOMETRYMODE << 24) | (1u << 8), (1u << 0) | (1u << 9) | (1u << 10));

    /* Background fill (fill color cyan), pushed to primitive far depth so
     * it behaves like a real background instead of occluding the scene. */
    o = wp(o, (RDP_SETPRIMDEPTH << 24) | 0x7FFFu, 0u);
    o = wp(o, (RDP_SETFILLCOLOR << 24), 0x20C0C0FFu);
    o = rdp4(o, (RDP_FILLRECT << 24), 0u, (319u << 16) | 239u, 0u);

    /* gSPVertex(addr, n=3, v0=0): F3DEX2 packs n<<20 | (v0+n)<<12 */
    o = wp(o, (OP_VTX << 24) | (3u << 20) | (3u << 12), (uint32_t)vtx_off);

    /* gSP1Triangle(v0=0, v1=1, v2=2, layer=0) */
    o = wp(o, (OP_TRI1 << 24), (0u << 24) | (0u << 16) | (1u << 8) | 2u);

    /* gsSPPopMatrix(modelview): param byte in w0 bits 8..15 */
    o = wp(o, (OP_POPMTX << 24) | (0x00u << 8), 1);

    /* gsSPEndDisplayList */
    o = wp(o, (OP_ENDDL << 24), 0);
    return o;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    PcBackend *be = backendAutoSelect(NULL);
    if (!be) { fprintf(stderr, "no backend available\n"); return 1; }

    int W = VW, H = VH;
    if (!be->init(be, W, H, "SSB64 PC Port")) {
        fprintf(stderr, "backend init failed\n");
        return 1;
    }

    uint8_t *rdram = pcportRdram();
    memset(rdram, 0, PCPORT_RDRAM_SIZE);
    rdram_g = rdram;

    GbiInterpreter *gi = gbiCreate(be, rdram, PCPORT_RDRAM_SIZE);
    if (!gi) { fprintf(stderr, "interpreter init failed\n"); return 1; }

    be->bind_rdram(be, rdram, PCPORT_RDRAM_SIZE);

    /* ------------------------------------------------------------------
     * Stage 5: optional ROM boot integration. SSB_ROM=<path> makes the
     * harness load a validated Super Smash Bros. image and run its real
     * boot display list (the IPL gfx posted by osViSetMode at cold boot).
     * Without a ROM we fall back to the synthetic demo scene, which is
     * what the headless test suite exercises.
     * ---------------------------------------------------------------- */ */
    const char *rom_path = getenv("SSB_ROM");
    uint8_t *rom = NULL;
    PcRomInfo rom_info;
    uint32_t rom_dl_addr = 0;
    size_t   rom_dl_len  = 0x1000; /* conservative first-page budget */
    size_t   dl_end      = 0;
    int      using_rom   = 0;

    if (rom_path && *rom_path) {
        memset(&rom_info, 0, sizeof(rom_info));
        rom = PcRomLoad(rom_path, &rom_info);
        if (rom && rom_info.title_valid) {
            rom_dl_addr = PcRomIntegrateBoot(rom, rom_info.rom_size,
                                             rdram, PCPORT_RDRAM_SIZE,
                                             &rom_info);
            if (rom_dl_addr) {
                using_rom = 1;
                /* cap the top-level walk to the copied page so stray data
                 * past ENDDL can never be interpreted as commands */
                size_t page_avail = PCPORT_RDRAM_DL_PAGE - (rom_dl_addr % PCPORT_RDRAM_DL_PAGE);
                rom_dl_len = page_avail < 0x1000u ? page_avail : 0x1000u;
            }
        } else if (rom) {
            pclog("rom: title gate failed - ignoring ROM, using demo scene\n");
        }
    }

    if (!using_rom) {
        dl_end = buildDemoList(0x6000, 0x4000);
        pclog("demo display list built: %zu bytes @0x4000\n", dl_end - 0x4000);
    }

    /* libultra stub layer: VI retrace + RSP task bridge (stage 3). */
    PcOsVi *vi = pcViCreate(gi);
    PcGbiBridge *br = pcGbiBridgeCreate(vi);

    int is_null = getenv("SSB_BACKEND") && strcmp(getenv("SSB_BACKEND"), "null") == 0;
    int frames = is_null ? 1 : 240; /* ~4s at 60fps on GPU backends */
    {
        const char *f_env = getenv("SSB_FRAMES"); /* CI override from setup wizard */
        if (f_env) { int n = atoi(f_env); if (n > 0) frames = n; }
    }

    for (int f = 0; f < frames; f++) {
        be->begin_frame(be);
        be->clear(be, 0x102030FFu, 1.0f);
        /* Game scheduler would post the frame's gfx task here; run it via
         * the bridge exactly like osSpQueueYielded/osAfterMniTask paths. */
        if (using_rom)
            pcGbiBridgeSubmit(br, rom_dl_addr, (uint32_t)rom_dl_len);
        else
            pcGbiBridgeSubmit(br, 0x4000u, (uint32_t)(dl_end - 0x4000));
        pcContRead(0);      /* input snapshot (stub) */
        pcAudioTick((uint32_t)f); /* audio tick (stub) */
        pcViWaitRetrace(vi);
        pcViSwapBuffers(vi);
    }

    if (is_null && !using_rom) {
        unsigned draws = nullDrawCount(), fills = nullFillCount();
        const uint8_t *fb = nullFramebuffer();
        int painted = 0;
        if (fb) {
            for (int y = 0; y < VH; y++)
                for (int x = 0; x < VW; x++) {
                    const uint8_t *px = fb + ((size_t)y * VW + x) * 4;
                    if (px[0] || px[1] || px[2]) { painted++; }
                }
        }
        printf("headless: draws=%u fills=%u painted_px=%d\n", draws, fills, painted);
        if (draws < 1 || fills < 1 || painted < 1000) {
            fprintf(stderr, "FAIL: demo scene did not render\n");
            return 1;
        }
    }

    pcGbiBridgeDestroy(br);
    pcViDestroy(vi);
    gbiDestroy(gi);
    be->shutdown(be);
    pclog("port harness finished OK\n");
    return 0;
}
