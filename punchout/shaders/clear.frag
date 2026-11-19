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
} pc;
void main() { ocol0 = pc.color; }
