#ifndef PC_LIBULTRA_H
#define PC_LIBULTRA_H

/*
 * ssb-decomp PC Port - libultra stub layer (stage 3).
 *
 * The decompiled game code calls into osVi/osSp/osCont/osAudio routines that
 * on N64 are provided by the kernel. On PC we stand in a small cooperative
 * scheduler: video retrace drives frames, RSP tasks are executed inline via
 * the GBI interpreter, controllers map to keyboard/gamepad state, and audio
 * is a no-op ring buffer until a real mixer is wired.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcOsVi      PcOsVi;
typedef struct PcGbiBridge PcGbiBridge;

/* Create the fake VI + bridge over an already-created interpreter context.
 * `gi` is the opaque GbiInterpreter* from gbi_interpreter.h. */
PcOsVi *pcViCreate(void *gi);
void    pcViDestroy(PcOsVi *vi);

/* Block until the next simulated retrace (null backend: immediate). */
uint32_t pcViWaitRetrace(PcOsVi *vi);

/* Swap/present the current framebuffer (calls backend end_frame). */
void pcViSwapBuffers(PcOsVi *vi);

/* RSP task bridge: the game's scheduler posts display lists here; we run
 * them through the interpreter synchronously (retargeting-friendly). */
PcGbiBridge *pcGbiBridgeCreate(PcOsVi *vi);
void pcGbiBridgeSubmit(PcGbiBridge *br, uint32_t rdram_addr, uint32_t length_bytes);
void pcGbiBridgeDestroy(PcGbiBridge *br);

/* Controller state (stage 4): bitmask of pressed buttons, mapped from
 * keyboard when running with a windowed backend. */
#define PC_BTN_A     0x0001u
#define PC_BTN_B     0x0002u
#define PC_BTN_Z     0x0004u
#define PC_BTN_START 0x0008u
#define PC_BTN_DPAD  0x00F0u
#define PC_BTN_STICK 0xFF00u /* analog axes packed (see pcContRead) */

uint32_t pcContRead(int port);          /* digital+analog snapshot */
int16_t  pcContStickX(int port);
int16_t  pcContStickY(int port);

/* Audio (stage 4): minimal frame-tick so the game loop can advance without
 * a sequencer; real AI/AudioEmu wiring is TODO. */
void pcAudioTick(uint32_t frame_count);

#ifdef __cplusplus
}
#endif

#endif /* PC_LIBULTRA_H */
