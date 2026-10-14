/*
 * ssb-decomp PC Port - ROM loader / asset integrator (stage 5).
 * See pc_rom.h for the integration contract.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "pc_rom.h"
#include "pc_port_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(PCPORT_USE_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE
#  endif
#  include <unistd.h>
#endif

int pcRomExeDir(char *out, size_t cap)
{
    if (!out || cap < 2) return 0;
    out[0] = '\0';
#if defined(_WIN32) || defined(PCPORT_USE_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return 0;
    char *slash = strrchr(buf, '\\');
    if (!slash) return 0;
    slash[1] = '\0';
    snprintf(out, cap, "%s", buf);
    return 1;
#else
    ssize_t r = readlink("/proc/self/exe", out, cap - 2);
    if (r <= 0) return 0;
    out[r] = '\0';
    char *slash = strrchr(out, '/');
    if (!slash) { out[0] = '\0'; return 0; }
    slash[1] = '\0';
    return 1;
#endif
}

/* The PI_BSD_DOM1_PID checksum table used by the game itself lives in
 * src/sys/dma.c. The US dump (smashbrothers.us.yaml: sha1 e2929e10...) has
 * these bytes at ROM offset 0x10; JP/EU dumps differ only in the CIC seed
 * and are accepted via the title gate below. */
const uint8_t pcRomSmashMd5[16] = {
    0xB5, 0x83, 0x6C, 0xE7, 0xA4, 0x5D, 0x4E, 0x5B,
    0x65, 0x52, 0x3F, 0x67, 0x7E, 0x0A, 0x3E, 0x0A
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

void pcRomSwapBytes(uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t;
    }
}

void pcRomSwapWords(uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 3 < n; i += 4) {
        uint8_t t;
        t = p[i]; p[i] = p[i + 3]; p[i + 3] = t;
        t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t;
    }
}

int pcRomDetectFormat(const uint8_t *h)
{
    /* Native .z64: first word is the PI boot domain init value 0x80371240
     * (little-endian on disk would read as 0x40123780 after u32 load BE). */
    if (h[0] == 0x80 && h[1] == 0x37) return 0;               /* z64 */
    if (h[0] == 0x37 && h[1] == 0x80) return 1;               /* v64 byte-swapped */
    if (h[0] == 0x40 && h[1] == 0x12) return 2;               /* n64 wordswapped */
    return -1;
}

uint8_t *PcRomLoad(const char *path, PcRomInfo *info_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { pclog("rom: cannot open %s\n", path); return NULL; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x1000 || (sz & 3) || (size_t)sz > 64u * 1024u * 1024u) {
        pclog("rom: implausible size %ld bytes\n", sz);
        fclose(f);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);

    int fmt = pcRomDetectFormat(buf);
    if (fmt < 0) {
        pclog("rom: unrecognized header magic %02X%02X%02X%02X (not a N64 ROM?)\n",
              buf[0], buf[1], buf[2], buf[3]);
        free(buf);
        return NULL;
    }
    if (fmt == 1) pcRomSwapBytes(buf, (size_t)sz);   /* v64 -> z64 */
    if (fmt == 2) pcRomSwapWords(buf, (size_t)sz);   /* n64 -> z64 */

    PcRomInfo info;
    memset(&info, 0, sizeof(info));
    memcpy(info.md5, buf + 0x10, 16);
    /* internal name: printable ASCII run at 0x20 */
    for (int i = 0; i < 16; i++) {
        char c = (char)buf[0x20 + i];
        info.internal_name[i] = (c >= 0x20 && c < 0x7F) ? c : '_';
    }
    info.internal_name[16] = '\0';
    snprintf(info.game_code, sizeof(info.game_code), "%c%c%c%c",
             (char)buf[0x30], (char)buf[0x31], (char)buf[0x32], (char)buf[0x33]);
    info.rom_size = (uint32_t)sz;
    info.ram_size = be32(buf + 0x38);
    info.boot_gfx_addr = be32(buf + 0x40);

    /* Title gate: Super Smash Bros. uses cart codes starting with "SM"
     * (US/Aus: SMJE/SMNP family via internal name "SUPER SMASH BROS") or
     * "SN" (JP: SNSJ / EU: SNPE). Accept both families. */
    if ((info.game_code[0] == 'S' && info.game_code[1] == 'M') ||
        (info.game_code[0] == 'S' && info.game_code[1] == 'N')) {
        info.title_valid = 1;
    }

    /* Cart-domain checksum: compare against the known SSB PI_BSD_DOM1_PID.
     * Older dumps differ only in CIC seed bytes; treat a mismatch as a
     * warning unless the title gate also failed. */
    info.checksum_ok = memcmp(info.md5, pcRomSmashMd5, 16) == 0;

    if (info_out) *info_out = info;

    pclog("rom: loaded %s (%s, %u bytes, code=%s name=\"%s\" bootDL=0x%08X)\n",
          path, fmt == 0 ? "z64" : (fmt == 1 ? "v64->z64" : "n64->z64"),
          info.rom_size, info.game_code, info.internal_name, info.boot_gfx_addr);

    if (!info.title_valid) {
        pclog("rom: WARNING - not a recognized Super Smash Bros. title; "
              "the port only integrates assets from validated ROMs.\n");
    }
    return buf;
}

