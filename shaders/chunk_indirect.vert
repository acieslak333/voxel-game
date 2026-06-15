#version 450

// -----------------------------------------------------------------------------
//  chunk_indirect.vert — GPU-driven (multi-draw-indirect) variant of chunk.vert
// -----------------------------------------------------------------------------
//  Identical lighting / foliage-sway / water / PS1 logic to chunk.vert, with ONE
//  difference: the per-chunk model transform no longer arrives as a push constant
//  (indirect draws can't carry per-draw push constants). Instead every chunk's
//  world translation lives in a storage buffer (binding 2), indexed by
//  gl_InstanceIndex. The renderer encodes the chunk's slot in each indirect
//  command's `firstInstance`, and because instanceCount is 1, gl_InstanceIndex ==
//  firstInstance == slot. Requires the drawIndirectFirstInstance device feature.
//
//  The chunk model is a PURE TRANSLATION (chunkPos), so a chunk-local offset is a
//  world offset — exactly as chunk.vert relied on for its `push.model` being a
//  translation. params (pass-wide output alpha + the water-pass flag) still comes
//  via push constant: it is constant across a whole pass, set once before the
//  indirect draw, so it does not need to be per-chunk.
// -----------------------------------------------------------------------------

// Per-vertex inputs (see render/Vertex.h) — unchanged from chunk.vert.
layout(location = 0) in vec3  inPos;
layout(location = 1) in vec2  inUV;
layout(location = 2) in uint  inLayer;
layout(location = 3) in vec2  inLight;
layout(location = 4) in uint  inNormal;
layout(location = 5) in vec4  inBlockColor;
layout(location = 6) in vec4  inTint;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir;
    vec4 sunCol;
    vec4 misc;   // x: animation time, y: PS1 jitter grid, z: affine flag
    vec4 heldLight;
    vec4 heldLightCol;
} camera;

// Per-chunk draw data, indexed by gl_InstanceIndex (== the command's firstInstance
// == the chunk's mesh slot). xyz = chunk world-space translation; w is reserved
// (0). Sized to the renderer's mesh-slot count; culled/empty slots are simply
// never referenced (their indirect command has instanceCount 0).
struct ChunkDraw { vec4 posPad; };
layout(std430, set = 0, binding = 2) readonly buffer ChunkDrawData {
    ChunkDraw chunks[];
} drawData;

// Pass-wide params only (the model matrix is gone — see header). Kept as the same
// 20-float block chunk.vert/Pipeline use so the pipeline layout is unchanged; the
// `model` field is ignored here.
layout(push_constant) uniform Push {
    mat4 model;  // ignored in the indirect path
    vec4 params; // params.x = output alpha (1 opaque, <1 water)
} push;

layout(location = 0) out vec2      fragUV;
layout(location = 1) out flat uint fragLayer;
layout(location = 2) out vec2      fragLight;
layout(location = 3) out flat uint fragNormal;
layout(location = 4) out flat float fragAlpha;
layout(location = 5) out vec3      fragBlockColor;
layout(location = 6) out vec3      fragTint;
layout(location = 7) noperspective out vec2 fragUVaffine;
layout(location = 8) out vec3 fragWorldPos;

void main() {
    vec3 chunkPos = drawData.chunks[gl_InstanceIndex].posPad.xyz;

    float t = camera.misc.x;
    vec3 wp = inPos + chunkPos;          // model is a pure translation
    vec3 p = inPos;
    float sway = 1.0 - inTint.a;
    if (sway > 0.001) {
        float h  = fract(inPos.y);
        float ph = wp.x * 0.7 + wp.z * 0.7;
        float amp = sway * 0.12 * h;
        p.x += amp * sin(t * 1.6 + ph);
        p.z += amp * cos(t * 1.3 + ph * 1.1);
    }
    if (push.params.x < 0.999 && inNormal == 3u) {
        p.y += 0.05 * sin(t * 1.1 + wp.x * 0.6 + wp.z * 0.6) - 0.02;
    }

    gl_Position = camera.proj * camera.view * vec4(p + chunkPos, 1.0);
    if (camera.misc.y > 0.0) {
        vec2 g = vec2(camera.misc.y);
        gl_Position.xy = floor(gl_Position.xy / gl_Position.w * g) / g * gl_Position.w;
    }
    vec2 outUV = inUV;
    if (push.params.x < 0.999) outUV += vec2(t * 0.04, t * 0.02);
    fragUV         = outUV;
    fragUVaffine   = outUV;
    fragLayer      = inLayer;
    fragLight      = inLight;
    fragNormal     = inNormal;
    fragAlpha      = push.params.x;
    fragBlockColor = inBlockColor.rgb;
    fragTint       = inTint.rgb;
    fragWorldPos   = p + chunkPos;
}
