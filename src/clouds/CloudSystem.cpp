#include "clouds/CloudSystem.h"

#include "core/DayNight.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>

namespace vg {

namespace {

// Deterministic 0..1 hash of a sequence index (drives the autonomous scheduler's
// state picks, wind headings, and hold durations — no Date/rand needed).
float hashSeq(uint32_t s) {
    uint32_t h = s * 0x9e3779b9u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

// --- Hybrid weather states (issue #10 C) -------------------------------------
// A discrete daily "mood" picked deterministically per day. Its calm/fair
// endpoints feed the existing diurnal blend (so cover builds through the
// afternoon), the weather map adds spatial variation within the amp range, and
// the per-state layer band / cirrus deck / fog / star-clarity give whole days a
// distinct character. States are anchors; everything between them is noise.
struct WeatherState {
    float calmCov, fairCov;     // diurnal coverage endpoints (morning..afternoon)
    float calmType, fairType;   // diurnal type endpoints
    float covAmp, typeAmp;      // spatial-variation amplitudes (weather map)
    float bottom, top;          // cloud layer band (world Y) — different heights
    float highCloud;            // cirrus second-deck amount (0..1)
    float fog;                  // fog density 0..1 (Phase E)
    float clarity;              // night star clarity 0..1 (Phase F)
    float stormBase;            // flat overcast nimbostratus deck amount (0..1)
    float windMul;              // wind-speed multiplier (calm clear .. fast storm)
};

// clear / fair / broken / overcast / stormy / foggy. The autonomous scheduler
// transitions toward fairCov/fairType as each state's settled values (the old
// calm/fair pair is kept as the representative endpoint; diurnal blend retired).
const WeatherState kStates[6] = {
    // calmCov fairCov calmTy fairTy covAmp tyAmp  bot   top   high  fog  clar  storm wind
    {0.06f, 0.20f, 0.22f, 0.35f, 0.30f, 0.40f, 150.f, 235.f, 0.18f, 0.00f, 1.00f, 0.00f, 0.5f}, // clear
    {0.30f, 0.55f, 0.35f, 0.55f, 0.45f, 0.50f, 150.f, 300.f, 0.30f, 0.03f, 0.90f, 0.00f, 0.9f}, // fair
    {0.50f, 0.72f, 0.45f, 0.62f, 0.50f, 0.50f, 150.f, 320.f, 0.22f, 0.06f, 0.70f, 0.00f, 1.1f}, // broken
    {0.80f, 0.92f, 0.18f, 0.25f, 0.22f, 0.30f, 140.f, 205.f, 0.05f, 0.16f, 0.35f, 0.45f, 0.8f}, // overcast
    // stormy: near-full uniform coverage (low covAmp) + a thick dark overcast
    // base so cumulonimbus towers rise out of a sky-filling deck, blown in fast.
    {0.88f, 0.98f, 0.80f, 0.97f, 0.16f, 0.40f, 130.f, 345.f, 0.12f, 0.30f, 0.20f, 0.85f, 1.8f}, // stormy
    {0.42f, 0.60f, 0.12f, 0.20f, 0.35f, 0.30f, 150.f, 225.f, 0.10f, 0.55f, 0.40f, 0.00f, 0.3f}, // foggy
};

// Weighted next-state pick (fair/broken common, stormy/foggy rarer), keyed to the
// transition sequence so it is deterministic and day-independent. Never returns
// the current state (so every scheduled transition actually changes the sky).
int pickNextState(int cur, uint32_t seq) {
    static const int kBag[10] = {0, 1, 1, 1, 2, 2, 2, 3, 4, 5}; // clear..foggy weights
    for (uint32_t i = 0; i < 6; ++i) {
        uint32_t h = seq * 0x9e3779b9u + i * 0x85ebca6bu;
        h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
        const int s = kBag[h % 10u];
        if (s != cur) return s;
    }
    return (cur + 1) % 6;
}

} // namespace

CloudSystem::CloudSystem(VulkanContext& ctx, const std::string& cloudsYaml,
                         const std::string& noiseCacheFile)
    : noise_(ctx, noiseCacheFile), weather_(ctx, 0xC10DDA7Au) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(cloudsYaml);
    } catch (const YAML::Exception&) {
        root = YAML::Node(); // missing file: all defaults
    }
    auto num = [&](const char* key, float fb) {
        return root[key] ? root[key].as<float>() : fb;
    };
    auto boolean = [&](const char* key, bool fb) {
        return root[key] ? root[key].as<bool>() : fb;
    };

