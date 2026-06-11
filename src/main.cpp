#include "core/App.h"
#include "core/Input.h"
#include "entity/ItemEntity.h"
#include "player/ChestStore.h"
#include "player/Crafting.h"
#include "player/Equipment.h"
#include "player/PlayerController.h"
#include "player/PlayerSave.h"

#include "world/BlockRegistry.h"
#include "world/TerrainGenerator.h"
#include "world/World.h"
#include "world/WorldConfig.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef VG_ASSET_DIR
#define VG_ASSET_DIR "assets"
#endif

namespace {

// FNV-1a over 8 bytes — a cheap, stable spatial hash for snapshot testing.
inline uint64_t fnv1a(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h = (h ^ (v & 0xffu)) * 0x100000001b3ull;
        v >>= 8;
    }
    return h;
}

// Hash a world's generated blocks + baked lighting through the public accessors.
uint64_t hashWorld(const vg::World& w) {
    const glm::ivec3 s = w.sizeInBlocks();
    uint64_t h = 0xcbf29ce484222325ull;
    for (int z = 0; z < s.z; ++z) {
        for (int y = 0; y < s.y; ++y) {
            for (int x = 0; x < s.x; ++x) {
                const vg::Block b = w.blockAt(x, y, z);
                h = fnv1a(h, b.id);
                h = fnv1a(h, b.metadata);
                h = fnv1a(h, w.skyLightAt(x, y, z));
                h = fnv1a(h, w.blockLightAt(x, y, z));
            }
        }
    }
    return h;
}

