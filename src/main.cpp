#include "core/App.h"
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
