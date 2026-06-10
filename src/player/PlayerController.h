#pragma once

#include "player/Camera.h"
#include "player/Inventory.h"

#include <glm/glm.hpp>

#include <functional>

namespace vg {

struct InputState;

// -----------------------------------------------------------------------------
//  PlayerController
// -----------------------------------------------------------------------------
//  Drives the first-person camera and the player's physical presence in the
//  world. Two modes:
//    * Walking  — gravity, jumping, and AABB collision against solid blocks.
//    * Free-fly — no gravity or collision; fly freely (debug/exploration).
//
//  Collision queries the world through a caller-supplied predicate, so this
//  class has no dependency on how the world is stored (single chunk now,
//  streamed chunks later).
// -----------------------------------------------------------------------------
class PlayerController {
public:
    enum class Mode { Walking, FreeFly };

    // Returns true if the block at integer coords (x,y,z) is solid.
    using SolidFn = std::function<bool(int x, int y, int z)>;
    // For a solid block, its collision-box X/Z inset (0 = full cell; >0 = a centred
    // column, e.g. the thin tree trunk). Y always spans the full cell. Optional.
    using InsetFn = std::function<float(int x, int y, int z)>;

    explicit PlayerController(glm::vec3 feetPosition);

    void setSolidFn(SolidFn fn) { isSolid_ = std::move(fn); }
    void setCollisionInsetFn(InsetFn fn) { collisionInset_ = std::move(fn); }

    // Advance one frame given elapsed time and this frame's input.
    void update(float dt, const InputState& input);

    // Move the player to a new position (clears velocity). Used for spawning /
    // the debug fly-overview.
    void teleport(glm::vec3 feet);
    void setMode(Mode m);

    [[nodiscard]] Camera&       camera()       { return camera_; }
    [[nodiscard]] const Camera& camera() const { return camera_; }
    [[nodiscard]] Mode  mode()    const { return mode_; }
    [[nodiscard]] bool  onGround() const { return onGround_; }

    // --- Health / damage (ISSUES #13B; no hunger) --------------------------
    [[nodiscard]] float health()    const { return health_; }
    [[nodiscard]] float maxHealth() const { return maxHealth_; }
    [[nodiscard]] bool  isDead()    const { return health_ <= 0.0f; }
    // Subtract `amount` HP (clamped at 0); a no-op while invulnerable. Pauses the
    // slow passive regen briefly so chip damage isn't instantly undone.
    void damage(float amount);
    void heal(float amount);
    void setHealth(float h);
    // Creative / debug: ignore all damage (fall, lava, combat).
    void setInvulnerable(bool v) { invulnerable_ = v; }
    [[nodiscard]] bool invulnerable() const { return invulnerable_; }

    // Pure fall-damage curve: HP lost on landing at `impactSpeed` (downward m/s).
    // Below the safe-fall height it's 0; above it scales with the extra height.
    // Static + side-effect-free so --logictest can verify it without a world.
    [[nodiscard]] static float fallDamage(float impactSpeed);

    // The player's item storage (hotbar + backpack). Mining adds to it; placing
    // consumes from its selected hotbar slot. See player/Inventory.h.
    [[nodiscard]] Inventory&       inventory()       { return inventory_; }
    [[nodiscard]] const Inventory& inventory() const { return inventory_; }

    // Tunables exposed to the options menu.
    void setMouseSensitivity(float s) { sensitivity_ = s; }
    void setFlySpeed(float s)          { flySpeed_ = s; }
    [[nodiscard]] float mouseSensitivity() const { return sensitivity_; }
    [[nodiscard]] float flySpeed()         const { return flySpeed_; }

    // Does the player's AABB overlap the block cell (bx,by,bz)? Used to refuse
    // placing a block inside the player.
    [[nodiscard]] bool occupies(int bx, int by, int bz) const;

    // TODO(future): additional survival stats (stamina) would live next to health_.

private:
    // Move along one axis (0=x,1=y,2=z) by `delta`, swept against solid blocks:
    // advances exactly to the contact plane instead of stopping a block short.
    void moveAxis(glm::vec3& feet, float delta, int axis);

    void syncCameraToBody();

    Camera    camera_;
    glm::vec3 feet_{0.0f};      // bottom-centre of the player AABB
    glm::vec3 velocity_{0.0f};
    Mode      mode_     = Mode::Walking;
    bool      onGround_ = false;
    float     health_     = 100.0f;
    float     maxHealth_  = 100.0f;
    bool      invulnerable_ = false;
    float     regenDelay_ = 0.0f; // seconds before passive regen resumes after a hit
    Inventory inventory_;

    float     sensitivity_ = 0.08f; // mouse look (matches the old kSensitivity)
    float     flySpeed_    = 12.0f; // free-fly base speed (kFlySpeed)

    SolidFn isSolid_;
    InsetFn collisionInset_; // optional: thin-block X/Z inset (tree trunk)
};

} // namespace vg
