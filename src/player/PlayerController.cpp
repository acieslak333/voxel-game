/**
 * @file PlayerController.cpp
 * @brief Implementation of walking physics, free-fly, collision sweep, and health.
 *
 * Contains the per-axis swept AABB collision (moveAxis), auto-step onto slabs/stairs,
 * sneak edge-stop, swim/drown physics, passive health regen, and view-bob smoothing.
 * All tunables are anonymous-namespace constants (see CLAUDE.md — REVIEW R7).
 * @see docs/CODE_INDEX.md
 */

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
// Auto-step: walk up obstacles no taller than this without jumping (half-slabs and
// stair steps are 0.5; below 1.0 so you can't walk up a full block).
constexpr float kStepHeight  = 0.6f;

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

// --- Camera feel (smoothing, inertia, bob) ----------------------------------
// Exponential rates: higher = snappier, lower = softer/slower. ~9 reaches ~85% in
// ~0.22s (a gentle glide rather than a snap).
constexpr float kMoveAccel      = 9.0f;  // horizontal velocity ease toward target
constexpr float kStepSmoothRate = 9.0f;  // camera ease over an auto-step / small drop
// View bob: subtle + soft. Vertical sways at twice the lateral frequency (foot-fall
// cadence); phase advances with distance travelled so it tracks speed. Blocks.
constexpr float kBobRamp     = 6.0f;   // how gently the bob fades in/out with motion
constexpr float kBobPerBlock = 1.8f;   // bob-phase radians per block walked (slower cadence)
constexpr float kBobVert     = 0.028f; // vertical bob amplitude (small)
constexpr float kBobHoriz    = 0.020f; // lateral bob amplitude (small)
} // namespace

/// Initialise feet position and snap the camera to the body (no smoothing lag on spawn).
PlayerController::PlayerController(glm::vec3 feetPosition) : feet_(feetPosition) {
    syncCameraToBody();
}

void PlayerController::syncCameraToBody() {
    // Snap the smoothed eye to the true eye (used for spawn/teleport/free-fly, where
    // there's nothing to ease). The walking path eases camEyeY_ itself, see update().
    const float eye = kEyeHeight - (sneaking_ ? kCrouchDrop : 0.0f);
    camEyeY_ = feet_.y + eye;
    camera_.position = glm::vec3(feet_.x, camEyeY_, feet_.z);
}

/// Move to a new feet position and reset velocity; restores full breath and clears bob.
void PlayerController::teleport(glm::vec3 feet) {
    feet_ = feet;
    velocity_ = glm::vec3(0.0f);
    air_ = maxAir_; // surface / respawn -> full breath
    bobAmt_ = 0.0f; // no bob mid-jump after a teleport/respawn
    syncCameraToBody();
}

/// Switch locomotion mode and zero velocity so there is no residual drift.
void PlayerController::setMode(Mode m) {
    mode_ = m;
    velocity_ = glm::vec3(0.0f);
}

/// Pure fall-damage calculation: converts impact speed to height, subtracts safe-fall, scales.
float PlayerController::fallDamage(float impactSpeed) {
    if (impactSpeed <= 0.0f) return 0.0f;
    // Height the impact speed corresponds to (v^2 = 2 g h).
    const float h = impactSpeed * impactSpeed / (2.0f * -kGravity);
    return std::max(0.0f, h - kSafeFall) * kFallDmgPerBlock;
}

