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
    float       fov            = 70.0f;      // vertical field of view, degrees
    float       sensitivity    = 0.08f;      // mouse look
    float       flySpeed       = 12.0f;      // free-fly base speed (blocks/s)
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
