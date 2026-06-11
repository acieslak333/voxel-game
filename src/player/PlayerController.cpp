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
constexpr float kSneakSpeed  = 1.4f;   // crouch walk speed
constexpr float kCrouchDrop  = 0.28f;  // camera lowered this much while sneaking
constexpr float kFlySpeed    = 12.0f;
constexpr float kFlySprint   = 26.0f;

constexpr float kGravity     = -26.0f;  // m/s^2
constexpr float kJumpSpeed   = 8.4f;    // gives ~1.35 block jump height

// Fall damage: falls up to kSafeFall blocks are harmless; beyond that you lose
// kFallDmgPerBlock HP per extra block (so on a 100-HP scale a ~23-block drop kills).
constexpr float kSafeFall       = 3.0f;
constexpr float kFallDmgPerBlock = 5.0f;
// Passive regen: after kRegenDelay seconds without a hit, heal kRegenRate HP/s.
constexpr float kRegenDelay = 5.0f;
constexpr float kRegenRate  = 2.0f;

// Swim physics (body in water): buoyancy weakens gravity, vertical drag damps the
// fall to a slow sink, and holding jump swims up. Horizontal swim is slower.
constexpr float kWaterGravityScale    = 0.28f; // effective gravity while submerged
constexpr float kWaterVerticalDrag    = 6.0f;  // s^-1 exponential damping of vy
constexpr float kWaterSinkSpeedMax    = 2.2f;  // terminal sink speed (m/s)
constexpr float kSwimUpSpeed          = 4.0f;  // rise speed while holding jump
constexpr float kWaterHorizontalScale = 0.62f; // horizontal speed multiplier
// Drowning: breath drains while the head is submerged; once empty you take HP/s.
constexpr float kAirRefillRate     = 4.0f; // breath/s recovered with the head out
constexpr float kDrownDamagePerSec = 4.0f; // HP/s once breath hits 0
} // namespace

PlayerController::PlayerController(glm::vec3 feetPosition) : feet_(feetPosition) {
    syncCameraToBody();
}

void PlayerController::syncCameraToBody() {
    const float eye = kEyeHeight - (sneaking_ ? kCrouchDrop : 0.0f);
    camera_.position = feet_ + glm::vec3(0.0f, eye, 0.0f);
}

void PlayerController::teleport(glm::vec3 feet) {
    feet_ = feet;
    velocity_ = glm::vec3(0.0f);
    air_ = maxAir_; // surface / respawn -> full breath
    syncCameraToBody();
}

void PlayerController::setMode(Mode m) {
    mode_ = m;
    velocity_ = glm::vec3(0.0f);
}

float PlayerController::fallDamage(float impactSpeed) {
    if (impactSpeed <= 0.0f) return 0.0f;
    // Height the impact speed corresponds to (v^2 = 2 g h).
    const float h = impactSpeed * impactSpeed / (2.0f * -kGravity);
    return std::max(0.0f, h - kSafeFall) * kFallDmgPerBlock;
}

void PlayerController::damage(float amount) {
    if (invulnerable_ || amount <= 0.0f) return;
    amount *= (1.0f - armorReduction_); // armour soaks a fraction of every hit
    health_ = std::max(0.0f, health_ - amount);
    regenDelay_ = kRegenDelay; // hold off regen so it isn't instantly undone
}

void PlayerController::heal(float amount) {
    if (amount <= 0.0f) return;
    health_ = std::min(maxHealth_, health_ + amount);
}

