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
    uint flags;  // 1 half scale (box filter), 2 copy depth, 4 the EFB's format has alpha (six bits a channel), 8 true colour
    float gamma;
    uvec3 weights;  // the copy filter's weights: the line above, the line, the line below (64: unchanged)
    float clamp_top, clamp_bottom, line;  // the lines the filter may read; one EFB line (in 0..1)
} pc;

// One EFB pixel as a copy reads it, in whole bytes. A depth copy (flags bit 1)
// reads the 24-bit depth as bytes, (z >> 16, z >> 8, z, 255) like a colour,
// and the formats below pick from those as they would from colour: Z8 is the
// high byte, Z8M the middle, Z8L the low (Dolphin's EFB copy conversion). The
// depth image is sampled with a nearest sampler.
uint efb_z(vec2 at) { return min(uint(clamp(texture(efb, at).r, 0.0, 1.0) * 16777216.0), 0xFFFFFFu); }
uvec4 z_bytes(uint z) { return uvec4((z >> 16) & 255u, (z >> 8) & 255u, z & 255u, 255u); }
uvec4 efb_bytes(vec2 at) { return uvec4(roundEven(texture(efb, at) * 255.0)); }

// One line's pixel as the copy reads it. An EFB with alpha holds six bits a
// channel (the image here holds them as n/63, so n is the byte's top six
// bits), which read as eight with the top two repeated below; in true colour
// (flags bit 8) all eight bits are the picture's own. Half scale reads a 2x2
// box: the four pixels added and what is below a whole step dropped, not
// rounded (as Dolphin's copies do); four sixes added are eight bits already,
// so a six-bit EFB's box keeps its two extra bits (Dolphin's software
// renderer). A depth box is over the depths, then split into bytes.
uvec4 row(vec2 at) {
    bool depth = (pc.flags & 2u) != 0u, six = (pc.flags & 14u) == 4u;
    if ((pc.flags & 1u) == 0u) {
        if (depth) return z_bytes(efb_z(at));
        uvec4 v = efb_bytes(at);
        return six ? (v & 0xFCu) | (v >> 6u) : v;
    }
    vec2 t = 0.5 / vec2(textureSize(efb, 0));
    vec2 a = at - t, b = at + vec2(t.x, -t.y), c = at + vec2(-t.x, t.y), d = at + t;
    if (depth) return z_bytes((efb_z(a) + efb_z(b) + efb_z(c) + efb_z(d)) >> 2u);
    if (six) {
        uvec4 sum = (efb_bytes(a) >> 2u) + (efb_bytes(b) >> 2u) + (efb_bytes(c) >> 2u) + (efb_bytes(d) >> 2u);
        return sum + (sum >> 6u);
    }
    return (efb_bytes(a) + efb_bytes(b) + efb_bytes(c) + efb_bytes(d)) >> 2u;
}

// The EFB read as its other pixel format (format 0xFFFE: what was RGB8 as RGBA6; 0xFFFD:
// back), pixel for pixel: the console's 24 bits a pixel are R,G,B from the top in one and
// four sixes from the top in the other (the layout Dolphin gives them). Six-bit channels
// are held here as n/63, and read back as the top six bits of the byte.
bool convert() {
    if (pc.format != 0xFFFEu && pc.format != 0xFFFDu) return false;
    uvec4 s = uvec4(roundEven(texelFetch(efb, ivec2(gl_FragCoord.xy), 0) * 255.0));
    if (pc.format == 0xFFFEu) {
        uint bits = (s.r << 16u) | (s.g << 8u) | s.b;
        ocol0 = vec4((bits >> 18u) & 63u, (bits >> 12u) & 63u, (bits >> 6u) & 63u, bits & 63u) / 63.0;
    } else {
        uint bits = ((s.r >> 2u) << 18u) | ((s.g >> 2u) << 12u) | ((s.b >> 2u) << 6u) | (s.a >> 2u);
        ocol0 = vec4(vec3((bits >> 16u) & 255u, (bits >> 8u) & 255u, bits & 255u) / 255.0, 1.0);
    }
    return true;
}

