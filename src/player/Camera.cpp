/**
 * @file Camera.cpp
 * @brief Camera direction vectors and view-matrix computation.
 * @see docs/CODE_INDEX.md
 */

#include "player/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace vg {

void Camera::addLook(float deltaX, float deltaY, float sensitivity) {
    yaw += deltaX * sensitivity;
    // Screen Y grows downward; subtract so moving the mouse up looks up.
    pitch -= deltaY * sensitivity;
    pitch = std::clamp(pitch, -89.0f, 89.0f);
}

glm::vec3 Camera::front() const {
    const float y = glm::radians(yaw);
    const float p = glm::radians(pitch);
    return glm::normalize(glm::vec3(std::cos(y) * std::cos(p),
                                    std::sin(p),
                                    std::sin(y) * std::cos(p)));
}

glm::vec3 Camera::forwardHorizontal() const {
    const float y = glm::radians(yaw);
    return glm::normalize(glm::vec3(std::cos(y), 0.0f, std::sin(y)));
}

glm::vec3 Camera::rightHorizontal() const {
    // cross(forwardHorizontal, worldUp) -> points to the camera's right.
    return glm::normalize(glm::cross(forwardHorizontal(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(position, position + front(), glm::vec3(0.0f, 1.0f, 0.0f));
}

} // namespace vg
