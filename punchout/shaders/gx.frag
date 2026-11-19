// GX's texture environment (TEV): up to 16 stages of the colour and alpha
// combiners over the rasterised colours, textures (direct and indirect) and
// konstants, then the alpha test and fog, as Dolphin's pixel ubershader
// computes them (UberShaderPixel.cpp) in TEV's own integer arithmetic.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "state.glsl"

layout(location = 0) in vec4 colors_0;
layout(location = 1) in vec4 colors_1;
layout(location = 2) in vec3 tex[8];
layout(set = 1, binding = 0) uniform sampler2D samp[8];
layout(location = 0) out vec4 ocol0;

int iround(float x) { return int(round(x)); }
ivec4 iround(vec4 x) { return ivec4(round(x)); }

ivec4 sample_tex(uint map, ivec2 uv) {
    vec2 size = vec2(S.texdims[map].xy * 128);
    vec2 coords = vec2(uv) / size;
    uint mode0 = bp((map < 4u ? 0x80u : 0xA0u) + (map & 3u));
    float lod_bias = float(int(bits(mode0, 9u, 8u) << 24) >> 24) / 32.0;
    return iround(255.0 * texture(samp[map], coords, lod_bias));
}

ivec2 tex_coord(uint i) {
    vec3 t = tex[i];
    vec2 c = t.z == 0.0 ? t.xy : t.xy / t.z;
    return ivec2(c * vec2(S.texdims[i].zw * 128));
}

ivec4 swizzle(uint s, ivec4 c) {
    uint lo = bp(0xF6u + s * 2u), hi = bp(0xF6u + s * 2u + 1u);
    return ivec4(c[bits(lo, 0u, 2u)], c[bits(lo, 2u, 2u)], c[bits(hi, 0u, 2u)], c[bits(hi, 2u, 2u)]);
}

int wrap(int coord, uint mode) {
    if (mode == 0u) return coord;
    if (mode < 6u) return coord & (0xfffe >> mode);
    return 0;
}

int tev_lerp(int A, int B, int C, int D, uint bias, bool op, uint scale) {
    C += C >> 7;
    if (bias == 1u) D += 128;
    else if (bias == 2u) D -= 128;
    int lerp = (A << 8) + (B - A) * C;
    if (scale != 3u) {
        lerp = lerp << scale;
        D = D << scale;
        lerp = lerp + (op ? 127 : 128);
    }
    int result = lerp >> 8;
    result = op ? D - result : D + result;
    if (scale == 3u) result = result >> 1;
    return result;
}
ivec3 tev_lerp3(ivec3 A, ivec3 B, ivec3 C, ivec3 D, uint bias, bool op, uint scale) {
    return ivec3(tev_lerp(A.r, B.r, C.r, D.r, bias, op, scale), tev_lerp(A.g, B.g, C.g, D.g, bias, op, scale),
                 tev_lerp(A.b, B.b, C.b, D.b, bias, op, scale));
}

bool tev_compare(uint op, ivec3 a, ivec3 b) {
    switch (op) {
    case 0u: return a.r > b.r;
    case 1u: return a.r == b.r;
    case 2u: return (a.r | (a.g << 8)) > (b.r | (b.g << 8));
    case 3u: return a.r == b.r && a.g == b.g;
    case 4u: return (a.r | (a.g << 8) | (a.b << 16)) > (b.r | (b.g << 8) | (b.b << 16));
    case 5u: return a.r == b.r && a.g == b.g && a.b == b.b;
    default: return false;
    }
}

bool alpha_compare(int a, int b, uint cmp) {
    switch (cmp) {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    default: return true;
    }
}

const int kFractions[8] = int[8](255, 223, 191, 159, 128, 96, 64, 32);  // 1, 7/8 ... 1/8
ivec4 konst_color(uint sel) {
    if (sel < 8u) return ivec4(kFractions[sel]);
    if (sel < 12u) return ivec4(0);
    if (sel < 16u) return S.konst[sel - 12u];
    return ivec4(S.konst[(sel - 16u) & 3u][(sel - 16u) >> 2]);  // K0-3 _R, _G, _B, _A
}
int konst_alpha(uint sel) {
    if (sel < 8u) return kFractions[sel];
    if (sel < 16u) return 0;
    return S.konst[(sel - 16u) & 3u][(sel - 16u) >> 2];
}

ivec4 Reg[4];
ivec4 TexColor, RawTexColor;
int AlphaBump;

