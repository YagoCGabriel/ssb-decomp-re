/*
 * ssb-decomp PC Port - runtime glue shared by all entry points:
 * virtual RDRAM allocation, logging, backend auto-selection.
 */
#include "pc_port_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void pclog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

uint8_t *pcportRdram(void)
{
    static uint8_t *rdram;
    if (!rdram) {
        rdram = (uint8_t *)calloc(1, PCPORT_RDRAM_SIZE);
        if (!rdram) {
            pclog("port: FATAL: cannot allocate %u MB virtual RDRAM\n",
                  PCPORT_RDRAM_SIZE / (1024u * 1024u));
            abort();
        }
    }
    return rdram;
}

static int matches(const char *name, const char *pref)
{
    if (!pref || !*pref) return 0;
    return strcmp(name, pref) == 0;
}

PcBackend *backendAutoSelect(const char *preference)
{
    PcBackend *b;

    if (matches("null", preference)) {
        return backendCreateNull();
    }
#if defined(PCPORT_HAVE_VULKAN)
    if (matches("vulkan", preference)) {
        b = backendCreateVulkan();
        if (b) return b;
        pclog("port: Vulkan requested but unavailable\n");
    }
#endif
#if defined(PCPORT_HAVE_D3D)
    if (matches("d3d11", preference)) {
        b = backendCreateD3D11();
        if (b) return b;
        pclog("port: D3D11 requested but unavailable\n");
    }
    if (matches("d3d10", preference)) {
        b = backendCreateD3D10();
        if (b) return b;
        pclog("port: D3D10 requested but unavailable\n");
    }
#endif
    /* Auto order: Vulkan -> D3D11 -> D3D10 -> headless */
#if defined(PCPORT_HAVE_VULKAN)
    b = backendCreateVulkan();
    if (b && b->init(b, 640, 480, "SSB64")) return b;
    if (b) b->shutdown(b);
#endif
#if defined(PCPORT_HAVE_D3D)
    b = backendCreateD3D11();
    if (b && b->init(b, 640, 480, "SSB64")) return b;
    if (b) b->shutdown(b);
    b = backendCreateD3D10();
    if (b && b->init(b, 640, 480, "SSB64")) return b;
    if (b) b->shutdown(b);
#endif
    b = backendCreateNull();
    return b;
}

PcBackend *backendCreateWin32Common(void) { return NULL; }
