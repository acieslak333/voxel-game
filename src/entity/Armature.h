#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  Box-part armature + keyframe animation (ISSUES #13E)
// -----------------------------------------------------------------------------
//  A blocky, Minecraft-style entity is a *tree of cuboid parts* (head, torso,
//  arms, legs), NOT a skinned mesh. Each part is a Joint with a rest transform
//  relative to its parent; animation rotates/translates parts around their
//  pivots and the pose is composed down the hierarchy. Geometry is a set of
//  axis-aligned Boxes, each attached to a joint and carried by that joint's
//  world matrix — no vertex weights / skinning shader.
//
//  This file is pure CPU math (no Vulkan), so the whole rig + animation core is
//  exercised headlessly by `--logictest`. The renderer (EntityRenderer) and the
//  glTF loader build on top of it.
// -----------------------------------------------------------------------------

// One node in the part hierarchy. The rest transform (T*R*S) places the joint
// relative to its parent; an animation overrides any of T/R/S per frame.
struct Joint {
    std::string name;
    int         parent = -1;                 // index of the parent joint, -1 = root
    glm::vec3   restT{0.0f};                  // rest translation from the parent
    glm::quat   restR{1.0f, 0.0f, 0.0f, 0.0f}; // rest rotation (identity)
    glm::vec3   restS{1.0f};                   // rest scale
};

// An axis-aligned cuboid of geometry attached to a joint, in that joint's local
// space. uvMin/uvMax is the texture rectangle (0..1) the box samples; `layer`
// selects a texture-array slice (set by the renderer/loader). Rendering bakes the
// 8 corners through the joint's world matrix each frame.
struct Box {
    int       joint = 0;
    glm::vec3 min{-0.5f};
    glm::vec3 max{0.5f};
    glm::vec2 uvMin{0.0f};
    glm::vec2 uvMax{1.0f};
    // Per-face UV rects (x=uMin, y=vMin, z=uMax, w=vMax), in bakeMesh's face order
    // +X, -X, +Y, -Y, +Z, -Z. Filled by the Blockbench loader (a model textures each
    // face from a different region of its skin). When `perFaceUV` is false the box
    // uses uvMin/uvMax on every face (the original single-rect behaviour).
    glm::vec4 faceUV[6]{};
    bool      perFaceUV = false;
    uint32_t  layer = 0;
};

// The static rig: a topologically ordered joint list (every parent precedes its
// children, so a single forward pass composes world matrices) plus its boxes.
struct Skeleton {
    std::vector<Joint> joints;
    std::vector<Box>   boxes;

    // Index of the joint named `name`, or -1 if absent.
    [[nodiscard]] int find(const std::string& name) const;
    // True if joints are ordered parent-before-child (required by the composer).
    [[nodiscard]] bool isTopologicallyOrdered() const;
};

// A per-joint local transform set (the result of sampling a clip at some time).
// Entries default to the joint's rest transform; channels override them.
struct LocalPose {
    std::vector<glm::vec3> t;
    std::vector<glm::quat> r;
    std::vector<glm::vec3> s;
};

// One animated joint's keyframes. Any channel may be empty (then the joint keeps
// its rest value for that component). `times` is shared by whichever of the T/R/S
// arrays are non-empty; each non-empty array must match `times` in length.
struct AnimChannel {
    int                     joint = 0;
    std::vector<float>      times;
    std::vector<glm::vec3>  translations; // empty or times.size()
    std::vector<glm::quat>  rotations;    // empty or times.size()
    std::vector<glm::vec3>  scales;       // empty or times.size()
};

// A named animation: a duration and a set of per-joint channels. `loop` wraps the
// sample time into [0, duration).
struct AnimationClip {
    std::string              name;
    float                    duration = 0.0f;
    bool                     loop     = true;
    std::vector<AnimChannel> channels;
};

// Build a local T*R*S matrix.
[[nodiscard]] glm::mat4 jointMatrix(const glm::vec3& t, const glm::quat& r,
                                    const glm::vec3& s);

// The rest local pose (every joint at its rest transform).
[[nodiscard]] LocalPose restPose(const Skeleton& skel);

// Sample `clip` at `time` (seconds), starting from rest and overriding with the
// clip's channels. Looping clips wrap `time` into [0, duration); non-looping clips
// clamp to the ends. Between keyframes: translation/scale lerp, rotation slerp.
[[nodiscard]] LocalPose sampleClip(const Skeleton& skel, const AnimationClip& clip,
                                   float time);

// Compose a local pose into per-joint WORLD matrices (root applied last-to-first
// down the chain). Requires the skeleton to be topologically ordered.
[[nodiscard]] std::vector<glm::mat4> worldMatrices(const Skeleton& skel,
                                                   const LocalPose& pose);

// One baked entity vertex. Model-space (the renderer applies the entity's world
// placement); the box rig is already posed into these positions by bakeMesh().
struct EntityVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    uint32_t  layer;
};

// Bake every box of `skel` into a non-indexed triangle list (36 verts/box),
// transforming each box's corners + normals by its joint's world matrix. Called
// per frame for animated entities (cheap for the small box counts a mob has). The
// renderer uploads the result to a dynamic vertex buffer.
[[nodiscard]] std::vector<EntityVertex> bakeMesh(const Skeleton& skel,
                                                 const std::vector<glm::mat4>& world);

} // namespace vg
