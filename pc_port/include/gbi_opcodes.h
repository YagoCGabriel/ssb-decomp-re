#ifndef GBI_OPCODES_H
#define GBI_OPCODES_H

/* F3DEX2 (F3DEX_GBI_2) opcode values, mirroring include/PR/gbi.h from the
 * decompilation project. SSB loads gspF3DEX2_fifo (see dSYTaskmanUcodes in
 * src/sys/taskman.c). */

/* RSP commands */
#define OP_SPNOOP        0xE0
#define OP_NOOP          0x00
#define OP_VTX           0x01
#define OP_MODIFYVTX     0x02
#define OP_CULLDL        0x03
#define OP_BRANCH_Z      0x04
#define OP_TRI1          0x05
#define OP_TRI2          0x06
#define OP_LINE3D        0x08
#define OP_SETOTHERMODE_H 0xE3
#define OP_SETOTHERMODE_L 0xE2
#define OP_RDPHALF_1     0xE1
#define OP_RDPHALF_2     0xF1
#define OP_ENDDL         0xDF
#define OP_DL            0xDE  /* gSPDisplayList (mode bit in w1 low bit = 0)
                                * gSPBranchList  (mode bit = 1) */
#define OP_LOAD_UCODE    0xDD
#define OP_MOVEMEM       0xDC
#define OP_MOVEWORD      0xDB
#define OP_MTX           0xDA
#define OP_GEOMETRYMODE  0xD9  /* set/clr distinguished by bit 8 of w0 */
#define OP_POPMTX        0xD8
#define OP_TEXTURE       0xD7
#define OP_DMA_IO        0xD6  /* gSPSegment / gSPClipRatio / gSPViewport-ish */
#define OP_SPECIAL_1     0xD5
#define OP_SPECIAL_2     0xD4
#define OP_SPECIAL_3     0xD3

/* Pure-DP command range: RDP packets have their opcode in the top byte and
 * live in the 0xC8..0xFF window. SSB's sSYTaskmanRdpResetDL and generated
 * triangle packets fall here. */
#define RDP_SETCIMG      0xFF
#define RDP_SETZIMG      0xFE
#define RDP_SETTIMG      0xFD
#define RDP_SETCOMBINE   0xFC
#define RDP_SETENVCOLOR  0xFB
#define RDP_SETPRIMCOLOR 0xFA
#define RDP_SETBLENDCOLOR 0xF9
#define RDP_SETFOGCOLOR  0xF8
#define RDP_SETFILLCOLOR 0xF7
#define RDP_FILLRECT     0xF6
#define RDP_SETTILE      0xF5
#define RDP_LOADTILE     0xF4
#define RDP_LOADBLOCK    0xF3
#define RDP_SETTILESIZE  0xF2
#define RDP_LOADTLUT     0xF0
#define RDP_RDPSETOTHERMODE 0xEF
#define RDP_SETPRIMDEPTH 0xEE
#define RDP_SETSCISSOR   0xED
#define RDP_SETCONVERT   0xEC
#define RDP_SETKEYR      0xEB
#define RDP_SETKEYGB     0xEA
#define RDP_FULLSYNC     0xE9
#define RDP_TILESYNC     0xE8
#define RDP_PIPESYNC     0xE7
#define RDP_LOADSYNC     0xE6
#define RDP_TEXRECTFLIP  0xE5
#define RDP_TEXRECT      0xE4
#define RDP_TRI_GEN_BASE 0xC8  /* generated edge/shade/tex/z tri packets C8..CF */
#define RDP_TRI_GEN_TOP  0xCF

#endif /* GBI_OPCODES_H */
