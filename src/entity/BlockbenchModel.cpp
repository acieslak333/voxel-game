/**
 * @file BlockbenchModel.cpp
 * @brief .bbmodel JSON parsing: outliner walk, box-UV cross, base64 PNG decode.
 *
 * Uses yaml-cpp (which parses JSON, a strict subset of YAML) to read the .bbmodel
 * format. Walks the outliner tree recursively, creating joints for bone groups and
 * rotated elements. Texture selection picks the most-referenced texture index.
 * @see docs/CODE_INDEX.md
 */

#include "entity/BlockbenchModel.h"

#include <yaml-cpp/yaml.h>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace vg {

namespace {

glm::vec3 vec3Of(const YAML::Node& n, glm::vec3 fallback = glm::vec3(0.0f)) {
    if (!n || !n.IsSequence() || n.size() < 3) return fallback;
    return {n[0].as<float>(), n[1].as<float>(), n[2].as<float>()};
}

// Blockbench rotation is XYZ Euler degrees about the part's origin. Compose Z*Y*X
// (the order the editor's gizmo applies) into a quaternion.
glm::quat eulerDegToQuat(const glm::vec3& deg) {
    const glm::vec3 r = glm::radians(deg);
    return glm::angleAxis(r.z, glm::vec3(0, 0, 1)) *
           glm::angleAxis(r.y, glm::vec3(0, 1, 0)) *
           glm::angleAxis(r.x, glm::vec3(1, 0, 0));
}

// Decode a standard base64 string into raw bytes. Tolerates whitespace and missing
// padding; stops at the first '='. Used for Blockbench's embedded "data:...;base64,"
// texture sources.
std::vector<unsigned char> base64Decode(const std::string& in) {
    static const auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    out.reserve(in.size() * 3 / 4);
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue; // skip newlines / stray chars
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// Per-face UV rects for a box-UV element: a single (u,v) offset on the skin plus the
// box size unwraps into the canonical Minecraft/Blockbench cross. Pixel rects (not yet
// normalised), in the loader's face order +X,-X,+Y,-Y,+Z,-Z (east,west,up,down,south,north).
void boxUVRects(const glm::vec3& from, const glm::vec3& to, const glm::vec2& uv,
                glm::vec4 out[6]) {
    const float dx = std::abs(to.x - from.x), dy = std::abs(to.y - from.y),
                dz = std::abs(to.z - from.z);
    const float u = uv.x, v = uv.y;
    // [uMin,vMin,uMax,vMax]
    out[0] = {u,            v + dz, u + dz,           v + dz + dy}; // +X east
    out[1] = {u + dz + dx,  v + dz, u + dz + dx + dz, v + dz + dy}; // -X west
    out[2] = {u + dz,       v,      u + dz + dx,      v + dz};      // +Y up
    out[3] = {u + dz + dx,  v,      u + dz + 2 * dx,  v + dz};      // -Y down
    out[4] = {u + 2*dz + dx, v + dz, u + 2*dz + 2*dx, v + dz + dy}; // +Z south
    out[5] = {u + dz,       v + dz, u + dz + dx,      v + dz + dy}; // -Z north
}

} // namespace

/// Load a .bbmodel at `path` into a BlockbenchModel. Throws std::runtime_error on failure.
BlockbenchModel loadBlockbenchModel(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path); // .bbmodel is JSON; yaml-cpp parses JSON (a YAML subset)
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("BlockbenchModel: failed to load '" + path + "': " + e.what());
    }

    BlockbenchModel model;
    if (const YAML::Node res = root["resolution"]) {
        if (res["width"])  model.texW = res["width"].as<int>();
        if (res["height"]) model.texH = res["height"].as<int>();
    }
    // All texture filenames (basenames), in declaration order. The skin we expose is
    // the one the FACES actually reference (the most-used face texture index) — so a
    // model that paints every face from textures[2] uses that texture, not textures[0].
    // Any filename works; the caller loads it from the model's own dir.
    std::vector<std::string> texNames;
    std::vector<std::string> texSources; // "data:...;base64,...." data URI, or "" if external
    if (const YAML::Node texs = root["textures"]; texs && texs.IsSequence()) {
        for (const YAML::Node& t : texs) {
            std::string p;
            if (t["path"])      p = t["path"].as<std::string>();
            else if (t["name"]) p = t["name"].as<std::string>();
            const size_t slash = p.find_last_of("/\\");
            if (slash != std::string::npos) p = p.substr(slash + 1);
            if (!p.empty() && p.find('.') == std::string::npos) p += ".png";
            texNames.push_back(p);
            // Blockbench's native save embeds the pixels as a base64 data URI in `source`.
            std::string src;
            if (t["source"]) src = t["source"].as<std::string>();
            texSources.push_back(src.rfind("data:", 0) == 0 ? src : std::string());
        }
    }
    std::vector<int> texHits(texNames.size(), 0); // count of face references per texture

    const float W = model.texW > 0 ? static_cast<float>(model.texW) : 16.0f;
    const float H = model.texH > 0 ? static_cast<float>(model.texH) : 16.0f;
    const float S = 1.0f / 16.0f; // Blockbench units -> blocks

    // Model-level default UV mode. box_uv = a single (u,v) offset per element that
    // unwraps into the classic cross (vs per-face uv rects); an element may override.
    bool modelBoxUV = false;
    if (const YAML::Node meta = root["meta"]; meta && meta["box_uv"]) {
        try { modelBoxUV = meta["box_uv"].as<bool>(); } catch (...) {}
    }

    // Index elements by uuid (the outliner references them).
    std::unordered_map<std::string, YAML::Node> elems;
    if (const YAML::Node es = root["elements"]) {
        for (const YAML::Node& e : es) {
            if (e["uuid"]) elems[e["uuid"].as<std::string>()] = e;
        }
    }

    Skeleton& skel = model.skeleton;
    skel.joints.push_back({"root", -1, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(1.0f)});

    // Blockbench face name -> bakeMesh face index (+X,-X,+Y,-Y,+Z,-Z).
    struct FaceMap { const char* name; int idx; };
    static const FaceMap kFaces[6] = {
        {"east", 0}, {"west", 1}, {"up", 2}, {"down", 3}, {"south", 4}, {"north", 5},
    };

    // Turn one element into a Box under `parentJoint` whose pivot is `parentOrigin`
    // (Blockbench units). A rotated element gets its own joint so it poses correctly.
    auto addElement = [&](const YAML::Node& e, int parentJoint, const glm::vec3& parentOrigin) {
        const glm::vec3 from   = vec3Of(e["from"]);
        const glm::vec3 to     = vec3Of(e["to"]);
        const glm::vec3 origin = e["origin"] ? vec3Of(e["origin"]) : parentOrigin;
        const glm::vec3 rot    = vec3Of(e["rotation"]);

        int       joint = parentJoint;
        glm::vec3 pivot = parentOrigin;
        if (rot != glm::vec3(0.0f)) {
            Joint j;
            j.name   = e["name"] ? e["name"].as<std::string>() : "part";
            j.parent = parentJoint;
            j.restT  = (origin - parentOrigin) * S;
            j.restR  = eulerDegToQuat(rot);
            skel.joints.push_back(j);
            joint = static_cast<int>(skel.joints.size()) - 1;
            pivot = origin;
        }

        Box b;
        b.joint     = joint;
        b.min       = (glm::min(from, to) - pivot) * S;
        b.max       = (glm::max(from, to) - pivot) * S;
        b.perFaceUV = true;
        for (glm::vec4& f : b.faceUV) f = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f); // default full

        // box_uv mode: derive all six face rects from the element's single (u,v)
        // offset + its size (the classic cross), instead of per-face uv rects.
        const bool elemBoxUV = e["box_uv"] ? [&] {
            try { return e["box_uv"].as<bool>(); } catch (...) { return modelBoxUV; }
        }() : modelBoxUV;
        if (elemBoxUV) {
            glm::vec2 uvOff(0.0f);
            if (const YAML::Node uo = e["uv_offset"]; uo && uo.IsSequence() && uo.size() >= 2)
                uvOff = {uo[0].as<float>(), uo[1].as<float>()};
            glm::vec4 rects[6];
            boxUVRects(from, to, uvOff, rects);
            for (int i = 0; i < 6; ++i)
                b.faceUV[i] = glm::vec4(rects[i].x / W, rects[i].y / H,
                                        rects[i].z / W, rects[i].w / H);
        }

        if (const YAML::Node faces = e["faces"]) {
            for (const FaceMap& fm : kFaces) {
                const YAML::Node face = faces[fm.name];
                if (!face) continue;
                const YAML::Node uv = face["uv"];
                if (!elemBoxUV && uv && uv.IsSequence() && uv.size() >= 4) {
                    b.faceUV[fm.idx] = glm::vec4(uv[0].as<float>() / W, uv[1].as<float>() / H,
                                                 uv[2].as<float>() / W, uv[3].as<float>() / H);
                }
                if (face["texture"]) { // tally which texture this face samples
                    const int ti = face["texture"].as<int>();
                    if (ti >= 0 && ti < static_cast<int>(texHits.size())) ++texHits[ti];
                }
            }
        }
        skel.boxes.push_back(b);
    };

    // Walk the outliner: a scalar is a loose element uuid; a map is a bone group.
    std::function<void(const YAML::Node&, int, glm::vec3)> walk =
        [&](const YAML::Node& node, int parentJoint, glm::vec3 parentOrigin) {
            if (node.IsScalar()) {
                const auto it = elems.find(node.as<std::string>());
                if (it != elems.end()) addElement(it->second, parentJoint, parentOrigin);
                return;
            }
            if (!node.IsMap()) return;
            Joint j;
            j.name   = node["name"] ? node["name"].as<std::string>() : "group";
            j.parent = parentJoint;
            const glm::vec3 origin = node["origin"] ? vec3Of(node["origin"]) : parentOrigin;
            j.restT = (origin - parentOrigin) * S;
            if (node["rotation"]) j.restR = eulerDegToQuat(vec3Of(node["rotation"]));
            skel.joints.push_back(j);
            const int gj = static_cast<int>(skel.joints.size()) - 1;
            if (const YAML::Node ch = node["children"]) {
                for (const YAML::Node& c : ch) walk(c, gj, origin);
            }
        };

    if (const YAML::Node outliner = root["outliner"]) {
        for (const YAML::Node& n : outliner) walk(n, 0, glm::vec3(0.0f));
    } else {
        // No outliner: attach every element directly to the root joint.
        for (const auto& kv : elems) addElement(kv.second, 0, glm::vec3(0.0f));
    }

    // Skin = the most-referenced texture (the one the faces actually paint with),
    // falling back to the first declared texture.
    if (!texNames.empty()) {
        size_t best = 0;
        for (size_t i = 1; i < texHits.size(); ++i) {
            if (texHits[i] > texHits[best]) best = i;
        }
        model.skin = texNames[best];
        // If that texture is embedded (base64 data URI), decode its PNG bytes so the
        // caller can load it from memory instead of an external file.
        if (best < texSources.size() && !texSources[best].empty()) {
            const std::string& src = texSources[best];
            const size_t comma = src.find(',');
            if (comma != std::string::npos)
                model.embeddedPNG = base64Decode(src.substr(comma + 1));
        }
    }

    return model;
}

} // namespace vg
