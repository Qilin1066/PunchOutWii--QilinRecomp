// EFB copies: the EFB's pixels as the texture format the copy writes would
// read back when sampled (formats per Dolphin's EFB copy conversion), or as
// they go to the frame buffer.
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 ocol0;
layout(set = 0, binding = 0) uniform sampler2D efb;
layout(push_constant) uniform Push {
    vec4 src;
    vec4 color;
    float depth;
    uint format;
    uint flags;  // 1 half scale (box filter), 2 copy depth, 4 the EFB's format has alpha
    float gamma;
    uvec3 weights;  // the copy filter's weights: the line above, the line, the line below (64: unchanged)
    float clamp_top, clamp_bottom, line;  // the lines the filter may read; one EFB line (in 0..1)
} pc;

vec4 q(vec4 c, float levels) { return floor(c * levels + 0.5) / levels; }

// One EFB pixel as a copy reads it. A depth copy (flags bit 1) reads the
// 24-bit depth as bytes, (z >> 16, z >> 8, z, 255) like a colour, and the
// formats below pick from those as they would from colour: Z8 is the high
// byte, Z8M the middle, Z8L the low (Dolphin's EFB copy conversion). The
// depth image is sampled with a nearest sampler.
vec4 efb_pixel(vec2 at) {
    if ((pc.flags & 2u) == 0u) return texture(efb, at);
    uint z = min(uint(clamp(texture(efb, at).r, 0.0, 1.0) * 16777216.0), 0xFFFFFFu);
    return vec4(float((z >> 16) & 255u), float((z >> 8) & 255u), float(z & 255u), 255.0) / 255.0;
}

// One line's pixel as the copy reads it: half scale averages a 2x2 box (over
// the bytes, for depth too).
vec4 row(vec2 at) {
    if ((pc.flags & 1u) == 0u) return efb_pixel(at);
    vec2 t = 1.0 / vec2(textureSize(efb, 0));
    return 0.25 * (efb_pixel(at + vec2(-0.5, -0.5) * t) + efb_pixel(at + vec2(0.5, -0.5) * t) +
                   efb_pixel(at + vec2(-0.5, 0.5) * t) + efb_pixel(at + vec2(0.5, 0.5) * t));
}

void main() {
    // The copy filter, as the hardware's (and Dolphin's): the line above, the
    // line and the line below, in 8-bit integers, weighted, over 64; colour
    // only, alpha as it is. Lines are the console's (at any internal
    // resolution), clamped to the copy's rectangle if its command says so.
    vec4 mid = row(uv);
    uvec3 sum = uvec3(mid.rgb * 255.0) * pc.weights.y;
    if (pc.weights.x != 0u || pc.weights.z != 0u) {
        vec2 above = vec2(uv.x, clamp(uv.y - pc.line, pc.clamp_top, pc.clamp_bottom));
        vec2 below = vec2(uv.x, clamp(uv.y + pc.line, pc.clamp_top, pc.clamp_bottom));
        sum += uvec3(row(above).rgb * 255.0) * pc.weights.x + uvec3(row(below).rgb * 255.0) * pc.weights.z;
    }
    uvec3 rgb = sum >> 6u;
    if (pc.weights.x + pc.weights.y + pc.weights.z >= 128u) rgb &= 0x1FFu;  // past 511 the hardware wraps
    // Alpha only from a format that keeps it (RGBA6_Z24); else whatever the
    // EFB image holds from before is not the hardware's: it reads 255.
    vec4 c = vec4(vec3(min(rgb, uvec3(255u))) / 255.0, (pc.flags & 4u) != 0u ? mid.a : 1.0);
    if (pc.gamma != 1.0) c.rgb = pow(c.rgb, vec3(1.0 / pc.gamma));
    if (pc.format == 0xFFFFu) {  // to the frame buffer
        ocol0 = vec4(c.rgb, 1.0);
        return;
    }
    bool intensity = (pc.format & 0x100u) != 0u;
    uint f = pc.format & 0xFFu;
    float Y = clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);
    float R = intensity ? Y : c.r;
    switch (f) {
    case 0u: ocol0 = q(vec4(R), 15.0); break;                                       // R4 / I4
    case 1u: case 8u: ocol0 = vec4(R); break;                                        // R8 / I8
    case 2u: ocol0 = q(vec4(R, R, R, c.a), 15.0); break;                             // RA4 / IA4
    case 3u: ocol0 = vec4(R, R, R, c.a); break;                                      // RA8 / IA8
    case 4u: ocol0 = vec4(q(c, 31.0).r, q(c, 63.0).g, q(c, 31.0).b, 1.0); break;     // RGB565
    case 5u:                                                                         // RGB5A3
        if (c.a >= 224.0 / 255.0) ocol0 = vec4(q(c, 31.0).rgb, 1.0);
        else ocol0 = vec4(q(c, 15.0).rgb, q(c, 7.0).a);
        break;
    case 7u: ocol0 = vec4(c.a); break;                                               // A8
    case 9u: ocol0 = vec4(c.g); break;                                               // G8
    case 10u: ocol0 = vec4(c.b); break;                                              // B8
    case 11u: ocol0 = vec4(c.r, c.r, c.r, c.g); break;                               // RG8 (as IA8): intensity R, alpha G
    case 12u: ocol0 = vec4(c.g, c.g, c.g, c.b); break;                               // GB8 (as IA8): intensity G, alpha B
    default: ocol0 = c; break;                                                       // RGBA8
    }
}
