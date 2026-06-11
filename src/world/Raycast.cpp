#include "world/Raycast.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vg {

namespace {
// Ray vs axis-aligned box (slab method). On a hit within [0, maxT] returns true
// with `tNear` (entry distance) and the entry-face normal pointing back toward the
// ray origin (matching the DDA's normal convention). `d` must be normalized.
bool rayBox(const glm::vec3& o, const glm::vec3& d, const glm::vec3& bmin,
            const glm::vec3& bmax, float maxT, float& tNear, glm::ivec3& normal) {
    float tmin = 0.0f, tmax = maxT;
    int axis = -1;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) {
            if (o[i] < bmin[i] || o[i] > bmax[i]) return false; // parallel & outside
            continue;
        }
        const float inv = 1.0f / d[i];
        const float t1  = (bmin[i] - o[i]) * inv;
        const float t2  = (bmax[i] - o[i]) * inv;
        const float tlo = std::min(t1, t2), thi = std::max(t1, t2);
        if (tlo > tmin) { tmin = tlo; axis = i; }
        if (thi < tmax) tmax = thi;
        if (tmin > tmax) return false;
    }
    if (axis < 0) return false; // origin inside the box: no surface to report
    tNear = tmin;
    normal = glm::ivec3(0);
    normal[axis] = (d[axis] > 0.0f) ? -1 : 1;
    return true;
}
} // namespace

RaycastHit raycastVoxel(const glm::vec3& origin, const glm::vec3& dir,
                        float maxDistance, const SolidFn& solid, const BoxesFn& boxes) {
    RaycastHit result;
    if (glm::dot(dir, dir) == 0.0f) {
        return result; // no direction, no hit
    }
    const glm::vec3 d = glm::normalize(dir);

    // Integer voxel the ray starts in.
    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    // Step direction (+1 / -1 / 0) per axis.
    const int stepX = (d.x > 0) ? 1 : (d.x < 0 ? -1 : 0);
    const int stepY = (d.y > 0) ? 1 : (d.y < 0 ? -1 : 0);
    const int stepZ = (d.z > 0) ? 1 : (d.z < 0 ? -1 : 0);

    constexpr float kInf = std::numeric_limits<float>::infinity();

    // tMax: ray parameter t at which we cross into the next voxel along each
    // axis. tDelta: how much t advances to traverse one whole voxel on that axis.
    auto firstBoundary = [](float o, float dd, int cell, int step) -> float {
        if (dd == 0.0f) return kInf;
        const float boundary = static_cast<float>(step > 0 ? cell + 1 : cell);
        return (boundary - o) / dd;
    };
    float tMaxX = firstBoundary(origin.x, d.x, x, stepX);
    float tMaxY = firstBoundary(origin.y, d.y, y, stepY);
    float tMaxZ = firstBoundary(origin.z, d.z, z, stepZ);

    const float tDeltaX = (d.x != 0.0f) ? std::abs(1.0f / d.x) : kInf;
    const float tDeltaY = (d.y != 0.0f) ? std::abs(1.0f / d.y) : kInf;
    const float tDeltaZ = (d.z != 0.0f) ? std::abs(1.0f / d.z) : kInf;

    // Step voxel-by-voxel to the nearest boundary until we hit a solid block or
    // run past the reach distance. The entered face's normal is the opposite of
    // the axis step we just took.
    glm::ivec3 normal{0};
    float t = 0.0f;
    while (t <= maxDistance) {
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            x += stepX;
            t = tMaxX;
            tMaxX += tDeltaX;
            normal = {-stepX, 0, 0};
        } else if (tMaxY <= tMaxZ) {
            y += stepY;
            t = tMaxY;
            tMaxY += tDeltaY;
            normal = {0, -stepY, 0};
        } else {
            z += stepZ;
            t = tMaxZ;
            tMaxZ += tDeltaZ;
            normal = {0, 0, -stepZ};
        }
        if (t > maxDistance) {
            break;
        }
        if (solid(x, y, z)) {
            ShapeBox cellBoxes[kMaxShapeBoxes];
            const int n = boxes ? boxes(x, y, z, cellBoxes) : 0;
            if (n == 0) {
                // Full-cell target (cut-out foliage, or no box provider): the DDA
                // entry face/point is the hit.
                result.hit    = true;
                result.block  = {x, y, z};
                result.normal = normal;
                result.point  = origin + t * d;
                return result;
            }
            // Refine: hit the nearest sub-box the ray actually pierces; if it
            // misses them all (e.g. aiming over a slab), keep marching to the
            // block behind. normal/point describe the struck sub-box face.
            float      bestT = std::numeric_limits<float>::infinity();
            glm::ivec3 bnormal{0};
            bool       got = false;
            for (int i = 0; i < n; ++i) {
                float tn = 0.0f;
                glm::ivec3 nrm{0};
                if (rayBox(origin, d, cellBoxes[i].lo, cellBoxes[i].hi, maxDistance, tn, nrm) &&
                    tn < bestT) {
                    bestT   = tn;
                    bnormal = nrm;
                    got     = true;
                }
            }
            if (got) {
                result.hit    = true;
                result.block  = {x, y, z};
                result.normal = bnormal;
                result.point  = origin + bestT * d;
                return result;
            }
            // missed every sub-box — fall through and continue the DDA
        }
    }
    return result;
}

} // namespace vg
