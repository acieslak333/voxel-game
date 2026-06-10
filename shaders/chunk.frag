#version 450

// The block texture array. UVs are in block units and the sampler uses REPEAT
// addressing, so each block of a greedy-merged quad shows one full texture tile.
layout(set = 0, binding = 1) uniform sampler2DArray texArray;

// Camera + day-night sun state (shared with the vertex shader).
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir; // xyz: toward the active light (sun/moon), w: ambient floor
    vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
} camera;

layout(location = 0) in vec2      fragUV;
layout(location = 1) in flat uint fragLayer;
layout(location = 2) in vec2      fragLight;  // x = sky-lit, y = block-lit (0..1)
layout(location = 3) in flat uint fragNormal; // face index (Face enum, 0..5)
layout(location = 4) in flat float fragAlpha; // 1 opaque pass, <1 translucent water

layout(location = 0) out vec4 outColor;

// Face-index -> unit normal (matches the Face enum: axis*2 + positive).
const vec3 kNormals[6] = vec3[6](
    vec3(-1, 0, 0), vec3(1, 0, 0),
    vec3(0, -1, 0), vec3(0, 1, 0),
    vec3(0, 0, -1), vec3(0, 0, 1));

// 4x4 ordered (Bayer) dither matrix.
const float kBayer[16] = float[16](
     0.0,  8.0,  2.0, 10.0,
    12.0,  4.0, 14.0,  6.0,
     3.0, 11.0,  1.0,  9.0,
    15.0,  7.0, 13.0,  5.0);

void main() {
    vec4 texel = texture(texArray, vec3(fragUV, float(fragLayer)));
    if (texel.a < 0.5) {
        discard; // supports cut-out textures later (e.g. plants)
    }

    // --- Directional sun/moon lighting ---------------------------------------
    // The celestial light is a moving direction vector: faces turned toward it
    // are bright, faces away fall to the ambient floor — so shading sweeps
    // around the blocks as the day passes. The sky term gates it (caves get no
    // sun even at noon); intensity dims the whole thing toward night.
    vec3 N = kNormals[fragNormal];
    float ndl = max(dot(N, camera.sunDir.xyz), 0.0);
    float ambient = camera.sunDir.w;
    float directional = ambient + (1.0 - ambient) * ndl;
    float sky   = fragLight.x * directional * camera.sunCol.a;
    float block = fragLight.y;

    // --- Coloured light ------------------------------------------------------
    // Sky light is tinted by the time of day (warm noon, orange dusk, pale blue
    // moonlight); block light is the warm emitter glow. Blend by dominance.
    const vec3 kSource = vec3(1.00, 0.66, 0.32);
    float wSky = sky / max(sky + block, 1e-4);
    vec3 lightCol = mix(kSource, camera.sunCol.rgb, wSky);
    vec3 surf = texel.rgb * lightCol; // fully-lit surface colour

    // --- 7-level dithered shadow ramp ----------------------------------------
    // Brightness is quantised to 7 levels with ordered dithering at the level
    // boundaries. Level 0 is as dark as the night sky (so fully-unlit terrain
    // melts into the dark sky instead of glowing brighter than it); level 6 is
    // fully lit with no dithering. gl_FragCoord is in the low-res offscreen
    // image, so the stipple is chunky to match the pixelation. (The sky shader
    // dithers the same way so the gradient sky stipples to match this terrain.)
    const float kLevels = 7.0;
    int bx = int(gl_FragCoord.x) & 3;
    int by = int(gl_FragCoord.y) & 3;
    float thresh = (kBayer[by * 4 + bx] + 0.5) / 16.0;

    float lit = max(sky, block);
    float x = lit * (kLevels - 1.0);
    float level = floor(x) + step(thresh, fract(x)); // dithered quantise
    float litQ = level / (kLevels - 1.0);

    // Matches the authored night zenith (#070A1A in linear) so the deepest
    // shadow level is as dark as the dark night sky.
    const vec3 kShadow = vec3(0.0030, 0.0040, 0.0130);
    vec3 color = mix(kShadow, surf, litQ);

    // Colours are linear here; the sRGB swapchain encodes gamma on write.
    // fragAlpha is 1 for opaque geometry and <1 for the translucent water pass,
    // which alpha-blends this surface over the seabed/terrain drawn behind it.
    outColor = vec4(color, fragAlpha);
}
