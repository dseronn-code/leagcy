#pragma once
// Tela de carregamento: a imagem do produto (logo, nome) centralizada sobre o fundo escuro e uma barra de progresso
// em degradê, desenhadas por cópias de imagem (vkCmdBlitImage) direto na imagem da janela — sem depender do
// renderizador, que só existe depois da cena carregada. Cada desenho também atende os eventos da janela (o Windows
// deixa de marcar "Não está respondendo").
#include <string>

#include "rhi/resources.h"

struct GLFWwindow;
namespace rhi {
struct Swapchain;
}

class TelaCarregamento {
 public:
  // `imagem`: PNG da tela (opcional; sem ele, só o fundo e a barra). `vsync` é o mesmo do jogo (para recriar a
  // swapchain se a janela mudar de tamanho durante a carga).
  void iniciar(rhi::Context& ctx, rhi::Swapchain& sc, VkSurfaceKHR surface, GLFWwindow* janela, const std::string& imagem,
               bool vsync);
  void desenhar(float fracao);
  void encerrar();

 private:
  rhi::Context* ctx_ = nullptr;
  rhi::Swapchain* sc_ = nullptr;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  GLFWwindow* janela_ = nullptr;
  bool vsync_ = true, blit_ = false, srgb_ = true;
  VkFence fence_ = VK_NULL_HANDLE;
  rhi::Image logo_{}, trilho_{}, barra_{};
};