ivec4 ras_color(uint order, uint ac) {
    uint ras = bits(order, 7u, 3u);
    if (ras < 2u) return swizzle(bits(ac, 0u, 2u), iround((ras == 0u ? colors_0 : colors_1) * 255.0));
    if (ras == 5u) return ivec4(AlphaBump);
    if (ras == 6u) return ivec4(AlphaBump | (AlphaBump >> 5));
    return ivec4(0);
}

ivec3 color_input(uint i, uint order, uint ac, ivec4 k) {
    switch (i) {
    case 0u: return Reg[0].rgb;
    case 1u: return Reg[0].aaa;
    case 2u: return Reg[1].rgb;
    case 3u: return Reg[1].aaa;
    case 4u: return Reg[2].rgb;
    case 5u: return Reg[2].aaa;
    case 6u: return Reg[3].rgb;
    case 7u: return Reg[3].aaa;
    case 8u: return TexColor.rgb;
    case 9u: return TexColor.aaa;
    case 10u: return ras_color(order, ac).rgb;
    case 11u: return ras_color(order, ac).aaa;
    case 12u: return ivec3(255);
    case 13u: return ivec3(128);
    case 14u: return k.rgb;
    default: return ivec3(0);
    }
}
int alpha_input(uint i, uint order, uint ac, ivec4 k) {
    switch (i) {
    case 0u: return Reg[0].a;
    case 1u: return Reg[1].a;
    case 2u: return Reg[2].a;
    case 3u: return Reg[3].a;
    case 4u: return TexColor.a;
    case 5u: return ras_color(order, ac).a;
    case 6u: return k.a;
    default: return 0;
    }
}

