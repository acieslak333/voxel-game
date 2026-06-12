#include "core/App.h"

#include "core/Ui.h"
#include "player/PlayerSave.h"
#include "world/Raycast.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Asset/shader directories are baked in at build time by CMake so the executable
// can find them regardless of the working directory.
#ifndef VG_SHADER_DIR
#define VG_SHADER_DIR "shaders"
#endif
#ifndef VG_ASSET_DIR
#define VG_ASSET_DIR "assets"
#endif

namespace vg {

namespace {
// The shapes the hammer radial offers, in display order (left to right).
constexpr ShapeKind kPickerShapes[] = {
    ShapeKind::Cube, ShapeKind::Slab, ShapeKind::Stairs,
    ShapeKind::Post, ShapeKind::Wall, ShapeKind::VerticalSlab,
};
constexpr int kPickerShapeCount = static_cast<int>(sizeof(kPickerShapes) /
                                                   sizeof(kPickerShapes[0]));
int shapeIndex(ShapeKind k) {
    for (int i = 0; i < kPickerShapeCount; ++i)
        if (kPickerShapes[i] == k) return i;
    return 0;
}

// Display name for a block shape (HUD indicator + picker labels).
const char* shapeName(ShapeKind k) {
    switch (k) {
        case ShapeKind::Cube:         return "Cube";
        case ShapeKind::Slab:         return "Slab";
        case ShapeKind::Stairs:       return "Stairs";
        case ShapeKind::Post:         return "Post";
        case ShapeKind::Wall:         return "Wall";
        case ShapeKind::VerticalSlab: return "Vertical Slab";
    }
    return "Cube";
}

// Orientation byte for a shape being placed / applied, from the ray hit (which
// face + where on the cell) and the player's horizontal facing. Predictable rule:
// the material goes where you point. A slab fills the half you aimed at; a stair
// ascends in the direction you face (upside-down only when you hit the underside);
// a post takes the clicked face's axis; a vertical slab hugs the clicked side.
// `frac` is the hit point's fractional position within its cell.
uint8_t orientForPlacement(ShapeKind shape, const glm::ivec3& normal,
                           const glm::vec3& frac, const glm::vec3& viewDir) {
    auto facingFromView = [&]() -> int {
        return (std::abs(viewDir.x) >= std::abs(viewDir.z))
                   ? (viewDir.x >= 0.0f ? 1 : 3)   // +X : -X
                   : (viewDir.z >= 0.0f ? 2 : 0);  // +Z : -Z
    };
    auto sideFromNormal = [&]() -> int {
        if (normal.x > 0) return 1; // +X face
        if (normal.x < 0) return 3; // -X
        if (normal.z > 0) return 2; // +Z
        return 0;                   // -Z
    };
    switch (shape) {
        case ShapeKind::Slab:
            // The half you aimed at stays. A horizontal face pins it exactly (the hit
            // point sits on a cell boundary, so frac.y is ambiguous there); a side
            // face uses the clicked height. 1 = top, 0 = bottom.
            if (normal.y > 0) return 1;             // clicked the TOP face -> top half
            if (normal.y < 0) return 0;             // clicked the BOTTOM face -> bottom half
            return frac.y > 0.5f ? 1 : 0;           // side -> by clicked height
        case ShapeKind::VerticalSlab:
            return static_cast<uint8_t>((normal.x || normal.z) ? sideFromNormal()
                                                               : facingFromView());
        case ShapeKind::Post:
            if (normal.y) return 1;                 // top/bottom face -> Y axis
            if (normal.x) return 0;                 // X face -> X axis
            return 2;                               // Z face -> Z axis
        case ShapeKind::Stairs: {
            // Ascends the way you look; upside-down only when you hit the underside.
            const int  facing = facingFromView();
            const bool top    = normal.y < 0;
            return static_cast<uint8_t>((facing & 3) | (top ? 4 : 0));
        }
        case ShapeKind::Wall:
        case ShapeKind::Cube:
        default:
            return 0;
    }
}

// A centred unit cube (half-extent `half`) as EntityVertex geometry, textured with
// a block's six face layers and CCW-outward wound to match bakeMesh (so the shared
// EntityRenderer pipeline draws it correctly). Used for dropped items and break
// particles — both small textured cubes drawn with a per-draw model matrix.
std::vector<EntityVertex> makeCubeMesh(const BlockRegistry& reg, uint16_t id, float half) {
    struct Face { int axis; float sign; int u; int v; int face; };
    static const Face kFaces[6] = {
        {0, +1.0f, 1, 2, FacePosX}, {0, -1.0f, 2, 1, FaceNegX},
        {1, +1.0f, 2, 0, FacePosY}, {1, -1.0f, 0, 2, FaceNegY},
        {2, +1.0f, 0, 1, FacePosZ}, {2, -1.0f, 1, 0, FaceNegZ},
    };
    std::vector<EntityVertex> out;
    out.reserve(36);
    for (const Face& f : kFaces) {
        glm::vec3 n(0.0f); n[f.axis] = f.sign;
        glm::vec3 u(0.0f); u[f.u] = 1.0f;
        glm::vec3 v(0.0f); v[f.v] = 1.0f;
        const glm::vec3 fc = n * half;
        const glm::vec3 p[4] = {fc - u * half - v * half, fc + u * half - v * half,
                                fc + u * half + v * half, fc - u * half + v * half};
        const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        const uint32_t layer = reg.faceLayer(id, f.face);
        auto emit = [&](int k) { out.push_back({p[k], n, uv[k], layer}); };
        emit(0); emit(1); emit(2);
        emit(0); emit(2); emit(3);
    }
    return out;
}

// Like makeCubeMesh but every face samples one explicit layer — used for the
// block-break crack overlay (a slightly inflated cube of the crack-stage texture
// drawn over the mined block; the entity shader's alpha cutout shows only cracks).
std::vector<EntityVertex> makeCubeMeshLayer(uint32_t layer, float half) {
    struct Face { int axis; float sign; int u; int v; };
    static const Face kFaces[6] = {
        {0, +1.0f, 1, 2}, {0, -1.0f, 2, 1}, {1, +1.0f, 2, 0},
        {1, -1.0f, 0, 2}, {2, +1.0f, 0, 1}, {2, -1.0f, 1, 0},
    };
    std::vector<EntityVertex> out;
    out.reserve(36);
    for (const Face& f : kFaces) {
        glm::vec3 n(0.0f); n[f.axis] = f.sign;
        glm::vec3 u(0.0f); u[f.u] = 1.0f;
        glm::vec3 v(0.0f); v[f.v] = 1.0f;
        const glm::vec3 fc = n * half;
        const glm::vec3 p[4] = {fc - u * half - v * half, fc + u * half - v * half,
                                fc + u * half + v * half, fc - u * half + v * half};
        const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        auto emit = [&](int k) { out.push_back({p[k], n, uv[k], layer}); };
        emit(0); emit(1); emit(2);
        emit(0); emit(2); emit(3);
    }
    return out;
}

// Load the world config, but pre-apply the player's saved light falloff so the
// world is generated AND meshed with the FINAL lighting. Without this,
// applySettings() calls setLightFalloff() right after construction, and a changed
// falloff forces a redundant full relight + remeshAll() of the entire window —
// re-meshing every chunk a second time (seconds of startup) right after buildMeshes.
WorldConfig worldConfigWithSettings(const std::string& path, const Settings& s) {
    WorldConfig c = WorldConfig::load(path);
    c.skyFalloff   = std::clamp(s.skyFalloff, 1, 15);
    c.blockFalloff = std::clamp(s.blockFalloff, 1, 15);
    return c;
}
} // namespace

std::string App::settingsPath() {
    return std::string(VG_ASSET_DIR) + "/settings.yaml";
}

App::App()
    : settings_(Settings::load(settingsPath())),
      palette_(std::string(VG_ASSET_DIR) + "/colors.yaml"),
      dayNight_(std::string(VG_ASSET_DIR) + "/sky.yaml", palette_),
      window_(kWidth, kHeight, "Voxel Survival Game"),
      context_(window_),
      swapchain_(context_, window_),
      renderer_(context_, swapchain_, window_),
      clouds_(context_, std::string(VG_ASSET_DIR) + "/clouds.yaml",
              std::string(VG_ASSET_DIR) + "/clouds_noise.cache"),
      skyRenderer_(context_, renderer_.sceneRenderPass(),
                   static_cast<uint32_t>(Renderer::kMaxFramesInFlight), VG_SHADER_DIR,
                   clouds_.noise().baseView(), clouds_.noise().detailView(),
                   clouds_.weatherMap().view(), clouds_.noise().sampler()),
      world_(worldConfigWithSettings(std::string(VG_ASSET_DIR) + "/world.yaml", settings_),
             std::string(VG_ASSET_DIR) + "/blocks.yaml"),
      worldRenderer_(context_, renderer_.sceneRenderPass(),
                     static_cast<uint32_t>(Renderer::kMaxFramesInFlight), world_,
                     VG_SHADER_DIR, std::string(VG_ASSET_DIR) + "/textures"),
      entityRenderer_(context_, renderer_.sceneRenderPass(),
                      static_cast<uint32_t>(Renderer::kMaxFramesInFlight), VG_SHADER_DIR,
                      worldRenderer_.blockTextureView(), worldRenderer_.blockTextureSampler()),
      ui_(context_, renderer_.uiRenderPass(),
          static_cast<uint32_t>(Renderer::kMaxFramesInFlight),
          std::string(VG_ASSET_DIR) + "/fonts/ari/" + settings_.font, 32.0f,
          worldRenderer_.blockTextureView(), worldRenderer_.blockTextureSampler()),
      input_(window_),
      player_(glm::vec3(0.0f)),
      crafting_(std::string(VG_ASSET_DIR) + "/recipes.yaml", world_.registry()) {
    // Spawn standing on the surface at the centre of the world.
    const int cx = world_.sizeInBlocks().x / 2;
    const int cz = world_.sizeInBlocks().z / 2;
    spawnFeet_ = glm::vec3(static_cast<float>(cx),
                           static_cast<float>(world_.surfaceHeight(cx, cz)) + 2.0f,
                           static_cast<float>(cz));
    player_.teleport(spawnFeet_);

    // Test biped (ISSUES #13E): stand it a few blocks in front of the spawn camera
    // (which defaults to looking down -Z) on the surface, so it's visible the moment
    // the world loads.
    {
        const int ex = cx, ez = cz - 4;
        entityPos_ = glm::vec3(static_cast<float>(ex),
                               static_cast<float>(world_.surfaceHeight(ex, ez)) + 1.0f,
                               static_cast<float>(ez));
        buildTestEntity();
    }
    spawnCritters(); // a few passive wanderers around spawn
    player_.setInvulnerable(creativeMode_); // creative ignores fall/lava/combat damage

    // Collide against the generated world.
    player_.setSolidFn([this](int x, int y, int z) { return world_.isSolid(x, y, z); });
    // Shaped/thin blocks (slabs, stairs, posts, walls, the tree trunk) collide as
    // their box union, not a full cell — so the player stands on a slab, steps onto
    // a stair, and brushes a thin post.
    player_.setCollisionBoxesFn(
        [this](int x, int y, int z, ShapeBox out[]) {
            return world_.collisionBoxesAt(x, y, z, out);
        });
    // Swim physics + drowning: tell the player which cells are water (resolve the id
    // once; blockAt is a cheap window lookup).
    const uint16_t waterId = world_.registry().idByName("water");
    player_.setWaterFn(
        [this, waterId](int x, int y, int z) { return world_.blockAt(x, y, z).id == waterId; });

    // Start in creative with every block available (press G to switch to survival).
    // In survival, mining adds to the inventory and placing consumes the held slot;
    // seed a small starter kit there so there's something to place before you mine.
    if (creativeMode_) {
        stockCreative();
    } else {
        Inventory& inv = player_.inventory();
        const BlockRegistry& reg = world_.registry();
        auto give = [&](const char* name, int n) {
            try { inv.add(reg.idByName(name), n); } catch (const std::out_of_range&) {}
        };
        give("pickaxe", 1); // survival starts with the two tools
        give("sword", 1);
        give("dirt", 64);
        give("cobblestone", 64);
        give("planks", 32);
        give("oak_trunk", 16);
        give("oak_leaves", 16);
        give("glowstone", 8);
        give("torch", 16);
        give("chest", 3);
        give("iron_boots", 1); // only armour piece (ISSUES #15: armour trimmed to boots)
        give("swift_charm", 1);
    }
    // The hammer (block-shape tool) is available in both modes (ISSUES #16).
    {
        Inventory& inv = player_.inventory();
        try { inv.add(world_.registry().idByName("hammer"), 1); } catch (...) {}
    }

    try { chestId_ = world_.registry().idByName("chest"); } catch (...) { chestId_ = 0; }
    try { hammerId_ = world_.registry().idByName("hammer"); } catch (...) { hammerId_ = 0; }
    // Data-driven break particles (tunable in tools/particle_tool.py). A missing or
    // bad file leaves the default burst.
    try {
        breakEffect_ = ParticleEffect::load(std::string(VG_ASSET_DIR) + "/particles/break.prtcl");
    } catch (...) {}

    // Restore a saved player (position/look/health/inventory/mode) if one exists for
    // this world, overriding the default spawn + starter kit above. No-op otherwise.
    loadPlayer();
    loadChests(); // restore persisted chest contents for this world

    // Push the loaded settings to every subsystem (pixelate, lighting, FOV, …).
    applySettings();

    // Prime the cloud weather so the first frame already has valid parameters.
    clouds_.update(0.0f, dayNight_);

    window_.setCursorDisabled(true);
}

void App::applySettings() {
    renderer_.setPixelScale(static_cast<uint32_t>(std::max(1, settings_.pixelate)));
    // Light falloff lives in the world's light fields, which are baked into the
    // chunk vertex data — a change means relight + remesh everything.
    if (world_.setLightFalloff(settings_.skyFalloff, settings_.blockFalloff)) {
        worldRenderer_.remeshAll();
    }
    player_.camera().fovDegrees = settings_.fov;
    player_.setMouseSensitivity(settings_.sensitivity);
    player_.setFlySpeed(settings_.flySpeed);
    window_.setFullscreen(settings_.fullscreen);
    dayNight_.setDayLengthMinutes(settings_.dayLengthMinutes);
    dayNight_.setRunning(settings_.timeRunning);
    // Sky colour from the palette: re-tints the *daytime* zenith of the day-night
    // sky. Fall back to a known colour if the saved name is gone from colors.yaml.
    const std::string sky = palette_.has(settings_.skyColor) ? settings_.skyColor : "sky_blue";
    settings_.skyColor = sky;
    dayNight_.setDayZenithOverride(palette_.linear(sky));
    // The font is applied at construction (ui_ is built with it); changing it
    // later goes through cycleFont().
}

void App::togglePause() {
    paused_ = !paused_;
    // Free the cursor for the menu; relock it for gameplay. Reset the look delta
    // so the cursor jump doesn't spin the camera on the next frame.
    window_.setCursorDisabled(!paused_);
    input_.resetMouseDelta();
    if (!paused_) {
        settings_.save(settingsPath()); // persist any changes made in the menu
    }
}

void App::toggleInventory() {
    inventoryOpen_ = !inventoryOpen_;
    // The inventory screen needs the cursor free (to click slots) and gameplay
    // suspended, just like the menu. Reset the look delta so freeing/relocking the
    // cursor doesn't fling the camera.
    window_.setCursorDisabled(!inventoryOpen_);
    input_.resetMouseDelta();
    if (!inventoryOpen_ && !cursorStack_.empty()) {
        // Closing with an item on the cursor: tuck it back into the inventory so it
        // isn't lost (drop-on-ground comes with the items milestone).
        player_.inventory().add(cursorStack_.blockId, cursorStack_.count);
        cursorStack_.clear();
    }
}

void App::stockCreative() {
    // One full stack of every placeable block (id 1..blockCount-1), in registry
    // order, starting at slot 0 — so the hotbar holds the first nine block types and
    // the rest sit in the backpack, all reachable from the inventory screen.
    Inventory& inv = player_.inventory();
    for (int i = 0; i < Inventory::kSlots; ++i) {
        inv.slot(i).clear();
    }
    const BlockRegistry& reg = world_.registry();
    const int blocks = static_cast<int>(reg.blockCount());
    int s = 0;
    for (int id = 1; id < blocks && s < Inventory::kSlots; ++id) {
        if (!reg.placeable(static_cast<uint16_t>(id))) {
            continue; // tools/items aren't part of the creative block palette
        }
        inv.slot(s++) = ItemStack{static_cast<uint16_t>(id), Inventory::kMaxStack};
    }
}

void App::toggleGameMode() {
    creativeMode_ = !creativeMode_;
    player_.setInvulnerable(creativeMode_); // creative is invincible; survival can be hurt
    if (creativeMode_) {
        player_.setHealth(player_.maxHealth()); // leave survival -> full heal on return is fine
    }
    // Entering creative refills every block; leaving it keeps whatever you hold
    // (so a switch isn't destructive). Placing/mining behaviour is gated on the
    // flag in editBlocks().
    if (creativeMode_) {
        stockCreative();
    }
}

void App::editBlocks(const InputState& in, float dt) {
    Inventory& inv = player_.inventory();
    // Select the active hotbar slot with the number keys (1..9) or the mouse wheel.
    if (in.selectSlot >= 1 && in.selectSlot <= Inventory::kHotbarSlots) {
        inv.setSelected(in.selectSlot - 1);
    }
    if (in.hotbarScroll != 0) {
        inv.scrollSelected(in.hotbarScroll);
    }

    // Cast from the eye along the look direction to find the targeted block.
    // Targets solids AND foliage (leaves/bush), so the canopy is breakable.
    const Camera& cam = player_.camera();
    const RaycastHit hit = raycastVoxel(
        cam.position, cam.front(), kReach,
        [this](int x, int y, int z) { return world_.isTargetable(x, y, z); },
        [this](int x, int y, int z, ShapeBox out[]) { return world_.collisionBoxesAt(x, y, z, out); });

    const uint16_t heldId       = inv.selectedStack().blockId;
    const bool     holdingHammer = (hammerId_ != 0 && heldId == hammerId_);

    if (holdingHammer) {
        // ---- Hammer: left-click reshapes/rotates the targeted block (first click
        //      sets the active shape; a block already that shape advances its
        //      orientation). Right-click opens the shape radial — handled in the main
        //      loop, since it suspends gameplay. The hammer neither mines nor places.
        mineActive_     = false;
        mineProgress_   = 0.0f;
        mineProgress01_ = 0.0f;
        if (in.breakBlock && hit.hit) {
            const Block hb = world_.blockAt(hit.block.x, hit.block.y, hit.block.z);
            if (world_.registry().shapeable(hb.id)) {
                const glm::vec3 frac = hit.point - glm::floor(hit.point);
                // Default: orient the shape from where the player is looking. Hold
                // Ctrl to instead step through the orientations on each click (only
                // meaningful once the block already is the active shape).
                const bool ctrlRotate = in.ctrl && shapeKindOf(hb.metadata) == buildShape_;
                const uint8_t orient =
                    ctrlRotate
                        ? static_cast<uint8_t>((shapeOrientOf(hb.metadata) + 1) %
                                               shapeOrientCount(buildShape_))
                        : orientForPlacement(buildShape_, hit.normal, frac, cam.front());

                // Double-slab: applying a slab to the OPPOSITE half of an existing
                // slab fills it back into a full cube (bottom + top = block).
                uint8_t meta = packShape(buildShape_, orient);
                if (!ctrlRotate && buildShape_ == ShapeKind::Slab &&
                    shapeKindOf(hb.metadata) == ShapeKind::Slab &&
                    shapeOrientOf(hb.metadata) != (orient & 1)) {
                    meta = 0; // full cube
                }
                reshapeBlockAt(hit.block, hb.id, meta);
            }
        }
        return; // hammer: no mining, no placement
    } else if (in.breakHeld && hit.hit) {
        // ---- Mining: hold the left button to break, paced by the block's hardness
        //      and the held tool's speed (creative breaks instantly). Aiming away or
        //      at a new block restarts the timer, so you must dwell to break it. -----
        if (!mineActive_ || hit.block != mineBlock_) {
            mineBlock_   = hit.block;
            mineActive_  = true;
            mineProgress_ = 0.0f;
            const uint16_t tid  = world_.blockAt(hit.block.x, hit.block.y, hit.block.z).id;
            mineNeeded_ = creativeMode_ ? 0.0f : world_.registry().breakSeconds(tid, heldId);
        }
        if (mineNeeded_ >= 0.0f) { // not unbreakable
            mineProgress_ += dt;
            if (mineProgress_ >= mineNeeded_) {
                breakBlockAt(mineBlock_);
                mineActive_ = false;
                mineProgress_ = 0.0f;
            }
        }
    } else {
        mineActive_ = false;
        mineProgress_ = 0.0f;
    }
    mineProgress01_ = (mineActive_ && mineNeeded_ > 0.0f)
                          ? std::clamp(mineProgress_ / mineNeeded_, 0.0f, 1.0f)
                          : 0.0f;

    // ---- Right-click: open a chest you're looking at; otherwise place the held
    //      block onto the hit face. Placed blocks are always full cubes — shapes are
    //      applied afterwards with the hammer (left-click). ------------------------
    if (in.placeBlock && hit.hit) {
        const BlockRegistry& reg = world_.registry();
        const Block hitBlock = world_.blockAt(hit.block.x, hit.block.y, hit.block.z);

        if (chestId_ != 0 && hitBlock.id == chestId_) {
            openChestAt(hit.block);
            return;
        }

        const uint16_t placeId = heldId;
        if (placeId != 0 && reg.placeable(placeId)) {
            const glm::ivec3 t = hit.block + hit.normal;
            if (!world_.isSolid(t.x, t.y, t.z) && !player_.occupies(t.x, t.y, t.z)) {
                placeBlockAt(t, placeId); // metadata 0 = full cube
            }
        }
    }
}

void App::breakBlockAt(const glm::ivec3& b) {
    // A block edit mutates the World; first let any in-flight background relight
    // finish (it reads/writes the same chunk + light data) and enqueue its remeshes,
    // then make sure no streaming worker is mid-read before setBlock() rewrites it.
    if (relightFuture_.valid()) {
        worldRenderer_.streamRemesh(relightFuture_.get());
    }
    worldRenderer_.streamBarrier();

    const uint16_t broken = world_.blockAt(b.x, b.y, b.z).id;

    // Breaking a chest spills its contents into the player and forgets the store
    // entry; close its screen if it happened to be open.
    if (broken == chestId_ && chests_.has(b)) {
        ChestStore::Chest& chest = chests_.at(b);
        for (ItemStack& s : chest) {
            if (!s.empty()) { player_.inventory().add(s.blockId, s.count); s.clear(); }
        }
        chests_.erase(b);
        if (chestOpen_ && openChest_ == b) toggleChest();
    }

    const std::vector<glm::ivec3> dirty = world_.setBlock(b.x, b.y, b.z, Block{});
    // Harvest gating: a block below its harvest tier still breaks but drops nothing
    // (so stone/ores need the right pickaxe tier — the classic progression loop).
    const uint16_t heldTool = player_.inventory().selectedStack().blockId;
    if (broken != 0 && !creativeMode_ && world_.registry().canHarvest(broken, heldTool)) {
        // Survival: into the inventory, but if it's full the surplus becomes a
        // dropped-item entity at the block centre rather than vanishing.
        const int leftover = player_.inventory().add(broken, 1);
        if (leftover > 0) {
            droppedItems_.spawn(glm::vec3(b) + glm::vec3(0.5f),
                                ItemStack{broken, static_cast<uint16_t>(leftover)});
        }
    }
    worldRenderer_.remeshChunks(dirty);
    seedLiquid(b.x, b.y, b.z); // liquid may flow into the new gap
    // Break feedback: pop a burst of chips out of the gap. The effect can name its
    // own texture; an empty texture uses the broken block's top face.
    if (broken != 0) {
        const uint32_t layer = breakEffect_.texture.empty()
            ? world_.registry().faceLayer(broken, FacePosY)
            : world_.registry().textureLayer(breakEffect_.texture);
        particles_.spawnEffect(breakEffect_, glm::vec3(b) + glm::vec3(0.5f), layer);
    }
}

void App::placeBlockAt(const glm::ivec3& t, uint16_t id, uint8_t metadata) {
    if (relightFuture_.valid()) {
        worldRenderer_.streamRemesh(relightFuture_.get());
    }
    worldRenderer_.streamBarrier();

    const std::vector<glm::ivec3> dirty = world_.setBlock(t.x, t.y, t.z, Block{id, metadata});
    if (!creativeMode_) {
        player_.inventory().takeFromSelected(); // placing uses one up (creative is infinite)
    }
    worldRenderer_.remeshChunks(dirty);
    seedLiquid(t.x, t.y, t.z);
}

void App::reshapeBlockAt(const glm::ivec3& b, uint16_t id, uint8_t metadata) {
    if (relightFuture_.valid()) {
        worldRenderer_.streamRemesh(relightFuture_.get());
    }
    worldRenderer_.streamBarrier();
    // Keep the block id, only change its shape metadata; relight + remesh as usual.
    const std::vector<glm::ivec3> dirty = world_.setBlock(b.x, b.y, b.z, Block{id, metadata});
    worldRenderer_.remeshChunks(dirty);
}

void App::updateSurvival(float dt) {
    if (creativeMode_) {
        return; // creative: no environmental damage, no death
    }
    // Environmental damage: standing in lava hurts continuously. Scan the block
    // cells the player's AABB overlaps for lava (cheap: a handful of cells).
    const glm::vec3 eye = player_.camera().position;
    const int x0 = static_cast<int>(std::floor(eye.x - 0.3f));
    const int x1 = static_cast<int>(std::floor(eye.x + 0.3f));
    const int z0 = static_cast<int>(std::floor(eye.z - 0.3f));
    const int z1 = static_cast<int>(std::floor(eye.z + 0.3f));
    const int feetY = static_cast<int>(std::floor(eye.y - 1.62f)); // eye -> feet
    const int headY = static_cast<int>(std::floor(eye.y));
    const uint16_t lava = world_.registry().idByName("lava");
    bool inLava = false;
    for (int y = feetY; y <= headY && !inLava; ++y) {
        for (int x = x0; x <= x1 && !inLava; ++x) {
            for (int z = z0; z <= z1 && !inLava; ++z) {
                if (world_.blockAt(x, y, z).id == lava) inLava = true;
            }
        }
    }
    if (inLava) {
        player_.damage(kLavaDamagePerSec * dt);
    }

    // Death -> respawn at the world spawn with full health (player save comes later).
    if (player_.isDead()) {
        player_.teleport(spawnFeet_);
        player_.setHealth(player_.maxHealth());
    }
}

void App::savePlayer() const {
    const std::string& dir = world_.savePath();
    if (dir.empty()) {
        return; // persistence off (non-streaming world)
    }
    PlayerSave ps;
    ps.feet     = player_.feetPosition();
    ps.yaw      = player_.camera().yaw;
    ps.pitch    = player_.camera().pitch;
    ps.health   = player_.health();
    ps.selected = player_.inventory().selected();
    ps.creative = creativeMode_;
    ps.slots.reserve(Inventory::kSlots);
    for (int i = 0; i < Inventory::kSlots; ++i) {
        const ItemStack& s = player_.inventory().slot(i);
        ps.slots.emplace_back(s.blockId, s.count);
    }
    ps.equip.reserve(Equipment::kSlots);
    for (const ItemStack& s : equipment_.slots) {
        ps.equip.emplace_back(s.blockId, s.count);
    }
    const std::vector<uint8_t> bytes = ps.serialize();
    std::ofstream f(dir + "/player.dat", std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
}

bool App::loadPlayer() {
    const std::string& dir = world_.savePath();
    if (dir.empty()) return false;
    std::ifstream f(dir + "/player.dat", std::ios::binary);
    if (!f) return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    PlayerSave ps;
    if (!ps.deserialize(bytes.data(), bytes.size())) {
        return false; // missing / corrupt / wrong version: keep the default spawn
    }

    Inventory& inv = player_.inventory();
    const uint16_t blockCount = world_.registry().blockCount();
    for (int i = 0; i < Inventory::kSlots; ++i) {
        ItemStack s;
        if (i < static_cast<int>(ps.slots.size())) {
            s.blockId = ps.slots[static_cast<size_t>(i)].first;
            s.count   = ps.slots[static_cast<size_t>(i)].second;
            if (s.blockId >= blockCount) s.clear(); // a removed block id: drop it
        }
        inv.slot(i) = s;
    }

    for (int i = 0; i < Equipment::kSlots; ++i) {
        ItemStack s;
        if (i < static_cast<int>(ps.equip.size())) {
            s.blockId = ps.equip[static_cast<size_t>(i)].first;
            s.count   = ps.equip[static_cast<size_t>(i)].second;
            // Drop a removed id, or one that no longer fits this slot type.
            if (s.blockId >= blockCount || !Equipment::fits(world_.registry(), i, s.blockId)) {
                s.clear();
            }
        }
        equipment_.slots[static_cast<size_t>(i)] = s;
    }

    player_.teleport(ps.feet);
    player_.camera().yaw = ps.yaw;
    player_.camera().pitch = ps.pitch;
    player_.setHealth(ps.health);
    inv.setSelected(ps.selected);
    creativeMode_ = ps.creative;
    player_.setInvulnerable(creativeMode_);
    applyEquipmentStats();
    return true;
}

void App::applyEquipmentStats() {
    const Equipment::Stats st = equipment_.computeStats(world_.registry());
    player_.setEquipModifiers(st.armorReduction, st.speedMul, st.jumpMul, st.regenBonus);
}

void App::clickEquipSlot(int slotIndex) {
    ItemStack& s = equipment_.slots[static_cast<size_t>(slotIndex)];
    if (cursorStack_.empty()) {
        cursorStack_ = s; // take the equipped item off
        s.clear();
    } else if (Equipment::fits(world_.registry(), slotIndex, cursorStack_.blockId)) {
        std::swap(s, cursorStack_); // equip / swap a valid item in
    }
    // else: the held item doesn't fit this slot — ignore the click
    applyEquipmentStats();
}

void App::seedLiquid(int x, int y, int z) {
    if (liquidQueue_.size() > 12000) return; // hard cap: never let the queue run away
    liquidQueue_.push_back({x, y, z});
    liquidQueue_.push_back({x + 1, y, z});
    liquidQueue_.push_back({x - 1, y, z});
    liquidQueue_.push_back({x, y + 1, z});
    liquidQueue_.push_back({x, y - 1, z});
    liquidQueue_.push_back({x, y, z + 1});
    liquidQueue_.push_back({x, y, z - 1});
}

void App::tickLiquids() {
    if (liquidQueue_.empty()) {
        return;
    }
    const uint16_t water = world_.registry().idByName("water");
    const uint16_t lava  = world_.registry().idByName("lava");
    const uint16_t stone = world_.registry().idByName("stone");
    auto isLiquid = [&](uint16_t id) { return id == water || id == lava; };

    // Mutating the world: sync with the streaming workers exactly like editBlocks.
    if (relightFuture_.valid()) {
        worldRenderer_.streamRemesh(relightFuture_.get());
    }
    worldRenderer_.streamBarrier();

    // Collect this tick's fills and apply them in ONE batched relight (see
    // World::setBlocksBatch) — so the whole tick costs a single light flood, not
    // one per fill. metadata is the liquid level: 0 = full source, rising to
    // kMaxLevel at the thin spreading edge (where it stops).
    constexpr int kMaxFills = 16;   // cells filled per tick (now cheap: one relight)
    constexpr int kScan     = 4096; // cheap blockAt reads per tick (drains dead cells)
    // How far a liquid spreads from a source before the thin edge stops (each ring
    // out is +1 level). Kept small: every extra ring is more cells to fill AND a
    // larger remesh every tick while it spreads — the big puddles were frying the
    // frame rate, and a falling stream re-sources to 0 so each landing spreads this
    // far again. 3 = a ~7-wide puddle.
    constexpr int kMaxLevel = 3;
    std::vector<std::pair<glm::ivec3, Block>> edits;
    // Cells filled THIS tick must spread on the NEXT tick, not this one: the edits
    // aren't applied to the world until the batch at the end, so re-reading a
    // just-filled cell now returns air and it would be dropped (the bug that made
    // flow stop after one ring). Collect them here and append after the batch.
    std::deque<glm::ivec3> next;

    auto pending = [&](int x, int y, int z) {
        for (const auto& e : edits) {
            if (e.first.x == x && e.first.y == y && e.first.z == z) return true;
        }
        return false;
    };

    int fills = 0, scans = 0;
    while (!liquidQueue_.empty() && fills < kMaxFills && scans < kScan) {
        const glm::ivec3 c = liquidQueue_.front();
        liquidQueue_.pop_front();
        ++scans;
        const Block b = world_.blockAt(c.x, c.y, c.z);
        if (!isLiquid(b.id)) {
            continue; // emptied / changed since it was queued (cheap skip)
        }
        const uint16_t lid   = b.id;
        const int      level = b.metadata;

        // Water <-> lava contact: the lava cell turns to STONE (user: always stone,
        // no obsidian). Scan the six neighbours; convert whichever cell is lava.
        {
            const int ndx[6] = {1, -1, 0, 0, 0, 0};
            const int ndy[6] = {0, 0, 1, -1, 0, 0};
            const int ndz[6] = {0, 0, 0, 0, 1, -1};
            auto convertToStone = [&](int x, int y, int z) {
                if (pending(x, y, z)) return; // already edited this tick
                edits.push_back({glm::ivec3{x, y, z}, Block{stone, 0}});
                ++fills; // count toward the per-tick cap so a big contact spreads over ticks
            };
            bool becameStone = false;
            for (int k = 0; k < 6; ++k) {
                const int nx = c.x + ndx[k], ny = c.y + ndy[k], nz = c.z + ndz[k];
                const uint16_t nid = world_.blockAt(nx, ny, nz).id;
                if (lid == lava && nid == water) {
                    convertToStone(c.x, c.y, c.z); // this lava solidifies
                    becameStone = true;
                    break;
                }
                if (lid == water && nid == lava) {
                    convertToStone(nx, ny, nz);    // the touched lava solidifies
                }
            }
            if (becameStone) continue; // this cell is stone now — it no longer flows
        }

        // Infinite water source: a FLOWING water cell (level > 0) with >=2 horizontal
        // SOURCE-water neighbours becomes a source itself (Minecraft rule). So a 2x2
        // pool turns all-source and a hole between two sources refills permanently.
        if (lid == water && level > 0) {
            const int hx[4] = {1, -1, 0, 0};
            const int hz[4] = {0, 0, 1, -1};
            int sourceNeighbours = 0;
            for (int k = 0; k < 4; ++k) {
                const Block n = world_.blockAt(c.x + hx[k], c.y, c.z + hz[k]);
                if (n.id == water && n.metadata == 0) ++sourceNeighbours;
            }
            if (sourceNeighbours >= 2 && !pending(c.x, c.y, c.z)) {
                edits.push_back({c, Block{water, 0}}); // promote to a source
                if (next.size() < 12000) next.push_back(c); // spreads as a source next tick
                ++fills;
                continue;
            }
        }

        auto fill = [&](int x, int y, int z, int nl) {
            if (world_.blockAt(x, y, z).id != 0 || pending(x, y, z)) {
                return; // occupied, or already filled earlier this tick
            }
            edits.push_back({glm::ivec3{x, y, z}, Block{lid, static_cast<uint8_t>(nl)}});
            if (next.size() < 12000) {
                next.push_back({x, y, z}); // spreads on the NEXT tick (after the batch applies)
            }
            ++fills;
        };

        // Liquid prefers to fall; a falling stream re-sources to full (level 0) so
        // it spreads at full strength wherever it lands. On a floor it spreads
        // thinner (level+1) until it reaches the thin edge (kMaxLevel), then stops.
        if (world_.blockAt(c.x, c.y - 1, c.z).id == 0) {
            fill(c.x, c.y - 1, c.z, 0);
        } else if (level < kMaxLevel) {
            fill(c.x + 1, c.y, c.z, level + 1);
            fill(c.x - 1, c.y, c.z, level + 1);
            fill(c.x, c.y, c.z + 1, level + 1);
            fill(c.x, c.y, c.z - 1, level + 1);
            // Didn't finish all four under the fill cap? Re-queue so it resumes.
            if (fills >= kMaxFills) {
                liquidQueue_.push_back(c);
            }
        }
    }

    // Hand this tick's freshly-filled cells to the main queue now that the batch
    // below applies them to the world — they spread on the next tick.
    for (const glm::ivec3& c : next) {
        liquidQueue_.push_back(c);
    }

    if (!edits.empty()) {
        const std::vector<glm::ivec3> dirty = world_.setBlocksBatch(edits); // one relight
        // Flow remeshes go through the async streaming path (applied by streamPump +
        // recordPendingUploads, deferred-buffer retire) instead of remeshChunks. The
        // old synchronous path did a full vkDeviceWaitIdle every 0.2s tick while water
        // spread — the remaining flow-time stutter. The next tick's streamBarrier()
        // (top of this fn) keeps worker reads ordered before the next mutation.
        worldRenderer_.streamRemesh(dirty);
    }
}

void App::enableFlyOverview() {
    const glm::ivec3 size = world_.sizeInBlocks();
    player_.setMode(PlayerController::Mode::FreeFly);
    player_.teleport(glm::vec3(size.x * 0.5f - 30.0f,
                               static_cast<float>(size.y) + 18.0f,
                               size.z * 0.5f - 30.0f));
    player_.camera().yaw   = 45.0f;   // toward the centre of the map
    player_.camera().pitch = -34.0f;  // looking down
}

void App::buildTestEntity() {
    // Skin: reuse a block's texture-array layer so the rig is visibly textured
    // (per-mob skins arrive with the glTF loader, E3).
    uint32_t skin = 0;
    try { skin = world_.registry().faceLayer(world_.registry().idByName("snow"), 0); } catch (...) {}

    Skeleton& s = testEntity_;
    s.joints.clear();
    s.boxes.clear();
    const glm::quat q(1, 0, 0, 0);
    // 0 root(hip) 1 torso 2 head 3 armL 4 armR 5 legL 6 legR (parent < index).
    s.joints.push_back({"root",  -1, glm::vec3(0.0f, 0.70f, 0.0f),  q, glm::vec3(1.0f)});
    s.joints.push_back({"torso",  0, glm::vec3(0.0f, 0.0f, 0.0f),   q, glm::vec3(1.0f)});
    s.joints.push_back({"head",   1, glm::vec3(0.0f, 0.55f, 0.0f),  q, glm::vec3(1.0f)});
    s.joints.push_back({"armL",   1, glm::vec3(-0.26f, 0.50f, 0.0f), q, glm::vec3(1.0f)});
    s.joints.push_back({"armR",   1, glm::vec3(0.26f, 0.50f, 0.0f),  q, glm::vec3(1.0f)});
    s.joints.push_back({"legL",   0, glm::vec3(-0.10f, 0.0f, 0.0f),  q, glm::vec3(1.0f)});
    s.joints.push_back({"legR",   0, glm::vec3(0.10f, 0.0f, 0.0f),   q, glm::vec3(1.0f)});

    auto box = [&](int joint, glm::vec3 mn, glm::vec3 mx) {
        Box b; b.joint = joint; b.min = mn; b.max = mx; b.layer = skin;
        b.uvMin = glm::vec2(0.0f); b.uvMax = glm::vec2(1.0f);
        s.boxes.push_back(b);
    };
    box(1, glm::vec3(-0.18f, 0.0f, -0.10f),   glm::vec3(0.18f, 0.55f, 0.10f)); // torso
    box(2, glm::vec3(-0.16f, 0.0f, -0.16f),   glm::vec3(0.16f, 0.32f, 0.16f)); // head
    box(3, glm::vec3(-0.09f, -0.50f, -0.09f), glm::vec3(0.09f, 0.06f, 0.09f)); // armL
    box(4, glm::vec3(-0.09f, -0.50f, -0.09f), glm::vec3(0.09f, 0.06f, 0.09f)); // armR
    box(5, glm::vec3(-0.09f, -0.70f, -0.09f), glm::vec3(0.09f, 0.0f, 0.09f));  // legL
    box(6, glm::vec3(-0.09f, -0.70f, -0.09f), glm::vec3(0.09f, 0.0f, 0.09f));  // legR

    // Walk clip: limbs swing about X; legs and the opposite arms move in phase.
    testWalk_ = AnimationClip{};
    testWalk_.name     = "walk";
    testWalk_.duration = 1.0f;
    testWalk_.loop     = true;
    const float amp = glm::radians(28.0f);
    auto swing = [&](int joint, float dir) {
        AnimChannel ch;
        ch.joint = joint;
        ch.times = {0.0f, 0.5f, 1.0f};
        const float a = dir * amp;
        ch.rotations = {glm::angleAxis(a, glm::vec3(1, 0, 0)),
                        glm::angleAxis(-a, glm::vec3(1, 0, 0)),
                        glm::angleAxis(a, glm::vec3(1, 0, 0))};
        testWalk_.channels.push_back(ch);
    };
    swing(5, +1.0f); // legL
    swing(6, -1.0f); // legR (opposite)
    swing(3, -1.0f); // armL counter-swings legL
    swing(4, +1.0f); // armR counter-swings legR
}

void App::spawnCritters() {
    // Seed a handful of wanderers on the surface near spawn (placeholder box rig;
    // real mob models arrive with the glTF loader). Each drops onto the terrain.
    const glm::vec3 c = entityPos_;
    for (int i = 0; i < 6; ++i) {
        const int ox = static_cast<int>(c.x) + (i % 3) * 3 - 3;
        const int oz = static_cast<int>(c.z) + (i / 3) * 3 + 2;
        const float y = static_cast<float>(world_.surfaceHeight(ox, oz)) + 1.0f;
        critters_.spawn(glm::vec3(static_cast<float>(ox), y, static_cast<float>(oz)));
    }
}

void App::run(long maxFrames, const std::string& screenshotPath) {
    double lastTime = glfwGetTime();
    long frame = 0;

    // Debug hook for sky/cloud verification screenshots (headless): VG_HOUR sets
    // and freezes the time of day and aims the camera at the sun/moon; VG_PITCH /
    // VG_YAW_OFF adjust the view; VG_LOW starts at ground level instead of above
    // the world. Inert unless the environment variables are set.
    if (const char* hh = std::getenv("VG_HOUR")) {
        dayNight_.setHour(static_cast<float>(std::atof(hh)));
        if (const char* dd = std::getenv("VG_DAY")) { // moon phase / multi-day weather
            dayNight_.setDay(std::atoi(dd));
        }
        dayNight_.setRunning(false);
        clouds_.update(0.0f, dayNight_);
        const glm::ivec3 size = world_.sizeInBlocks();
        player_.setMode(PlayerController::Mode::FreeFly);
        float camY = size.y + 10.0f;
        if (std::getenv("VG_LOW")) { // ground-level view (under the cloud layer)
            camY = world_.surfaceHeight(size.x / 2, size.z / 2) + 4.0f;
        }
        player_.teleport(glm::vec3(size.x * 0.5f, camY, size.z * 0.5f));
        const DayNight::SkyState s = dayNight_.state();
        const glm::vec3 d = (s.sunDir.y >= 0.0f) ? s.sunDir : s.moonDir;
        player_.camera().yaw = glm::degrees(std::atan2(d.z, d.x));
        player_.camera().pitch = 18.0f; // up at the cloud layer
        if (const char* p = std::getenv("VG_PITCH")) {
            player_.camera().pitch = static_cast<float>(std::atof(p));
        }
        if (const char* off = std::getenv("VG_YAW_OFF")) {
            player_.camera().yaw += static_cast<float>(std::atof(off));
        }
    }

    // Debug hook (headless screenshots): VG_SHAPES_DEMO places a row of stone in
    // each shape on the surface near spawn and frames it, to verify shaped geometry
    // and 16px/block textures. Inert unless the variable is set.
    if (std::getenv("VG_SHAPES_DEMO")) {
        const glm::ivec3 size = world_.sizeInBlocks();
        const int bx = size.x / 2, bz = size.z / 2;
        const int gy = world_.surfaceHeight(bx, bz);
        const uint16_t stone = world_.registry().idByName("stone");
        struct DemoBlock { glm::ivec3 p; ShapeKind k; uint8_t o; };
        const DemoBlock demo[] = {
            {{bx + 0, gy, bz}, ShapeKind::Slab, 0},          // bottom slab
            {{bx + 1, gy, bz}, ShapeKind::Slab, 1},          // top slab
            {{bx + 2, gy, bz}, ShapeKind::Stairs, 0},        // stairs
            {{bx + 3, gy, bz}, ShapeKind::Post, 1},          // upright post
            {{bx + 4, gy, bz}, ShapeKind::Wall, 0},          // wall (connects below)
            {{bx + 4, gy, bz + 1}, ShapeKind::Wall, 0},      // ...neighbour so an arm shows
            {{bx + 5, gy, bz}, ShapeKind::VerticalSlab, 0},  // vertical slab
            {{bx + 6, gy, bz}, ShapeKind::Cube, 0},          // full cube (reference)
        };
        std::vector<glm::ivec3> dirty;
        for (const DemoBlock& d : demo) {
            auto dd = world_.setBlock(d.p.x, d.p.y, d.p.z, Block{stone, packShape(d.k, d.o)});
            dirty.insert(dirty.end(), dd.begin(), dd.end());
        }
        worldRenderer_.streamBarrier();
        worldRenderer_.remeshChunks(dirty);
        player_.setMode(PlayerController::Mode::FreeFly);
        player_.teleport(glm::vec3(bx + 3.0f, gy + 2.0f, bz + 7.0f));
        player_.camera().yaw   = -90.0f; // look toward -Z, at the row
        player_.camera().pitch = -10.0f;
    }

    while (!window_.shouldClose()) {
        window_.pollEvents();

        // Delta time, clamped so a hitch (or a slow first frame) cannot launch
        // the player through the world.
        const double now = glfwGetTime();
        const float dt = static_cast<float>(std::min(now - lastTime, 0.05));
        lastTime = now;

        const InputState in = input_.poll();

        if (in.toggleMenu) {
            // Esc closes an open chest / inventory / shape picker first, else menu.
            if (chestOpen_) {
                toggleChest();
            } else if (inventoryOpen_) {
                toggleInventory();
            } else if (shapePickerOpen_) {
                shapePickerOpen_ = false; // Esc cancels the radial (keeps the old shape)
                input_.resetMouseDelta();
            } else {
                togglePause();
            }
        }
        // E opens/closes the inventory (closes a chest if one is open instead).
        if (in.toggleInventory && !paused_) {
            if (chestOpen_) toggleChest();
            else toggleInventory();
        }
        // Hammer shape radial: hold right-click (while holding the hammer) to open;
        // the mouse slides the selector; release commits the highlighted shape. The
        // cursor stays locked — selection is driven by mouse delta in buildShapePicker.
        if (!paused_ && !inventoryOpen_ && !chestOpen_) {
            const bool holdingHammer =
                hammerId_ != 0 && player_.inventory().selectedStack().blockId == hammerId_;
            if (shapePickerOpen_) {
                if (in.placeReleased) {        // commit the highlighted shape
                    buildShape_ = shapePickerSel_;
                    shapePickerOpen_ = false;
                    input_.resetMouseDelta();
                }
            } else if (holdingHammer && in.placeBlock) {
                shapePickerOpen_ = true;       // open, starting on the current shape
                shapePickerSel_  = buildShape_;
                pickerSelPos_    = static_cast<float>(shapeIndex(buildShape_)) + 0.5f;
                input_.resetMouseDelta();
            }
        }
        // G switches creative <-> survival (during gameplay only).
        if (in.toggleGameMode && !paused_ && !inventoryOpen_ && !chestOpen_) {
            toggleGameMode();
        }
        if (in.toggleDebug) {
            debugOverlay_ = !debugOverlay_;
        }
        // F11 toggles fullscreen (and remembers the choice in settings).
        if (in.toggleFullscreen) {
            window_.setFullscreen(!window_.isFullscreen());
            settings_.fullscreen = window_.isFullscreen();
        }
        // Exponentially smoothed frame time so the overlay's FPS readout is
        // steady instead of flickering with every frame.
        smoothedDt_ += (dt - smoothedDt_) * 0.05f;
        // Gameplay only runs while no overlay (menu / inventory / chest) owns the
        // cursor.
        if (!paused_ && !inventoryOpen_ && !chestOpen_ && !shapePickerOpen_) {
            applyEquipmentStats(); // armour/trinket bonuses, current before movement
            player_.update(dt, in);
            editBlocks(in, dt);
            updateSurvival(dt);
            entityAnimTime_ += dt; // drive the test biped's walk cycle
            // Dropped-item entities: fall, magnetise to the player, walk-over pickup
            // (rendered as little spinning block cubes by the EntityRenderer pass).
            droppedItems_.update(dt, [this](int x, int y, int z) { return world_.isSolid(x, y, z); },
                                 player_.feetPosition(), player_.inventory());
            // Passive critters: wander + gravity (rendered as box-rig figures below).
            critters_.update(dt, [this](int x, int y, int z) { return world_.isSolid(x, y, z); });
            // Block-break chip particles: gravity + settle, age out.
            particles_.update(dt, [this](int x, int y, int z) { return world_.isSolid(x, y, z); });
            // Liquid flow: drain a budget of the flow queue a few times a second.
            liquidTimer_ += dt;
            if (liquidTimer_ >= 0.20f) {
                liquidTimer_ = 0.0f;
                tickLiquids();
            }
            // Stream chunks so the loaded window follows the player.
            {
                // A finished background relight (async streaming): collect its dirty
                // list and enqueue the remeshes on the main thread.
                if (relightFuture_.valid() &&
                    relightFuture_.wait_for(std::chrono::milliseconds(0)) ==
                        std::future_status::ready) {
                    worldRenderer_.streamRemesh(relightFuture_.get());
                }

                const glm::vec3 p = player_.camera().position;
                const int pcx = static_cast<int>(std::floor(p.x / Chunk::kSize));
                const int pcz = static_cast<int>(std::floor(p.z / Chunk::kSize));

                if (world_.streamAsync()) {
                    // Generate + move the window on the main thread (fast); flood the
                    // light on a background thread (the big per-boundary cost) so the
                    // frame never blocks on it. Recenter ONLY when the mesh workers are
                    // idle (and no relight is in flight), so streamBarrier() drains an
                    // already-empty pool instead of stalling the frame on the previous
                    // crossing's backlog — that wait was the streaming hitch.
                    if (!relightFuture_.valid() && worldRenderer_.streamWorkersIdle() &&
                        world_.needsRecenter(pcx, pcz)) {
                        worldRenderer_.streamBarrier(); // already idle: returns immediately
                        std::vector<glm::ivec4> boxes;
                        std::vector<glm::ivec3> dirty = world_.recenter(pcx, pcz, boxes);
                        relightFuture_ = std::async(
                            std::launch::async,
                            [this, boxes = std::move(boxes),
                             dirty = std::move(dirty)]() mutable -> std::vector<glm::ivec3> {
                                world_.relightBoxes(boxes, dirty); // heavy flood, off the main thread
                                return std::move(dirty);
                            });
                    }
                } else if (world_.needsRecenter(pcx, pcz)) {
                    // Synchronous: generate, relight, and enqueue on the main thread.
                    worldRenderer_.streamBarrier();
                    std::vector<glm::ivec4> boxes;
                    std::vector<glm::ivec3> dirty = world_.recenter(pcx, pcz, boxes);
                    world_.relightBoxes(boxes, dirty);
                    worldRenderer_.streamRemesh(dirty);
                }
            }
            // Apply a slice of streamed meshes each frame so a freshly streamed-in
            // edge melts in over several frames; a small budget keeps the per-frame
            // upload (buffer creation) under the frame budget instead of spiking.
            worldRenderer_.streamPump(12);
            dayNight_.advance(dt);     // the sun keeps moving while playing
            clouds_.update(dt, dayNight_); // weather drifts with it
        }

        // Build the UI for this frame (handles menu clicks, which may apply
        // settings that touch the GPU — safe here, between frames).
        buildUi(in);

        renderer_.drawFrame(
            [this](VkCommandBuffer cmd, uint32_t, VkExtent2D) {
                // Upload this frame's streamed chunk meshes as part of the frame's
                // own command buffer (before the render pass) — no extra GPU sync.
                worldRenderer_.recordPendingUploads(cmd);
            },
            [this](VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent) {
                const Camera& cam = player_.camera();
                const float aspect = extent.height == 0
                                         ? 1.0f
                                         : static_cast<float>(extent.width) /
                                               static_cast<float>(extent.height);
                glm::mat4 view = cam.viewMatrix();
                glm::mat4 proj = glm::perspective(glm::radians(cam.fovDegrees), aspect,
                                                  cam.nearZ, cam.farZ);
                proj[1][1] *= -1.0f; // flip Y for Vulkan's clip space

                // Sky first (no depth), then the world over it, lit by the same
                // sun/moon state so terrain shading matches the sky.
                const DayNight::SkyState sky = dayNight_.state();
                skyRenderer_.record(cmd, frameIndex, extent, view, proj, cam.position,
                                    sky, clouds_.gpuParams());
                // Clouds dim & flatten the terrain light (issue #10 D): heavy cover
                // blocks the sun/moon, so the directional light fades, the ambient
                // floor rises (shading flattens out), and the tint greys/cools
                // toward an overcast look. Cheap — driven by overhead cloud cover.
                const float cov   = clouds_.coverage();
                const float shade = cov * cov * (3.0f - 2.0f * cov); // smoothed
                const float ambient   = glm::mix(sky.ambient, 0.92f, shade * 0.55f);
                const float intensity = sky.skyIntensity * glm::mix(1.0f, 0.35f, shade);
                const float lum = glm::dot(sky.lightColor, glm::vec3(0.299f, 0.587f, 0.114f));
                const glm::vec3 lightCol =
                    glm::mix(sky.lightColor, glm::vec3(lum) * glm::vec3(0.95f, 0.98f, 1.05f),
                             shade * 0.6f);
                worldRenderer_.record(cmd, frameIndex, extent, view, proj,
                                      glm::vec4(sky.lightDir, ambient),
                                      glm::vec4(lightCol, intensity));

                // Entities into the same scene pass (shared depth + composite/fog,
                // lit like the terrain): the test biped, dropped items as little
                // spinning block cubes, and break particles as tiny block cubes.
                {
                    const LocalPose pose = sampleClip(testEntity_, testWalk_, entityAnimTime_);
                    const std::vector<EntityVertex> bipedMesh =
                        bakeMesh(testEntity_, worldMatrices(testEntity_, pose));
                    std::vector<EntityRenderer::Draw> draws{
                        {&bipedMesh, glm::translate(glm::mat4(1.0f), entityPos_)}};

                    // Passive critters: the SAME box rig, each baked at its own walk
                    // phase and placed by its position + heading. Reserve so the held
                    // meshes don't reallocate (draws hold pointers into this vector).
                    std::vector<std::vector<EntityVertex>> critterMeshes;
                    critterMeshes.reserve(critters_.size());
                    for (const Critter& cr : critters_.all()) {
                        critterMeshes.push_back(bakeMesh(
                            testEntity_,
                            worldMatrices(testEntity_,
                                          sampleClip(testEntity_, testWalk_, cr.animTime))));
                        glm::mat4 m = glm::translate(glm::mat4(1.0f), cr.pos);
                        m = glm::rotate(m, cr.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                        draws.push_back({&critterMeshes.back(), m});
                    }

                    // One cube mesh per distinct block id (reused across items +
                    // particles of that type); the map keeps the vertex data alive for
                    // the record() call below.
                    std::unordered_map<uint16_t, std::vector<EntityVertex>> cubes;
                    auto cubeFor = [&](uint16_t id) -> const std::vector<EntityVertex>* {
                        auto it = cubes.find(id);
                        if (it == cubes.end())
                            it = cubes.emplace(id, makeCubeMesh(world_.registry(), id, 0.5f)).first;
                        return &it->second;
                    };

                    // Dropped items: a small block, bobbing and spinning about Y.
                    for (const ItemEntity& it : droppedItems_.items()) {
                        if (it.stack.blockId == 0) continue;
                        const float bob = 0.09f * std::sin(it.spin * 1.5f);
                        glm::mat4 m = glm::translate(glm::mat4(1.0f),
                                                     it.pos + glm::vec3(0.0f, bob, 0.0f));
                        m = glm::rotate(m, it.spin, glm::vec3(0.0f, 1.0f, 0.0f));
                        m = glm::scale(m, glm::vec3(0.34f));
                        draws.push_back({cubeFor(it.stack.blockId), m});
                    }

                    // Break particles: flat camera-facing billboards (2D chips), all
                    // batched into one mesh in world space using the camera's right/up
                    // axes (extracted from the view matrix). Each chip shows a 1/4
                    // sub-rect of its block's top texture; lit as a top face (up normal).
                    std::vector<EntityVertex> chips;
                    if (particles_.size() > 0) {
                        const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
                        const glm::vec3 up(view[0][1], view[1][1], view[2][1]);
                        const glm::vec3 N(0.0f, 1.0f, 0.0f);
                        const float ds = 0.25f;
                        chips.reserve(particles_.size() * 6);
                        for (const Particle& p : particles_.all()) {
                            const uint32_t layer = p.layer;
                            const float h = p.size;
                            // Roll the billboard axes by the particle's spin for variety.
                            const float cs = std::cos(p.spin), sn = std::sin(p.spin);
                            const glm::vec3 r = right * cs + up * sn;
                            const glm::vec3 u = up * cs - right * sn;
                            const glm::vec3 c0 = p.pos - r * h - u * h;
                            const glm::vec3 c1 = p.pos + r * h - u * h;
                            const glm::vec3 c2 = p.pos + r * h + u * h;
                            const glm::vec3 c3 = p.pos - r * h + u * h;
                            const glm::vec2 v = p.uv0;
                            auto push = [&](const glm::vec3& pp, glm::vec2 uv) {
                                chips.push_back({pp, N, uv, layer});
                            };
                            push(c0, {v.x, v.y + ds});      push(c1, {v.x + ds, v.y + ds});
                            push(c2, {v.x + ds, v.y});      push(c0, {v.x, v.y + ds});
                            push(c2, {v.x + ds, v.y});      push(c3, {v.x, v.y});
                        }
                        if (!chips.empty()) draws.push_back({&chips, glm::mat4(1.0f)});
                    }

                    // Crack overlay: while mining, a slightly inflated cube of the
                    // crack-stage texture over the targeted block (cutout shows only
                    // the cracks). The stage tracks the break progress.
                    std::vector<EntityVertex> crackMesh; // lives until record() below
                    if (mineActive_ && mineProgress01_ > 0.0f) {
                        const BlockRegistry& reg = world_.registry();
                        const int stage = std::min(reg.crackStages() - 1,
                            static_cast<int>(mineProgress01_ * reg.crackStages()));
                        crackMesh = makeCubeMeshLayer(reg.crackLayer(stage), 0.504f);
                        draws.push_back({&crackMesh,
                            glm::translate(glm::mat4(1.0f),
                                           glm::vec3(mineBlock_) + glm::vec3(0.5f))});
                    }

                    entityRenderer_.record(cmd, frameIndex, extent, view, proj,
                                           glm::vec4(sky.lightDir, ambient),
                                           glm::vec4(lightCol, intensity), draws);
                }

                // Fog (issue #10 E): the composite pass fogs the terrain by depth.
                // Haze colour tracks the horizon sky (pale blue by day, warm at
                // dusk, dark at night); density follows the weather state + a
                // dawn/dusk ground-fog boost so foggy mornings genuinely roll in.
                const float hour = dayNight_.hour();
                const float fogW = clouds_.fogDensity();          // 0..1 weather fog
                const float dawn = glm::clamp(1.0f - std::abs(hour - 6.0f) / 3.5f, 0.0f, 1.0f);
                // Combined fog amount: near-zero on a clear day, high on a foggy
                // dawn. Ground fog must vanish when clear — a horizontal ground-
                // level ray accumulates it over a long column, so any sizeable base
                // would grey out a clear midday distance.
                const float fogAmt = glm::clamp(fogW + 0.5f * dawn * (0.3f + fogW), 0.0f, 1.0f);
                const glm::vec3 dayHaze(0.66f, 0.74f, 0.86f);
                glm::vec3 haze = glm::mix(sky.horizon, dayHaze,
                                          glm::clamp(sky.skyIntensity * 1.3f, 0.0f, 1.0f));
                haze = glm::mix(haze, sky.sunsetColor, sky.sunsetAmount * 0.5f);
                if (fogHazeTuned_) haze = fogHaze_;        // tuning panel override
                const float fogOn = fogEnabled_ ? 1.0f : 0.0f;
                CompositeRenderer::Fog fog{};
                fog.invViewProj = glm::inverse(proj * view);
                fog.camPos = glm::vec4(cam.position, 0.0f);
                fog.color  = glm::vec4(haze, fogOn * fogDistMul_ * 0.00006f * (1.0f + 4.0f * fogW));
                fog.params = glm::vec4(fogFalloff_,                         // height falloff
                                       fogOn * fogGroundMul_ * 0.0028f * fogAmt, // ground fog
                                       72.0f,                               // fog top Y
                                       fogMax_);                            // max fog
                // Underwater (issue #13A): when the camera eye sits inside a water
                // block, drown the view in blue murk via the composite pass.
                const uint16_t waterId = world_.registry().idByName("water");
                const int eyeX = static_cast<int>(std::floor(cam.position.x));
                const int eyeY = static_cast<int>(std::floor(cam.position.y));
                const int eyeZ = static_cast<int>(std::floor(cam.position.z));
                fog.submerged =
                    (world_.blockAt(eyeX, eyeY, eyeZ).id == waterId) ? 1.0f : 0.0f;
                renderer_.setFog(fog);
            },
            [this](VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent) {
                ui_.record(cmd, frameIndex, extent);
            });

        if (maxFrames >= 0 && ++frame >= maxFrames) {
            break;
        }
    }

    // Join any in-flight background relight before tearing down (its result is no
    // longer needed), then make sure the GPU is finished before destructors free.
    if (relightFuture_.valid()) {
        relightFuture_.get();
    }
    renderer_.waitIdle();

    settings_.save(settingsPath()); // persist on exit (e.g. via the Exit button)
    savePlayer();                   // persist position/health/inventory next to chunks
    saveChests();                   // persist chest contents next to chunks

    if (!screenshotPath.empty()) {
        renderer_.saveScreenshot(screenshotPath);
    }
}

// -----------------------------------------------------------------------------
//  UI
// -----------------------------------------------------------------------------

namespace {
// Charcoal / cream / lilac pixel-art theme (palette colours, sRGB).
const glm::vec4 kCharcoal {0.200f, 0.184f, 0.180f, 1.0f};
const glm::vec4 kCream    {0.965f, 0.859f, 0.769f, 1.0f};
const glm::vec4 kLilac    {0.875f, 0.620f, 0.914f, 1.0f};
const glm::vec4 kUiText   = kCream;
const glm::vec4 kPanelFill{0.200f, 0.184f, 0.180f, 0.96f}; // charcoal, near-opaque
const glm::vec4 kOverlayFill{0.200f, 0.184f, 0.180f, 0.82f};
const glm::vec4 kUiDim    {0.07f, 0.06f, 0.06f, 0.55f};    // charcoal screen dim
// Pixel-art frame metrics (match Ui.cpp's kUnit = 3). Every outline is at least
// 2 "blocks" (2 * 3 = 6 px) thick.
constexpr float kBlock       = 3.0f;       // one UI pixel-art "block"
constexpr float kFrameThin   = 2 * kBlock; // overlay / hotbar ring (>= 2 blocks)
constexpr float kFrameThick  = 3 * kBlock; // panels + selected hotbar slot
constexpr float kFrameRadius = 3 * kBlock;
} // namespace

void App::buildUi(const InputState& in) {
    const VkExtent2D ext = swapchain_.extent();
    ui_.begin(ext);
    Ui ui(ui_, in.cursor.x, in.cursor.y, in.pointerDown, in.pointerPressed);
    ui.setSprites(world_.registry().uiSprites()); // 9-patch UI sprites (ISSUES #15)

    const float W = static_cast<float>(ext.width);
    const float H = static_cast<float>(ext.height);

    // The inventory/chest screens draw their own hotbar row, so hide the HUD bar then.
    if (!inventoryOpen_ && !chestOpen_) {
        buildHotbar(ui, W, H);
    }
    if (debugOverlay_) {
        buildDebugOverlay(ui);
    }

    if (paused_) {
        ui.panel(0.0f, 0.0f, W, H, kUiDim); // dim the world behind the menu
        // Two columns centred as a pair: the options menu + the atmosphere tuner.
        const float pw = 460.0f, tw = 460.0f, gap = 24.0f, ph = 712.0f;
        const float startX = std::round((W - (pw + gap + tw)) * 0.5f);
        const float py = std::round((H - ph) * 0.5f);
        buildMenu(ui, startX, py, pw, ph);
        buildTuning(ui, startX + pw + gap, py, tw, ph);
    } else if (chestOpen_) {
        buildChest(ui, W, H, in);
    } else if (inventoryOpen_) {
        buildInventory(ui, W, H, in);
    } else if (shapePickerOpen_) {
        buildShapePicker(ui, W, H, in);
    } else {
        // Same camera the scene is drawn with, so the block wireframe lines up.
        const Camera& cam = player_.camera();
        const float aspect = H > 0.0f ? W / H : 1.0f;
        const glm::mat4 view = cam.viewMatrix();
        glm::mat4 proj = glm::perspective(glm::radians(cam.fovDegrees), aspect,
                                          cam.nearZ, cam.farZ);
        proj[1][1] *= -1.0f; // Vulkan clip-space Y flip (matches the world pass)

        buildBlockIndicator(ui, view, proj, W, H); // first, so the crosshair sits on top
        buildCrosshair(ui, W, H);
    }
}

void App::buildCrosshair(Ui& ui, float w, float h) {
    const float cx = w * 0.5f, cy = h * 0.5f;
    // A small single dot: a cream centre on a charcoal outline, so it reads on any
    // background. `rc` = cream radius; the charcoal disc is a little wider.
    const float rc = 2.5f, o = 2.0f;
    auto dot = [&](float r, const glm::vec4& col) {
        ui.roundRect(cx - r, cy - r, 2 * r, 2 * r, kBlock, col);
    };
    dot(rc + o, kCharcoal); // outline
    dot(rc, kCream);        // core

    // Mining feedback: a horizontal break meter just under the crosshair that fills
    // left-to-right as the held block breaks (a cheap stand-in until the crack-stage
    // overlay lands with the texture work). Hidden when not mining.
    if (mineProgress01_ > 0.0f) {
        const float bw = 40.0f, bh = 5.0f, by = cy + 12.0f, bx = cx - bw * 0.5f;
        ui.roundRect(bx - 1.0f, by - 1.0f, bw + 2.0f, bh + 2.0f, 2.0f, kCharcoal);
        ui.roundRect(bx, by, bw * mineProgress01_, bh, 2.0f, kCream);
    }
}

void App::buildBlockIndicator(Ui& ui, const glm::mat4& view, const glm::mat4& proj,
                              float w, float h) {
    const Camera& cam = player_.camera();
    const RaycastHit hit = raycastVoxel(
        cam.position, cam.front(), kReach,
        [this](int x, int y, int z) { return world_.isTargetable(x, y, z); },
        [this](int x, int y, int z, ShapeBox out[]) { return world_.collisionBoxesAt(x, y, z, out); });
    if (!hit.hit) {
        return; // nothing in reach
    }

    // Outline the block's ACTUAL shape — the union of its collision/render boxes
    // (vg::shapeBoxes via World::collisionBoxesAt) — so a slab/stairs/post/wall
    // highlights its real volume, matching what you collide with. Targetable
    // non-solid foliage returns no boxes, so fall back to the full cell.
    ShapeBox boxes[kMaxShapeBoxes];
    int nboxes = world_.collisionBoxesAt(hit.block.x, hit.block.y, hit.block.z, boxes);
    if (nboxes == 0) {
        boxes[0] = {glm::vec3(hit.block), glm::vec3(hit.block) + glm::vec3(1.0f)};
        nboxes = 1;
    }

    // Project a world point to HUD pixels; false if it is behind the camera.
    auto project = [&](const glm::vec3& wp, glm::vec2& out) -> bool {
        const glm::vec4 c = proj * view * glm::vec4(wp, 1.0f);
        if (c.w <= 1e-4f) return false;
        const glm::vec2 ndc = glm::vec2(c) / c.w;
        out = (ndc * 0.5f + 0.5f) * glm::vec2(w, h);
        return true;
    };

    // Each edge: its two corner indices (bit x=1,y=2,z=4) + the two faces it borders.
    struct Edge { int a, b, f0, f1; };
    static const Edge edges[12] = {
        {0, 1, 2, 4}, {2, 3, 3, 4}, {4, 5, 2, 5}, {6, 7, 3, 5}, // x edges
        {0, 2, 0, 4}, {1, 3, 1, 4}, {4, 6, 0, 5}, {5, 7, 1, 5}, // y edges
        {0, 4, 0, 2}, {1, 5, 1, 2}, {2, 6, 0, 3}, {3, 7, 1, 3}, // z edges
    };
    const float inflate = 0.006f; // nudge out a hair to avoid z-fighting the surface
    const glm::vec3 eye = cam.position;

    // Draw one box's camera-facing edges at a given thickness/colour. Only edges of
    // a box face turned toward the camera are drawn (a clean silhouette per box).
    auto drawBox = [&](const ShapeBox& bx, float thick, const glm::vec4& col) {
        const glm::vec3 mn = bx.lo - glm::vec3(inflate);
        const glm::vec3 mx = bx.hi + glm::vec3(inflate);
        glm::vec2 p[8];
        bool ok[8];
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y,
                                   (i & 4) ? mx.z : mn.z);
            ok[i] = project(corner, p[i]);
        }
        const bool front[6] = {eye.x < mn.x, eye.x > mx.x, eye.y < mn.y,
                               eye.y > mx.y, eye.z < mn.z, eye.z > mx.z};
        for (const Edge& e : edges) {
            if ((front[e.f0] || front[e.f1]) && ok[e.a] && ok[e.b]) {
                ui.line(p[e.a], p[e.b], thick, col);
            }
        }
    };
    // Halo pass over all boxes first, then the cream lines on top, so one box's
    // halo never paints over another's line.
    for (int i = 0; i < nboxes; ++i) drawBox(boxes[i], 5.0f, kCharcoal);
    for (int i = 0; i < nboxes; ++i) drawBox(boxes[i], 2.0f, kCream);
}

namespace {
// Draw one inventory slot: the charcoal+cream rounded frame (thicker cream when
// `highlight`), then the item's isometric icon and its stack count (if > 1). Shared
// by the HUD hotbar and the full inventory screen so both look identical.
void drawSlot(Ui& ui, const BlockRegistry& reg, float x, float y, float slot,
              float /*radius*/, const ItemStack& st, bool highlight) {
    ui.box(UiBox::Eq, x, y, slot, slot); // 9-patch slot background
    if (highlight) {
        ui.box(UiBox::Border, x, y, slot, slot, kLilac); // selected / hovered ring
    }
    if (st.empty()) {
        return;
    }
    const float iconR = slot * 0.5f - 11.0f; // inset to clear the slot border
    ui.isoCube(x + slot * 0.5f, y + slot * 0.5f, iconR,
               reg.faceLayer(st.blockId, FacePosY), reg.faceLayer(st.blockId, FacePosX));
    if (st.count > 1) {
        const std::string n = std::to_string(st.count);
        const float cx = x + slot - 14.0f, cy = y + slot - 18.0f;
        ui.labelCentered(cx + 1.0f, cy + 1.0f, n, 0.5f, kCharcoal); // shadow for legibility
        ui.labelCentered(cx, cy, n, 0.5f, kCream);
    }
}
} // namespace

void App::buildHotbar(Ui& ui, float w, float h) {
    const BlockRegistry& reg = world_.registry();
    const Inventory& inv = player_.inventory();
    const int count = Inventory::kHotbarSlots;
    const float slot = 60.0f, gap = 8.0f, radius = 20.0f;
    const float total = count * slot + (count - 1) * gap;
    const float x0 = (w - total) * 0.5f;
    const float y  = h - slot - 16.0f;

    // Name of the held item, centred above the bar.
    const ItemStack& sel = inv.selectedStack();
    if (!sel.empty() && sel.blockId < reg.blockCount()) {
        ui.labelCentered(w * 0.5f, y - 24.0f, reg.get(sel.blockId).name, 0.5f, kUiText);
    }

    for (int i = 0; i < count; ++i) {
        const float x = x0 + i * (slot + gap);
        drawSlot(ui, reg, x, y, slot, radius, inv.slot(i), i == inv.selected());
    }

    // Health bar (survival only — creative is invincible). A small red fill on a
    // charcoal track in the bottom-left corner, above the game-mode tag.
    if (!creativeMode_) {
        const float frac = std::clamp(player_.health() / player_.maxHealth(), 0.0f, 1.0f);
        const float bw = 120.0f, bh = 7.0f, bx = 14.0f, by = h - 44.0f;
        ui.roundRect(bx - 1.0f, by - 1.0f, bw + 2.0f, bh + 2.0f, 3.0f, kCharcoal);
        ui.roundRect(bx, by, bw, bh, 3.0f, glm::vec4(0.16f, 0.05f, 0.05f, 0.9f));
        if (frac > 0.0f) {
            ui.roundRect(bx, by, bw * frac, bh, 3.0f, glm::vec4(0.82f, 0.18f, 0.20f, 1.0f));
        }
    }

    // Game-mode tag at the bottom-left (G toggles it).
    ui.label(14.0f, h - 26.0f, creativeMode_ ? "Creative" : "Survival", 0.5f, kUiText);
}

void App::buildInventory(Ui& ui, float w, float h, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    Inventory& inv = player_.inventory();
    ui.panel(0.0f, 0.0f, w, h, kUiDim); // dim the world behind the screen

    const float S = 1.7f; // inventory screen scale — bigger, comfier slots (ISSUES #15)
    const float slot = 54.0f * S, gap = 8.0f * S, radius = 16.0f * S;
    const int   cols = Inventory::kStorageCols;
    const int   rows = Inventory::kStorageRows;
    const float gridW = cols * slot + (cols - 1) * gap;
    const float pad = 22.0f * S, titleH = 30.0f * S, hotGap = 18.0f * S;
    const float panelW = gridW + 2.0f * pad;
    const float panelH = titleH + rows * slot + (rows - 1) * gap + hotGap + slot + 2.0f * pad;
    const float px = std::round((w - panelW) * 0.5f);
    const float py = std::round((h - panelH) * 0.5f);
    ui.frame(px, py, panelW, panelH, kPanelFill, kCream, kFrameThick, kFrameRadius);
    ui.label(px + pad, py + pad - 4.0f, "Inventory", 0.6f, kUiText);

    const float gridX = px + pad;
    const float gridY = py + pad + titleH;

    // Draw one slot and, if clicked this frame, move items between it and the
    // mouse-held cursor stack (classic pick-up / drop / merge / swap).
    auto cell = [&](int index, float sx, float sy) {
        const bool hovered = ui.hovered(sx, sy, slot, slot);
        const bool sel = index < Inventory::kHotbarSlots && index == inv.selected();
        drawSlot(ui, reg, sx, sy, slot, radius, inv.slot(index), sel || hovered);
        if (hovered && in.pointerPressed) {
            clickSlot(inv.slot(index));
        }
    };

    // Backpack grid (slots 9..35).
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            cell(Inventory::kHotbarSlots + r * cols + c,
                 gridX + c * (slot + gap), gridY + r * (slot + gap));
        }
    }
    // Hotbar row (slots 0..8) below the grid.
    const float hotY = gridY + rows * (slot + gap) + hotGap;
    for (int c = 0; c < Inventory::kHotbarSlots; ++c) {
        cell(c, gridX + c * (slot + gap), hotY);
    }

    // Equipment column to the left, crafting list to the right of the inventory.
    buildEquipment(ui, px - 18.0f, py, in); // right-anchored beside the inventory
    buildCrafting(ui, px + panelW + 18.0f, py, 270.0f, in);

    // The cursor-held stack follows the mouse (icon only, no frame).
    if (!cursorStack_.empty()) {
        ui.isoCube(in.cursor.x, in.cursor.y, 22.0f,
                   reg.faceLayer(cursorStack_.blockId, FacePosY),
                   reg.faceLayer(cursorStack_.blockId, FacePosX));
        if (cursorStack_.count > 1) {
            ui.labelCentered(in.cursor.x + 16.0f, in.cursor.y + 12.0f,
                             std::to_string(cursorStack_.count), 0.5f, kCream);
        }
    }
}

