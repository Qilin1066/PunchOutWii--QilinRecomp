// One triangle over the target; the rectangle comes from the viewport.
#version 450
layout(location = 0) out vec2 uv;
layout(push_constant) uniform Push {
    vec4 src;      // copy: source rectangle in the EFB, normalised (x, y, w, h)
    vec4 color;    // clear: the colour
    float depth;   // clear: the depth
    uint format;   // copy: the texture format (with 0x100 for intensity), or 0xFFFF: frame buffer
    uint flags;    // copy: 1 half scale (box filter)
    float gamma;
} pc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = pc.src.xy + p * pc.src.zw;
    gl_Position = vec4(p * 2.0 - 1.0, pc.depth, 1.0);
}
