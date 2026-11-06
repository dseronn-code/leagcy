// Gera um glTF com as malhas comprimidas (EXT_meshopt_compression) e, opcionalmente, normais e UV quantizadas
// (KHR_mesh_quantization), a partir de um .gltf com .bin. Serve para testar o carregador do motor com assets no
// formato do gltfpack. Compilar (de engine/, depois do build):
//   g++ -O2 -std=c++17 tools/gltf_meshopt.cpp -Ibuild/_deps/meshoptimizer-src/src
//       -Ibuild/_deps/nlohmann_json-src/single_include build/_deps/meshoptimizer-build/libmeshoptimizer.a -o gltf_meshopt
// Uso: gltf_meshopt entrada.gltf saida.gltf [--quantizar]
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

static size_t compSize(int t) { return t == 5126 || t == 5125 ? 4 : t == 5123 || t == 5122 ? 2 : 1; }
static size_t numComps(const std::string& t) {
  return t == "SCALAR" ? 1 : t == "VEC2" ? 2 : t == "VEC3" ? 3 : t == "VEC4" ? 4 : t == "MAT4" ? 16 : 0;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "uso: gltf_meshopt entrada.gltf saida.gltf [--quantizar]\n";
    return 1;
  }
  const fs::path in = argv[1], out = argv[2];
  const bool quantize = argc > 3 && std::string(argv[3]) == "--quantizar";
  json g = json::parse(std::ifstream(in));
  if (g["buffers"].size() != 1) {
    std::cerr << "só glTF com um buffer\n";
    return 1;
  }
  std::ifstream bf(in.parent_path() / g["buffers"][0]["uri"].get<std::string>(), std::ios::binary);
  std::vector<uint8_t> bin((std::istreambuf_iterator<char>(bf)), {});

  // Views usadas por atributos de vértice e por índices.
  std::map<int, std::string> viewRole;  // "attr" / "index"
  std::map<int, int> viewAccessor;
  for (auto& m : g["meshes"])
    for (auto& p : m["primitives"]) {
      for (auto& [name, acc] : p["attributes"].items()) {
        const int v = g["accessors"][acc.get<int>()]["bufferView"];
        viewRole[v] = "attr";
        viewAccessor[v] = acc;
      }
      if (p.contains("indices")) {
        const int v = g["accessors"][p["indices"].get<int>()]["bufferView"];
        viewRole[v] = "index";
        viewAccessor[v] = p["indices"];
      }
    }

  // Quantização: normais -> int16 normalizado (VEC3 em 8 bytes), UV -> uint16 normalizado (UV no intervalo [0, 1]).
  std::vector<uint8_t> extra;  // dados novos (views quantizadas), anexados ao fim do buffer original
  if (quantize) {
    for (auto& m : g["meshes"])
      for (auto& p : m["primitives"])
        for (auto& [name, accj] : p["attributes"].items()) {
          if (name != "NORMAL" && name != "TEXCOORD_0") continue;
          json& acc = g["accessors"][accj.get<int>()];
          if (acc["componentType"] != 5126) continue;
          const json& view = g["bufferViews"][acc["bufferView"].get<int>()];
          const size_t n = acc["count"], nc = numComps(acc["type"]);
          const size_t off = view.value("byteOffset", 0) + acc.value("byteOffset", 0);
          const size_t stride = view.value("byteStride", int(nc * 4));
          bool ok = true;
          std::vector<uint8_t> q;
          const size_t qs = name == "NORMAL" ? 8 : 4;
          q.resize(n * qs);
          for (size_t i = 0; i < n && ok; ++i) {
            float f[3];
            std::memcpy(f, &bin[off + i * stride], nc * 4);
            for (size_t c = 0; c < nc; ++c) {
              if (name == "NORMAL") {
                const int16_t v = int16_t(std::lround(std::fmax(-1.f, std::fmin(1.f, f[c])) * 32767.f));
                std::memcpy(&q[i * qs + c * 2], &v, 2);
              } else {
                if (f[c] < 0.f || f[c] > 1.f) ok = false;
                const uint16_t v = uint16_t(std::lround(f[c] * 65535.f));
                std::memcpy(&q[i * qs + c * 2], &v, 2);
              }
            }
          }
          if (!ok) continue;
          while ((bin.size() + extra.size()) % 4) extra.push_back(0);
          const size_t newOff = bin.size() + extra.size();
          extra.insert(extra.end(), q.begin(), q.end());
          json nv = {{"buffer", 0}, {"byteOffset", newOff}, {"byteLength", q.size()}, {"byteStride", qs}};
          g["bufferViews"].push_back(nv);
          const int vi = int(g["bufferViews"].size()) - 1;
          acc["bufferView"] = vi;
          acc.erase("byteOffset");
          acc["componentType"] = name == "NORMAL" ? 5122 : 5123;
          acc["normalized"] = true;
          if (acc.contains("min")) acc.erase("min");
          if (acc.contains("max")) acc.erase("max");
          viewRole[vi] = "attr";
          viewAccessor[vi] = accj;
        }
    bin.insert(bin.end(), extra.begin(), extra.end());
    g["buffers"][0]["byteLength"] = bin.size();
  }

  // Comprime cada view de geometria; o buffer original vira o "fallback" (sem dados) e um buffer novo guarda os
  // dados comprimidos (as imagens continuam lendo o buffer original, que fica como buffer 0).
  std::vector<uint8_t> packed;
  size_t raw = 0;
  for (auto& [v, role] : viewRole) {
    json& view = g["bufferViews"][v];
    const json& acc = g["accessors"][viewAccessor[v]];
    const size_t count = acc["count"];
    const size_t elem = numComps(acc["type"]) * compSize(acc["componentType"]);
    const size_t stride = view.value("byteStride", int(elem));
    const size_t off = view.value("byteOffset", 0);
    std::vector<uint8_t> enc;
    std::string mode;
    if (role == "index") {
      if (count % 3 || (stride != 2 && stride != 4)) continue;
      std::vector<uint32_t> idx(count);
      for (size_t i = 0; i < count; ++i) {
        uint32_t x = 0;
        std::memcpy(&x, &bin[off + i * stride], stride);
        idx[i] = x;
      }
      enc.resize(meshopt_encodeIndexBufferBound(count, count));
      enc.resize(meshopt_encodeIndexBuffer(enc.data(), enc.size(), idx.data(), count));
      mode = "TRIANGLES";
    } else {
      if (stride % 4 || stride > 256) continue;
      enc.resize(meshopt_encodeVertexBufferBound(count, stride));
      enc.resize(meshopt_encodeVertexBuffer(enc.data(), enc.size(), &bin[off], count, stride));
      mode = "ATTRIBUTES";
    }
    while (packed.size() % 4) packed.push_back(0);
    const size_t poff = packed.size();
    packed.insert(packed.end(), enc.begin(), enc.end());
    raw += count * stride;
    // Zera os dados originais: o arquivo só funciona se o leitor descomprimir de verdade (prova do teste).
    std::memset(&bin[off], 0, std::min(count * stride, bin.size() - off));
    view["byteStride"] = stride;
    if (role == "index") view.erase("byteStride");
    view["extensions"]["EXT_meshopt_compression"] = {{"buffer", 1}, {"byteOffset", poff}, {"byteLength", enc.size()},
                                                     {"byteStride", stride}, {"count", count}, {"mode", mode}};
  }
  const std::string base = out.stem().string();
  g["buffers"][0]["uri"] = base + ".bin";
  g["buffers"].push_back({{"uri", base + ".meshopt.bin"}, {"byteLength", packed.size()}});
  for (const char* ext : {"EXT_meshopt_compression", "KHR_mesh_quantization"}) {
    if (std::string(ext) == "KHR_mesh_quantization" && !quantize) continue;
    for (const char* list : {"extensionsUsed", "extensionsRequired"}) {
      if (!g.contains(list)) g[list] = json::array();
      bool has = false;
      for (auto& e : g[list]) has |= e == ext;
      if (!has) g[list].push_back(ext);
    }
  }
  // Imagens: copia ao lado (mesmos nomes).
  for (auto& im : g.value("images", json::array()))
    if (im.contains("uri")) {
      std::error_code ec;
      fs::create_directories((out.parent_path() / im["uri"].get<std::string>()).parent_path(), ec);
      fs::copy_file(in.parent_path() / im["uri"].get<std::string>(), out.parent_path() / im["uri"].get<std::string>(),
                    fs::copy_options::overwrite_existing, ec);
    }
  std::ofstream(out.parent_path() / (base + ".bin"), std::ios::binary).write(reinterpret_cast<char*>(bin.data()), std::streamsize(bin.size()));
  std::ofstream(out.parent_path() / (base + ".meshopt.bin"), std::ios::binary)
      .write(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size()));
  std::ofstream(out) << g.dump(1);
  std::cout << "geometria: " << raw << " bytes -> " << packed.size() << " comprimidos ("
            << (100.0 * double(packed.size()) / double(raw ? raw : 1)) << "%)" << (quantize ? ", quantizada" : "") << "\n";
  return 0;
}