    enabled_       = boolean("enabled", enabled_);
    layerBottom_   = num("layerBottom", layerBottom_);
    layerTop_      = num("layerTop", layerTop_);
    if (root["wind"] && root["wind"].IsSequence() && root["wind"].size() == 3) {
        wind_ = {root["wind"][0].as<float>(), root["wind"][1].as<float>(),
                 root["wind"][2].as<float>()};
    }
    baseScale_     = 1.0f / std::max(1.0f, num("baseTileSize", 1.0f / baseScale_));
    detailScale_   = 1.0f / std::max(1.0f, num("detailTileSize", 1.0f / detailScale_));
    erosion_       = num("erosion", erosion_);
    densityScale_  = num("densityScale", densityScale_);
    extinction_    = num("extinction", extinction_);
    hgG_           = num("hgG", hgG_);
    ambientScale_  = num("ambientScale", ambientScale_);
    powderScale_   = num("powder", powderScale_);
    sunIntensity_  = num("sunIntensity", sunIntensity_);
    altitudeLift_  = num("altitudeLift", altitudeLift_);
    aerialDist_    = num("aerialDistance", aerialDist_);
    maxDist_       = num("maxDistance", maxDist_);
    primarySteps_  = num("primarySteps", primarySteps_);
    lightSteps_    = num("lightSteps", lightSteps_);
    lightStepLen_  = num("lightStepLength", lightStepLen_);
    msOctaves_     = static_cast<int>(num("msOctaves", static_cast<float>(msOctaves_)));
    msFalloff_     = num("msFalloff", msFalloff_);
    voxelize_      = num("voxelize", voxelize_);
    if (root["antiSolarTint"] && root["antiSolarTint"].IsSequence() &&
        root["antiSolarTint"].size() == 3) {
        antiSolar_ = {root["antiSolarTint"][0].as<float>(),
                      root["antiSolarTint"][1].as<float>(),
                      root["antiSolarTint"][2].as<float>()};
    }
    antiSolarStrength_ = num("antiSolarStrength", antiSolarStrength_);
    weatherScale_  = 1.0f / std::max(1.0f, num("weatherTileSize", 1.0f / weatherScale_));
    coverageAmp_   = num("coverageVariation", coverageAmp_);
    typeAmp_       = num("typeVariation", typeAmp_);
    calmCoverage_  = num("calmCoverage", calmCoverage_);
    fairCoverage_  = num("fairCoverage", fairCoverage_);
    calmType_      = num("calmType", calmType_);
    fairType_      = num("fairType", fairType_);
    buildStart_    = num("diurnalBuildStart", buildStart_);
    buildFull_     = num("diurnalBuildFull", buildFull_);
    fadeStart_     = num("diurnalFadeStart", fadeStart_);
    fadeEnd_       = num("diurnalFadeEnd", fadeEnd_);
    weatherLerpSec_ = std::max(0.1f, num("weatherLerpSec", weatherLerpSec_));
    multiDayAmp_   = num("multiDayVariation", multiDayAmp_);
    forceCoverage_ = num("forceCoverage", forceCoverage_);
    forceType_     = num("forceType", forceType_);
    // Pin a weather state from config (-1 = autonomous): 0 clear .. 5 foggy.
    forcedState_   = static_cast<int>(num("forceState",
                                          static_cast<float>(forcedState_)));

    // Autonomous weather-scheduler tunables (front sweep + wind change).
    frontDuration_ = std::max(1.0f, num("frontDuration", frontDuration_));
    frontSpan_     = std::max(100.0f, num("frontSpan", frontSpan_));
    frontWidth_    = std::max(1.0f, num("frontWidth", frontWidth_));
    holdMin_       = std::max(1.0f, num("weatherHoldMin", holdMin_));
    holdMax_       = std::max(holdMin_, num("weatherHoldMax", holdMax_));
    windLerpSec_   = std::max(0.1f, num("windLerpSec", windLerpSec_));