void main() {
    uint genmode = bp(0x00u);
    uint num_stages = bits(genmode, 10u, 4u);
    for (int i = 0; i < 4; i++) Reg[i] = S.tev_regs[i];
    TexColor = ivec4(0);
    RawTexColor = ivec4(0);
    AlphaBump = 0;
    ivec3 tevcoord = ivec3(0);

    for (uint stage = 0u; stage <= num_stages; stage++) {
        uint cc = bp(0xC0u + stage * 2u), ac = bp(0xC1u + stage * 2u);
        uint order = bp(0x28u + (stage >> 1));
        if ((stage & 1u) == 1u) order = order >> 12;
        ivec2 fixedPoint_uv = tex_coord(bits(order, 3u, 3u));
        bool texture_enabled = (order & 64u) != 0u;

        // Indirect texturing.
        uint tevind = bp(0x10u + stage);
        if (tevind != 0u) {
            uint bs = bits(tevind, 7u, 2u), fmt = bits(tevind, 2u, 2u), bias = bits(tevind, 4u, 3u), bt = bits(tevind, 0u, 2u);
            uint matrix_index = bits(tevind, 9u, 2u), matrix_id = bits(tevind, 11u, 2u);
            ivec2 indtevtrans = ivec2(0);
            uint iref = bp(0x27u);
            uint ind_coord = bits(iref, bt * 6u + 3u, 3u), ind_map = bits(iref, bt * 6u, 3u);
            if (bt < bits(genmode, 16u, 3u)) {
                ivec2 uv = tex_coord(ind_coord);
                uint scale = bp(0x25u + (bt >> 1));
                if ((bt & 1u) != 0u) scale = scale >> 8;
                uv = uv >> ivec2(int(bits(scale, 0u, 4u)), int(bits(scale, 4u, 4u)));
                ivec3 indcoord = sample_tex(ind_map, uv).abg;
                if (bs != 0u) AlphaBump = indcoord[bs - 1u];
                switch (fmt) {
                case 0u:
                    indcoord.x += (bias & 1u) != 0u ? -128 : 0;
                    indcoord.y += (bias & 2u) != 0u ? -128 : 0;
                    indcoord.z += (bias & 4u) != 0u ? -128 : 0;
                    AlphaBump = AlphaBump & 0xf8;
                    break;
                case 1u:
                    indcoord = (indcoord >> 3) + ivec3((bias & 1u) != 0u ? 1 : 0, (bias & 2u) != 0u ? 1 : 0, (bias & 4u) != 0u ? 1 : 0);
                    AlphaBump = AlphaBump << 5;
                    break;
                case 2u:
                    indcoord = (indcoord >> 4) + ivec3((bias & 1u) != 0u ? 1 : 0, (bias & 2u) != 0u ? 1 : 0, (bias & 4u) != 0u ? 1 : 0);
                    AlphaBump = AlphaBump << 4;
                    break;
                default:
                    indcoord = (indcoord >> 5) + ivec3((bias & 1u) != 0u ? 1 : 0, (bias & 2u) != 0u ? 1 : 0, (bias & 4u) != 0u ? 1 : 0);
                    AlphaBump = AlphaBump << 3;
                    break;
                }
                if (matrix_index != 0u) {
                    uint m = 2u * (matrix_index - 1u);
                    int shift = S.indmtx[m].w;
                    if (matrix_id == 0u) {
                        ivec3 r0 = S.indmtx[m].xyz, r1 = S.indmtx[m + 1u].xyz;
                        indtevtrans = ivec2(r0.x * indcoord.x + r0.y * indcoord.y + r0.z * indcoord.z,
                                            r1.x * indcoord.x + r1.y * indcoord.y + r1.z * indcoord.z) >> 3;
                    } else if (matrix_id == 1u) {
                        indtevtrans = (fixedPoint_uv * indcoord.xx) >> 8;
                    } else if (matrix_id == 2u) {
                        indtevtrans = (fixedPoint_uv * indcoord.yy) >> 8;
                    }
                    if (shift >= 0) indtevtrans = indtevtrans >> shift;
                    else indtevtrans = indtevtrans << ((-shift) & 31);
                }
            }
            ivec2 wrapped = ivec2(wrap(fixedPoint_uv.x, bits(tevind, 13u, 3u)), wrap(fixedPoint_uv.y, bits(tevind, 16u, 3u)));
            if ((tevind & (1u << 20)) != 0u) tevcoord.xy += wrapped + indtevtrans;
            else tevcoord.xy = wrapped + indtevtrans;
            tevcoord.xy = (tevcoord.xy << 8) >> 8;
        } else {
            tevcoord.xy = fixedPoint_uv;
        }

        if (texture_enabled) {
            RawTexColor = sample_tex(bits(order, 0u, 3u), tevcoord.xy);
            TexColor = swizzle(bits(ac, 2u, 2u), RawTexColor);
        } else {
            TexColor = ivec4(255);
        }

        uint ksel = bp(0xF6u + (stage >> 1));
        uint kc = (stage & 1u) == 0u ? bits(ksel, 4u, 5u) : bits(ksel, 14u, 5u);
        uint ka = (stage & 1u) == 0u ? bits(ksel, 9u, 5u) : bits(ksel, 19u, 5u);
        ivec4 k = ivec4(konst_color(kc).rgb, konst_alpha(ka));

        // Colour combiner.
        uint c_bias = bits(cc, 16u, 2u), c_scale = bits(cc, 20u, 2u), c_dest = bits(cc, 22u, 2u);
        bool c_op = bits(cc, 18u, 1u) != 0u, c_clamp = bits(cc, 19u, 1u) != 0u;
        ivec3 cA = color_input(bits(cc, 12u, 4u), order, ac, k) & 255;
        ivec3 cB = color_input(bits(cc, 8u, 4u), order, ac, k) & 255;
        ivec3 cC = color_input(bits(cc, 4u, 4u), order, ac, k) & 255;
        ivec3 cD = color_input(bits(cc, 0u, 4u), order, ac, k);
        ivec3 color;
        if (c_bias != 3u) {
            color = tev_lerp3(cA, cB, cC, cD, c_bias, c_op, c_scale);
        } else {
            uint cmp = (c_scale << 1) | uint(c_op);
            if (cmp == 6u) color = ivec3(cA.r > cB.r ? cC.r : 0, cA.g > cB.g ? cC.g : 0, cA.b > cB.b ? cC.b : 0);
            else if (cmp == 7u) color = ivec3(cA.r == cB.r ? cC.r : 0, cA.g == cB.g ? cC.g : 0, cA.b == cB.b ? cC.b : 0);
            else color = tev_compare(cmp, cA, cB) ? cC : ivec3(0);
            color = cD + color;
        }
        color = c_clamp ? clamp(color, 0, 255) : clamp(color, -1024, 1023);

        // Alpha combiner (reads the colour inputs as they were).
        uint a_bias = bits(ac, 16u, 2u), a_scale = bits(ac, 20u, 2u), a_dest = bits(ac, 22u, 2u);
        bool a_op = bits(ac, 18u, 1u) != 0u, a_clamp = bits(ac, 19u, 1u) != 0u;
        int aA = alpha_input(bits(ac, 13u, 3u), order, ac, k) & 255;
        int aB = alpha_input(bits(ac, 10u, 3u), order, ac, k) & 255;
        int aC = alpha_input(bits(ac, 7u, 3u), order, ac, k) & 255;
        int aD = alpha_input(bits(ac, 4u, 3u), order, ac, k);
        int alpha;
        if (a_bias != 3u) {
            alpha = tev_lerp(aA, aB, aC, aD, a_bias, a_op, a_scale);
        } else {
            uint cmp = (a_scale << 1) | uint(a_op);
            if (cmp == 6u) alpha = aA > aB ? aC : 0;
            else if (cmp == 7u) alpha = aA == aB ? aC : 0;
            else alpha = tev_compare(cmp, cA, cB) ? aC : 0;
            alpha = aD + alpha;
        }
        alpha = a_clamp ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);

        Reg[c_dest].rgb = color;
        Reg[a_dest].a = alpha;
    }

    uint last_cc = bp(0xC0u + num_stages * 2u), last_ac = bp(0xC1u + num_stages * 2u);
    ivec4 TevResult = ivec4(Reg[bits(last_cc, 22u, 2u)].rgb, Reg[bits(last_ac, 22u, 2u)].a) & 255;

    int zCoord = clamp(int(gl_FragCoord.z * 16777216.0), 0, 0xFFFFFF);

    // Alpha test.
    uint at = bp(0xF3u);
    bool comp0 = alpha_compare(TevResult.a, int(bits(at, 0u, 8u)), bits(at, 16u, 3u));
    bool comp1 = alpha_compare(TevResult.a, int(bits(at, 8u, 8u)), bits(at, 19u, 3u));
    switch (bits(at, 22u, 2u)) {
    case 0u: if (!(comp0 && comp1)) discard; break;
    case 1u: if (!(comp0 || comp1)) discard; break;
    case 2u: if (comp0 == comp1) discard; break;
    default: if (comp0 != comp1) discard; break;
    }
    if (TevResult.a == 1) TevResult.a = 0;  // passes the test, does nothing in blending

    bool rgba6 = bits(bp(0x43u), 0u, 3u) == 1u;
    if (rgba6 && bits(bp(0x41u), 2u, 1u) != 0u) {  // dither, 2x2 Bayer
        ivec2 d = ivec2(gl_FragCoord.xy / pc.scale) & 1;
        TevResult.rgb = (TevResult.rgb - (TevResult.rgb >> 6)) + (d.x ^ d.y) * 2 + d.y;
    }

    // Fog.
    uint f3 = bp(0xF1u);
    uint fog_function = bits(f3, 21u, 3u);
    if (fog_function != 0u) {
        float ze;
        if (bits(f3, 20u, 1u) == 0u)
            ze = (S.fog[0].x * 16777216.0) / float(S.misc[0].x - (zCoord >> S.misc[0].y));
        else
            ze = S.fog[0].x * float(zCoord) / 16777216.0;
        if (bits(bp(0xE8u), 10u, 1u) != 0u) {
            float offset = (2.0 * ((gl_FragCoord.x / pc.scale) / S.fog[0].w)) - 1.0 - S.fog[0].z;
            float floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
            uint lo = uint(floatindex), hi = lo + 1u;
            float k = mix(S.fog_range[lo >> 2u][lo & 3u], S.fog_range[hi >> 2u][hi & 3u], fract(floatindex));
            ze *= sqrt(offset * offset + k * k) / k;
        }
        float fog = clamp(ze - S.fog[0].y, 0.0, 1.0);
        if (fog_function == 4u) fog = 1.0 - exp2(-8.0 * fog);
        else if (fog_function == 5u) fog = 1.0 - exp2(-8.0 * fog * fog);
        else if (fog_function == 6u) fog = exp2(-8.0 * (1.0 - fog));
        else if (fog_function == 7u) { fog = 1.0 - fog; fog = exp2(-8.0 * fog * fog); }
        int ifog = iround(fog * 256.0);
        TevResult.rgb = (TevResult.rgb * (256 - ifog) + S.misc[1].rgb * ifog) >> 8;
    }

    if (rgba6) ocol0.rgb = vec3(TevResult.rgb >> 2) / 63.0;
    else ocol0.rgb = vec3(TevResult.rgb) / 255.0;
    uint dstalpha = bp(0x42u);
    if ((dstalpha & 0x100u) != 0u && (pc.flags & 1u) == 0u) ocol0.a = float(bits(dstalpha, 0u, 8u) >> 2) / 63.0;
    else ocol0.a = rgba6 ? float(TevResult.a >> 2) / 63.0 : float(TevResult.a) / 255.0;
}
