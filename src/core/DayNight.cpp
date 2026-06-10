#include "core/DayNight.h"

#include "core/Palette.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vg {

namespace {

constexpr float kTwoPi = 6.28318530718f;

float smoothstepf(float lo, float hi, float x) {
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Deterministic per-day jitter in [-1, 1) from a day index + a salt (+ a seed).
// An fmix-style integer avalanche, so each day's "weather" is stable and
// reproducible but unrelated to its neighbours.
float weatherJitter(int day, uint32_t salt, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(day) * 0x9e3779b9u ^ (salt * 0x85ebca6bu) ^ seed;
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x00800000) - 1.0f;
}

// Relative air mass for a ray leaving the ground at the given cosine of the
// zenith angle: ~1 looking straight up, ~36 grazing the horizon (Kasten-Young).
// Must match airMass() in sky.frag so terrain light and sky colour agree.
float airMass(float cosZenith) {
    const float c = std::clamp(cosZenith, 0.0f, 1.0f);
    const float zenithDeg = glm::degrees(std::acos(c));
    return 1.0f / (c + 0.15f * std::pow(93.885f - zenithDeg, -1.253f));
}

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Rotate a colour's hue by `a` radians about the luminance axis (classic
// YIQ-style matrix). Drives the per-twilight palette variety. Clamped >= 0.
glm::vec3 hueRotate(const glm::vec3& c, float a) {
    const float U = std::cos(a), W = std::sin(a);
    glm::vec3 o;
    o.r = (0.299f + 0.701f * U + 0.168f * W) * c.r +
          (0.587f - 0.587f * U + 0.330f * W) * c.g +
          (0.114f - 0.114f * U - 0.497f * W) * c.b;
    o.g = (0.299f - 0.299f * U - 0.328f * W) * c.r +
          (0.587f + 0.413f * U + 0.035f * W) * c.g +
          (0.114f - 0.114f * U + 0.292f * W) * c.b;
    o.b = (0.299f - 0.300f * U + 1.250f * W) * c.r +
          (0.587f - 0.588f * U - 1.050f * W) * c.g +
          (0.114f + 0.886f * U - 0.203f * W) * c.b;
    return glm::max(o, glm::vec3(0.0f));
}

// Push saturation away from the colour's own luminance (k > 1 = more vivid).
glm::vec3 saturate(const glm::vec3& c, float k) {
    const float l = glm::dot(c, glm::vec3(0.299f, 0.587f, 0.114f));
    return glm::max(glm::vec3(l) + (c - glm::vec3(l)) * k, glm::vec3(0.0f));
}

// A colour entry: either a palette name ("amber") or a "#RRGGBB" literal.
// Returned in linear space, ready for the sRGB render target.
glm::vec3 parseColor(const YAML::Node& node, const char* key, const Palette& palette,
                     const std::string& file) {
    if (!node[key]) {
        throw std::runtime_error("DayNight: '" + file + "' is missing colour '" + key + "'");
    }
    const std::string v = node[key].as<std::string>();
    if (!v.empty() && v.front() == '#') {
        if (v.size() != 7) {
            throw std::runtime_error("DayNight: bad hex colour '" + v + "' for " + key);
        }
        auto ch = [&](int o) {
            return srgbToLinear(static_cast<float>(std::stoi(v.substr(o, 2), nullptr, 16)) / 255.0f);
        };
        return {ch(1), ch(3), ch(5)};
    }
    if (!palette.has(v)) {
        throw std::runtime_error("DayNight: unknown palette colour '" + v + "' for " + key);
    }
    return palette.linear(v);
}

} // namespace

