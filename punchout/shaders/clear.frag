#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 ocol0;
layout(push_constant) uniform Push {
    vec4 src;
    vec4 color;
    float depth;
    uint format;
    uint flags;
    float gamma;
    uvec3 weights;      // copies: the copy filter's weights (above, the line, below)
    float clamp_top, clamp_bottom, line;
} pc;
void main() { ocol0 = pc.color; }
