/**
 * @file Feature.cpp
 * @brief YAML loader and per-cell evaluation for procedural Feature objects.
 *
 * Parses assets/features/*.yaml into Feature structs (op shapes, randomisable
 * dimensions, weighted block palettes, fill modes). Feature::at() evaluates all
 * ops in order for a local cell and returns the block id the cell should hold,
 * or kSkip to leave terrain untouched.
 * @see docs/CODE_INDEX.md
 */

#include "world/Feature.h"

#include "world/BlockRegistry.h"
#include "utilities/noise/Noise.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace vg {

// -----------------------------------------------------------------------------
//  YAML format (assets/features/*.yaml):
//
//    name: boulder
//    scatter: { density: 0.25, spacing: 48, surface: true,
//               biomes: [plains, forest], min_elevation: 2, max_elevation: 9999 }
//    size:   [9, 8, 9]      # MAX bounding box (gather reach + default anchor)
//    anchor: [4, 0, 4]      # local cell meeting the surface (default [x/2,0,z/2])
//    ops:                   # applied in order; a later op overrides an earlier one
//      - shape: sphere      # box|sphere|ellipsoid|cylinder|cone|column|line
//        at: [0, 0, 0]      # offset from the anchor (x,z centred; y up from ground)
//        radius: {min: 2, max: 4}        # a number, or {min,max} (randomized)
//        height: 5                        # column/cone/cylinder length; line run
//        size:   [3, 2, 3]                # box/ellipsoid extents (each num or {min,max})
//        block:  [ {name: stone, w: 3}, {name: cobblestone, w: 1} ]  # or "stone"
//        block_pick: per_cell             # per_cell | per_instance
//        fill: solid                      # solid | shell | noise
//        noise: { freq: 0.25, threshold: 0.0 }
//        place: force                     # force | air_only
// -----------------------------------------------------------------------------
namespace {

// Integer hashing → [0,1). Same family as the worldgen scatter hashes, so feature
// randomness is decorrelated from terrain.
uint32_t mix(uint32_t a, uint32_t b) {
    a ^= b + 0x9e3779b9u + (a << 6) + (a >> 2);
    return a;
}
float h01(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return static_cast<float>(x & 0x00FFFFFFu) / 16777216.0f;
}

uint16_t resolveBlock(const BlockRegistry& reg, const std::string& name) {
    if (name == "air" || name == "skip") return name == "air" ? 0 : Feature::kSkip;
    try {
        return reg.idByName(name);
    } catch (const std::exception&) {
        throw std::runtime_error("Feature: unknown block '" + name + "'");
    }
}

RandF parseRandF(const YAML::Node& n, float def) {
    RandF r;
    r.fixed = true;
    r.a = def;
    if (!n || !n.IsDefined() || n.IsNull()) return r; // absent/null -> default
    if (n.IsMap()) {                       // {min, max}
        r.fixed = false;
        r.a = n["min"] ? n["min"].as<float>() : def;
        r.b = n["max"] ? n["max"].as<float>() : r.a;
        if (r.b < r.a) std::swap(r.a, r.b);
    } else if (n.IsScalar()) {             // scalar -> fixed
        r.a = n.as<float>();
    }
    return r;
}

Feature::Shape parseShape(const std::string& s) {
    if (s == "sphere")    return Feature::Shape::Sphere;
    if (s == "ellipsoid") return Feature::Shape::Ellipsoid;
    if (s == "cylinder")  return Feature::Shape::Cylinder;
    if (s == "cone")      return Feature::Shape::Cone;
    if (s == "column")    return Feature::Shape::Column;
    if (s == "line")      return Feature::Shape::Line;
    if (s == "torus")     return Feature::Shape::Torus;
    if (s == "arch")      return Feature::Shape::Arch;
    if (s == "spiral")    return Feature::Shape::Spiral;
    if (s == "voxels")    return Feature::Shape::Voxels;
    return Feature::Shape::Box;
}

void parseBlocks(const YAML::Node& n, const BlockRegistry& reg, Feature::Op& op) {
    op.blocks.clear();
    op.totalW = 0.0f;
    auto add = [&](const std::string& name, float w) {
        op.blocks.push_back({resolveBlock(reg, name), w});
        op.totalW += w;
    };
    if (!n || !n.IsDefined() || n.IsNull()) { add("stone", 1.0f); return; }
    if (n.IsScalar()) { add(n.as<std::string>(), 1.0f); return; }
    if (n.IsSequence()) {
        for (const auto& e : n) {
            if (e.IsMap() && e["name"]) add(e["name"].as<std::string>(),
                                            e["w"] ? e["w"].as<float>() : 1.0f);
            else if (e.IsScalar())      add(e.as<std::string>(), 1.0f);
        }
    }
    if (op.blocks.empty()) add("stone", 1.0f);
}

// Parse one op node into a Feature::Op (shared by `ops:` and each variant's ops).
Feature::Op parseOp(const YAML::Node& on, uint32_t opIdx, const BlockRegistry& reg) {
    Feature::Op op;
    op.shape = parseShape(on["shape"] ? on["shape"].as<std::string>() : "box");
    if (const YAML::Node at = on["at"]; at && at.IsSequence() && at.size() >= 3)
        op.at = {at[0].as<int>(), at[1].as<int>(), at[2].as<int>()};
    op.radius = parseRandF(on["radius"], op.shape == Feature::Shape::Column ? 0.0f : 2.0f);
    op.height = parseRandF(on["height"], 4.0f);
    op.thickness = parseRandF(on["thickness"], 1.5f);
    if (on["turns"])   op.turns   = on["turns"].as<float>();
    if (on["scatter"]) op.scatter = on["scatter"].as<float>();
    if (const YAML::Node sz = on["size"]; sz && sz.IsSequence() && sz.size() >= 3) {
        op.sizeX = parseRandF(sz[0], 3.0f);
        op.sizeY = parseRandF(sz[1], 3.0f);
        op.sizeZ = parseRandF(sz[2], 3.0f);
    } else {
        op.sizeX = parseRandF(YAML::Node(), 3.0f);
        op.sizeY = parseRandF(YAML::Node(), 3.0f);
        op.sizeZ = parseRandF(YAML::Node(), 3.0f);
    }
    if (on["cells"] && on["cells"].IsSequence()) {  // Voxels op: hand-placed cells
        for (const auto& c : on["cells"]) {
            if (!c.IsSequence() || c.size() < 3) continue;
            const int x = c[0].as<int>(), y = c[1].as<int>(), z = c[2].as<int>();
            if (x < -128 || x > 127 || y < -128 || y > 127 || z < -128 || z > 127) continue;
            uint16_t bid = 0xFFFEu; // sentinel: use the op's palette
            if (c.size() >= 4) bid = resolveBlock(reg, c[3].as<std::string>());
            op.voxels[Feature::voxelKey(x, y, z)] = bid;
        }
    }
    parseBlocks(on["block"], reg, op);
    op.pick = (on["block_pick"] && on["block_pick"].as<std::string>() == "per_instance")
                  ? Feature::Pick::PerInstance : Feature::Pick::PerCell;
    const std::string fill = on["fill"] ? on["fill"].as<std::string>() : "solid";
    op.fill = fill == "shell"   ? Feature::Fill::Shell
            : fill == "noise"   ? Feature::Fill::Noise
            : fill == "scatter" ? Feature::Fill::Scatter : Feature::Fill::Solid;
    if (const YAML::Node nn = on["noise"]) {
        op.nFreq   = nn["freq"]      ? nn["freq"].as<float>()      : 0.2f;
        op.nThresh = nn["threshold"] ? nn["threshold"].as<float>() : 0.0f;
    }
    op.place = (on["place"] && on["place"].as<std::string>() == "air_only")
                   ? Feature::Place::AirOnly : Feature::Place::Force;
    op.salt = (opIdx + 1u) * 2654435761u;
    return op;
}

std::vector<Feature::Op> parseOps(const YAML::Node& ops, const BlockRegistry& reg) {
    std::vector<Feature::Op> out;
    if (ops && ops.IsSequence()) {
        uint32_t i = 0;
        for (const auto& on : ops) out.push_back(parseOp(on, i++, reg));
    }
    return out;
}

Feature loadOne(const std::string& path, const BlockRegistry& reg) {
    const YAML::Node root = YAML::LoadFile(path);
    Feature f;
    f.name = root["name"] ? root["name"].as<std::string>()
                          : std::filesystem::path(path).stem().string();

    // NOTE: a feature's `scatter:` block (density/spacing/biome gates/masks) is no
    // longer parsed — automatic spawning was removed with the worldgen overhaul. A
    // feature is now just its voxel-op data (size/anchor/ops), placed explicitly.

    if (const YAML::Node s = root["size"]; s && s.IsSequence() && s.size() >= 3)
        f.size = {std::max(1, s[0].as<int>()), std::max(1, s[1].as<int>()), std::max(1, s[2].as<int>())};
    if (const YAML::Node a = root["anchor"]; a && a.IsSequence() && a.size() >= 3)
        f.anchor = {a[0].as<int>(), a[1].as<int>(), a[2].as<int>()};
    else
        f.anchor = {f.size.x / 2, 0, f.size.z / 2};

    f.ops = parseOps(root["ops"], reg);
    if (root["variants"] && root["variants"].IsSequence()) {   // alternative forms
        for (const auto& vn : root["variants"]) {
            std::vector<Feature::Op> vops = parseOps(vn["ops"] ? vn["ops"] : vn, reg);
            if (!vops.empty()) f.variants.push_back(std::move(vops));
        }
    }
    return f;
}

/// Weighted pick from an op's block palette for a roll value in [0,1).
uint16_t pickBlock(const Feature::Op& op, float roll) {
    if (op.blocks.empty() || op.totalW <= 0.0f) return Feature::kSkip;
    float t = roll * op.totalW;
    for (const auto& b : op.blocks) { t -= b.w; if (t < 0.0f) return b.id; }
    return op.blocks.back().id;
}

} // namespace

