#version 450

// Composite pass. Samples the low-res scene image (sky + world drawn into the
// offscreen target) with NEAREST filtering — the chunky pixelation upscale onto
// the swapchain. On top of the upscale it applies depth-based fog (issue #10 E):
// distance haze (distant terrain dissolves into the sky/haze colour) plus an
// analytic exponential height/valley fog (pools in low ground), all driven by the
// weather state + time of day. Sky pixels (depth == far) are skipped — the sky
// shader already paints the horizon haze there. When the camera is submerged it
// also drowns the whole view (sky included) in distance-thickening blue murk. The
// retro posterise+dither stays per-surface in chunk.frag / sky.frag, so this pass
// only upscales + fogs.
layout(set = 0, binding = 0) uniform sampler2D scene;      // NEAREST -> crisp pixelation
layout(set = 0, binding = 1) uniform sampler2D depthTex;
// Bloom (issue: glow) reads the SAME scene image through a LINEAR sampler so the
// wide blur taps interpolate smoothly instead of snapping to the chunky low-res
// grid; the base image keeps `scene` (NEAREST) for the pixelated look.
layout(set = 0, binding = 3) uniform sampler2D sceneLinear;

// Post-FX params that don't fit in the (already full 128-byte) push block. Bloom
// is quasi-static (only changes when the player drags a settings slider), so one
// host-visible UBO updated each frame is plenty — a 1-frame-stale value during a
// drag is invisible.
layout(set = 0, binding = 2) uniform Post {
    vec4 bloom;  // x intensity (0 = off), y threshold, z radius (texels), w soft knee
    vec4 godray; // x intensity (0 = off), y density, z decay, w per-sample weight
    vec4 sun;    // xy sun screen UV, z inFront (1/0), w visibility (0 at night/horizon)
    // Retro (PS1/PS2) post. retro.x colour levels/channel (0 = off), y dither (0..1),
    // z interlace (0..1), w soft-blur amount (0..1). retro2.x frame parity (0/1),
    // y PS2 bilinear base (0/1), z soft-blur radius (texels).
    vec4 retro;
    vec4 retro2;
    // Selectable retro colour palette. palMeta.x = swatch count (0 = palette off,
    // fall back to the per-channel quantiser above). palette[] holds up to 64 sRGB
    // swatches in .rgb; the whole frame is remapped to the nearest one.
    vec4 palMeta;
    vec4 palette[64];
} post;

layout(push_constant) uniform Push {
    mat4  invViewProj; // NDC -> world (inverse of the scene's proj*view)
    vec4  camPos;      // xyz camera world position
    vec4  fogColor;    // rgb haze colour, w = distance-fog density (per world unit)
    vec4  fogParams;   // x heightFalloff, y groundFogDensity, z fogTopY, w maxFog
    vec2  noise;       // x = dark-grain max opacity (0 = off), y = time (s) reseed
    float submerged;   // 0 = above water, 1 = camera underwater (blue murk + tint)
    float pixel;       // grain cell size in screen px (matches the pixelate blocks)
} pc;

layout(location = 0) in  vec2 uv;
layout(location = 0) out vec4 outColor;

// Deep blue-green the scene fades into when the camera is underwater.
const vec3 kWaterMurk = vec3(0.04, 0.18, 0.28);

// 4x4 ordered (Bayer) dither matrix — fakes extra shades over the quantised palette.
const float kBayer[16] = float[16](
     0.0,  8.0,  2.0, 10.0,
    12.0,  4.0, 14.0,  6.0,
     3.0, 11.0,  1.0,  9.0,
    15.0,  7.0, 13.0,  5.0);

