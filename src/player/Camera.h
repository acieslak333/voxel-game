#pragma once

/**
 * @file Camera.h
 * @brief Minimal first-person camera: yaw/pitch angles and view-matrix generation.
 *
 * Owns no movement logic — position and orientation are written by PlayerController
 * (or any other controller). Provides direction vectors used by locomotion and by
 * the renderer for projection.
 * @see docs/CODE_INDEX.md
 */

#include <glm/glm.hpp>

namespace vg {

// -----------------------------------------------------------------------------
//  Camera
// -----------------------------------------------------------------------------
//  A first-person camera defined by a position and yaw/pitch angles (degrees).
//  It only knows how to look around and produce a view matrix; *where* it moves
//  is decided by the PlayerController. Kept deliberately small so other
//  controllers (spectator, cutscene, ...) could drive it later.
// -----------------------------------------------------------------------------
/** @brief Yaw/pitch first-person camera; position and orientation owned externally. */
class Camera {
public:
    glm::vec3 position{0.0f};

    float yaw   = -90.0f; // degrees; -90 looks down -Z
    float pitch = 0.0f;   // degrees; clamped to avoid gimbal flip

    float fovDegrees = 70.0f;
    float nearZ = 0.05f;
    float farZ  = 500.0f;

    // Apply a mouse movement (in pixels) scaled by sensitivity.
    void addLook(float deltaX, float deltaY, float sensitivity);

    // Full look direction (includes pitch) — used for free-fly and rendering.
    [[nodiscard]] glm::vec3 front() const;
    // Look direction flattened onto the XZ plane — used for walking so looking
    // up/down does not change walk speed.
    [[nodiscard]] glm::vec3 forwardHorizontal() const;
    [[nodiscard]] glm::vec3 rightHorizontal() const;

    [[nodiscard]] glm::mat4 viewMatrix() const;
};

} // namespace vg
