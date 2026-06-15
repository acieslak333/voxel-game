#pragma once

#include <string>

namespace vg {

// -----------------------------------------------------------------------------
//  Settings
// -----------------------------------------------------------------------------
//  Player-adjustable options, persisted to a small YAML file so they survive
//  across runs (and rebuilds). App loads this at startup, applies each field to
//  the relevant subsystem, and writes it back when the menu closes.
// -----------------------------------------------------------------------------
struct Settings {
    int         pixelate       = 4;          // 0/1 = off .. 16
    // Light lost per block while spreading (levels are 0..15, so reach ~= 15 /
    // falloff). Sky falloff governs how far daylight leaks into caves; block
    // falloff is the glow radius of emitters (glowstone, lava). Clamped 1..15.
    int         skyFalloff     = 2;
    int         blockFalloff   = 1;
    // Render distance in chunks (the loaded voxel window's radius — the world is a
    // (2*renderDistance+1) square). Higher = sees farther but heavier. Overrides
    // world.yaml's view_radius. Applied when the world is built, so a change takes
    // effect on the next launch (the window is allocated once at startup).
    int         renderDistance = 9;
    // Low-light grain: max strength of the soft static shown when the player stands
    // in darkness (caves/night). Any light at the eye (torch/daylight) fades it out;
    // 0 = off. Applied in composite.frag, kept subtle and near-monochrome.
    float       darkNoise      = 0.14f;
    // Retro PS1/PS2 rendering — each effect is independent so they can be mixed
    // freely (affine + bits 5 for a PS1 look; interlace + bits ~6 for PS2).
    // All-off/neutral leaves the output unchanged. See composite.frag + the chunk
    // vertex shaders.
    bool        retroAffine    = false;      // affine (non-perspective) texture warp
    int         retroColorBits = 8;          // colour bits/channel (8 = off, 5 = PS1)
    float       retroDither    = 1.0f;       // ordered-dither amount (only when quantizing)
    float       retroInterlace = 0.0f;       // scanline-dim flicker (0 = off .. 1)
    // Selectable post-process colour palette (file stem under assets/colorpalettes/,
    // e.g. "gameboy"; "" = off -> use the per-channel retroColorBits quantiser).
    // When set, the composite remaps the whole frame to the nearest palette colour.
    std::string retroPalette;
    float       fov            = 70.0f;      // vertical field of view, degrees
    float       sensitivity    = 0.08f;      // mouse look
    float       flySpeed       = 12.0f;      // free-fly base speed (blocks/s)
    bool        viewBob        = true;       // subtle head-bob while walking
    bool        lod            = true;       // distant-terrain LOD shell (FarTerrainRenderer)
    float       dayLengthMinutes = 10.0f;    // real minutes per in-game day
    bool        timeRunning    = true;       // false freezes the time of day
    bool        fullscreen     = false;      // borderless-fullscreen vs windowed (F11)
    std::string skyColor       = "sky_blue"; // palette colour name
    std::string font           = "ari-w9500.ttf"; // file under assets/fonts/ari

    // Load from `path`; any missing field (or a missing/unreadable file) keeps
    // its default, so a fresh install just runs with defaults.
    [[nodiscard]] static Settings load(const std::string& path);
    // Write to `path`. Best-effort: failures are ignored.
    void save(const std::string& path) const;
};

} // namespace vg
