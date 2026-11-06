#include "core/paths.h"

namespace core {
namespace {
std::string& shaders() { static std::string s = SHADER_DIR; return s; }
std::string& game() { static std::string s = GAME_DIR; return s; }
}  // namespace
const std::string& shaderDir() { return shaders(); }
const std::string& gameDir() { return game(); }
void setDataDirs(const std::string& s, const std::string& g) { shaders() = s; game() = g; }
}  // namespace core
