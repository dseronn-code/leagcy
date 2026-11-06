#pragma once
// Pastas de dados do motor em tempo de execução. No desktop vêm do build (SHADER_DIR, GAME_DIR); no Android, da
// pasta interna do app, para onde os arquivos do APK são copiados na primeira abertura.
#include <string>

namespace core {
const std::string& shaderDir();
const std::string& gameDir();
void setDataDirs(const std::string& shaders, const std::string& game);
}  // namespace core
