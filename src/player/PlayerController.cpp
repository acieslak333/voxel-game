#include "player/PlayerController.h"

#include "core/Input.h"

#include <cmath>

namespace vg {

namespace {
// Player body + movement tuning. Block units are metres.
constexpr float kHalfWidth   = 0.3f;   // AABB is 0.6 wide/deep
constexpr float kHeight      = 1.8f;
constexpr float kEyeHeight   = 1.62f;

constexpr float kWalkSpeed   = 4.3f;
constexpr float kSprintSpeed = 7.0f;
constexpr float kFlySpeed    = 12.0f;
constexpr float kFlySprint   = 26.0f;

constexpr float kGravity     = -26.0f;  // m/s^2
constexpr float kJumpSpeed   = 8.4f;    // gives ~1.35 block jump height
constexpr float kSensitivity = 0.08f;
} // namespace

PlayerController::PlayerController(glm::vec3 feetPosition) : feet_(feetPosition) {
    syncCameraToBody();
}

void PlayerController::syncCameraToBody() {
    camera_.position = feet_ + glm::vec3(0.0f, kEyeHeight, 0.0f);
}

void PlayerController::teleport(glm::vec3 feet) {
    feet_ = feet;
    velocity_ = glm::vec3(0.0f);
    syncCameraToBody();
}

void PlayerController::setMode(Mode m) {
    mode_ = m;
    velocity_ = glm::vec3(0.0f);
}

void PlayerController::update(float dt, const InputState& input) {
    // --- Look --------------------------------------------------------------
    camera_.addLook(input.look.x, input.look.y, kSensitivity);

    // --- Mode toggle -------------------------------------------------------
    if (input.toggleFreeFly) {
        mode_ = (mode_ == Mode::Walking) ? Mode::FreeFly : Mode::Walking;
        velocity_ = glm::vec3(0.0f);
    }

    if (mode_ == Mode::FreeFly) {
        // Fly along the full look direction; no gravity or collision.
        glm::vec3 wish = camera_.front() * input.move.y +
                         camera_.rightHorizontal() * input.move.x;
        wish.y += (input.ascend ? 1.0f : 0.0f) - (input.descend ? 1.0f : 0.0f);
        if (glm::dot(wish, wish) > 0.0f) {
            wish = glm::normalize(wish);
        }
        const float speed = input.sprint ? kFlySprint : kFlySpeed;
        feet_ += wish * speed * dt;
        syncCameraToBody();
        return;
    }

    // --- Walking -----------------------------------------------------------
    // Horizontal velocity is set directly from input (no momentum, which feels
    // responsive for a blocky game). Vertical velocity is driven by gravity.
    glm::vec3 wish = camera_.forwardHorizontal() * input.move.y +
                     camera_.rightHorizontal() * input.move.x;
    if (glm::dot(wish, wish) > 0.0f) {
        wish = glm::normalize(wish);
    }
    const float speed = input.sprint ? kSprintSpeed : kWalkSpeed;
    velocity_.x = wish.x * speed;
    velocity_.z = wish.z * speed;

    velocity_.y += kGravity * dt;
    if (onGround_ && input.jump) {
        velocity_.y = kJumpSpeed;
    }

    // Integrate + resolve collisions per axis (allows sliding along walls).
    onGround_ = false;
    const glm::vec3 delta = velocity_ * dt;
    moveAxis(feet_, delta.x, 0);
    moveAxis(feet_, delta.z, 2);
    moveAxis(feet_, delta.y, 1);

    syncCameraToBody();
}

bool PlayerController::collides(const glm::vec3& feet) const {
    if (!isSolid_) {
        return false;
    }
    const glm::vec3 mn(feet.x - kHalfWidth, feet.y, feet.z - kHalfWidth);
    const glm::vec3 mx(feet.x + kHalfWidth, feet.y + kHeight, feet.z + kHalfWidth);

    const int x0 = static_cast<int>(std::floor(mn.x));
    const int x1 = static_cast<int>(std::floor(mx.x));
    const int y0 = static_cast<int>(std::floor(mn.y));
    const int y1 = static_cast<int>(std::floor(mx.y));
    const int z0 = static_cast<int>(std::floor(mn.z));
    const int z1 = static_cast<int>(std::floor(mx.z));

    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            for (int z = z0; z <= z1; ++z) {
                if (isSolid_(x, y, z)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void PlayerController::moveAxis(glm::vec3& feet, float delta, int axis) {
    if (delta == 0.0f) {
        return;
    }
    glm::vec3 test = feet;
    test[axis] += delta;
    if (!collides(test)) {
        feet = test;
        return;
    }
    // Blocked on this axis: cancel velocity, and note landing when moving down.
    if (axis == 1) {
        if (delta < 0.0f) {
            onGround_ = true;
        }
        velocity_.y = 0.0f;
    } else {
        velocity_[axis] = 0.0f;
    }
    // NOTE: leaves a small gap up to one frame's movement. A swept-AABB solve
    // (move exactly to contact) is a future refinement. TODO(future)
}

} // namespace vg
