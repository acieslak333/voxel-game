#pragma once

/**
 * @file CloudSystem.h
 * @brief Declares CloudSystem, the high-level cloud model and weather scheduler.
 *
 * Owns CloudNoise and WeatherMap, and evolves weather state over time: wind drift,
 * discrete state transitions via a front-sweep scheduler, and smoothly tracked
 * secondary parameters (layer band, cirrus decks, fog density, star clarity).
 * Exposes gpuParams() — a 13-vec4 block appended verbatim to the sky UBO and
 * consumed by the volumetric cloud raymarch in sky.frag.
 * All tunables are loaded from assets/clouds.yaml.
 * @see docs/CODE_INDEX.md
 */

#include "clouds/CloudNoise.h"
#include "clouds/WeatherMap.h"

#include <glm/glm.hpp>

#include <string>

namespace vg {

class VulkanContext;
class DayNight;

/**
 * @brief High-level cloud model: noise fields, weather map, and autonomous weather scheduler.
 *
 * Weather transitions are driven by an independent clock (not the game day); each
 * change is a wind-driven front that sweeps from one state to the next over
 * frontDuration seconds. Secondary parameters (layer, decks, fog, clarity) glide
 * smoothly toward the target state via exponential lerp. The GPU parameter block
 * is rebuilt every update() call and forwarded to SkyRenderer via gpuParams().
 */
class CloudSystem {
public:
    /// GPU parameter block appended verbatim to the sky UBO (std140, vec4s only, 13 elements).
    struct GpuParams {
        glm::vec4 layer;   // x bottom, y top (world Y), z aerial fade dist, w enabled
        glm::vec4 wind;    // xyz accumulated wind offset (world units), w time (s)
        glm::vec4 shape;   // x baseScale, y detailScale, z erosion, w densityScale
        glm::vec4 scat;    // x extinction, y HG g, z ambientScale, w powderScale
        glm::vec4 weather; // x coverage base, y type base, z weatherScale, w coverage amp
        glm::vec4 march;   // x primarySteps, y lightSteps, z lightStepLen, w type amp
        glm::vec4 sun;     // rgb sun colour above atmosphere, w direct intensity
        glm::vec4 moon;    // rgb moonlight colour, w altitude sun-lift (twilight)
        glm::vec4 anti;    // rgb anti-solar twilight tint, w strength
        glm::vec4 misc;    // x march max distance, y ms octaves, z ms falloff,
                           // w voxelise cell size (blocks; 0 = smooth)
        glm::vec4 deck;    // x high-cirrus second-deck amount, y fog density,
                           // z night star clarity, w storm overcast-base amount
        glm::vec4 front0;  // x oldCov, y oldType, z oldStorm, w frontS (along-wind
                           // dist from camera where the transition boundary sits)
        glm::vec4 front1;  // x windDirX, y windDirZ (normalized), z frontWidth,
                           // w frontActive (1 during a transition, else 0)
    };

    /**
     * @brief Construct the cloud system and load tunables from @p cloudsYaml.
     * @param ctx             Vulkan device context (passed to CloudNoise + WeatherMap).
     * @param cloudsYaml      Path to assets/clouds.yaml; missing file uses all defaults.
     * @param noiseCacheFile  Path to the noise cache binary; regenerated on first run.
     */
    CloudSystem(VulkanContext& ctx, const std::string& cloudsYaml,
                const std::string& noiseCacheFile);

    /**
     * @brief Advance the weather simulation by @p dt seconds.
     *
     * Updates the autonomous scheduler (front phase, hold timer, state transitions),
     * glides wind and secondary parameters, and rebuilds gpu_ ready for the next frame.
     *
     * @param dt  Frame delta time in seconds.
     * @param dn  DayNight instance (provides sun/moon colours consumed by gpu_.sun/moon).
     */
    void update(float dt, const DayNight& dn);

