#include "core/App.h"
#include "core/Input.h"
#include "entity/Armature.h"
#include "entity/BlockbenchModel.h"
#include "entity/ItemEntity.h"
#include "player/ChestStore.h"
#include "player/Crafting.h"
#include "player/Equipment.h"
#include "player/PlayerController.h"
#include "player/PlayerSave.h"

#include "world/BlockRegistry.h"
#include "utilities/hash/Hash.h"
#include "utilities/noise/Noise.h"
#include "utilities/noise/NoiseStack.h"
#include "world/World.h"
#include "world/WorldConfig.h"

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VG_ASSET_DIR
#define VG_ASSET_DIR "assets"
#endif

namespace {

// FNV-1a over 8 bytes â€” a cheap, stable spatial hash for snapshot testing.
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
// (independent of assets/world.yaml) so the result is config-stable, generates the
// world twice, and checks that regeneration is bit-identical (h1 == h2) within the
// process.
//
// NO GOLDEN (REVIEW R10). A recorded golden hash used to live here too, but it was
// dead weight: (1) it goes stale every time biomes.yaml / the splines are tuned, and
// (2) worldgen is cross-PROCESS non-deterministic — the hash can vary run-to-run
// even though the in-process h1 == h2 holds — so a golden can never pass reliably and
// everyone learned to ignore the failure, eroding the test's authority. The within-
// process regen check is the meaningful invariant (worldgen is a pure function of
// (seed, coords) for a given build), so that is all this asserts. The hash is still
// printed for manual A/B comparison across builds in one environment. If the cross-
// process non-determinism is ever hunted down (it also implies saved worlds may not
// regenerate identically), a golden can be reinstated then — but as its own session.
int runWorldGenSelfTest(const std::string& assetDir) {
    vg::WorldConfig cfg;
    cfg.seed          = 1337u;
    cfg.streaming     = false; // fixed grid: no threads, no disk, fully deterministic
    cfg.streamWorkers = 0;
    cfg.viewRadius    = 4;      // 9x9 chunks
    cfg.heightChunks  = 16;     // 256 tall: sea level 128 in the middle (biome pipeline)
    cfg.chunksX = cfg.chunksZ = 2 * cfg.viewRadius + 1;
    cfg.chunksY = cfg.heightChunks;
    // Everything else keeps the documented WorldConfig.h defaults (stable).

    const std::string blocks = assetDir + "/blocks.yaml";

    uint64_t h1 = 0, h2 = 0;
    try {
        h1 = hashWorld(vg::World(cfg, blocks));
        h2 = hashWorld(vg::World(cfg, blocks));
    } catch (const std::exception& e) {
        std::cerr << "[selftest] FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "[selftest] worldgen hash = 0x" << std::hex << h1 << std::dec
              << " (in-process regen check only; no golden — REVIEW R10)\n";
    if (h1 != h2) {
        std::cerr << "[selftest] FAIL: regeneration is non-deterministic (0x" << std::hex
                  << h1 << " vs 0x" << h2 << std::dec << ")\n";
        std::cout << "[selftest] FAIL\n";
        return EXIT_FAILURE;
    }
    std::cout << "[selftest] PASS\n";
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
        // ---- NoiseStack Layer 0 extensions (docs/WORLDGEN.md) ------------------
        // Worley/cellular, ridged-multifractal, domain warp and the transfer stage.
        // The hard requirements: every new primitive is (a) a pure function of
        // (seed, coords) — same input → bit-identical output, including across two
        // separately-built stacks (the property that makes streaming seam-safe) —
        // and (b) bounded to ~[-1,1] like fbm so it drops into a weighted blend.
        {
            using NS = vg::NoiseStack;
            // Sample points spanning several lattice cells, incl. negatives and the
            // origin (Perlin's lattice-zero, a classic flatness trap).
            const float pts[][3] = {
                {0.0f, 0.0f, 0.0f}, {13.5f, 0.0f, -7.25f}, {-128.0f, 40.0f, 256.0f},
                {1000.3f, 12.0f, -333.7f}, {-4096.0f, 70.0f, 4096.0f}, {17.0f, 5.0f, 17.0f},
            };
            auto inRange = [](float v) { return v >= -1.0001f && v <= 1.0001f; };

            // Build a stack twice from the same seed; assert the two are identical
            // and in-range at every point (2D and 3D). `build` parameterises the one
            // layer under test so each primitive gets the same treatment.
            auto deterministicAndBounded = [&](const char* label, const NS::Layer& layer) {
                NS a, b;
                a.addLayer(layer, 0xC0FFEEu);
                b.addLayer(layer, 0xC0FFEEu);
                bool det = true, bounded = true;
                for (const auto& p : pts) {
                    const float a2 = a.value(p[0], p[2]), b2 = b.value(p[0], p[2]);
                    const float a3 = a.value(p[0], p[1], p[2]);
                    const float b3 = b.value(p[0], p[1], p[2]);
                    if (a2 != b2 || a3 != b3) det = false;             // exact, not approx
                    if (a.value(p[0], p[2]) != a2) det = false;        // stable on re-sample
                    if (!inRange(a2) || !inRange(a3)) bounded = false;
                }
                check(det, std::string(label) + ": pure (bit-identical across builds)");
                check(bounded, std::string(label) + ": output bounded to ~[-1,1]");
            };

            NS::Layer worley; worley.type = NS::Type::Worley; worley.frequency = 0.02f;
            worley.cell = vg::Noise::Cell::F2mF1; worley.metric = vg::Noise::Metric::Manhattan;
            deterministicAndBounded("worley F2-F1/manhattan", worley);

            NS::Layer rmf; rmf.type = NS::Type::Ridged; rmf.multifractal = true;
            rmf.octaves = 5; rmf.frequency = 0.01f; rmf.mfOffset = 1.0f; rmf.mfGain = 2.0f;
            deterministicAndBounded("ridged multifractal", rmf);

            NS::Layer hmf; hmf.type = NS::Type::Perlin; hmf.multifractal = true;
            hmf.octaves = 5; hmf.frequency = 0.01f; hmf.mfOffset = 0.7f;
            deterministicAndBounded("hybrid multifractal", hmf);

            NS::Layer warped; warped.type = NS::Type::Perlin; warped.frequency = 0.01f;
            warped.warpAmp = 60.0f; warped.warpFreq = 0.004f;
            deterministicAndBounded("domain-warped perlin", warped);

            // Domain warp must actually displace the field (not silently no-op): a
            // warped layer differs from the same layer unwarped at most sample points.
            {
                NS plain, warp;
                NS::Layer base; base.type = NS::Type::Perlin; base.frequency = 0.01f;
                plain.addLayer(base, 7u);
                NS::Layer w = base; w.warpAmp = 80.0f; w.warpFreq = 0.003f;
                warp.addLayer(w, 7u);
                int differ = 0;
                for (const auto& p : pts)
                    if (plain.value(p[0], p[2]) != warp.value(p[0], p[2])) ++differ;
                check(differ >= 4, "domain warp changes the field (not a no-op)");
            }

            // Identity transfer (redistribution 1, no terrace) is a true no-op, so a
            // stack that sets it matches one that does not — the byte-identical guard.
            {
                NS plain, ident;
                NS::Layer base; base.type = NS::Type::Ridged; base.frequency = 0.013f;
                base.octaves = 4;
                plain.addLayer(base, 99u);
                ident.addLayer(base, 99u);
                ident.setRedistribution(1.0f);
                ident.setTerrace(0, 1.0f);
                bool same = true;
                for (const auto& p : pts)
                    if (plain.value(p[0], p[2]) != ident.value(p[0], p[2])) same = false;
                check(same, "identity transfer is byte-identical (no regression)");
            }

            // Terracing quantises the field: a strongly terraced stack collapses a
            // dense sweep into a small number of distinct output levels.
            {
                NS terr;
                NS::Layer base; base.type = NS::Type::Perlin; base.frequency = 0.005f;
                terr.addLayer(base, 314u);
                terr.setTerrace(4, 1000.0f); // 4 crisp steps (near-vertical risers)
                std::vector<float> seen;
                bool bounded = true;
                int onPlateau = 0;
                const int N = 400;
                for (int i = 0; i < N; ++i) {
                    const float v = terr.value(static_cast<float>(i) * 3.0f, 0.0f);
                    if (v < -1.0001f || v > 1.0001f) bounded = false;
                    // Plateaus sit at 2*(k/4)-1 for integer k in [0,4]: {-1,-.5,0,.5,1}.
                    for (int k = 0; k <= 4; ++k)
                        if (std::fabs(v - (2.0f * (k / 4.0f) - 1.0f)) < 1e-3f) { ++onPlateau; break; }
                    bool found = false;
                    for (float s : seen) if (std::fabs(s - v) < 1e-4f) { found = true; break; }
                    if (!found) seen.push_back(v);
                }
                check(bounded, "terraced stack stays bounded");
                check(onPlateau >= N - 10,
                      "crisp terracing snaps the field onto discrete plateaus");
            }

            // Eroded layer (derivative-attenuated fbm) is pure + bounded.
            NS::Layer eroded; eroded.type = NS::Type::Perlin; eroded.erodeDetail = true;
            eroded.octaves = 6; eroded.frequency = 0.02f;
            deterministicAndBounded("derivative-attenuated fbm", eroded);
        }

        // ---- Analytic-gradient Perlin (docs/WORLDGEN.md Layer 0 / §4) ----------
        // The derivative-returning perlin() must (a) return the SAME value as the
        // scalar perlin() and (b) return a gradient that matches central finite
        // differences — the property the fake-erosion / slope code relies on.
        {
            vg::Noise n(0xBADCAB1Eu);
            const float h = 0.001f;
            bool valueMatch = true, gradMatch = true;
            const float pts[][2] = {
                {0.37f, 1.91f}, {12.4f, -8.13f}, {-3.27f, 5.55f}, {101.1f, 202.2f},
                {0.5f, 0.5f}, {-44.44f, 0.123f},
            };
            for (const auto& p : pts) {
                float dx = 0.0f, dy = 0.0f;
                const float v = n.perlin(p[0], p[1], dx, dy);
                if (v != n.perlin(p[0], p[1])) valueMatch = false; // bit-identical value
                const float cdx = (n.perlin(p[0] + h, p[1]) - n.perlin(p[0] - h, p[1])) / (2 * h);
                const float cdy = (n.perlin(p[0], p[1] + h) - n.perlin(p[0], p[1] - h)) / (2 * h);
                if (std::fabs(dx - cdx) > 5e-3f || std::fabs(dy - cdy) > 5e-3f) gradMatch = false;
            }
            check(valueMatch, "derivative perlin() value == scalar perlin()");
            check(gradMatch, "analytic gradient matches central differences");
        }

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

        // Foliage shadows: light_opacity is decoupled from `opaque`, so thin,
        // non-opaque foliage still dims the sky passing through it (a solid cube
        // blocks fully, ground/air passes light). See World::computeSkyLight.
        const uint16_t oakLeaves = reg.idByName("oak_leaves");
        const uint16_t oakTrunk  = reg.idByName("oak_trunk");
        check(reg.lightOpacity(stone) == 15,     "stone blocks sky light fully (opaque default)");
        check(reg.lightOpacity(air) == 0,        "air passes sky light");
        check(reg.lightOpacity(oakLeaves) == 3,  "leaves dim the sky a little");
        check(reg.lightOpacity(oakTrunk) == 4,   "trunk dims the sky below it");
        check(reg.lightOpacity(bush) == 2,       "bush casts a faint shadow");
        check(!reg.isOpaque(oakTrunk) && reg.lightOpacity(oakTrunk) > 0,
              "trunk shadows the sky yet stays non-opaque (decoupled from meshing)");
        check(!reg.isOpaque(oakLeaves) && reg.lightOpacity(oakLeaves) > 0,
              "leaves shadow the sky yet stay non-opaque (decoupled from meshing)");

        // Tool-tier harvest gating (ISSUES #13I): a block drops only with a matching
        // tool of high enough tier; below tier it still breaks but yields nothing.
        const uint16_t woodPick  = reg.idByName("wood_pickaxe");
        const uint16_t stonePick = reg.idByName("stone_pickaxe");
        check(reg.canHarvest(dirt, air),          "dirt drops by hand (no tier gate)");
        check(!reg.canHarvest(stone, air),        "stone needs a pickaxe (no bare-hand drop)");
        check(reg.canHarvest(stone, woodPick),    "wood pickaxe harvests stone");
        check(reg.canHarvest(stone, stonePick),   "stone pickaxe harvests stone");
        check(reg.toolTier(stonePick) == 2,       "stone pickaxe is tier 2");
        check(reg.toolTier(air) == 0,             "bare hand is tier 0");

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

            // Sneak: slower than a normal walk + a lowered camera.
            auto floorAll = [](int, int y, int) { return y < 100; };
            vg::InputState fwd{};  fwd.move = {0.0f, 1.0f};
            vg::InputState sneakFwd = fwd; sneakFwd.sneak = true;
            vg::PlayerController walk(glm::vec3(0.5f, 100.0f, 0.5f));
            walk.setSolidFn(floorAll);
            stepFor(walk, fwd, 0.5f);
            const glm::vec3 walkP = walk.feetPosition();
            vg::PlayerController crouch(glm::vec3(0.5f, 100.0f, 0.5f));
            crouch.setSolidFn(floorAll);
            stepFor(crouch, sneakFwd, 0.5f);
            const glm::vec3 crouchP = crouch.feetPosition();
            const float walkDist = glm::length(glm::vec2(walkP.x - 0.5f, walkP.z - 0.5f));
            const float crouchDist = glm::length(glm::vec2(crouchP.x - 0.5f, crouchP.z - 0.5f));
            check(crouchDist < walkDist * 0.5f, "sneak walks much slower than a normal walk");
            check(crouch.sneaking(), "sneak flag set while holding the sneak key");
            check(crouch.camera().position.y < walk.camera().position.y - 0.2f,
                  "sneak lowers the camera (crouch)");

            // Edge-stop: on a single 1x1 pillar, a normal walk strides off and falls;
            // sneaking refuses the step that would drop the player.
            auto pillar = [](int x, int y, int z) { return x == 0 && z == 0 && y < 100; };
            vg::InputState diag{}; diag.move = {1.0f, 1.0f};
            vg::InputState sneakDiag = diag; sneakDiag.sneak = true;
            vg::PlayerController fell(glm::vec3(0.5f, 100.0f, 0.5f));
            fell.setSolidFn(pillar);
            stepFor(fell, diag, 1.0f);
            vg::PlayerController held(glm::vec3(0.5f, 100.0f, 0.5f));
            held.setSolidFn(pillar);
            stepFor(held, sneakDiag, 1.0f);
            check(fell.feetPosition().y < 99.0f, "walking off a pillar falls");
            check(held.feetPosition().y > 99.5f, "sneak edge-stop keeps the player on the pillar");
        }

        // Box-rig + animation core (ISSUES #13E) â€” pure math, no Vulkan.
        {
            auto worldPos = [](const glm::mat4& m) {
                return glm::vec3(m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            };
            auto vnear = [](const glm::vec3& a, const glm::vec3& b) {
                return glm::length(a - b) < 1e-3f;
            };
            vg::Skeleton skel;
            skel.joints.push_back({"root", -1, glm::vec3(0.0f),
                                   glm::quat(1, 0, 0, 0), glm::vec3(1.0f)});
            skel.joints.push_back({"arm", 0, glm::vec3(0.0f, 1.0f, 0.0f),
                                   glm::quat(1, 0, 0, 0), glm::vec3(1.0f)});
            check(skel.isTopologicallyOrdered(), "skeleton is parent-before-child ordered");
            check(skel.find("arm") == 1, "joint lookup by name");

            const auto rest = vg::worldMatrices(skel, vg::restPose(skel));
            check(vnear(worldPos(rest[1]), glm::vec3(0.0f, 1.0f, 0.0f)),
                  "rest pose places child above root");

            // Rotate the root +90deg about Z: the arm tip swings from +Y to -X.
            vg::AnimationClip spin;
            spin.name = "spin"; spin.duration = 1.0f; spin.loop = false;
            vg::AnimChannel ch; ch.joint = 0; ch.times = {0.0f, 1.0f};
            ch.rotations = {glm::quat(1, 0, 0, 0),
                            glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1))};
            spin.channels.push_back(ch);
            const auto endP = vg::worldMatrices(skel, vg::sampleClip(skel, spin, 1.0f));
            check(vnear(worldPos(endP[1]), glm::vec3(-1.0f, 0.0f, 0.0f)),
                  "90deg root spin swings child to -X");

            const auto midP = vg::worldMatrices(skel, vg::sampleClip(skel, spin, 0.5f));
            const float s = std::sqrt(0.5f);
            check(vnear(worldPos(midP[1]), glm::vec3(-s, s, 0.0f)), "slerp midpoint = 45deg");

            // Looping wraps the sample time (t=1.5 on a 1s loop -> t=0.5).
            vg::AnimationClip slide;
            slide.name = "slide"; slide.duration = 1.0f; slide.loop = true;
            vg::AnimChannel tch; tch.joint = 0; tch.times = {0.0f, 1.0f};
            tch.translations = {glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f)};
            slide.channels.push_back(tch);
            const auto looped = vg::worldMatrices(skel, vg::sampleClip(skel, slide, 1.5f));
            check(vnear(worldPos(looped[0]), glm::vec3(5.0f, 0.0f, 0.0f)),
                  "loop wraps t=1.5 -> t=0.5");

