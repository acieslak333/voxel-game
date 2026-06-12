#version 450

// The shared block texture array (mipmapped — far tiles minify smoothly).
layout(set = 0, binding = 1) uniform sampler2DArray texArray;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir;  // xyz: toward the active light, w: ambient floor
    vec4 sunCol;  // rgb: linear light tint, a: sky-light intensity
    vec4 camPos;  // xyz: camera world pos, w: haze fade-start distance
    vec4 haze;    // rgb: horizon haze colour, w: haze fade-end distance
} cam;

layout(location = 0) in vec3      fragNormal;
layout(location = 1) in vec2      fragUV;
layout(location = 2) in flat uint fragLayer;
layout(location = 3) in vec3      fragTint;
layout(location = 4) in vec3      fragWorld;

layout(location = 0) out vec4 outColor;

// 4x4 ordered (Bayer) dither matrix — identical to chunk.frag.
const float kBayer[16] = float[16](
     0.0,  8.0,  2.0, 10.0,
    12.0,  4.0, 14.0,  6.0,
     3.0, 11.0,  1.0,  9.0,
    15.0,  7.0, 13.0,  5.0);

void main() {
    // Far terrain is the open surface, fully sky-exposed (no block light, no cave
    // shadow). Shade it EXACTLY like chunk.frag at full sky light so the far shell
    // matches the near terrain with no brightness step at the LOD seam: the same
    // directional sun/ambient term, then the same 7-level dithered shadow ramp from
    // the dark-night-sky floor (kShadow) up to the fully-lit surface colour.
    vec3 albedo = texture(texArray, vec3(fragUV, float(fragLayer))).rgb * fragTint;

    vec3  N   = normalize(fragNormal);
    float ndl = max(dot(N, cam.sunDir.xyz), 0.0);
    float amb = cam.sunDir.w;
    float lit = (amb + (1.0 - amb) * ndl) * cam.sunCol.a; // == chunk.frag's sky term
    vec3  surf = albedo * cam.sunCol.rgb;                 // fully-lit colour

    const float kLevels = 7.0;
    int bx = int(gl_FragCoord.x) & 3;
    int by = int(gl_FragCoord.y) & 3;
    float thresh = (kBayer[by * 4 + bx] + 0.5) / 16.0;
    float x = lit * (kLevels - 1.0);
    float level = floor(x) + step(thresh, fract(x)); // dithered quantise
    float litQ = level / (kLevels - 1.0);
    const vec3 kShadow = vec3(0.0030, 0.0040, 0.0130); // night-zenith floor
    vec3 color = mix(kShadow, surf, litQ);

    // Distance haze-fade: dissolve the shell into the horizon haze over its outer
    // band so its finite edge never reads as a hard cutoff against the sky. Local
    // to the far shell; the global composite fog still applies on top.
    float dist = length(fragWorld - cam.camPos.xyz);
    float t = clamp((dist - cam.camPos.w) / max(cam.haze.w - cam.camPos.w, 1.0), 0.0, 1.0);
    t = t * t * (3.0 - 2.0 * t); // smoothstep
    color = mix(color, cam.haze.rgb, t);

    outColor = vec4(color, 1.0); // opaque; linear (composite/swapchain gamma)
}
