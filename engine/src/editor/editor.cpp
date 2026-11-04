#include "editor/editor.h"

#define GLM_ENABLE_EXPERIMENTAL  // glm/gtx (decompose)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>

#include "game/world.h"
#include "rhi/context.h"
#include "scene/camera.h"
#include "scene/scene.h"

namespace editor {

namespace {

void checkVk(VkResult r) {
  if (r != VK_SUCCESS) LOG_ERROR("Editor (ImGui/Vulkan): VkResult %d", r);
}

const char* motionLabel(game::Motion m) {
  return m == game::Motion::Static ? "estático" : m == game::Motion::Dynamic ? "dinâmico" : "cinemático";
}

std::string entityLabel(const game::World& w, entt::entity e) {
  if (const game::Name* n = w.registry.try_get<game::Name>(e); n && !n->value.empty()) return n->value;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "entidade %u", unsigned(entt::to_integral(e)));
  return buf;
}

}  // namespace

void Editor::init(rhi::Context& ctx, GLFWwindow* window, VkFormat format, uint32_t imageCount) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.IniFilename = nullptr;  // layout fixo (nada gravado na pasta do motor)
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;

  ImGui_ImplGlfw_InitForVulkan(window, true);
  format_ = format;
  ImGui_ImplVulkan_InitInfo ii{};
  ii.ApiVersion = VK_API_VERSION_1_3;
  ii.Instance = ctx.instance;
  ii.PhysicalDevice = ctx.gpu;
  ii.Device = ctx.device;
  ii.QueueFamily = ctx.queueFamily;
  ii.Queue = ctx.queue;
  ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
  ii.MinImageCount = 2;
  ii.ImageCount = std::max(imageCount, 2u);
  ii.UseDynamicRendering = true;
  ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format_;
  ii.CheckVkResultFn = checkVk;
  ready_ = ImGui_ImplVulkan_Init(&ii);
  if (!ready_) LOG_ERROR("Editor: ImGui_ImplVulkan_Init falhou");
  else LOG_INFO("Editor ligado: clique num objeto para selecionar; F1 alterna mover/girar");
}

void Editor::shutdown() {
  if (!ready_) return;
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  ready_ = false;
}

bool Editor::capturingMouse() const { return ready_ && (ImGui::GetIO().WantCaptureMouse || ImGuizmo::IsUsing()); }
bool Editor::capturingKeyboard() const { return ready_ && ImGui::GetIO().WantCaptureKeyboard; }

