#include "core/App.h"

#include "core/ColorPalette.h"
#include "core/ShapePicker.h"
#include "core/Ui.h"
#include "player/PlayerSave.h"
#include "world/Raycast.h"

#include <glm/gtc/matrix_transform.hpp>
#include <yaml-cpp/yaml.h>
#include <stb_image.h> // declarations only; the implementation lives in TextureArray.cpp

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
// Discover Blockbench models: every assets/models/<name>/ subdir that contains a
// <name>.bbmodel. Sorted by name so the skin-atlas order is stable and shared by
// the entitySkins_ texture array and buildModels() (layer index == position here).
// Adding a model is just dropping a new <name>/<name>.bbmodel + <name>.png folder.
std::vector<std::string> discoverModelNames(const std::string& modelsDir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(modelsDir, ec)) {
        if (!e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        if (std::filesystem::exists(e.path() / (name + ".bbmodel"))) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}
// Edge length of the model skin atlas. Source skins of ANY size are resized to this
// (preserving their [0,1] UV layout), so models needn't share a texture resolution.
constexpr int kModelAtlasSize = 256;

// Build the skin atlas (layer i == discoverModelNames()[i]): for each model, load the
// texture its faces reference (any name, from the model's own dir; falls back to
// <name>.png), nearest-resize it to kModelAtlasSize, and return the RGBA layers. This
// is what removes the "same size / named <name>.png" rules — drop any .bbmodel + the
// texture it points at, any size, and it works.
std::vector<std::vector<unsigned char>> buildModelAtlas(const std::string& modelsDir) {
    const int size = kModelAtlasSize;
    const size_t bytes = static_cast<size_t>(size) * size * 4;
    std::vector<std::vector<unsigned char>> layers;
    for (const std::string& n : discoverModelNames(modelsDir)) {
        const std::string dir = modelsDir + "/" + n + "/";
        BlockbenchModel bb;
        try { bb = loadBlockbenchModel(dir + n + ".bbmodel"); } catch (...) {}
        int w = 0, h = 0, ch = 0;
        stbi_uc* data = nullptr;
        // Prefer an embedded (base64) skin — Blockbench's native save format — then an
        // external file the model references, then the <name>.png convention.
        if (!bb.embeddedPNG.empty())
            data = stbi_load_from_memory(bb.embeddedPNG.data(),
                                         static_cast<int>(bb.embeddedPNG.size()),
                                         &w, &h, &ch, STBI_rgb_alpha);
        if (!data && !bb.skin.empty())
            data = stbi_load((dir + bb.skin).c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!data) data = stbi_load((dir + n + ".png").c_str(), &w, &h, &ch, STBI_rgb_alpha);
        std::vector<unsigned char> layer(bytes, 0); // missing texture -> transparent layer
        if (data && w > 0 && h > 0) {
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const int sx = x * w / size, sy = y * h / size; // nearest sample
                    const unsigned char* p = data + (static_cast<size_t>(sy) * w + sx) * 4;
                    unsigned char* d = layer.data() + (static_cast<size_t>(y) * size + x) * 4;
                    d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3];
                }
            }
            stbi_image_free(data);
        }
        layers.push_back(std::move(layer));
    }
    // The atlas needs >=1 layer (an empty TextureArray throws); if there are no
    // models, hand back a single transparent layer so the game still starts.
    if (layers.empty()) layers.emplace_back(bytes, 0);
    return layers;
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
    // Render distance is player-driven (Settings), overriding world.yaml's
    // view_radius. Recompute the derived chunk-grid dims load() set from the YAML
    // value so the world is allocated at the chosen radius.
    c.viewRadius = std::clamp(s.renderDistance, 4, 16);
    c.chunksX = c.chunksZ = 2 * c.viewRadius + 1;
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
      entitySkins_(context_, buildModelAtlas(std::string(VG_ASSET_DIR) + "/models"),
                   kModelAtlasSize, kModelAtlasSize),
      entityRenderer_(context_, renderer_.sceneRenderPass(),
                      static_cast<uint32_t>(Renderer::kMaxFramesInFlight), VG_SHADER_DIR,
                      worldRenderer_.blockTextureView(), worldRenderer_.blockTextureSampler(),
                      entitySkins_.view(), entitySkins_.sampler()),
      ui_(context_, renderer_.uiRenderPass(),
          static_cast<uint32_t>(Renderer::kMaxFramesInFlight),
          std::string(VG_ASSET_DIR) + "/fonts/ari/" + settings_.font, 32.0f,
          worldRenderer_.blockTextureView(), worldRenderer_.blockTextureSampler()),
      input_(window_),
      player_(glm::vec3(0.0f)),
      crafting_(std::string(VG_ASSET_DIR) + "/recipes.yaml", world_.registry()) {
    // Flat world: every column is identical, so spawn at the world origin on top of
    // the grass surface (surfaceHeight scans the actual voxels — grass tops out at 16).
    int cx = 0, cz = 0;
    spawnFeet_ = glm::vec3(static_cast<float>(cx),
                           static_cast<float>(world_.surfaceHeight(cx, cz)) + 2.0f,
                           static_cast<float>(cz));
    // Recentre the streamed window onto the spawn so the eastern coast is loaded from
    // frame 0. A jump this large takes recenter()'s synchronous full-regen path (it
    // also recomputes sky/block light); we then rebuild the window's meshes (melt-in).
    // streamBarrier first so no mesh worker reads the world mid-regen (invariant #1).
    if (world_.streaming()) {
        constexpr int N = Chunk::kSize;
        const int scx = (cx >= 0 ? cx : cx - (N - 1)) / N;
        const int scz = (cz >= 0 ? cz : cz - (N - 1)) / N;
        worldRenderer_.streamBarrier();
        std::vector<glm::ivec4> boxes;
        world_.recenter(scx, scz, boxes);
        worldRenderer_.remeshAll();
    }
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
        // Load the Blockbench tool models (hammer/sword/pickaxe/torch) for the held
        // viewmodel. Not spawned in the world — drawn in-hand when selected.
        buildModels();
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
    // Swim physics + drowning: the flat world has no water, so nothing is ever "in
    // water" (water was removed with the worldgen overhaul). Keep the predicate so
    // the player API stays wired; it simply always reports dry.
    player_.setWaterFn([](int, int, int) { return false; });

    // Start in creative with every block available (press G to switch to survival).
    // In survival, mining adds to the inventory and placing consumes the held slot;
    // seed a small starter kit there so there's something to place before you mine.
    if (creativeMode_) {
        stockCreative(); // places the tools in slots 0-2, then the block palette
    } else {
        Inventory& inv = player_.inventory();
        const BlockRegistry& reg = world_.registry();
        // First three hotbar slots are the tools (pickaxe/sword/hammer); these are
        // also what the first-person viewmodel shows when selected. The block starter
        // kit then fills from slot 3 onward (add() skips the occupied tool slots).
        int t = 0;
        for (const char* tool : {"pickaxe", "sword", "hammer"}) {
            try { inv.slot(t++) = ItemStack{reg.idByName(tool), 1}; } catch (...) {}
        }
        auto give = [&](const char* name, int n) {
            try { inv.add(reg.idByName(name), n); } catch (const std::out_of_range&) {}
        };
        give("dirt", 64);
        give("cobblestone", 64);
        give("planks", 32);
        give("oak_trunk", 16);
        give("oak_leaves", 16);
        give("torch", 16);
        give("chest", 3);
        give("iron_boots", 1); // only armour piece (ISSUES #15: armour trimmed to boots)
        give("swift_charm", 1);
    }

    try { chestId_ = world_.registry().idByName("chest"); } catch (...) { chestId_ = 0; }
    try { hammerId_ = world_.registry().idByName("hammer"); } catch (...) { hammerId_ = 0; }
    // Data-driven break particles (tunable in tools/particle_tool.py). A missing or
    // bad file leaves the default burst.
    try {
        breakEffect_ = ParticleEffect::load(std::string(VG_ASSET_DIR) + "/particles/break.prtcl");
    } catch (...) {}
    try {
        placeEffect_ = ParticleEffect::load(std::string(VG_ASSET_DIR) + "/particles/place.prtcl");
    } catch (...) {}
    try {
        splashEffect_ = ParticleEffect::load(std::string(VG_ASSET_DIR) + "/particles/splash.prtcl");
    } catch (...) {}
    try {
        emberEffect_ = ParticleEffect::load(std::string(VG_ASSET_DIR) + "/particles/ember.prtcl");
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
    // chunk vertex data — a change means relight + remesh everything. At startup the
    // values are pre-applied via worldConfigWithSettings, so this normally returns
    // false (no work) and the drain below is a no-op (no relight/workers yet); the
    // drain matters only if applySettings is ever reached with a falloff change while
    // streaming is live (REVIEW R1).
    drainBeforeWorldMutation();
    if (world_.setLightFalloff(settings_.skyFalloff, settings_.blockFalloff)) {
        worldRenderer_.remeshAll();
    }
    player_.camera().fovDegrees = settings_.fov;
    player_.setMouseSensitivity(settings_.sensitivity);
    player_.setFlySpeed(settings_.flySpeed);
    player_.setViewBob(settings_.viewBob);
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
    applyRetroPalette();
}

void App::applyRetroPalette() {
    // Empty name = palette off (the per-channel "Colour bits" quantiser runs
    // instead). Otherwise load assets/colorpalettes/<name>.hex and bind it; a
    // missing/garbage file loads as no colours, which the composite reads as off.
    std::vector<glm::vec3> colors;
    if (!settings_.retroPalette.empty()) {
        colors = loadColorPalette(std::string(VG_ASSET_DIR) + "/colorpalettes/" +
                                  settings_.retroPalette + ".hex");
    }
    renderer_.setRetroPalette(colors);
}

void App::togglePause() {
    paused_ = !paused_;
    // Free the cursor for the menu; relock it for gameplay. Reset the look delta
    // so the cursor jump doesn't spin the camera on the next frame.
    window_.setCursorDisabled(!paused_);
    input_.resetMouseDelta();
    if (!paused_) {
        palettePickerOpen_ = false;      // don't reopen the popup on next pause
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
    // First three hotbar slots are the tools (pickaxe/sword/hammer), then one full
    // stack of every placeable block in registry order — so the hotbar reads
    // tool, tool, tool, block, block… and the overflow sits in the backpack.
    Inventory& inv = player_.inventory();
    for (int i = 0; i < Inventory::kSlots; ++i) {
        inv.slot(i).clear();
    }
    const BlockRegistry& reg = world_.registry();
    int s = 0;
    for (const char* tool : {"pickaxe", "sword", "hammer"}) {
        try { inv.slot(s++) = ItemStack{reg.idByName(tool), 1}; } catch (...) {}
    }
    const int blocks = static_cast<int>(reg.blockCount());
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

void App::drainBeforeWorldMutation() {
    // Let any in-flight background relight finish (it reads/writes the same chunk +
    // light data) and enqueue its remeshes, then make sure no streaming worker is
    // mid-read before the caller mutates the World.
    if (relightFuture_.valid()) {
        worldRenderer_.streamRemesh(relightFuture_.get());
    }
    worldRenderer_.streamBarrier();
}

bool App::tryPrepareWorldMutation() {
    // Like drainBeforeWorldMutation, but never blocks on the relight (REVIEW R3). A
    // finished relight is collected; an in-flight one makes us bail so the caller can
    // defer/skip. The worker barrier is fine to take — it only drains the mesh pool,
    // which is idle between window steps.
    if (relightFuture_.valid()) {
        if (relightFuture_.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
            return false; // edge relight still flooding — don't stall the frame
        }
        worldRenderer_.streamRemesh(relightFuture_.get());
    }
    worldRenderer_.streamBarrier();
    return true;
}

void App::enqueueEdit(const PendingEdit& e) {
    for (const PendingEdit& p : pendingEdits_) {
        if (p.kind == e.kind && p.pos == e.pos) return; // already queued this cell+action
    }
    pendingEdits_.push_back(e);
}

void App::flushPendingEdits() {
    if (pendingEdits_.empty()) {
        return;
    }
    // Wait until the relight that forced the deferral has finished — applying mid-
    // relight is exactly the race we deferred to avoid. tryPrepareWorldMutation
    // (called by the re-dispatched edits) collects it.
    if (relightFuture_.valid() &&
        relightFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    // Swap out first: the re-dispatched *BlockAt calls below run their normal path
    // (and must not see the queue they are draining).
    std::deque<PendingEdit> q;
    q.swap(pendingEdits_);
    for (const PendingEdit& e : q) {
        switch (e.kind) {
            case PendingEdit::Break:   breakBlockAt(e.pos); break;
            case PendingEdit::Place:   placeBlockAt(e.pos, e.id, e.metadata); break;
            case PendingEdit::Reshape: reshapeBlockAt(e.pos, e.id, e.metadata); break;
        }
    }
}

void App::breakBlockAt(const glm::ivec3& b) {
    if (!tryPrepareWorldMutation()) {
        enqueueEdit({PendingEdit::Break, b, 0, 0}); // apply when the relight finishes
        return;
    }

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
    // Remesh the dirtied chunks on the async streaming path (workers, or the
    // budgeted no-worker queue) rather than meshing them inline — the synchronous
    // greedyMesh + buffer build was the remaining main-thread cost of an edit. The
    // result installs via the per-frame streamPump (same path liquid flow uses); the
    // next mutation's streamBarrier orders worker reads before it. ~1 frame later.
    worldRenderer_.streamRemesh(dirty);
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
    if (!tryPrepareWorldMutation()) {
        enqueueEdit({PendingEdit::Place, t, id, metadata});
        return;
    }

    const std::vector<glm::ivec3> dirty = world_.setBlock(t.x, t.y, t.z, Block{id, metadata});
    if (!creativeMode_) {
        player_.inventory().takeFromSelected(); // placing uses one up (creative is infinite)
    }
    worldRenderer_.streamRemesh(dirty); // async remesh, like breakBlockAt
    // Place poof: a soft dust puff in the placed block's own colour (ISSUES #13M).
    const uint32_t layer = placeEffect_.texture.empty()
        ? world_.registry().faceLayer(id, FacePosY)
        : world_.registry().textureLayer(placeEffect_.texture);
    particles_.spawnEffect(placeEffect_, glm::vec3(t) + glm::vec3(0.5f), layer);
}

void App::reshapeBlockAt(const glm::ivec3& b, uint16_t id, uint8_t metadata) {
    if (!tryPrepareWorldMutation()) {
        enqueueEdit({PendingEdit::Reshape, b, id, metadata});
        return;
    }
    // Keep the block id, only change its shape metadata; relight + remesh as usual.
    const std::vector<glm::ivec3> dirty = world_.setBlock(b.x, b.y, b.z, Block{id, metadata});
    worldRenderer_.streamRemesh(dirty); // async remesh, like breakBlockAt
}

void App::updateSurvival(float dt) {
    if (creativeMode_) {
        return; // creative: no environmental damage, no death
    }
    // (Lava environmental damage was removed with the worldgen overhaul — the flat
    // world has no lava.)

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


void App::streamWindow() {
    // Advance the streamed world one step toward the player each frame. This is the
    // most invariant-laden code in the project (REVIEW R9 pulled it out of the frame
    // loop so the rules live in one place); the four-actor safety model it upholds:
    //
    //   1. Mesh workers only READ the world; the main thread (here) is the sole
    //      mutator and must streamBarrier() (drain workers) before any mutation.
    //   2. The background relight writes only the edge light slab; the window must
    //      not be mutated while it runs, so every step joins relightFuture_ first.
    //   3. The background pregen reads only the immutable generator + save files, so
    //      it may overlap the relight — but NOT a window move (gated below).
    //   4. Worker mesh results are version-stamped, so a discarded in-flight result
    //      after a forced barrier is always safe.
    //
    // A finished background relight: collect its dirty list and enqueue the remeshes
    // on the main thread (the renderer's per-slot version state stays single-threaded).
    if (relightFuture_.valid() &&
        relightFuture_.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
        worldRenderer_.streamRemesh(relightFuture_.get());
    }

    const glm::vec3 p = player_.camera().position;
    const int pcx = static_cast<int>(std::floor(p.x / Chunk::kSize));
    const int pcz = static_cast<int>(std::floor(p.z / Chunk::kSize));

    if (world_.streamAsync()) {
        // Streaming pipeline with every heavy stage off the main thread:
        //   pregen (background noise) -> apply strip (cheap chunk swap, main thread)
        //   -> relight (background flood) -> remesh (worker pool). One column per
        //   cycle. The window may only be mutated when no relight is in flight AND the
        //   mesh workers are idle, so streamBarrier() drains an already-empty pool.
        //   The strip generation used to run synchronously inside recenter() — that
        //   was the ~90ms per-boundary frame spike.
        const glm::ivec3 org = world_.chunkOrigin();
        const glm::ivec3 cnt = world_.chunkCounts();
        const int viewR   = (cnt.x - 1) / 2;
        const int targetX = pcx - viewR, targetZ = pcz - viewR;
        auto launchRelight = [this](std::vector<glm::ivec4>&& boxes,
                                    std::vector<glm::ivec3>&& dirty) {
            relightFuture_ = std::async(
                std::launch::async,
                [this, boxes = std::move(boxes),
                 dirty = std::move(dirty)]() mutable -> std::vector<glm::ivec3> {
                    world_.relightBoxes(boxes, dirty); // heavy flood, off-thread
                    return std::move(dirty);
                });
        };
        // Force a window step when the busy-gate has postponed it for too long
        // (REVIEW R5): drain the relight + workers (a bounded wait) rather than let
        // the window fall ever further behind the player.
        auto forceDrainForStep = [this] {
            if (relightFuture_.valid()) {
                worldRenderer_.streamRemesh(relightFuture_.get());
            }
            worldRenderer_.streamBarrier();
        };
        // Reap finished stale strips (a turn abandoned their axis/dir). Destroying a
        // ready/moved-from future never blocks; not-ready ones stay to drain next frame.
        pregenRetired_.erase(
            std::remove_if(pregenRetired_.begin(), pregenRetired_.end(),
                           [](std::future<World::PregenStrip>& f) {
                               return !f.valid() ||
                                      f.wait_for(std::chrono::milliseconds(0)) ==
                                          std::future_status::ready;
                           }),
            pregenRetired_.end());
        // Abandon the staged strips WITHOUT joining on the main thread (a join would be
        // the per-turn hitch): move them to the drain. Proven safe to keep running
        // through the coming window move — see pregenRetired_'s declaration.
        auto retireQueue = [this] {
            for (auto& f : pregenQueue_) {
                if (f.valid()) pregenRetired_.push_back(std::move(f));
            }
            pregenQueue_.clear();
        };
        const bool need = world_.needsRecenter(pcx, pcz);

        // The first column step recenter() would take toward the player (X before Z,
        // matching recenter()/recenterWithStrip()).
        int  stepDir    = 0;
        bool stepAlongX = true;
        if (need) {
            if (targetX != org.x) { stepAlongX = true;  stepDir = targetX > org.x ? 1 : -1; }
            else                  { stepAlongX = false; stepDir = targetZ > org.z ? 1 : -1; }
        }

        const bool teleport = need && (std::abs(targetX - org.x) >= cnt.x ||
                                       std::abs(targetZ - org.z) >= cnt.z);

        // The window may have fallen behind the player (the gate stays closed while a
        // relight/mesh of the previous column is still in flight). We let it trail
        // freely — EXCEPT when the player is within kWindowEdgeSafetyChunks of the
        // leading edge: past that they would leave the loaded window. `lag` is how many
        // columns the origin still has to advance to recenter; lag == viewR means the
        // player has reached the edge. Force a (blocking) load when that close — rare,
        // only when out-running generation. This replaces the old per-frame starve
        // counter that fired during normal play and caused the ~115ms streaming hitch.
        const int lag = std::max(std::abs(targetX - org.x), std::abs(targetZ - org.z));
        const bool forceLoad = need && lag >= std::max(1, viewR - kWindowEdgeSafetyChunks);

        if (teleport) {
            // Nothing in the window is reusable — take the synchronous full-regen path
            // (rare, inherently a load). recenter() regenerates EVERY column (touching
            // every save file), so unlike a one-column step it cannot overlap a pregen:
            // join all in-flight strips before it (blocking is fine on this rare path).
            for (auto& f : pregenQueue_)   { if (f.valid()) f.get(); }
            for (auto& f : pregenRetired_) { if (f.valid()) f.get(); }
            pregenQueue_.clear();
            pregenRetired_.clear();
            pregenDir_ = 0;
            // A teleport / huge jump is a genuine load (nothing in the window is
            // reusable): drain and regenerate the whole window now.
            forceDrainForStep();
            std::vector<glm::ivec4> boxes;
            std::vector<glm::ivec3> dirty = world_.recenter(pcx, pcz, boxes);
            launchRelight(std::move(boxes), std::move(dirty));
        } else {
            // A turn invalidates the staged strips (they step along the old axis/dir):
            // drop them and rebuild for the new direction.
            if (need && (stepDir != pregenDir_ || stepAlongX != pregenAlongX_)) {
                retireQueue();
                pregenDir_    = stepDir;
                pregenAlongX_ = stepAlongX;
            }

            // Apply the front strip — one column per frame, as before. The queue keeps
            // the NEXT column already generating, so a fast crossing finds it ready
            // instead of waiting on per-column pregen.
            if (need && !pregenQueue_.empty() &&
                pregenQueue_.front().wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::ready) {
                const bool gateOpen =
                    !relightFuture_.valid() && worldRenderer_.streamWorkersIdle();
                // Step when the gate is open (no relight/mesh in flight — the common
                // case, no frame cost) OR when the window has fallen to the edge and a
                // load is forced. Otherwise wait: the frame is never blocked here.
                if (gateOpen || forceLoad) {
                    // VG_STREAM_TIME: isolate the synchronous main-thread cost of a
                    // window step (worker drain + strip apply) — the heavy relight +
                    // remesh are off-thread/budget-spread, so this is what a boundary
                    // actually costs the frame. Inert unless the env var is set.
                    static const bool kStreamTime = std::getenv("VG_STREAM_TIME") != nullptr;
                    const auto t0 = kStreamTime ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                    World::PregenStrip strip = pregenQueue_.front().get();
                    pregenQueue_.pop_front();
                    forceDrainForStep(); // idle path: returns immediately
                    const auto t1 = kStreamTime ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                    std::vector<glm::ivec4> boxes;
                    std::vector<glm::ivec3> dirty =
                        world_.recenterWithStrip(pcx, pcz, std::move(strip), boxes);
                    // A strip that no longer matches the needed step (a turn between
                    // queueing and applying) moves nothing and returns empty — drop the
                    // rest of the now-stale queue so it rebuilds for the real direction.
                    if (dirty.empty() && world_.needsRecenter(pcx, pcz)) {
                        retireQueue();
                        pregenDir_ = 0;
                    } else if (!boxes.empty()) {
                        launchRelight(std::move(boxes), std::move(dirty));
                    }
                    if (kStreamTime) {
                        const auto t2 = std::chrono::steady_clock::now();
                        auto ms = [](auto a, auto b) {
                            return std::chrono::duration<double, std::milli>(b - a).count();
                        };
                        std::printf("[stream] step %s: drain %.2fms apply %.2fms total %.2fms\n",
                                    gateOpen ? "gate" : "LOAD", ms(t0, t1), ms(t1, t2),
                                    ms(t0, t2));
                    }
                }
            } else if (forceLoad) {
                // forceLoad guarantees a step when the player reaches the window edge,
                // but the cheap path above also requires a READY pregen strip. When the
                // player has out-run pregen (queue empty or front still generating) the
                // strip path would skip and the player could leave the loaded window
                // (collision/edits then query air). Fall back to a blocking synchronous
                // catch-up — recenter() steps column-by-column all the way to the player,
                // regenerating entering columns inline. Rare and a genuine load, like the
                // teleport path; safe because forceDrainForStep() drains relight+workers
                // before the mutation. (Also the escape hatch for sustained worker-busy
                // starvation, e.g. a large ongoing liquid flow holding the gate closed.)
                retireQueue();
                pregenDir_ = 0;
                forceDrainForStep();
                std::vector<glm::ivec4> boxes;
                std::vector<glm::ivec3> dirty = world_.recenter(pcx, pcz, boxes);
                launchRelight(std::move(boxes), std::move(dirty));
            }

            // Replenish up to kPregenAhead along the current travel axis. Re-read the
            // origin: an apply above advanced it, so the front (k=0) steps from the new
            // origin and the k-th staged strip steps from origin + k*step. Safe
            // alongside a running relight — pregen reads no window state.
            if (need && pregenDir_ != 0) {
                const glm::ivec3 curOrg = world_.chunkOrigin();
                while (static_cast<int>(pregenQueue_.size()) < kPregenAhead) {
                    const int k     = static_cast<int>(pregenQueue_.size());
                    const int fromX = pregenAlongX_ ? curOrg.x + k * pregenDir_ : curOrg.x;
                    const int fromZ = pregenAlongX_ ? curOrg.z : curOrg.z + k * pregenDir_;
                    pregenQueue_.push_back(std::async(
                        std::launch::async,
                        [this, dir = pregenDir_, ax = pregenAlongX_, fromX, fromZ] {
                            return world_.pregenStrip(dir, ax, fromX, fromZ);
                        }));
                }
            }

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

void App::buildModels() {
    // Load each Blockbench tool model and bake its static (rest-pose) mesh once. Its
    // boxes carry per-face UVs; we stamp each model's boxes with its skin-atlas slice
    // (layer index MUST match the entitySkins_ filename list) so the EntityRenderer
    // samples the right PNG with useSkin=1. A missing/bad model is non-fatal — that
    // tool is just skipped.
    // Discovery order == the entitySkins_ texture order, so model i's boxes sample
    // skin-atlas layer i. "hand" is the first-person arm drawn alongside any tool.
    const std::string base = std::string(VG_ASSET_DIR) + "/models/";
    const std::vector<std::string> names = discoverModelNames(base);
    toolModels_.clear();
    std::unordered_map<std::string, size_t> byName;
    for (uint32_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        try {
            BlockbenchModel m = loadBlockbenchModel(base + n + "/" + n + ".bbmodel");
            for (Box& b : m.skeleton.boxes) b.layer = i; // skin-atlas layer (matches entitySkins_)
            std::vector<EntityVertex> mesh =
                bakeMesh(m.skeleton, worldMatrices(m.skeleton, restPose(m.skeleton)));
            // AABB of the baked rest pose, so a world-dropped item can be centred on
            // its position and scaled to a uniform size (held draw ignores these).
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (const EntityVertex& v : mesh) { lo = glm::min(lo, v.pos); hi = glm::max(hi, v.pos); }
            glm::vec3 center = mesh.empty() ? glm::vec3(0.0f) : 0.5f * (lo + hi);
            glm::vec3 size = mesh.empty() ? glm::vec3(1.0f) : (hi - lo);
            float span = std::max(size.x, std::max(size.y, size.z));
            // The "critter" model is a mob rig: keep its loaded Skeleton (its leg
            // groups became joints) so the wandering critters animate, rather than a
            // static held/dropped bake. Centre it on the entity pos: xz-centre at the
            // origin, feet (AABB bottom) at y=0 (the critter's ground position).
            if (n == "critter" && !m.skeleton.joints.empty()) {
                critterRig_    = m.skeleton; // boxes already carry the skin layer (stamped above)
                critterOffset_ = glm::vec3(-center.x, -lo.y, -center.z);
                critterWalk_ = AnimationClip{};
                critterWalk_.name = "walk"; critterWalk_.duration = 0.8f; critterWalk_.loop = true;
                auto swingLeg = [&](const char* jn, float dir) {
                    const int ji = critterRig_.find(jn);
                    if (ji < 0) return;
                    AnimChannel ch; ch.joint = ji; ch.times = {0.0f, 0.4f, 0.8f};
                    const float a = dir * glm::radians(26.0f);
                    ch.rotations = {glm::angleAxis(a, glm::vec3(1, 0, 0)),
                                    glm::angleAxis(-a, glm::vec3(1, 0, 0)),
                                    glm::angleAxis(a, glm::vec3(1, 0, 0))};
                    critterWalk_.channels.push_back(ch);
                };
                swingLeg("legFL", +1.0f); swingLeg("legBR", +1.0f); // diagonal gait
                swingLeg("legFR", -1.0f); swingLeg("legBL", -1.0f);
                hasCritterModel_ = !critterWalk_.channels.empty();
            }
            byName[n] = toolModels_.size();
            toolModels_.push_back({std::move(mesh), n, center, span > 1e-4f ? span : 1.0f});
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[models] %s load failed: %s\n", n.c_str(), e.what());
        }
    }

    // Held viewmodel binding: a model whose name matches an item is drawn in-hand
    // when that item is selected (so dropping assets/models/<item>/ "just works");
    // all pickaxe tiers share the one pickaxe model. The "hand" model is the arm.
    heldModelByItem_.clear();
    const BlockRegistry& reg = world_.registry();
    auto bind = [&](const std::string& item, const std::string& model) {
        auto mi = byName.find(model);
        if (mi == byName.end()) return;
        try { heldModelByItem_[reg.idByName(item)] = mi->second; } catch (...) {}
    };
    for (const auto& kv : byName) bind(kv.first, kv.first); // model name == item name
    for (const char* p : {"wood_pickaxe", "stone_pickaxe"})
        bind(p, "pickaxe"); // tiers share the pickaxe model

    // The first-person arm (drawn alongside any held tool, sharing its transform).
    auto hi = byName.find("hand");
    handModel_ = hi != byName.end() ? static_cast<int>(hi->second) : -1;
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

    // Gated frame profiler (VG_FRAME_TIME=1, inert otherwise): averages where each
    // frame goes — game update (input/physics/streaming), UI build, and the draw
    // split from Renderer::phaseTimes() — and prints once every 120 frames. `wait`
    // dominating means GPU-bound; `record` dominating means CPU-bound recording.
    const bool profFrames = std::getenv("VG_FRAME_TIME") != nullptr;
    double pUpdate = 0, pUi = 0, pWait = 0, pAcq = 0, pRec = 0, pSub = 0, pTotal = 0;
    double pMax = 0, pMaxUpdate = 0;
    long   pN = 0;

    // Debug hook (streaming perf): VG_AUTOWALK=<speed> flies the player along +X at
    // that many blocks/sec, crossing a chunk boundary every 16/speed seconds — the
    // way to measure recenter/streaming frame spikes headlessly. Inert unless set.
    float autoWalk = 0.0f;
    if (const char* aw = std::getenv("VG_AUTOWALK")) {
        autoWalk = static_cast<float>(std::atof(aw));
        player_.setMode(PlayerController::Mode::FreeFly);
        // Glide above the terrain so the path never tunnels through a mountain.
        glm::vec3 p = player_.feetPosition();
        p.y = static_cast<float>(world_.sizeInBlocks().y) * 0.8f;
        player_.teleport(p);
    }

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

    // Debug hook (headless screenshots): VG_MODEL_DEMO selects a hotbar tool so the
    // first-person held viewmodel is visible; VG_HELD=<item> picks which (default
    // sword). Stays in normal first-person play so the in-hand placement is exact.
    if (std::getenv("VG_MODEL_DEMO")) {
        player_.setMode(PlayerController::Mode::Walking);
        const char* want = std::getenv("VG_HELD");
        uint16_t id = 0;
        try { id = world_.registry().idByName(want ? want : "sword"); } catch (...) {}
        Inventory& inv = player_.inventory();
        if (id != 0) {
            int slot = -1; // find it in the hotbar, else drop it into slot 0
            for (int i = 0; i < Inventory::kHotbarSlots; ++i)
                if (inv.slot(i).blockId == id) { slot = i; break; }
            if (slot < 0) { inv.slot(0) = ItemStack{id, 1}; slot = 0; }
            inv.setSelected(slot);
        }
        // Aim at the open sky for a clean backdrop (the viewmodel is camera-locked, so
        // its on-screen placement is unaffected by where the camera looks).
        player_.camera().pitch = 40.0f;
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
        // Barrier BEFORE the setBlock loop: startup meshing now runs on the worker
        // pool, so the world must not be mutated while workers are mid-read.
        worldRenderer_.streamBarrier();
        std::vector<glm::ivec3> dirty;
        for (const DemoBlock& d : demo) {
            auto dd = world_.setBlock(d.p.x, d.p.y, d.p.z, Block{stone, packShape(d.k, d.o)});
            dirty.insert(dirty.end(), dd.begin(), dd.end());
        }
        worldRenderer_.remeshChunks(dirty);
        player_.setMode(PlayerController::Mode::FreeFly);
        player_.teleport(glm::vec3(bx + 3.0f, gy + 2.0f, bz + 7.0f));
        player_.camera().yaw   = -90.0f; // look toward -Z, at the row
        player_.camera().pitch = -10.0f;
    }

    // Debug hook (headless screenshots): VG_WATER_DEMO carves a pool whose floor
    // ramps from 1 block deep to ~16 deep and fills it with source water, to verify
    // depth darkening (shallow = bright, deep = dark). Inert unless set.
    if (std::getenv("VG_WATER_DEMO")) {
        const glm::ivec3 size = world_.sizeInBlocks();
        const int bx = size.x / 2, bz = size.z / 2;
        const int gy = world_.surfaceHeight(bx, bz);
        uint16_t water = 0, floor = 0;
        try { water = world_.registry().idByName("water"); } catch (...) {}
        try { floor = world_.registry().idByName("snow"); } catch (...) {} // bright floor for contrast
        std::vector<glm::ivec3> dirty;
        auto put = [&](int x, int y, int z, uint16_t id) {
            auto dd = world_.setBlock(x, y, z, Block{id, 0});
            dirty.insert(dirty.end(), dd.begin(), dd.end());
        };
        // An elevated stepped pool in clear air: a constant water TOP over a bright
        // floor that ramps from 1 block deep (near) to W deep (far). Seen from above,
        // the water TOP face is tinted by the column depth, so shallow reads bright
        // (floor shows through) and deep reads dark — the depth gradient at a glance.
        const int W = 16, D = 12;
        // Keep the whole structure under the world ceiling (high-terrain seeds put gy
        // near the top, so an unclamped lift would write out of bounds).
        const int topY  = std::min(gy + 40, world_.sizeInBlocks().y - 2);
        const int baseY = topY - W - 1;
        // Barrier BEFORE the puts: startup meshing now runs on the worker pool, so
        // the world must not be mutated while workers are mid-read.
        worldRenderer_.streamBarrier();
        for (int i = 0; i < W; ++i) {              // column i is (i+1) blocks deep
            const int floorY = topY - 1 - i;
            for (int z = 0; z < D; ++z) {
                for (int y = baseY; y < floorY; ++y) put(bx + i, y, bz + z, floor); // solid base
                put(bx + i, floorY, bz + z, floor);                                 // pool floor
                for (int y = floorY + 1; y <= topY; ++y) put(bx + i, y, bz + z, water);
                put(bx + i, topY + 1, bz + z, 0);                                    // clear above
            }
        }
        worldRenderer_.remeshChunks(dirty);
        player_.setMode(PlayerController::Mode::FreeFly);
        player_.teleport(glm::vec3(bx + W * 0.5f - 0.5f, topY + 15.0f, bz + D * 0.5f - 0.5f));
        player_.camera().yaw   = -90.0f;
        player_.camera().pitch = -82.0f;  // near-straight-down onto the water surface
    }

    // Debug hook (headless screenshots): VG_MENU opens the paused escape menu at
    // startup (optionally on a given tab, VG_MENU_TAB=0..3) so the options layout can
    // be captured headlessly. Inert unless set.
    if (const char* m = std::getenv("VG_MENU")) {
        if (const char* t = std::getenv("VG_MENU_TAB")) {
            menuTab_ = std::max(0, std::min(4, std::atoi(t)));
        }
        if (std::atoi(m) != 0 && !paused_) togglePause();
    }
    // VG_PALETTE_DEMO opens the Esc menu straight into the retro colour-palette
    // picker popup, so its swatch-strip preview can be captured headlessly.
    if (std::getenv("VG_PALETTE_DEMO")) {
        menuTab_ = 4; // Retro
        if (!paused_) togglePause();
        refreshPaletteCache();
        palettePickerOpen_ = true;
    }

    // Debug hook (headless screenshots): VG_DROP_DEMO spawns one of each tool as a
    // dropped ItemEntity in a row near spawn and frames them, to verify dropped
    // items render as their Blockbench model (not a block cube). Inert unless set.
    if (std::getenv("VG_DROP_DEMO")) {
        const glm::ivec3 size = world_.sizeInBlocks();
        const int bx = size.x / 2, bz = size.z / 2;
        const int gy = world_.surfaceHeight(bx, bz);
        const float sky = gy + 30.0f; // lift into clear sky for an unoccluded shot
        const char* tools[] = {"pickaxe", "sword", "hammer", "torch"};
        for (int i = 0; i < 4; ++i) {
            uint16_t id = 0;
            try { id = world_.registry().idByName(tools[i]); } catch (...) {}
            if (id != 0)
                droppedItems_.spawn(glm::vec3(bx + i * 1.2f, sky, bz), ItemStack{id, 1});
        }
        player_.setMode(PlayerController::Mode::FreeFly);
        player_.teleport(glm::vec3(bx + 1.8f, sky + 0.2f, bz + 4.5f));
        player_.camera().yaw   = -90.0f; // look toward -Z, at the row
        player_.camera().pitch = 0.0f;
    }

    // Debug hook (headless screenshots): VG_CRITTER_DEMO spawns a few critters in a
    // row lifted into clear sky and frames them, to verify the .bbmodel mob rig (and
    // its walk pose). Inert unless set.
    if (std::getenv("VG_CRITTER_DEMO")) {
        const glm::ivec3 size = world_.sizeInBlocks();
        const int bx = size.x / 2, bz = size.z / 2;
        const int gy = world_.surfaceHeight(bx, bz);
        const float sky = gy + 24.0f;
        for (int i = 0; i < 3; ++i) {
            critters_.spawn(glm::vec3(bx + i * 1.6f, sky, bz));
        }
        player_.setMode(PlayerController::Mode::FreeFly);
        player_.teleport(glm::vec3(bx + 1.6f, sky + 0.35f, bz + 3.4f));
        player_.camera().yaw   = -90.0f; // look toward -Z, at the row
        player_.camera().pitch = -6.0f;
    }

    while (!window_.shouldClose()) {
        const auto profT0 = std::chrono::steady_clock::now();
        window_.pollEvents();

        // Delta time, clamped so a hitch (or a slow first frame) cannot launch
        // the player through the world.
        const double now = glfwGetTime();
        const float dt = static_cast<float>(std::min(now - lastTime, 0.05));
        lastTime = now;

        const InputState in = input_.poll();

        if (autoWalk != 0.0f && !paused_) {
            // VG_AUTOWALK: constant glide along +X (streaming-spike measurement).
            glm::vec3 p = player_.feetPosition();
            p.x += autoWalk * dt;
            player_.teleport(p);
        }

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
        // Q throws the selected hotbar stack out in front of the player (a little
        // outward + upward pop); the pickup delay keeps it from instantly re-collecting.
        if (in.drop && !paused_ && !inventoryOpen_ && !chestOpen_ && !shapePickerOpen_) {
            ItemStack& sel = player_.inventory().slot(player_.inventory().selected());
            if (!sel.empty()) {
                const Camera& cam = player_.camera();
                const glm::vec3 dir = cam.front();
                droppedItems_.spawn(cam.position + dir * 0.8f, sel,
                                    dir * 6.0f + glm::vec3(0.0f, 2.0f, 0.0f));
                sel.clear(); // threw the whole held stack
            }
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
            flushPendingEdits(); // re-apply any edits deferred during a relight (R3)
            editBlocks(in, dt);
            updateSurvival(dt);
            // Juice (ISSUES #13M): a splash when the player drops into water, and a
            // screen shake when they take damage (fall / lava / future combat). The
            // shake decays fast; the view jitter is applied in the scene record below.
            const bool nowInWater = player_.inWater();
            if (nowInWater && !wasInWater_) {
                uint32_t wlayer = 0;
                try { wlayer = world_.registry().textureLayer(splashEffect_.texture); } catch (...) {}
                particles_.spawnEffect(splashEffect_,
                                       player_.feetPosition() + glm::vec3(0.0f, 0.3f, 0.0f), wlayer);
            }
            wasInWater_ = nowInWater;
            const float hp = player_.health();
            if (hp < prevHealth_ - 0.01f) {
                const float dmg = prevHealth_ - hp;
                shakeMag_ = std::min(0.30f, shakeMag_ + dmg * 0.012f);
                if (damageNumbers_.size() < 24) // cheap cap
                    damageNumbers_.push_back({player_.feetPosition() + glm::vec3(0.0f, 1.7f, 0.0f),
                                              dmg, 0.0f});
            }
            prevHealth_ = hp;
            shakeMag_ *= std::exp(-dt * 9.0f); // settle quickly
            // Float the damage numbers up and age them out.
            for (FloatText& d : damageNumbers_) { d.age += dt; d.pos.y += dt * 0.8f; }
            damageNumbers_.erase(
                std::remove_if(damageNumbers_.begin(), damageNumbers_.end(),
                               [](const FloatText& d) { return d.age > 1.1f; }),
                damageNumbers_.end());
            entityAnimTime_ += dt; // drive the test biped's walk cycle
            // Dropped-item entities: fall, magnetise to the player, walk-over pickup
            // (rendered as little spinning block cubes by the EntityRenderer pass).
            droppedItems_.update(dt, [this](int x, int y, int z) { return world_.isSolid(x, y, z); },
                                 player_.feetPosition(), player_.inventory());
            // Passive critters: wander + gravity (rendered as box-rig figures below).
            critters_.update(dt, [this](int x, int y, int z) { return world_.isSolid(x, y, z); });
            // Block-break chip particles: gravity + settle, age out.
            particles_.update(dt, [this](int x, int y, int z) { return world_.isSolid(x, y, z); });
            // VG_UPDATE_TIME: attribute per-frame update() spikes to the streaming
            // sub-phases (window step / mesh upload pump / far-LOD rebuild). Prints any
            // call over 4ms so an occasional frame stretch can be pinned to its source.
            // Inert unless the env var is set.
            static const bool kUpdateTime = std::getenv("VG_UPDATE_TIME") != nullptr;
            auto timed = [](const char* tag, auto&& fn) {
                if (!kUpdateTime) { fn(); return; }
                const auto a = std::chrono::steady_clock::now();
                fn();
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - a).count();
                if (ms > 4.0) std::printf("[update] %s %.2fms\n", tag, ms);
            };
            // Stream chunks so the loaded window follows the player (the pregen /
            // relight futures + busy-gate; invariants stated on App::streamWindow).
            timed("streamWindow", [&] { streamWindow(); });
            // Apply a slice of streamed meshes each frame so a freshly streamed-in
            // edge melts in over several frames; a small budget keeps the per-frame
            // upload (buffer creation) under the frame budget instead of spiking.
            // Budget is a world.yaml stream_tuning knob (REVIEW R7).
            timed("streamPump", [&] { worldRenderer_.streamPump(world_.config().streamPumpBudget); });
            dayNight_.advance(dt);     // the sun keeps moving while playing
            clouds_.update(dt, dayNight_); // weather drifts with it
        }

        const auto profT1 = std::chrono::steady_clock::now();
        // Build the UI for this frame (handles menu clicks, which may apply
        // settings that touch the GPU — safe here, between frames).
        buildUi(in);
        const auto profT2 = std::chrono::steady_clock::now();

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
                // Camera shake (ISSUES #13M juice): a tiny decaying view jitter when
                // the player was hurt. Per-frame pseudo-random offset scaled by shakeMag_.
                if (shakeMag_ > 0.001f) {
                    const float a = entityAnimTime_ * 131.0f;
                    const glm::vec3 j(std::sin(a), std::cos(a * 1.7f), std::sin(a * 0.7f));
                    view = glm::translate(glm::mat4(1.0f), j * (shakeMag_ * 0.05f)) * view;
                }
                // The depth-based fog uses this same proj so it stays consistent.
                const float farPlane = cam.farZ;
                glm::mat4 proj = glm::perspective(glm::radians(cam.fovDegrees), aspect,
                                                  cam.nearZ, farPlane);
                // Reversed-Z: remap clip depth [0,1] -> [1,0] (near->1, far->0) so the
                // float depth buffer's dense precision near 0 lands at the FAR distance
                // — the correct distribution for the huge near:far ratio the LOD shell
                // needs (otherwise distant geometry z-fights). Pipelines clear depth to
                // 0 and test GREATER; the composite fog + sky use this same proj, and
                // Gribb-Hartmann frustum culling is unaffected (same clip volume).
                glm::mat4 rev(1.0f);
                rev[2][2] = -1.0f;
                rev[3][2] =  1.0f;
                proj = rev * proj;
                proj[1][1] *= -1.0f; // flip Y for Vulkan's clip space

                // Affine texture warp (independent retro FX): fed to the chunk pass
                // via the UBO's misc.z. 0/off leaves the geometry unchanged.
                const float retroAffine = settings_.retroAffine ? 1.0f : 0.0f;
                worldRenderer_.setRetro(retroAffine);

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
                // Horizon haze colour, shared by the far-terrain edge fade and the
                // composite distance fog: the sky's horizon warmed toward day haze,
                // then toward the sunset colour at dusk.
                const glm::vec3 dayHaze(0.66f, 0.74f, 0.86f);
                glm::vec3 haze = glm::mix(sky.horizon, dayHaze,
                                          glm::clamp(sky.skyIntensity * 1.3f, 0.0f, 1.0f));
                haze = glm::mix(haze, sky.sunsetColor, sky.sunsetAmount * 0.5f);
                if (fogHazeTuned_) haze = fogHaze_; // tuning panel override
                // Held light: if the selected hotbar item is an emitter (a lit
                // torch, glowstone, ...), cast a dynamic point light from the eye
                // that travels with the player. Lit per-fragment in the chunk
                // shader (no chunk relight). Off (radius 0) for non-emitters.
                glm::vec4 heldLight(0.0f);
                glm::vec4 heldLightCol(0.0f);
                {
                    const uint16_t heldId = player_.inventory().selectedStack().blockId;
                    const uint8_t emission =
                        heldId < world_.registry().blockCount()
                            ? world_.registry().get(heldId).emission : 0;
                    if (emission > 0) {
                        const glm::vec3 c = world_.registry().get(heldId).emissionColor;
                        heldLight    = glm::vec4(cam.position, static_cast<float>(emission));
                        heldLightCol = glm::vec4(c, emission / 15.0f);
                    }
                }
                worldRenderer_.record(cmd, frameIndex, extent, view, proj,
                                      glm::vec4(sky.lightDir, ambient),
                                      glm::vec4(lightCol, intensity),
                                      heldLight, heldLightCol);

                // Entities into the same scene pass (shared depth + composite/fog,
                // lit like the terrain): the test biped, dropped items as little
                // spinning block cubes, and break particles as tiny block cubes.
                {
                    const LocalPose pose = sampleClip(testEntity_, testWalk_, entityAnimTime_);
                    const std::vector<EntityVertex> bipedMesh =
                        bakeMesh(testEntity_, worldMatrices(testEntity_, pose));
                    std::vector<EntityRenderer::Draw> draws{
                        {&bipedMesh, glm::translate(glm::mat4(1.0f), entityPos_)}};

                    // First-person held tool (sword/pickaxe/torch/hammer): the selected
                    // hotbar item, if it has a Blockbench model, drawn in front of the
                    // camera (lower-right, angled across the view). Placed in VIEW space
                    // — model = inverse(view) * offset — so it stays locked to the camera.
                    // Only in normal first-person play (not free-fly / menus). A subtle
                    // bob on the forward/up axes gives it life as the player moves.
                    const bool showHeld = player_.mode() == PlayerController::Mode::Walking &&
                                          !inventoryOpen_ && !chestOpen_ && !paused_;
                    if (showHeld) {
                        const uint16_t held = player_.inventory().selectedStack().blockId;
                        auto hm = heldModelByItem_.find(held);
                        if (hm != heldModelByItem_.end() && !toolModels_[hm->second].mesh.empty()) {
                            const float bob = std::sin(entityAnimTime_ * 1.7f) * 0.010f;
                            // Held-tool placement. Rotation (deg): Z screen tilt, Y outward
                            // turn, X pitch (wrist toward camera). Position: TX right, TY up,
                            // TZ toward camera (less negative = closer). HS scale. Defaults
                            // chosen from the v8 grid pick, nudged right/closer/more-outward.
                            // All overridable via env (VG_HZ/HY/HX, VG_TX/TY/TZ, VG_HS) for tuning.
                            float hz = 75.0f, hy = -50.0f, hx = -35.0f;
                            float tx = 0.42f, ty = -0.30f, tz = -0.42f, hs = 0.36f;
                            auto envf = [](const char* k, float& v) {
                                if (const char* e = std::getenv(k)) v = static_cast<float>(std::atof(e));
                            };
                            envf("VG_HZ", hz); envf("VG_HY", hy); envf("VG_HX", hx);
                            envf("VG_TX", tx); envf("VG_TY", ty); envf("VG_TZ", tz); envf("VG_HS", hs);
                            glm::mat4 vm(1.0f);
                            vm = glm::translate(vm, glm::vec3(tx, ty + bob, tz + bob));
                            vm = glm::rotate(vm, glm::radians(hz), glm::vec3(0, 0, 1)); // screen tilt
                            vm = glm::rotate(vm, glm::radians(hy), glm::vec3(0, 1, 0)); // outward turn
                            vm = glm::rotate(vm, glm::radians(hx), glm::vec3(1, 0, 0)); // pitch (wrist->cam)
                            vm = glm::scale(vm, glm::vec3(hs));
                            const glm::mat4 model = glm::inverse(view) * vm;
                            // The arm is authored in the tool's own space (handle at the
                            // origin), so the SAME transform makes the fist grip the handle.
                            if (handModel_ >= 0 && !toolModels_[handModel_].mesh.empty())
                                draws.push_back({&toolModels_[handModel_].mesh, model, 1u});
                            draws.push_back({&toolModels_[hm->second].mesh, model, 1u});
                        }
                    }

                    // Passive critters: the SAME box rig, each baked at its own walk
                    // phase and placed by its position + heading. Reserve so the held
                    // meshes don't reallocate (draws hold pointers into this vector).
                    std::vector<std::vector<EntityVertex>> critterMeshes;
                    critterMeshes.reserve(critters_.size());
                    for (const Critter& cr : critters_.all()) {
                        glm::mat4 m = glm::translate(glm::mat4(1.0f), cr.pos);
                        m = glm::rotate(m, cr.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                        if (hasCritterModel_) {
                            // .bbmodel mob rig, walk-cycled by the critter's phase, skin path.
                            critterMeshes.push_back(bakeMesh(
                                critterRig_,
                                worldMatrices(critterRig_,
                                              sampleClip(critterRig_, critterWalk_, cr.animTime))));
                            m = glm::translate(m, critterOffset_);
                            draws.push_back({&critterMeshes.back(), m, 1u});
                        } else {
                            critterMeshes.push_back(bakeMesh(
                                testEntity_,
                                worldMatrices(testEntity_,
                                              sampleClip(testEntity_, testWalk_, cr.animTime))));
                            draws.push_back({&critterMeshes.back(), m});
                        }
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

                    // Dropped items, bobbing and spinning about Y. A tool/item with a
                    // Blockbench model (pickaxe/sword/hammer/torch) renders as that model
                    // (centred on its AABB, scaled to a uniform size, skin path); anything
                    // else falls back to a small block cube.
                    for (const ItemEntity& it : droppedItems_.items()) {
                        if (it.stack.blockId == 0) continue;
                        const float bob = 0.09f * std::sin(it.spin * 1.5f);
                        auto dm = heldModelByItem_.find(it.stack.blockId);
                        if (dm != heldModelByItem_.end() && !toolModels_[dm->second].mesh.empty()) {
                            const ToolModel& tm = toolModels_[dm->second];
                            const float target = 0.55f;                 // world size across
                            const float s = target / tm.span;
                            glm::mat4 m = glm::translate(glm::mat4(1.0f),
                                                         it.pos + glm::vec3(0.0f, bob, 0.0f));
                            m = glm::rotate(m, it.spin, glm::vec3(0.0f, 1.0f, 0.0f));
                            m = glm::scale(m, glm::vec3(s));
                            m = glm::translate(m, -tm.center);          // centre the AABB on pos
                            draws.push_back({&tm.mesh, m, 1u});
                            continue;
                        }
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
                // `haze` (horizon fog colour) was computed once above, before the
                // far-terrain pass, so the shell edge fade and this fog agree.
                const float fogOn = fogEnabled_ ? 1.0f : 0.0f;
                CompositeRenderer::Fog fog{};
                fog.invViewProj = glm::inverse(proj * view);
                fog.camPos = glm::vec4(cam.position, 0.0f);
                fog.color  = glm::vec4(haze, fogOn * fogDistMul_ * 0.00006f * (1.0f + 4.0f * fogW));
                fog.params = glm::vec4(fogFalloff_,                         // height falloff
                                       fogOn * fogGroundMul_ * 0.0028f * fogAmt, // ground fog
                                       72.0f,                               // fog top Y
                                       fogMax_);                            // max fog
                // Underwater murk is gone with the worldgen overhaul — the flat world
                // has no water, so the view is never submerged.
                fog.submerged = 0.0f;
                const int eyeX = static_cast<int>(std::floor(cam.position.x));
                const int eyeY = static_cast<int>(std::floor(cam.position.y));
                const int eyeZ = static_cast<int>(std::floor(cam.position.z));
                // Low-light grain: static creeps in only when the player actually
                // stands in darkness, and any light (a torch's block-light, or
                // daylight reaching the spot) eliminates it. Brightness at the eye =
                // the stronger of block-light and sky-light scaled by the time of day
                // (so open ground is bright at noon, dark at night, caves stay dark).
                const float skyL = world_.skyLightAt(eyeX, eyeY, eyeZ) / 15.0f;
                const float blkL = world_.blockLightAt(eyeX, eyeY, eyeZ) / 15.0f;
                const float eyeBright = std::max(blkL, skyL * sky.skyIntensity);
                const float darkness  = 1.0f - glm::smoothstep(0.12f, 0.5f, eyeBright);
                fog.noiseAmount = settings_.darkNoise * darkness;
                fog.noiseTime   = static_cast<float>(glfwGetTime());
                // Retro post FX (each independent): colour quantisation (bits < 8),
                // ordered dither, interlace flicker. Every term 0/neutral leaves the
                // composite output unchanged.
                retroParity_ ^= 1;
                const int colBits = glm::clamp(settings_.retroColorBits, 3, 8);
                fog.retroLevels    = (colBits < 8) ? static_cast<float>(1 << colBits) : 0.0f;
                fog.retroDither    = settings_.retroDither;
                fog.retroInterlace = settings_.retroInterlace;
                fog.retroParity    = static_cast<float>(retroParity_);
                renderer_.setFog(fog);
            },
            [this](VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent) {
                ui_.record(cmd, frameIndex, extent);
            });

        if (profFrames) {
            const auto profT3 = std::chrono::steady_clock::now();
            auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            const Renderer::PhaseTimes& ph = renderer_.phaseTimes();
            pUpdate += ms(profT0, profT1);
            pUi     += ms(profT1, profT2);
            pWait   += ph.wait;
            pAcq    += ph.acquire;
            pRec    += ph.record;
            pSub    += ph.submit;
            pTotal  += ms(profT0, profT3);
            pMax       = std::max(pMax, ms(profT0, profT3));
            pMaxUpdate = std::max(pMaxUpdate, ms(profT0, profT1));
            if (++pN == 120) {
                std::printf("[frame] avg %.2fms (%.0f fps) | max %.1f (update %.1f) | "
                            "update %.2f | ui %.2f | draw: wait %.2f acq %.2f rec %.2f "
                            "sub %.2f | %zu chunks %.1fM tris drawn | cull: %zu vis "
                            "%zu culled %zu calls\n",
                            pTotal / pN, 1000.0 * pN / pTotal, pMax, pMaxUpdate,
                            pUpdate / pN, pUi / pN,
                            pWait / pN, pAcq / pN, pRec / pN, pSub / pN,
                            worldRenderer_.drawnChunkCount(),
                            static_cast<double>(worldRenderer_.triangleCount()) / 1e6,
                            worldRenderer_.visibleChunkCount(),
                            worldRenderer_.culledChunkCount(),
                            worldRenderer_.drawCallCount());
                pUpdate = pUi = pWait = pAcq = pRec = pSub = pTotal = 0;
                pMax = pMaxUpdate = 0;
                pN = 0;
            }
        }

        if (maxFrames >= 0 && ++frame >= maxFrames) {
            break;
        }
    }

    // Join any in-flight background relight/pregen before tearing down (their
    // results are no longer needed), then make sure the GPU is finished before
    // destructors free.
    if (relightFuture_.valid()) {
        relightFuture_.get();
    }
    for (auto& f : pregenQueue_) {
        if (f.valid()) f.get();
    }
    for (auto& f : pregenRetired_) {
        if (f.valid()) f.get();
    }
    pregenQueue_.clear();
    pregenRetired_.clear();
    renderer_.waitIdle();

    settings_.save(settingsPath()); // persist on exit (e.g. via the Exit button)
    savePlayer();                   // persist position/health/inventory next to chunks
    saveChests();                   // persist chest contents next to chunks

    if (!screenshotPath.empty()) {
        renderer_.saveScreenshot(screenshotPath);
    }
}

} // namespace vg