    /// Current GPU parameter block; call after update() and pass to SkyRenderer::record().
    [[nodiscard]] const GpuParams& gpuParams() const { return gpu_; }
    /// The noise textures (passed to SkyRenderer at construction).
    [[nodiscard]] const CloudNoise& noise() const { return noise_; }
    /// The weather variation map (passed to SkyRenderer at construction).
    [[nodiscard]] const WeatherMap& weatherMap() const { return weather_; }

    /// Smoothly-tracked fog density (0..1), consumed by the fog pass (Phase E).
    [[nodiscard]] float fogDensity() const { return fogDensity_; }
    /// Smoothly-tracked night star clarity (0..1), consumed by sky.frag (Phase F).
    [[nodiscard]] float starClarity() const { return starClarity_; }
    /**
     * @brief Overhead cloud coverage (0 = clear, 1 = overcast).
     *
     * Used to dim and grey terrain lighting (Phase D). Respects forceCoverage_ debug
     * overrides so a forced overcast sky also dims the ground.
     */
    [[nodiscard]] float coverage() const {
        return forceCoverage_ >= 0.0f ? forceCoverage_ : baseCoverage_;
    }
    /// Current cloud type (0 = cumulus, 1 = cumulonimbus). Respects forceType_ override.
    [[nodiscard]] float type() const {
        return forceType_ >= 0.0f ? forceType_ : baseType_;
    }

    // --- Live tuning (in-game options panel) ---------------------------------

    /// Number of discrete weather states (clear / fair / broken / overcast / stormy / foggy).
    static constexpr int kStateCount = 6;
    void setForcedState(int s)    { forcedState_ = s; }
    void setForceCoverage(float c){ forceCoverage_ = c; }
    void setForceType(float t)    { forceType_ = t; }
    void setDensityScale(float v) { densityScale_ = v; }
    void setErosion(float v)      { erosion_ = v; }
    void setVoxelize(float v)     { voxelize_ = v; }
    void setExtinction(float v)   { extinction_ = v; }
    // Autonomous weather scheduler knobs (options panel).
    void setFrontDuration(float v)  { frontDuration_ = v; }          // sweep seconds
    void setChangeInterval(float v) { holdMin_ = v * 0.6f; holdMax_ = v * 1.4f; }
    void setFrontWidth(float v)     { frontWidth_ = v; }
    void setWindSpeed(float v);     // also rescales the live wind target (see .cpp)
    void triggerWeatherChange()     { if (forcedState_ < 0 && !transitioning_) weatherTimer_ = 0.0f; }
    [[nodiscard]] int   forcedState()   const { return forcedState_; }
    [[nodiscard]] float forceCoverage() const { return forceCoverage_; }
    [[nodiscard]] float forceType()     const { return forceType_; }
    [[nodiscard]] float densityScale()  const { return densityScale_; }
    [[nodiscard]] float erosion()       const { return erosion_; }
    [[nodiscard]] float voxelize()      const { return voxelize_; }
    [[nodiscard]] float extinction()    const { return extinction_; }
    [[nodiscard]] float frontDuration() const { return frontDuration_; }
    [[nodiscard]] float changeInterval()const { return 0.5f * (holdMin_ + holdMax_); }
    [[nodiscard]] float frontWidth()    const { return frontWidth_; }
    [[nodiscard]] float windSpeed()     const { return baseWindSpeed_; }

private:
    CloudNoise noise_;
    WeatherMap weather_;