void main() {
    if (convert()) return;
    // The copy filter, as the hardware's (and Dolphin's): the line above, the
    // line and the line below, in 8-bit integers, weighted, over 64; colour
    // only, alpha as it is. Lines are the console's (at any internal
    // resolution), clamped to the copy's rectangle if its command says so.
    uvec4 mid = row(uv);
    uvec3 sum = mid.rgb * pc.weights.y;
    if (pc.weights.x != 0u || pc.weights.z != 0u) {
        vec2 above = vec2(uv.x, clamp(uv.y - pc.line, pc.clamp_top, pc.clamp_bottom));
        vec2 below = vec2(uv.x, clamp(uv.y + pc.line, pc.clamp_top, pc.clamp_bottom));
        sum += row(above).rgb * pc.weights.x + row(below).rgb * pc.weights.z;
    }
    uvec3 rgb = sum >> 6u;
    if (pc.weights.x + pc.weights.y + pc.weights.z >= 128u) rgb &= 0x1FFu;  // past 511 the hardware wraps
    rgb = min(rgb, uvec3(255u));
    // Gamma, back to whole bytes.
    if (pc.gamma != 1.0) rgb = uvec3(round(pow(vec3(rgb) / 255.0, vec3(1.0 / pc.gamma)) * 255.0));
    // Alpha only from a format that keeps it (RGBA6_Z24); else whatever the
    // EFB image holds from before is not the hardware's: it reads 255.
    vec4 c = vec4(vec3(rgb), (pc.flags & 4u) != 0u ? float(mid.a) : 255.0) / 255.0;
    if (pc.format == 0xFFFFu) {  // to the frame buffer
        ocol0 = vec4(c.rgb, 1.0);
        return;
    }
    // The copy's texture format, as whole bytes: a narrower channel keeps its
    // top bits (the hardware drops the rest, it doesn't round), and reads back
    // as that fraction of full scale.
    bool intensity = (pc.format & 0x100u) != 0u;
    uint f = pc.format & 0xFFu;
    // Intensity: the hardware's integer weights over 256, a half rounded up (measured by Dolphin).
    uint y256 = 66u * rgb.r + 129u * rgb.g + 25u * rgb.b + 16u * 256u;
    uint a8 = (pc.flags & 4u) != 0u ? mid.a : 255u;
    uint r8 = intensity ? min((y256 >> 8u) + ((y256 >> 7u) & 1u), 255u) : rgb.r;
    float R = float(r8) / 255.0, R4 = float(r8 & 0xF0u) / 240.0;
    // True colour keeps all eight bits of the two formats that only narrow the picture (as
    // Dolphin's does): a fight's shadows are an RGB565 copy, whose mid grey would darken and
    // tint the whole floor a little, as on the console.
    if ((pc.flags & 8u) != 0u && (f == 4u || f == 5u)) {
        ocol0 = f == 4u ? vec4(c.rgb, 1.0) : c;
        return;
    }
    switch (f) {
    case 0u: ocol0 = vec4(R4); break;                                                // R4 / I4
    case 1u: case 8u: ocol0 = vec4(R); break;                                        // R8 / I8
    case 2u: ocol0 = vec4(R4, R4, R4, float(a8 & 0xF0u) / 240.0); break;             // RA4 / IA4
    case 3u: ocol0 = vec4(R, R, R, c.a); break;                                      // RA8 / IA8
    case 4u: ocol0 = vec4(float(rgb.r & 0xF8u) / 248.0, float(rgb.g & 0xFCu) / 252.0, float(rgb.b & 0xF8u) / 248.0, 1.0); break;  // RGB565
    case 5u:                                                                         // RGB5A3: five bits opaque, else four and three of alpha
        if (a8 >= 224u) ocol0 = vec4(vec3(rgb & 0xF8u) / 248.0, 1.0);
        else ocol0 = vec4(vec3(rgb & 0xF0u) / 240.0, float(a8 & 0xE0u) / 224.0);
        break;
    case 7u: ocol0 = vec4(c.a); break;                                               // A8
    case 9u: ocol0 = vec4(c.g); break;                                               // G8
    case 10u: ocol0 = vec4(c.b); break;                                              // B8
    case 11u: ocol0 = vec4(c.r, c.r, c.r, c.g); break;                               // RG8 (as IA8): intensity R, alpha G
    case 12u: ocol0 = vec4(c.g, c.g, c.g, c.b); break;                               // GB8 (as IA8): intensity G, alpha B
    default: ocol0 = c; break;                                                       // RGBA8
    }
}
