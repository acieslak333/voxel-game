#pragma once

#include "clouds/CloudSystem.h"
#include "core/DayNight.h"
#include "core/Input.h"
#include "core/Palette.h"
#include "core/Settings.h"
#include "core/Window.h"
#include "player/PlayerController.h"
#include "render/Renderer.h"
#include "render/SkyRenderer.h"
#include "render/Swapchain.h"
#include "render/UiRenderer.h"
#include "render/VulkanContext.h"
#include "render/WorldRenderer.h"
#include "world/World.h"

#include <deque>
#include <future>
#include <string>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  App
// -----------------------------------------------------------------------------
//  Top-level object that owns the window, the Vulkan stack, the world (data),
//  the world renderer and the player, and runs the main loop. Member
//  declaration order matters: each subsystem depends on the ones declared above
//  it, and members are constructed top-to-bottom / destroyed bottom-to-top,
//  which is exactly the order Vulkan requires.
// -----------------------------------------------------------------------------
class App {
public:
    App();

    void run(long maxFrames = -1, const std::string& screenshotPath = "");

    // Debug: start in free-fly looking down over the whole world. Handy for a
    // bird's-eye verification screenshot of the procedural terrain.
    void enableFlyOverview();

private:
    // Mine (hold-to-break, by hardness/tool) and place the block under the
    // crosshair, then remesh only the chunk(s) the edit touched (see
    // World::setBlock / WorldRenderer::remeshChunk). dt drives the mining timer.
    void editBlocks(const InputState& in, float dt);
    // Destroy the block at world coords (drops it to the inventory in survival) /
    // place `id` there; both sync with streaming, remesh and reseed liquid flow.
    void breakBlockAt(const glm::ivec3& b);
    void placeBlockAt(const glm::ivec3& t, uint16_t id);

    // Survival per-frame upkeep: environmental damage (lava) and death/respawn.
    // No-op in creative. Health regen/fall damage live in PlayerController.
    void updateSurvival(float dt);

    // Player persistence (ISSUES #13K): position/look/health/inventory/game-mode to
    // <world save dir>/player.dat, next to the saved chunks. No-op when persistence
    // is off (non-streaming world). loadPlayer() returns false if there's nothing to
    // restore, so the caller keeps the default spawn + starter kit.
    void savePlayer() const;
    bool loadPlayer();

    // Simple liquid flow (water & lava): drain a budget of queued liquid cells,
    // spreading them down then sideways (decaying distance in Block::metadata, no
    // recede). Event-driven — editBlocks seeds the queue around any edit. Mutates
    // the world through the same stream-barrier path as editBlocks.
    void tickLiquids();
    void seedLiquid(int x, int y, int z); // queue a cell + its 6 neighbours

    // Build this frame's UI (HUD + menu) into ui_, handling menu interactions.
    void buildUi(const InputState& in);
    void buildHotbar(class Ui& ui, float w, float h);
    // Full inventory screen (E): the backpack grid + hotbar row, with click-to-move
    // between slots via a mouse-held cursor stack.
    void buildInventory(class Ui& ui, float w, float h, const InputState& in);
    void buildMenu(class Ui& ui, float px, float py, float pw, float ph); // Esc menu column
    // Second column: live atmosphere tuning (clouds/fog/weather/sky). Ephemeral.
    void buildTuning(class Ui& ui, float px, float py, float pw, float ph);
    // F1 debug overlay: position, chunk, facing, FPS, light levels, target, ...
    void buildDebugOverlay(class Ui& ui);
    // Centre-screen aiming reticle (pixel-art outlined plus).
    void buildCrosshair(class Ui& ui, float w, float h);
    // Wireframe box around the block under the crosshair, projected into the HUD.
    void buildBlockIndicator(class Ui& ui, const glm::mat4& view, const glm::mat4& proj,
                             float w, float h);
    void togglePause();
    void toggleInventory();
    // Switch between creative (every block, infinite, no depletion) and survival
    // (mine to collect, placing consumes). Entering creative re-stocks all blocks.
    void toggleGameMode();
    // Fill the inventory with one full stack of every placeable block type.
    void stockCreative();

