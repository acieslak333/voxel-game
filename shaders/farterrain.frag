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

void main() {
    // Far terrain is the open surface, so it's fully sky-exposed: shade it with
    // the same directional sun/moon + ambient floor + sky intensity the near
    // terrain uses at full sky light (no block light, no cave shadow).
    vec3 albedo = texture(texArray, vec3(fragUV, float(fragLayer))).rgb * fragTint;

    vec3  N   = normalize(fragNormal);
    float ndl = max(dot(N, cam.sunDir.xyz), 0.0);
    float amb = cam.sunDir.w;
    float lit = (amb + (1.0 - amb) * ndl) * cam.sunCol.a;
    vec3 color = albedo * cam.sunCol.rgb * lit;

    // Distance haze-fade: dissolve the shell into the horizon haze colour over its
    // outer band so its finite edge never reads as a hard cutoff against the sky.
    // This is local to the far shell (the near terrain keeps its crispness); the
    // global composite fog still applies on top.
    float dist = length(fragWorld - cam.camPos.xyz);
    float t = clamp((dist - cam.camPos.w) / max(cam.haze.w - cam.camPos.w, 1.0), 0.0, 1.0);
    t = t * t * (3.0 - 2.0 * t); // smoothstep
    color = mix(color, cam.haze.rgb, t);

    outColor = vec4(color, 1.0); // opaque; linear (composite/swapchain gamma)
}
