#version 450

// Far-terrain LOD shell: a coarse heightmap drawn beyond the voxel window
// (FarTerrainRenderer). Vertices are already in WORLD space (no model matrix),
// so this is a straight view/proj transform. Lit like the terrain in the
// fragment shader so the distant ground matches the near chunks.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir; // xyz: toward the active light, w: ambient floor
    vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
} cam;

layout(location = 0) in vec3 inPos;     // world-space position
layout(location = 1) in vec3 inNormal;  // per-triangle face normal
layout(location = 2) in vec2 inUV;      // block-unit UV (REPEAT tiling)
layout(location = 3) in uint inLayer;   // block texture array layer (surface top)
layout(location = 4) in vec4 inTint;    // biome veg tint (RGBA8 -> normalized)

layout(location = 0) out vec3      fragNormal;
layout(location = 1) out vec2      fragUV;
layout(location = 2) out flat uint fragLayer;
layout(location = 3) out vec3      fragTint;

void main() {
    gl_Position = cam.proj * cam.view * vec4(inPos, 1.0);
    fragNormal  = inNormal;
    fragUV      = inUV;
    fragLayer   = inLayer;
    fragTint    = inTint.rgb;
}