// Headless world-generation self-test (no window/Vulkan). Uses a FIXED config
// (independent of assets/world.yaml) so the golden hash is stable, generates the
// world twice, and checks (a) regeneration is bit-identical and (b) the output
// matches the recorded golden. Bump kGolden only for an intentional worldgen
// change (per WORLD_GEN_AGENT_TIPS §6).
int runWorldGenSelfTest(const std::string& assetDir) {
    vg::WorldConfig cfg;
    cfg.seed          = 1337u;
    cfg.streaming     = false; // fixed grid: no threads, no disk, fully deterministic
    cfg.streamWorkers = 0;
    cfg.viewRadius    = 4;      // 9x9 chunks
    cfg.heightChunks  = 8;      // 128 tall: sea level 64 in the middle (biome pipeline)
    cfg.chunksX = cfg.chunksZ = 2 * cfg.viewRadius + 1;
    cfg.chunksY = cfg.heightChunks;
    // Everything else keeps the documented WorldConfig.h defaults (stable).

    const std::string blocks = assetDir + "/blocks.yaml";

    // Recorded golden. NOTE: the selftest reads assets/biomes.yaml, so TUNING the
    // generation (e.g. via tools/genmap_tool.py) intentionally changes this hash —
    // rebaseline when the config settles. Last set for single-island mode. Bump ONLY
    // for an intentional worldgen change (per WORLD_GEN_AGENT_TIPS §6).
    constexpr uint64_t kGolden = 0x3ca4dfb49ca7f61eull;

    uint64_t h1 = 0, h2 = 0;
    try {
        h1 = hashWorld(vg::World(cfg, blocks));
        h2 = hashWorld(vg::World(cfg, blocks));
    } catch (const std::exception& e) {
        std::cerr << "[selftest] FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "[selftest] worldgen hash = 0x" << std::hex << h1 << std::dec << '\n';
    bool ok = true;
    if (h1 != h2) {
        std::cerr << "[selftest] FAIL: regeneration is non-deterministic (0x" << std::hex
                  << h1 << " vs 0x" << h2 << std::dec << ")\n";
        ok = false;
    }
    if (kGolden != 0 && h1 != kGolden) {
        std::cerr << "[selftest] FAIL: worldgen output changed vs golden 0x" << std::hex
                  << kGolden << std::dec << " (bump kGolden if this was intentional)\n";
        ok = false;
    }
    if (ok) {
        std::cout << "[selftest] PASS";
        if (kGolden == 0) {
            std::cout << " (golden not locked — set kGolden to 0x" << std::hex << h1
                      << std::dec << ")";
        }
        std::cout << '\n';
    } else {
        std::cout << "[selftest] FAIL\n";
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Headless top-down map export (no window/Vulkan). Samples the terrain generator
// over a grid and writes a PNG coloured by surface block + water depth + hillshade,
// so the whole world can be inspected/tuned at a glance. This is the engine the
// live generation tool drives (it edits assets/biomes.yaml, re-runs this, shows the
// PNG) — so the tool always matches the game's real generation. Uses a FIXED seed
// so a parameter change is comparable run-to-run.
int runGenMap(const std::string& assetDir, int pixels, int step, const std::string& outPath) {
    pixels = std::clamp(pixels, 64, 4096);
    step   = std::max(1, step);
    const vg::WorldConfig cfg = vg::WorldConfig::load(assetDir + "/world.yaml");
    const std::uint32_t seed = 1337u; // stable across runs for comparable tuning
    const int worldHeight = cfg.chunksY * 16;

    vg::BlockRegistry reg(assetDir + "/blocks.yaml");
    vg::TerrainGenerator gen(seed, reg, assetDir, worldHeight);
    auto bid = [&](const char* n) -> int {
        try { return static_cast<int>(reg.idByName(n)); } catch (...) { return -1; }
    };
    const int snowId = bid("snow"), sandId = bid("sand"), stoneId = bid("stone");

    std::vector<unsigned char> img(static_cast<size_t>(pixels) * pixels * 3);
    const int half = pixels / 2;
    auto H = [&](int wx, int wz) { return gen.height(wx, wz); };
    for (int py = 0; py < pixels; ++py) {
        for (int px = 0; px < pixels; ++px) {
            const int wx = (px - half) * step, wz = (py - half) * step;
            const vg::ColumnInfo ci = gen.columnInfo(wx, wz);
            float r, g, b;
            if (ci.height < ci.waterLevel) {
                const float t = std::min(1.0f, static_cast<float>(ci.waterLevel - ci.height) / 40.0f);
                r = 60.0f - 46.0f * t; g = 130.0f - 92.0f * t; b = 200.0f - 110.0f * t;
            } else {
                if (ci.topId == snowId)       { r = 236; g = 241; b = 248; }
                else if (ci.topId == sandId)  { r = 224; g = 205; b = 150; }
                else if (ci.topId == stoneId) { r = 132; g = 129; b = 124; }
                else                          { r = 86;  g = 140; b = 70;  } // grass / default
                // Hillshade: light from the NW, so slopes read as relief.
                const float dx = static_cast<float>(H(wx + step, wz) - H(wx - step, wz));
                const float dz = static_cast<float>(H(wx, wz + step) - H(wx, wz - step));
                const float shade = std::clamp(1.0f + (-dx - dz) * 0.05f, 0.55f, 1.35f);
                r *= shade; g *= shade; b *= shade;
            }
            const size_t o = (static_cast<size_t>(py) * pixels + px) * 3;
            img[o + 0] = static_cast<unsigned char>(std::clamp(r, 0.0f, 255.0f));
            img[o + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 255.0f));
            img[o + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 255.0f));
        }
    }
    if (!stbi_write_png(outPath.c_str(), pixels, pixels, 3, img.data(), pixels * 3)) {
        std::cerr << "[genmap] failed to write " << outPath << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "[genmap] wrote " << outPath << " (" << pixels << "x" << pixels
              << ", " << step << " blocks/px, seed " << seed << ")\n";
    return EXIT_SUCCESS;
}

// A diverging blue->white->red ramp for a signed value in [-1, 1]: negative reads
// cool, zero white, positive warm. Used to visualise raw noise layers.
void divergingRamp(float t, float& r, float& g, float& b) {
    t = std::clamp(t, -1.0f, 1.0f);
    if (t >= 0.0f) { // white -> red
        r = 255.0f; g = 255.0f - 175.0f * t; b = 255.0f - 215.0f * t;
    } else {         // white -> blue
        const float s = -t;
        r = 255.0f - 215.0f * s; g = 255.0f - 150.0f * s; b = 255.0f;
    }
}

// Headless raw-noise-field export (no window/Vulkan). Samples ONE of the terrain
// generator's underlying noise layers over a top-down grid and writes a diverging
// blue/white/red PNG, so each layer that feeds the terrain can be inspected and
// tuned in isolation (not just the final surface). The relief field also draws the
// sea-level (value 0) coastline in black. Same fixed seed + centring as --genmap.
int runGenNoise(const std::string& assetDir, int pixels, int step,
                const std::string& outPath, const std::string& layer) {
    pixels = std::clamp(pixels, 64, 4096);
    step   = std::max(1, step);
    const vg::WorldConfig cfg = vg::WorldConfig::load(assetDir + "/world.yaml");
    const std::uint32_t seed = 1337u;
    const int worldHeight = cfg.chunksY * 16;
    vg::BlockRegistry reg(assetDir + "/blocks.yaml");
    vg::TerrainGenerator gen(seed, reg, assetDir, worldHeight);

    using F = vg::TerrainGenerator::Field;
    F field; bool isRelief = false;
    if      (layer == "cont" || layer == "continentalness") field = F::Continentalness;
    else if (layer == "ero"  || layer == "erosion")         field = F::Erosion;
    else if (layer == "peak" || layer == "peaks")           field = F::Peaks;
    else if (layer == "temp" || layer == "temperature")     field = F::Temperature;
    else if (layer == "hum"  || layer == "humidity")        field = F::Humidity;
    else if (layer == "river" || layer == "rivers")         field = F::River;
    else if (layer == "relief" || layer == "height") { field = F::Relief; isRelief = true; }
    else {
        std::cerr << "[genmap] unknown --layer '" << layer << "' (use cont|ero|peak|"
                     "temp|hum|river|relief)\n";
        return EXIT_FAILURE;
    }
    // Relief is in blocks; normalise by a representative span so the ramp uses its
    // full range. Noise layers are already ~[-1, 1].
    const float reliefSpan = static_cast<float>(std::max(16, worldHeight / 2));

    std::vector<unsigned char> img(static_cast<size_t>(pixels) * pixels * 3);
    const int half = pixels / 2;
    for (int py = 0; py < pixels; ++py) {
        for (int px = 0; px < pixels; ++px) {
            const int wx = (px - half) * step, wz = (py - half) * step;
            float v = gen.fieldValue(field, wx, wz);
            float r, g, b;
            if (isRelief) {
                divergingRamp(v / reliefSpan, r, g, b);
                if (std::fabs(v) < 0.75f) { r = g = b = 0.0f; } // sea-level coastline
            } else {
                divergingRamp(v, r, g, b);
            }
            const size_t o = (static_cast<size_t>(py) * pixels + px) * 3;
            img[o + 0] = static_cast<unsigned char>(std::clamp(r, 0.0f, 255.0f));
            img[o + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 255.0f));
            img[o + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 255.0f));
        }
    }
    if (!stbi_write_png(outPath.c_str(), pixels, pixels, 3, img.data(), pixels * 3)) {
        std::cerr << "[genmap] failed to write " << outPath << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "[genmap] wrote " << outPath << " (noise:" << layer << ", " << pixels
              << "x" << pixels << ", " << step << " blocks/px, seed " << seed << ")\n";
    return EXIT_SUCCESS;
}

// Headless vertical cross-section export (no window/Vulkan). Slices the world along
// the world-X axis through Z=0 (the island centre) and draws a side-on profile:
// surface height, water column, and the soil/stone/snow layering the generator
// places — so the vertical shape (oceans, coasts, hills, snow caps) can be read at a
// glance. Caves/ores live in World's per-voxel pass and are intentionally omitted
// here (this uses only the streaming-safe TerrainGenerator surface model). The image
// is `pixels` wide (world X) by a vertical band sized to the world height.
int runGenCross(const std::string& assetDir, int pixels, int step,
                const std::string& outPath) {
    pixels = std::clamp(pixels, 64, 4096);
    step   = std::max(1, step);
    const vg::WorldConfig cfg = vg::WorldConfig::load(assetDir + "/world.yaml");
    const std::uint32_t seed = 1337u;
    const int worldHeight = cfg.chunksY * 16;
    vg::BlockRegistry reg(assetDir + "/blocks.yaml");
    vg::TerrainGenerator gen(seed, reg, assetDir, worldHeight);
    auto bid = [&](const char* n) -> int {
        try { return static_cast<int>(reg.idByName(n)); } catch (...) { return -1; }
    };
    const int snowId = bid("snow"), sandId = bid("sand"), stoneId = bid("stone");

    const int H = worldHeight; // one image row per world-Y level
    std::vector<unsigned char> img(static_cast<size_t>(pixels) * H * 3);
    const int half = pixels / 2;
    auto put = [&](int px, int row, float r, float g, float b) {
        const size_t o = (static_cast<size_t>(row) * pixels + px) * 3;
        img[o + 0] = static_cast<unsigned char>(std::clamp(r, 0.0f, 255.0f));
        img[o + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 255.0f));
        img[o + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 255.0f));
    };
    for (int px = 0; px < pixels; ++px) {
        const int wx = (px - half) * step;
        const vg::ColumnInfo ci = gen.columnInfo(wx, 0);
        const int h = ci.height, water = ci.waterLevel;
        for (int y = 0; y < H; ++y) {
            const int row = H - 1 - y; // flip so the sky is at the top
            float r, g, b;
            if (y <= h) {
                if (y == h) {                                   // surface block
                    if      (ci.topId == snowId)  { r = 236; g = 241; b = 248; }
                    else if (ci.topId == sandId)  { r = 224; g = 205; b = 150; }
                    else if (ci.topId == stoneId) { r = 130; g = 127; b = 122; }
                    else                          { r = 86;  g = 140; b = 70;  }
                } else if (y >= h - 4) {                          // filler band (dirt/sand)
                    if (ci.fillerId == sandId) { r = 214; g = 196; b = 146; }
                    else                       { r = 120; g = 86;  b = 56;  } // dirt
                } else {                                          // stone interior
                    const float shade = 0.78f + 0.22f * static_cast<float>(y) / static_cast<float>(std::max(1, h));
                    r = 122 * shade; g = 119 * shade; b = 114 * shade;
                }
            } else if (y <= water) {                              // water column
                const float t = std::min(1.0f, static_cast<float>(water - y) / 32.0f);
                r = 56.0f - 36.0f * t; g = 122.0f - 70.0f * t; b = 196.0f - 80.0f * t;
            } else {                                              // sky
                const float t = static_cast<float>(y) / static_cast<float>(H);
                r = 150 + 50 * t; g = 180 + 40 * t; b = 220 + 30 * t;
            }
            put(px, row, r, g, b);
        }
    }
    if (!stbi_write_png(outPath.c_str(), pixels, H, 3, img.data(), pixels * 3)) {
        std::cerr << "[genmap] failed to write " << outPath << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "[genmap] wrote " << outPath << " (cross-section, " << pixels << "x"
              << H << ", " << step << " blocks/px, seed " << seed << ")\n";
    return EXIT_SUCCESS;
}

