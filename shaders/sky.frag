#version 450

// Procedural sky. Daytime colour comes from analytic single-scattering
// (Rayleigh + Mie): per pixel, the colour depends on the angle between the view
// ray and the sun, so the sunset glow hugs the sun's azimuth while the opposite
// sky stays blue, the horizon pales with air mass, and the sun disc reddens as
// its light crosses more atmosphere. Below the horizon the analytic model
// blends into an authored night sky (dark gradient + moon). A legacy authored-
// gradient path remains behind a flag (zenith.w == 0).
//
// On top of the atmosphere sits the volumetric cloud layer: a raymarched slab
// (Beer extinction, Henyey-Greenstein phase, powder term, multi-scatter
// octaves), voxelised so the clouds read as chunky blocks. Clouds are
// lit by the same sun/moon state as everything else: warm direct light that
// reddens at dusk, ambient sampled from this very sky model (blue by day,
// pink at twilight), moonlit silver at night.
//
// All inputs come pre-converted to linear space (see vg::DayNight::state()).
// skyRadiance() returns HDR radiance; an exposure + ACES tonemap stage brings
// it to LDR before the sRGB target encodes it.
layout(set = 0, binding = 0) uniform SkyUBO {
    mat4 invViewProj; // inverse of proj * rotation-only view: NDC -> world dir
    vec4 sunDir;      // xyz: direction toward the sun,  w: dayBlend (0..1)
    vec4 moonDir;     // xyz: direction toward the moon, w: exposure
    vec4 zenith;      // rgb: night/legacy zenith,  w: 1 = analytic, 0 = legacy
    vec4 horizon;     // rgb: night/legacy horizon, w: sun intensity (HDR scale)
    vec4 sunDisc;     // rgb: disc colour above the atmosphere, w: cos(outer radius)
    vec4 moonDisc;    // rgb + w = cos(outer angular radius)
    vec4 params;      // x: glow, y: cosSunInner, z: cosMoonInner, w: mie G
    vec4 betaR;       // rgb: Rayleigh scattering coefficients, w: Mie coefficient
    vec4 tint;        // rgb: daytime re-tint (Sky option), w: sunset strength
    vec4 sunset;      // rgb: horizon sunset band (gold), w: band amount (0..1)
    vec4 sunsetMid;   // rgb: mid sunset band (orange), w: spare
    vec4 sunsetHigh;  // rgb: high-sky afterglow (pink/violet), w: spare
    vec4 ozone;       // rgb: Chappuis absorption coeff, w: strength
    vec4 cloudDusk;   // rgb: warm cloud-underside dusk tint, w: strength
    // --- Clouds (mirrors vg::CloudSystem::GpuParams) -------------------------
    vec4 cLayer;      // x bottom, y top (world Y), z aerial fade dist, w enabled
    vec4 cWind;       // xyz accumulated wind offset, w time
    vec4 cShape;      // x baseScale, y detailScale, z erosion, w densityScale
    vec4 cScat;       // x extinction, y HG g, z ambientScale, w powder
    vec4 cWeather;    // x coverage base, y type base, z weatherScale, w coverage amp
    vec4 cMarch;      // x primarySteps, y lightSteps, z lightStepLen, w type amp
    vec4 cSun;        // rgb sun colour above atmosphere, w direct intensity
    vec4 cMoon;       // rgb moonlight colour, w altitude sun-lift
    vec4 cAnti;       // rgb anti-solar twilight tint, w strength
    vec4 cMisc;       // x march max distance, y ms octaves, z ms falloff, w voxelise
    vec4 cDeck;       // x high-cirrus deck amount, y fog density, z star clarity, w storm base
    vec4 cFront0;     // x oldCov, y oldType, z oldStorm, w frontS (along-wind dist from cam)
    vec4 cFront1;     // x windDirX, y windDirZ, z frontWidth, w frontActive (1/0)
    vec4 camPos;      // xyz camera world position (rays start here for clouds)
    vec4 star;        // x siderealAngle, y latitude, z brightness, w milkyWay
    vec4 star2;       // x twinkleSpeed, y starExtinction, z planets, w shootingStars
} sky;

layout(set = 0, binding = 1) uniform sampler3D cloudBase;   // Perlin-Worley
layout(set = 0, binding = 2) uniform sampler3D cloudDetail; // Worley erosion
layout(set = 0, binding = 3) uniform sampler2D weatherMap;  // coverage/type field

layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 outColor;

// --- Analytic single scattering ----------------------------------------------

float rayleighPhase(float mu) {
    return 0.75 * (1.0 + mu * mu);
}

