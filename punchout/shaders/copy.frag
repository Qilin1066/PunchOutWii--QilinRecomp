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
    uint flags;  // 1 half scale (box filter), 2 copy depth
    float gamma;
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

void main() {
    vec4 c;
    if ((pc.flags & 1u) != 0u) {  // half scale: a 2x2 box (over the bytes, for depth too)
        vec2 t = 1.0 / vec2(textureSize(efb, 0));
        c = 0.25 * (efb_pixel(uv + vec2(-0.5, -0.5) * t) + efb_pixel(uv + vec2(0.5, -0.5) * t) +
                    efb_pixel(uv + vec2(-0.5, 0.5) * t) + efb_pixel(uv + vec2(0.5, 0.5) * t));
    } else {
        c = efb_pixel(uv);
    }
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
