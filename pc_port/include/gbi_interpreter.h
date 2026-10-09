#ifndef GBI_INTERPRETER_H
#define GBI_INTERPRETER_H

/*
 * ssb-decomp PC Port - GBI (Graphics Bus Interface) display-list interpreter.
 *
 * The original Super Smash Bros. 64 code builds RSP/RDP display lists (Gfx
 * command buffers). This header defines the subset of the N64 GBI we need to
 * recognize, and an "engine" interface that a hardware backend (Direct3D 10,
 * Direct3D 11 or Vulkan) implements so those display lists can be rendered on
 * PC GPUs without modifying game code.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * N64 GBI constants (subset, values identical to include/PR/gbi.h)
 * ------------------------------------------------------------------------- */

/* Geometry mode bits (hi / lo), see G_MDSFT_* in gbi.h */
#define G_MW_MATRIX        0x00F
#define G_MW_TEXTURE       0x003
#define G_MW_FOG           0x015
#define G_MW_LIGHTING      0x017
#define G_MW_OTHERMODE     0x00D

#define G_MWO_MATRIX_XX_XY    0x000
#define G_MWO_MATRIX_ZX_ZY    0x004
#define G_MWO_MATRIX_YZ_WZ    0x00A
#define G_MWO_SEGMENT_SHIFT   28
#define G_MWO_LOAD_PROJ       0x0
#define G_MWO_LOAD_MODEL      0x1

#define G_DM_VERSION   0x00000240
#define G_DM_OFFSET    0x00000000
#define G_DM_SCALE     0x00000010
#define G_DM_CIL       0x00000001
#define G_DM_CAL       0x00000002
#define G_DM_FILLER    0xFFFFFFFF

/* Other-mode fields (lo word shifts) */
#define G_MDSFT_ALPHACOMPARE 0
#define G_MDSFT_ZSRCSEL      2
#define G_MDSFT_RENDERMODE   3

/* Other-mode fields (hi word shifts) */
#define G_MDSFT_BLENDMASK        0
#define G_MDSFT_ALPHADITHER      4
#define G_MDSFT_RGBDITHER        6
#define G_MDSFT_COMBKEY          8
#define G_MDSFT_TEXTLOD          9
#define G_MDSFT_TEXTDETAIL       10
#define G_MDSFT_TEXTPERSP        12
#define G_MDSFT_CYCLETYPE        14
#define G_MDSFT_COLORDB          16
#define G_MDSFT_PerspNormEn      17 /* AA_ENABLE / ZMODE-related legacy bit */
#define G_MDSFT_PIPELINE         23

/* Texture convert / key modes */
#define G_TX_KLRGB     0
#define G_TX_KLR       1
#define G_TX_KLG       2
#define G_TX_KLB       3
#define G_TX_KLA       4

/* Cycle types */
#define G_CYC_1CYCLE   0
#define G_CYC_2CYCLE   1
#define G_CYC_COPY     2
#define G_CYC_FILL     3

/* Textures */
#define G_IM_FMT_RGBA  0
#define G_IM_FMT_YUV   1
#define G_IM_FMT_CI    2
#define G_IM_FMT_IA    3
#define G_IM_FMT_I     4

#define G_IM_SIZ_4b    0
#define G_IM_SIZ_8b    1
#define G_IM_SIZ_16b   2
#define G_IM_SIZ_32b   3

#define G_TX_RENDERTILE 0
#define G_TX_NOMIRROR  0
#define G_TX_WRAP      0
#define G_TX_MIRROR    1
#define G_TX_CLAMP     2
#define G_TX_NOMASK    0

/* Combiner constants */
#define G_CC_ENVIRONMENT 0
#define G_CC_PRIIMITIVE  1
#define G_CC_SHADE       2
#define G_CC_TEXTURE0    3
#define G_CC_TEXTURE1    4
#define G_CC_LOD_FRACTION 5
#define G_CC_PRIM_COLOR   6
#define G_CC_KEY          7
#define G_CC_K4           8
#define G_CC_VIB           9
#define G_CC_VIC           10