// Cheap 3D hash -> [0,1) (Dave Hoskins). Seeds the low-light grain from (cell, frame)
// so each frame is an independent draw — the field never just translates/tiles.
float hash13(vec3 p3) {
    p3  = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

// Bright-pass: keep only the part of a colour above the threshold, with a soft
// knee so highlights ramp in gradually rather than popping at a hard cutoff.
vec3 brightPass(vec3 c, float thr, float knee) {
    float l    = max(max(c.r, c.g), c.b);
    float soft = clamp(l - thr + knee, 0.0, 2.0 * knee);
    soft       = soft * soft / (4.0 * knee + 1e-4);
    float contrib = max(soft, l - thr) / max(l, 1e-4);
    return c * max(contrib, 0.0);
}

// Cheap single-pass bloom: a centre tap plus two 8-way rings of bright-passed
// samples (17 taps total), Gaussian-ish weighted by ring. `radius` is in low-res
// texels; the linear sampler smooths each tap so the glow is soft and round even
// though the source is the chunky offscreen image.
vec3 bloomGather(vec2 uvc, vec2 texel, float radius, float thr, float knee) {
    vec3  sum  = brightPass(texture(sceneLinear, uvc).rgb, thr, knee);
    float wsum = 1.0;
    for (int i = 0; i < 8; ++i) {
        float a   = float(i) * 0.78539816; // 45 degrees
        vec2  dir = vec2(cos(a), sin(a));
        vec3 ci = brightPass(texture(sceneLinear, uvc + dir * texel * radius * 0.55).rgb, thr, knee);
        sum += ci * 0.7; wsum += 0.7;
        vec3 co = brightPass(texture(sceneLinear, uvc + dir * texel * radius * 1.15).rgb, thr, knee);
        sum += co * 0.4; wsum += 0.4;
    }
    return sum / wsum;
}

// Crepuscular rays ("god rays"): a screen-space radial blur of the visible sky,
// marched from each pixel toward the sun's screen position. Sky pixels (depth == 0)
// emit light; geometry (blocks, trees) is a black occluder, so the smear leaves
// bright shafts streaming out through every gap — between leaves, over ridgelines,
// through cloud breaks (bright cloud = strong emitter, dark cloud = weak). The rays
// inherit the sky/sun tint because the emitter is the actual sky colour.
vec3 godRays(vec2 uvc) {
    if (post.godray.x <= 0.0 || post.sun.z < 0.5 || post.sun.w <= 0.0) return vec3(0.0);

    const int N = 48;                                  // march steps toward the sun
    vec2  delta = (uvc - post.sun.xy) * (post.godray.y / float(N));
    vec2  coord = uvc;
    float illum = 1.0;                                 // decays along the shaft
    vec3  acc   = vec3(0.0);
    float wsum  = 0.0;
    for (int i = 0; i < N; ++i) {
        coord -= delta;
        // Emit only where the sky shows through; geometry is a black occluder, so a
        // ray crossing leaves/blocks gets darkened in proportion to how much it hides.
        float d = texture(depthTex, coord).r;
        vec3  s = (d <= 0.0) ? texture(sceneLinear, coord).rgb : vec3(0.0);
        float w = illum * post.godray.w;
        acc   += s * w;
        wsum  += w;                                     // counts occluded samples too
        illum *= post.godray.z;
    }
    // Normalise to a weighted average (the sky-visible fraction of the path, tinted
    // by the sky colour), so `intensity` is a clean 0..1 dial instead of an unbounded
    // sum that blows out to white when the whole path is open sky.
    vec3 shaft = (wsum > 1e-4) ? acc / wsum : vec3(0.0);
    return shaft * post.godray.x * post.sun.w;
}

void main() {
    vec3 col = texture(scene, uv).rgb; // NEAREST base = crisp PS1 pixels

    // --- PS2 soft / bilinear upscale -------------------------------------------
    // PS1 keeps the sharp nearest base above; PS2 smoothed its output, so blend in a
    // linear-sampled, lightly cross-blurred copy for that fuzzy-TV softness.
    if (post.retro2.y > 0.5) {
        vec2 ts = post.retro2.z / vec2(textureSize(sceneLinear, 0));
        vec3 sm = texture(sceneLinear, uv).rgb * 2.0;
        sm += texture(sceneLinear, uv + vec2(ts.x, 0.0)).rgb;
        sm += texture(sceneLinear, uv - vec2(ts.x, 0.0)).rgb;
        sm += texture(sceneLinear, uv + vec2(0.0, ts.y)).rgb;
        sm += texture(sceneLinear, uv - vec2(0.0, ts.y)).rgb;
        col = mix(col, sm / 6.0, post.retro.w);
    }

    // --- Bloom: add a blurred glow drawn from the scene's bright areas ----------
    // Applied before fog/murk so distant or submerged glow is hazed/absorbed like
    // everything else. post.bloom.x == 0 disables it (no taps taken).
    if (post.bloom.x > 0.0) {
        vec2 texel = 1.0 / vec2(textureSize(sceneLinear, 0));
        vec3 glow  = bloomGather(uv, texel, post.bloom.z, post.bloom.y, post.bloom.w);
        col += glow * post.bloom.x;
    }

    float depth = texture(depthTex, uv).r;
    // Reversed-Z: the far plane is depth 0 and the depth buffer is cleared to 0, so
    // untouched sky pixels read 0 (the sky shader already painted the horizon haze
    // there). Real geometry has depth > 0.
    bool  isSky = depth <= 0.0;

    // Distance to this pixel (sky pixels sit at the far plane -> use a big value so
    // they read as "far" for the underwater murk below).
    float dist = 200.0;
    vec3  dir  = vec3(0.0, 0.0, 1.0);
    if (!isSky) {
        vec4 world = pc.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
        world.xyz /= world.w;
        vec3 d = world.xyz - pc.camPos.xyz;
        dist   = length(d);
        dir    = d / max(dist, 1e-4);
    }

    // --- Atmospheric fog (above water only; the sky shader owns sky pixels) -----
    // Optical depth: a uniform distance term + an analytic exponential height term
    // (closed-form integral of exp(-k*(y-top)) along the view ray, so fog density
    // grows toward the ground and pools in valleys — no marching).
    if (!isSky) {
        float distOpt = dist * pc.fogColor.w;
        float kf    = pc.fogParams.x;                            // height falloff
        float dCam  = exp(-kf * (pc.camPos.y - pc.fogParams.z)); // density at the camera
        float denom = kf * dir.y;
        float heightOpt = (abs(denom) > 1e-4)
                            ? dCam * (1.0 - exp(-denom * dist)) / denom
                            : dCam * dist;                      // ray ~parallel to ground
        heightOpt = max(heightOpt, 0.0) * pc.fogParams.y;

        float fog = 1.0 - exp(-(distOpt + heightOpt));
        fog = min(fog, pc.fogParams.w);                         // keep silhouettes readable
        col = mix(col, pc.fogColor.rgb, fog);
    }

    // --- Underwater murk (camera submerged) ------------------------------------
    // Blue-green absorption that thickens with distance and tints the whole view
    // (including the sky above the surface), so being underwater reads as a murky,
    // short-sighted blue rather than a clear scene. Cheap exponential — no march.
    if (pc.submerged > 0.0) {
        float murk = 1.0 - exp(-dist * 0.045);
        murk = clamp(mix(0.30, 0.94, murk), 0.0, 0.94); // even near things are tinted
        col = mix(col, kWaterMurk, murk * pc.submerged);
    }

    // --- God rays (added on top of fog so the shafts stay readable) -------------
    // Suppressed underwater (the murky view owns that look). Additive, sun-tinted.
    col += godRays(uv) * (1.0 - pc.submerged);

    // --- Low-light sensor grain ------------------------------------------------
    // Soft static that appears only when the player stands in darkness; the amount
    // (noise.x) is computed on the CPU from the eye's light level, so a nearby torch
    // or daylight fades it out. Cells snap to the pixelate block grid so the grain is
    // the same chunkiness as the rest of the frame. noise.x == 0 disables it.
    if (pc.noise.x > 0.0) {
        // Reseed ~10x/s; the frame index goes INTO the hash (3rd axis), so each frame
        // is a fresh independent field — no repeating/translating pattern over the
        // screen. Cells snap to the pixelate grid to match the chunky look.
        float t    = floor(pc.noise.y * 10.0);
        // Grain cells are a couple of pixelate-blocks wide -> a coarser, chunkier
        // static than 1:1 with the upscale grid (bump 2.0 up for even coarser).
        vec2  cell = floor(gl_FragCoord.xy / (max(pc.pixel, 1.0) * 2.0));
        vec3  g    = vec3(hash13(vec3(cell, t)),
                          hash13(vec3(cell, t + 11.0)),
                          hash13(vec3(cell, t + 23.0))) - 0.5;
        // Mostly monochrome: keep only a hint of colour so it reads as soft film
        // grain, not harsh RGB confetti.
        g = mix(vec3((g.r + g.g + g.b) / 3.0), g, 0.25);
        // Light you can see nullifies it: any lit pixel clears, only the dark stays
        // grainy (so a lit opening / torch glow in view reads clean).
        float lum  = dot(col, vec3(0.299, 0.587, 0.114));
        float keep = 1.0 - smoothstep(0.18, 0.5, lum);
        col += g * keep * pc.noise.x; // signed -> mean-preserving (no wash-out)
    }

    // --- Colour quantisation + ordered dither (PS1 15-bit palette) -------------
    // Crush each channel to a few levels (5-bit = 32) and dither at the level
    // boundaries with a Bayer pattern so banding reads as a crosshatch stipple. The
    // dither cell snaps to the pixelate grid so it stays chunky like the rest.
    if (post.palMeta.x > 0.5) {
        // A palette is bound: remap the frame to the nearest swatch. The Bayer
        // nudge (same grid as the rest of the retro FX) is applied to the probe
        // colour before the search, so smooth gradients stipple between the two
        // closest swatches instead of hard-banding at swatch boundaries.
        int   n     = int(post.palMeta.x);
        ivec2 cell  = ivec2(floor(gl_FragCoord.xy / max(pc.pixel, 1.0))) & 3;
        float bayer = (kBayer[cell.y * 4 + cell.x] + 0.5) / 16.0 - 0.5; // [-0.5,0.5]
        // Spread ~ inverse cube-root of the swatch count: denser palettes need a
        // smaller nudge to cross between neighbours.
        float spread = post.retro.y / max(pow(post.palMeta.x, 0.3333), 1.0);
        vec3  probe  = col + bayer * spread;
        vec3  best   = post.palette[0].rgb;
        float bestd  = 1e9;
        for (int i = 0; i < n; ++i) {
            vec3  d  = probe - post.palette[i].rgb;
            float dd = dot(d, d);
            if (dd < bestd) { bestd = dd; best = post.palette[i].rgb; }
        }
        col = best;
    } else if (post.retro.x > 0.0) {
        float levels = post.retro.x;
        ivec2 cell  = ivec2(floor(gl_FragCoord.xy / max(pc.pixel, 1.0))) & 3;
        float bayer = (kBayer[cell.y * 4 + cell.x] + 0.5) / 16.0 - 0.5; // [-0.5,0.5]
        col += bayer * (post.retro.y / levels);   // nudge across the level boundary
        col = floor(col * levels + 0.5) / levels; // snap to the quantised palette
    }

    // --- Interlace flicker (PS2 alternating fields) ----------------------------
    // Dim every other (chunky) scanline, flipping which set each frame, for the
    // subtle vertical shimmer of an interlaced signal.
    if (post.retro.z > 0.0) {
        int line = int(gl_FragCoord.y / max(pc.pixel, 1.0));
        if ((line & 1) == int(post.retro2.x)) {
            col *= (1.0 - post.retro.z);
        }
    }

    outColor = vec4(col, 1.0);
}