float miePhase(float mu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (12.5663706 * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

// Relative air mass for a ray at the given cosine of the zenith angle: ~1
// straight up, ~36 grazing the horizon (Kasten-Young). This is what makes the
// horizon pale and sunsets redden. Mirrored in C++ (DayNight) for terrain light.
float airMass(float cosZenith) {
    float c = clamp(cosZenith, 0.0, 1.0);
    float zenithDeg = degrees(acos(c));
    return 1.0 / (c + 0.15 * pow(93.885 - zenithDeg, -1.253));
}

// Single-scattered sun radiance along viewDir for a flat world. Two factors:
//   * the view path accumulates in-scatter, saturating with its optical depth
//     ((1 - e^-od): thin air overhead stays deep blue, the long horizon path
//     saturates toward the phase-weighted scatter colour -> the horizon pales);
//   * the *sun* path filters the light feeding that in-scatter (e^-od: blue is
//     scattered away first, so low sun feeds the sky warm light -> the sunset
//     glow, strongest toward the sun where the Mie phase peaks).
// `tint.w` (sunsetStrength) exaggerates the sun-path filtering for art control.
// HDR — tonemapped before output.
vec3 skyRadiance(vec3 viewDir, vec3 sunDir) {
    float mu     = dot(viewDir, sunDir);
    float amView = airMass(viewDir.y);
    float amSun  = airMass(max(sunDir.y, 0.0));

    vec3 betaTotal  = sky.betaR.rgb + vec3(sky.betaR.w);
    vec3 viewFactor = 1.0 - exp(-betaTotal * amView);
    vec3 sunFilter  = exp(-betaTotal * amSun * sky.tint.w);

    // The Mie haze sits low and sees the fully filtered sun (warm at dusk); the
    // Rayleigh blue is scattered high where the sun path is shorter, so it sees
    // a much weaker filter. Splitting them keeps the away-from-sun sky blue at
    // sunset instead of turning muddy green (a single-scatter artefact —
    // multiple scattering would do this for us in a full model).
    vec3 rayFilter = pow(sunFilter, vec3(0.4));

    vec3 rayleigh = (sky.betaR.rgb / betaTotal) * rayleighPhase(mu) * rayFilter;
    vec3 mie = vec3(sky.betaR.w / betaTotal) * miePhase(mu, sky.params.w) * sunFilter;

    // Ozone (Chappuis band): absorbs green/red along the long view path, leaving
    // the deep blue/purple of a real post-sunset sky. The view path lengthens
    // toward the horizon (amView), so the twilight band cools naturally.
    vec3 ozoneT = exp(-sky.ozone.rgb * sky.ozone.w * amView);

    return sky.horizon.w * (rayleigh + mie) * viewFactor * ozoneT;
}

// ACES filmic tonemap (Narkowicz fit): HDR radiance -> [0,1], still linear.
vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// A finished (tonemapped + night-blended) sky colour for an arbitrary direction.
// Used as the clouds' ambient term so their shadow side is genuinely sky-lit:
// blue at noon, pink-orange at dusk, near-black blue at night.
vec3 skyColorLDR(vec3 dirIn) {
    vec3 viewDir = normalize(vec3(dirIn.x, max(dirIn.y, 0.0), dirIn.z));
    vec3 day = skyRadiance(viewDir, sky.sunDir.xyz) * sky.tint.rgb;
    day = acesTonemap(day * sky.moonDir.w);
    vec3 night = mix(sky.horizon.rgb, sky.zenith.rgb, smoothstep(0.0, 0.45, dirIn.y));
    return mix(night, day, sky.sunDir.w);
}

// --- Clouds (volumetric density model) ---------------------------------------

float cremap(float x, float a, float b, float c, float d) {
    return c + (x - a) * (d - c) / max(b - a, 1e-4);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 443.8975);
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.x + p3.y) * p3.z);
}

// Weather-front blend at a world XZ: 1 on the "new" (incoming) side, 0 on the
// "old" side, with a soft boundary that sweeps along the wind direction. The
// boundary position (cFront0.w) advances from upwind to downwind over a weather
// transition, so a new regime (e.g. a storm) rolls in from the windward horizon
// and passes overhead instead of fading in uniformly. 1 everywhere when idle.
float weatherFront(vec2 xz) {
    if (sky.cFront1.w < 0.5) return 1.0;
    float s = dot(xz - sky.camPos.xz, sky.cFront1.xy); // along-wind dist from camera
    float width = max(sky.cFront1.z, 1.0);
    return 1.0 - smoothstep(sky.cFront0.w - width, sky.cFront0.w + width, s);
}

// Weather at a world XZ: (coverage, type), spatial variation around the evolving
// base values, scrolled by the wind. During a transition the base coverage/type
// cross-fade old->new across the moving front (so the change arrives from the
// wind direction).
vec2 cloudWeather(vec2 xz) {
    vec2 uv = (xz + sky.cWind.xz) * sky.cWeather.z;
    vec2 t = texture(weatherMap, uv).rg - 0.5;
    float nw = weatherFront(xz);
    float covBase  = mix(sky.cFront0.x, sky.cWeather.x, nw);
    float typeBase = mix(sky.cFront0.y, sky.cWeather.y, nw);
    return clamp(vec2(covBase + t.x * sky.cWeather.w,
                      typeBase + t.y * sky.cMarch.w),
                 0.0, 1.0);
}