void App::buildShapePicker(Ui& ui, float w, float h, const InputState& in) {
    ui.panel(0.0f, 0.0f, w, h, kUiDim); // dim the world behind the radial
    const int   n = kPickerShapeCount;
    const float cell = 96.0f, gap = 12.0f, pad = 22.0f, titleH = 34.0f, hintH = 22.0f;
    const float rowW = n * cell + (n - 1) * gap;
    const float panelW = rowW + 2.0f * pad;
    const float panelH = titleH + cell + hintH + 2.0f * pad;
    const float px = std::round((w - panelW) * 0.5f);
    const float py = std::round((h - panelH) * 0.5f);

    // The mouse slides the selector (the cursor stays locked, so we use the raw
    // horizontal delta). ~110 px of movement steps one cell. Clamp to the row.
    pickerSelPos_ += in.look.x / 110.0f;
    pickerSelPos_ = std::clamp(pickerSelPos_, 0.0f, static_cast<float>(n) - 0.001f);
    const int selIdx = static_cast<int>(pickerSelPos_);
    shapePickerSel_ = kPickerShapes[selIdx];

    ui.box(UiBox::Bg2, px, py, panelW, panelH);
    ui.label(px + pad, py + pad - 4.0f, "Build Shape", 0.6f, kUiText);

    const float rowX = px + pad, rowY = py + pad + titleH;
    for (int i = 0; i < n; ++i) {
        const float cx = rowX + i * (cell + gap);
        const bool on = (i == selIdx);
        ui.roundRect(cx, rowY, cell, cell, 10.0f, on ? kCream : kCharcoal);
        ui.labelCentered(cx + cell * 0.5f, rowY + cell * 0.5f - 6.0f,
                         shapeName(kPickerShapes[i]), 0.5f, on ? kCharcoal : kCream);
    }
    ui.labelCentered(px + panelW * 0.5f, py + panelH - pad - 4.0f,
                     "move the mouse to choose  -  release to set", 0.4f, kUiText);
}

