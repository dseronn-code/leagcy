#include "core/paths.h"

#include "core/log.h"

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace core {
namespace {
// Pasta dos shaders: "shaders/" ao lado do executável (jogo exportado por tools/empacotar.py) ou, sem ela, a pasta
// do build (SHADER_DIR, caminho fixado na compilação). No Android o android_main troca por setDataDirs.
std::string defaultShaderDir() {
  const std::filesystem::path exe = executableDir();
  if (!exe.empty()) {
    const std::filesystem::path local = exe / "shaders";
    std::error_code ec;
    if (std::filesystem::exists(local / "lighting.comp.spv", ec) &&
        !std::filesystem::equivalent(local, std::filesystem::path(SHADER_DIR), ec)) {
      LOG_INFO("Shaders do pacote: %s", local.string().c_str());
      return local.string();
    }
  }
  return std::string(SHADER_DIR);
}
std::string& shaders() { static std::string s = defaultShaderDir(); return s; }
std::string& game() { static std::string s = GAME_DIR; return s; }
}  // namespace
const std::string& shaderDir() { return shaders(); }
const std::string& gameDir() { return game(); }
void setDataDirs(const std::string& s, const std::string& g) { shaders() = s; game() = g; }

std::filesystem::path executableDir() {
  static const std::filesystem::path dir = [] {
    std::filesystem::path exe;
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n > 0 && n < buf.size()) exe = std::filesystem::path(std::wstring(buf.data(), n));
#else
    std::error_code ec;
    exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    return exe.empty() ? exe : exe.parent_path();
  }();
  return dir;
}

}  // namespace core