            // A rotation-only channel leaves translation at its rest value.
            const auto restT = vg::worldMatrices(skel, vg::sampleClip(skel, spin, 0.0f));
            check(vnear(worldPos(restT[1]), glm::vec3(0.0f, 1.0f, 0.0f)),
                  "unanimated channels keep their rest transform");

            // Bake: one box -> 36 verts, all within its bounds, with unit normals.
            vg::Skeleton bx;
            bx.joints.push_back({"root", -1, glm::vec3(0.0f),
                                 glm::quat(1, 0, 0, 0), glm::vec3(1.0f)});
            vg::Box box; box.joint = 0; box.min = glm::vec3(-1.0f); box.max = glm::vec3(1.0f);
            bx.boxes.push_back(box);
            const auto verts = vg::bakeMesh(bx, vg::worldMatrices(bx, vg::restPose(bx)));
            check(verts.size() == 36, "one box bakes to 36 vertices");
            bool inBounds = true, unitN = true;
            for (const auto& vv : verts) {
                if (std::fabs(vv.pos.x) > 1.001f || std::fabs(vv.pos.y) > 1.001f ||
                    std::fabs(vv.pos.z) > 1.001f) inBounds = false;
                if (std::fabs(glm::length(vv.normal) - 1.0f) > 1e-3f) unitN = false;
            }
            check(inBounds, "baked box verts lie within the box bounds");
            check(unitN, "baked normals are unit length");

