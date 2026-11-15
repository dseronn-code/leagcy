#include "core/carregamento.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace carregamento {
namespace {
Desenho g_desenho;
std::thread::id g_thread;
float g_fracao = 0.0f;
size_t g_modelos = 0;
std::chrono::steady_clock::time_point g_ultimo{};

void talvezDesenhar(bool forcar) {
  if (!g_desenho || std::this_thread::get_id() != g_thread) return;
  const auto agora = std::chrono::steady_clock::now();
  if (!forcar && agora - g_ultimo < std::chrono::milliseconds(33)) return;
  g_ultimo = agora;
  g_desenho(g_fracao);
}
}  // namespace

void ligar(Desenho desenho) {
  g_desenho = std::move(desenho);
  g_thread = std::this_thread::get_id();
  g_fracao = 0.0f;
  g_modelos = 0;
  talvezDesenhar(true);
}

void desligar() {
  if (g_desenho && std::this_thread::get_id() == g_thread) {
    g_fracao = 1.0f;
    g_desenho(1.0f);
  }
  g_desenho = nullptr;
}

void progresso(float fracao) {
  if (!g_desenho) return;
  if (std::this_thread::get_id() == g_thread) g_fracao = std::max(g_fracao, std::clamp(fracao, 0.0f, 1.0f));
  talvezDesenhar(false);
}

void modeloCarregado() {
  if (!g_desenho || std::this_thread::get_id() != g_thread) return;
  ++g_modelos;
  progresso(0.03f + 0.47f * (1.0f - std::exp(-float(g_modelos) / 25.0f)));
}

void etapa(size_t i, size_t n) { progresso(0.5f + 0.45f * (n ? float(i) / float(n) : 1.0f)); }

void pulso() { talvezDesenhar(false); }

std::string imagemDaCena(const std::string& cena, const char* chave) {
  namespace fs = std::filesystem;
  if (fs::path(cena).extension() != ".json") return {};
  std::ifstream f(cena);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  const nlohmann::json doc = nlohmann::json::parse(ss.str(), nullptr, false, true);  // aceita comentários
  if (doc.is_discarded() || !doc.contains(chave) || !doc[chave].is_string()) return {};
  const std::string rel = doc[chave].get<std::string>();
  for (const fs::path& base : {fs::path(cena).parent_path(), fs::path("cenas"), fs::path(".")}) {
    std::error_code ec;
    if (fs::exists(base / rel, ec)) return (base / rel).string();
  }
  return {};
}

}  // namespace carregamento
