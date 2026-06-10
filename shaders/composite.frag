#version 450

// Composite pass. Samples the low-res scene image (sky + world drawn into the
// offscreen target) with NEAREST filtering — the chunky pixelation upscale onto
// the swapchain. On top of the upscale it applies depth-based fog (issue #10 E):
// distance haze (distant terrain dissolves into the sky/haze colour) plus an
// analytic exponential height/valley fog (pools in low ground), all driven by the
// weather state + time of day. Sky pixels (depth == far) are skipped — the sky
// shader already paints the horizon haze there. The retro posterise+dither stays
// per-surface in chunk.frag / sky.frag, so this pass only upscales + fogs.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(push_constant) uniform Push {
    mat4  invViewProj; // NDC -> world (inverse of the scene's proj*view)
    vec4  camPos;      // xyz camera world position
    vec4  fogColor;    // rgb haze colour, w = distance-fog density (per world unit)
    vec4  fogParams;   // x heightFalloff, y groundFogDensity, z fogTopY, w maxFog
    vec2  lowRes;      // offscreen size (unused here; kept for layout compat)
    float levels;      // unused here
} pc;

layout(location = 0) in  vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 col = texture(scene, uv).rgb;

    float depth = texture(depthTex, uv).r;
    if (depth >= 1.0) {        // sky / far plane: the sky already holds horizon haze
        outColor = vec4(col, 1.0);
        return;
    }

    // Reconstruct the world position of this pixel from depth.
    vec4 world = pc.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    world.xyz /= world.w;
    vec3  d    = world.xyz - pc.camPos.xyz;
    float dist = length(d);

    // Optical depth: a uniform distance term + an analytic exponential height
    // term (closed-form integral of exp(-k*(y-top)) along the view ray, so fog
    // density grows toward the ground and pools in valleys — no marching).
    float distOpt = dist * pc.fogColor.w;

    vec3  dir   = d / max(dist, 1e-4);
    float kf    = pc.fogParams.x;                          // height falloff
    float dCam  = exp(-kf * (pc.camPos.y - pc.fogParams.z)); // density at the camera
    float denom = kf * dir.y;
    float heightOpt = (abs(denom) > 1e-4)
                        ? dCam * (1.0 - exp(-denom * dist)) / denom
                        : dCam * dist;                    // ray ~parallel to ground
    heightOpt = max(heightOpt, 0.0) * pc.fogParams.y;

    float fog = 1.0 - exp(-(distOpt + heightOpt));
    fog = min(fog, pc.fogParams.w);                       // keep silhouettes readable

    outColor = vec4(mix(col, pc.fogColor.rgb, fog), 1.0);
}
