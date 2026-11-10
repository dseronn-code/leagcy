#include <cmath>
#include "game/ui.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "core/log.h"
#include "core/paths.h"

namespace game {

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Próximo código do texto UTF-8 (fora de 32..255 vira '?').
uint32_t nextCodepoint(const std::string& s, size_t& i) {
  const auto c = uint8_t(s[i++]);
  uint32_t cp = c;
  if (c >= 0xC0 && c < 0xE0 && i < s.size()) cp = ((c & 0x1Fu) << 6) | (uint8_t(s[i++]) & 0x3Fu);
  else if (c >= 0xE0 && c < 0xF0 && i + 1 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((uint8_t(s[i]) & 0x3Fu) << 6) | (uint8_t(s[i + 1]) & 0x3Fu); i += 2; }
  else if (c >= 0xF0) { while (i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80) ++i; cp = '?'; }
  return (cp >= 32 && cp <= 255) ? cp : uint32_t('?');
}

}  // namespace

bool Ui::load() {
  const std::string files[2] = {core::gameDir() + "/SairaCondensed-ExtraBold.ttf", core::gameDir() + "/Barlow-SemiBold.ttf"};
  std::vector<uint8_t> ttf[2];
  for (int f = 0; f < 2; ++f) {
    ttf[f] = readFile(files[f]);
    if (ttf[f].empty()) { LOG_WARN("HUD: fonte não encontrada: %s", files[f].c_str()); return false; }
  }
  std::vector<uint8_t> alpha(size_t(kAtlas) * kAtlas, 0);
  stbtt_pack_context pc;
  if (!stbtt_PackBegin(&pc, alpha.data(), int(kAtlas), int(kAtlas), 0, 2, nullptr)) return false;
  stbtt_PackSetOversampling(&pc, 2, 2);
  for (int f = 0; f < 2; ++f) {
    std::vector<stbtt_packedchar> pcs(224);
    if (!stbtt_PackFontRange(&pc, ttf[f].data(), 0, kBakePx, 32, 224, pcs.data())) { stbtt_PackEnd(&pc); LOG_WARN("HUD: atlas pequeno"); return false; }
    glyphs_[f].resize(224);
    for (int i = 0; i < 224; ++i) {
      const stbtt_packedchar& p = pcs[size_t(i)];
      glyphs_[f][size_t(i)] = {float(p.x0) / kAtlas, float(p.y0) / kAtlas, float(p.x1) / kAtlas, float(p.y1) / kAtlas,
                               p.xoff, p.yoff, p.xoff2, p.yoff2, p.xadvance};
    }
  }
  stbtt_PackEnd(&pc);
  atlas_.assign(size_t(kAtlas) * kAtlas * 4, 255);
  for (size_t i = 0; i < alpha.size(); ++i) atlas_[i * 4] = alpha[i];
  loaded_ = true;
  return true;
}

void Ui::quad(glm::vec2 a, glm::vec2 b, glm::vec2 uva, glm::vec2 uvb, glm::vec4 color) {
  if (!out_) return;
  using V = renderer::Renderer::UiVertex;
  const V v0{a, uva, color}, v1{{b.x, a.y}, {uvb.x, uva.y}, color}, v2{{a.x, b.y}, {uva.x, uvb.y}, color}, v3{b, uvb, color};
  out_->insert(out_->end(), {v0, v1, v2, v2, v1, v3});
}

void Ui::rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color) { quad(pos, pos + size, {-1, -1}, {-1, -1}, color); }

void Ui::line(glm::vec2 a, glm::vec2 b, float t, glm::vec4 color) {
  if (!out_) return;
  using V = renderer::Renderer::UiVertex;
  glm::vec2 d = b - a;
  const float l = glm::length(d);
  if (l < 1e-3f) return;
  const glm::vec2 n = glm::vec2(-d.y, d.x) / l * (0.5f * t);
  const V v0{a + n, {-1, -1}, color}, v1{b + n, {-1, -1}, color}, v2{a - n, {-1, -1}, color}, v3{b - n, {-1, -1}, color};
  out_->insert(out_->end(), {v0, v1, v2, v2, v1, v3});
}