// Vertical density profile by cloud type (issue #10 B), low ty -> high ty:
//   cirrus (thin high sheet) -> stratus (low flat) -> cumulus (puffy) ->
//   cumulonimbus (tower with an anvil that keeps 0.25 at the top).
float cloudProfile(float h, float ty) {
    float cirrus = clamp(cremap(h, 0.74, 0.82, 0.0, 1.0), 0.0, 1.0) *
                   clamp(cremap(h, 0.90, 0.98, 1.0, 0.0), 0.0, 1.0);
    float stratus = clamp(cremap(h, 0.0, 0.08, 0.0, 1.0), 0.0, 1.0) *
                    clamp(cremap(h, 0.10, 0.20, 1.0, 0.0), 0.0, 1.0);
    float cumulus = clamp(cremap(h, 0.0, 0.20, 0.0, 1.0), 0.0, 1.0) *
                    clamp(cremap(h, 0.45, 0.75, 1.0, 0.0), 0.0, 1.0);
    float cumulonim = clamp(cremap(h, 0.0, 0.10, 0.0, 1.0), 0.0, 1.0) *
                      clamp(cremap(h, 0.85, 1.0, 1.0, 0.25), 0.25, 1.0);
    // ty: 0 cirrus | ~0.25 stratus | ~0.6 cumulus | 1 cumulonimbus.
    float a = mix(cirrus, stratus, clamp(ty / 0.25, 0.0, 1.0));
    float b = mix(a, cumulus, clamp((ty - 0.25) / 0.35, 0.0, 1.0));
    return clamp(mix(b, cumulonim, clamp((ty - 0.60) / 0.40, 0.0, 1.0)), 0.0, 1.0);
}

// Cloud density at a world position. `cheap` skips the detail erosion (used for
// the light march, where the base shape is accurate enough).
float cloudDensity(vec3 p, vec2 w, float cheap) {
    // Drift the whole cloud field with the wind FIRST, *then* voxelise, so the
    // chunky cloud blocks translate across the sky from the wind direction
    // instead of flickering on a fixed world grid (wind.y == 0, so height stays
    // world-aligned). cMisc.w = cell size in blocks (0 = smooth).
    vec3 wp = p + sky.cWind.xyz;
    if (sky.cMisc.w > 0.0) {
        wp = (floor(wp / sky.cMisc.w) + 0.5) * sky.cMisc.w;
    }
    float h = clamp((wp.y - sky.cLayer.x) / max(sky.cLayer.y - sky.cLayer.x, 1e-3),
                    0.0, 1.0);
    float prof = cloudProfile(h, w.y);
    // Second deck (issue #10 C): a faint high cirrus sheet present *alongside*
    // the main cloud type, its amount set by the day's weather state — so a fair
    // day can show low cumulus and high cirrus at once without a second march.
    float cirrusDeck = clamp(cremap(h, 0.78, 0.86, 0.0, 1.0), 0.0, 1.0) *
                       clamp(cremap(h, 0.92, 0.99, 1.0, 0.0), 0.0, 1.0);
    prof = max(prof, cirrusDeck * sky.cDeck.x);
    // Storm overcast base: a thick flat nimbostratus deck that fills the whole
    // sky from the layer floor up to mid height, with the cumulonimbus towers
    // (from the high stormy type) rising out of it. The amount cross-fades across
    // the moving front like coverage/type, so the dark base rolls in with the
    // storm rather than popping in everywhere at once. 0 outside storms.
    float stormAmt = mix(sky.cFront0.z, sky.cDeck.w, weatherFront(p.xz));
    float stormDeck = clamp(cremap(h, 0.0, 0.06, 0.0, 1.0), 0.0, 1.0) *
                      clamp(cremap(h, 0.50, 0.75, 1.0, 0.25), 0.25, 1.0);
    prof = max(prof, stormDeck * stormAmt);
    // Cloud shape is a *changing weighted sum of noises*, never one field
    // everywhere (issue #10 B / user request). The primary sample is the warped
    // Perlin-Worley base; a second, decorrelated sample (offset + a different
    // scale) is blended in with a weight that drifts across the sky and slowly
    // over time, so different regions/moments read as different noise mixes.
    // The cheap light-march path keeps just the primary sample.
    float base = texture(cloudBase, wp * sky.cShape.x).r;
    if (cheap <= 0.5) {
        float nB = texture(cloudBase, (wp * 2.1 + vec3(37.0, 11.0, 53.0)) * sky.cShape.x).r;
        float region = 0.5 + 0.5 * sin(dot(wp.xz, vec2(0.011, 0.008)) + sky.cWind.w * 0.04);
        base = mix(base, nB, mix(0.18, 0.50, region)); // drifting blend weight
    }
    // The power curve calibrates yaml coverage to visual sky coverage for this
    // noise field's value distribution:
    // 0.3 = sparse, 0.5 = scattered, 0.7 = broken, 0.85+ = overcast.
    float shape = cremap(base * prof, pow(1.0 - w.x, 1.6), 1.0, 0.0, 1.0);
    if (shape <= 0.0) return 0.0;
    if (cheap > 0.5) return clamp(shape, 0.0, 1.0) * sky.cShape.w;
    // Detail erosion with altitude-dependent character (issue #10 B), reusing the
    // single detail fetch (no extra cost):
    //  * near the top, compress the sampling along the wind (x) so high cloud
    //    shreds into wind-aligned streaks (cirrus wisps);
    //  * at mid height, push erosion harder so mid decks break into many small
    //    rounded puffs (altocumulus 'mackerel' rows).
    vec3 dscale = vec3(sky.cShape.y);
    float streak = smoothstep(0.55, 0.90, h);
    dscale.x *= mix(1.0, 0.35, streak); // elongate features along the wind
    // Sample the detail in the SAME voxelised drift frame as the base, so each
    // cloud block stays a coherent solid unit that travels as a whole instead of
    // boiling in place. The cloudscape still evolves — as the field drifts it
    // moves through the spatially-varying weather map (different coverage/type),
    // so clouds genuinely grow and dissipate, with no in-block shimmer.
    float det = texture(cloudDetail, wp * dscale).r;
    det = mix(det, 1.0 - det, clamp(h * 3.0, 0.0, 1.0)); // wispy base, billowy top
    float erode = sky.cShape.z * (1.0 + 0.6 * (1.0 - abs(h - 0.5) * 2.0));
    shape = cremap(shape, det * erode, 1.0, 0.0, 1.0);
    // Keep the storm overcast base a solid, gap-free sheet: the erosion above can
    // punch holes through it, so re-assert a smooth full-sky floor from the storm
    // deck (varies with the base noise but never falls to zero → no holes).
    float stormFill = stormDeck * stormAmt * (0.45 + 0.55 * base);
    shape = max(shape, stormFill);
    return clamp(shape, 0.0, 1.0) * sky.cShape.w;
}

