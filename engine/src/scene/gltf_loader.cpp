// Carregador glTF 2.0: malhas (cada primitiva vira uma malha), hierarquia de nós,
// materiais PBR e texturas (com espaço de cor correto por uso).
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <filesystem>
#include <map>

#include <glm/gtc/type_ptr.hpp>

#include "scene/scene.h"

namespace scene {

namespace {

struct Loader {
  Scene& scene;
  cgltf_data* data;
  std::filesystem::path dir;
  std::map<std::pair<const cgltf_image*, bool>, int32_t> textureCache;  // (imagem, sRGB) -> índice
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> meshPrims;   // malha glTF -> (malha, material)
  std::vector<uint32_t> materialMap;
  uint32_t defaultMaterial = 0;

  int32_t texture(const cgltf_texture_view& view, bool srgb) {
    if (!view.texture || !view.texture->image) return -1;
    const cgltf_image* img = view.texture->image;
    auto key = std::make_pair(img, srgb);
    if (auto it = textureCache.find(key); it != textureCache.end()) return it->second;

    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = nullptr;
    if (img->buffer_view) {
      const uint8_t* src = static_cast<const uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
      pixels = stbi_load_from_memory(src, int(img->buffer_view->size), &w, &h, &comp, 4);
    } else if (img->uri) {
      std::string uri = img->uri;
      cgltf_decode_uri(uri.data());
      uri.resize(std::strlen(uri.c_str()));
      pixels = stbi_load((dir / uri).string().c_str(), &w, &h, &comp, 4);
    }
    if (!pixels) {
      LOG_WARN("Textura não carregada: %s", img->uri ? img->uri : "(embutida)");
      return textureCache[key] = -1;
    }
    TextureData t;
    t.width = uint32_t(w);
    t.height = uint32_t(h);
    t.srgb = srgb;
    t.rgba.assign(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);
    return textureCache[key] = int32_t(scene.addTexture(std::move(t)));
  }

  void loadMaterials() {
    for (size_t i = 0; i < data->materials_count; ++i) {
      const cgltf_material& src = data->materials[i];
      Material m;
      if (src.has_pbr_metallic_roughness) {
        const auto& pbr = src.pbr_metallic_roughness;
        m.baseColor = glm::make_vec4(pbr.base_color_factor);
        m.metallic = pbr.metallic_factor;
        m.roughness = pbr.roughness_factor;
        m.baseColorTex = texture(pbr.base_color_texture, true);
        m.metalRoughTex = texture(pbr.metallic_roughness_texture, false);
      }
      m.emissive = glm::make_vec3(src.emissive_factor);
      if (src.has_emissive_strength) m.emissive *= src.emissive_strength.emissive_strength;
      m.emissiveTex = texture(src.emissive_texture, true);
      m.normalTex = texture(src.normal_texture, false);
      m.normalScale = src.normal_texture.texture ? src.normal_texture.scale : 1.0f;
      if (src.alpha_mode == cgltf_alpha_mode_mask) {
        m.flags |= kMaterialAlphaMask;
        m.alphaCutoff = src.alpha_cutoff;
      } else if (src.alpha_mode == cgltf_alpha_mode_blend) {
        // Transparência real ainda não existe; trata como recorte (melhor que opaco).
        m.flags |= kMaterialAlphaMask;
        m.alphaCutoff = 0.5f;
      }
      materialMap.push_back(scene.addMaterial(m));
    }
    defaultMaterial = scene.addMaterial({});
  }

  void loadMeshes() {
    meshPrims.resize(data->meshes_count);
    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
      const cgltf_mesh& mesh = data->meshes[mi];
      for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
        const cgltf_primitive& prim = mesh.primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles) continue;
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *tan = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a) {
          const cgltf_attribute& attr = prim.attributes[a];
          if (attr.type == cgltf_attribute_type_position) pos = attr.data;
          else if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
          else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
          else if (attr.type == cgltf_attribute_type_tangent) tan = attr.data;
        }
        if (!pos) continue;
        MeshData md;
        md.vertices.resize(pos->count);
        for (size_t v = 0; v < pos->count; ++v) {
          Vertex& vx = md.vertices[v];
          cgltf_accessor_read_float(pos, v, &vx.position.x, 3);
          vx.normal = glm::vec3(0, 1, 0);
          if (nrm) cgltf_accessor_read_float(nrm, v, &vx.normal.x, 3);
          vx.uv = glm::vec2(0.0f);
          if (uv) cgltf_accessor_read_float(uv, v, &vx.uv.x, 2);
          vx.tangent = glm::vec4(1, 0, 0, 1);
          if (tan) cgltf_accessor_read_float(tan, v, &vx.tangent.x, 4);
        }
        if (prim.indices) {
          md.indices.resize(prim.indices->count);
          for (size_t i = 0; i < prim.indices->count; ++i) md.indices[i] = uint32_t(cgltf_accessor_read_index(prim.indices, i));
        } else {
          md.indices.resize(pos->count);
          for (size_t i = 0; i < pos->count; ++i) md.indices[i] = uint32_t(i);
        }
        if (!tan) computeTangents(md);
        uint32_t material = prim.material ? materialMap[size_t(prim.material - data->materials)] : defaultMaterial;
        meshPrims[mi].push_back({scene.addMesh(std::move(md)), material});
      }
    }
  }

  void loadNodes(const glm::mat4& root) {
    for (size_t i = 0; i < data->nodes_count; ++i) {
      const cgltf_node& node = data->nodes[i];
      if (!node.mesh) continue;
      float world[16];
      cgltf_node_transform_world(&node, world);
      glm::mat4 m = root * glm::make_mat4(world);
      for (auto [mesh, material] : meshPrims[size_t(node.mesh - data->meshes)]) scene.addObject(mesh, m, material);
    }
  }
};

}  // namespace

bool loadGltf(Scene& scene, const std::string& path, const glm::mat4& rootTransform) {
  cgltf_options options{};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
    LOG_ERROR("Não foi possível ler %s (rode: cmake -P tools/fetch_assets.cmake)", path.c_str());
    return false;
  }
  if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
    LOG_ERROR("Buffers de %s não carregados", path.c_str());
    cgltf_free(data);
    return false;
  }
  Loader l{scene, data, std::filesystem::path(path).parent_path(), {}, {}, {}, 0};
  l.loadMaterials();
  l.loadMeshes();
  l.loadNodes(rootTransform);
  LOG_INFO("glTF: %s (%zu nós, %zu materiais)", path.c_str(), data->nodes_count, data->materials_count);
  cgltf_free(data);
  return true;
}

}  // namespace scene