void Ui::triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec4 color) {
  if (!out_) return;
  using V = renderer::Renderer::UiVertex;
  out_->insert(out_->end(), {V{a, {-1, -1}, color}, V{b, {-1, -1}, color}, V{c, {-1, -1}, color}});
}

void Ui::disc(glm::vec2 c, float r, glm::vec4 color, int n) {
  if (!out_) return;
  using V = renderer::Renderer::UiVertex;
  for (int i = 0; i < n; ++i) {
    const float a0 = 6.2831853f * float(i) / float(n), a1 = 6.2831853f * float(i + 1) / float(n);
    out_->insert(out_->end(), {V{c, {-1, -1}, color}, V{c + r * glm::vec2(std::cos(a0), std::sin(a0)), {-1, -1}, color},
                               V{c + r * glm::vec2(std::cos(a1), std::sin(a1)), {-1, -1}, color}});
  }
}

void Ui::ring(glm::vec2 c, float r, float t, glm::vec4 color, int n) {
  if (!out_) return;
  using V = renderer::Renderer::UiVertex;
  for (int i = 0; i < n; ++i) {
    const float a0 = 6.2831853f * float(i) / float(n), a1 = 6.2831853f * float(i + 1) / float(n);
    const glm::vec2 d0(std::cos(a0), std::sin(a0)), d1(std::cos(a1), std::sin(a1));
    const V o0{c + d0 * r, {-1, -1}, color}, o1{c + d1 * r, {-1, -1}, color}, i0{c + d0 * (r - t), {-1, -1}, color}, i1{c + d1 * (r - t), {-1, -1}, color};
    out_->insert(out_->end(), {o0, o1, i0, i0, o1, i1});
  }
}

void Ui::frame(glm::vec2 pos, glm::vec2 size, float t, glm::vec4 color) {
  rect(pos, {size.x, t}, color);
  rect({pos.x, pos.y + size.y - t}, {size.x, t}, color);
  rect({pos.x, pos.y + t}, {t, size.y - 2 * t}, color);
  rect({pos.x + size.x - t, pos.y + t}, {t, size.y - 2 * t}, color);
}

float Ui::measure(const std::string& s, float px, Font font) const {
  if (!loaded_) return 0.0f;
  const auto& g = glyphs_[int(font)];
  float w = 0.0f;
  for (size_t i = 0; i < s.size();) w += g[nextCodepoint(s, i) - 32].advance;
  return w * px / kBakePx;
}

float Ui::text(const std::string& s, glm::vec2 pos, float px, glm::vec4 color, Font font, Align align) {
  if (!loaded_) return 0.0f;
  const float w = measure(s, px, font), k = px / kBakePx;
  float x = pos.x - (align == Align::Center ? 0.5f * w : align == Align::Right ? w : 0.0f);
  const auto& g = glyphs_[int(font)];
  for (size_t i = 0; i < s.size();) {
    const Glyph& q = g[nextCodepoint(s, i) - 32];
    if (q.x1 > q.x0) quad({x + q.xoff * k, pos.y + q.yoff * k}, {x + q.xoff2 * k, pos.y + q.yoff2 * k}, {q.x0, q.y0}, {q.x1, q.y1}, color);
    x += q.advance * k;
  }
  return w;
}

float Ui::textShadow(const std::string& s, glm::vec2 pos, float px, glm::vec4 color, Font font, Align align) {
  const glm::vec4 shadow(0.0f, 0.0f, 0.0f, 0.55f * color.a);
  const float o = std::max(1.5f, px * 0.05f);
  text(s, pos + glm::vec2(o, o), px, shadow, font, align);
  return text(s, pos, px, color, font, align);
}

}  // namespace game
