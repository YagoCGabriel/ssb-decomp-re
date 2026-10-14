/* pc_config.h — advanced runtime configuration for the PC port.
 *
 * Single source of truth for all tunables consumed by main.c, backends and
 * the libultra stub.  Loaded in this order (later wins):
 *   1) built-in defaults
 *   2) config file:  --config <path> | SSB_CONFIG env | ssb_config.ini next
 *      to the executable (simple KEY = VALUE lines, '#' comments; values
 *      are also accepted as "key=value")
 *   3) environment overrides  SSB_<KEY>   (e.g. SSB_BACKEND=vulkan)
 *   4) command line           --key value (highest precedence)
 *
 * Keys (see pcCfgDefaults for values):
 *   rom, backend, frames, width, height, fullscreen, vsync, max_fps,
 *   resolution_scale, internal_width, internal_height, aspect, integer_scale,
 *   cull_mode, depth_func, wireframe, clear_color, anisotropy, msaa,
 *   log_level, sim_buttons, dump_dir, headless.
 */
#ifndef PC_CONFIG_H
#define PC_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcConfig {
    /* boot / general */
    char    rom[512];          /* path to .z64/.v64/.n64 (empty = demo scene) */
    char    backend[16];       /* auto|vulkan|d3d11|d3d10|null */
    int     frames;            /* 0 = run forever; >0 = exit after N frames */
    int     headless;          /* 1 = no window/present (CI) */

    /* window / presentation */
    int     width, height;     /* window size in pixels */
    int     fullscreen;        /* 0/1 */
    int     vsync;             /* 0 = off, 1 = on, 2 = adaptive */
    int     max_fps;           /* 0 = uncapped; else frame limiter */

    /* rendering */
    float   resolution_scale;  /* internal buffer scale (1.0 = native-ish) */
    int     internal_width;    /* 0 = derive from width*scale */
    int     internal_height;   /* 0 = derive from height*scale */
    float   aspect;            /* display aspect; <=0 => 4:3 (N64 default) */
    int     integer_scale;     /* snap framebuffer to integer multiples */
    int     cull_mode;         /* 0 none, 1 front, 2 back (default 2) */
    int     depth_func;        /* 0 disable, 1 less, 2 lequal, 3 greater */
    int     wireframe;         /* debug: draw edges only */
    uint32_t clear_color;      /* 0xRRGGBBAA */
    int     anisotropy;        /* 1..16 texture filtering hint */
    int     msaa;              /* 0/1 = off, 2/4/8 = sample count */

    /* diagnostics */
    int     log_level;         /* 0 quiet .. 3 verbose */
    uint32_t sim_buttons;      /* libultra stub: simulated controller bits */
    char    dump_dir[512];     /* framebuffer dumps (.rgba); empty = off */
} PcConfig;

/* Returns the process-wide singleton config (already loaded). */
PcConfig *pcConfig(void);

/* Parse argv (--rom X --backend vulkan ...), then config file, then env.
 * Unknown args are ignored (returns 0) except --help which returns 1 so the
 * caller can print usage and exit. */
int pcConfigInit(int argc, char **argv);

/* Write current settings to an ini file (used by the launcher). */
int pcConfigSave(const char *path);

/* Dump keys/values to the log at level info. */
void pcConfigLog(const PcConfig *cfg);

/* Effective internal render-buffer size after applying scale/integer rules. */
void pcConfigFramebufferSize(const PcConfig *cfg, int *w, int *h);

#ifdef __cplusplus
}
#endif
#endif /* PC_CONFIG_H */