void App::buildCrafting(Ui& ui, float x, float y, float w, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    Inventory& inv = player_.inventory();
    const std::vector<Crafting::Recipe>& recipes = crafting_.recipes();
    const int total = static_cast<int>(recipes.size());

    const float S = 1.5f; // scale rows/icons up to match the bigger inventory
    const float pad = 16.0f * S, rowH = 46.0f * S, titleH = 28.0f * S;
    const int   maxVisible = 8;
    const int   visible = total > 0 ? std::min(total, maxVisible) : 1;
    const float panelH = titleH + 2.0f * pad + visible * rowH;
    ui.box(UiBox::Bg2, x, y, w, panelH);
    ui.label(x + pad, y + pad - 4.0f, "Crafting", 0.6f, kUiText);

    if (total == 0) {
        ui.label(x + pad, y + pad + titleH + 4.0f, "(no recipes)", 0.42f, kUiText);
        return;
    }

    // Scroll with the mouse wheel (free while the inventory screen owns the cursor;
    // wheel up scrolls up). Clamp to the list bounds.
    const int maxScroll = std::max(0, total - maxVisible);
    craftScroll_ = std::clamp(craftScroll_ - in.hotbarScroll, 0, maxScroll);

    const float rx = x + pad, rw = w - 2.0f * pad, iconR = 15.0f * S;
    float ry = y + pad + titleH;
    int hoverIdx = -1;
    for (int row = 0; row < maxVisible; ++row) {
        const int idx = craftScroll_ + row;
        if (idx >= total) break;
        const Crafting::Recipe& r = recipes[static_cast<size_t>(idx)];
        const bool craftable = Crafting::canCraft(r, inv);
        const bool hov = ui.hovered(rx, ry, rw, rowH - 8.0f);

        ui.box(UiBox::Bg3, rx, ry, rw, rowH - 8.0f);
        if (craftable && hov) ui.box(UiBox::Border, rx, ry, rw, rowH - 8.0f, kLilac);
        ui.isoCube(rx + iconR + 6.0f, ry + (rowH - 8.0f) * 0.5f, iconR,
                   reg.faceLayer(r.output, FacePosY), reg.faceLayer(r.output, FacePosX));
        std::string label = r.name;
        if (r.outCount > 1) label += " x" + std::to_string(r.outCount);
        ui.label(rx + 2.0f * iconR + 14.0f, ry + 6.0f, label, 0.46f, kCream);

        if (!craftable) {
            // Darken the whole row (icon + text) so un-craftable recipes read as
            // disabled but still visible.
            ui.roundRect(rx, ry, rw, rowH - 8.0f, 8.0f, kUiDim);
        } else if (hov && in.pointerPressed) {
            Crafting::craft(r, inv);
        }
        if (hov) hoverIdx = idx;
        ry += rowH;
    }
    // Scroll affordance: a hint when the list overflows the visible window.
    if (maxScroll > 0) {
        ui.labelCentered(x + w * 0.5f, y + panelH - pad + 1.0f, "scroll for more",
                         0.34f, kUiText);
    }

    // Hover tooltip: what the recipe consumes (and whether you have enough).
    if (hoverIdx >= 0) {
        const Crafting::Recipe& r = recipes[static_cast<size_t>(hoverIdx)];
        std::string need = "Needs: ";
        for (size_t k = 0; k < r.inputs.size(); ++k) {
            const uint16_t iid = r.inputs[k].first;
            const int want = r.inputs[k].second, have = inv.count(iid);
            need += std::to_string(want) + "x " + reg.get(iid).name +
                    " (" + std::to_string(have) + ")";
            if (k + 1 < r.inputs.size()) need += ",  ";
        }
        const float tw = std::max(120.0f, 8.0f * static_cast<float>(need.size()) + 24.0f);
        const float tx = std::min(in.cursor.x + 14.0f, x + w - tw);
        const float ty = in.cursor.y + 14.0f;
        ui.frame(tx, ty, tw, 30.0f, kPanelFill, kCream, kFrameThin, 8.0f);
        ui.label(tx + 10.0f, ty + 7.0f, need, 0.4f, kUiText);
    }
}