DayNight::DayNight(const std::string& skyFile, const Palette& palette) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(skyFile);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("DayNight: failed to load '" + skyFile + "': " + e.what());
    }

    auto num = [&](const char* key, float fallback) {
        return root[key] ? root[key].as<float>() : fallback;
    };
    // Optional colour: parse if present, else keep the fallback (a present but
    // malformed value still throws via parseColor).
    auto colorOr = [&](const char* key, glm::vec3 fb) {
        return root[key] ? parseColor(root, key, palette, skyFile) : fb;
    };

    dayZenith_     = parseColor(root, "dayZenith", palette, skyFile);
    dayHorizon_    = parseColor(root, "dayHorizon", palette, skyFile);
    nightZenith_   = parseColor(root, "nightZenith", palette, skyFile);
    nightHorizon_  = parseColor(root, "nightHorizon", palette, skyFile);
    sunsetHorizon_ = parseColor(root, "sunsetHorizon", palette, skyFile);
    sunsetMid_     = colorOr("sunsetMid", sunsetHorizon_);
    sunsetHigh_    = colorOr("sunsetHigh", sunsetHorizon_);
    cloudDusk_     = colorOr("cloudDuskTint", glm::vec3(1.0f, 0.42f, 0.24f));
    sunColor_      = parseColor(root, "sunColor", palette, skyFile);
    moonColor_     = parseColor(root, "moonColor", palette, skyFile);
    sunlightDay_   = parseColor(root, "sunlightDay", palette, skyFile);
    sunlightSunset_ = parseColor(root, "sunlightSunset", palette, skyFile);
    moonlight_     = parseColor(root, "moonlight", palette, skyFile);

    sunSizeDeg_    = num("sunSizeDeg", sunSizeDeg_);
    moonSizeDeg_   = num("moonSizeDeg", moonSizeDeg_);
    sunGlow_       = num("sunGlow", sunGlow_);
    dayAmbient_    = num("dayAmbient", dayAmbient_);
    nightAmbient_  = num("nightAmbient", nightAmbient_);
    moonIntensity_ = num("moonIntensity", moonIntensity_);

    // Analytic atmosphere knobs (all optional; defaults give an earth-like sky).
    useAnalytic_ = root["useAnalyticSky"] ? root["useAnalyticSky"].as<bool>() : true;
    if (root["betaR"] && root["betaR"].IsSequence() && root["betaR"].size() == 3) {
        betaR_ = {root["betaR"][0].as<float>(), root["betaR"][1].as<float>(),
                  root["betaR"][2].as<float>()};
    }
    betaM_          = num("betaM", betaM_);
    turbidity_      = num("turbidity", turbidity_);
    mieG_           = std::clamp(num("mieG", mieG_), 0.0f, 0.99f);
    sunIntensity_   = num("sunIntensity", sunIntensity_);
    exposure_       = num("exposure", exposure_);
    sunsetStrength_ = num("sunsetStrength", sunsetStrength_);
    dayVariation_   = std::max(0.0f, num("dayVariation", dayVariation_));

    // Multi-band sunset (issue #10 A): ozone, cloud dusk glow, and the per-day
    // sunset-mood presets.
    if (root["ozone"] && root["ozone"].IsSequence() && root["ozone"].size() == 3) {
        ozone_ = {root["ozone"][0].as<float>(), root["ozone"][1].as<float>(),
                  root["ozone"][2].as<float>()};
    }
    ozoneStrength_     = std::max(0.0f, num("ozoneStrength", ozoneStrength_));
    cloudDuskStrength_ = std::max(0.0f, num("cloudDuskStrength", cloudDuskStrength_));
    sunsetDrama_       = std::max(0.0f, num("sunsetDrama", sunsetDrama_));
    sunsetHueVary_     = std::max(0.0f, num("sunsetHueVary", sunsetHueVary_));
    if (root["sunsetPresets"] && root["sunsetPresets"].IsSequence()) {
        for (const auto& p : root["sunsetPresets"]) {
            sunsetPresets_.push_back(
                {p["high"]    ? parseColor(p, "high", palette, skyFile)    : sunsetHigh_,
                 p["mid"]     ? parseColor(p, "mid", palette, skyFile)     : sunsetMid_,
                 p["horizon"] ? parseColor(p, "horizon", palette, skyFile) : sunsetHorizon_});
        }
    }

    // Night sky (latitude given in degrees in the yaml for readability).
    latitudeRad_    = glm::radians(num("latitude", glm::degrees(latitudeRad_)));
    siderealRate_   = num("siderealRate", siderealRate_);
    starBrightness_ = std::max(0.0f, num("starBrightness", starBrightness_));
    milkyWay_       = std::max(0.0f, num("milkyWay", milkyWay_));
    twinkleSpeed_   = std::max(0.0f, num("twinkleSpeed", twinkleSpeed_));
    starExtinction_ = std::max(0.0f, num("starExtinction", starExtinction_));
    planets_        = std::max(0.0f, num("planets", planets_));
    shootingStars_  = std::max(0.0f, num("shootingStars", shootingStars_));
    // The yaml's dayZenith doubles as the neutral reference for the in-game Sky
    // option: override == reference -> tint (1,1,1) -> untinted analytic sky.
    dayZenithRef_ = dayZenith_;

    setHour(num("startHour", 9.0f));
}

