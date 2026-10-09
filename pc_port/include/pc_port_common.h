#ifndef PC_PORT_COMMON_H
#define PC_PORT_COMMON_H

/*
 * ssb-decomp PC Port - shared declarations.
 *
 * Backend selection is done at runtime (Vulkan -> D3D11 -> D3D10 fallback),
 * so all backends expose the same factory symbol name pattern and are linked
 * into one binary.
 */

#include "gbi_interpreter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend factories (defined under src/backend). Return NULL if the API is
 * unavailable on the current OS/GPU. */
PcBackend *backendCreateNull(void);       /* headless, always available (tests) */
PcBackend *backendCreateWin32Common(void);/* placeholder for future Win32 glue  */

#if defined(PCPORT_HAVE_D3D)
PcBackend *backendCreateD3D10(void);
PcBackend *backendCreateD3D11(void);
#endif

#if defined(PCPORT_HAVE_VULKAN)
PcBackend *backendCreateVulkan(void);
#endif

/* Pick the best backend honoring SSB_BACKEND=vulkan|d3d11|d3d10|null env var. */
PcBackend *backendAutoSelect(const char *preference);

/* Virtual RDRAM: the port gives the game code a flat 8 MB buffer that stands
 * in for N64 RDRAM; display lists and assets live inside it. */
#define PCPORT_RDRAM_SIZE (8u * 1024u * 1024u)
uint8_t *pcportRdram(void);

/* Simple logging */
void pclog(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* PC_PORT_COMMON_H */
