#pragma once

#include "player/Inventory.h" // ItemStack, Inventory

#include <functional>
#include <glm/glm.hpp>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  ItemEntity / ItemEntities 
// -----------------------------------------------------------------------------
//  Dropped items that live in the world as physical entities: they fall, settle on
//  the ground, get magnetised toward a nearby player and are picked up on contact
//  (after a short delay so a just-dropped stack isn't instantly re-collected).
//
//  The simulation is pure (collision via a solid() predicate, like PlayerController)
//  so it's headlessly testable; RENDERING is separate and will hang off the
//  EntityRenderer (billboarded quads) — until then this can run with an empty list.
// -----------------------------------------------------------------------------
struct ItemEntity {
    glm::vec3 pos{0.0f};       // world position (item centre)
    glm::vec3 vel{0.0f};
    ItemStack stack;           // what was dropped
    float     age = 0.0f;      // seconds since spawn (gates pickup)
    float     spin = 0.0f;     // visual yaw for the future billboard render
};

class ItemEntities {
public:
    using SolidFn = std::function<bool(int x, int y, int z)>;

    // Drop `stack` at `pos` with an optional initial velocity (e.g. a little pop).
    void spawn(const glm::vec3& pos, const ItemStack& stack,
               const glm::vec3& vel = glm::vec3(0.0f));

    // Advance all items: gravity + ground settle, magnetise toward the player and
    // collect on contact (after kPickupDelay) into `inv`. Returns how many stacks
    // were picked up this step. Removes collected/dead entities.
    int update(float dt, const SolidFn& solid, const glm::vec3& playerFeet, Inventory& inv);

    [[nodiscard]] const std::vector<ItemEntity>& items() const { return items_; }
    [[nodiscard]] size_t size() const { return items_.size(); }
    void clear() { items_.clear(); }

    static constexpr float kPickupRadius = 1.4f; // collect within this of the body
    static constexpr float kMagnetRadius = 3.2f; // start pulling within this
    static constexpr float kPickupDelay  = 0.5f; // seconds before an item can be collected

private:
    std::vector<ItemEntity> items_;
};

} // namespace vg
