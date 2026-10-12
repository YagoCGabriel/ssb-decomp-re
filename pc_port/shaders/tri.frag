/* ssb-decomp PC Port - passthrough colored-triangle fragment shader */
#version 450
layout(location = 0) in vec4 outColor;
layout(location = 0) out vec4 fragColor;
void main(void)
{
    fragColor = outColor;
}
