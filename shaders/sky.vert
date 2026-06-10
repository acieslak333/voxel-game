#version 450

// Fullscreen triangle generated from gl_VertexIndex (no vertex buffer): indices
// 0,1,2 produce NDC corners that cover the whole screen. The sky is drawn first
// in the scene pass with depth test/write off; the world then draws over it.
layout(location = 0) out vec2 ndc;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    ndc = p * 2.0 - 1.0;
    gl_Position = vec4(ndc, 1.0, 1.0);
}