            // A translated joint carries its baked geometry along.
            bx.joints[0].restT = glm::vec3(5.0f, 0.0f, 0.0f);
            const auto moved = vg::bakeMesh(bx, vg::worldMatrices(bx, vg::restPose(bx)));
            bool shifted = true;
            for (const auto& vv : moved)
                if (vv.pos.x < 3.999f || vv.pos.x > 6.001f) shifted = false;
            check(shifted, "translating a joint moves its baked box");
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

        // Equipment (ISSUES #15: armour trimmed to a single boots slot + trinkets).
        const uint16_t boots = reg.idByName("iron_boots");
        const uint16_t swift = reg.idByName("swift_charm");
        const uint16_t life = reg.idByName("life_charm");
        check(vg::Equipment::accepts(0) == vg::EquipSlot::Feet,    "slot 0 is the boots slot");
        check(vg::Equipment::accepts(1) == vg::EquipSlot::Trinket, "slot 1 is a trinket slot");
        check(vg::Equipment::fits(reg, 0, boots),  "boots fit the boots slot");
        check(!vg::Equipment::fits(reg, 0, swift), "a trinket doesn't fit the boots slot");
        check(vg::Equipment::fits(reg, 1, swift),  "trinket fits a trinket slot");
        check(!vg::Equipment::fits(reg, 1, boots), "boots don't fit a trinket slot");

