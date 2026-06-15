#version 450

// Per-vertex inputs (see entity/Armature.h EntityVertex). Unlike chunk geometry,
// entity normals are real vectors (the box rig rotates), not face indices.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uint inLayer;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir; // xyz: toward the active light, w: ambient floor
    vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
    vec4 retro;  // x: PS1 vertex-jitter grid resolution (0 = off)
} cam;

// Per-entity model transform (world placement of the whole rig; the rig's joint
// transforms are already baked into inPos on the CPU).
layout(push_constant) uniform Push {
    mat4 model;
    uint useSkin; // 0 = sample the block atlas (binding 1), 1 = the skin atlas (binding 2)
} push;

layout(location = 0) out vec3      fragNormal;
layout(location = 1) out vec2      fragUV;
layout(location = 2) out flat uint fragLayer;

void main() {
    vec4 worldPos = push.model * vec4(inPos, 1.0);
    gl_Position   = cam.proj * cam.view * worldPos;
    // PS1 vertex jitter: snap the projected position to a coarse grid so mobs and
    // held items quiver (cam.retro.x = grid resolution, 0 = off).
    if (cam.retro.x > 0.0) {
        vec2 g = vec2(cam.retro.x);
        gl_Position.xy = floor(gl_Position.xy / gl_Position.w * g) / g * gl_Position.w;
    }
    fragNormal    = normalize(mat3(push.model) * inNormal);
    fragUV        = inUV;
    fragLayer     = inLayer;
}
