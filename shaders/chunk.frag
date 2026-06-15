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
    vec4 misc;   // x: time, y: PS1 jitter grid, z: PS1 affine-warp flag (0/1)
    vec4 heldLight;    // xyz: held-emitter world pos, w: radius in blocks (0 = off)
    vec4 heldLightCol; // rgb: linear colour, a: intensity (0..1)
} camera;

layout(location = 0) in vec2      fragUV;
layout(location = 1) in flat uint fragLayer;
layout(location = 2) in vec2      fragLight;  // x = sky-lit, y = block-lit (0..1)
layout(location = 3) in flat uint fragNormal; // face index (Face enum, 0..5)
layout(location = 4) in flat float fragAlpha; // 1 opaque pass, <1 translucent water
layout(location = 5) in vec3      fragBlockColor; // emitter hue for the block-lit term
layout(location = 6) in vec3      fragTint;        // biome vegetation tint (white = none)
layout(location = 7) noperspective in vec2 fragUVaffine; // PS1 affine-warped UV
layout(location = 8) in vec3 fragWorldPos; // world-space position (held point light)

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
    // PS1 affine warp: use the non-perspective-corrected UV so textures swim/stretch
    // across polygons (misc.z on); otherwise the normal perspective-correct UV.
    vec2 uv = camera.misc.z > 0.5 ? fragUVaffine : fragUV;
    vec3 uvw = vec3(uv, float(fragLayer));
    vec4 texel = texture(texArray, uvw); // mip-filtered colour (anti-shimmer)
    // Cut-out test against the FULL-RES alpha (LOD 0), not the mip-averaged alpha:
    // averaging alpha shrinks/grows a thin shape's coverage and dissolves foliage,
    // leaves and water edges at distance. Sampling LOD 0 here preserves the exact
    // silhouette while the colour above still benefits from mipmapping.
    if (textureLod(texArray, uvw, 0.0).a < 0.5) {
        discard; // supports cut-out textures (plants, leaves, ...)
    }
    // Biome vegetation tint: white for ordinary blocks (no-op), a per-biome colour
    // for grass/leaves/plants so the same green texture reads lush / dry / pale by
    // climate. Applied to the albedo before lighting.
    texel.rgb *= fragTint;

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
    // moonlight); block light is the emitter's own colour (a warm torch, orange
    // lava, ...), carried per-vertex from the dominant emitter reaching it. Blend
    // by dominance. Where no block light reaches (block ~= 0) fragBlockColor is
    // black, but wSky -> 1 there so the sky tint wins and the black is ignored.
    const vec3 kSourceFallback = vec3(1.00, 0.66, 0.32);
    vec3 blockCol = block > 1e-4 ? fragBlockColor : kSourceFallback;

    // --- Held point light ----------------------------------------------------
    // A lit emitter in the player's hand (torch/glowstone) casts a dynamic light
    // that follows them. Distance attenuation with a quadratic-ish falloff to the
    // radius; folded into the block term so it shares the colour + dither ramp.
    // When it out-shines the baked block light here, it also drives the hue.
    if (camera.heldLight.w > 0.0) {
        float d   = distance(fragWorldPos, camera.heldLight.xyz);
        float att = clamp(1.0 - d / camera.heldLight.w, 0.0, 1.0);
        float pl  = att * att * camera.heldLightCol.a;
        if (pl > block) blockCol = camera.heldLightCol.rgb;
        block = max(block, pl);
    }

    float wSky = sky / max(sky + block, 1e-4);
    vec3 lightCol = mix(blockCol, camera.sunCol.rgb, wSky);
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
