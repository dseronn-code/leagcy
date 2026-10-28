#include "scene/texture_compress.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <bc7decomp.h>
#include <bc7enc.h>

#include "core/log.h"

namespace scene {

namespace {

constexpr uint32_t kCacheMagic = 0x37434C4C;  // "LLC7"
// Sobe quando o formato do cache ou a compressão mudam (invalida os arquivos antigos).
constexpr uint32_t kCacheVersion = 1;

// Tabelas sRGB <-> linear (a média de cores precisa ser feita na luz, não nos valores gama).
struct SrgbTables {
  float toLinear[256];
  SrgbTables() {
    for (int i = 0; i < 256; ++i) {
      float c = float(i) / 255.0f;
      toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
  }
  static uint8_t toSrgb(float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return uint8_t(c * 255.0f + 0.5f);
  }
};
const SrgbTables& srgbTables() {
  static const SrgbTables t;
  return t;
}

// Próximo mip por caixa 2x2 (bordas ímpares repetem o último texel).
std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, bool srgb) {
  const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
  std::vector<uint8_t> dst(size_t(nw) * nh * 4);
  const SrgbTables& t = srgbTables();
  for (uint32_t y = 0; y < nh; ++y)
    for (uint32_t x = 0; x < nw; ++x) {
      const uint32_t xs[2] = {std::min(2 * x, w - 1), std::min(2 * x + 1, w - 1)};
      const uint32_t ys[2] = {std::min(2 * y, h - 1), std::min(2 * y + 1, h - 1)};
      for (int c = 0; c < 4; ++c) {
        float sum = 0.0f;
        for (uint32_t sy : ys)
          for (uint32_t sx : xs) {
            uint8_t v = src[(size_t(sy) * w + sx) * 4 + c];
            sum += (srgb && c < 3) ? t.toLinear[v] : float(v);
          }
        sum *= 0.25f;
        dst[(size_t(y) * nw + x) * 4 + c] = (srgb && c < 3) ? SrgbTables::toSrgb(sum) : uint8_t(sum + 0.5f);
      }
    }
  return dst;
}

// Um nível em BC7, blocos divididos entre as threads.
std::vector<uint8_t> encodeLevel(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, bool srgb) {
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, blocks = bw * bh;
  std::vector<uint8_t> out(size_t(blocks) * 16);
  bc7enc_compress_block_params params;
  bc7enc_compress_block_params_init(&params);  // pesos perceptuais (YCbCr): cor
  if (!srgb) bc7enc_compress_block_params_init_linear_weights(&params);  // dados e normais: erro por canal
  std::atomic<uint32_t> next{0};
  auto work = [&]() {
    uint8_t block[16 * 4];
    for (uint32_t b = next.fetch_add(64); b < blocks; b = next.fetch_add(64))
      for (uint32_t i = b; i < std::min(b + 64, blocks); ++i) {
        const uint32_t bx = i % bw, by = i / bw;
        for (uint32_t y = 0; y < 4; ++y)
          for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
            std::memcpy(block + (y * 4 + x) * 4, &px[(size_t(sy) * w + sx) * 4], 4);
          }
        bc7enc_compress_block(&out[size_t(i) * 16], block, &params);
      }
  };
  const uint32_t threads = std::max(1u, std::min(std::thread::hardware_concurrency(), (blocks + 63) / 64));
  std::vector<std::thread> pool;
  for (uint32_t i = 1; i < threads; ++i) pool.emplace_back(work);
  work();
  for (auto& th : pool) th.join();
  return out;
}

uint64_t fnv1a(const uint8_t* data, size_t n, uint64_t h = 1469598103934665603ull) {
  for (size_t i = 0; i < n; ++i) h = (h ^ data[i]) * 1099511628211ull;
  return h;
}

bool loadCache(const std::filesystem::path& file, CompressedTexture& t) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  uint32_t header[6] = {};
  in.read(reinterpret_cast<char*>(header), sizeof(header));
  if (!in || header[0] != kCacheMagic || header[1] != kCacheVersion) return false;
  t.format = VkFormat(header[2]);
  t.width = header[3];
  t.height = header[4];
  t.mips.resize(header[5]);
  uint32_t w = t.width, h = t.height;
  for (auto& m : t.mips) {
    m.resize(size_t((w + 3) / 4) * ((h + 3) / 4) * 16);
    in.read(reinterpret_cast<char*>(m.data()), std::streamsize(m.size()));
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  return bool(in);
}

void saveCache(const std::filesystem::path& file, const CompressedTexture& t) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::filesystem::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return;
    const uint32_t header[6] = {kCacheMagic, kCacheVersion, uint32_t(t.format), t.width, t.height, uint32_t(t.mips.size())};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    for (const auto& m : t.mips) out.write(reinterpret_cast<const char*>(m.data()), std::streamsize(m.size()));
    if (!out) return;
  }
  std::filesystem::rename(tmp, file, ec);  // atômico: um cache pela metade nunca é lido
}

}  // namespace

CompressedTexture compressTexture(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb, const std::string& cacheDir) {
  static std::once_flag init;
  std::call_once(init, [] { bc7enc_compress_block_init(); });

  const uint32_t key[4] = {kCacheVersion, width, height, srgb ? 1u : 0u};
  uint64_t hash = fnv1a(reinterpret_cast<const uint8_t*>(key), sizeof(key));
  hash = fnv1a(rgba, size_t(width) * height * 4, hash);
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.bc7", static_cast<unsigned long long>(hash));
  const std::filesystem::path file = std::filesystem::path(cacheDir) / name;

  CompressedTexture t;
  if (loadCache(file, t)) return t;

  t.format = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
  t.width = width;
  t.height = height;
  std::vector<uint8_t> level(rgba, rgba + size_t(width) * height * 4);
  uint32_t w = width, h = height;
  for (;;) {
    t.mips.push_back(encodeLevel(level, w, h, srgb));
    if (w == 1 && h == 1) break;
    level = downsample(level, w, h, srgb);
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  saveCache(file, t);
  return t;
}

double compressionPsnr(const CompressedTexture& t, const uint8_t* rgba) {
  const uint32_t bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
  double se = 0.0;
  size_t n = 0;
  bc7decomp::color_rgba px[16];
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      bc7decomp::unpack_bc7(&t.mips[0][(size_t(by) * bw + bx) * 16], px);
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t sx = bx * 4 + x, sy = by * 4 + y;
          if (sx >= t.width || sy >= t.height) continue;
          for (int c = 0; c < 4; ++c) {
            const double d = double(px[y * 4 + x].m_comps[c]) - double(rgba[(size_t(sy) * t.width + sx) * 4 + c]);
            se += d * d;
          }
          n += 4;
        }
    }
  const double mse = se / double(std::max<size_t>(n, 1));
  return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace scene