// Raymarch the cloud slab. `bg` is the finished sky behind the clouds (for the
// aerial-perspective fade). Returns premultiplied colour + alpha.
vec4 marchClouds(vec3 ro, vec3 rd, vec3 bg) {
    if (sky.cLayer.w < 0.5 || sky.zenith.w < 0.5) return vec4(0.0);

    // Slab entry/exit.
    float bot = sky.cLayer.x, top = sky.cLayer.y;
    float t0, t1;
    if (abs(rd.y) < 1e-4) {
        if (ro.y < bot || ro.y > top) return vec4(0.0);
        t0 = 0.0;
        t1 = sky.cMisc.x;
    } else {
        float ta = (bot - ro.y) / rd.y;
        float tb = (top - ro.y) / rd.y;
        t0 = max(min(ta, tb), 0.0);
        t1 = min(max(ta, tb), sky.cMisc.x);
        if (t1 <= t0) return vec4(0.0);
    }

    float steps = max(sky.cMarch.x, 8.0);
    float dt = (t1 - t0) / steps;
    float t = t0 + dt * hash12(gl_FragCoord.xy); // static jitter; the frame-wide
                                                 // dither + pixelation hide it

    // Height-aware sky ambient: bluer toward the cloud top (zenith), warmer at
    // the base (horizon in the view direction — at twilight this is where the
    // pink/orange lives). Hoisted out of the loop.
    vec3 ambZen = skyColorLDR(vec3(0.0, 1.0, 0.0));
    vec3 ambHor = skyColorLDR(normalize(vec3(rd.x, 0.08, rd.z)));
    // Belt of Venus: opposite a low sun, bias the ambient toward the pink
    // anti-solar tint over the blue earth-shadow band.
    float twilight = clamp(1.0 - abs(sky.sunDir.y) / 0.30, 0.0, 1.0);
    float anti = smoothstep(0.2, 1.0,
                            -dot(normalize(rd.xz + vec2(1e-5)), normalize(sky.sunDir.xz + vec2(1e-5))));
    ambHor = mix(ambHor, sky.cAnti.rgb * max(ambHor.r, max(ambHor.g, ambHor.b)) * 1.6,
                 anti * twilight * sky.cAnti.w);

    vec3 betaTotal = sky.betaR.rgb + vec3(sky.betaR.w);
    float night = 1.0 - sky.sunDir.w; // 0 day .. 1 night (for the moonless floor)

    float transmit = 1.0;
    vec3 scatter = vec3(0.0);
    float firstHit = -1.0;

    for (int i = 0; i < 128; ++i) {
        if (float(i) >= steps || transmit < 0.01) break;
        vec3 p = ro + rd * t;
        vec2 w = cloudWeather(p.xz);
        float d = cloudDensity(p, w, 0.0);
        if (d > 0.0) {
            if (firstHit < 0.0) firstHit = t;
            float h = clamp((p.y - bot) / max(top - bot, 1e-3), 0.0, 1.0);

            // Direct light: the sun seen from the cloud's *altitude* — high
            // samples keep catching reddened light after the ground darkens
            // (the pink-cloud window at dusk). Falls back to moonlight at night.
            float elevC = sky.sunDir.y + sky.cMoon.w * h;
            float sunVis = smoothstep(-0.04, 0.04, elevC);
            vec3 sunCol = sky.cSun.rgb * exp(-betaTotal * airMass(max(elevC, 0.0))) *
                          sky.cSun.w * sunVis;
            float moonVis = smoothstep(-0.02, 0.10, sky.moonDir.y) * (1.0 - sunVis);
            vec3 moonCol = sky.cMoon.rgb * moonVis * 0.5;
            vec3 lDir = (sunVis >= moonVis) ? sky.sunDir.xyz : sky.moonDir.xyz;
            vec3 lightCol = sunCol + moonCol;

            // Light march toward the active light (cheap density: base only).
            float ld = 0.0;
            float ls = sky.cMarch.z * 0.5;
            for (int j = 0; j < 6; ++j) {
                if (float(j) >= sky.cMarch.y) break;
                ld += cloudDensity(p + lDir * ls, w, 1.0) * sky.cMarch.z;
                ls += sky.cMarch.z;
            }

            // Multi-scatter octaves (Wrenninge): decreasing extinction and
            // phase eccentricity so warm light bleeds *into* the body instead
            // of only rimming it. The difference between a flat orange blob
            // and a glowing sunset cloud.
            float mu = dot(rd, lDir);
            float energy = 0.0;
            float aw = 1.0, bw = 1.0, g = sky.cScat.y;
            for (int o = 0; o < 4; ++o) {
                if (float(o) >= sky.cMisc.y) break;
                energy += aw * exp(-ld * sky.cScat.x * bw) *
                          mix(0.0796, miePhase(mu, g), 0.85);
                aw *= sky.cMisc.z;
                bw *= sky.cMisc.z;
                g *= 0.5;
            }

            // Powder: dark crisp edges where density is low toward the light.
            float powder = 1.0 - exp(-d * sky.cScat.x * 2.0);
            powder = mix(1.0, powder, sky.cScat.w);

            // A faint moonless night glow so clouds stay *slightly* visible as
            // silhouettes against the dark sky even when the moon is down — the
            // sky-sampled ambient alone is near-black at night.
            vec3 ambient = mix(ambHor, ambZen, h) * sky.cScat.z
                         + sky.cMoon.rgb * (0.018 * night);
            vec3 lum = lightCol * energy * powder + ambient;

            // Warm underside dusk glow (issue #10 A): lit cloud bases blush
            // pink/red when the sun is low, independent of the near-white sun
            // disc colour. Strongest at the base (1-h) and scaled by how lit the
            // sample is (energy) so only sunward faces catch it.
            float dusk = clamp(1.0 - abs(sky.sunDir.y) / 0.28, 0.0, 1.0);
            lum += sky.cloudDusk.rgb * sky.cloudDusk.w * dusk * (1.0 - h) * energy;

            float dext = exp(-d * sky.cScat.x * dt);
            scatter += transmit * (1.0 - dext) * lum;
            transmit *= dext;
        }
        t += dt;
    }

    float alpha = 1.0 - transmit;
    if (alpha <= 0.001) return vec4(0.0);

    // Aerial perspective: distant clouds dissolve into the sky behind them
    // instead of sitting pasted on top of it.
    float ap = exp(-max(firstHit, 0.0) / max(sky.cLayer.z, 1.0));
    scatter = mix(bg * alpha, scatter, ap);
    return vec4(scatter, alpha);
}

