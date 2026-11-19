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
    uint flags;  // 1: constant alpha can't be written (the blend reads source alpha);
                 // bits 4-5: a logic op's help (1 output 0, 2 output 1, 3 invert the output)
    float scale; // EFB pixels to target pixels
    float point_size;  // points: in target pixels
} pc;

#define S st[pc.state]
uint bp_dyn(uint i) { return S.bp[i >> 2][i & 3u]; }
#ifndef BP_SPECIALIZED  // gx.frag reads the shape registers from specialisation constants
uint bp(uint i) { return bp_dyn(i); }
#endif
uint bits(uint v, uint at, uint n) { return bitfieldExtract(v, int(at), int(n)); }
