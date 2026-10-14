#ifndef PC_ROM_H
#define PC_ROM_H

/*
 * ssb-decomp PC Port - ROM loader / asset integrator (stage 5).
 *
 * Reads a Nintendo 64 ROM image (.z64 big-endian, .v64 byte-swapped or
 * .n64 wordswapped), normalizes it to native big-endian layout in memory,
 * validates the PI cart domain checksum against the game's own table
 * (src/sys/dma.c: D_800D3010/D_800D3020) and the Super Smash Bros.
 * title codes ("SM/" US, "SN" JP/EU), then extracts the assets the port
 * can integrate today:
 *
 *   - boot display list (address 0x40 of the ROM header) -> RDRAM @0x4000
 *     and executes it through the GBI interpreter as the first frame;
 *   - F3DEX2 / fifo microcode (header ucode pointer + known SSB offsets)
 *     -> copied into RDRAM so gsSPLoadUcode lists resolve;
 *   - static data blobs referenced by the decompiled build symbols are
 *     handled by the linker (the game code is compiled natively); the
 *     runtime ROM DMA path (osPiReadIo -> PcRomCopyToRdram) lets any
 *     remaining osPi calls read real asset data out of the loaded image.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcRomInfo {
    uint8_t  md5[16];          /* PI_BSD_DOM1_PID field (ROM bytes 0x10..0x1F) */
    char     internal_name[17];/* ASCII internal name at 0x20                  */
    char     game_code[5];     /* e.g. "SMJE", "SNSJ", "SMNP"                  */
    uint32_t rom_size;         /* file size after normalization                */
    uint32_t ram_size;         /* header RAMSize field                         */
    uint32_t boot_gfx_addr;    /* header word at 0x40: boot display list       */
    int      title_valid;      /* matches one of the supported SSB titles      */
    int      checksum_ok;      /* PI cart domain check passed                  */
} PcRomInfo;

/* Load and normalize a ROM from disk. Returns NULL on failure. */
uint8_t *PcRomLoad(const char *path, PcRomInfo *info_out);
void     PcRomUnload(uint8_t *rom);

/* Byte-swap helpers used by the loader (exposed for tests). */
void pcRomSwapBytes(uint8_t *p, size_t n);   /* v64: swap adjacent bytes   */
void pcRomSwapWords(uint8_t *p, size_t n);   /* n64: swap u32 endianness   */

/* Detect the swap format from magic + heuristics; returns 0=z64, 1=v64, 2=n64, -1 unknown */
int  pcRomDetectFormat(const uint8_t *head48);

/* Copy `len` bytes of ROM at `rom_addr` into virtual RDRAM at `rdram_addr`
 * (mirrors osPiReadIo/osEPi behavior used by sys/dma.c). Bounds-checked. */
int  PcRomCopyToRdram(const uint8_t *rom, uint32_t rom_size,
                      uint8_t *rdram, size_t rdram_size,
                      uint32_t rom_addr, uint32_t rdram_addr, uint32_t len);

/* Integrate the ROM's boot assets into RDRAM: copies the boot display list
 * (and its referenced vertex/matrix blobs when they live inside the DL's
 * page) to their canonical RDRAM addresses. Returns the resolved top-level
 * DL address in RDRAM space, or 0 if nothing usable was found. */
uint32_t PcRomIntegrateBoot(const uint8_t *rom, uint32_t rom_size,
                            uint8_t *rdram, size_t rdram_size,
                            const PcRomInfo *info);

/* The official PI_BSD_DOM1_PID MD5 for Super Smash Bros. (N64), taken from
 * src/sys/dma.c (D_800D3010). Exposed so the CLI setup flow can print it. */
extern const uint8_t pcRomSmashMd5[16];

#ifdef __cplusplus
}
#endif

#endif /* PC_ROM_H */