    // Seed the scheduler in a settled "fair" state (index 1) so frame 0 is sane
    // before update() drives the transitions. Wind starts at the yaml vector;
    // its length is the reference speed each state scales by windMul.
    curState_ = nextState_ = 1;
    const WeatherState& st0 = kStates[1];
    fromCov_ = toCov_ = st0.fairCov;
    fromType_ = toType_ = st0.fairType;
    fromStorm_ = toStorm_ = st0.stormBase;
    baseCoverage_  = st0.fairCov;
    baseType_      = st0.fairType;
    baseWindSpeed_ = std::max(0.01f, glm::length(glm::vec2(wind_.x, wind_.z)));
    windDirTarget_ = glm::normalize(glm::vec2(wind_.x, wind_.z) + glm::vec2(1e-5f, 0.0f));
    windSpeedTarget_ = baseWindSpeed_ * st0.windMul;
    weatherTimer_  = glm::mix(holdMin_, holdMax_, 0.5f);
    // Seed the smoothly-tracked secondary mood params.
    activeBottom_  = st0.bottom;
    activeTop_     = st0.top;
    activeCovAmp_  = st0.covAmp;
    activeTypeAmp_ = st0.typeAmp;
    activeHigh_    = st0.highCloud;
    activeStorm_   = st0.stormBase;
    fogDensity_    = st0.fog;
    starClarity_   = st0.clarity;
}

