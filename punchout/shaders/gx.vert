// GX's transform unit: position and normal matrices, the projection, the
// two lighting channels and texture coordinate generation, as Dolphin's
// vertex ubershader computes them (UberShaderVertex.cpp, UberShaderCommon.cpp).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "state.glsl"

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_tangent;
layout(location = 3) in vec3 a_binormal;
layout(location = 4) in vec4 a_color0;
layout(location = 5) in vec4 a_color1;
layout(location = 6) in vec2 a_uv[8];
layout(location = 14) in uvec3 a_mtx;

layout(location = 0) out vec4 colors_0;
layout(location = 1) out vec4 colors_1;
layout(location = 2) out vec3 tex[8];

const uint HAS_COL0 = 16u;

uint mtx_index(uint i) {  // 0: position, 1-8: texture matrices
    return (a_mtx[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu;
}

ivec4 lighting(uint index, uint attnfunc, uint diffusefunc, vec3 pos, vec3 normal) {
    vec3 ldir, cosAttn, distAttn;
    float dist, dist2, attn;
    Light L = S.lights[index];
    switch (attnfunc) {
    case 0u:  // none
    case 2u:  // directional
        ldir = normalize(L.pos.xyz - pos);
        attn = 1.0;
        if (length(ldir) == 0.0) ldir = normal;
        break;
    case 1u:  // specular
        ldir = normalize(L.pos.xyz - pos);
        attn = (dot(normal, ldir) >= 0.0) ? max(0.0, dot(normal, L.dir.xyz)) : 0.0;
        cosAttn = L.cosatt.xyz;
        distAttn = diffusefunc == 0u ? L.distatt.xyz : normalize(L.distatt.xyz);
        attn = max(0.0, dot(cosAttn, vec3(1.0, attn, attn * attn))) / dot(distAttn, vec3(1.0, attn, attn * attn));
        break;
    case 3u:  // spot
        ldir = L.pos.xyz - pos;
        dist2 = dot(ldir, ldir);
        dist = sqrt(dist2);
        ldir = ldir / dist;
        attn = max(0.0, dot(ldir, L.dir.xyz));
        attn = max(0.0, L.cosatt.x + L.cosatt.y * attn + L.cosatt.z * attn * attn) / dot(L.distatt.xyz, vec3(1.0, dist, dist2));
        break;
    default:
        attn = 1.0;
        ldir = normal;
        break;
    }
    switch (diffusefunc) {
    case 0u: return ivec4(round(attn * L.color));
    case 1u: return ivec4(round(attn * dot(ldir, normal) * L.color));
    case 2u: return ivec4(round(attn * max(0.0, dot(ldir, normal)) * L.color));
    default: return ivec4(0);
    }
}

void main() {
    vec4 rawpos = vec4(a_pos, 1.0);
    uint posidx = mtx_index(0u);
    vec4 P0 = S.posmtx[posidx], P1 = S.posmtx[posidx + 1u], P2 = S.posmtx[posidx + 2u];
    uint normidx = posidx >= 32u ? posidx - 32u : posidx;
    vec3 N0 = S.nrmmtx[normidx].xyz, N1 = S.nrmmtx[(normidx + 1u) & 31u].xyz, N2 = S.nrmmtx[(normidx + 2u) & 31u].xyz;

    vec4 pos = vec4(dot(P0, rawpos), dot(P1, rawpos), dot(P2, rawpos), 1.0);
    vec4 o_pos = vec4(dot(S.projection[0], pos), dot(S.projection[1], pos), dot(S.projection[2], pos), dot(S.projection[3], pos));

    vec3 _normal = normalize(vec3(dot(N0, a_normal), dot(N1, a_normal), dot(N2, a_normal)));
    vec3 _tangent = vec3(dot(N0, a_tangent), dot(N1, a_tangent), dot(N2, a_tangent));
    vec3 _binormal = vec3(dot(N0, a_binormal), dot(N1, a_binormal), dot(N2, a_binormal));

    // Lighting: two channels, colour and alpha each with its own control.
    vec4 vertex_color[2] = vec4[2](a_color0, a_color1);
    vec4 lit[2];
    for (uint chan = 0u; chan < 2u; chan++) {
        uint colorreg = chan == 0u ? S.xf[0].y : S.xf[0].z;
        uint alphareg = chan == 0u ? S.xf[0].w : S.xf[1].x;
        ivec4 mat = S.materials[chan + 2u];
        ivec4 lacc = ivec4(255);
        if (bits(colorreg, 0u, 1u) != 0u) mat.xyz = ivec3(round(vertex_color[chan].xyz * 255.0));
        if (bits(alphareg, 0u, 1u) != 0u) mat.w = int(round(vertex_color[chan].w * 255.0));
        else mat.w = S.materials[chan + 2u].w;
        if (bits(colorreg, 1u, 1u) != 0u) {
            if (bits(colorreg, 6u, 1u) != 0u) lacc.xyz = ivec3(round(vertex_color[chan].xyz * 255.0));
            else lacc.xyz = S.materials[chan].xyz;
            uint mask = bits(colorreg, 2u, 4u) | (bits(colorreg, 11u, 4u) << 4u);
            uint attn = bits(colorreg, 9u, 2u), diffuse = bits(colorreg, 7u, 2u);
            for (uint l = 0u; l < 8u; l++)
                if ((mask & (1u << l)) != 0u) lacc.xyz += lighting(l, attn, diffuse, pos.xyz, _normal).xyz;
        }
        if (bits(alphareg, 1u, 1u) != 0u) {
            if (bits(alphareg, 6u, 1u) != 0u) {
                if ((pc.has & (HAS_COL0 << chan)) != 0u) lacc.w = int(round(vertex_color[chan].w * 255.0));
                else if ((pc.has & HAS_COL0) != 0u) lacc.w = int(round(vertex_color[0].w * 255.0));
                else lacc.w = 255;
            } else {
                lacc.w = S.materials[chan].w;
            }
            uint mask = bits(alphareg, 2u, 4u) | (bits(alphareg, 11u, 4u) << 4u);
            uint attn = bits(alphareg, 9u, 2u), diffuse = bits(alphareg, 7u, 2u);
            for (uint l = 0u; l < 8u; l++)
                if ((mask & (1u << l)) != 0u) lacc.w += lighting(l, attn, diffuse, pos.xyz, _normal).w;
        }
        lacc = clamp(lacc, 0, 255);
        lit[chan] = vec4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;
    }
    vec4 o_colors_0 = lit[0], o_colors_1 = lit[1];

    // Texture coordinate generation.
    vec3 o_tex[8];
    for (uint i = 0u; i < 8u; i++) o_tex[i] = vec3(0.0);
    uint num_texgen = S.xf[1].z;
    for (uint texgen = 0u; texgen < num_texgen; texgen++) {
        uint info = S.xf[2u + (texgen >> 2)][texgen & 3u];
        vec4 coord = vec4(0.0, 0.0, 1.0, 1.0);
        uint source = bits(info, 7u, 5u);
        if (source == 0u) coord.xyz = rawpos.xyz;
        else if (source == 1u) { if ((pc.has & 2u) != 0u) coord.xyz = a_normal; }
        else if (source == 3u) { if ((pc.has & 4u) != 0u) coord.xyz = a_tangent; }
        else if (source == 4u) { if ((pc.has & 8u) != 0u) coord.xyz = a_binormal; }
        else if (source >= 5u && source <= 12u) {
            uint t = source - 5u;
            if ((pc.has & (64u << t)) != 0u) coord = vec4(a_uv[t], 1.0, 1.0);
        }
        if (bits(info, 2u, 1u) == 0u) coord.z = 1.0;  // AB11 input form
        if (isnan(coord.x)) coord.x = 1.0;
        if (isnan(coord.y)) coord.y = 1.0;
        if (isnan(coord.z)) coord.z = 1.0;

        uint type = bits(info, 4u, 3u);
        vec3 out_tex;
        if (type == 1u) {  // emboss
            uint light = bits(info, 15u, 3u), src = bits(info, 12u, 3u);
            out_tex = src < texgen ? o_tex[src] : vec3(0.0);
            vec3 ldir = normalize(S.lights[light].pos.xyz - pos.xyz);
            out_tex += vec3(dot(ldir, _tangent), dot(ldir, _binormal), 0.0);
        } else if (type == 2u) {
            out_tex = vec3(o_colors_0.x, o_colors_0.y, 1.0);
        } else if (type == 3u) {
            out_tex = vec3(o_colors_1.x, o_colors_1.y, 1.0);
        } else {
            uint m = mtx_index(1u + texgen);
            if (bits(info, 1u, 1u) != 0u)  // STQ
                out_tex = vec3(dot(coord, S.posmtx[m]), dot(coord, S.posmtx[m + 1u]), dot(coord, S.posmtx[m + 2u]));
            else
                out_tex = vec3(dot(coord, S.posmtx[m]), dot(coord, S.posmtx[m + 1u]), 1.0);
        }
        if (S.xf[1].y != 0u) {  // dual texture transform
            uint post = S.xf[4u + (texgen >> 2)][texgen & 3u];
            uint base = bits(post, 0u, 6u);
            vec4 Q0 = S.postmtx[base & 63u], Q1 = S.postmtx[(base + 1u) & 63u], Q2 = S.postmtx[(base + 2u) & 63u];
            if (bits(post, 8u, 1u) != 0u) out_tex = normalize(out_tex);
            out_tex = vec3(dot(Q0.xyz, out_tex) + Q0.w, dot(Q1.xyz, out_tex) + Q1.w, dot(Q2.xyz, out_tex) + Q2.w);
        }
        if (type == 0u && out_tex.z == 0.0)
            out_tex.xy = clamp(out_tex.xy / 2.0, vec2(-1.0), vec2(1.0));
        o_tex[texgen] = out_tex;
    }

    uint num_chans = S.xf[0].x;
    if (num_chans == 0u) o_colors_0 = vec4(0.0);
    if (num_chans <= 1u) o_colors_1 = vec4(0.0);

    // Depth into Vulkan's range, the half-pixel offset, Vulkan's y.
    o_pos.z = o_pos.w * S.pixel_center.w - o_pos.z * S.pixel_center.z;
    o_pos.xy *= sign(S.pixel_center.xy * vec2(1.0, -1.0));
    o_pos.xy = o_pos.xy - o_pos.w * S.pixel_center.xy;

    colors_0 = o_colors_0;
    colors_1 = o_colors_1;
    tex = o_tex;
    gl_Position = vec4(o_pos.x, -o_pos.y, o_pos.z, o_pos.w);
    gl_PointSize = pc.point_size;  // used only when drawing points
}