void App::buildEquipment(Ui& ui, float rightX, float y, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    const float S = 1.7f; // match the inventory screen scale
    const float slot = 54.0f * S, gap = 8.0f * S, radius = 16.0f * S;
    const float pad = 15.0f * S, titleH = 26.0f * S, secGap = 20.0f * S, subH = 16.0f * S;
    // Boots (the lone armour slot) above the trinkets laid out in a compact 2-wide
    // grid (ISSUES #15 — trimmed armour + tighter trinket space).
    const int   cols = 2;
    const int   trinkRows = (Equipment::kTrinketSlots + cols - 1) / cols;
    const float gridW = cols * slot + (cols - 1) * gap;
    const float panelW = gridW + 2.0f * pad;
    const float panelH = titleH + slot + secGap + subH +
                         trinkRows * slot + (trinkRows - 1) * gap + 2.0f * pad;
    const float x = rightX - panelW; // right-anchored beside the inventory
    ui.box(UiBox::Bg2, x, y, panelW, panelH);
    ui.label(x + pad - 2.0f, y + pad - 4.0f, "Gear", 0.55f, kUiText);

    auto eqCell = [&](int idx, float cx, float cy) {
        const ItemStack& s = equipment_.slots[static_cast<size_t>(idx)];
        const bool hov = ui.hovered(cx, cy, slot, slot);
        drawSlot(ui, reg, cx, cy, slot, radius, s, hov);
        if (hov && in.pointerPressed) clickEquipSlot(idx);
    };
    const float sx = x + pad;
    float sy = y + pad + titleH;
    eqCell(0, sx + (gridW - slot) * 0.5f, sy); // boots, centred over the grid
    sy += slot + secGap;
    ui.label(sx, sy - subH + 2.0f, "Trinkets", 0.42f, kUiText);
    for (int i = 0; i < Equipment::kTrinketSlots; ++i) {
        const int r = i / cols, c = i % cols;
        eqCell(Equipment::kArmorSlots + i, sx + c * (slot + gap), sy + r * (slot + gap));
    }
}

