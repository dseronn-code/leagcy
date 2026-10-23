#include "util/png.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace util {

namespace {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    init = true;
  }
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.insert(v.end(), {uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)});
}

void chunk(std::ofstream& f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> len;
  put32(len, uint32_t(data.size()));
  f.write(reinterpret_cast<const char*>(len.data()), 4);
  f.write(type, 4);
  if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(type), 4);
  crc = crc32(data.data(), data.size(), crc) ^ 0xFFFFFFFFu;
  std::vector<uint8_t> tail;
  put32(tail, crc);
  f.write(reinterpret_cast<const char*>(tail.data()), 4);
}

}  // namespace

bool writePng(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  const uint8_t sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  f.write(reinterpret_cast<const char*>(sig), 8);

  std::vector<uint8_t> ihdr;
  put32(ihdr, w);
  put32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB
  chunk(f, "IHDR", ihdr);

  // Scanlines com filtro "Sub" (melhora a compressão de quem reabrir/otimizar o arquivo).
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (w * 3 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(1);
    const uint8_t* row = rgba + size_t(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) raw.push_back(uint8_t(row[x * 4 + c] - (x ? row[(x - 1) * 4 + c] : 0)));
  }

  // zlib com blocos deflate "stored" (sem compressão): simples e sempre válido.
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    size_t len = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + len >= raw.size();
    z.insert(z.end(), {uint8_t(last ? 1 : 0), uint8_t(len), uint8_t(len >> 8), uint8_t(~len), uint8_t(~len >> 8)});
    z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + len));
    pos += len;
    if (last) break;
  }
  put32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  return bool(f);
}

}  // namespace util
