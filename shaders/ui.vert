#version 450

// 2D UI vertex: position in screen pixels (top-left origin), a font-atlas UV,
// and an RGBA colour. The screen size comes in as a push constant so positions
// can be given in intuitive pixel coordinates.
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
// Block-texture-array layer to sample, or < 0 to use the font atlas (text/solid
// quads). Lets the same batch draw HUD panels/text and textured block icons.
layout(location = 3) in float inLayer;

layout(push_constant) uniform Push {
    vec2 screen; // framebuffer size in pixels
} pc;

layout(location = 0) out vec2  fragUV;
layout(location = 1) out vec4  fragColor;
layout(location = 2) out float fragLayer;

void main() {
    // Pixel -> Vulkan clip space. Vulkan's NDC has +Y pointing down, which
    // matches a top-left pixel origin, so no flip is needed.
    vec2 ndc = inPos / pc.screen * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUV    = inUV;
    fragColor = inColor;
    fragLayer = inLayer;
}
