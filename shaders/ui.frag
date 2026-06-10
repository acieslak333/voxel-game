#version 450

// Single-channel font atlas. Solid quads sample a white texel so they come out
// as their flat colour; text quads sample glyph coverage for anti-aliasing.
layout(set = 0, binding = 0) uniform sampler2D fontTex;
// The block texture array, shared with the world renderer, so the HUD can draw
// block icons with their real textures. Sampled when fragLayer >= 0.
layout(set = 0, binding = 1) uniform sampler2DArray blockTex;

layout(location = 0) in vec2  fragUV;
layout(location = 1) in vec4  fragColor;
layout(location = 2) in float fragLayer;

layout(location = 0) out vec4 outColor;

// The swapchain is an sRGB target that encodes linear->sRGB on write, so convert
// the (intuitively sRGB) UI colour to linear here for an accurate result.
vec3 toLinear(vec3 c) {
    bvec3 hi = greaterThan(c, vec3(0.04045));
    vec3 lo = c / 12.92;
    vec3 pw = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, pw, vec3(hi));
}

void main() {
    if (fragLayer < 0.0) {
        // Font atlas / solid quad: fragColor is an sRGB colour, the atlas gives
        // coverage (1.0 for solid quads, glyph alpha for text).
        float coverage = texture(fontTex, fragUV).r;
        outColor = vec4(toLinear(fragColor.rgb), fragColor.a * coverage);
    } else {
        // Block icon: sample the (sRGB-format, so already-linear) block texture
        // and tint by fragColor.rgb, used here as a per-face shade multiplier.
        vec4 texel = texture(blockTex, vec3(fragUV, fragLayer));
        outColor = vec4(texel.rgb * fragColor.rgb, fragColor.a * texel.a);
    }
}