float RandF::atf(uint32_t h) const { return fixed ? a : a + (b - a) * h01(h); }
int   RandF::ati(uint32_t h) const { return static_cast<int>(std::lround(atf(h))); }

Feature::Cell Feature::at(uint32_t originSeed, int lx, int ly, int lz,
                          const Noise& noise, int wx, int wy, int wz) const {
    Cell result; // kSkip by default
    // Pick this instance's variant (ops = #0, variants[i] = #i+1) from the origin seed
    // so the same instance always renders the same form (seam-safe).
    const std::vector<Op>* selOps = &ops;
    if (!variants.empty()) {
        const uint32_t n = 1u + static_cast<uint32_t>(variants.size());
        uint32_t idx = static_cast<uint32_t>(h01(mix(originSeed, 0x7a91u)) * static_cast<float>(n));
        if (idx >= n) idx = n - 1;
        if (idx > 0) selOps = &variants[idx - 1];
    }
    for (const Op& op : *selOps) {
        // Op-local coordinates: x,z centred on the op origin, y up from the ground.
        const float fx = static_cast<float>(lx - anchor.x - op.at.x);
        const float fy = static_cast<float>(ly - anchor.y - op.at.y);
        const float fz = static_cast<float>(lz - anchor.z - op.at.z);

        const int   r  = std::max(0, op.radius.ati(mix(originSeed, op.salt ^ 0x101u)));
        const int   h  = std::max(1, op.height.ati(mix(originSeed, op.salt ^ 0x202u)));
        const float sx = static_cast<float>(std::max(1, op.sizeX.ati(mix(originSeed, op.salt ^ 0x303u))));
        const float sy = static_cast<float>(std::max(1, op.sizeY.ati(mix(originSeed, op.salt ^ 0x404u))));
        const float sz = static_cast<float>(std::max(1, op.sizeZ.ati(mix(originSeed, op.salt ^ 0x505u))));
        const float tube = std::max(0.5f, op.thickness.atf(mix(originSeed, op.salt ^ 0x606u)));

        bool     inside = false;
        float    margin = 9999.0f; // distance inside the boundary (for shells)
        uint16_t voxId  = kSkip;   // Voxels op: the cell's explicit block (or palette)
        switch (op.shape) {
            case Shape::Box: {
                const float hx = sx * 0.5f, hz = sz * 0.5f;
                inside = std::fabs(fx) <= hx && std::fabs(fz) <= hz && fy >= 0.0f && fy <= sy - 1.0f;
                if (inside) margin = std::min({hx - std::fabs(fx), hz - std::fabs(fz),
                                               fy, (sy - 1.0f) - fy});
                break;
            }
            case Shape::Sphere: {
                const float dy = fy - static_cast<float>(r);
                const float d  = std::sqrt(fx * fx + dy * dy + fz * fz);
                inside = d <= r + 0.001f;
                margin = static_cast<float>(r) - d;
                break;
            }
            case Shape::Ellipsoid: {
                const float rx = sx * 0.5f, ry = sy * 0.5f, rz = sz * 0.5f;
                const float ex = fx / std::max(0.5f, rx);
                const float ey = (fy - ry) / std::max(0.5f, ry);
                const float ez = fz / std::max(0.5f, rz);
                const float e  = std::sqrt(ex * ex + ey * ey + ez * ez);
                inside = e <= 1.0f;
                margin = (1.0f - e) * std::min({rx, ry, rz});
                break;
            }
            case Shape::Cylinder:
            case Shape::Column: {
                const float rad = std::sqrt(fx * fx + fz * fz);
                inside = rad <= r + 0.001f && fy >= 0.0f && fy <= h - 1.0f;
                if (inside) margin = std::min({static_cast<float>(r) - rad, fy, (h - 1.0f) - fy});
                break;
            }
            case Shape::Cone: {
                const float rr  = static_cast<float>(r) * (1.0f - fy / static_cast<float>(h));
                const float rad = std::sqrt(fx * fx + fz * fz);
                inside = fy >= 0.0f && fy <= h - 1.0f && rad <= rr + 0.001f;
                if (inside) margin = std::min({rr - rad, fy, (h - 1.0f) - fy});
                break;
            }
            case Shape::Line: {
                inside = fz == 0.0f && fy == 0.0f && fx >= 0.0f && fx <= h - 1.0f;
                margin = 9999.0f;
                break;
            }
            case Shape::Torus: {            // horizontal ring, major radius r, tube `tube`
                const float q  = std::sqrt(fx * fx + fz * fz) - static_cast<float>(r);
                const float dy = fy - tube; // rests on the ground (centre at y = tube)
                const float d  = std::sqrt(q * q + dy * dy);
                inside = d <= tube + 0.001f;
                margin = tube - d;
                break;
            }
            case Shape::Arch: {             // vertical half-ring (doorway), thin in Z
                const float ring = std::sqrt(fx * fx + fy * fy) - static_cast<float>(r);
                inside = fy >= -0.001f && std::fabs(fz) <= tube + 0.001f &&
                         std::fabs(ring) <= tube + 0.001f;
                if (inside) margin = std::min(tube - std::fabs(ring), tube - std::fabs(fz));
                break;
            }
            case Shape::Spiral: {           // helix of radius r winding up over height h
                if (fy < 0.0f || fy > h - 1.0f) break;
                const float theta = (fy / static_cast<float>(std::max(1, h))) * op.turns * 6.2831853f;
                const float hx = static_cast<float>(r) * std::cos(theta);
                const float hz = static_cast<float>(r) * std::sin(theta);
                const float d  = std::sqrt((fx - hx) * (fx - hx) + (fz - hz) * (fz - hz));
                inside = d <= tube + 0.001f;
                margin = tube - d;
                break;
            }
            case Shape::Voxels: {           // hand-placed cells (sculpt mode)
                const auto it = op.voxels.find(Feature::voxelKey(
                    static_cast<int>(std::lround(fx)), static_cast<int>(std::lround(fy)),
                    static_cast<int>(std::lround(fz))));
                if (it != op.voxels.end()) { inside = true; voxId = it->second; }
                break;
            }
        }
        if (!inside) continue;

        if (op.fill == Fill::Shell && margin > 1.5f) continue;
        if (op.fill == Fill::Noise) {
            const float n = noise.perlin(static_cast<float>(wx) * op.nFreq,
                                         static_cast<float>(wy) * op.nFreq,
                                         static_cast<float>(wz) * op.nFreq);
            if (n <= op.nThresh) continue;
        }
        if (op.fill == Fill::Scatter) {  // randomly drop cells -> fuzzy clouds/clusters
            const float s = h01(mix(mix(static_cast<uint32_t>(wx) * 374761393u,
                                        static_cast<uint32_t>(wz) * 668265263u)
                                        ^ (static_cast<uint32_t>(wy) * 2246822519u),
                                    op.salt ^ 0x5CA7u));
            if (s >= op.scatter) continue;
        }

        const float roll = op.pick == Pick::PerInstance
            ? h01(mix(originSeed, op.salt ^ 0xB10Cu))
            : h01(mix(mix(static_cast<uint32_t>(wx) * 73856093u,
                          static_cast<uint32_t>(wz) * 19349663u)
                          ^ (static_cast<uint32_t>(wy) * 83492791u), op.salt));
        // A Voxels cell with an explicit block uses it; otherwise the op's palette.
        const uint16_t id = (voxId != kSkip && voxId != 0xFFFEu) ? voxId : pickBlock(op, roll);
        if (id != kSkip) result = {id, op.place == Place::Force}; // later op overrides
    }
    return result;
}

FeatureSet::FeatureSet(const std::string& dir, const BlockRegistry& registry) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return; // no dir -> features off

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file()) {
            const auto ext = e.path().extension().string();
            if (ext == ".yaml" || ext == ".yml") files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end()); // deterministic order
    for (const auto& f : files) {
        try {
            features_.push_back(loadOne(f.string(), registry));
        } catch (const std::exception& e) {
            throw std::runtime_error("Feature '" + f.filename().string() + "': " + e.what());
        }
    }
}

} // namespace vg
