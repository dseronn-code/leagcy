#pragma once
// HUD do jogo: atlas de fontes (stb_truetype) e desenho de retângulos e texto na camada 2D do renderer.
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "renderer/renderer.h"

namespace game {

enum class Font { Display = 0, Body = 1 };  // Saira Condensed ExtraBold (títulos) e Barlow SemiBold (texto)
enum class Align { Left, Center, Right };

class Ui {
 public:
  // Lê as fontes de core::gameDir() e monta o atlas (RGBA, cobertura no R). false = sem fontes (texto não aparece).
  bool load();
  const std::vector<uint8_t>& atlas() const { return atlas_; }
  uint32_t atlasSize() const { return kAtlas; }

  void begin(std::vector<renderer::Renderer::UiVertex>* out, glm::vec2 screen) { out_ = out; screen_ = screen; }
  glm::vec2 screen() const { return screen_; }
  // Cores em sRGB (0..1) com alfa.
  void rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color);
  void frame(glm::vec2 pos, glm::vec2 size, float thickness, glm::vec4 color);  // só a borda
  // Texto UTF-8 (acentos do português); `px` = altura da fonte em pixels; y = linha de base. Devolve a largura.
  float text(const std::string& s, glm::vec2 pos, float px, glm::vec4 color, Font font = Font::Body, Align align = Align::Left);
  float measure(const std::string& s, float px, Font font = Font::Body) const;
  // Texto com sombra suave por baixo (legível sobre a imagem do estádio).
  float textShadow(const std::string& s, glm::vec2 pos, float px, glm::vec4 color, Font font = Font::Body, Align align = Align::Left);

 private:
  static constexpr uint32_t kAtlas = 2048;
  static constexpr float kBakePx = 72.0f;
  struct Glyph { float x0, y0, x1, y1, xoff, yoff, xoff2, yoff2, advance; };
  std::vector<uint8_t> atlas_;
  std::vector<Glyph> glyphs_[2];  // códigos 32..255
  bool loaded_ = false;
  std::vector<renderer::Renderer::UiVertex>* out_ = nullptr;
  glm::vec2 screen_{1.0f};
  void quad(glm::vec2 a, glm::vec2 b, glm::vec2 uva, glm::vec2 uvb, glm::vec4 color);
};

}  // namespace game