void PlayerController::setHealth(float h) {
    health_ = std::clamp(h, 0.0f, maxHealth_);
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
        // Holding Shift (sneak key) flies ~2.17x faster (old kFlySprint/kFlySpeed).
        const float speed = flySpeed_ * (input.sneak ? (kFlySprint / kFlySpeed) : 1.0f);
        feet_ += wish * speed * dt;
        syncCameraToBody();
        return;
    }

    // --- Walking -----------------------------------------------------------
    // Sample whether the body / head are in water this frame (drives swim physics
    // + drowning). Cheap: a couple of cell lookups down the player's centre column.
    bool bodyInWater = false, headInWater = false;
    if (isWater_) {
        const int cx = static_cast<int>(std::floor(feet_.x));
        const int cz = static_cast<int>(std::floor(feet_.z));
        bodyInWater = isWater_(cx, static_cast<int>(std::floor(feet_.y + 0.1f)), cz) ||
                      isWater_(cx, static_cast<int>(std::floor(feet_.y + kHeight * 0.5f)), cz);
        headInWater = isWater_(cx, static_cast<int>(std::floor(feet_.y + kEyeHeight)), cz);
    }
    inWater_ = bodyInWater;
    // Sneaking (crouch): slower walk + a lowered camera + edge-stop below. Sneak
    // wins over sprint. Not while swimming (you tread water, not crouch).
    sneaking_ = input.sneak && !bodyInWater;

    // Horizontal velocity is set directly from input (no momentum, which feels
    // responsive for a blocky game). Swimming damps it. Vertical velocity is driven
    // by gravity, weakened by buoyancy while submerged.
    glm::vec3 wish = camera_.forwardHorizontal() * input.move.y +
                     camera_.rightHorizontal() * input.move.x;
    if (glm::dot(wish, wish) > 0.0f) {
        wish = glm::normalize(wish);
    }
    float speed = sneaking_ ? kSneakSpeed
                            : (input.sprint ? kSprintSpeed : kWalkSpeed);
    speed *= speedMul_;
    if (bodyInWater) speed *= kWaterHorizontalScale;
    velocity_.x = wish.x * speed;
    velocity_.z = wish.z * speed;

    if (bodyInWater) {
        // Buoyant, damped vertical motion: weak gravity, exponential drag toward a
        // slow sink, and swim up while holding jump. No instant fall-through.
        velocity_.y += kGravity * kWaterGravityScale * dt;
        velocity_.y -= velocity_.y * kWaterVerticalDrag * dt;
        velocity_.y = std::max(velocity_.y, -kWaterSinkSpeedMax);
        if (input.jump) velocity_.y = kSwimUpSpeed;
    } else {
        velocity_.y += kGravity * dt;
        if (onGround_ && input.jump) {
            velocity_.y = kJumpSpeed * jumpMul_;
        }
    }

    // Integrate + resolve collisions per axis (allows sliding along walls). Capture
    // the downward speed just before the Y sweep so a landing can score fall damage.
    const bool wasGrounded = onGround_;
    const bool wasAirborne = !onGround_;
    const float impactSpeed = -velocity_.y; // >0 while falling
    onGround_ = false;
    const glm::vec3 delta = velocity_ * dt;

    // Sneak edge-stop: while crouched on the ground, refuse a horizontal step that
    // would leave the player hanging over a drop (Minecraft-style). Try each axis,
    // and undo it if the footprint ends up with no solid block beneath it.
    const bool edgeStop = sneaking_ && wasGrounded;
    const float prevX = feet_.x;
    moveAxis(feet_, delta.x, 0);
    if (edgeStop && !hasGroundSupport()) { feet_.x = prevX; velocity_.x = 0.0f; }
    const float prevZ = feet_.z;
    moveAxis(feet_, delta.z, 2);
    if (edgeStop && !hasGroundSupport()) { feet_.z = prevZ; velocity_.z = 0.0f; }
    moveAxis(feet_, delta.y, 1);

    // Landed this frame after falling: apply fall damage from the impact speed —
    // unless we're in water, which breaks the fall (Minecraft-style).
    if (onGround_ && wasAirborne && !bodyInWater) {
        damage(fallDamage(impactSpeed));
    }

    // Breath / drowning: drains while the head is submerged; once empty, continuous
    // damage. Refills quickly with the head above water.
    if (headInWater) {
        air_ = std::max(0.0f, air_ - dt);
        if (air_ <= 0.0f) damage(kDrownDamagePerSec * dt);
    } else {
        air_ = std::min(maxAir_, air_ + dt * kAirRefillRate);
    }

    // Slow passive regen once you've gone a few seconds without taking damage
    // (there's no hunger to gate it, by design).
    if (regenDelay_ > 0.0f) {
        regenDelay_ = std::max(0.0f, regenDelay_ - dt);
    } else {
        heal((kRegenRate + regenBonus_) * dt);
    }

    syncCameraToBody();
}

bool PlayerController::hasGroundSupport() const {
    if (!isSolid_) return true; // no world knowledge -> assume supported
    // The block layer just under the feet, scanned across the AABB footprint
    // (shrunk slightly so a box flush against an edge doesn't borrow the far cell).
    const int y  = static_cast<int>(std::floor(feet_.y - 1e-3f));
    const int x0 = static_cast<int>(std::floor(feet_.x - kHalfWidth + 1e-4f));
    const int x1 = static_cast<int>(std::floor(feet_.x + kHalfWidth - 1e-4f));
    const int z0 = static_cast<int>(std::floor(feet_.z - kHalfWidth + 1e-4f));
    const int z1 = static_cast<int>(std::floor(feet_.z + kHalfWidth - 1e-4f));
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z)
            if (isSolid_(x, y, z)) return true;
    return false;
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