void App::clickSlot(ItemStack& s) {
    if (cursorStack_.empty()) {
        cursorStack_ = s;
        s.clear();
    } else if (s.empty()) {
        s = cursorStack_;
        cursorStack_.clear();
    } else if (s.blockId == cursorStack_.blockId) {
        const int space = Inventory::kMaxStack - s.count;
        const int put   = std::min(space, static_cast<int>(cursorStack_.count));
        s.count = static_cast<uint16_t>(s.count + put);
        cursorStack_.count = static_cast<uint16_t>(cursorStack_.count - put);
        if (cursorStack_.count == 0) cursorStack_.clear();
    } else {
        std::swap(s, cursorStack_);
    }
}

void App::openChestAt(const glm::ivec3& pos) {
    openChest_ = pos;
    chests_.at(pos); // ensure an entry exists
    chestOpen_ = true;
    window_.setCursorDisabled(false); // free the cursor to click slots
    input_.resetMouseDelta();
}

void App::toggleChest() {
    chestOpen_ = false;
    window_.setCursorDisabled(true);
    input_.resetMouseDelta();
    if (!cursorStack_.empty()) {
        // Don't lose a held stack on close: tuck it back into the player inventory.
        player_.inventory().add(cursorStack_.blockId, cursorStack_.count);
        cursorStack_.clear();
    }
}

