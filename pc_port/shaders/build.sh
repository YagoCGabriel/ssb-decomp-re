#!/bin/sh
# Compile GLSL -> SPIR-V and embed as C arrays for the Vulkan backend.
# Requires glslangValidator (Vulkan SDK) and xxd (or use spirv-embed.py).
set -e
cd "$(dirname "$0")"
for s in tri.vert tri.frag; do
    glslangValidator -V "$s" -o "${s}.spv"
done
python3 ./embed_spv.py tri_vert tri.vert.spv
python3 ./embed_spv.py tri_frag tri.frag.spv
echo "shaders embedded -> ../src/backend/shaders_spv.c"
