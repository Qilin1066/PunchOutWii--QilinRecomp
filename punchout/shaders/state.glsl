// The per-draw GX state (gx.hpp's State: keep the two in step).
struct Light {
    vec4 color, cosatt, distatt, pos, dir;
};
struct State {
    vec4 posmtx[64];
    vec4 nrmmtx[32];
    vec4 postmtx[64];
    Light lights[8];
    vec4 projection[4];
    vec4 pixel_center;
    ivec4 materials[4];
    uvec4 xf[8];
    uvec4 bp[64];
    ivec4 texdims[8];
    ivec4 tev_regs[4];
    ivec4 konst[4];
    ivec4 indmtx[6];
    ivec4 misc[4];
    vec4 fog[2];
    vec4 fog_range[3];
};
layout(std430, set = 0, binding = 0) readonly buffer States {
    State st[];
};
layout(push_constant) uniform Push {
    uint state;
    uint has;    // gx::Has
    uint flags;  // 1: constant alpha can't be written (the blend reads source alpha)
    float scale; // EFB pixels to target pixels
} pc;

#define S st[pc.state]
uint bp(uint i) { return S.bp[i >> 2][i & 3u]; }
uint bits(uint v, uint at, uint n) { return bitfieldExtract(v, int(at), int(n)); }
