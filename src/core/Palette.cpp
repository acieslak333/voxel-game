#include "core/Palette.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <stdexcept>

namespace vg {

namespace {

// Standard sRGB -> linear transfer function (per channel).
float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 parseHex(const std::string& value, const std::string& name) {
    std::string h = value;
    if (!h.empty() && h.front() == '#') {
        h.erase(0, 1);
    }
    if (h.size() != 6) {
        throw std::runtime_error("Palette: colour '" + name +
                                 "' must be a #RRGGBB hex string, got '" + value + "'");
    }
    try {
        auto channel = [&](int offset) {
            return static_cast<float>(std::stoi(h.substr(offset, 2), nullptr, 16)) / 255.0f;
        };
        return glm::vec3(channel(0), channel(2), channel(4));
    } catch (const std::exception&) {
        throw std::runtime_error("Palette: colour '" + name + "' has invalid hex '" +
                                 value + "'");
    }
}

} // namespace

Palette::Palette(const std::string& paletteFile) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(paletteFile);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Palette: failed to load '" + paletteFile + "': " + e.what());
    }
    if (!root.IsMap()) {
        throw std::runtime_error("Palette: '" + paletteFile +
                                 "' must be a map of name -> \"#RRGGBB\"");
    }
    for (const auto& entry : root) {
        const std::string name = entry.first.as<std::string>();
        if (colors_.emplace(name, parseHex(entry.second.as<std::string>(), name)).second) {
            order_.push_back(name);
        }
    }
}

glm::vec3 Palette::srgb(const std::string& name) const {
    const auto it = colors_.find(name);
    if (it == colors_.end()) {
        throw std::out_of_range("Palette: unknown colour '" + name + "'");
    }
    return it->second;
}

glm::vec3 Palette::linear(const std::string& name) const {
    const glm::vec3 c = srgb(name);
    return glm::vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b));
}

bool Palette::has(const std::string& name) const {
    return colors_.find(name) != colors_.end();
}

} // namespace vg