// 4x4 ordered (Bayer) dither matrix — the same one the terrain uses.
const float kBayer[16] = float[16](
     0.0,  8.0,  2.0, 10.0,
    12.0,  4.0, 14.0,  6.0,
     3.0, 11.0,  1.0,  9.0,
    15.0,  7.0, 13.0,  5.0);

// =============================================================================
//  Night sky: procedural stars + Milky Way (driven by DayNight via sky.star)
// =============================================================================
vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// t 0..1 -> blue-white .. white .. yellow .. orange (rough stellar colour).
vec3 blackbody(float t) {
    return mix(vec3(0.62, 0.74, 1.0),
               mix(vec3(1.0), vec3(1.0, 0.78, 0.55), t), smoothstep(0.0, 1.0, t));
}

// Rotate a direction about the (latitude-tilted) celestial pole (Rodrigues).
vec3 celestialRotate(vec3 dir, float ang, float latitude) {
    vec3 axis = normalize(vec3(0.0, sin(latitude), cos(latitude)));
    float c = cos(ang), s = sin(ang);
    return dir * c + cross(axis, dir) * s + axis * dot(axis, dir) * (1.0 - c);
}

// One cell-hash star layer: hash the nearest cell, place a jittered star, soft
// power-law-magnitude dot. The radius is floored at ~1.5 px (fwidth) so a star is
// never a single sub-pixel dot — that is what kills the shimmer/crawl on panning.
vec3 starLayer(vec3 dir, float density, float threshold, float time) {
    vec3 id  = round(dir * density);
    vec3 rnd = hash33(id);
    if (rnd.x < threshold) return vec3(0.0);            // most cells are empty sky
    vec3 sdir = normalize(id + (rnd - 0.5) * 0.9);
    float d   = distance(dir, sdir);
    float mag = pow(rnd.y, 6.0);                        // few bright, many faint
    // Bigger discs with a higher min-size floor: a star always covers several
    // low-res pixels with a soft edge, so the ordered dither can shade it
    // smoothly instead of chewing a 1px dot into grain.
    float radius = max(mix(0.022, 0.0070, mag), 3.2 * fwidth(d));
    float core = smoothstep(radius, 0.0, d) * mag;
    float twinkle = 0.78 + 0.22 * sin(time * sky.star2.x + rnd.z * 6.2831);
    return core * twinkle * blackbody(rnd.z);
}

