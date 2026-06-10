#version 450

// Fullscreen triangle (no vertex buffer): indices 0,1,2 cover the screen. Used by
// the composite pass that upscales + dithers the low-res scene onto the swapchain.
layout(location = 0) out vec2 uv;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); // (0,0)(2,0)(0,2)
    uv = p;                          // 0..1 across the screen
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
