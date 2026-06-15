#version 450

// Per-vertex inputs (see render/Vertex.h).
layout(location = 0) in vec3  inPos;    // position in chunk-local space
layout(location = 1) in vec2  inUV;     // UV in *block units* (tiles via REPEAT)
layout(location = 2) in uint  inLayer;  // texture-array layer for this face
layout(location = 3) in vec2  inLight;  // x = sky-lit, y = block-lit (incl AO)
layout(location = 4) in uint  inNormal; // face index (Face enum, 0..5)
layout(location = 5) in vec4  inBlockColor; // emitter hue for the block-lit term (RGBA8->0..1)
layout(location = 6) in vec4  inTint;       // biome vegetation tint (RGBA8->0..1, white = none)

// Camera matrices + the day-night sun state, shared by the whole draw. The sun
// fields are consumed in the fragment shader.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir; // xyz: toward the active light (sun/moon), w: ambient floor
    vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
    vec4 misc;   // x: animation time (seconds) for foliage sway / water waves
    vec4 heldLight;    // xyz: held-emitter world pos, w: radius (0 = off)
    vec4 heldLightCol; // rgb: linear colour, a: intensity (0..1)
} camera;

// Per-draw model transform (chunk world position) + params: params.x is the
// output alpha (1 for the opaque pass, <1 for the translucent water pass).
layout(push_constant) uniform Push {
    mat4 model;
    vec4 params;
} push;

layout(location = 0) out vec2      fragUV;
layout(location = 1) out flat uint fragLayer;  // 'flat': do not interpolate ints
layout(location = 2) out vec2      fragLight;
layout(location = 3) out flat uint fragNormal;
layout(location = 4) out flat float fragAlpha;
layout(location = 5) out vec3      fragBlockColor;
layout(location = 6) out vec3      fragTint;
// Same UV without perspective correction -> PS1 affine texture warp. The frag
// picks this one when misc.z (affine flag) is on; otherwise it uses fragUV.
layout(location = 7) noperspective out vec2 fragUVaffine;
layout(location = 8) out vec3 fragWorldPos; // world-space position for the held point light

void main() {
    // --- Ambient motion (foliage sway + water waves) -------------------------
    // Displace in chunk-local space (the chunk model is a pure translation, so a
    // local offset is a world offset). Phase varies by world position so plants
    // don't all wave in lockstep.
    float t = camera.misc.x;
    vec3 wp = (push.model * vec4(inPos, 1.0)).xyz;
    vec3 p = inPos;
    // Foliage: tint.a < 1 marks swayable cross fronds / ground plants. The top of
    // the plant bends; the base (fract(y) ~ 0) stays planted.
    float sway = 1.0 - inTint.a;
    if (sway > 0.001) {
        float h  = fract(inPos.y);
        float ph = wp.x * 0.7 + wp.z * 0.7;
        float amp = sway * 0.12 * h;
        p.x += amp * sin(t * 1.6 + ph);
        p.z += amp * cos(t * 1.3 + ph * 1.1);
    }
    // Water: the translucent pass (params.x < 1) gently bobs its top surface.
    if (push.params.x < 0.999 && inNormal == 3u) {
        p.y += 0.05 * sin(t * 1.1 + wp.x * 0.6 + wp.z * 0.6) - 0.02;
    }

    gl_Position = camera.proj * camera.view * push.model * vec4(p, 1.0);
    // --- PS1 vertex jitter -----------------------------------------------------
    // Snap the projected position to a coarse grid (misc.y = grid resolution, 0 =
    // off) so the low-precision look makes geometry quiver as it/the camera moves.
    if (camera.misc.y > 0.0) {
        vec2 g = vec2(camera.misc.y);
        gl_Position.xy = floor(gl_Position.xy / gl_Position.w * g) / g * gl_Position.w;
    }
    vec2 outUV = inUV;
    // Water scrolls its UV slightly so the surface reads as flowing, not glass.
    if (push.params.x < 0.999) outUV += vec2(t * 0.04, t * 0.02);
    fragUV         = outUV;
    fragUVaffine   = outUV;
    fragLayer      = inLayer;
    fragLight      = inLight;
    fragNormal     = inNormal;
    fragAlpha      = push.params.x;
    fragBlockColor = inBlockColor.rgb;
    fragTint       = inTint.rgb;
    fragWorldPos   = (push.model * vec4(p, 1.0)).xyz;
}
