#pragma once
#include <filesystem>

namespace core {

// Pasta do executável (jogo exportado: os arquivos do pacote ficam ao lado dele). Vazio se o sistema não disser.
std::filesystem::path executableDir();

}  // namespace core