        vg::Equipment eq;
        eq.slots[0] = vg::ItemStack{boots, 1};        // 6 armour -> 6% reduction
        eq.slots[1] = vg::ItemStack{swift, 1};        // x1.35 speed
        eq.slots[2] = vg::ItemStack{life, 1};         // +3 regen
        const vg::Equipment::Stats st = eq.computeStats(reg);
        check(near(st.armorReduction, 0.06f), "6 armour = 6% reduction");
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

        // Block shapes (ISSUES #16): shapeable flags, metadata pack/unpack, the box
        // union shared by mesher + collision, and that the player stands on a slab.
        {
            const uint16_t cobble = reg.idByName("cobblestone");
            const uint16_t hammer = reg.idByName("hammer");
            check(reg.shapeable(stone),   "stone (solid opaque cube) is shapeable");
            check(reg.shapeable(cobble),  "cobblestone is shapeable");
            check(!reg.shapeable(bush),   "foliage is NOT shapeable");
            check(!reg.placeable(hammer), "hammer is NOT placeable (a held tool)");

            // metadata pack/unpack round-trip + default (0) == Cube.
            check(vg::shapeKindOf(0) == vg::ShapeKind::Cube, "metadata 0 decodes to Cube");
            const uint8_t m = vg::packShape(vg::ShapeKind::Stairs, 5);
            check(vg::shapeKindOf(m) == vg::ShapeKind::Stairs && vg::shapeOrientOf(m) == 5,
                  "shape+orient packs and unpacks");

            vg::ShapeBox bx[vg::kMaxShapeBoxes];
            check(vg::shapeBoxes(vg::ShapeKind::Cube, 0, 0, bx) == 1 &&
                  near(bx[0].lo.y, 0.0f) && near(bx[0].hi.y, 1.0f), "cube = one full box");
            vg::shapeBoxes(vg::ShapeKind::Slab, 0, 0, bx);  // bottom
            check(near(bx[0].hi.y, 0.5f) && near(bx[0].lo.y, 0.0f), "bottom slab = lower half");
            vg::shapeBoxes(vg::ShapeKind::Slab, 1, 0, bx);  // top
            check(near(bx[0].lo.y, 0.5f) && near(bx[0].hi.y, 1.0f), "top slab = upper half");
            check(vg::shapeBoxes(vg::ShapeKind::Stairs, 0, 0, bx) == 2, "stairs = two boxes");
            vg::shapeBoxes(vg::ShapeKind::Post, 1, 0, bx);  // Y axis
            check(bx[0].lo.x > 0.0f && bx[0].hi.x < 1.0f && near(bx[0].lo.y, 0.0f) &&
                  near(bx[0].hi.y, 1.0f), "Y post = centred full-height column");
            check(vg::shapeBoxes(vg::ShapeKind::Wall, 0, 0, bx) == 1, "wall, no neighbours = post only");
            check(vg::shapeBoxes(vg::ShapeKind::Wall, 0, 0xF, bx) == 5,
                  "wall with four connections = post + 4 arms");

            // Collision: a floor of bottom-slabs at y=0 stops the player at y=0.5,
            // half a block lower than a full-cube floor would.
            auto stepFor = [](vg::PlayerController& p, const vg::InputState& in, float secs) {
                for (float t = 0.0f; t < secs; t += 1.0f / 60.0f) p.update(1.0f / 60.0f, in);
            };
            const vg::InputState idle{};
            vg::PlayerController onSlab(glm::vec3(0.5f, 5.0f, 0.5f));
            onSlab.setSolidFn([](int, int y, int) { return y <= 0; });
            onSlab.setCollisionBoxesFn([](int x, int y, int z, vg::ShapeBox out[]) {
                if (y > 0) return 0;
                out[0] = {glm::vec3(x, y, z), glm::vec3(x + 1, y + 0.5f, z + 1)};
                return 1;
            });
            stepFor(onSlab, idle, 2.0f);
            check(near(onSlab.feetPosition().y, 0.5f), "player rests on a bottom-slab floor at y=0.5");

            // Auto-step: full cubes below y=0, and the whole y=0 layer is bottom-slabs
            // except the start cell. Walking forward bumps a slab side and should climb
            // onto it (~y=0.5) instead of stopping dead.
            auto climb_solid = [](int x, int y, int z) {
                return y < 0 || (y == 0 && !(x == 0 && z == 0));
            };
            auto climb_boxes = [](int x, int y, int z, vg::ShapeBox out[]) {
                if (y < 0) { out[0] = {glm::vec3(x, y, z), glm::vec3(x + 1, y + 1, z + 1)}; return 1; }
                if (y == 0 && !(x == 0 && z == 0)) {
                    out[0] = {glm::vec3(x, 0, z), glm::vec3(x + 1, 0.5f, z + 1)};
                    return 1;
                }
                return 0;
            };
            vg::PlayerController climb(glm::vec3(0.5f, 0.0f, 0.5f));
            climb.setSolidFn(climb_solid);
            climb.setCollisionBoxesFn(climb_boxes);
            vg::InputState walkFwd{}; walkFwd.move = {0.0f, 1.0f};
            stepFor(climb, walkFwd, 1.0f);
            check(climb.feetPosition().y > 0.4f, "auto-step climbs onto a half-slab while walking");

            // Camera step-smoothing: the body pops up a slab in one frame, but the
            // *rendered* eye should ease up over a few frames (not snap). Re-run the
            // climb frame-by-frame (view-bob off so it doesn't perturb the eye), and
            // the frame the feet jump, the camera must still lag the true eye height â€”
            // then converge to it shortly after.
            vg::PlayerController smooth(glm::vec3(0.5f, 0.0f, 0.5f));
            smooth.setSolidFn(climb_solid);
            smooth.setCollisionBoxesFn(climb_boxes);
            smooth.setViewBob(false);
            stepFor(smooth, idle, 0.2f); // settle on the ground first
            float camAtStep = smooth.camera().position.y, feetAtStep = 0.0f;
            for (int i = 0; i < 180; ++i) {
                const float yPrev = smooth.feetPosition().y;
                smooth.update(1.0f / 60.0f, walkFwd);
                if (smooth.feetPosition().y > yPrev + 0.2f) { // the step happened this frame
                    camAtStep  = smooth.camera().position.y;
                    feetAtStep = smooth.feetPosition().y;
                    break;
                }
            }
            check(camAtStep < feetAtStep + 1.62f - 0.05f,
                  "camera eases over an auto-step (lags the instant body pop)");
            stepFor(smooth, walkFwd, 1.5f); // generous: tolerant of soft/slow ease rates
            check(std::fabs(smooth.camera().position.y -
                            (smooth.feetPosition().y + 1.62f)) < 0.02f,
                  "stepped camera converges to true eye height");
        }

