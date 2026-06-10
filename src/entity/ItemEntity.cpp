#include "entity/ItemEntity.h"

#include <cmath>

namespace vg {

namespace {
constexpr float kGravity   = -20.0f; // m/s^2 (lighter than the player: items float a touch)
constexpr float kRest      = 0.18f;  // item half-height: where it rests above the block top
constexpr float kMagnetAcc = 22.0f;  // pull acceleration toward the player
constexpr float kDrag      = 4.0f;   // horizontal damping so settled items stop sliding
} // namespace

void ItemEntities::spawn(const glm::vec3& pos, const ItemStack& stack, const glm::vec3& vel) {
    if (stack.empty()) return;
    ItemEntity e;
    e.pos = pos;
    e.vel = vel;
    e.stack = stack;
    items_.push_back(e);
}

int ItemEntities::update(float dt, const SolidFn& solid, const glm::vec3& playerFeet,
                         Inventory& inv) {
    const glm::vec3 body = playerFeet + glm::vec3(0.0f, 0.9f, 0.0f); // aim at the torso
    int collected = 0;

    for (size_t i = 0; i < items_.size();) {
        ItemEntity& e = items_[i];
        e.age += dt;
        e.spin += dt * 2.0f;

        // Gravity + settle on the first solid block below.
        e.vel.y += kGravity * dt;
        glm::vec3 next = e.pos + e.vel * dt;
        const int bx = static_cast<int>(std::floor(next.x));
        const int bz = static_cast<int>(std::floor(next.z));
        const int below = static_cast<int>(std::floor(next.y - kRest));
        if (solid && e.vel.y < 0.0f && solid(bx, below, bz)) {
            next.y = static_cast<float>(below) + 1.0f + kRest; // rest on top of the block
            e.vel.y = 0.0f;
            // Horizontal drag once grounded so items don't drift forever.
            e.vel.x -= e.vel.x * std::min(1.0f, kDrag * dt);
            e.vel.z -= e.vel.z * std::min(1.0f, kDrag * dt);
        }
        e.pos = next;

        // Magnet + pickup, once the spawn delay has elapsed.
        if (e.age >= kPickupDelay) {
            const glm::vec3 to = body - e.pos;
            const float d2 = glm::dot(to, to);
            if (d2 <= kPickupRadius * kPickupRadius) {
                const int leftover = inv.add(e.stack.blockId, e.stack.count);
                if (leftover == 0) {
                    items_[i] = items_.back();
                    items_.pop_back();
                    ++collected;
                    continue; // don't advance i: a new entity is now at slot i
                }
                e.stack.count = static_cast<uint16_t>(leftover); // inventory full: keep the rest
            } else if (d2 <= kMagnetRadius * kMagnetRadius) {
                const float d = std::sqrt(d2);
                e.vel += (to / d) * kMagnetAcc * dt; // accelerate toward the player
            }
        }
        ++i;
    }
    return collected;
}

} // namespace vg
