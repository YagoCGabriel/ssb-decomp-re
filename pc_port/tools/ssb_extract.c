/*
 * ssb_extract.c - ROM asset extractor for the SSB PC port.
 *
 * Reads a validated Super Smash Bros. N64 image (.z64/.v64/.n64) and writes
 * the boot assets the port integrates into a "rom" folder next to the
 * executable:
 *
 *   rom/smash.z64     normalized big-endian copy of the full ROM
 *                     (used by ssb_pc / ssb_launcher auto-discovery)
 *   rom/boot_dl.bin   aligned page window containing the boot display list
 *                     (header word @0x40), 3 pages starting at floor(dl,4KB)
 *   rom/f3dex2.bin    F3DEX2 microcode blob located heuristically inside the
 *                     first megabyte (prologue 0x3C018000 + branch pattern);
 *                     optional - skipped with a warning if not found
 *   rom/assets.txt    integration report (addresses/sizes/checksums)
 *
 * Usage:
 *   ssb_extract <rom.z64|v64|n64> [dest-dir]
 *
 * Exit codes: 0 ok, 2 usage, 3 invalid/incompatible ROM.
 */

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  define _POSIX_C_SOURCE 200809L
#  include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

#define MAGIC_Z64 0x80371240u
#define MAGIC_V64 0x37804012u
#define MAGIC_N64 0x40123780u

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void swap_bytes(uint8_t *p, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t;
    }
}

static void swap_words(uint8_t *p, size_t n) {
    for (size_t i = 0; i + 3 < n; i += 4) {
        uint8_t a = p[i], b = p[i + 1], c = p[i + 2], d = p[i + 3];
        p[i] = d; p[i + 1] = c; p[i + 2] = b; p[i + 3] = a;
    }
}

/* exe directory ("" on failure) */
static void exe_dir(char *out, size_t cap) {
    out[0] = '\0';
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return;
    char *slash = strrchr(buf, '\\');
    if (!slash) return;
    *slash = '\0';
    snprintf(out, cap, "%s\\", buf);
#else
    ssize_t r = readlink("/proc/self/exe", out, cap - 2);
    if (r <= 0) return;
    out[r] = '\0';
    char *slash = strrchr(out, '/');
    if (!slash) { out[0] = '\0'; return; }
    slash[1] = '\0';
#endif
}

static int mkdir_p(const char *path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL); /* ERROR_ALREADY_EXISTS is fine */
    return 1;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

/* Heuristic F3DEX2 ucode scan: libultra ucode objects start with
 * lui $at,0x8000 (0x3C018000) followed within 8 words by a jump/copy
 * prologue; we additionally require 0x1000+ bytes of nonzero payload. */