        // ---- Foliage shadows survive edits (relight == gen-time flood) ---------
        // computeSkyLight dims sky light through foliage (light_opacity), so canopies
        // shade the ground. The incremental edit-time relight (relightField) must use
        // the SAME model, or breaking a block near a tree would relight its box WITHOUT
        // the canopy shadow (the reported bug). A NO-OP edit (set a block to itself)
        // reruns relightField over its box; if the two models agree, the sky-light
        // field is byte-identical before and after. (A small fixed headless world,
        // origin {0,0,0} like --selftest.)
        {
            vg::WorldConfig wcfg;
            wcfg.seed = 1337u;
            wcfg.streaming = false;
            wcfg.streamWorkers = 0;
            wcfg.viewRadius = 4;
            wcfg.heightChunks = 16;
            wcfg.chunksX = wcfg.chunksZ = 2 * wcfg.viewRadius + 1;
            wcfg.chunksY = wcfg.heightChunks;
            vg::World w(wcfg, assetDir + "/blocks.yaml");
            const glm::ivec3 sz = w.sizeInBlocks();

            // The flat world grows no foliage, so build a small leaf canopy ourselves
            // a few blocks above the surface — its light_opacity casts the partial sky
            // shadow this test needs to exercise the edit-relight path.
            {
                const uint16_t leaf = w.registry().idByName("oak_leaves");
                const int cxp = sz.x / 2, czp = sz.z / 2;
                const int surf = w.surfaceHeight(cxp, czp);
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dz = -1; dz <= 1; ++dz)
                        w.setBlock(cxp + dx, surf + 3, czp + dz, vg::Block{leaf, 0});
            }

            // Any leaves species counts as a canopy to centre the edit on.
            std::vector<uint16_t> leafIds;
            for (const char* n : {"oak_leaves", "birch_leaves", "pine_leaves"}) {
                try { leafIds.push_back(w.registry().idByName(n)); } catch (...) {}
            }
            auto isLeaf = [&](uint16_t id) {
                for (uint16_t l : leafIds) if (id == l) return true;
                return false;
            };

            // Centre the relight box on a canopy if one grew in-window; else on any
            // partial-shade cell (1..14) â€” a foliage/relief shadow the edit must keep.
            glm::ivec3 c{sz.x / 2, sz.y / 2, sz.z / 2};
            bool foundLeaves = false, partialShade = false;
            for (int x = 0; x < sz.x; ++x)
                for (int z = 0; z < sz.z; ++z)
                    for (int y = sz.y - 1; y >= 0; --y) {
                        const uint16_t id = w.blockAt(x, y, z).id;
                        if (isLeaf(id) && !foundLeaves) {
                            c = {x, y, z};
                            foundLeaves = true;
                        }
                        const uint8_t sl = w.skyLightAt(x, y, z);
                        if (sl >= 1 && sl <= 14) {
                            partialShade = true;
                            if (!foundLeaves) c = {x, y, z}; // fallback centre
                        }
                    }
            check(partialShade, "foliage/relief casts a partial sky shadow (skylight 1..14)");
            check(foundLeaves || partialShade, "found a shaded region to relight-test");

