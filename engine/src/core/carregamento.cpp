#include "core/carregamento.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/log.h"

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
  // Trecho da carga que passou muito tempo sem atender a janela: aparece no log para achar o laço que falta avisar.
  const double parado = std::chrono::duration<double>(agora - g_ultimo).count();
  if (g_ultimo.time_since_epoch().count() && parado > 2.0)
    LOG_INFO("Tela de carregamento: %.1f s sem redesenhar (em %.0f%%)", parado, 100.0 * g_fracao);
  g_ultimo = agora;
  g_desenho(g_fracao);
}
}  // namespace

void ligar(Desenho desenho) {
  g_desenho = std::move(desenho);
  g_thread = std::this_thread::get_id();
  g_fracao = 0.0f;
  g_modelos = 0;
  g_ultimo = {};
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

void dentroDoModelo(float f) {
  if (!g_desenho || std::this_thread::get_id() != g_thread) return;
  const auto base = [](size_t n) { return 0.03f + 0.47f * (1.0f - std::exp(-float(n) / 25.0f)); };
  progresso(base(g_modelos) + (base(g_modelos + 1) - base(g_modelos)) * std::clamp(f, 0.0f, 0.95f));
}

void emParalelo(const std::function<void()>& trabalho, size_t itens) {
  const size_t n = std::min<size_t>(itens, std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
  // Com a tela ligada, a thread principal só desenha (um item enorme nela travaria a janela); sem tela, trabalha junto.
  const bool tela = g_desenho && std::this_thread::get_id() == g_thread;
  std::atomic<size_t> prontas{0};
  std::vector<std::thread> pool;
  for (size_t t = tela ? 0 : 1; t < n; ++t)
    pool.emplace_back([&] {
      trabalho();
      ++prontas;
    });
  if (!tela) trabalho();
  if (tela)
    while (prontas.load() < pool.size()) {
      talvezDesenhar(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
  for (std::thread& t : pool) t.join();
}

std::string pastaCache() {
  const char* p = std::getenv("LEGACY_CACHE");
  return p && *p ? p : "texcache";
}

void cacheUsado(const std::string& nome) {
  static const char* lista = std::getenv("LEGACY_COZINHAR");
  if (!lista || !*lista) return;
  static std::mutex m;
  std::lock_guard<std::mutex> trava(m);
  std::ofstream(lista, std::ios::app) << nome << '\n';
}

std::string textoDaCena(const std::string& cena, const char* chave) {
  namespace fs = std::filesystem;
  if (fs::path(cena).extension() != ".json") return {};
  std::ifstream f(cena);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  const nlohmann::json doc = nlohmann::json::parse(ss.str(), nullptr, false, true);  // aceita comentários
  if (doc.is_discarded() || !doc.contains(chave) || !doc[chave].is_string()) return {};
  return doc[chave].get<std::string>();
}

std::string imagemDaCena(const std::string& cena, const char* chave) {
  namespace fs = std::filesystem;
  const std::string rel = textoDaCena(cena, chave);
  if (rel.empty()) return {};
  for (const fs::path& base : {fs::path(cena).parent_path(), fs::path("cenas"), fs::path(".")}) {
    std::error_code ec;
    if (fs::exists(base / rel, ec)) return (base / rel).string();
  }
  return {};
}

}  // namespace carregamento