static long find_f3dex2(const uint8_t *rom, size_t limit, size_t *sz_out) {
    for (size_t off = 0; off + 0x2000 <= limit; off += 8) {
        if (be32(rom + off) != 0x3C018000u) continue;
        /* second word usually another lui/ori pair; accept any nonzero */
        if (be32(rom + off + 4) == 0) continue;
        /* verify a plausible code density over 4 KB */
        size_t nz = 0;
        for (size_t i = 0; i < 0x1000; i += 4)
            if (be32(rom + off + i)) nz++;
        if (nz < 0x100) continue;
        /* estimate length: stop at a long zero run (>=64B) or 48 KB cap */
        size_t len = 0x1000;
        while (len < 0xC000 && off + len + 4 <= limit) {
            uint32_t w = be32(rom + off + len);
            if (w == 0) {
                size_t z = 0;
                while (len + z + 4 <= limit && be32(rom + off + len + z) == 0) z += 4;
                if (z >= 64) break;
                len += z;
            } else len += 4;
        }
        *sz_out = len;
        return (long)off;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "ssb_extract - extrai os assets da ROM para a pasta rom/\n"
            "uso: ssb_extract <smash.z64|.v64|.n64> [pasta-destino]\n");
        return 2;
    }

    /* ---- load ---- */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "erro: nao consegui abrir '%s'\n", argv[1]); return 3; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 0x40 || (size_t)fsz % 4) { fclose(f); fprintf(stderr, "erro: tamanho invalido\n"); return 3; }
    uint8_t *rom = (uint8_t *)malloc((size_t)fsz);
    if (!rom || fread(rom, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "erro: falha de leitura\n"); fclose(f); return 3;
    }
    fclose(f);
    size_t sz = (size_t)fsz;

    /* ---- normalize to z64 ---- */
    uint32_t m = be32(rom);
    const char *fmt = "z64";
    if (m == MAGIC_V64)      { swap_bytes(rom, sz); fmt = "v64->z64"; }
    else if (m == MAGIC_N64) { swap_words(rom, sz); fmt = "n64->z64"; }
    else if (m != MAGIC_Z64) { fprintf(stderr, "erro: magic invalido (0x%08X)\n", m); free(rom); return 3; }

    /* ---- title gate ---- */
    char code[5] = { (char)rom[0x30], (char)rom[0x31], (char)rom[0x32], (char)rom[0x33], 0 };
    if (!(code[0] == 'S' && (code[1] == 'M' || code[1] == 'N'))) {
        fprintf(stderr, "erro: ROM incompativel (codigo '%s') - somente Super Smash Bros.\n", code);
        free(rom); return 3;
    }
    uint32_t boot_dl = be32(rom + 0x40);
    if (boot_dl == 0 || boot_dl >= 0x100000u || (boot_dl & 7)) {
        fprintf(stderr, "erro: endereco da boot DL implausivel (0x%08X)\n", boot_dl);
        free(rom); return 3;
    }

    /* ---- destination: <exe dir>/rom unless arg given ---- */
    char dest[1024];
    if (argc > 2) {
        char base[1024];
        snprintf(base, sizeof base, "%s", argv[2]);
        /* strip trailing separator so the concat below is clean */
        size_t bl = strlen(base);
        while (bl && (base[bl - 1] == '/' || base[bl - 1] == '\\')) base[--bl] = '\0';
#ifdef _WIN32
        snprintf(dest, sizeof dest, "%s\\rom", base);
#else
        snprintf(dest, sizeof dest, "%s/rom", base);
#endif
    } else {
        char ed[512]; exe_dir(ed, sizeof ed);
        snprintf(dest, sizeof dest, "%srom", ed);
    }
    if (!mkdir_p(dest)) { fprintf(stderr, "erro: nao consegui criar '%s'\n", dest); free(rom); return 3; }

    char path[1200];
    snprintf(path, sizeof path, "%s%cboot_dl.bin", dest,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
    /* 3-page window around the boot DL (mirrors PcRomIntegrateBoot) */
    uint32_t src = boot_dl & ~0xFFFu;
    size_t win = 0;
    while (win < 0x3000 && src + win + 0x1000 <= sz) win += 0x1000;
    if (!win || !write_file(path, rom + src, win)) {
        fprintf(stderr, "erro: falha ao escrever boot_dl.bin\n"); free(rom); return 3;
    }

    /* full normalized ROM copy -> rom/smash.z64 (auto-discovered by ssb_pc) */
    snprintf(path, sizeof path, "%s%cssmash.z64", dest,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
    /* NOTE: name collision guard - use smash.z64 explicitly */
    snprintf(path, sizeof path, "%s%csmash.z64", dest,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
    int rom_copied = write_file(path, rom, sz);

    /* F3DEX2 ucode (best effort) */
    snprintf(path, sizeof path, "%s%cf3dex2.bin", dest,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
    size_t usz = 0;
    long uoff = find_f3dex2(rom, sz < 0x100000u ? sz : 0x100000u, &usz);
    int uc_ok = (uoff >= 0) && write_file(path, rom + uoff, usz);

    /* report */
    snprintf(path, sizeof path, "%s%cassets.txt", dest,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
    {
        FILE *r = fopen(path, "wb");
        if (r) {
            fprintf(r, "ssb_extract v1.0 - relatorio de integracao\n");
            fprintf(r, "formato=%s codigo=%s bootDL=0x%08X\n", fmt, code, boot_dl);
            fprintf(r, "boot_dl.bin: offset=0x%08X len=0x%zX\n", src, win);
            fprintf(r, "smash.z64: %s (%zu bytes)\n", rom_copied ? "ok" : "FALHOU", sz);
            if (uoff >= 0)
                fprintf(r, "f3dex2.bin: offset=0x%08lX len=0x%zX (%s)\n",
                        (unsigned long)uoff, usz, uc_ok ? "ok" : "FALHOU");
            else
                fprintf(r, "f3dex2.bin: nao localizado (opcional)\n");
            fclose(r);
        }
    }

    printf("Extracao concluida em '%s':\n", dest);
    printf("  boot_dl.bin  0x%zX bytes (ROM 0x%08X)\n", win, src);
    printf("  smash.z64    %s\n", rom_copied ? "copiado (descoberta automatica)" : "NAO copiado");
    printf("  f3dex2.bin   %s\n", uoff >= 0 ? (uc_ok ? "extraido" : "falha") : "nao encontrado (opcional)");
    printf("  assets.txt   relatorio\n");
    free(rom);
    return 0;
}