/* Display-list opcodes we care about (upper 8 bits of w0) */
enum GBOpcode {
    GB_NOOP            = 0x00,
    GB_LOAD_UCODE      = 0x01, /* gSPLoadUcode* (RSP alias) */
    GB_CLEAR_VIEWPORT  = 0x02, /* gsSPViewportAlias-ish; not used */
    GB_VTX             = 0x04,
    GB_MODIFYVTX       = 0x05,
    GB_CULL_FACE       = 0x06,
    GB_MATRIX          = 0x07,
    GB_BRANCH_LIST     = 0x08,
    GB_ENDDL           = 0x09,
    GB_SET_GEOMETRY    = 0x0B, /* gSPSetGeometryMode / ClrGeometryMode */
    GB_TEXTURE         = 0x13,
    GB_SEGMENT         = 0x14,
    GB_SELECT_CLIP     = 0x15,
    GB_DISPTLIST       = 0x17, /* gSPDisplayList */
    GB_DL_CONTINUE     = 0x18,
    GB_SETOTHERMODE_H  = 0x23,
    GB_SETOTHERMODE_L  = 0x24,
    GB_RDP_SYNC        = 0x25,
    GB_SP_BUFFER       = 0x26,
    GB_1QB1            = 0x27,
    GB_MOVE_WMEM       = 0x28, /* gSPOssMem / ucode move */
    GB_SET_TILE_SIZE   = 0xB4,
    GB_SET_TILE        = 0xB5,
    GB_SET_FILL_COLOR  = 0xB6,
    GB_SET_RENDER_MODE = 0xBA,
    GB_SET_COMBINE     = 0xBB,
    GB_SET_TIMG        = 0xBC,
    GB_SET_CIMG        = 0xBD,
    GB_SET_SCISSOR     = 0xBE,
    GB_SET_PRIM_COLOR  = 0xBF,
    GB_SET_ENV_COLOR   = 0xBF, /* gsDPSetEnvColor shares slot via bits; handled by low opcode */
    GB_LOAD_BLOCK      = 0xF3,
    GB_LOAD_TILE       = 0xF4,
    GB_LOAD_TLUT       = 0xF5,
    GB_FILL_RECT       = 0xF7,
    GB_TRI1            = 0xEF,
    GB_TRI2            = 0xFB,
    GB_LINE3D          = 0xD8,
    GB_BG              = 0xE4, /* gsSPBgRectCopy / 1Cbr (s2dex/sd0-ish) */
    GB_OBJ_RECTANGLE   = 0xA3, /* s2dex: gsSPObjRectangle */
    GB_OBJ_SPRITE      = 0xA4, /* s2dex: gsSPObjSprite */
    GB_OBJ_LOAD_TLUT   = 0xB0,
    GB_OBJ_RENDERMODE  = 0xB1
};

/* ---------------------------------------------------------------------------
 * Portable vertex produced by the interpreter for the backends
 * ------------------------------------------------------------------------- */
typedef struct PcMatrix {
    float m[4][4];     /* row-major float conversion of the N64 s15.16 Mtx */
} PcMatrix;

typedef struct PcVertex {
    float pos[3];      /* post-matrix-transform clip-space input (world/view) */
    float color[4];    /* RGBA, normalized 0..1 (vertex shade color) */
    float uv[2];       /* texture coords (float, already scaled from fx10.2) */
    float light[4];    /* per-vertex lighting result (RGBA after lighting) */
    uint32_t flags;
} PcVertex;

#define PCV_FLAG_LIGHTEN   0x1u  /* geometry mode LIGHTEN enabled -> use light[] */
#define PCV_FLAG_CULLFACE  0x2u  /* culling state snapshot */

/* Pipeline state snapshot handed to the backend at draw time */
typedef struct PcDrawState {
    uint64_t othermode_hi;
    uint64_t othermode_lo;
    uint32_t geom_mode;
    uint32_t combine_a[4], combine_b[4], combine_c[4], combine_d[4];
    uint32_t tex_format, tex_size, tex_width;
    uintptr_t timg_addr;   /* segment-relative resolved address */
    uint32_t tlut_addr;
    uint32_t tex_tile;
    uint32_t mask_s, mirror_s, shift_s;
    uint32_t mask_t, mirror_t, shift_t;
    uint32_t prim_color;   /* packed RGBA8888 */
    uint32_t env_color;
    uint32_t fog_min, fog_max, fog_mode;
    float scissor[4];      /* l, t, r, b in pixels */
    float viewport[4];     /* scale_x, trans_x, scale_y, trans_y */
    int zsrc_sel;          /* 0 = interp z, 1 = max(z, fog) */
    int alpha_comp;
    int cycle_type;
    int texture_en;
    int depth_write;
    int depth_test;
    int lighting;
    int cull_face;         /* 0 none, 1 front, 2 back */
    PcMatrix *model_matrix;
    PcMatrix *proj_matrix;
} PcDrawState;

