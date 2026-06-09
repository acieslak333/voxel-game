#version 450

// Per-vertex inputs (see render/Vertex.h).
layout(location = 0) in vec3  inPos;    // position in chunk-local space
layout(location = 1) in vec2  inUV;     // UV in *block units* (tiles via REPEAT)
layout(location = 2) in uint  inLayer;  // texture-array layer for this face
layout(location = 3) in float inShade;  // per-face brightness

// Camera matrices, shared by the whole draw.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

// Per-draw model transform (chunk world position).
layout(push_constant) uniform Push {
    mat4 model;
} push;

layout(location = 0) out vec2      fragUV;
layout(location = 1) out flat uint fragLayer; // 'flat': do not interpolate an int
layout(location = 2) out float     fragShade;

void main() {
    gl_Position = camera.proj * camera.view * push.model * vec4(inPos, 1.0);
    fragUV    = inUV;
    fragLayer = inLayer;
    fragShade = inShade;
}