// Cheap trilinear value noise from the star hash, for the Milky Way clumps/dust.
float vnoise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(mix(hash33(i + vec3(0,0,0)).x, hash33(i + vec3(1,0,0)).x, f.x),
                      mix(hash33(i + vec3(0,1,0)).x, hash33(i + vec3(1,1,0)).x, f.x), f.y),
                  mix(mix(hash33(i + vec3(0,0,1)).x, hash33(i + vec3(1,0,1)).x, f.x),
                      mix(hash33(i + vec3(0,1,1)).x, hash33(i + vec3(1,1,1)).x, f.x), f.y), f.z);
    return a;
}

// A faint glowing band along the galactic plane, clumped by noise with darker
// dust lanes carved out. `dir` is already celestial-rotated.
vec3 milkyWay(vec3 dir) {
    const vec3 pole = normalize(vec3(0.30, 0.40, 0.85)); // galactic pole (art tilt)
    float gp   = dot(dir, pole);
    float band = exp(-gp * gp * 22.0);                   // glow tight to the plane
    // Lower-frequency clumps and dust than before so the band is broad and soft
    // instead of high-frequency speckle the dither turns to grain. The dust lanes
    // only *dim* the glow (mix floor 0.5) rather than punching hard black holes
    // in it, which is what read as messy.
    float clump = vnoise3(dir * 4.0) * 0.65 + vnoise3(dir * 8.0) * 0.35;
    float dust  = vnoise3(dir * 6.0 + 5.0);
    float m = band * clump * mix(0.5, 1.0, smoothstep(0.25, 0.85, dust));
    vec3 col = mix(vec3(0.55, 0.60, 0.85), vec3(0.88, 0.80, 0.68), clump);
    return col * m;
}

// Planets: a few bright, *steady* (non-twinkling) points on fixed celestial
// directions near the ecliptic, with distinct colours. Rotate with the stars.
vec3 planets(vec3 dir, float bright) {
    if (bright <= 0.0) return vec3(0.0);
    const vec3 dirs[4] = vec3[4](vec3( 0.85, 0.32,  0.41), vec3(-0.55, 0.18, -0.81),
                                 vec3( 0.10, 0.55,  0.83), vec3(-0.78, 0.40,  0.48));
    const vec3 cols[4] = vec3[4](vec3(1.0, 0.96, 0.85),    vec3(1.0, 0.55, 0.40),   // Venus, Mars
                                 vec3(0.95, 0.93, 0.82),   vec3(1.0, 0.85, 0.55));  // Jupiter, Saturn
    const float mags[4] = float[4](1.0, 0.55, 0.8, 0.5);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float d = distance(dir, normalize(dirs[i]));
        float r = max(0.0016, 1.5 * fwidth(d));
        acc += smoothstep(r, 0.0, d) * cols[i] * mags[i];
    }
    return acc * bright;
}

// Shooting stars: a few time-sliced meteor "slots", each spawning an occasional
// streak that arcs across the sky and fades. View-space (not celestial), since
// they're transient. `rate` 0 = off .. 1 = frequent.
vec3 shootingStars(vec3 dir, float time, float rate) {
    if (rate <= 0.0) return vec3(0.0);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 3; i++) {
        float period = mix(20.0, 6.0, clamp(rate, 0.0, 1.0)); // seconds/slot
        float epoch  = floor(time / period) + float(i) * 31.0;
        vec3  h      = hash33(vec3(epoch, float(i) * 7.0, 3.0));
        if (h.x > 0.55) continue;                  // most periods: no meteor
        float ph = fract(time / period) / 0.10;    // life over the first 10% of the period
        if (ph > 1.0) continue;
        float life = sin(ph * 3.14159);            // fade in/out
        vec3 a  = normalize(vec3(h.y * 2.0 - 1.0, 0.30 + h.z * 0.6, hash33(h).x * 2.0 - 1.0));
        vec3 hd = normalize(cross(a, vec3(0.0, 1.0, 0.0)) + (hash33(a) - 0.5) * 0.3);
        const float travel = 0.55;                 // angular length of the streak
        float s = 0.0;
        for (int j = 0; j < 6; j++) {              // head + 5 fading tail samples
            vec3 p  = normalize(a + hd * ((ph - float(j) * 0.03) * travel));
            float dd = distance(dir, p);
            s = max(s, smoothstep(0.004, 0.0, dd) * (1.0 - float(j) / 6.0));
        }
        acc += s * life * vec3(0.85, 0.92, 1.0);
    }
    return acc;
}