void App::buildChest(Ui& ui, float w, float h, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    Inventory& inv = player_.inventory();
    ChestStore::Chest& chest = chests_.at(openChest_);
    ui.panel(0.0f, 0.0f, w, h, kUiDim);

    const float slot = 54.0f, gap = 8.0f, radius = 16.0f;
    const int   cols = Inventory::kStorageCols;        // 9
    const int   chestRows = ChestStore::kSlots / cols; // 3
    const int   invRows = Inventory::kStorageRows;     // 3
    const float gridW = cols * slot + (cols - 1) * gap;
    const float pad = 22.0f, titleH = 30.0f, secGap = 22.0f, hotGap = 18.0f;
    const float panelW = gridW + 2.0f * pad;
    const float panelH = titleH + chestRows * slot + (chestRows - 1) * gap + secGap +
                         invRows * slot + (invRows - 1) * gap + hotGap + slot + 2.0f * pad;
    const float px = std::round((w - panelW) * 0.5f);
    const float py = std::round((h - panelH) * 0.5f);
    ui.frame(px, py, panelW, panelH, kPanelFill, kCream, kFrameThick, kFrameRadius);
    ui.label(px + pad, py + pad - 4.0f, "Chest", 0.6f, kUiText);

    const float gx = px + pad;
    float gy = py + pad + titleH;
    auto cell = [&](ItemStack& s, bool selected, float sx, float sy) {
        const bool hov = ui.hovered(sx, sy, slot, slot);
        drawSlot(ui, reg, sx, sy, slot, radius, s, selected || hov);
        if (hov && in.pointerPressed) clickSlot(s);
    };

    // Chest contents (top three rows).
    for (int r = 0; r < chestRows; ++r) {
        for (int c = 0; c < cols; ++c) {
            cell(chest[static_cast<size_t>(r * cols + c)], false,
                 gx + c * (slot + gap), gy + r * (slot + gap));
        }
    }
    gy += chestRows * (slot + gap) + secGap;

    // Player backpack (slots 9..35), then the hotbar row.
    for (int r = 0; r < invRows; ++r) {
        for (int c = 0; c < cols; ++c) {
            cell(inv.slot(Inventory::kHotbarSlots + r * cols + c), false,
                 gx + c * (slot + gap), gy + r * (slot + gap));
        }
    }
    const float hotY = gy + invRows * (slot + gap) + hotGap;
    for (int c = 0; c < Inventory::kHotbarSlots; ++c) {
        cell(inv.slot(c), c == inv.selected(), gx + c * (slot + gap), hotY);
    }

    if (!cursorStack_.empty()) {
        ui.isoCube(in.cursor.x, in.cursor.y, 22.0f,
                   reg.faceLayer(cursorStack_.blockId, FacePosY),
                   reg.faceLayer(cursorStack_.blockId, FacePosX));
        if (cursorStack_.count > 1) {
            ui.labelCentered(in.cursor.x + 16.0f, in.cursor.y + 12.0f,
                             std::to_string(cursorStack_.count), 0.5f, kCream);
        }
    }
}