void DayNight::advance(float dt) {
    if (!running_ || dayLenSec_ <= 0.0f) {
        return;
    }
    t_ += dt / dayLenSec_;
    const float wraps = std::floor(t_);
    days_ += static_cast<int>(wraps); // count completed days for totalDays()
    t_ -= wraps;                      // wrap to [0,1)
}

void DayNight::setHour(float h) {
    t_ = std::clamp(h, 0.0f, 24.0f) / 24.0f;
    t_ -= std::floor(t_);
}

void DayNight::setDayLengthMinutes(float minutes) {
    dayLenSec_ = std::max(0.1f, minutes) * 60.0f;
}

DayNight::SkyState DayNight::state() const {
    SkyState s{};

    // Sun arc: rises in +X at 6:00, peaks overhead at noon, sets in -X at 18:00.
    // A small fixed Z lean keeps it off the exact axis so faces light unevenly.
    const float theta = kTwoPi * (t_ - 0.25f);
    s.sunDir  = glm::normalize(glm::vec3(std::cos(theta), std::sin(theta), 0.30f));
    // Moon orbit (issue #10 F): the moon lags the sun by the synodic elongation E,
    // which advances one full turn every ~29.53 days — so it rises ~50 min later
    // each night and cycles through its phases (E=0 new, beside the sun; E=pi full,
    // opposite). A slightly different z-lean gives it its own arc. The lit fraction
    // illum = (1 - cos E)/2 dims new-moon nights; the disc's crescent/gibbous shape
    // is shaded in sky.frag from the sun/moon directions.
    const float synodic = 29.53f;
    const double phaseDays = totalDays() / static_cast<double>(synodic);
    const float E = kTwoPi * static_cast<float>(phaseDays - std::floor(phaseDays));
    const float thetaM = theta - E;
    s.moonDir = glm::normalize(glm::vec3(std::cos(thetaM), std::sin(thetaM), 0.20f));
    const float moonIllum = 0.5f * (1.0f - std::cos(E)); // 0 new .. 1 full
    const float moonLit   = 0.10f + 0.90f * moonIllum;   // night brightness scale

    const float elev = s.sunDir.y;
    // Daylight blend: 0 at night, 1 in full day, smoothed through twilight.
    const float day = smoothstepf(-0.10f, 0.20f, elev);
    // Sunset strength: peaks while the sun crosses the horizon.
    const float sunset = std::clamp(1.0f - std::abs(elev) / 0.35f, 0.0f, 1.0f);

    // --- Per-day weather mood ------------------------------------------------
    // Each whole day draws a stable random mood — overall colour temperature,
    // haze, and brightness — that nudges the colours/params below so consecutive
    // days look a little different. dayVariation_ == 0 leaves everything as-is.
    // (The mood is constant per day; it only "pops" at midnight, in the dark.)
    const int   dayIndex = static_cast<int>(std::floor(days_ + t_));
    const float v      = dayVariation_;
    const float warmth = weatherJitter(dayIndex, 0x57A1u, weatherSeed_); // warm(+)/cool(-)
    const float haze   = weatherJitter(dayIndex, 0x4A2Eu, weatherSeed_); // hazy(+)/clear(-)
    const float bright = weatherJitter(dayIndex, 0x8B17u, weatherSeed_); // bright(+)/dim(-)
    // A coherent colour-temperature tint (warm days redden, cool days blue) and a
    // brightness scale, plus jittered copies of the colours/params one notices.
    const glm::vec3 warmTint(1.0f + 0.12f * v * warmth, 1.0f + 0.02f * v * warmth,
                             1.0f - 0.12f * v * warmth);
    const float brightScale = 1.0f + 0.10f * v * bright;
    // Pick the day's sunset mood preset (deterministic), then warmth-tint the
    // three bands together so the whole dusk shifts coherently.
    glm::vec3 ssHi = sunsetHigh_, ssMid = sunsetMid_, ssHor = sunsetHorizon_;
    if (tunedSunset_) {                       // tuning panel pins exact colours
        ssHi = tHigh_; ssMid = tMid_; ssHor = tHorizon_;
    } else if (!sunsetPresets_.empty()) {
        // Dawn and dusk draw their own mood (sunrise != sunset).
        const bool evening = t_ > 0.5f;
        const float pj =
            weatherJitter(dayIndex, evening ? 0x9C0Eu : 0x33A7u, weatherSeed_) * 0.5f + 0.5f;
        const int n = static_cast<int>(sunsetPresets_.size());
        const int pi = std::clamp(static_cast<int>(pj * n), 0, n - 1);
        ssHi = sunsetPresets_[pi].high;
        ssMid = sunsetPresets_[pi].mid;
        ssHor = sunsetPresets_[pi].horizon;
        // Per-twilight hue rotation: spin the whole palette and spread the bands
        // across the colour wheel, so no two dawns/dusks repeat. Saturation
        // boosted for the 'go wild' look.
        if (sunsetHueVary_ > 0.0f) {
            const float h0 = weatherJitter(dayIndex, evening ? 0x6B1Du : 0x2F8Eu, weatherSeed_) *
                             kTwoPi * 0.5f * sunsetHueVary_;       // up to +-180 deg
            const float h1 = weatherJitter(dayIndex, evening ? 0x71C4u : 0x18B3u, weatherSeed_) *
                             1.2f * sunsetHueVary_;                // band spread across the wheel
            const float sat = 1.0f + 0.6f * sunsetHueVary_;
            ssHor = saturate(hueRotate(ssHor, h0), sat);
            ssMid = saturate(hueRotate(ssMid, h0 + 0.4f * h1), sat);
            ssHi  = saturate(hueRotate(ssHi, h0 + h1), sat);
        }
    }
    // Tuned colours bypass the per-day warmth mood so the sliders read exactly.
    const glm::vec3 sunsetWarm     = tunedSunset_ ? glm::vec3(1.0f)
                                                  : warmTint * (1.0f + 0.12f * v * warmth);
    const glm::vec3 sunsetColJ     = ssHor * sunsetWarm; // horizon band (gold)
    const glm::vec3 sunsetMidJ     = ssMid * sunsetWarm; // mid band (orange)
    const glm::vec3 sunsetHighJ    = ssHi * sunsetWarm;  // high afterglow (pink/violet)
    const glm::vec3 sunlightDayJ  = sunlightDay_ * warmTint * brightScale;
    const glm::vec3 moonlightJ    = moonlight_ *
        glm::vec3(1.0f - 0.05f * v * warmth, 1.0f, 1.0f + 0.05f * v * warmth) *
        (1.0f + 0.06f * v * bright);
    const glm::vec3 nightZenithJ  = nightZenith_ *
        (glm::vec3(1.0f) + 0.06f * v * glm::vec3(0.5f * warmth, 0.0f, -0.5f * warmth));
    const glm::vec3 nightHorizonJ = nightHorizon_ * warmTint;
    const glm::vec3 sunDiscJ      = sunColor_ * warmTint;
    const float turbJ      = turbidity_ * (1.0f + 0.5f * v * haze);
    const float sunsetStrJ = sunsetStrength_ * (1.0f + 0.4f * v * (0.5f * haze + 0.5f * warmth));
    const float exposureJ  = exposure_ * brightScale;
    const float glowJ      = sunGlow_ * (1.0f + 0.3f * v * (0.5f + 0.5f * haze));
    // The daytime sky shifts subtly too (half-strength so noon stays believable).
    const glm::vec3 dayTint = glm::mix(glm::vec3(1.0f), warmTint, 0.5f);

    const float betaMEff = betaM_ * turbJ;

    s.analyticSky    = useAnalytic_;
    s.betaR          = betaR_;
    s.betaM          = betaMEff;
    s.mieG           = mieG_;
    s.sunIntensity   = sunIntensity_;
    s.exposure       = exposureJ;
    s.sunsetStrength = sunsetStrJ;
    s.dayBlend       = day;
    // The sky keeps its full sunset saturation while the sun is up and only
    // fades to the night gradient once it dips below the horizon (the wider
    // `day` window above would grey the dusk out with night colours too early).
    s.skyBlend       = smoothstepf(-0.12f, 0.02f, elev);
    // Authored sunset band: strongest as the sun crosses the horizon, gone by
    // full day or full night.
    s.sunsetColor  = sunsetColJ;
    // Drama (>1) lets the painted bands dominate the analytic dusk; the shader
    // clamps the per-pixel mix, so this widens the reach of the strong colour.
    s.sunsetAmount = sunset * s.skyBlend * sunsetDrama_;
    s.sunsetMid    = sunsetMidJ;
    s.sunsetHigh   = sunsetHighJ;
    // Ozone deepens twilight; haze pushes it (and the dusk) redder/stronger.
    s.ozone        = ozone_;
    s.ozoneStrength = ozoneStrength_ * (1.0f + 0.5f * v * haze);
    s.cloudDusk    = tunedSunset_ ? tDusk_ : cloudDusk_ * warmTint;
    s.cloudDuskAmt = cloudDuskStrength_;
    // Sky-option re-tint, relative to the yaml's dayZenith so the default palette
    // colour leaves the analytic sky untouched, then the day's mood tint on top.
    s.zenithTint = glm::clamp(dayZenith_ / glm::max(dayZenithRef_, glm::vec3(1e-3f)),
                              glm::vec3(0.0f), glm::vec3(4.0f)) * dayTint;

    if (useAnalytic_) {
        // Day colours come from skyRadiance() in the shader; only the night sky
        // is authored.
        s.zenith  = nightZenithJ;
        s.horizon = nightHorizonJ;
    } else {
        // Legacy authored gradient, fully blended on the CPU as before.
        s.zenith  = glm::mix(nightZenithJ, dayZenith_ * dayTint, day);
        s.horizon = glm::mix(nightHorizonJ, dayHorizon_ * dayTint, day);
        s.horizon = glm::mix(s.horizon, sunsetColJ, sunset * 0.85f);
    }

    s.sunDisc      = sunDiscJ;
    s.cosSunOuter  = std::cos(glm::radians(sunSizeDeg_));
    s.cosSunInner  = std::cos(glm::radians(sunSizeDeg_ * 0.65f));
    s.moonDisc     = moonColor_;
    s.cosMoonOuter = std::cos(glm::radians(moonSizeDeg_));
    s.cosMoonInner = std::cos(glm::radians(moonSizeDeg_ * 0.65f));
    s.glow         = glowJ * (1.0f + 2.0f * sunset * day);

    // Terrain light: the sun while it is up, the moon at night. The direction
    // swaps at the horizon, where intensity is lowest, hiding the pop.
    s.lightDir = (elev >= 0.0f) ? s.sunDir : s.moonDir;
    s.ambient  = glm::mix(nightAmbient_, dayAmbient_, day);
    if (useAnalytic_) {
        // Direct sunlight tinted by the atmosphere it crossed to reach the
        // ground: the same transmittance that reddens the sky reddens the
        // terrain, so the two stay coherent through sunset for free.
        const float amSun = airMass(std::max(elev, 0.0f));
        const glm::vec3 transmit = glm::exp(-(betaR_ + glm::vec3(betaMEff)) * amSun);
        s.lightColor = glm::mix(moonlightJ, sunlightDayJ * transmit, day);
    } else {
        s.lightColor = glm::mix(
            moonlightJ,
            glm::mix(sunlightSunset_ * warmTint, sunlightDayJ, smoothstepf(0.05f, 0.35f, elev)),
            day);
    }
    // Night sky-light scales with the moon's phase: a full moon lights the ground,
    // a new moon leaves it near-black (issue #10 F).
    s.skyIntensity = glm::mix(moonIntensity_ * moonLit, 1.0f, day);

    s.sunBase  = sunlightDayJ;
    s.moonBase = moonlightJ;

    // Night sky: the field wheels about the pole one (siderealRate) turn per day.
    // Wrap to [0, 2pi) — sin/cos are periodic, so this is seamless and keeps float
    // precision over many days. Latitude/brightness/Milky-Way pass through.
    s.siderealAngle  = static_cast<float>(std::fmod(totalDays() * siderealRate_, 1.0)) * kTwoPi;
    s.latitude       = latitudeRad_;
    s.starBrightness = starBrightness_;
    s.milkyWay       = milkyWay_;
    s.twinkleSpeed   = twinkleSpeed_;
    s.starExtinction = starExtinction_;
    s.planets        = planets_;
    s.shootingStars  = shootingStars_;

    return s;
}

} // namespace vg
