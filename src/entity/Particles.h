#pragma once

/**
 * @file Particles.h
 * @brief Data-driven billboard particle system: burst effects loaded from .prtcl YAML.
 *
 * ParticleEffect describes emitter parameters (count, gravity, speed, size, lifetime).
 * Particles::spawnEffect() launches a burst; update() advances physics and ages them out.
 * Collision via a SolidFn predicate. Pure simulation (no Vulkan); rendering done by
 * the caller as camera-facing quads.
 * @see docs/CODE_INDEX.md
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace vg {

// -----------------------------------------------------------------------------
//  Particles (ISSUES #13H/#13M — game-feel juice)
// -----------------------------------------------------------------------------
//  A pooled particle system for short-lived 2D billboard effects (block-break
//  chips today; place poofs / splashes / sparks later). Effects are data-driven:
//  a `.prtcl` file (assets/particles/*.prtcl) describes the emitter + per-particle
//  animation, authored live in tools/particle_tool.py. The simulation is pure
//  (collision through a solid() predicate) so it stays headlessly testable;
//  rendering reuses the EntityRenderer as flat camera-facing quads.
// -----------------------------------------------------------------------------

// A data-driven particle effect (one burst), loaded from a `.prtcl` YAML file.
/** @brief Emitter template loaded from a .prtcl file: all tunable burst parameters. */
struct ParticleEffect {
    std::string name;
    std::string texture;        // texture filename (e.g. "stone.block.png"); empty =
                                // use the source block's texture (break chips)
    int   count       = 14;     // particles spawned per burst
    float gravity     = -22.0f; // m/s^2
    float drag        = 0.0f;   // horizontal damping (per second)
    float spawnRadius = 0.30f;  // spawn jitter around the emit point (blocks)
    float speedMin    = 1.5f;   // initial outward speed range
    float speedMax    = 4.0f;
    float upBias      = 1.5f;   // extra upward velocity added to every particle
    float lifeMin     = 0.5f;   // lifetime range (seconds)
    float lifeMax     = 1.1f;
    float sizeMin     = 0.06f;  // spawn half-extent range (blocks)
    float sizeMax     = 0.12f;
    float sizeEnd     = 0.2f;   // size multiplier at end of life (linear shrink)
    float spinMin     = -12.0f; // billboard roll speed range (rad/s)
    float spinMax     = 12.0f;

    // Parse a `.prtcl` YAML file. Missing keys keep their defaults. Throws
    // std::runtime_error if the file can't be read/parsed.
    static ParticleEffect load(const std::string& path);
};

/** @brief Live particle instance: position, velocity, lifetime, size, spin, and texture info. */
struct Particle {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    float     life     = 0.0f;  // seconds remaining
    float     maxLife  = 1.0f;
    float     size     = 0.12f; // CURRENT half-extent (size0 scaled by the shrink curve)
    float     size0    = 0.12f; // spawn half-extent
    float     sizeEnd  = 0.2f;  // size multiplier at death
    float     gravity  = -22.0f;
    float     drag     = 0.0f;
    float     spin     = 0.0f;  // current billboard roll
    float     spinVel  = 0.0f;
    uint32_t  layer    = 0;     // texture-array layer this chip samples
    glm::vec2 uv0{0.0f};        // top-left of the chip's sub-rect in the texture
};

/** @brief Pooled particle system: spawn bursts from a ParticleEffect, step physics, expire. */
class Particles {
public:
    using SolidFn = std::function<bool(int x, int y, int z)>;

    // Spawn one burst of `fx` centred at `center`, sampling texture-array `layer`
    // (the caller resolves it from the effect's texture or the source block).
    void spawnEffect(const ParticleEffect& fx, const glm::vec3& center, uint32_t layer);

    // Advance: gravity, move, settle on the first solid block below, shrink, age out.
    void update(float dt, const SolidFn& solid);

    [[nodiscard]] const std::vector<Particle>& all() const { return particles_; }
    [[nodiscard]] size_t size() const { return particles_.size(); }
    void clear() { particles_.clear(); }

    static constexpr size_t kMaxParticles = 1024; // hard cap (oldest dropped beyond it)

private:
    float rand01(); // cheap deterministic-ish RNG (xorshift); no std::random needed

    std::vector<Particle> particles_;
    uint32_t              rng_ = 0x9E3779B9u;
};

} // namespace vg
