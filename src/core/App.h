#pragma once

#include "clouds/CloudSystem.h"
#include "core/DayNight.h"
#include "entity/Armature.h"
#include "entity/BlockbenchModel.h"
#include "entity/Critters.h"
#include "entity/ItemEntity.h"
#include "entity/Particles.h"
#include "core/Input.h"
#include "core/Palette.h"
#include "core/Settings.h"
#include "core/Window.h"
#include "player/ChestStore.h"
#include "player/Crafting.h"
#include "player/Equipment.h"
#include "player/PlayerController.h"
#include "render/Renderer.h"
#include "render/SkyRenderer.h"
#include "render/Swapchain.h"
#include "render/EntityRenderer.h"
#include "render/TextureArray.h"
#include "render/UiRenderer.h"
#include "render/VulkanContext.h"
#include "render/WorldRenderer.h"
#include "world/World.h"

#include <deque>
#include <future>
#include <string>
#include <unordered_map>
#include <utility>
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
    void placeBlockAt(const glm::ivec3& t, uint16_t id, uint8_t metadata = 0);
    // Change only a block's shape metadata in place (keeps its id; no inventory
    // cost) — the hammer's reshape/rotate. Same streaming/relight/remesh path.
    void reshapeBlockAt(const glm::ivec3& b, uint16_t id, uint8_t metadata);

    // An edit deferred because a background relight was in flight (REVIEW R3): the
    // *BlockAt entry points record the INTENT here and flushPendingEdits() re-runs
    // them once the relight finishes (usually 1-3 frames). Capturing intent (not the
    // half-done edit) means the inventory/particle side effects happen exactly once,
    // when the edit applies. Defined before the helpers below that take it by ref.
    struct PendingEdit {
        enum Kind { Break, Place, Reshape } kind;
        glm::ivec3 pos;
        uint16_t   id       = 0;
        uint8_t    metadata = 0;
    };

    // Preamble every world mutation (setBlock, setLightFalloff, recenter) must run
    // first: join any in-flight background relight (enqueuing its remeshes), then
    // drain the mesh-worker pool so no worker is mid-read of the World. The only
    // safe ordering against the streaming threads (REVIEW invariants 1-2, R1).
    // BLOCKS on an in-flight relight — use only off the per-frame hot path (the
    // Esc-menu falloff sliders); edits/liquids use tryPrepareWorldMutation instead.
    void drainBeforeWorldMutation();

    // Non-blocking variant for the per-frame edit/liquid paths (REVIEW R3). Prepares
    // the world for mutation (collect a finished relight's remeshes, drain workers)
    // and returns true ONLY if that needs no blocking; returns false while a
    // background relight is still flooding the edge, so the caller defers (edits) or
    // skips (liquids) rather than stalling the frame.
    [[nodiscard]] bool tryPrepareWorldMutation();
    // Re-run any edits deferred by tryPrepareWorldMutation, once the relight that
    // forced the deferral has finished. Called each frame before editBlocks.
    void flushPendingEdits();
    // Record a deferred edit (deduped on kind+pos: a held mine button can re-trigger
    // the same break every frame while the relight runs).
    void enqueueEdit(const PendingEdit& e);

    // Survival per-frame upkeep: environmental damage (lava) and death/respawn.
    // No-op in creative. Health regen/fall damage live in PlayerController.
    void updateSurvival(float dt);

    // Player persistence (ISSUES #13K): position/look/health/inventory/game-mode to
    // <world save dir>/player.dat, next to the saved chunks. No-op when persistence
    // is off (non-streaming world). loadPlayer() returns false if there's nothing to
    // restore, so the caller keeps the default spawn + starter kit.
    void savePlayer() const;
    bool loadPlayer();
    // Chest contents persistence (<save dir>/chests.dat). No-op when persistence off.
    void saveChests() const;
    void loadChests();

    // Advance the streamed chunk window one step toward the player (per frame): the
    // pregen/relight futures, the window-step busy-gate (R5), and the synchronous
    // fallback. Holds the four-actor threading invariants in one place (REVIEW R9) —
    // see the definition's header comment before touching it.
    void streamWindow();

    // Build this frame's UI (HUD + menu) into ui_, handling menu interactions.
    void buildUi(const InputState& in);
    void buildHotbar(class Ui& ui, float w, float h);
    // Full inventory screen (E): the backpack grid + hotbar row, with click-to-move
    // between slots via a mouse-held cursor stack.
    void buildInventory(class Ui& ui, float w, float h, const InputState& in);
    // Hammer shape picker: a hold-to-open radial. Holding right-click (with the
    // hammer) shows the row of shapes; the mouse slides the selector; release sets
    // the highlighted shape as the active build shape.
    void buildShapePicker(class Ui& ui, float w, float h, const InputState& in);
    // Terraria-style crafting list beside the inventory: the recipes craftable from
    // the current inventory, click an output to craft one. (ISSUES #13B)
    void buildCrafting(class Ui& ui, float x, float y, float w, const InputState& in);
    // Open-chest screen: the chest's 27-slot grid above the player's inventory,
    // click-to-move between them (and the player hotbar). (ISSUES #13B)
    void buildChest(class Ui& ui, float w, float h, const InputState& in);
    // Move items between a clicked slot and the mouse-held cursor stack (pick up /
    // drop / merge / swap) — shared by the inventory and chest screens.
    void clickSlot(ItemStack& s);
    // Armour/trinket equip column in the inventory screen; clicks validate the item
    // type per slot. applyEquipmentStats pushes the aggregated bonuses to the player.
    void buildEquipment(class Ui& ui, float x, float y, const InputState& in);
    void clickEquipSlot(int slotIndex);
    void applyEquipmentStats();
    void openChestAt(const glm::ivec3& pos); // open the chest block at pos
    void toggleChest();                      // close the open chest (return cursor)
    void buildMenu(class Ui& ui, float px, float py, float pw, float ph); // Esc menu column
    // Second column: live atmosphere tuning (clouds/fog/weather/sky). Ephemeral.
    void buildTuning(class Ui& ui, float px, float py, float pw, float ph);
    // Modal palette picker (opened from the Retro tab): one row per palette showing
    // its name and a live swatch strip, so you can see each look before choosing.
    void buildPalettePicker(class Ui& ui, float w, float h, const InputState& in);
    // (Re)scan assets/colorpalettes/ into paletteList_ (Off + each .hex with its
    // colours) and rebuild each palette's remapped preview thumbnail. Called when
    // the picker opens.
    void refreshPaletteCache();
    // Load assets/palette_preview.png once and box-downsample it into previewThumb_
    // (the reference game frame the picker shows remapped through each palette).
    void loadPreviewThumb();
    // F1 debug overlay: position, chunk, facing, FPS, light levels, target, ...
    void buildDebugOverlay(class Ui& ui);
    // Centre-screen aiming reticle (pixel-art outlined plus).
    void buildCrosshair(class Ui& ui, float w, float h);
    // Floating damage numbers projected from world space to the HUD.
    void buildDamageNumbers(class Ui& ui, const glm::mat4& view, const glm::mat4& proj,
                            float w, float h);
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
    // Load settings_.retroPalette from disk and bind it to the composite pass
    // (empty name -> palette off). Called from applySettings() and cyclePalette().
    void applyRetroPalette();
    // Where the persisted settings live (next to the game's assets).
    [[nodiscard]] static std::string settingsPath();

    static constexpr int kWidth  = 1600;
    static constexpr int kHeight = 900;

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
    TextureArray     entitySkins_;    // Blockbench-model skin atlas (assets/models/*.png)
    EntityRenderer   entityRenderer_; // animated box-rig entities (ISSUES #13E)
    UiRenderer       ui_;        // 2D HUD/menu renderer (after renderer_)
    Input            input_;
    PlayerController  player_;
    Crafting          crafting_; // data-driven recipe list (assets/recipes.yaml)
    ChestStore        chests_;   // per-position chest contents (persisted to disk)
    Equipment         equipment_; // worn armour + trinkets (persisted with the player)
    ItemEntities      droppedItems_; // dropped-item entities (physics + walk-over pickup)
    Particles         particles_;    // block-break chip particles (ISSUES #13M)
    ParticleEffect    breakEffect_;  // data-driven break burst (assets/particles/break.prtcl)
    ParticleEffect    placeEffect_;  // soft dust poof on placing a block (place.prtcl)
    ParticleEffect    splashEffect_; // droplets when the player enters water (splash.prtcl)
    // Game-feel juice (ISSUES #13H/M): a decaying screen-shake magnitude (blocks of
    // camera jitter) and a brief hit-stop freeze, triggered by hard landings / damage.
    float             shakeMag_  = 0.0f;
    float             hitStop_   = 0.0f;   // seconds of frozen gameplay remaining
    float             prevHealth_ = 100.0f; // to detect damage (drives shake + hit-stop)
    bool              wasInWater_ = false; // edge-detect water entry for the splash
    ParticleEffect    emberEffect_;        // glowing sparks rising off nearby lava (ember.prtcl)
    float             emberTimer_   = 0.0f;
    uint32_t          emberCounter_ = 0;   // rotates which nearby lava cell emits
    // Floating damage numbers (ISSUES #13M): rise + fade from where damage was taken.
    struct FloatText { glm::vec3 pos; float value; float age; };
    std::vector<FloatText> damageNumbers_;

    // Test entity (ISSUES #13E E4): a procedural animated biped proving the rig +
    // EntityRenderer pipeline end to end. Built once; animated by entityAnimTime_.
    Skeleton          testEntity_;
    AnimationClip     testWalk_;
    glm::vec3         entityPos_{0.0f};   // world position (feet) of the test biped
    float             entityAnimTime_ = 0.0f;
    void buildTestEntity();

    // Blockbench tool viewmodels (ISSUES #13E): the hammer/sword/pickaxe/torch .bbmodels
    // (each from its own skin-atlas slice) rendered as the first-person HELD item — the
    // selected hotbar tool is drawn in front of the camera via the EntityRenderer skin
    // path. Each mesh is baked once at rest pose; the held draw transforms it by the
    // inverse view. `heldModelByItem_` maps a held item id -> its toolModels_ index.
    struct ToolModel {
        std::vector<EntityVertex> mesh;    // baked rest pose (skin layer baked into verts)
        std::string               name;    // source .bbmodel filename (for logging)
        glm::vec3                 center{0.0f}; // AABB centre (for centring a world drop)
        float                     span = 1.0f;  // largest AABB dimension (for scaling a drop)
    };
    std::vector<ToolModel>                  toolModels_;
    std::unordered_map<uint16_t, size_t>    heldModelByItem_; // item id -> toolModels_ index
    int                                     handModel_ = -1;  // toolModels_ index of the first-person arm

    // Mob rig from a .bbmodel (ISSUES #13E stage 2): if assets/models/critter exists,
    // its loaded Skeleton (legs as joints) drives the wandering critters with a
    // procedural walk clip, replacing the hand-coded biped. Empty -> biped fallback.
    Skeleton          critterRig_;
    AnimationClip     critterWalk_;
    glm::vec3         critterOffset_{0.0f}; // centres the rig on the entity pos (feet at y)
    bool              hasCritterModel_ = false;
    void buildModels();

    Critters          critters_;          // passive wandering critters (.bbmodel rig or box fallback)
    void spawnCritters();                 // seed a few around spawn on the surface

    // UI state.
    bool             paused_ = false;        // escape menu open
    bool             inventoryOpen_ = false; // inventory screen open (E)
    bool             chestOpen_ = false;     // a chest's screen is open
    glm::ivec3       openChest_{0, 0, 0};    // which chest block is open
    bool             creativeMode_ = true;   // creative: every block, infinite, no depletion (G toggles)
    ItemStack        cursorStack_;           // item held by the mouse in the inventory screen
    bool             debugOverlay_ = false;  // F1 info overlay visible
    int              craftScroll_ = 0;       // first visible row of the (scrollable) crafting list

    // Hold-to-break mining state. While the left button is held on one block, time
    // accumulates until it reaches the block's break time (hardness / tool speed);
    // changing target or releasing resets it. mineProgress01_ feeds the HUD.
    glm::ivec3 mineBlock_{0, 0, 0};
    bool       mineActive_   = false;
    float      mineProgress_ = 0.0f; // seconds accumulated on the current block
    float      mineNeeded_   = 0.0f; // seconds required to break it (0 = instant)
    float      mineProgress01_ = 0.0f; // 0..1 for the crosshair break meter

    // Edits deferred because a background relight was in flight (REVIEW R3); see the
    // PendingEdit definition above. flushPendingEdits() drains this each frame once
    // the relight finishes.
    std::deque<PendingEdit> pendingEdits_;

    glm::vec3  spawnFeet_{0.0f}; // respawn point (world spawn; player save comes later)
    uint16_t   chestId_ = 0;     // resolved id of the chest block (0 if undefined)

    // Hammer / block shapes (ISSUES #16). The hammer is a held tool that reshapes
    // shapeable blocks. buildShape_ is the active shape (set via the B picker); a
    // shapeable block placed while the hammer is in the hotbar is placed in this
    // shape, and right-clicking a block with the hammer sets/rotates it.
    uint16_t   hammerId_     = 0;                   // resolved id of the hammer item (0 if none)
    ShapeKind  buildShape_   = ShapeKind::Slab;     // active shape the hammer applies/places
    bool       shapePickerOpen_ = false;            // the hold-to-open shape radial is showing
    ShapeKind  shapePickerSel_  = ShapeKind::Slab;  // shape currently highlighted in the radial
    float      pickerSelPos_    = 0.0f;             // selector position (cell units) the mouse slides
    float            smoothedDt_ = 1.0f / 60.0f; // EMA of frame time, for the overlay's FPS

    int   retroParity_ = 0;     // flips 0/1 each frame to drive the PS2 interlace field
    // Retro colour-palette picker (Retro tab -> modal popup). paletteList_ caches
    // the selectable palettes (name + swatches; index 0 = "" Off) for the preview.
    bool  palettePickerOpen_ = false;
    std::vector<std::pair<std::string, std::vector<glm::vec3>>> paletteList_;
    // Per-palette preview: previewThumb_ is the reference frame downsampled to
    // previewW_ x previewH_ (sRGB); palettePreview_[i] is that frame remapped
    // through paletteList_[i] (index 0 = Off = the unmodified frame).
    std::vector<glm::vec3>              previewThumb_;
    int                                 previewW_ = 0, previewH_ = 0;
    std::vector<std::vector<glm::vec3>> palettePreview_;
    // Escape-menu tabs (left column). Groups the options so they fit one panel.
    int   menuTab_     = 0;     // 0 display, 1 effects, 2 game, 3 world, 4 retro
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

    // Async streaming: the in-flight background relight task (World::relightBoxes).
    // It returns the dirty chunk list; the MAIN thread enqueues the remeshes (so the
    // renderer's per-slot version state stays single-threaded). Invalid when none is
    // running. Declared last so it is destroyed first — its destructor joins the
    // task while world_/worldRenderer_ are still alive.
    std::future<std::vector<glm::ivec3>> relightFuture_;
    // A window step (pregen-strip apply / teleport regen) may only mutate the window
    // when no relight is in flight AND the mesh workers are idle (the gate). When the
    // gate is closed the step simply waits — the frame is NEVER blocked on relight, so
    // the window just trails the player and catches up at relight throughput (the
    // far-terrain LOD shell covers anything beyond the window meanwhile). The old
    // "force a bounded drain after N postponed frames" path was the streaming hitch:
    // under fast travel the gate is closed most frames, so it fired almost every step
    // and blocked the frame ~115ms on relightFuture_.get()+streamBarrier (measured).
    // The ONLY time a step force-drains now is the genuine-load case below: the window
    // has fallen so far behind that the player is about to reach its leading edge.
    // Past that the player would leave the loaded window (collision/edits query air),
    // so a bounded blocking catch-up is the right tradeoff there — and it is rare
    // (only when the player out-runs generation, not during normal play).
    static constexpr int kWindowEdgeSafetyChunks = 1; // force a load within this many
                                                      // chunks of the leading edge
    // Background strip pregeneration (World::pregenStrip): the entering edge columns,
    // generated off-thread so the window step itself is just a swap (the per-boundary
    // frame spike was this generation). Reads only the immutable generator + save
    // files, so it may overlap the relight — but the window must not move while a
    // strip is being APPLIED (all recenter paths are gated on it).
    //
    // A small FIFO queue stages several columns ahead of the player along the current
    // travel axis: each entry's strip steps from origin+k, so when the window advances
    // (front applied, origin += step) the next entry is already generating or ready —
    // the per-column pregen never lands on the critical path even when the player
    // crosses several boundaries per second. The queue is for a single (dir,alongX);
    // a turn clears it (pregenDir_/pregenAlongX_ track what it was built for). Declared
    // last, like relightFuture_, so the futures' destructors join before world_ dies.
    std::deque<std::future<World::PregenStrip>> pregenQueue_;
    // Stale strips (a turn abandoned their axis/dir) drain here instead of being joined
    // on the main thread — joining a mid-flight pregen would reintroduce the per-turn
    // hitch. Their results are discarded; they are reaped (destroyed) once ready, which
    // never blocks. SAFE to keep running through a window move: a pregen only writes its
    // own staging chunks and reads the immutable generator + its leading-edge save file,
    // while a move touches in-memory ring slots and the trailing-edge departing files —
    // disjoint memory, disjoint files (leading vs trailing edge are >=1 chunk apart).
    std::vector<std::future<World::PregenStrip>> pregenRetired_;
    int  pregenDir_    = 0;     // +1/-1 along the queued axis (0 = queue empty/idle)
    bool pregenAlongX_ = true;  // axis the queued strips step along
    static constexpr int kPregenAhead = 2; // columns staged ahead of the player
};

} // namespace vg
