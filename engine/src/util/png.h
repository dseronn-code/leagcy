#pragma once
#include <cstdint>
#include <string>

namespace util {

// Grava PNG RGB 8 bits a partir de pixels RGBA (alfa descartado). Sem dependências externas.
bool writePng(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace util