    // --- Config (assets/clouds.yaml) ------------------------------------------
    bool      enabled_       = true;
    float     layerBottom_   = 150.0f, layerTop_ = 260.0f;
    glm::vec3 wind_{6.0f, 0.0f, 2.0f}; // world units / second
    float     baseScale_     = 1.0f / 260.0f; // noise tiles per world unit
    float     detailScale_   = 1.0f / 48.0f;
    float     erosion_       = 0.62f;
    float     densityScale_  = 0.55f;
    float     extinction_    = 0.9f;
    float     hgG_           = 0.55f;
    float     ambientScale_  = 0.9f;
    float     powderScale_   = 1.0f;
    float     sunIntensity_  = 1.6f;  // direct-light scale on the cloud march
    float     altitudeLift_  = 0.08f; // sun elevation gained at the layer top
    float     aerialDist_    = 2600.0f;
    float     maxDist_       = 9000.0f;
    float     primarySteps_  = 48.0f;
    float     lightSteps_    = 5.0f;
    float     lightStepLen_  = 14.0f;
    int       msOctaves_     = 3;
    float     msFalloff_     = 0.5f;
    float     voxelize_      = 3.0f; // density sample grid (blocks); 0 = smooth
    glm::vec3 antiSolar_{0.85f, 0.45f, 0.55f}; // Belt-of-Venus pink (linear)
    float     antiSolarStrength_ = 0.35f;
    float     weatherScale_  = 1.0f / 1400.0f; // weather tiles per world unit
    float     coverageAmp_   = 0.45f;
    float     typeAmp_       = 0.5f;
    float     calmCoverage_  = 0.28f, fairCoverage_ = 0.55f;
    float     calmType_      = 0.20f, fairType_ = 0.55f;
    float     buildStart_    = 9.0f, buildFull_ = 15.0f;
    float     fadeStart_     = 17.0f, fadeEnd_ = 20.0f;
    float     weatherLerpSec_ = 8.0f;
    float     multiDayAmp_   = 0.4f;
    float     forceCoverage_ = -1.0f, forceType_ = -1.0f; // >=0 = debug override
    int       forcedState_   = -1;    // >=0 = pin a weather state (tuning panel)

    // --- Front tunables (assets/clouds.yaml) ----------------------------------
    // The autonomous weather scheduler: a state holds for hold[Min..Max] seconds,
    // then a front sweeps the sky to the next state over frontDuration seconds,
    // travelling along the (newly picked) wind direction. frontSpan is how far
    // up/downwind the boundary sweeps; frontWidth softens it. windLerpSec glides
    // the wind vector to each state's speed + a fresh random direction.
    float     frontDuration_ = 30.0f;
    float     frontSpan_     = 4000.0f;
    float     frontWidth_    = 1200.0f;
    float     holdMin_       = 80.0f, holdMax_ = 220.0f;
    float     windLerpSec_   = 12.0f;

    // --- Evolving state --------------------------------------------------------
    float     time_ = 0.0f;
    glm::vec3 windOffset_{0.0f};
    float     baseCoverage_ = 0.4f;   // scalar coverage for terrain dimming
    float     baseType_     = 0.35f;
    // Autonomous, day-independent weather scheduler (replaces the per-day pick):
    // weather changes on its own clock and each change is a wind-driven front.
    int       curState_ = 1, nextState_ = 1; // start "fair"; differ during a sweep
    bool      transitioning_ = false;
    float     frontPhase_   = 1.0f;   // 0..1 across the current sweep (1 = settled)
    float     weatherTimer_ = 30.0f;  // seconds until the next transition begins
    uint32_t  weatherSeq_   = 0;      // increments per transition (deterministic rng)
    float     fromCov_ = 0.3f, fromType_ = 0.35f, fromStorm_ = 0.0f; // front "old" side
    float     toCov_   = 0.3f, toType_   = 0.35f, toStorm_   = 0.0f; // front "new" side
    glm::vec2 windDirTarget_{1.0f, 0.0f}; // per-period wind heading (xz, normalized)
    float     windSpeedTarget_ = 6.0f;
    float     baseWindSpeed_   = 6.0f;    // reference speed (length of yaml wind)
    // Per-day weather state mood, smoothly tracked toward the incoming state so
    // the secondary params (band/decks/fog/clarity) never pop.
    float     activeBottom_  = 150.0f, activeTop_ = 260.0f; // dynamic layer band
    float     activeCovAmp_  = 0.45f, activeTypeAmp_ = 0.5f;
    float     activeHigh_    = 0.2f;  // cirrus second-deck amount
    float     activeStorm_   = 0.0f;  // storm overcast-base deck amount (0..1)
    float     fogDensity_    = 0.0f;  // 0..1 (consumed by Phase E fog)
    float     starClarity_   = 1.0f;  // 0..1 (consumed by Phase F stars)

    GpuParams gpu_{};
};

} // namespace vg