    // Push every value in settings_ to the subsystem that owns it.
    void applySettings();
    void cycleFont(int dir);
    // Where the persisted settings live (next to the game's assets).
    [[nodiscard]] static std::string settingsPath();

    static constexpr int kWidth  = 1280;
    static constexpr int kHeight = 720;

    // How far (in blocks) the player can reach to edit terrain.
    static constexpr float kReach = 5.0f;
    // HP per second of standing in lava.
    static constexpr float kLavaDamagePerSec = 20.0f;

    // World size, seed, and terrain shaping live in assets/world.yaml (loaded into
    // a WorldConfig and passed to World below).

    Settings         settings_;  // persisted options (declared first: loaded early)
    Palette          palette_;   // named colours from assets/colors.yaml
    DayNight         dayNight_;  // time of day + sky/light state (assets/sky.yaml)
    Window           window_;
    VulkanContext    context_;
    Swapchain        swapchain_;
    Renderer         renderer_;
    CloudSystem      clouds_;     // weather + noise for the volumetric cloud layer
    SkyRenderer      skyRenderer_; // atmosphere + volumetric clouds, first in the scene pass
    World            world_;
    WorldRenderer    worldRenderer_;
    UiRenderer       ui_;        // 2D HUD/menu renderer (after renderer_)
    Input            input_;
    PlayerController  player_;

    // UI state.
    bool             paused_ = false;        // escape menu open
    bool             inventoryOpen_ = false; // inventory screen open (E)
    bool             creativeMode_ = true;   // creative: every block, infinite, no depletion (G toggles)
    ItemStack        cursorStack_;           // item held by the mouse in the inventory screen
    bool             debugOverlay_ = false;  // F1 info overlay visible

    // Hold-to-break mining state. While the left button is held on one block, time
    // accumulates until it reaches the block's break time (hardness / tool speed);
    // changing target or releasing resets it. mineProgress01_ feeds the HUD.
    glm::ivec3 mineBlock_{0, 0, 0};
    bool       mineActive_   = false;
    float      mineProgress_ = 0.0f; // seconds accumulated on the current block
    float      mineNeeded_   = 0.0f; // seconds required to break it (0 = instant)
    float      mineProgress01_ = 0.0f; // 0..1 for the crosshair break meter

    glm::vec3  spawnFeet_{0.0f}; // respawn point (world spawn; player save comes later)
    float            smoothedDt_ = 1.0f / 60.0f; // EMA of frame time, for the overlay's FPS

    // Atmosphere tuning panel (ephemeral 2nd menu column; not persisted).
    int   tuningTab_   = 0;     // 0 weather, 1 clouds, 2 fog, 3 sky
    int   colorTarget_ = 0;     // which colour the RGB sliders edit (Sky tab)
    bool  fogEnabled_   = true;
    float fogDistMul_   = 1.0f; // x distance-fog density
    float fogGroundMul_ = 1.0f; // x ground-fog density
    float fogFalloff_   = 0.05f;// height falloff
    float fogMax_       = 0.92f;// max fog
    bool      fogHazeTuned_ = false;
    glm::vec3 fogHaze_{0.66f, 0.74f, 0.86f};

    // Liquid flow: cells pending a flow evaluation, drained a budget per tick.
    std::deque<glm::ivec3> liquidQueue_;
    float                  liquidTimer_ = 0.0f; // accumulates dt between flow ticks

    // Async streaming: the in-flight background relight task (World::relightBoxes).
    // It returns the dirty chunk list; the MAIN thread enqueues the remeshes (so the
    // renderer's per-slot version state stays single-threaded). Invalid when none is
    // running. Declared last so it is destroyed first — its destructor joins the
    // task while world_/worldRenderer_ are still alive.
    std::future<std::vector<glm::ivec3>> relightFuture_;
};

} // namespace vg
