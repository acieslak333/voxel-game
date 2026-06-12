#include "entity/Armature.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace vg {

int Skeleton::find(const std::string& name) const {
    for (size_t i = 0; i < joints.size(); ++i) {
        if (joints[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool Skeleton::isTopologicallyOrdered() const {
    for (size_t i = 0; i < joints.size(); ++i) {
        if (joints[i].parent >= static_cast<int>(i)) return false; // parent must precede child
    }
    return true;
}

glm::mat4 jointMatrix(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
    m *= glm::mat4_cast(r);
    m = glm::scale(m, s);
    return m;
}

LocalPose restPose(const Skeleton& skel) {
    LocalPose p;
    const size_t n = skel.joints.size();
    p.t.resize(n);
    p.r.resize(n);
    p.s.resize(n);
    for (size_t i = 0; i < n; ++i) {
        p.t[i] = skel.joints[i].restT;
        p.r[i] = skel.joints[i].restR;
        p.s[i] = skel.joints[i].restS;
    }
    return p;
}

namespace {

// Locate the keyframe segment for `time` in the sorted `times`. Returns the lower
// index `i` and the 0..1 blend `f` toward `i+1`. Clamps at both ends.
void segment(const std::vector<float>& times, float time, size_t& i, float& f) {
    if (times.size() <= 1 || time <= times.front()) {
        i = 0;
        f = 0.0f;
        return;
    }
    if (time >= times.back()) {
        i = times.size() - 1; // clamp: caller treats i as the last sample (f unused)
        f = 0.0f;
        return;
    }
    // times is small (a handful of keys); a linear scan is fine and branch-cheap.
    size_t hi = 1;
    while (hi < times.size() && times[hi] < time) ++hi;
    i = hi - 1;
    const float span = times[hi] - times[i];
    f = span > 0.0f ? (time - times[i]) / span : 0.0f;
}

glm::vec3 sampleVec3(const std::vector<float>& times, const std::vector<glm::vec3>& v,
                     float time, glm::vec3 fallback) {
    if (v.empty()) return fallback;
    if (v.size() == 1) return v[0];
    size_t i = 0;
    float f = 0.0f;
    segment(times, time, i, f);
    if (i + 1 >= v.size()) return v.back();
    return glm::mix(v[i], v[i + 1], f);
}

glm::quat sampleQuat(const std::vector<float>& times, const std::vector<glm::quat>& q,
                     float time, glm::quat fallback) {
    if (q.empty()) return fallback;
    if (q.size() == 1) return q[0];
    size_t i = 0;
    float f = 0.0f;
    segment(times, time, i, f);
    if (i + 1 >= q.size()) return q.back();
    return glm::slerp(q[i], q[i + 1], f);
}

} // namespace

LocalPose sampleClip(const Skeleton& skel, const AnimationClip& clip, float time) {
    LocalPose pose = restPose(skel);

    float t = time;
    if (clip.duration > 0.0f) {
        if (clip.loop) {
            t = std::fmod(time, clip.duration);
            if (t < 0.0f) t += clip.duration; // wrap negatives forward
        } else {
            t = std::clamp(time, 0.0f, clip.duration);
        }
    }

    for (const AnimChannel& ch : clip.channels) {
        if (ch.joint < 0 || ch.joint >= static_cast<int>(skel.joints.size())) continue;
        const size_t j = static_cast<size_t>(ch.joint);
        pose.t[j] = sampleVec3(ch.times, ch.translations, t, pose.t[j]);
        pose.r[j] = sampleQuat(ch.times, ch.rotations, t, pose.r[j]);
        pose.s[j] = sampleVec3(ch.times, ch.scales, t, pose.s[j]);
    }
    return pose;
}

std::vector<glm::mat4> worldMatrices(const Skeleton& skel, const LocalPose& pose) {
    const size_t n = skel.joints.size();
    std::vector<glm::mat4> world(n, glm::mat4(1.0f));
    for (size_t i = 0; i < n; ++i) {
        const glm::mat4 local = jointMatrix(pose.t[i], pose.r[i], pose.s[i]);
        const int parent = skel.joints[i].parent;
        // Topological order guarantees the parent's world matrix is already final.
        world[i] = (parent >= 0) ? world[static_cast<size_t>(parent)] * local : local;
    }
    return world;
}

std::vector<EntityVertex> bakeMesh(const Skeleton& skel,
                                   const std::vector<glm::mat4>& world) {
    // Six faces, each defined by its outward axis + sign and two in-plane axes
    // chosen so unit(uAxis) x unit(vAxis) == the outward normal (CCW winding).
    struct Face { int axis; float sign; int u; int v; };
    static const Face kFaces[6] = {
        {0, +1.0f, 1, 2}, {0, -1.0f, 2, 1}, // +X, -X
        {1, +1.0f, 2, 0}, {1, -1.0f, 0, 2}, // +Y, -Y
        {2, +1.0f, 0, 1}, {2, -1.0f, 1, 0}, // +Z, -Z
    };

    std::vector<EntityVertex> out;
    out.reserve(skel.boxes.size() * 36);

    for (const Box& b : skel.boxes) {
        if (b.joint < 0 || b.joint >= static_cast<int>(world.size())) continue;
        const glm::mat4 m  = world[static_cast<size_t>(b.joint)];
        const glm::mat3 nm = glm::mat3(m); // rotation (entities use uniform scale)
        const glm::vec3 c  = (b.min + b.max) * 0.5f;
        const glm::vec3 h  = (b.max - b.min) * 0.5f;

        int fi = 0;
        for (const Face& f : kFaces) {
            glm::vec3 n(0.0f); n[f.axis] = f.sign;
            glm::vec3 u(0.0f); u[f.u] = 1.0f;
            glm::vec3 v(0.0f); v[f.v] = 1.0f;
            const float hu = h[f.u], hv = h[f.v];
            const glm::vec3 fc = c + n * h[f.axis]; // face centre in box space

            // Four corners CCW (-u,-v)(+u,-v)(+u,+v)(-u,+v) with matching uvs.
            const glm::vec3 p[4] = {
                fc - u * hu - v * hv, fc + u * hu - v * hv,
                fc + u * hu + v * hv, fc - u * hu + v * hv,
            };
            // Per-face UV (Blockbench skins) or the box's single rect (legacy rigs).
            const glm::vec4 fuv = b.perFaceUV ? b.faceUV[fi]
                                              : glm::vec4(b.uvMin, b.uvMax);
            const glm::vec2 uv[4] = {
                {fuv.x, fuv.w}, {fuv.z, fuv.w}, {fuv.z, fuv.y}, {fuv.x, fuv.y},
            };
            const glm::vec3 wn = glm::normalize(nm * n);
            auto emit = [&](int k) {
                EntityVertex ev;
                ev.pos    = glm::vec3(m * glm::vec4(p[k], 1.0f));
                ev.normal = wn;
                ev.uv     = uv[k];
                ev.layer  = b.layer;
                out.push_back(ev);
            };
            emit(0); emit(1); emit(2); // two triangles per face
            emit(0); emit(2); emit(3);
            ++fi;
        }
    }
    return out;
}

} // namespace vg
