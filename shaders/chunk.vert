#version 450

// Per-vertex inputs (see render/Vertex.h).
layout(location = 0) in vec3  inPos;    // position in chunk-local space
layout(location = 1) in vec2  inUV;     // UV in *block units* (tiles via REPEAT)
layout(location = 2) in uint  inLayer;  // texture-array layer for this face
layout(location = 3) in vec2  inLight;  // x = sky-lit, y = block-lit (incl AO)
layout(location = 4) in uint  inNormal; // face index (Face enum, 0..5)

// Camera matrices + the day-night sun state, shared by the whole draw. The sun
// fields are consumed in the fragment shader.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 sunDir; // xyz: toward the active light (sun/moon), w: ambient floor
    vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
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

void main() {
    gl_Position = camera.proj * camera.view * push.model * vec4(inPos, 1.0);
    fragUV     = inUV;
    fragLayer  = inLayer;
    fragLight  = inLight;
    fragNormal = inNormal;
    fragAlpha  = push.params.x;
}
