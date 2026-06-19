#pragma once

/**
 * @file DayNight.h
 * @brief Time-of-day model: advances the sun/moon, snapshots sky and lighting state.
 *
 * DayNight owns the time fraction and all sky parameters loaded from
 * assets/sky.yaml. Each frame, state() produces a SkyState consumed by
 * SkyRenderer (gradient + disc rendering) and WorldRenderer (chunk lighting).
 * Per-day weather variation nudges colours without breaking reproducibility.
 * @see docs/CODE_INDEX.md
 */
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vg {

class Palette;

/**
 * @brief Time-of-day controller that drives the sky and terrain light each frame.
 *
 * Advances a fractional day [0,1) in real time, moves the sun and moon across
 * their arcs, applies a per-day weather mood, and computes the full SkyState
 * snapshot once per frame via state().
 */
class DayNight {
public:
    /** @brief Per-frame snapshot of all sky and lighting parameters for the renderers. */
    struct SkyState {
        // Directions point *toward* the celestial body (unit vectors).
        glm::vec3 sunDir;
        glm::vec3 moonDir;
        // Sky gradient colours (linear). With the analytic atmosphere these hold
        // the *night* sky only (the day sky is computed per pixel in sky.frag);
        // with `useAnalyticSky: false` they hold the fully blended legacy colours.
        glm::vec3 zenith;
        glm::vec3 horizon;
        // Discs: colour + cosine thresholds of the angular radius (outer = edge,
        // inner = solid core; the band between is the soft rim). sunDisc is the
        // colour *above* the atmosphere; the shader reddens it by transmittance.
        glm::vec3 sunDisc;
        float     cosSunOuter, cosSunInner;
        glm::vec3 moonDisc;
        float     cosMoonOuter, cosMoonInner;
        float     glow; // additive halo strength around the sun

        // Analytic single-scattering inputs (consumed by sky.frag).
        bool      analyticSky;    // false = legacy authored-gradient path
        glm::vec3 betaR;          // Rayleigh scattering coefficients (blue-heavy)
        float     betaM;          // Mie scattering coefficient (incl. turbidity)
        float     mieG;           // Mie anisotropy (forward-scatter sharpness)
        float     sunIntensity;   // sun radiance scale (HDR)
        float     exposure;       // tonemap exposure applied in the sky shader
        float     sunsetStrength; // exaggerates sun-path filtering (warmer dusk)
        float     dayBlend;       // terrain day/night blend (wide twilight window)
        float     skyBlend;       // sky day/night blend: fades only below the
                                  // horizon, so sunset colours stay saturated
        // Authored sunset band (the yaml's sunsetHorizon colour): painted over
        // the analytic sky near the horizon around the sun's azimuth while the
        // sun is low. Keeps dusk reliably warm and colourful.
        glm::vec3 sunsetColor;    // horizon band (hot gold), the day's preset pick
        float     sunsetAmount;   // 0 = none, peaks as the sun crosses the horizon
        glm::vec3 sunsetMid;      // mid-altitude sunset band (orange)
        glm::vec3 sunsetHigh;     // high-sky sunset afterglow (pink/violet)
        glm::vec3 ozone;          // Chappuis-band absorption coeff (deepens twilight)
        float     ozoneStrength;  // scales the ozone term (grows with haze)
        glm::vec3 cloudDusk;      // warm tint added to lit cloud bases at low sun
        float     cloudDuskAmt;   // cloud dusk-glow strength (0 = off)
        glm::vec3 zenithTint;     // daytime re-tint from the Sky option (1 = neutral)

        // Terrain lighting: the active light (sun by day, moon by night).
        glm::vec3 lightDir;
        float     ambient;      // directional ambient floor (0..1)
        glm::vec3 lightColor;   // linear tint of sky-lit surfaces
        float     skyIntensity; // sky-light scale: 1 at noon, moonIntensity at night

        // Raw light colours *above* the atmosphere, for systems (clouds) that
        // apply their own altitude-dependent transmittance to them.
        glm::vec3 sunBase;      // = sunlightDay from sky.yaml
        glm::vec3 moonBase;     // = moonlight from sky.yaml

        // Night sky (procedural stars + Milky Way, drawn in sky.frag).
        float siderealAngle;    // celestial rotation about the pole (radians, wraps)
        float latitude;         // observer latitude (radians) -> pole tilt
        float starBrightness;   // overall star intensity scale
        float milkyWay;         // Milky Way band strength (0 = off)
        float twinkleSpeed;     // star scintillation rate
        float starExtinction;   // horizon dimming/reddening strength
        float planets;          // planet brightness (0 = off; steady, non-twinkling)
        float shootingStars;    // meteor frequency (0 = off)
    };

    // Loads assets/sky.yaml. Colours may be palette names or "#RRGGBB" hex.
    DayNight(const std::string& skyFile, const Palette& palette);

    // Advance time by dt seconds of real time (no-op while not running).
    void advance(float dt);

    [[nodiscard]] float hour() const { return t_ * 24.0f; }
    void setHour(float h);
    // Set the elapsed whole-day count (drives moon phase + multi-day weather).
    // Mainly for debug/verification hooks; normal play advances it via advance().
    void setDay(int d) { days_ = d; }

    // Continuous day count since launch (whole days survived + the current day's
    // fraction). Drives multi-day weather variation; setHour() does not reset it.
    [[nodiscard]] double totalDays() const { return days_ + static_cast<double>(t_); }

    void setDayLengthMinutes(float minutes);
    void setRunning(bool r) { running_ = r; }
    [[nodiscard]] bool running() const { return running_; }

    // The in-game "Sky" option re-tints the daytime sky from the palette. With
    // the analytic atmosphere the override acts as a tint *relative to* the
    // yaml's dayZenith (so the default palette colour is neutral); the legacy
    // path uses it directly as the day zenith colour.
    void setDayZenithOverride(const glm::vec3& linearColor) { dayZenith_ = linearColor; }

    // --- Live tuning (in-game options panel) ---------------------------------
    void setStarBrightness(float v) { starBrightness_ = v; }
    void setMilkyWay(float v)       { milkyWay_ = v; }
    void setOzoneStrength(float v)  { ozoneStrength_ = v; }
    [[nodiscard]] float starBrightness() const { return starBrightness_; }
    [[nodiscard]] float milkyWay()       const { return milkyWay_; }
    [[nodiscard]] float ozoneStrength()  const { return ozoneStrength_; }
    // Tuned sunset band colours (linear). Enabling replaces the per-day preset
    // pick (and skips the warmth mood) so the panel's RGB sliders read exactly;
    // clearTunedSunset() restores the daily mood.
    void setTunedSunset(const glm::vec3& high, const glm::vec3& mid,
                        const glm::vec3& horizon, const glm::vec3& dusk) {
        tHigh_ = high; tMid_ = mid; tHorizon_ = horizon; tDusk_ = dusk; tunedSunset_ = true;
    }
    void clearTunedSunset() { tunedSunset_ = false; }
    [[nodiscard]] bool sunsetTuned() const { return tunedSunset_; }
    // Current band colours (the tuned values if active, else the yaml bases) —
    // used to seed the sliders.
    [[nodiscard]] glm::vec3 sunsetHigh()    const { return tunedSunset_ ? tHigh_ : sunsetHigh_; }
    [[nodiscard]] glm::vec3 sunsetMid()     const { return tunedSunset_ ? tMid_ : sunsetMid_; }
    [[nodiscard]] glm::vec3 sunsetHorizon() const { return tunedSunset_ ? tHorizon_ : sunsetHorizon_; }
    [[nodiscard]] glm::vec3 cloudDusk()     const { return tunedSunset_ ? tDusk_ : cloudDusk_; }

    // Snapshot the current sky/lighting for this frame.
    [[nodiscard]] SkyState state() const;

private:
    // Config (linear-space colours).
    glm::vec3 dayZenith_{}, dayHorizon_{}, nightZenith_{}, nightHorizon_{};
    glm::vec3 sunsetHorizon_{}, sunsetMid_{}, sunsetHigh_{};
    // Per-day sunset moods (high/mid/horizon, linear); empty = use the three
    // colours above as the only preset.
    struct SunsetPreset { glm::vec3 high, mid, horizon; };
    std::vector<SunsetPreset> sunsetPresets_;
    // Tuning overrides for the sunset colours (see setTunedSunset).
    bool      tunedSunset_ = false;
    glm::vec3 tHigh_{}, tMid_{}, tHorizon_{}, tDusk_{};
    glm::vec3 ozone_{0.0021f, 0.0034f, 0.0009f};
    float     ozoneStrength_ = 1.0f;
    glm::vec3 cloudDusk_{};
    float     cloudDuskStrength_ = 0.6f;
    // Go-wild sunset variety: band dominance + per-twilight hue rotation.
    float     sunsetDrama_   = 1.0f;
    float     sunsetHueVary_ = 0.0f;
    glm::vec3 sunColor_{}, moonColor_{};
    glm::vec3 sunlightDay_{}, sunlightSunset_{}, moonlight_{};
    float sunSizeDeg_ = 3.5f, moonSizeDeg_ = 2.4f, sunGlow_ = 0.22f;
    float dayAmbient_ = 0.45f, nightAmbient_ = 0.30f, moonIntensity_ = 0.16f;

    // Night sky: observer latitude (pole tilt), how fast the field wheels (turns
    // per in-game day), overall star brightness, and Milky Way strength.
    float latitudeRad_    = 0.78f; // ~45 degrees
    float siderealRate_   = 1.0f;
    float starBrightness_ = 1.0f;
    float milkyWay_       = 0.6f;
    float twinkleSpeed_   = 2.5f;
    float starExtinction_ = 0.30f;
    float planets_        = 0.8f;
    float shootingStars_  = 0.3f;

    // Per-day weather variation: each whole day gets a deterministic "mood"
    // (overall warmth / haze / brightness) that nudges the sunset, sunlight,
    // moonlight, zenith/horizon and haze a little, so no two days look identical.
    // 0 = every day identical; ~1 = pronounced. See assets/sky.yaml.
    float    dayVariation_ = 0.5f;
    uint32_t weatherSeed_  = 0x5EED0517u; // salts the per-day hash

    // Analytic single-scattering atmosphere (see sky.frag / assets/sky.yaml).
    bool      useAnalytic_  = true;
    glm::vec3 betaR_{0.0093f, 0.0216f, 0.0530f}; // Rayleigh, blue scatters most
    float     betaM_        = 0.010f;            // Mie (haze), colour-neutral
    float     turbidity_    = 1.0f;              // scales betaM (hazier = bigger)
    float     mieG_         = 0.92f;             // forward-scatter anisotropy
    float     sunIntensity_ = 18.0f;             // HDR sun radiance scale
    float     exposure_     = 0.4f;              // sky tonemap exposure
    float     sunsetStrength_ = 1.3f;            // sun-path filter exponent
    // Tempers how dark the ground gets through dusk. The day->moonlight terrain
    // blend collapses fast as the sun crosses the horizon, blacking the ground out
    // under a still-bright dusk sky. This lifts terrain light + ambient through the
    // sunset band (peaks at the horizon, fades to 0 by full day/night). 0 = the old
    // steep falloff; ~0.45 keeps a gentle dusk glow. Fraction, so 0.45 = +45%.
    float     duskBrightness_ = 0.6f;
    glm::vec3 dayZenithRef_{};                   // yaml dayZenith: tint reference

    float t_          = 9.0f / 24.0f; // fraction of a day [0,1)
    int   days_       = 0;            // whole days elapsed (for totalDays())
    float dayLenSec_  = 600.0f;       // real seconds per in-game day
    bool  running_    = true;
};

} // namespace vg