void CloudSystem::update(float dt, const DayNight& dn) {
    time_ += dt;

    // --- Autonomous, day-independent weather scheduler -----------------------
    // Weather sits in a state for a randomized hold, then a front sweeps the sky
    // to the next state over frontDuration seconds, travelling along the (newly
    // picked) wind heading. Nothing is keyed to the day count or the hour, so
    // midnight never snaps the sky. A pinned state (tuning panel) holds steady.
    const bool pinned = (forcedState_ >= 0 && forcedState_ < kStateCount);
    if (pinned) {
        if (curState_ != forcedState_ || transitioning_) {
            curState_ = nextState_ = forcedState_;
            const WeatherState& ps = kStates[forcedState_];
            fromCov_ = toCov_ = ps.fairCov;
            fromType_ = toType_ = ps.fairType;
            fromStorm_ = toStorm_ = ps.stormBase;
            windSpeedTarget_ = baseWindSpeed_ * ps.windMul;
            transitioning_ = false;
            frontPhase_ = 1.0f;
        }
    } else if (transitioning_) {
        frontPhase_ += dt / std::max(0.1f, frontDuration_);
        if (frontPhase_ >= 1.0f) {              // front has swept the whole sky
            frontPhase_ = 1.0f;
            transitioning_ = false;
            curState_ = nextState_;
            fromCov_ = toCov_; fromType_ = toType_; fromStorm_ = toStorm_;
            weatherTimer_ = glm::mix(holdMin_, holdMax_, hashSeq(weatherSeq_ * 7u + 3u));
        }
    } else {
        weatherTimer_ -= dt;
        if (weatherTimer_ <= 0.0f) {            // begin a new front
            ++weatherSeq_;
            fromCov_ = toCov_; fromType_ = toType_; fromStorm_ = toStorm_;
            nextState_ = pickNextState(curState_, weatherSeq_);
            const WeatherState& ns = kStates[nextState_];
            toCov_ = ns.fairCov; toType_ = ns.fairType; toStorm_ = ns.stormBase;
            const float ang = hashSeq(weatherSeq_ * 13u + 1u) * 6.2831853f; // fresh heading
            windDirTarget_ = glm::vec2(std::cos(ang), std::sin(ang));
            windSpeedTarget_ = baseWindSpeed_ * ns.windMul;
            frontPhase_ = 0.0f;
            transitioning_ = true;
        }
    }

    // Wind: glide the active vector toward the (changing) target heading + speed,
    // then accumulate the scroll offset that drifts both clouds and weather map.
    const float kw = 1.0f - std::exp(-dt / windLerpSec_);
    const glm::vec3 windTgt(windDirTarget_.x * windSpeedTarget_, 0.0f,
                            windDirTarget_.y * windSpeedTarget_);
    wind_ = glm::mix(wind_, windTgt, kw);
    windOffset_ += wind_ * dt;

    // The headline coverage/type/storm transition spatially via the front (in the
    // shader); the secondary mood params (layer band/decks/fog/clarity) glide
    // globally toward the incoming state — subtle enough that a uniform fade reads
    // fine. baseCoverage_/baseType_ are the scalar values terrain dimming samples.
    const WeatherState& tgt = kStates[nextState_];
    const float k = 1.0f - std::exp(-dt / weatherLerpSec_);
    baseCoverage_  = glm::mix(baseCoverage_, toCov_, k);
    baseType_      = glm::mix(baseType_, toType_, k);
    activeBottom_  = glm::mix(activeBottom_, tgt.bottom, k);
    activeTop_     = glm::mix(activeTop_, tgt.top, k);
    activeCovAmp_  = glm::mix(activeCovAmp_, tgt.covAmp, k);
    activeTypeAmp_ = glm::mix(activeTypeAmp_, tgt.typeAmp, k);
    activeHigh_    = glm::mix(activeHigh_, tgt.highCloud, k);
    activeStorm_   = glm::mix(activeStorm_, tgt.stormBase, k);
    fogDensity_    = glm::mix(fogDensity_, tgt.fog, k);
    starClarity_   = glm::mix(starClarity_, tgt.clarity, k);

    // Debug overrides force a uniform sky (used to verify the type presets); they
    // also disable the front so the forced look reads pure everywhere.
    const bool forceCov = forceCoverage_ >= 0.0f;
    const bool forceTyp = forceType_ >= 0.0f;
    const bool noFront  = forceCov || forceTyp;

    const float newCov   = forceCov ? forceCoverage_ : toCov_;
    const float newType  = forceTyp ? forceType_ : toType_;
    const float newStorm = noFront ? 0.0f : toStorm_;
    const float oldCov   = forceCov ? forceCoverage_ : fromCov_;
    const float oldType  = forceTyp ? forceType_ : fromType_;
    const float oldStorm = noFront ? 0.0f : fromStorm_;

    const DayNight::SkyState s = dn.state();

    gpu_.layer   = {activeBottom_, activeTop_, aerialDist_, enabled_ ? 1.0f : 0.0f};
    gpu_.wind    = {windOffset_, time_};
    gpu_.shape   = {baseScale_, detailScale_, erosion_, densityScale_};
    gpu_.scat    = {extinction_, hgG_, ambientScale_, powderScale_};
    gpu_.weather = {newCov, newType, weatherScale_, forceCov ? 0.0f : activeCovAmp_};
    gpu_.march   = {primarySteps_, lightSteps_, lightStepLen_,
                    forceTyp ? 0.0f : activeTypeAmp_};
    gpu_.sun     = {s.sunBase, sunIntensity_};
    gpu_.moon    = {s.moonBase, altitudeLift_};
    gpu_.anti    = {antiSolar_, antiSolarStrength_};
    gpu_.misc    = {maxDist_, static_cast<float>(msOctaves_), msFalloff_, voxelize_};
    // cDeck.w carries the incoming storm overcast-base amount; the decks are
    // zeroed under a force-override so a forced type reads pure.
    gpu_.deck    = {noFront ? 0.0f : activeHigh_, fogDensity_, starClarity_, newStorm};
    // Front: the old-side weather + the moving boundary position, sweeping along
    // the incoming weather's wind heading (stable across the transition even while
    // the active wind vector is still gliding into it). frontActive gates it off
    // when idle/forced. The boundary travels upwind->downwind, so the new regime
    // arrives from the windward horizon and passes overhead.
    const float frontS = glm::mix(-frontSpan_, frontSpan_, frontPhase_);
    gpu_.front0  = {oldCov, oldType, oldStorm, frontS};
    gpu_.front1  = {windDirTarget_.x, windDirTarget_.y, frontWidth_,
                    (transitioning_ && !noFront) ? 1.0f : 0.0f};
}

void CloudSystem::setWindSpeed(float v) {
    baseWindSpeed_ = std::max(0.0f, v);
    // Re-derive the live target so the change takes effect immediately (the wind
    // vector then glides to it over windLerpSec), keeping the current heading.
    windSpeedTarget_ = baseWindSpeed_ * kStates[nextState_].windMul;
}

} // namespace vg