void Editor::frame(game::World* world, scene::Scene& scene, const scene::Camera& camera, VkExtent2D extent,
                   float frameMs) {
  if (!ready_) return;
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  ImGuizmo::BeginFrame();
  if (ImGui::IsKeyPressed(ImGuiKey_F1)) gizmoOperation_ = 1 - gizmoOperation_;

  // Painel da cena.
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("Legacy Engine");
  ImGui::Text("%.2f ms  (%.0f quadros/s)", frameMs, frameMs > 0.0f ? 1000.0f / frameMs : 0.0f);
  if (world) {
    // Modo de edição: o editor abre com a física parada. Jogar guarda as poses; Parar volta a elas.
    if (!editModeSet_) {
      world->simulate = false;
      editModeSet_ = true;
    }
    if (!playing_) {
      if (ImGui::Button("Jogar")) {
        world->snapshot();
        world->simulate = true;
        playing_ = true;
      }
    } else {
      if (ImGui::Button("Parar")) {
        world->restore(scene);
        world->simulate = false;
        playing_ = false;
      }
      ImGui::SameLine();
      if (ImGui::Button(world->simulate ? "Pausar" : "Continuar")) world->simulate = !world->simulate;
    }
    ImGui::SameLine();
    ImGui::Text("%s  t = %.1f s", playing_ ? (world->simulate ? "jogando" : "pausado") : "editando", world->time);
    ImGui::RadioButton("Mover", &gizmoOperation_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Girar", &gizmoOperation_, 1);
    ImGui::InputText("##arquivo", savePath_, sizeof(savePath_));
    ImGui::SameLine();
    if (ImGui::Button("Salvar")) status_ = world->save(savePath_) ? std::string("Salvo em ") + savePath_ : "Falha ao salvar";
    if (!status_.empty()) ImGui::TextUnformatted(status_.c_str());
  } else {
    ImGui::TextWrapped("Cena sem mundo de jogo: abra uma cena .json (ex.: --scene cenas/parque.json) para editar.");
  }
  ImGui::End();

  if (world) {
    entityList(*world);
    properties(*world, scene);
    gizmo(*world, scene, camera, extent);
    picking(*world, camera, extent);
  }
}

void Editor::entityList(game::World& world) {
  ImGui::SetNextWindowPos(ImVec2(10, 190), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_FirstUseEver);
  ImGui::Begin("Entidades");
  std::vector<entt::entity> list;
  for (auto e : world.registry.view<game::Transform>()) list.push_back(e);
  std::sort(list.begin(), list.end(), [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
  ImGui::Text("%zu entidades", list.size());
  ImGui::Separator();
  for (entt::entity e : list) {
    const std::string label = entityLabel(world, e) + "##" + std::to_string(entt::to_integral(e));
    if (ImGui::Selectable(label.c_str(), e == selected_)) selected_ = e;
  }
  ImGui::End();
}

void Editor::properties(game::World& world, scene::Scene& scene) {
  ImGui::SetNextWindowPos(ImVec2(10, 560), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("Propriedades");
  if (!world.registry.valid(selected_) || !world.registry.all_of<game::Transform>(selected_)) {
    ImGui::TextUnformatted("Nada selecionado.");
    ImGui::End();
    return;
  }
  auto& reg = world.registry;
  ImGui::TextUnformatted(entityLabel(world, selected_).c_str());
  game::Transform t = reg.get<game::Transform>(selected_);
  glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
  bool changed = ImGui::DragFloat3("Posição (m)", glm::value_ptr(t.position), 0.02f);
  if (ImGui::DragFloat3("Rotação (°)", glm::value_ptr(euler), 0.5f)) {
    t.rotation = glm::quat(glm::radians(euler));
    changed = true;
  }
  if (changed) world.setEntityPose(scene, selected_, t.position, t.rotation);
  if (const game::Shape* s = reg.try_get<game::Shape>(selected_)) {
    if (s->type == game::ShapeType::Box)
      ImGui::Text("Caixa: %.2f x %.2f x %.2f m", 2 * s->halfExtents.x, 2 * s->halfExtents.y, 2 * s->halfExtents.z);
    else if (s->type == game::ShapeType::Sphere)
      ImGui::Text("Esfera: raio %.2f m", s->radius);
    else
      ImGui::Text("Cápsula: raio %.2f m, altura %.2f m", s->radius, 2 * (s->halfHeight + s->radius));
  }
  if (game::RigidBody* b = reg.try_get<game::RigidBody>(selected_)) {
    ImGui::SeparatorText("Física");
    ImGui::Text("Tipo: %s", motionLabel(b->desc.motion));
    if (b->bodyId != ~0u && b->desc.motion == game::Motion::Dynamic) {
      const glm::vec3 v = world.physics->linearVelocity(b->bodyId);
      ImGui::Text("Velocidade: %.2f m/s%s", glm::length(v), world.physics->isActive(b->bodyId) ? "" : " (dormindo)");
      if (ImGui::Button("Empurrar para cima")) world.physics->addImpulse(b->bodyId, glm::vec3(0.0f, 6.0f, 0.0f) * std::max(b->desc.mass, 1.0f));
    }
    ImGui::DragFloat("Massa (kg)", &b->desc.mass, 0.1f, 0.0f, 1e6f);
    ImGui::DragFloat("Atrito", &b->desc.friction, 0.01f, 0.0f, 2.0f);
    ImGui::DragFloat("Quique", &b->desc.restitution, 0.01f, 0.0f, 1.0f);
    ImGui::TextDisabled("Massa, atrito e quique valem ao salvar e abrir de novo.");
  }
  if (game::Character* c = reg.try_get<game::Character>(selected_)) {
    ImGui::SeparatorText("Personagem");
    ImGui::DragFloat("Andando (m/s)", &c->walkSpeed, 0.05f, 0.0f, 30.0f);
    ImGui::DragFloat("Correndo (m/s)", &c->runSpeed, 0.05f, 0.0f, 50.0f);
    ImGui::DragFloat("Pulo (m/s)", &c->jumpSpeed, 0.05f, 0.0f, 30.0f);
    if (c->id != ~0u) ImGui::Text("No chão: %s", world.physics->characterOnGround(c->id) ? "sim" : "não");
  }
  if (const game::Model* m = reg.try_get<game::Model>(selected_)) {
    ImGui::SeparatorText("Modelo");
    ImGui::TextWrapped("%s (%u objetos)", m->path.c_str(), m->objectCount);
  }
  ImGui::End();
}

void Editor::gizmo(game::World& world, scene::Scene& scene, const scene::Camera& camera, VkExtent2D extent) {
  auto& reg = world.registry;
  if (!reg.valid(selected_) || !reg.all_of<game::Transform, game::Shape>(selected_) || reg.all_of<game::Character>(selected_))
    return;
  const game::Transform& t = reg.get<game::Transform>(selected_);
  // O ImGuizmo espera a projeção do OpenGL (Y para cima, Z de -1 a 1); a do motor é Vulkan com Z invertido.
  const float aspect = float(extent.width) / float(std::max(extent.height, 1u));
  const glm::mat4 view = camera.view();
  const glm::mat4 proj = glm::perspective(camera.fovY, aspect, 0.05f, 2000.0f);
  glm::mat4 m = glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation);
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
  ImGuizmo::SetRect(0.0f, 0.0f, float(extent.width), float(extent.height));
  const ImGuizmo::OPERATION op = gizmoOperation_ == 0 ? ImGuizmo::TRANSLATE : ImGuizmo::ROTATE;
  if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, ImGuizmo::WORLD, glm::value_ptr(m))) {
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(m, scale, rotation, translation, skew, perspective);
    world.setEntityPose(scene, selected_, translation, rotation);
  }
}

void Editor::picking(game::World& world, const scene::Camera& camera, VkExtent2D extent) {
  const ImGuiIO& io = ImGui::GetIO();
  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.WantCaptureMouse || ImGuizmo::IsOver()) return;
  // Raio da câmera pelo pixel do mouse.
  const float aspect = float(extent.width) / float(std::max(extent.height, 1u));
  const float x = 2.0f * io.MousePos.x / float(extent.width) - 1.0f;
  const float y = 1.0f - 2.0f * io.MousePos.y / float(extent.height);
  const float tanHalf = std::tan(0.5f * camera.fovY);
  const glm::vec3 fwd = camera.forward(), right = camera.right(), up = glm::cross(right, fwd);
  const glm::vec3 dir = glm::normalize(fwd + right * (x * tanHalf * aspect) + up * (y * tanHalf));
  const uint32_t body = world.physics->castRay(camera.position, dir, 2000.0f);
  selected_ = world.entityOfBody(body);
}

void Editor::selectByName(game::World& world, const std::string& name) {
  for (auto e : world.registry.view<game::Name>())
    if (world.registry.get<game::Name>(e).value == name) selected_ = e;
}

void Editor::render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
  if (!ready_) return;
  ImGui::Render();
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = target;
  att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, extent};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vkCmdBeginRendering(cmd, &ri);
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
  vkCmdEndRendering(cmd);
}

}  // namespace editor
