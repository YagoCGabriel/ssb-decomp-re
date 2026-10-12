/* ssb-decomp PC Port - triangle vertex shader (Vulkan/GLSL) */
#version 450
layout(location = 0) in vec3 inPos;   /* render-target pixel coords, z in [0,1] */
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;
void main(void)
{
    /* Vulkan NDC: +Y down, z in [0,1]; x/y from pixels to [-1,1] done on CPU */
    gl_Position = vec4(inPos.x, inPos.y, inPos.z, 1.0);
    outColor = inColor;
}