            // The box a setBlock at c relights: c +/- 16 in X/Z, full height.
            const int R = 16;
            const int x0 = std::max(0, c.x - R), x1 = std::min(sz.x - 1, c.x + R);
            const int z0 = std::max(0, c.z - R), z1 = std::min(sz.z - 1, c.z + R);
            std::vector<uint8_t> before;
            before.reserve(static_cast<size_t>(x1 - x0 + 1) * sz.y * (z1 - z0 + 1));
            for (int x = x0; x <= x1; ++x)
                for (int y = 0; y < sz.y; ++y)
                    for (int z = z0; z <= z1; ++z)
                        before.push_back(w.skyLightAt(x, y, z));

            // No-op edit: set the centre block to itself -> reruns relightField.
            w.setBlock(c.x, c.y, c.z, w.blockAt(c.x, c.y, c.z));

            bool identical = true;
            size_t i = 0;
            for (int x = x0; x <= x1; ++x)
                for (int y = 0; y < sz.y; ++y)
                    for (int z = z0; z <= z1; ++z)
                        if (w.skyLightAt(x, y, z) != before[i++]) identical = false;
            check(identical,
                  "no-op edit preserves sky light (edit relight matches gen-time flood)");
        }

        // ---- Light-changing edits match a full recompute (clamped relight) -----
        // setBlock relights only a clamped box around the edit: sky from the edit
        // down to the first blocker below it, block light a wy±16 cube. Each must
        // reproduce what a full recompute would — the bug this guards is a box too
        // small, leaving stale light (a placed block whose shadow falls below the
        // box, or an emitter whose glow is clipped). A falloff toggle forces a full
        // computeSkyLight/computeBlockLight as ground truth; compare the whole window.
        {
            vg::WorldConfig wcfg;
            wcfg.seed = 4242u;
            wcfg.streaming = false;
            wcfg.streamWorkers = 0;
            wcfg.viewRadius = 3;
            wcfg.heightChunks = 16;
            wcfg.chunksX = wcfg.chunksZ = 2 * wcfg.viewRadius + 1;
            wcfg.chunksY = wcfg.heightChunks;
            vg::World w(wcfg, assetDir + "/blocks.yaml");
            const glm::ivec3 sz = w.sizeInBlocks();
            const uint16_t stone = w.registry().idByName("stone");
            uint16_t emitter = 0; // first emissive block the registry happens to have
            for (const char* n : {"glowstone", "torch", "lava", "jack_o_lantern", "lantern"}) {
                try {
                    const uint16_t id = w.registry().idByName(n);
                    if (w.registry().emission(id) > 0) { emitter = id; break; }
                } catch (...) {}
            }

            // Force a full recompute at the original falloff (toggle away and back).
            auto fullRecompute = [&] {
                w.setLightFalloff(wcfg.skyFalloff + 1, wcfg.blockFalloff);
                w.setLightFalloff(wcfg.skyFalloff, wcfg.blockFalloff);
            };
            // Snapshot the incremental result, force a full recompute, compare both
            // light fields over the whole window. (Each call ends on a full recompute,
            // so the next edit starts from a known-correct field.)
            auto matchesFull = [&](const char* label) {
                std::vector<uint8_t> aSky, aBlk;
                aSky.reserve(static_cast<size_t>(sz.x) * sz.y * sz.z);
                aBlk.reserve(static_cast<size_t>(sz.x) * sz.y * sz.z);
                for (int x = 0; x < sz.x; ++x)
                    for (int y = 0; y < sz.y; ++y)
                        for (int z = 0; z < sz.z; ++z) {
                            aSky.push_back(w.skyLightAt(x, y, z));
                            aBlk.push_back(w.blockLightAt(x, y, z));
                        }
                fullRecompute();
                bool ok = true;
                size_t i = 0;
                for (int x = 0; x < sz.x; ++x)
                    for (int y = 0; y < sz.y; ++y)
                        for (int z = 0; z < sz.z; ++z) {
                            if (w.skyLightAt(x, y, z) != aSky[i] ||
                                w.blockLightAt(x, y, z) != aBlk[i]) ok = false;
                            ++i;
                        }
                check(ok, label);
            };

            // An open-sky air cell a few blocks above the surface: placing a solid
            // block there casts a shadow that falls below it (the clamped-sky case).
            glm::ivec3 air{-1, -1, -1};
            for (int x = 1; x < sz.x - 1 && air.x < 0; ++x)
                for (int z = 1; z < sz.z - 1 && air.x < 0; ++z)
                    for (int y = sz.y - 2; y >= 1; --y)
                        if (w.blockAt(x, y, z).id != 0) {
                            const int ay = y + 8;
                            if (ay < sz.y && w.blockAt(x, ay, z).id == 0 &&
                                w.skyLightAt(x, ay, z) == 15) {
                                air = {x, ay, z};
                            }
                            break; // topmost solid in this column found
                        }
            check(air.x >= 0, "found an open-sky air cell to edit-test");

            w.setBlock(air.x, air.y, air.z, vg::Block{stone, 0});
            matchesFull("placing a sky-blocker relights to match a full recompute");

            w.setBlock(air.x, air.y, air.z, vg::Block{});
            matchesFull("breaking the blocker relights to match a full recompute");

            if (emitter) {
                w.setBlock(air.x, air.y, air.z, vg::Block{emitter, 0});
                matchesFull("placing an emitter relights block light to match full");
                w.setBlock(air.x, air.y, air.z, vg::Block{});
                matchesFull("removing the emitter relights block light to match full");
            }

            // ---- Batched + multi-chunk edits match a full recompute ------------
            // The cases above use single setBlock edits at one cell. Exercise the
            // BATCH path (setBlocksBatch, one relight for the union box) AND edits
            // whose light effect spans several chunks — a tall blocker casting a
            // deep multi-chunk sky shadow, and twin emitters whose glows overlap
            // across a chunk seam. This is the guard for the LightPropagator
            // refactor (Stage 5): free cross-chunk flood + light removal must
            // still reproduce a from-scratch computeSkyLight/computeBlockLight.
            {
                // A vertical stone pillar dropping from the open-sky air cell:
                // its shadow crosses several vertical chunk boundaries (kSize=16).
                std::vector<std::pair<glm::ivec3, vg::Block>> pillar;
                for (int dy = 0; dy < 40 && air.y - dy >= 1; ++dy) {
                    if (w.blockAt(air.x, air.y - dy, air.z).id == 0) {
                        pillar.push_back({{air.x, air.y - dy, air.z}, vg::Block{stone, 0}});
                    }
                }
                if (!pillar.empty()) {
                    w.setBlocksBatch(pillar);
                    matchesFull("batched tall blocker (multi-chunk shadow) matches full");
                    std::vector<std::pair<glm::ivec3, vg::Block>> clear = pillar;
                    for (auto& p : clear) p.second = vg::Block{};
                    w.setBlocksBatch(clear);
                    matchesFull("removing the batched blocker matches full");
                }

                if (emitter) {
                    // Two emitters a few cells apart (across a likely chunk seam)
                    // so their glows overlap and flood through each other.
                    std::vector<std::pair<glm::ivec3, vg::Block>> emit;
                    emit.push_back({{air.x, air.y, air.z}, vg::Block{emitter, 0}});
                    if (air.x + 5 < sz.x - 1 &&
                        w.blockAt(air.x + 5, air.y, air.z).id == 0) {
                        emit.push_back({{air.x + 5, air.y, air.z}, vg::Block{emitter, 0}});
                    }
                    w.setBlocksBatch(emit);
                    matchesFull("batched emitters (overlapping cross-chunk glow) match full");
                    std::vector<std::pair<glm::ivec3, vg::Block>> off = emit;
                    for (auto& e : off) e.second = vg::Block{};
                    w.setBlocksBatch(off);
                    matchesFull("removing the batched emitters matches full");
                }
            }
        }

        // ---- Bounded recenter relight is byte-identical to full height ---------
        // Moving the window relights only the entering edge's box (World::relightBox),
        // whose sky pass is HEIGHT-BOUNDED: it skips the open sky above the highest
        // blocker / stale sub-15 remnant, where both old and new light are already 15.
        // This is an OPTIMISATION that must not change the baked result — the bug it
        // could introduce is a box too small, leaving stale light at a streamed-in seam
        // (the failure mode that disables floating islands). Recenter two identical
        // streaming worlds the same way — one bounded (production), one forced to full
        // height — and require their light fields to match cell-for-cell.
        // (Incremental streaming relight is deliberately NOT compared to a from-scratch
        //  recompute: the retained interior keeps its open-border generation light, so
        //  they legitimately differ; the invariant that matters is bounded == full.)
        {
            auto runRecenter = [&](bool fullHeight,
                                   std::vector<uint8_t>& sky,
                                   std::vector<uint8_t>& blk) {
                vg::WorldConfig wcfg;
                wcfg.seed           = 4242u;
                wcfg.streaming      = true;
                wcfg.streamWorkers  = 0;     // serial relight: deterministic
                wcfg.asyncStreaming = false;
                wcfg.viewRadius     = 3;
                wcfg.heightChunks   = 16;
                wcfg.chunksX = wcfg.chunksZ = 2 * wcfg.viewRadius + 1;
                wcfg.chunksY = wcfg.heightChunks;
                vg::World w(wcfg, assetDir + "/blocks.yaml");
                w.setSkyRelightFullHeight(fullHeight);
                const glm::ivec3 org = w.chunkOrigin();
                std::vector<glm::ivec4> boxes;
                std::vector<glm::ivec3> dirty = w.recenter(
                    org.x + wcfg.viewRadius + 2, org.z + wcfg.viewRadius + 2, boxes);
                w.relightBoxes(boxes, dirty);
                const glm::ivec3 sz = w.sizeInBlocks();
                const glm::ivec3 o  = w.chunkOrigin() * vg::Chunk::kSize;
                sky.clear();
                blk.clear();
                for (int x = o.x; x < o.x + sz.x; ++x)
                    for (int y = 0; y < sz.y; ++y)
                        for (int z = o.z; z < o.z + sz.z; ++z) {
                            sky.push_back(w.skyLightAt(x, y, z));
                            blk.push_back(w.blockLightAt(x, y, z));
                        }
            };
            std::vector<uint8_t> bSky, bBlk, fSky, fBlk;
            runRecenter(/*fullHeight=*/false, bSky, bBlk);
            runRecenter(/*fullHeight=*/true,  fSky, fBlk);
            check(bSky == fSky && bBlk == fBlk,
                  "bounded recenter sky relight is byte-identical to full height");
        }

        // ---- Blockbench (.bbmodel) loader (ISSUES #13E) ------------------------
        // Parses a Blockbench box model into the armature Skeleton: outliner groups
        // -> joints, elements -> per-face-UV boxes, in Blockbench units (16 = 1 block).
        {
            // Structural invariants of the LOADER (not the specific art, so an edit
            // to hammer.bbmodel â€” more elements, a different skin resolution â€” doesn't
            // break the test): a skin, positive resolution, a parent-ordered skeleton
            // with the root + at least one group joint and at least one per-face-UV
            // box, all UVs normalised into [0,1], and Blockbench units (16=1 block).
            const vg::BlockbenchModel bb =
                vg::loadBlockbenchModel(assetDir + "/models/hammer/hammer.bbmodel");
            check(bb.skin == "hammer.png", "bbmodel reads its skin filename");
            check(bb.texW > 0 && bb.texH > 0, "bbmodel reads a positive skin resolution");
            check(bb.skeleton.joints.size() >= 2, "bbmodel outliner group -> a joint");
            check(bb.skeleton.boxes.size() >= 1, "bbmodel elements -> boxes");
            check(bb.skeleton.isTopologicallyOrdered(), "bbmodel skeleton is parent-ordered");
            check(bb.skeleton.joints[1].parent == 0, "bbmodel group joint parents to root");
            check(bb.skeleton.boxes[0].perFaceUV, "bbmodel boxes carry per-face UVs");
            bool uvNormalised = true;
            for (const vg::Box& bx : bb.skeleton.boxes)
                for (const glm::vec4& f : bx.faceUV)
                    if (f.x < 0.0f || f.y < 0.0f || f.z > 1.0001f || f.w > 1.0001f)
                        uvNormalised = false;
            check(uvNormalised, "bbmodel per-face UVs are normalised into [0,1]");
            // The handle's from=[7.5,0,7.5] about its [8,0,8] pivot -> min ~ -1/32 block.
            check(near(bb.skeleton.boxes[0].min.x, -0.03125f) &&
                  near(bb.skeleton.boxes[0].min.y, 0.0f),
                  "bbmodel converts Blockbench units to block space");
        }

        // ---- box_uv + embedded base64 textures (Blockbench-native save support) ----
        // Models saved (not exported) from Blockbench use a single per-element (u,v)
        // offset (box_uv) and embed the skin as a base64 data URI. Write tiny fixtures
        // and confirm the loader unwraps the box UV + decodes the embedded PNG.
        {
            auto writeFixture = [](const std::string& path, const char* json) {
                std::ofstream f(path);
                f << json;
            };
            // box_uv: an 8^3 cube at uv (0,0) on a 64^2 skin. The cross layout puts the
            // top face at [8,0,16,8] and the south face at [24,8,32,16] (pixels).
            const std::string boxPath = assetDir + "/_lt_boxuv.bbmodel";
            writeFixture(boxPath, R"({
              "meta":{"format_version":"4.5","model_format":"free","box_uv":true},
              "name":"boxuv","resolution":{"width":64,"height":64},
              "elements":[{"name":"c","from":[0,0,0],"to":[8,8,8],"origin":[4,0,4],
                "uuid":"u1","box_uv":true,"uv_offset":[0,0],
                "faces":{"north":{"texture":0},"south":{"texture":0},"east":{"texture":0},
                         "west":{"texture":0},"up":{"texture":0},"down":{"texture":0}}}],
              "outliner":[{"name":"g","origin":[4,0,4],"uuid":"go","children":["u1"]}],
              "textures":[{"name":"t","id":"0","path":"t.png"}]})");
            const vg::BlockbenchModel box = vg::loadBlockbenchModel(boxPath);
            std::remove(boxPath.c_str());
            check(box.skeleton.boxes.size() == 1 && box.skeleton.boxes[0].perFaceUV,
                  "box_uv element -> per-face UV box");
            const glm::vec4 up = box.skeleton.boxes[0].faceUV[2];   // +Y
            const glm::vec4 south = box.skeleton.boxes[0].faceUV[4]; // +Z
            check(near(up.x, 0.125f) && near(up.y, 0.0f) && near(up.z, 0.25f) && near(up.w, 0.125f),
                  "box_uv unwraps the top face rect");
            check(up != south, "box_uv gives distinct faces different rects");
            bool boxUVNorm = true;
            for (const glm::vec4& f : box.skeleton.boxes[0].faceUV)
                if (f.x < 0.0f || f.y < 0.0f || f.z > 1.0001f || f.w > 1.0001f) boxUVNorm = false;
            check(boxUVNorm, "box_uv rects are normalised into [0,1]");

            // Embedded skin: a 1x1 PNG as a base64 data URI -> decoded PNG bytes.
            const std::string emPath = assetDir + "/_lt_embed.bbmodel";
            writeFixture(emPath, R"({
              "meta":{"format_version":"4.5","model_format":"free","box_uv":false},
              "name":"embed","resolution":{"width":1,"height":1},
              "elements":[{"name":"c","from":[0,0,0],"to":[1,1,1],"origin":[0,0,0],
                "uuid":"e1","faces":{"north":{"uv":[0,0,1,1],"texture":0}}}],
              "outliner":["e1"],
              "textures":[{"name":"t","id":"0","path":"t.png","source":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="}]})");
            const vg::BlockbenchModel em = vg::loadBlockbenchModel(emPath);
            std::remove(emPath.c_str());
            check(!em.embeddedPNG.empty(), "embedded base64 texture is decoded");
            check(em.embeddedPNG.size() >= 8 && em.embeddedPNG[0] == 0x89 &&
                  em.embeddedPNG[1] == 'P' && em.embeddedPNG[2] == 'N' && em.embeddedPNG[3] == 'G',
                  "decoded embedded texture is a real PNG (signature)");
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
//   --selftest        Run the headless world-generation determinism test and exit
//                     (no window). Exit code 0 = pass.
int main(int argc, char** argv) {
    long maxFrames = -1; // run until the window is closed
    bool framesSet = false;
    std::string screenshotPath;
    bool flycam = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtol(argv[++i], nullptr, 10);
            framesSet = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[++i];
        } else if (std::strcmp(argv[i], "--flycam") == 0) {
            flycam = true;
        } else if (std::strcmp(argv[i], "--selftest") == 0) {
            // Headless worldgen determinism test — no window/Vulkan.
            return runWorldGenSelfTest(VG_ASSET_DIR);
        } else if (std::strcmp(argv[i], "--logictest") == 0) {
            // Headless game-logic tests (mining/tools/…) — no window/Vulkan.
            return runLogicTest(VG_ASSET_DIR);
        } else {
            std::cerr << "Unknown argument: " << argv[i] << '\n';
            return EXIT_FAILURE;
        }
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