void main() {
    // Reconstruct the world-space view ray for this pixel. The view matrix had
    // its translation stripped, so the unprojected far-plane point *is* a ray
    // direction from the camera.
    vec4 t = sky.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(t.xyz / t.w);

    vec3 col;
    if (sky.zenith.w > 0.5) {
        // --- Analytic day sky, blended into the authored night sky ----------
        // Clamp the view ray to the horizon for scattering: below it there is
        // no more atmosphere in this flat world, just the ground fade.
        vec3 viewDir = normalize(vec3(dir.x, max(dir.y, 0.0), dir.z));
        vec3 day = skyRadiance(viewDir, sky.sunDir.xyz) * sky.tint.rgb;
        day = acesTonemap(day * sky.moonDir.w); // exposure, then tonemap (LDR)

        // Multi-band sunset (issue #10 A): three elevation zones — a hot gold
        // strip at the horizon, an orange band above it, and a pink/violet
        // afterglow high in the sky that spreads wider than the sun's azimuth
        // (Belt of Venus). Painted over the analytic sky while the sun is low.
        float mu = dot(viewDir, sky.sunDir.xyz);
        float e  = viewDir.y;
        // Colour by elevation: horizon -> mid -> high afterglow going up. Wider
        // reach than before so the day's colour fills more of the sky.
        vec3 sCol = mix(sky.sunset.rgb, sky.sunsetMid.rgb, smoothstep(0.02, 0.18, e));
        sCol      = mix(sCol, sky.sunsetHigh.rgb, smoothstep(0.16, 0.70, e));
        // Sun-azimuth focus: fairly tight for the low bands, broad for the high
        // afterglow so it wraps across (and a little opposite) the sky.
        float toward    = clamp(mu * 0.5 + 0.5, 0.0, 1.0);
        float focusLow  = pow(toward, 2.2);
        float focusHigh = 0.45 + 0.55 * toward;
        float focus     = mix(focusLow, focusHigh, smoothstep(0.16, 0.55, e));
        // Amount peaks as the sun crosses the horizon; fade the very top out so
        // the zenith stays sky. `sunset.a` carries the per-day drama (>1), and the
        // mix is clamped, so a dramatic dusk paints the chosen colour near-fully
        // instead of merely tinting the analytic orange.
        float band = sky.sunset.a * focus * (1.0 - smoothstep(0.72, 1.05, e) * 0.45);
        day = mix(day, sCol * (0.72 + 0.5 * focus), clamp(band, 0.0, 1.0));

        vec3 night = mix(sky.horizon.rgb, sky.zenith.rgb,
                         smoothstep(0.0, 0.45, dir.y));
        col = mix(night, day, sky.sunDir.w);
    } else {
        // --- Legacy authored gradient (useAnalyticSky: false) ----------------
        col = mix(sky.horizon.rgb, sky.zenith.rgb, smoothstep(0.0, 0.45, dir.y));
    }

    // Below the horizon fade darker (only visible at the world's edge).
    col = mix(col, col * 0.35, smoothstep(0.0, 0.30, -dir.y));

    // --- Night sky: stars + Milky Way (added before the discs & clouds so the
    // moon covers the stars behind it and clouds occlude them) -----------------
    float night = 1.0 - sky.sunDir.w; // sunDir.w = dayBlend (1 day .. 0 night)
    if (night > 0.001) {
        // Wheel the field about the celestial pole with the DayNight clock.
        vec3 sdir = celestialRotate(dir, sky.star.x, sky.star.y);
        float tw = sky.cWind.w; // shared scene time (s), for the twinkle
        // Two sparse layers only: a scatter of bright stars and a thinner layer
        // of fainter ones. The old third (density-1500) layer packed in tiny
        // sub-pixel stars that the low-res dither chewed into grain; dropping it
        // and raising the empty-cell thresholds leaves a cleaner, sparser field.
        vec3 stars = starLayer(sdir, 260.0, 0.96, tw)
                   + starLayer(sdir, 560.0, 0.985, tw);
        stars *= sky.star.z;                 // brightness
        vec3 mw = milkyWay(sdir) * sky.star.w;

        // Planets ride with the stars but never twinkle; meteors are transient
        // and view-space.
        vec3 plan = planets(sdir, sky.star2.z);

        // Gating: only at night, none below the horizon, dimmed/reddened low in
        // the sky, thinned near a bright moon.
        float horizonFade = smoothstep(-0.02, 0.08, dir.y);
        float extinct = exp(-airMass(dir.y) * sky.star2.y);
        float moonWash = 1.0 - 0.7 * smoothstep(0.9, 1.0, dot(dir, sky.moonDir.xyz));
        // Moon-phase wash (issue #10 F): a full moon (illum -> 1) washes the whole
        // field out; a new moon leaves it brilliant. illum from the sun/moon angle.
        float moonIllum = 0.5 * (1.0 - dot(sky.sunDir.xyz, sky.moonDir.xyz));
        float moonUp    = smoothstep(-0.10, 0.12, sky.moonDir.y);
        moonWash *= 1.0 - 0.6 * moonIllum * moonUp;

        // Weather-driven visibility from the *shared* cloud weather (coverage near
        // the camera): hazy/overcast nights wash the field out, the faint Milky Way
        // first (clarity^2), then the dimmer stars (clarity).
        vec2 wx = cloudWeather(sky.camPos.xz);
        float turbidity = clamp(0.10 + 0.55 * wx.x, 0.0, 1.0);
        float clarity   = 1.0 - turbidity;

        // Stars + planets + Milky Way share the night/horizon/extinction gating;
        // planets are bright enough to survive more haze (clarity^0.5). The
        // weather state's global star clarity (cDeck.z) thins the field on
        // overcast/foggy/stormy nights on top of the local haze (issue #10 C/F).
        float stateClear = mix(0.4, 1.0, clamp(sky.cDeck.z, 0.0, 1.0));
        vec3 nightSky = ((stars + plan * sqrt(clarity)) + mw * clarity)
                        * night * horizonFade * extinct * moonWash * clarity * stateClear;
        // Meteors streak over the whole sky; gate by night + horizon only.
        nightSky += shootingStars(dir, sky.cWind.w, sky.star2.w) * night * horizonFade;
        // A faint moonlit-haze glow lifts the floor under overcast and washes the
        // very faintest stars near the horizon.
        float skyGlow = 0.02 * turbidity * horizonFade * night;
        col += max(nightSky - skyGlow, 0.0) + skyGlow * sky.moonDisc.rgb * 0.5;
    }

    // Sun: the disc colour is filtered by the transmittance through the air
    // mass its light crossed (with the same sunsetStrength exaggeration as the
    // sky), so it whitens at noon and goes deep orange at sunset.
    vec3 betaTotal = sky.betaR.rgb + vec3(sky.betaR.w);
    vec3 sunCol = sky.sunDisc.rgb;
    if (sky.zenith.w > 0.5) {
        sunCol *= exp(-betaTotal * airMass(max(sky.sunDir.y, 0.0)) * sky.tint.w);
        // Make the daytime sun read as a bright, near-white disc: as it climbs
        // above the horizon, whiten it and push it past 1 so the core clips to a
        // brilliant white. Low in the sky (sunHigh -> 0) it keeps its warm gold /
        // sunset-orange, untouched.
        float sunHigh = smoothstep(0.0, 0.30, sky.sunDir.y);
        sunCol = mix(sunCol, vec3(1.0), 0.6 * sunHigh) * mix(1.0, 4.0, sunHigh);
    }
    float ds = dot(dir, sky.sunDir.xyz);
    col += sunCol * pow(max(ds, 0.0), 350.0) * sky.params.x; // tight halo
    col = mix(col, sunCol, smoothstep(sky.sunDisc.w, sky.params.y, ds));

    // Moon: a phase-shaded disc (issue #10 F). Each point of the visible disc is a
    // point on the moon's sphere; it is lit where its outward normal faces the sun,
    // so the terminator sweeps the disc into a crescent/gibbous/full shape as the
    // sun-moon geometry changes over the month. Only the *lit* part is painted —
    // the dark side keeps the sky behind it (a faint earthshine at night only), so
    // a new moon is invisible rather than a black disc, especially in daylight.
    float dm = dot(dir, sky.moonDir.xyz);
    if (dm > sky.moonDisc.w && sky.moonDir.y > -0.15) {
        vec3 mUp    = abs(sky.moonDir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        vec3 mRight = normalize(cross(sky.moonDir.xyz, mUp));
        vec3 mTop   = cross(mRight, sky.moonDir.xyz);
        float discR = sqrt(max(1.0 - sky.moonDisc.w * sky.moonDisc.w, 1e-5));
        vec2  p     = vec2(dot(dir, mRight), dot(dir, mTop)) / discR; // ~[-1,1] over the disc
        float z     = sqrt(max(1.0 - dot(p, p), 0.0));                // toward the camera
        vec3  n     = mRight * p.x + mTop * p.y - sky.moonDir.xyz * z; // sphere outward normal
        float lit   = smoothstep(-0.07, 0.07, dot(n, sky.sunDir.xyz));
        float moonNight = 1.0 - sky.sunDir.w;                          // 1 night .. 0 day
        // Coverage: the lit crescent always shows; the dark side only as a faint
        // night-time earthshine (0 in daylight -> no black disc). Hidden as the
        // moon sinks below the horizon.
        float alpha = smoothstep(sky.moonDisc.w, sky.params.z, dm)
                    * clamp(lit + 0.07 * moonNight, 0.0, 1.0)
                    * smoothstep(-0.10, 0.04, sky.moonDir.y);
        col = mix(col, sky.moonDisc.rgb, alpha);
    }

    // --- Volumetric clouds, over the discs (they occlude the sun/moon) -------
    vec4 clouds = marchClouds(sky.camPos.xyz, dir, col);
    col = col * (1.0 - clouds.a) + clouds.rgb;

    // --- Posterise + ordered dither to match the dithered terrain ------------
    // The sky is drawn into the same low-res offscreen, so gl_FragCoord here is
    // the low-res pixel and the 4x4 Bayer cells line up with the world's stipple.
    // The sky is a full-colour gradient (no shadow ramp like the terrain), so we
    // quantise each channel; the ordered dither breaks the bands so the gradient
    // still reads as smooth.
    //
    // Quantise in PERCEPTUAL (gamma) space, not linear. The offscreen is an sRGB
    // target so `col` is still linear here: evenly-spaced *linear* levels put
    // almost the entire dark night sky into the bottom one or two steps and then
    // dither across a huge perceptual gap — that is the "messy" grainy night.
    // sqrt() (cheap gamma 2.0) spreads the levels so the darks get fine, smooth
    // steps, while the bright daytime gradient stays as chunky as before.
    const float kLevels = 8.0;
    int bx = int(gl_FragCoord.x) & 3;
    int by = int(gl_FragCoord.y) & 3;
    float thresh = (kBayer[by * 4 + bx] + 0.5) / 16.0;
    float n = kLevels - 1.0;
    vec3 g = sqrt(clamp(col, 0.0, 1.0)); // linear -> perceptual
    vec3 s = g * n;
    vec3 lo = floor(s);
    g = (lo + step(thresh, s - lo)) / n;
    col = g * g;                         // perceptual -> linear

    outColor = vec4(col, 1.0);
}