typedef struct PcMatrix PcMatrix;

/* ---------------------------------------------------------------------------
 * Backend interface: implemented once per API (D3D10 / D3D11 / Vulkan)
 * ------------------------------------------------------------------------- */
typedef struct PcTexInfo PcTexInfo;

typedef struct PcBackend {
    const char *name;      /* "Direct3D 11", "Vulkan", ... */

    int  (*init)(struct PcBackend *self, int width, int height, const char *title);
    void (*shutdown)(struct PcBackend *self);

    /* Frame lifecycle */
    void (*begin_frame)(struct PcBackend *self);
    void (*end_frame)(struct PcBackend *self);

    /* Resources referenced by display lists live in a virtual "RDRAM" flat
     * address space; backends upload textures sampled from it. */
    void (*bind_rdram)(struct PcBackend *self, const uint8_t *rdram, size_t size);

    /* State pushed by the interpreter */
    void (*set_viewport)(struct PcBackend *self, float lx, float ly, float sx, float sy);
    void (*set_scissor)(struct PcBackend *self, float l, float t, float r, float b);
    void (*set_othermode)(struct PcBackend *self, uint64_t hi, uint64_t lo);
    void (*set_geom_mode)(struct PcBackend *self, uint32_t mode);
    void (*set_combine)(struct PcBackend *self, const uint32_t a[4], const uint32_t b[4],
                        const uint32_t c[4], const uint32_t d[4]);
    void (*set_texture)(struct PcBackend *self, const PcTexInfo *tex);
    void (*set_prim_color)(struct PcBackend *self, uint32_t rgba8888);
    void (*set_env_color)(struct PcBackend *self, uint32_t rgba8888);
    void (*set_matrices)(struct PcBackend *self, const float model[16], const float proj[16]);

    /* Draws */
    void (*draw_triangles)(struct PcBackend *self, const PcVertex *v0, const PcVertex *v1,
                           const PcVertex *v2, const struct PcDrawState *state);
    void (*fill_rect)(struct PcBackend *self, float x0, float y0, float x1, float y1,
                      uint32_t color, const struct PcDrawState *state);
    void (*clear)(struct PcBackend *self, uint32_t color, float depth);
    /* current render-target size in pixels (0 if not initialized) */
    void (*get_size)(struct PcBackend *self, int *w, int *h);
} PcBackend;

struct PcTexInfo {
    uintptr_t addr;        /* resolved (segment-base added) RDRAM address */
    uint32_t width, height;
    uint32_t format;       /* G_IM_FMT_* */
    uint32_t size;         /* G_IM_SIZ_* */
    uint32_t tlut;         /* palette address for CI formats */
    uint32_t clamp_s, clamp_t, mask_s, mask_t, mirror_s, mirror_t, shift_s, shift_t;
};

/* ---------------------------------------------------------------------------
 * Interpreter public API
 * ------------------------------------------------------------------------- */
typedef struct GbiInterpreter GbiInterpreter;

GbiInterpreter *gbiCreate(PcBackend *backend, uint8_t *rdram, size_t rdram_size);
void            gbiDestroy(GbiInterpreter *gi);

/* Execute one display list (recurses into gSPDisplayList children, follows
 * branches/joins). `dl` points into the virtual RDRAM buffer. The list is
 * walked until gsSPEndDisplayList or `length_bytes` have been consumed,
 * whichever comes first; length_bytes = 0 means "unbounded scan" (stop at
 * the first ENDDL found in the buffer). */
void gbiRunDisplayList(GbiInterpreter *gi, const void *dl);
void gbiRunDisplayListN(GbiInterpreter *gi, const void *dl, size_t length_bytes);

/* Set the segment table base (as written by gSPSegment). */
void gbiSetSegment(GbiInterpreter *gi, int index, uint32_t base);

/* Resolve a segment-encoded address to a linear RDRAM pointer. */
void *gbiResolveAddr(GbiInterpreter *gi, uint32_t segaddr);

/* Query current accumulated state (for tests/debug overlays). */
const PcDrawState *gbiGetState(GbiInterpreter *gi);
PcBackend *gbiGetBackend(GbiInterpreter *gi);

#ifdef __cplusplus
}
#endif

#endif /* GBI_INTERPRETER_H */

/* Debug/test accessor: number of vertices currently held in the RSP vertex cache. */
uint32_t gbiVertexCacheCount(GbiInterpreter *gi);
