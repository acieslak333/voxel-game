#include "player/PlayerController.h"

#include "core/Input.h"

#include <algorithm>
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
    camera_.addLook(input.look.x, input.look.y, sensitivity_);

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
        // Sprinting flies ~2.17x faster (the old kFlySprint / kFlySpeed ratio).
        const float speed = flySpeed_ * (input.sprint ? (kFlySprint / kFlySpeed) : 1.0f);
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

bool PlayerController::occupies(int bx, int by, int bz) const {
    // The player AABB vs the unit cube [b, b+1] on each axis: overlap on all
    // three axes means the block would intersect the player.
    return feet_.x - kHalfWidth < bx + 1.0f && feet_.x + kHalfWidth > bx &&
           feet_.y               < by + 1.0f && feet_.y + kHeight    > by &&
           feet_.z - kHalfWidth < bz + 1.0f && feet_.z + kHalfWidth > bz;
}

void PlayerController::moveAxis(glm::vec3& feet, float delta, int axis) {
    if (delta == 0.0f || !isSolid_) {
        feet[axis] += delta;
        return;
    }

    // Small slack so a face that merely *touches* a block plane is treated as in
    // contact (not as overlapping), which keeps the player from sticking or
    // tunnelling at exact integer boundaries.
    constexpr float kEps = 1e-4f;

    // The player AABB. The two axes other than `axis` don't move this call, so
    // their span fixes the footprint we sweep through.
    const glm::vec3 lo(feet.x - kHalfWidth, feet.y,           feet.z - kHalfWidth);
    const glm::vec3 hi(feet.x + kHalfWidth, feet.y + kHeight, feet.z + kHalfWidth);

    const int o1 = (axis + 1) % 3;
    const int o2 = (axis + 2) % 3;
    // Footprint block range on the two stationary axes (shrunk by kEps so a box
    // flush against a plane doesn't pick up the block on the far side).
    const int a0 = static_cast<int>(std::floor(lo[o1] + kEps));
    const int a1 = static_cast<int>(std::floor(hi[o1] - kEps));
    const int b0 = static_cast<int>(std::floor(lo[o2] + kEps));
    const int b1 = static_cast<int>(std::floor(hi[o2] - kEps));

    // The collision AABB of a solid cell, accounting for thin (Model) blocks whose
    // box is a centred column inset on X/Z (full height on Y). False if not solid.
    auto cellBox = [&](int cx, int cy, int cz, glm::vec3& bmin, glm::vec3& bmax) {
        if (!isSolid_(cx, cy, cz)) {
            return false;
        }
        bmin = glm::vec3(cx, cy, cz);
        bmax = bmin + glm::vec3(1.0f);
        const float ins = collisionInset_ ? collisionInset_(cx, cy, cz) : 0.0f;
        if (ins > 0.0f) {
            bmin.x += ins; bmin.z += ins;
            bmax.x -= ins; bmax.z -= ins;
        }
        return true;
    };

    // Nearest contact coordinate along `axis` among blocks in slab cell-index `idx`
    // whose box actually overlaps the player's footprint on the two stationary axes
    // (a thin block may sit in the same cell yet miss the player). `positive` picks
    // the block's near face: its min side when moving +, its max side when moving -.
    auto slabFace = [&](int idx, bool positive, float& outFace) {
        bool  any  = false;
        float best = positive ? 1e30f : -1e30f;
        glm::vec3 bmin, bmax;
        glm::ivec3 c;
        c[axis] = idx;
        for (int a = a0; a <= a1; ++a) {
            for (int b = b0; b <= b1; ++b) {
                c[o1] = a;
                c[o2] = b;
                if (!cellBox(c.x, c.y, c.z, bmin, bmax)) {
                    continue;
                }
                if (hi[o1] - kEps <= bmin[o1] || lo[o1] + kEps >= bmax[o1]) continue;
                if (hi[o2] - kEps <= bmin[o2] || lo[o2] + kEps >= bmax[o2]) continue;
                const float face = positive ? bmin[axis] : bmax[axis];
                if (positive ? (face < best) : (face > best)) {
                    best = face;
                }
                any = true;
            }
        }
        outFace = best;
        return any;
    };

    float allowed = delta;
    bool  blocked = false;
    if (delta > 0.0f) {
        // Leading face hi[axis] sweeps forward; for each cell it may enter, the
        // contact is the block's near (min) face — inset for a thin block.
        const float face  = hi[axis];
        const int   first = static_cast<int>(std::floor(face + kEps));
        const int   last  = static_cast<int>(std::floor(face + delta + kEps));
        for (int i = first; i <= last; ++i) {
            float bf;
            if (slabFace(i, true, bf)) {
                const float cand = bf - face;
                if (cand >= -kEps) {
                    allowed = std::clamp(cand, 0.0f, delta);
                    blocked = true;
                    break;
                }
            }
        }
    } else {
        // Leading face lo[axis] sweeps back; the contact is the block's max face.
        const float face  = lo[axis];
        const int   first = static_cast<int>(std::floor(face - kEps));
        const int   last  = static_cast<int>(std::floor(face + delta - kEps));
        for (int i = first; i >= last; --i) {
            float bf;
            if (slabFace(i, false, bf)) {
                const float cand = bf - face;
                if (cand <= kEps) {
                    allowed = std::clamp(cand, delta, 0.0f);
                    blocked = true;
                    break;
                }
            }
        }
    }

    feet[axis] += allowed;

    if (blocked) {
        // Cancel the into-surface velocity (so gravity/run speed don't accumulate)
        // and record a landing when we were moving down.
        if (axis == 1) {
            if (delta < 0.0f) {
                onGround_ = true;
            }
            velocity_.y = 0.0f;
        } else {
            velocity_[axis] = 0.0f;
        }
    }
}

} // namespace vg
