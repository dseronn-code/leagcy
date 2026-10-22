#pragma once
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace scene {

// Câmera FPS com projeção perspectiva infinita e reverse-Z
// (profundidade 1 no near, 0 no infinito): máxima precisão para mundos abertos.
struct Camera {
  glm::vec3 position{0, 2, 8};
  float yaw = -glm::half_pi<float>();  // olhando para -Z
  float pitch = 0.0f;
  float fovY = glm::radians(60.0f);
  float nearPlane = 0.1f;

  glm::vec3 forward() const {
    return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
  }
  glm::vec3 right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0))); }

  void lookAt(const glm::vec3& target) {
    glm::vec3 d = glm::normalize(target - position);
    pitch = std::asin(d.y);
    yaw = std::atan2(d.z, d.x);
  }

  glm::mat4 view() const { return glm::lookAt(position, position + forward(), glm::vec3(0, 1, 0)); }

  glm::mat4 projection(float aspect) const {
    float f = 1.0f / std::tan(fovY * 0.5f);
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = -f;  // Vulkan: Y do clip space aponta para baixo
    p[2][3] = -1.0f;
    p[3][2] = nearPlane;
    return p;
  }
};

}  // namespace scene
