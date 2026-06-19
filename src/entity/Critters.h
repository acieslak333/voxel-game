#pragma once

/**
 * @file Critters.h
 * @brief Simple wandering mob simulation: wander AI, obstacle turns, gravity.
 *
 * Each Critter ambles on a random heading, pauses, picks a new direction, avoids
 * obstacles, and falls under gravity. Collision is through a caller-supplied SolidFn
 * predicate. Pure CPU state — no Vulkan — rendering is handled by the caller using
 * animTime to drive a shared box rig.
 * @see docs/CODE_INDEX.md
 */

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  Passive critters (ISSUES #13I, placeholder rigs)
// -----------------------------------------------------------------------------
//  Simple wandering creatures: they amble in a heading for a while, pause, pick a
//  new heading, turn at obstacles, and fall under gravity onto the terrain. Pure
//  CPU state + AI (no Vulkan, no rig) — App renders each by baking the shared box
//  rig at the critter's own walk phase. A stand-in until the glTF loader (E3) and
//  real mob models land; the AI/spawn/update seam stays the same.
// -----------------------------------------------------------------------------
/** @brief State for one wandering mob: position, facing, velocity, and animation phase. */
struct Critter {
    glm::vec3 pos{0.0f};      // feet position in world space
    float     yaw      = 0.0f; // facing (radians; 0 = +Z)
    float     vy       = 0.0f; // vertical velocity (gravity)
    bool      walking  = false;
    bool      onGround = false;
    float     wanderT  = 0.0f; // seconds until the next wander decision
    float     animTime = 0.0f; // walk-clip phase (advances only while walking)
};

/** @brief Collection of wandering Critter entities updated each frame via a SolidFn. */
class Critters {
public:
    // Is the block at integer (x,y,z) solid (collision)? Supplied by World.
    using SolidFn = std::function<bool(int, int, int)>;

    // Add a critter at a feet position (drops onto the ground under gravity).
    void spawn(const glm::vec3& feet);

    // Advance every critter: wander heading, horizontal move with obstacle turns,
    // gravity + ground snap, and walk-cycle phase.
    void update(float dt, const SolidFn& solid);

    [[nodiscard]] const std::vector<Critter>& all() const { return critters_; }
    [[nodiscard]] std::size_t size() const { return critters_.size(); }

private:
    std::vector<Critter> critters_;
    uint32_t rng_ = 0x9e3779b9u; // tiny LCG; mob motion needn't be reproducible
    float frand();               // [0,1)
};

} // namespace vg