// Headless game-logic tests (no window/Vulkan). A growing set of assertions over the
// pure data/logic systems (mining time & tools today; crafting, save round-trips,
// etc. as they land) so the parts that DON'T need a GPU stay verifiable in CI.
// Exit 0 = all pass. Run with `voxelgame --logictest`.
int runLogicTest(const std::string& assetDir) {
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::cout << (ok ? "[logic] PASS  " : "[logic] FAIL  ") << what << '\n';
        if (!ok) ++failures;
    };
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };

    try {
        vg::BlockRegistry reg(assetDir + "/blocks.yaml");
        const uint16_t air   = reg.idByName("air");
        const uint16_t stone = reg.idByName("stone");
        const uint16_t dirt  = reg.idByName("dirt");
        const uint16_t bush  = reg.idByName("bush");
        const uint16_t pick  = reg.idByName("pickaxe");
        const uint16_t sword = reg.idByName("sword");

        // Mining time = hardness / (matching-tool speed, else 1).
        check(near(reg.breakSeconds(stone, air), 1.5f),   "stone by hand = 1.5s");
        check(near(reg.breakSeconds(stone, pick), 0.3f),  "stone with pickaxe = 0.3s (5x)");
        check(near(reg.breakSeconds(stone, sword), 1.5f), "sword does NOT speed stone");
        check(near(reg.breakSeconds(dirt, pick), 0.6f),   "pickaxe doesn't speed dirt (no preferred tool)");
        check(near(reg.breakSeconds(bush, air), 0.0f),    "bush breaks instantly");

        // Tool/placement classification.
        check(reg.tool(pick) == vg::ToolKind::Pickaxe, "pickaxe is a Pickaxe tool");
        check(reg.tool(sword) == vg::ToolKind::Sword,  "sword is a Sword tool");
        check(reg.tool(stone) == vg::ToolKind::None,   "stone is not a tool");
        check(reg.placeable(stone),                    "stone is placeable");
        check(!reg.placeable(pick),                    "pickaxe is NOT placeable");
        check(!reg.placeable(sword),                   "sword is NOT placeable");

        // Torch: a placeable, walk-through, light-emitting block.
        const uint16_t torch = reg.idByName("torch");
        check(reg.placeable(torch),                    "torch is placeable");
        check(reg.emission(torch) > 0,                 "torch emits block light");
        check(!reg.isSolid(torch),                     "torch is walk-through");

        // Fall damage: harmless under the safe-fall height, scaling above it.
        check(near(vg::PlayerController::fallDamage(0.0f), 0.0f),  "no fall = no damage");
        check(vg::PlayerController::fallDamage(12.0f) == 0.0f,     "a 2-3 block fall is safe");
        check(vg::PlayerController::fallDamage(30.0f) > 0.0f,      "a big fall hurts");
        check(vg::PlayerController::fallDamage(40.0f) >
              vg::PlayerController::fallDamage(30.0f),             "higher fall = more damage");

        // Health API: damage clamps at 0, heal clamps at max, invulnerability blocks.
        vg::PlayerController pc(glm::vec3(0.0f));
        pc.damage(30.0f);
        check(near(pc.health(), 70.0f), "damage subtracts HP");
        pc.heal(1000.0f);
        check(near(pc.health(), pc.maxHealth()), "heal clamps at max");
        pc.damage(1e9f);
        check(pc.isDead() && pc.health() == 0.0f, "lethal damage -> dead, HP floored at 0");
        pc.setHealth(50.0f);
        pc.setInvulnerable(true);
        pc.damage(50.0f);
        check(near(pc.health(), 50.0f), "invulnerable ignores damage");

        // Swim physics + drowning. Stepping at a fixed 60 Hz, with water reported
        // everywhere, the player should sink far slower than in air, swim up on
        // jump, and drown once breath runs out (verifiable with no Vulkan/world).
        {
            auto stepFor = [](vg::PlayerController& p, const vg::InputState& in, float secs) {
                for (float t = 0.0f; t < secs; t += 1.0f / 60.0f) p.update(1.0f / 60.0f, in);
            };
            const vg::InputState idle{};

            vg::PlayerController dry(glm::vec3(0.0f, 100.0f, 0.0f));
            stepFor(dry, idle, 1.0f);
            const float dryDrop = 100.0f - dry.feetPosition().y; // free-fall reference

            vg::PlayerController wet(glm::vec3(0.0f, 100.0f, 0.0f));
            wet.setWaterFn([](int, int, int) { return true; });
            stepFor(wet, idle, 1.0f);
            const float wetDrop = 100.0f - wet.feetPosition().y;
            check(wetDrop < dryDrop * 0.5f, "buoyancy: sinks far slower in water than air");
            check(wet.inWater(), "player reports submerged in water");

            vg::PlayerController rise(glm::vec3(0.0f, 100.0f, 0.0f));
            rise.setWaterFn([](int, int, int) { return true; });
            vg::InputState jumpIn{}; jumpIn.jump = true;
            stepFor(rise, jumpIn, 0.5f);
            check(rise.feetPosition().y > 100.0f, "swim up: holding jump rises in water");

            vg::PlayerController drown(glm::vec3(0.0f, 100.0f, 0.0f));
            drown.setWaterFn([](int, int, int) { return true; });
            stepFor(drown, idle, drown.maxAir() + 3.0f);
            check(drown.air() == 0.0f, "breath drains to 0 with head underwater");
            check(drown.health() < 100.0f, "drowning deals damage once breath is gone");

            vg::PlayerController airborne(glm::vec3(0.0f, 100.0f, 0.0f));
            airborne.setWaterFn([](int, int, int) { return false; });
            stepFor(airborne, idle, 2.0f);
            check(airborne.air() == airborne.maxAir() && airborne.health() == 100.0f,
                  "out of water: full breath, no drown damage");
        }

        // Player save: serialize -> deserialize round-trips all fields.
        vg::PlayerSave save;
        save.feet = glm::vec3(12.5f, 70.0f, -8.25f);
        save.yaw = 45.0f; save.pitch = -12.0f; save.health = 63.0f;
        save.selected = 3; save.creative = false;
        save.slots = {{stone, 64}, {pick, 1}, {0, 0}, {dirt, 12}};
        const std::vector<uint8_t> bytes = save.serialize();
        vg::PlayerSave back;
        check(back.deserialize(bytes.data(), bytes.size()), "player save parses");
        check(back.feet == save.feet && near(back.yaw, 45.0f) && near(back.health, 63.0f) &&
              back.selected == 3 && back.creative == false && back.slots == save.slots,
              "player save round-trips all fields");
        vg::PlayerSave bad;
        const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        check(!bad.deserialize(junk, sizeof junk), "corrupt player save is rejected");

        // Crafting: recipes load, gate on inputs, and consume/produce correctly.
        vg::Crafting crafting(assetDir + "/recipes.yaml", reg);
        check(!crafting.recipes().empty(), "recipes.yaml loaded some recipes");
        const uint16_t trunk  = reg.idByName("oak_trunk");
        const uint16_t planks = reg.idByName("planks");
        // Find the 1 oak_trunk -> 4 planks recipe.
        const vg::Crafting::Recipe* plankRecipe = nullptr;
        for (const auto& r : crafting.recipes()) {
            if (r.output == planks && r.inputs.size() == 1 && r.inputs[0].first == trunk) {
                plankRecipe = &r; break;
            }
        }
        check(plankRecipe != nullptr, "found oak_trunk -> planks recipe");
        if (plankRecipe) {
            vg::Inventory inv2;
            check(crafting.craftable(inv2).empty(), "empty inventory crafts nothing");
            check(!vg::Crafting::canCraft(*plankRecipe, inv2), "can't craft planks with no wood");
            inv2.add(trunk, 2);
            check(vg::Crafting::canCraft(*plankRecipe, inv2), "2 trunks -> planks is craftable");
            check(vg::Crafting::craft(*plankRecipe, inv2), "craft consumes input, yields output");
            check(inv2.count(trunk) == 1, "one trunk consumed");
            check(inv2.count(planks) == 4, "four planks produced");
        }

        // Chest store: holds per-position contents and round-trips through disk bytes.
        vg::ChestStore store;
        const glm::ivec3 pa{10, 64, -20}, pb{-5, 70, 3};
        store.at(pa)[0] = vg::ItemStack{stone, 40};
        store.at(pa)[5] = vg::ItemStack{planks, 12};
        store.at(pb)[26] = vg::ItemStack{pick, 1};
        check(store.has(pa) && store.has(pb), "chests recorded at two positions");
        const std::vector<uint8_t> cbytes = store.serialize();
        vg::ChestStore loaded;
        check(loaded.deserialize(cbytes.data(), cbytes.size()), "chest store parses");
        check(loaded.has(pa) && loaded.at(pa)[0].blockId == stone &&
              loaded.at(pa)[0].count == 40 && loaded.at(pa)[5].blockId == planks,
              "chest A contents round-trip");
        check(loaded.at(pb)[26].blockId == pick, "chest B contents round-trip");
        store.erase(pa);
        check(!store.has(pa), "erased chest is gone");
        vg::ChestStore cbad;
        const uint8_t cjunk[6] = {9, 9, 9, 9, 9, 9};
        check(!cbad.deserialize(cjunk, sizeof cjunk), "corrupt chest store is rejected");

        // Equipment: slot validation + aggregated stats + armour damage reduction.
        const uint16_t helmet = reg.idByName("iron_helmet");
        const uint16_t chestplate = reg.idByName("iron_chestplate");
        const uint16_t swift = reg.idByName("swift_charm");
        const uint16_t life = reg.idByName("life_charm");
        check(vg::Equipment::accepts(0) == vg::EquipSlot::Head, "slot 0 is the head slot");
        check(vg::Equipment::fits(reg, 0, helmet), "helmet fits the head slot");
        check(!vg::Equipment::fits(reg, 0, swift), "a trinket doesn't fit the head slot");
        check(vg::Equipment::fits(reg, 4, swift), "trinket fits a trinket slot");
        check(!vg::Equipment::fits(reg, 1, helmet), "helmet doesn't fit the chest slot");

        vg::Equipment eq;
        eq.slots[0] = vg::ItemStack{helmet, 1};       // 8 armour
        eq.slots[1] = vg::ItemStack{chestplate, 1};   // 16 armour -> 24% reduction
        eq.slots[4] = vg::ItemStack{swift, 1};        // x1.35 speed
        eq.slots[5] = vg::ItemStack{life, 1};         // +3 regen
        const vg::Equipment::Stats st = eq.computeStats(reg);
        check(near(st.armorReduction, 0.24f), "8+16 armour = 24% reduction");
        check(near(st.speedMul, 1.35f), "swift charm = 1.35x speed");
        check(near(st.regenBonus, 3.0f), "life charm = +3 regen");

        vg::PlayerController pc2(glm::vec3(0.0f));
        pc2.setEquipModifiers(0.25f, 1.0f, 1.0f, 0.0f);
        pc2.damage(40.0f);
        check(near(pc2.health(), 70.0f), "25% armour: 40 damage -> 30 taken");

        // Dropped items: gravity + ground settle, then magnet pickup into inventory.
        auto floorAt0 = [](int, int y, int) { return y < 0; }; // solid everywhere below y=0
        {
            vg::ItemEntities items;
            vg::Inventory inv3;
            items.spawn(glm::vec3(0.5f, 5.0f, 0.5f), vg::ItemStack{stone, 3});
            // Player far away: it should fall and settle on the floor, not be collected.
            for (int s = 0; s < 240; ++s) items.update(0.05f, floorAt0, glm::vec3(40, 0, 40), inv3);
            check(items.size() == 1, "far item is not picked up");
            check(items.items()[0].pos.y > 0.0f && items.items()[0].pos.y < 0.5f,
                  "item settles on the ground");
            check(inv3.count(stone) == 0, "far item left the inventory empty");
        }
        {
            vg::ItemEntities items;
            vg::Inventory inv4;
            items.spawn(glm::vec3(0.0f, 0.9f, 0.0f), vg::ItemStack{stone, 5});
            items.update(0.05f, floorAt0, glm::vec3(0, 0, 0), inv4); // age 0.05 < delay
            check(items.size() == 1 && inv4.count(stone) == 0, "no pickup before the delay");
            for (int s = 0; s < 40; ++s) items.update(0.05f, floorAt0, glm::vec3(0, 0, 0), inv4);
            check(items.size() == 0 && inv4.count(stone) == 5, "nearby item is collected");
        }
    } catch (const std::exception& e) {
        std::cerr << "[logic] FAIL: exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "[logic] " << (failures == 0 ? "ALL PASS" : "FAILED")
              << " (" << failures << " failure(s))\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

// Entry point. Everything interesting lives in vg::App; main() parses a few
// optional flags, runs it, and turns any uncaught exception into a clean
// non-zero exit + error message.
//
// Flags:
//   --frames N        Run N frames then exit (headless smoke-testing / CI).
//   --screenshot PATH Render some frames, write PATH as a PNG, then exit.
//   --flycam          Start in free-fly looking down over the whole world
//                     (a bird's-eye view of the procedural terrain).
//   --selftest        Run the headless world-generation determinism/golden test
//                     and exit (no window). Exit code 0 = pass.
//   --genmap          Headless map export (no window). Sub-modes via --mode:
//                       top   (default) surface map coloured by block + hillshade
//                       noise raw noise layer (--layer cont|ero|peak|temp|hum|
//                             river|relief) as a diverging blue/white/red field
//                       cross vertical cross-section through Z=0 (terrain profile,
//                             water, soil/stone/snow layers)
//                     Sizing: --mapsize N (px), --mapstep B (blocks/px), --out PATH.
int main(int argc, char** argv) {
    long maxFrames = -1; // run until the window is closed
    bool framesSet = false;
    std::string screenshotPath;
    bool flycam = false;
    bool genMap = false;
    long mapPixels = 768, mapStep = 6;
    std::string mapOut = "genmap.png";
    std::string mapMode = "top";     // top | noise | cross
    std::string mapLayer = "cont";   // which noise layer for --mode noise

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtol(argv[++i], nullptr, 10);
            framesSet = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[++i];
        } else if (std::strcmp(argv[i], "--flycam") == 0) {
            flycam = true;
        } else if (std::strcmp(argv[i], "--selftest") == 0) {
            // Headless worldgen determinism/golden test — no window/Vulkan.
            return runWorldGenSelfTest(VG_ASSET_DIR);
        } else if (std::strcmp(argv[i], "--logictest") == 0) {
            // Headless game-logic tests (mining/tools/…) — no window/Vulkan.
            return runLogicTest(VG_ASSET_DIR);
        } else if (std::strcmp(argv[i], "--genmap") == 0) {
            genMap = true; // headless top-down map (run after all flags are parsed)
        } else if (std::strcmp(argv[i], "--mapsize") == 0 && i + 1 < argc) {
            mapPixels = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--mapstep") == 0 && i + 1 < argc) {
            mapStep = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            mapOut = argv[++i];
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mapMode = argv[++i]; // top | noise | cross (with --genmap)
        } else if (std::strcmp(argv[i], "--layer") == 0 && i + 1 < argc) {
            mapLayer = argv[++i]; // noise layer for --mode noise
        } else {
            std::cerr << "Unknown argument: " << argv[i] << '\n';
            return EXIT_FAILURE;
        }
    }

    if (genMap) {
        const int px = static_cast<int>(mapPixels), st = static_cast<int>(mapStep);
        if (mapMode == "noise") return runGenNoise(VG_ASSET_DIR, px, st, mapOut, mapLayer);
        if (mapMode == "cross") return runGenCross(VG_ASSET_DIR, px, st, mapOut);
        if (mapMode != "top") {
            std::cerr << "Unknown --mode '" << mapMode << "' (use top|noise|cross)\n";
            return EXIT_FAILURE;
        }
        return runGenMap(VG_ASSET_DIR, px, st, mapOut);
    }

    // When capturing a screenshot without an explicit frame count, pick a
    // sensible default: a static fly-cam needs only a couple of frames, while a
    // walking spawn needs time for gravity to settle the player.
    if (!screenshotPath.empty() && !framesSet) {
        maxFrames = flycam ? 3 : 120;
    }

    try {
        vg::App app;
        if (flycam) {
            app.enableFlyOverview();
        }
        app.run(maxFrames, screenshotPath);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
