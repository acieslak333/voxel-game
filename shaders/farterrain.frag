#version 450

// The shared block texture array (mipmapped — far tiles minify smoothly).
layout(set = 0, binding = 1) uniform sampler2DArray texArray;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir; // xyz: toward the active light, w: ambient floor
    vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
} cam;

layout(location = 0) in vec3      fragNormal;
layout(location = 1) in vec2      fragUV;
layout(location = 2) in flat uint fragLayer;
layout(location = 3) in vec3      fragTint;

layout(location = 0) out vec4 outColor;

void main() {
    // Far terrain is the open surface, so it's fully sky-exposed: shade it with
    // the same directional sun/moon + ambient floor + sky intensity the near
    // terrain uses at full sky light (no block light, no cave shadow). The
    // composite pass then fogs it into the horizon like everything else.
    vec3 albedo = texture(texArray, vec3(fragUV, float(fragLayer))).rgb * fragTint;

    vec3  N   = normalize(fragNormal);
    float ndl = max(dot(N, cam.sunDir.xyz), 0.0);
    float amb = cam.sunDir.w;
    float lit = (amb + (1.0 - amb) * ndl) * cam.sunCol.a;

    vec3 color = albedo * cam.sunCol.rgb * lit;
    outColor = vec4(color, 1.0); // opaque; linear (composite/swapchain gamma)
}
