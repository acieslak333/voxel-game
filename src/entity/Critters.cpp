#include "entity/Critters.h"

#include <cmath>

namespace vg {

namespace {
constexpr float kGravity   = 22.0f; // blocks/s^2
constexpr float kWalkSpeed = 1.4f;  // blocks/s while ambling
constexpr float kTwoPi     = 6.2831853f;

int ifloor(float v) { return static_cast<int>(std::floor(v)); }
} // namespace

float Critters::frand() {
    // xorshift-ish LCG -> [0,1). Determinism doesn't matter for wandering mobs.
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) / 16777216.0f;
}

void Critters::spawn(const glm::vec3& feet) {
    Critter c;
    c.pos     = feet;
    c.yaw     = frand() * kTwoPi;
    c.wanderT = 0.5f + frand() * 2.0f;
    critters_.push_back(c);
}

void Critters::update(float dt, const SolidFn& solid) {
    // Solid test at a world point (treats the critter as ~1 block wide/tall).
    auto solidAt = [&](float x, float y, float z) {
        return solid(ifloor(x), ifloor(y), ifloor(z));
    };

    for (Critter& c : critters_) {
        // --- Decide a new wander state periodically ---------------------------
        c.wanderT -= dt;
        if (c.wanderT <= 0.0f) {
            c.walking = frand() < 0.65f;          // mostly ambling, sometimes idle
            if (c.walking) c.yaw = frand() * kTwoPi;
            c.wanderT = 1.5f + frand() * 3.0f;
        }

        // --- Horizontal amble (only when grounded) ---------------------------
        if (c.walking && c.onGround) {
            const glm::vec3 d(std::sin(c.yaw), 0.0f, std::cos(c.yaw));
            const float nx = c.pos.x + d.x * kWalkSpeed * dt;
            const float nz = c.pos.z + d.z * kWalkSpeed * dt;
            // Blocked at body or head height -> turn away and try later.
            const bool blocked = solidAt(nx, c.pos.y + 0.1f, nz) ||
                                 solidAt(nx, c.pos.y + 1.1f, nz);
            if (blocked) {
                c.yaw += 2.0f + frand();          // veer
                c.wanderT = std::min(c.wanderT, 0.2f);
            } else {
                c.pos.x = nx;
                c.pos.z = nz;
                c.animTime += dt;                  // advance the walk cycle
            }
        }

        // --- Gravity + ground snap -------------------------------------------
        c.vy -= kGravity * dt;
        float ny = c.pos.y + c.vy * dt;
        if (c.vy <= 0.0f && solidAt(c.pos.x, ny, c.pos.z)) {
            c.pos.y  = static_cast<float>(ifloor(ny)) + 1.0f; // rest on the block top
            c.vy     = 0.0f;
            c.onGround = true;
        } else {
            c.pos.y  = ny;
            c.onGround = false;
        }
    }
}

} // namespace vg