/// Apply damage after armour reduction; no-op when invulnerable or amount <= 0.
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
        // Holding Shift (sprint) flies ~2.17x faster (kFlySprint/kFlySpeed).
        const float speed = flySpeed_ * (input.sprint ? (kFlySprint / kFlySpeed) : 1.0f);
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

    // Horizontal velocity eases toward the input-driven target — a small inertia so
    // starting/stopping/turning ramps over ~0.12s instead of snapping, while staying
    // snappy. Swimming damps it. Vertical velocity is gravity (buoyant when submerged).
    glm::vec3 wish = camera_.forwardHorizontal() * input.move.y +
                     camera_.rightHorizontal() * input.move.x;
    if (glm::dot(wish, wish) > 0.0f) {
        wish = glm::normalize(wish);
    }
    float speed = sneaking_ ? kSneakSpeed
                            : (input.sprint ? kSprintSpeed : kWalkSpeed);
    speed *= speedMul_;
    if (bodyInWater) speed *= kWaterHorizontalScale;
    const float accelF = 1.0f - std::exp(-dt * kMoveAccel);
    velocity_.x += (wish.x * speed - velocity_.x) * accelF;
    velocity_.z += (wish.z * speed - velocity_.z) * accelF;

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
    const glm::vec3 startFeet = feet_;
    auto horizontalMove = [&]() {
        const float prevX = feet_.x;
        moveAxis(feet_, delta.x, 0);
        if (edgeStop && !hasGroundSupport()) { feet_.x = prevX; velocity_.x = 0.0f; }
        const float prevZ = feet_.z;
        moveAxis(feet_, delta.z, 2);
        if (edgeStop && !hasGroundSupport()) { feet_.z = prevZ; velocity_.z = 0.0f; }
    };
    horizontalMove();

    // Auto-step: if grounded and a low obstacle stopped the horizontal move short,
    // retry it lifted by the step height, then drop back onto the step — so you walk
    // straight up slabs and stairs without jumping. Disabled while sneaking (the
    // edge-stop owns crouch movement) and only kept if it actually gains ground.
    constexpr float kEps = 1e-4f;
    const bool blocked = std::abs(feet_.x - startFeet.x) + kEps < std::abs(delta.x) ||
                         std::abs(feet_.z - startFeet.z) + kEps < std::abs(delta.z);
    if (wasGrounded && !edgeStop && blocked) {
        const glm::vec3 flat = feet_; // result without stepping
        auto horiz2 = [&](const glm::vec3& p) {
            const float dx = p.x - startFeet.x, dz = p.z - startFeet.z;
            return dx * dx + dz * dz;
        };
        feet_ = startFeet;
        const float yBefore = feet_.y;
        moveAxis(feet_, kStepHeight, 1);          // rise (capped by any ceiling)
        const float climbed = feet_.y - yBefore;
        horizontalMove();
        moveAxis(feet_, -climbed, 1);             // settle down onto the step
        if (horiz2(feet_) <= horiz2(flat) + kEps) {
            feet_ = flat;                         // no gain -> keep the flat result
        }
    }

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

    // --- Camera: smooth small vertical steps, snap big ones, add view bob --------
    // The body steps onto a slab/stair (or drops off one) in a single frame. Ease the
    // rendered eye toward the true eye for those small grounded steps so they read
    // smoothly; snap for jumps/falls/teleports (a large diff or being airborne) so
    // they stay crisp.
    const float eye      = kEyeHeight - (sneaking_ ? kCrouchDrop : 0.0f);
    const float trueEyeY = feet_.y + eye;
    const float diff     = trueEyeY - camEyeY_;
    if (wasGrounded && std::fabs(diff) <= kStepHeight + kCrouchDrop + 0.05f) {
        camEyeY_ += diff * (1.0f - std::exp(-dt * kStepSmoothRate));
    } else {
        camEyeY_ = trueEyeY; // jump / fall / first frame: no lag
    }

    // View bob: a subtle sway that ramps in with horizontal speed while grounded and
    // out when you stop. Vertical at twice the lateral cadence (foot-falls); the phase
    // advances with distance so it tracks walk/sprint speed. Off => bobAmt_ decays to 0.
    const float hspeed    = std::sqrt(velocity_.x * velocity_.x + velocity_.z * velocity_.z);
    const float targetBob = (viewBob_ && onGround_) ? std::min(hspeed / kWalkSpeed, 1.3f) : 0.0f;
    bobAmt_ += (targetBob - bobAmt_) * (1.0f - std::exp(-dt * kBobRamp));
    if (hspeed > 0.1f) bobPhase_ += hspeed * dt * kBobPerBlock;
    glm::vec3 bob(0.0f);
    if (bobAmt_ > 1e-3f) {
        bob.y = std::sin(bobPhase_ * 2.0f) * kBobVert * bobAmt_;
        bob += camera_.rightHorizontal() * (std::sin(bobPhase_) * kBobHoriz * bobAmt_);
    }
    camera_.position = glm::vec3(feet_.x, camEyeY_, feet_.z) + bob;
}

/// Scan the block layer immediately under the AABB footprint; used by the sneak edge-stop.
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

/// True when the player AABB overlaps the unit cube at (bx, by, bz); guards block placement.
bool PlayerController::occupies(int bx, int by, int bz) const {
    // The player AABB vs the unit cube [b, b+1] on each axis: overlap on all
    // three axes means the block would intersect the player.
    return feet_.x - kHalfWidth < bx + 1.0f && feet_.x + kHalfWidth > bx &&
           feet_.y               < by + 1.0f && feet_.y + kHeight    > by &&
           feet_.z - kHalfWidth < bz + 1.0f && feet_.z + kHalfWidth > bz;
}

/// Sweep the player AABB one axis at a time, stopping at the nearest contact plane.
/// @param feet  Current feet position, modified in place.
/// @param delta Signed displacement (blocks) along `axis`.
/// @param axis  0 = X, 1 = Y, 2 = Z.
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

    // The collision boxes of a cell, in world coords. A full cube is one box; a
    // reshaped block (slab/stairs/post/wall) or thin Model post returns several.
    // Returns the count (0 = not solid). Falls back to a full cube if no box
    // provider was wired (keeps unit tests / minimal setups working).
    auto cellBoxes = [&](int cx, int cy, int cz, ShapeBox out[]) -> int {
        if (boxesFn_) return boxesFn_(cx, cy, cz, out);
        if (!isSolid_(cx, cy, cz)) return 0;
        out[0] = {glm::vec3(cx, cy, cz), glm::vec3(cx + 1, cy + 1, cz + 1)};
        return 1;
    };

    // Nearest contact coordinate along `axis` among the boxes in slab cell-index
    // `idx` that actually overlap the player's footprint on the two stationary axes
    // (a partial box may sit in the cell yet miss the player). `positive` picks the
    // box's near face: its min side when moving +, its max side when moving -.
    auto slabFace = [&](int idx, bool positive, float& outFace) {
        bool  any  = false;
        float best = positive ? 1e30f : -1e30f;
        ShapeBox boxes[kMaxShapeBoxes];
        glm::ivec3 c;
        c[axis] = idx;
        for (int a = a0; a <= a1; ++a) {
            for (int b = b0; b <= b1; ++b) {
                c[o1] = a;
                c[o2] = b;
                const int n = cellBoxes(c.x, c.y, c.z, boxes);
                for (int i = 0; i < n; ++i) {
                    const glm::vec3& bmin = boxes[i].lo;
                    const glm::vec3& bmax = boxes[i].hi;
                    if (hi[o1] - kEps <= bmin[o1] || lo[o1] + kEps >= bmax[o1]) continue;
                    if (hi[o2] - kEps <= bmin[o2] || lo[o2] + kEps >= bmax[o2]) continue;
                    const float face = positive ? bmin[axis] : bmax[axis];
                    if (positive ? (face < best) : (face > best)) {
                        best = face;
                    }
                    any = true;
                }
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