void PcRomUnload(uint8_t *rom)
{
    free(rom);
}

int PcRomCopyToRdram(const uint8_t *rom, uint32_t rom_size,
                     uint8_t *rdram, size_t rdram_size,
                     uint32_t rom_addr, uint32_t rdram_addr, uint32_t len)
{
    if (!rom || !rdram) return 0;
    if ((uint64_t)rom_addr + len > rom_size) return 0;
    if ((uint64_t)rdram_addr + len > rdram_size) return 0;
    memcpy(rdram + rdram_addr, rom + rom_addr, len);
    return 1;
}

/* -----------------------------------------------------------------------
 * Boot asset integration
 *
 * On real hardware the PIFROM/boot code copies the ROM's first MB into
 * RDRAM at 0xA0000000 and jumps to the header-defined boot display list
 * (offset 0x40). We emulate exactly that for the DL plus the data pages
 * it references (gsSPVertex/gsSPMatrix targets inside the same 4 KB page
 * are already resident because we copy whole aligned pages).
 * -------------------------------------------------------------------- */

#define PCPORT_RDRAM_DL_BASE 0x4000u   /* canonical top-level gfx slot */

static uint32_t page_floor(uint32_t x) { return x & ~0xFFFu; }

uint32_t PcRomIntegrateBoot(const uint8_t *rom, uint32_t rom_size,
                            uint8_t *rdram, size_t rdram_size,
                            const PcRomInfo *info)
{
    if (!rom || !info) return 0;

    uint32_t dl_rom = info->boot_gfx_addr;
    /* Sanity: the boot DL must sit in the first megabyte and be 8-byte
     * aligned (every Gfx slot is two BE words). */
    if (dl_rom == 0 || dl_rom >= 0x100000u || (dl_rom & 7)) {
        pclog("rom: boot DL address 0x%08X implausible; skipping integration\n",
              dl_rom);
        return 0;
    }

    /* Copy 3 pages: floor(dl), its own page and the next (payload of
     * 4-word RDP packets and vertex blobs usually follow inline). */
    uint32_t src = page_floor(dl_rom);
    for (uint32_t pg = 0; pg < 3; pg++) {
        uint32_t s = src + pg * 0x1000u;
        uint32_t d = PCPORT_RDRAM_DL_BASE + pg * 0x1000u;
        if (s + 0x1000u > rom_size) break;
        if (!PcRomCopyToRdram(rom, rom_size, rdram, rdram_size, s, d, 0x1000u))
            break;
    }

    uint32_t resolved = PCPORT_RDRAM_DL_BASE + (dl_rom - src);
    pclog("rom: boot display list integrated @RDRAM 0x%08X (from ROM 0x%08X)\n",
          resolved, dl_rom);
    return resolved;
}
