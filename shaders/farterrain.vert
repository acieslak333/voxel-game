#version 450

// Far-terrain LOD shell + low-poly tree impostors drawn beyond the voxel window
// (FarTerrainRenderer). Vertices are already in WORLD space (no model matrix),
// so this is a straight view/proj transform. Lit like the terrain in the
// fragment shader so the distant ground matches the near chunks; the world
// position is passed through for the distance haze-fade.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir;  // xyz: toward the active light, w: ambient floor
    vec4 sunCol;  // rgb: linear light tint, a: sky-light intensity
    vec4 camPos;  // xyz: camera world pos, w: haze fade-start distance
    vec4 haze;    // rgb: horizon haze colour, w: haze fade-end distance
    vec4 lodFade; // x,y: impostor dissolve; z: PS1 vertex-jitter grid (0 = off)
} cam;

layout(location = 0) in vec3 inPos;     // world-space position
layout(location = 1) in vec3 inNormal;  // per-triangle face normal
layout(location = 2) in vec2 inUV;      // block-unit UV (REPEAT tiling)
layout(location = 3) in uint inLayer;   // block texture array layer
layout(location = 4) in vec4 inTint;    // biome/forest tint (RGBA8 -> normalized)

layout(location = 0) out vec3      fragNormal;
layout(location = 1) out vec2      fragUV;
layout(location = 2) out flat uint fragLayer;
layout(location = 3) out vec4      fragTint;  // rgb tint, a=0 marks a tree impostor
layout(location = 4) out vec3      fragWorld;

void main() {
    gl_Position = cam.proj * cam.view * vec4(inPos, 1.0);
    // PS1 vertex jitter (cam.lodFade.z = grid resolution, 0 = off).
    if (cam.lodFade.z > 0.0) {
        vec2 g = vec2(cam.lodFade.z);
        gl_Position.xy = floor(gl_Position.xy / gl_Position.w * g) / g * gl_Position.w;
    }
    fragNormal  = inNormal;
    fragUV      = inUV;
    fragLayer   = inLayer;
    fragTint    = inTint;
    fragWorld   = inPos;
}
