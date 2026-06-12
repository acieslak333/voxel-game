#include "core/Settings.h"

#include <yaml-cpp/yaml.h>

#include <fstream>

namespace vg {

Settings Settings::load(const std::string& path) {
    Settings s; // defaults
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception&) {
        return s; // missing or malformed: keep defaults
    }
    if (!root.IsMap()) {
        return s;
    }
    auto get = [&](const char* key, auto& field) {
        if (root[key]) {
            try {
                field = root[key].as<std::decay_t<decltype(field)>>();
            } catch (const YAML::Exception&) {
                // ignore a bad value, keep the default
            }
        }
    };
    get("pixelate", s.pixelate);
    get("skyFalloff", s.skyFalloff);
    get("blockFalloff", s.blockFalloff);
    get("renderDistance", s.renderDistance);
    get("darkNoise", s.darkNoise);
    get("fov", s.fov);
    get("sensitivity", s.sensitivity);
    get("flySpeed", s.flySpeed);
    get("viewBob", s.viewBob);
    get("lod", s.lod);
    get("dayLengthMinutes", s.dayLengthMinutes);
    get("timeRunning", s.timeRunning);
    get("fullscreen", s.fullscreen);
    get("skyColor", s.skyColor);
    get("font", s.font);
    return s;
}

void Settings::save(const std::string& path) const {
    YAML::Node root;
    root["pixelate"]       = pixelate;
    root["skyFalloff"]     = skyFalloff;
    root["blockFalloff"]   = blockFalloff;
    root["renderDistance"] = renderDistance;
    root["darkNoise"]      = darkNoise;
    root["fov"]            = fov;
    root["sensitivity"]    = sensitivity;
    root["flySpeed"]       = flySpeed;
    root["viewBob"]        = viewBob;
    root["lod"]            = lod;
    root["dayLengthMinutes"] = dayLengthMinutes;
    root["timeRunning"]    = timeRunning;
    root["fullscreen"]     = fullscreen;
    root["skyColor"]       = skyColor;
    root["font"]           = font;

    std::ofstream out(path);
    if (out) {
        out << "# Player settings, written by the game. Edit in-game via the Esc menu.\n";
        out << root;
        out << "\n";
    }
}

} // namespace vg
