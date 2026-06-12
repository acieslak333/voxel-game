#version 450

// Block atlas (binding 1) for entities reusing block textures (dropped-item cubes,
// placeholder rigs); skin atlas (binding 2) for Blockbench models with their own
// texture. push.useSkin picks between them per draw.
layout(set = 0, binding = 1) uniform sampler2DArray texArray;
layout(set = 0, binding = 2) uniform sampler2DArray skinArray;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir; // xyz: toward the active light, w: ambient floor
    vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
} cam;

layout(push_constant) uniform Push {
    mat4 model;
    uint useSkin;
} push;

layout(location = 0) in vec3      fragNormal;
layout(location = 1) in vec2      fragUV;
layout(location = 2) in flat uint fragLayer;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 uvw = vec3(fragUV, float(fragLayer));
    vec4 texel = (push.useSkin == 1u) ? texture(skinArray, uvw) : texture(texArray, uvw);
    // Cut-out test against full-res alpha (LOD 0) so mip-averaged alpha doesn't
    // dissolve thin shapes (crack overlay, billboards) at distance.
    float a = (push.useSkin == 1u) ? textureLod(skinArray, uvw, 0.0).a
                                   : textureLod(texArray, uvw, 0.0).a;
    if (a < 0.5) discard;

    // Directional sun/moon lighting, matching the terrain: faces toward the light
    // are bright, away fall to the ambient floor; the whole thing dims toward night
    // via the sky-light intensity. Linear colour (the composite/swapchain gammas).
    vec3  N    = normalize(fragNormal);
    float ndl  = max(dot(N, cam.sunDir.xyz), 0.0);
    float amb  = cam.sunDir.w;
    float lit  = (amb + (1.0 - amb) * ndl) * cam.sunCol.a;

    vec3 color = texel.rgb * cam.sunCol.rgb * lit;
    outColor = vec4(color, 1.0);
}
