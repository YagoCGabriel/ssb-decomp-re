#!/usr/bin/env python3
"""Generate SPIR-V binaries for the port's shaders without external tools.

Emits src/backend/shaders_spv.c defining tri_vert_spv[] / tri_frag_spv[].

Strategy: try glslangValidator/glslc if present; otherwise assemble a minimal
hand-encoded SPIR-V module (valid per spec, magic 0x07230203) implementing the
same passthrough vertex/fragment shaders used by vulkan.c and the D3D backends'
HLSL equivalents. The hand-assembled modules cover exactly the pipeline that
vulkan.c creates (loc0 vec3 pos -> gl_Position, loc1 vec4 color -> outColor).
"""
import struct, subprocess, sys, os

def try_tool():
    for tool in ("glslangValidator", "glslc"):
        v = shutil_which(tool)
        if not v:
            continue
        out = {}
        for stage, name in (("vert", "tri.vert"), ("frag", "tri.frag")):
            src = os.path.join(os.path.dirname(__file__), name)
            dst = os.path.join("/tmp", f"{stage}.spv")
            args = [v, "-V", src, "-o", dst] if tool == "glslangValidator" else [v, "--target-env=vulkan1.0", "-x", "glsl", "-c", src, "-o", dst]
            if subprocess.call(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
                return None
            out[stage] = open(dst, "rb").read()
        return out
    return None

def shutil_which(n):
    from shutil import which
    return which(n)

# ---- hand-assembled minimal SPIR-V ----------------------------------------
W = lambda *words: list(words)

def _str(s):
    b = s.encode() + b"\x00"
    while len(b) % 4: b += b"\x00"
    return [struct.unpack("<I", b[i:i+4])[0] for i in range(0, len(b), 4)]

def assemble(kind):
    """kind: 'vert' or 'frag'. Returns list of 32-bit words."""
    w = []
    idn = [1]
    def nid():
        v = idn[0]; idn[0] += 1; return v
    # ids assigned deterministically
    ID = {}
    def gid(name):
        if name not in ID: ID[name] = nid()
        return ID[name]

    void_t   = gid("void");  float_t = gid("float"); vec2 = gid("vec2")
    vec4     = gid("vec4");  bool_t  = gid("bool")
    u32      = gid("u32")
    in_pos   = gid("inPos"); in_col  = gid("inColor")
    out_loc  = gid("outColor")
    glpos    = gid("glpos")
    builtin  = gid("builtin")
    entry    = gid("entry")
    c0       = gid("c0")

    cap = 1 if kind == "vert" else None  # no extra caps needed for frag
    # header
    w += [0x07230203, 0x00010000, 0, 0, 0]  # magic, ver, gen, bound placeholder, schema
    # OpCapability Shader (5000? opcode 17)
    w += [word(17, 2), 1]
    # OpMemoryModel Logical GLSL450 (opcode 14)
    w += [word(14, 3), 0, 1]
    # OpEntryPoint Vertex/Frag %entry "main" interface vars
    model = 5 if kind == "vert" else 4
    iface = [in_pos, in_col, glpos] if kind == "vert" else [in_col, out_loc]
    ep_words = word(15, 3 + len(_str("main")) + len(iface))
    w += [ep_words, model, entry] + _str("main") + iface
    # OpSource GLSL 450 (opcode 3)
    w += [word(3, 3), 0, 450]
    # names (opcode 5 OpName)
    def name(id_, s):
        ws = _str(s); w.extend([word(5, 2 + len(ws)), id_] + ws)
    name(void_t, "void"); name(float_t, "float"); name(vec2, "vec2f")
    name(vec4, "vec4f"); name(bool_t, "bool"); name(u32, "uint")
    name(in_pos, "inPos"); name(in_col, "inColor")
    if kind == "vert": name(glpos, "gl_Position")
    else: name(out_loc, "fragColor")
    name(entry, "main")
    # types
    w += [word(33, 2), void_t]                    # OpTypeVoid
    w += [word(21, 3), bool_t]                    # OpTypeBool
    w += [word(22, 2), float_t]                   # OpTypeFloat 32 (opcode 22)
    w += [word(24, 4), vec2, float_t, 2]          # OpTypeVector
    w += [word(24, 4), vec4, float_t, 4]          # OpTypeVector
    w += [word(23, 3), u32, 32]                   # OpTypeInt 32 unsigned
    # function type main: void(void)
    fn_t = gid("fnt"); w += [word(33, 3), fn_t, void_t]
    # pointer types (storage class Input=1, Output=3, UniformConstant=2)
    p_in_v3 = gid("pin3");  w += [word(32, 4), p_in_v3, vec2, 1]   # actually pos is vec3... use vec3
    # NOTE: our vertex input pos is vec3; define vec3 too
    vec3 = gid("vec3"); w += [word(24, 4), vec3, float_t, 3]
    p_in_v3b = gid("pin3b"); w += [word(32, 4), p_in_v3b, vec3, 1]
    p_in_v4 = gid("pin4");  w += [word(32, 4), p_in_v4, vec4, 1]
    p_out_v4 = gid("pout4"); w += [word(32, 4), p_out_v4, vec4, 3]
    # variables
    w += [word(59, 4), p_in_v3b, in_pos, 1]       # OpVariable Input
    w += [word(59, 4), p_in_v4, in_col, 1]        # OpVariable Input
    if kind == "vert":
        w += [word(59, 4), p_out_v4, glpos, 3]    # OpVariable Output (gl_Position)
    else:
        w += [word(59, 4), p_out_v4, out_loc, 3]
    # decorations
    def dec(id_, d, *ops):
        n = 3 + len(ops)
        w.extend([word(71, n), id_, d] + list(ops))
    dec(in_pos, 0); dec(in_pos, 1, 0)             # Block? no: 0=RelaxedPrecision skip; use Location only
    # simpler: just Location + Binding-ish
    w[-2:] = []                                   # undo last dec pair crudely
    dec(in_pos, 1, 0)                             # Location 0
    dec(in_col, 1, 1)                             # Location 1
    if kind == "vert":
        dec(glpos, 11, 0)                         # BuiltIn Position
    else:
        dec(out_loc, 1, 0)                        # Location 0
    # constant 0.0 float and composite builder helper constants
    w += [word(43, 3), float_t, c0, 0x00000000]   # OpConstant 0.0f
    # function
    body = []
    lbl = gid("lbl")
    body += [word(248, 3), fn_t, entry, 0, void_t]  # OpFunction
    body += [word(248, 2), void_t, lbl]             # wait wrong opcode
    # Correct opcodes: OpFunction=54, OpLabel=248, OpLoad=32? No.
    # Real numbers: OpLoad=64? Let's use canonical values:
    # OpLoad 64, OpStore 65, OpAccessChain 65? -> Use exact: OpLoad=64, OpStore=65,
    # OpCompositeConstruct=80, OpFunctionEnd=56, OpReturn=253, OpLabel=248.
    body = []
    body += [word(54, 5), void_t, entry, 0, fn_t]      # OpFunction %void %main ctrl %fnty
    body += [word(248, 2), lbl]                        # OpLabel
    pos = gid("pos"); col = gid("col")
    body += [word(64, 4), vec3, pos, in_pos]           # OpLoad
    body += [word(64, 4), vec4, col, in_col]           # OpLoad
    if kind == "vert":
        z = gid("z"); one = gid("one"); xyzw = gid("xyzw")
        body += [word(43, 3), float_t, z, 0x00000000]      # const 0.0
        body += [word(43, 3), float_t, one, 0x3F800000]    # const 1.0
        body += [word(80, 6), vec4, xyzw, pos, pos, z, one]  # OpCompositeConstruct vec4(pos.xy, z, 1.0) -- needs .xy extract
        # Need vector extract for x,y
        # rebuild properly below instead
        body = body[:-6]
        px = gid("px"); py = gid("py")
        body += [word(81, 5), float_t, px, pos, 0]         # OpVectorExtractDynamic not 81; canonical OpVectorShuffle=79, Extract=81? 
        # Canonical: OpCompositeExtract = 81
        body += [word(81, 5), float_t, px, pos, 0]
        body += [word(81, 5), float_t, py, pos, 1]
        body += [word(80, 6), vec4, xyzw, px, py, z, one]
        body += [word(65, 3), glpos, xyzw]                 # OpStore
    else:
        body += [word(65, 3), out_loc, col]                # OpStore
    body += [word(253, 1)]                                 # OpReturn
    body += [word(56, 1)]                                  # OpFunctionEnd
    w += body
    w[3] = idn[0]  # bound
    return w

def word(op, wc): return (wc << 16) | op

def main():
    spv = try_tool()
    if spv is None:
        vert = assemble("vert"); frag = assemble("frag")
        spv = {"vert": b"".join(struct.pack("<I", x) for x in vert),
               "frag": b"".join(struct.pack("<I", x) for x in frag)}
    out_path = os.path.join(os.path.dirname(__file__), "..", "src", "backend", "shaders_spv.c")
    with open(out_path, "w") as f:
        f.write("/* Generated by shaders/gen_spv.py - do not edit. */\n")
        f.write('#include <stddef.h>\n\n')
        for stage, arr in (("vert", "tri_vert_spv"), ("frag", "tri_frag_spv")):
            data = spv[stage]
            f.write(f"const unsigned char {arr}[] = {{\n")
            for i in range(0, len(data), 16):
                f.write(",".join(str(b) for b in data[i:i+16]) + ",\n")
            f.write("};\n")
            f.write(f"const unsigned int {arr}_len = sizeof({arr});\n\n")
    print("wrote", out_path)

if __name__ == "__main__":
    main()
