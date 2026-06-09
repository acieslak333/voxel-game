#version 450

// The block texture array. UVs are in block units and the sampler uses REPEAT
// addressing, so each block of a greedy-merged quad shows one full texture tile.
layout(set = 0, binding = 1) uniform sampler2DArray texArray;

layout(location = 0) in vec2      fragUV;
layout(location = 1) in flat uint fragLayer;
layout(location = 2) in float     fragShade;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(texArray, vec3(fragUV, float(fragLayer)));
    if (texel.a < 0.5) {
        discard; // supports cut-out textures later (e.g. plants)
    }
    // Apply the cheap per-face directional shade. Colours are in linear space
    // here; the sRGB swapchain handles gamma encoding on write.
    outColor = vec4(texel.rgb * fragShade, 1.0);
}