void App::saveChests() const {
    const std::string& dir = world_.savePath();
    if (dir.empty()) return;
    const std::vector<uint8_t> bytes = chests_.serialize();
    std::ofstream f(dir + "/chests.dat", std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
}

void App::loadChests() {
    const std::string& dir = world_.savePath();
    if (dir.empty()) return;
    std::ifstream f(dir + "/chests.dat", std::ios::binary);
    if (!f) return;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    chests_.deserialize(bytes.data(), bytes.size());
}

// F1 info overlay: a column of small stat lines in the top-left. Read-only —
// it samples state assembled elsewhere (player, world, renderer) every frame.
void App::buildDebugOverlay(Ui& ui) {
    const Camera& cam = player_.camera();
    const glm::vec3 p = cam.position; // eye position
    const int bx = static_cast<int>(std::floor(p.x));
    const int by = static_cast<int>(std::floor(p.y));
    const int bz = static_cast<int>(std::floor(p.z));

    // Dominant horizontal axis of the view direction ("which way am I facing").
    const glm::vec3 f = cam.front();
    const char* facing = (std::abs(f.x) > std::abs(f.z)) ? (f.x > 0 ? "+X" : "-X")
                                                         : (f.z > 0 ? "+Z" : "-Z");

    // What the crosshair points at (same cast block editing uses).
    const RaycastHit hit = raycastVoxel(
        cam.position, cam.front(), kReach,
        [this](int x, int y, int z) { return world_.isTargetable(x, y, z); },
        [this](int x, int y, int z, ShapeBox out[]) { return world_.collisionBoxesAt(x, y, z, out); });

    const double elapsed = glfwGetTime();
    const int fps = static_cast<int>(std::lround(1.0f / std::max(smoothedDt_, 1e-5f)));

    char line[128];
    std::vector<std::string> lines;
    std::snprintf(line, sizeof line, "FPS: %d (%.1f ms)", fps, smoothedDt_ * 1000.0f);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "XYZ: %.2f / %.2f / %.2f", p.x, p.y, p.z);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Block: %d %d %d  Chunk: %d %d %d", bx, by, bz,
                  bx >> 4, by >> 4, bz >> 4);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Facing: %s (yaw %.1f, pitch %.1f)", facing,
                  cam.yaw, cam.pitch);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Mode: %s%s",
                  player_.mode() == PlayerController::Mode::Walking ? "walking" : "free-fly",
                  player_.onGround() ? " (on ground)" : "");
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Light here: sky %d, block %d",
                  world_.skyLightAt(bx, by, bz), world_.blockLightAt(bx, by, bz));
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Chunks drawn: %zu  Triangles: %zu",
                  worldRenderer_.drawnChunkCount(), worldRenderer_.triangleCount());
    lines.emplace_back(line);
    if (hit.hit) {
        const Block b = world_.blockAt(hit.block.x, hit.block.y, hit.block.z);
        std::snprintf(line, sizeof line, "Target: %s at %d %d %d",
                      world_.registry().get(b.id).name.c_str(), hit.block.x, hit.block.y,
                      hit.block.z);
    } else {
        std::snprintf(line, sizeof line, "Target: none");
    }
    lines.emplace_back(line);
    // No day/night cycle yet, so show the session clock (a TODO(future) hook).
    std::snprintf(line, sizeof line, "Session: %02d:%02d",
                  static_cast<int>(elapsed) / 60, static_cast<int>(elapsed) % 60);
    lines.emplace_back(line);

    // Backdrop + text. Sized to the longest line so it stays readable over any
    // terrain behind it.
    const float scale = 0.45f, lineH = 20.0f, pad = 8.0f;
    float maxW = 0.0f;
    for (const std::string& s : lines) {
        maxW = std::max(maxW, ui_.textWidth(s, scale));
    }
    ui.frame(8.0f, 8.0f, maxW + pad * 2.0f, lines.size() * lineH + pad * 2.0f,
             kOverlayFill, kCream, kFrameThin, kFrameRadius);
    float ty = 8.0f + pad;
    for (const std::string& s : lines) {
        ui.label(8.0f + pad, ty, s, scale, kUiText);
        ty += lineH;
    }
}

namespace {
// Selectable fonts (files under assets/fonts/ari/), with friendly labels.
struct FontOption { const char* file; const char* label; };
const FontOption kFonts[] = {
    {"ari-w9500.ttf", "Regular"},
    {"ari-w9500-bold.ttf", "Bold"},
    {"ari-w9500-condensed.ttf", "Condensed"},
    {"ari-w9500-condensed-bold.ttf", "Cond. Bold"},
    {"ari-w9500-display.ttf", "Display"},
    {"ari-w9500-condensed-display.ttf", "Cond. Display"},
};
std::string fontLabel(const std::string& file) {
    for (const FontOption& f : kFonts) {
        if (file == f.file) return f.label;
    }
    return file;
}
} // namespace

void App::cycleFont(int dir) {
    const int n = static_cast<int>(std::size(kFonts));
    int idx = 0;
    for (int i = 0; i < n; ++i) {
        if (settings_.font == kFonts[i].file) { idx = i; break; }
    }
    idx = ((idx + dir) % n + n) % n;
    settings_.font = kFonts[idx].file;
    ui_.setFont(std::string(VG_ASSET_DIR) + "/fonts/ari/" + settings_.font);
}

