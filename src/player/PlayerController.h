#pragma once

#include "player/Camera.h"

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

    explicit PlayerController(glm::vec3 feetPosition);

    void setSolidFn(SolidFn fn) { isSolid_ = std::move(fn); }

    // Advance one frame given elapsed time and this frame's input.
    void update(float dt, const InputState& input);

    [[nodiscard]] Camera&       camera()       { return camera_; }
    [[nodiscard]] const Camera& camera() const { return camera_; }
    [[nodiscard]] Mode  mode()    const { return mode_; }
    [[nodiscard]] float health()  const { return health_; }
    [[nodiscard]] bool  onGround() const { return onGround_; }

    // TODO(future): additional survival stats (hunger, thirst, stamina) and a
    // damage API would live next to health_ here.

private:
    // Is the player's AABB (bottom-centre at `feet`) intersecting a solid block?
    [[nodiscard]] bool collides(const glm::vec3& feet) const;
    // Try to move along one axis (0=x,1=y,2=z); stop at the first solid block.
    void moveAxis(glm::vec3& feet, float delta, int axis);

    void syncCameraToBody();

    Camera    camera_;
    glm::vec3 feet_{0.0f};      // bottom-centre of the player AABB
    glm::vec3 velocity_{0.0f};
    Mode      mode_     = Mode::Walking;
    bool      onGround_ = false;
    float     health_   = 100.0f;

    SolidFn isSolid_;
};

} // namespace vg
