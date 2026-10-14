/*
 * ssb-decomp PC Port - libultra stub implementation (stage 3/4).
 *
 * Provides the cooperative kernel pieces the game loop needs:
 *   - simulated VI retrace at ~60 Hz (sleep-based on GPU backends,
 *     immediate on null/headless so CI stays fast);
 *   - RSP task bridge that executes submitted display lists through the
 *     GBI interpreter synchronously;
 *   - controller state fed from environment/keyboard hooks (TODO: raw input);
 *   - audio tick placeholder.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pc_libultra.h"
#include "pc_port_common.h"
#include "gbi_interpreter.h"

#ifdef _WIN32
#include <windows.h>
static void pcSleepMs(unsigned ms) { Sleep(ms); }
#else
#define _POSIX_C_SOURCE 199309L
#include <time.h>
static void pcSleepMs(unsigned ms)
{
    struct timespec ts = { ms / 1000u, (ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

struct PcOsVi {
    GbiInterpreter *gi;
    PcBackend      *be;
    uint32_t        retrace_count;
    int             headless;   /* null backend -> no sleeping */
};

struct PcGbiBridge {
    PcOsVi *vi;
};

PcOsVi *pcViCreate(void *gi_)
{
    GbiInterpreter *gi = (GbiInterpreter *)gi_;
    if (!gi) return NULL;
    PcOsVi *vi = (PcOsVi *)calloc(1, sizeof(*vi));
    if (!vi) return NULL;
    vi->gi = gi;
    vi->be = gbiGetBackend(gi);
    vi->headless = (vi->be && strcmp(vi->be->name, "null") == 0);
    return vi;
}

void pcViDestroy(PcOsVi *vi)
{
    free(vi);
}

uint32_t pcViWaitRetrace(PcOsVi *vi)
{
    if (!vi) return 0;
    vi->retrace_count++;
    if (!vi->headless) {
        /* ~60 Hz cap; real vsync comes from the swapchain when wired. */
        pcSleepMs(16);
    }
    return vi->retrace_count;
}

void pcViSwapBuffers(PcOsVi *vi)
{
    if (vi && vi->be && vi->be->end_frame) {
        /* begin/end frame lifecycle is owned by the harness; present only. */
        vi->be->end_frame(vi->be);
    }
}

PcGbiBridge *pcGbiBridgeCreate(PcOsVi *vi)
{
    if (!vi) return NULL;
    PcGbiBridge *br = (PcGbiBridge *)calloc(1, sizeof(*br));
    if (br) br->vi = vi;
    return br;
}

void pcGbiBridgeSubmit(PcGbiBridge *br, uint32_t rdram_addr, uint32_t length_bytes)
{
    if (!br || !br->vi || !br->vi->gi) return;
    uint8_t *rdram = pcportRdram();
    if (rdram_addr >= PCPORT_RDRAM_SIZE) {
        pclog("gbi bridge: bad task address 0x%x\n", rdram_addr);
        return;
    }
    gbiRunDisplayListN(br->vi->gi, rdram + rdram_addr, length_bytes);
}

void pcGbiBridgeDestroy(PcGbiBridge *br)
{
    free(br);
}

/* --------------------------------------------------------------------------
 * Controller stubs. Keyboard mapping will be wired into the Win32/X11 event
 * pumps; until then we expose a neutral stick and no buttons. Tests can
 * override via SSB_SIM_BTN env bitmask for deterministic inputs.
 * ----------------------------------------------------------------------- */
uint32_t pcContRead(int port)
{
    if (port != 0) return 0xFFFFFFFFu; /* disconnected */
    const char *sim = getenv("SSB_SIM_BTN");
    return sim ? (uint32_t)strtoul(sim, NULL, 0) : 0u;
}

int16_t pcContStickX(int port) { (void)port; return 0; }
int16_t pcContStickY(int port) { (void)port; return 0; }

/* --------------------------------------------------------------------------
 * Audio placeholder: counts frames so music timers in game code advance.
 * TODO: hook the SSB sequencer (src/sys/audio*) to a real mixer backend.
 * ----------------------------------------------------------------------- */
void pcAudioTick(uint32_t frame_count)
{
    (void)frame_count;
}