// One escape menu with every option directly on it, then Resume / Exit.
void App::buildMenu(Ui& ui, float px, float py, float pw, float ph) {
    // Charcoal panel with a thick cream pixel-art border; lilac title accent.
    ui.frame(px, py, pw, ph, kPanelFill, kCream, kFrameThick, 12.0f);
    ui.labelCentered(px + pw * 0.5f, py + 14.0f, "Menu", 0.85f, kLilac);

    const float lx = px + 24.0f, cw = pw - 48.0f;
    float y = py + 46.0f;

    // A labelled slider; returns its (possibly dragged) value. `decimals` < 0 means
    // show as an integer. Rows are compact so all options fit one panel.
    auto sliderRow = [&](const std::string& name, float value, float lo, float hi,
                         int steps, int decimals) -> float {
        char buf[48];
        if (decimals < 0) {
            std::snprintf(buf, sizeof buf, "%s: %d", name.c_str(),
                          static_cast<int>(std::lround(value)));
        } else {
            std::snprintf(buf, sizeof buf, "%s: %.*f", name.c_str(), decimals, value);
        }
        ui.label(lx, y, buf, 0.46f, kUiText);
        y += 20.0f;
        // Track is tall enough that the 2-block (6 px) border still leaves a
        // visible lilac fill bar inside; it still fits inside the row's advance.
        const float nv = ui.slider(lx, y, cw, 22.0f, value, lo, hi, steps);
        y += 30.0f;
        return nv;
    };
    auto button = [&](const std::string& text) {
        const bool clicked = ui.button(lx, y, cw, 34.0f, text);
        y += 42.0f;
        return clicked;
    };

    // Game mode (G key) + fullscreen (F11), side by side on one row.
    {
        const float bw = (cw - 8.0f) * 0.5f;
        if (ui.button(lx, y, bw, 34.0f,
                      std::string("Mode: ") + (creativeMode_ ? "Creative" : "Survival"))) {
            toggleGameMode();
        }
        if (ui.button(lx + bw + 8.0f, y, bw, 34.0f,
                      std::string("Fullscreen: ") + (window_.isFullscreen() ? "On" : "Off"))) {
            window_.setFullscreen(!window_.isFullscreen());
            settings_.fullscreen = window_.isFullscreen();
        }
        y += 42.0f;
    }

    // Pixelate 0..16 (integer).
    const int pv = static_cast<int>(std::lround(
        sliderRow("Pixelate", static_cast<float>(settings_.pixelate), 0.0f, 16.0f, 16, -1)));
    if (pv != settings_.pixelate) {
        settings_.pixelate = pv;
        renderer_.setPixelScale(static_cast<uint32_t>(std::max(1, pv)));
    }
    // Light falloff (levels lost per block of spread; higher = darker caves /
    // tighter glow). Applying a change relights the world and rebuilds every
    // chunk, so only act when the slider actually lands on a new integer.
    const int skyF = static_cast<int>(std::lround(
        sliderRow("Cave darkness", static_cast<float>(settings_.skyFalloff), 1.0f, 5.0f, 4, -1)));
    const int blkF = static_cast<int>(std::lround(
        sliderRow("Glow falloff", static_cast<float>(settings_.blockFalloff), 1.0f, 5.0f, 4, -1)));
    if (skyF != settings_.skyFalloff || blkF != settings_.blockFalloff) {
        settings_.skyFalloff   = skyF;
        settings_.blockFalloff = blkF;
        if (world_.setLightFalloff(skyF, blkF)) {
            worldRenderer_.remeshAll();
        }
    }
    // Field of view.
    const float fov = sliderRow("FOV", settings_.fov, 50.0f, 110.0f, 60, -1);
    if (std::abs(fov - settings_.fov) > 0.01f) {
        settings_.fov = fov;
        player_.camera().fovDegrees = fov;
    }
    // Mouse sensitivity.
    const float sens = sliderRow("Sensitivity", settings_.sensitivity, 0.02f, 0.30f, 0, 3);
    if (std::abs(sens - settings_.sensitivity) > 1e-4f) {
        settings_.sensitivity = sens;
        player_.setMouseSensitivity(sens);
    }
    // Flight speed.
    const float fly = sliderRow("Flight speed", settings_.flySpeed, 4.0f, 40.0f, 0, 1);
    if (std::abs(fly - settings_.flySpeed) > 0.01f) {
        settings_.flySpeed = fly;
        player_.setFlySpeed(fly);
    }
    // Time of day (live; the sun/moon and lighting follow immediately).
    const float th = sliderRow("Time (h)", dayNight_.hour(), 0.0f, 24.0f, 48, 1);
    if (std::abs(th - dayNight_.hour()) > 0.01f) {
        dayNight_.setHour(th);
    }
    // Day length: real minutes for a full in-game day.
    const float dl = sliderRow("Day length (min)", settings_.dayLengthMinutes, 1.0f, 60.0f, 59, -1);
    if (std::abs(dl - settings_.dayLengthMinutes) > 0.01f) {
        settings_.dayLengthMinutes = dl;
        dayNight_.setDayLengthMinutes(dl);
    }
    // Freeze / resume the day-night cycle.
    if (button(std::string("Time: ") + (settings_.timeRunning ? "Running" : "Paused"))) {
        settings_.timeRunning = !settings_.timeRunning;
        dayNight_.setRunning(settings_.timeRunning);
    }
    // Day-sky colour (cycles the palette; tints the daytime zenith).
    if (button("Sky: " + settings_.skyColor)) {
        const std::vector<std::string>& names = palette_.names();
        if (!names.empty()) {
            int idx = 0;
            for (size_t i = 0; i < names.size(); ++i) {
                if (names[i] == settings_.skyColor) { idx = static_cast<int>(i); break; }
            }
            settings_.skyColor = names[(idx + 1) % names.size()];
            dayNight_.setDayZenithOverride(palette_.linear(settings_.skyColor));
        }
    }
    // Font (cycles the ari family).
    if (button("Font: " + fontLabel(settings_.font))) {
        cycleFont(+1);
    }

    if (button("Resume")) {
        togglePause();
    }
    if (button("Exit")) {
        window_.requestClose();
    }
}

void App::buildTuning(Ui& ui, float px, float py, float pw, float ph) {
    ui.frame(px, py, pw, ph, kPanelFill, kCream, kFrameThick, 12.0f);
    ui.labelCentered(px + pw * 0.5f, py + 14.0f, "Tune Sky", 0.85f, kLilac);

    const float lx = px + 24.0f, cw = pw - 48.0f;
    float y = py + 44.0f;

    // Tab row: Weather / Clouds / Fog / Sky.
    const char* tabs[4] = {"Weather", "Clouds", "Fog", "Sky"};
    const float tabGap = 6.0f, tabW = (cw - 3.0f * tabGap) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        if (ui.button(lx + i * (tabW + tabGap), y, tabW, 30.0f, tabs[i])) {
            tuningTab_ = i;
        }
    }
    y += 38.0f;
    ui.labelCentered(px + pw * 0.5f, y, tabs[tuningTab_], 0.5f, kLilac);
    y += 26.0f;

    // Shared row helpers (compact, like the menu's).
    auto sliderRow = [&](const std::string& name, float value, float lo, float hi,
                         int steps, int decimals) -> float {
        char buf[56];
        if (decimals < 0) {
            std::snprintf(buf, sizeof buf, "%s: %d", name.c_str(),
                          static_cast<int>(std::lround(value)));
        } else {
            std::snprintf(buf, sizeof buf, "%s: %.*f", name.c_str(), decimals, value);
        }
        ui.label(lx, y, buf, 0.44f, kUiText);
        y += 19.0f;
        const float nv = ui.slider(lx, y, cw, 20.0f, value, lo, hi, steps);
        y += 27.0f;
        return nv;
    };
    auto button = [&](const std::string& text) {
        const bool clicked = ui.button(lx, y, cw, 32.0f, text);
        y += 40.0f;
        return clicked;
    };
    auto changed = [](float a, float b) { return std::abs(a - b) > 1e-4f; };

    if (tuningTab_ == 0) { // --- Weather: force a state + coverage/type --------
        ui.label(lx, y, "Force weather state:", 0.44f, kUiText);
        y += 22.0f;
        const char* st[6] = {"Clear", "Fair", "Broken", "Overcast", "Stormy", "Foggy"};
        const float halfW = (cw - 8.0f) * 0.5f;
        for (int i = 0; i < 6; ++i) {
            const float bx = lx + ((i & 1) ? halfW + 8.0f : 0.0f);
            if (ui.button(bx, y, halfW, 30.0f, st[i])) {
                clouds_.setForcedState(i);
            }
            if (i & 1) y += 36.0f;
        }
        y += 4.0f;
        if (button("Auto weather")) {
            clouds_.setForcedState(-1);
            clouds_.setForceCoverage(-1.0f);
            clouds_.setForceType(-1.0f);
        }
        // Autonomous-scheduler speed knobs (apply in Auto mode).
        const float ci = sliderRow("Change every (s)", clouds_.changeInterval(),
                                   15.0f, 600.0f, 0, -1);
        if (changed(ci, clouds_.changeInterval())) clouds_.setChangeInterval(ci);
        const float fd = sliderRow("Front sweep (s)", clouds_.frontDuration(),
                                   3.0f, 120.0f, 0, -1);
        if (changed(fd, clouds_.frontDuration())) clouds_.setFrontDuration(fd);
        if (button("Change weather now")) clouds_.triggerWeatherChange();
        const float cov = sliderRow("Coverage", clouds_.coverage(), 0.0f, 1.0f, 0, 2);
        if (changed(cov, clouds_.coverage())) clouds_.setForceCoverage(cov);
        const float ty = sliderRow("Cloud type", clouds_.type(), 0.0f, 1.0f, 0, 2);
        if (changed(ty, clouds_.type())) clouds_.setForceType(ty);
        const int fs = clouds_.forcedState();
        ui.label(lx, y, fs < 0 ? "Active: auto" : (std::string("Active: ") + st[fs]),
                 0.42f, kLilac);
    } else if (tuningTab_ == 1) { // --- Clouds: shape + light --------------------
        const float d = sliderRow("Density", clouds_.densityScale(), 0.0f, 2.0f, 0, 2);
        if (changed(d, clouds_.densityScale())) clouds_.setDensityScale(d);
        const float e = sliderRow("Erosion (lacy)", clouds_.erosion(), 0.0f, 1.0f, 0, 2);
        if (changed(e, clouds_.erosion())) clouds_.setErosion(e);
        const float vx = sliderRow("Blockiness", clouds_.voxelize(), 0.0f, 12.0f, 0, 1);
        if (changed(vx, clouds_.voxelize())) clouds_.setVoxelize(vx);
        const float ex = sliderRow("Extinction", clouds_.extinction(), 0.2f, 3.0f, 0, 2);
        if (changed(ex, clouds_.extinction())) clouds_.setExtinction(ex);
        const float ws = sliderRow("Wind speed", clouds_.windSpeed(), 0.0f, 30.0f, 0, 1);
        if (changed(ws, clouds_.windSpeed())) clouds_.setWindSpeed(ws);
    } else if (tuningTab_ == 2) { // --- Fog ------------------------------------
        if (button(std::string("Fog: ") + (fogEnabled_ ? "On" : "Off"))) {
            fogEnabled_ = !fogEnabled_;
        }
        fogDistMul_   = sliderRow("Distance haze", fogDistMul_, 0.0f, 5.0f, 0, 2);
        fogGroundMul_ = sliderRow("Ground fog", fogGroundMul_, 0.0f, 8.0f, 0, 2);
        fogFalloff_   = sliderRow("Height falloff", fogFalloff_, 0.005f, 0.20f, 0, 3);
        fogMax_       = sliderRow("Max fog", fogMax_, 0.10f, 1.0f, 0, 2);
    } else { // --- Sky: moon, stars, ozone + RGB colour editor ----------------
        const float md = std::fmod(static_cast<float>(dayNight_.totalDays()), 29.53f);
        const float nmd = sliderRow("Moon day (phase)", md, 0.0f, 29.0f, 29, -1);
        if (changed(nmd, md)) dayNight_.setDay(static_cast<int>(std::lround(nmd)));
        const float oz = sliderRow("Ozone (twilight)", dayNight_.ozoneStrength(), 0.0f, 3.0f, 0, 2);
        if (changed(oz, dayNight_.ozoneStrength())) dayNight_.setOzoneStrength(oz);
        const float sb = sliderRow("Stars", dayNight_.starBrightness(), 0.0f, 2.0f, 0, 2);
        if (changed(sb, dayNight_.starBrightness())) dayNight_.setStarBrightness(sb);
        const float mw = sliderRow("Milky Way", dayNight_.milkyWay(), 0.0f, 1.5f, 0, 2);
        if (changed(mw, dayNight_.milkyWay())) dayNight_.setMilkyWay(mw);

        // RGB colour editor: a target selector + 3 channel sliders (linear space).
        const char* ct[5] = {"Sunset High", "Sunset Mid", "Sunset Horizon",
                             "Cloud Dusk", "Fog Haze"};
        if (button(std::string("Colour: ") + ct[colorTarget_])) {
            colorTarget_ = (colorTarget_ + 1) % 5;
        }
        glm::vec3 hi = dayNight_.sunsetHigh(), mid = dayNight_.sunsetMid();
        glm::vec3 hor = dayNight_.sunsetHorizon(), dusk = dayNight_.cloudDusk();
        glm::vec3 cur = colorTarget_ == 0 ? hi
                      : colorTarget_ == 1 ? mid
                      : colorTarget_ == 2 ? hor
                      : colorTarget_ == 3 ? dusk
                                          : fogHaze_;
        const float r = sliderRow("R", cur.r, 0.0f, 1.5f, 0, 2);
        const float g = sliderRow("G", cur.g, 0.0f, 1.5f, 0, 2);
        const float b = sliderRow("B", cur.b, 0.0f, 1.5f, 0, 2);
        // Live swatch of the colour being mixed (linear -> approx sRGB so it
        // reads on the swapchain), inside a cream frame.
        const glm::vec3 disp = glm::pow(glm::clamp(glm::vec3(r, g, b), 0.0f, 1.0f),
                                        glm::vec3(1.0f / 2.2f));
        ui.roundRect(lx - 2.0f, y - 2.0f, cw + 4.0f, 24.0f, 5.0f, kCream);
        ui.roundRect(lx + 2.0f, y + 2.0f, cw - 4.0f, 16.0f, 3.0f, glm::vec4(disp, 1.0f));
        y += 28.0f;
        if (changed(r, cur.r) || changed(g, cur.g) || changed(b, cur.b)) {
            const glm::vec3 nc(r, g, b);
            if (colorTarget_ == 4) {
                fogHaze_ = nc;
                fogHazeTuned_ = true;
            } else {
                if (colorTarget_ == 0) hi = nc;
                else if (colorTarget_ == 1) mid = nc;
                else if (colorTarget_ == 2) hor = nc;
                else dusk = nc;
                dayNight_.setTunedSunset(hi, mid, hor, dusk);
            }
        }
        if (button("Auto colours")) {
            dayNight_.clearTunedSunset();
            fogHazeTuned_ = false;
        }
    }
}

} // namespace vg
