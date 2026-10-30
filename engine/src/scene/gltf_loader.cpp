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

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <string>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "scene/scene.h"

namespace scene {

namespace {

constexpr float kGltfEmissiveNits = 1000.0f;

struct Loader {
  Scene& scene;
  cgltf_data* data;
  std::filesystem::path dir;
  // (imagem, sRGB, repetição u, repetição v) -> índice
  std::map<std::tuple<const cgltf_image*, bool, int, int>, int32_t> textureCache;
  struct Primitive {
    MeshData data;
    uint32_t material = 0;
    int sceneMesh = -1;  // malha compartilhada (instâncias sem esqueleto)
  };
  std::vector<std::vector<Primitive>> meshPrims;  // malha glTF -> primitivas
  std::vector<uint32_t> materialMap;
  uint32_t defaultMaterial = 0;
  int model = -1;  // índice do Model criado (arquivos com animação ou esqueleto)

  // Repetição do sampler do glTF (códigos do OpenGL: 33071 = clamp, 33648 = espelhado, 10497 = repetir).
  static void setWrap(TextureData& t, const cgltf_sampler* smp) {
    auto mode = [](int gl) {
      return gl == 33071 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
           : gl == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                         : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    if (!smp) return;
    t.wrapU = mode(smp->wrap_s);
    t.wrapV = mode(smp->wrap_t);
  }

  // Imagem alternativa em DDS declarada por MSFT_texture_dds ({"source": índice}), ou nulo.
  const cgltf_image* ddsImage(const cgltf_texture& tex) const {
    for (size_t i = 0; i < tex.extensions_count; ++i) {
      if (std::strcmp(tex.extensions[i].name, "MSFT_texture_dds") != 0 || !tex.extensions[i].data) continue;
      const char* p = std::strstr(tex.extensions[i].data, "\"source\"");
      if (!p || !(p = std::strchr(p, ':'))) return nullptr;
      const long index = std::strtol(p + 1, nullptr, 10);
      return index >= 0 && size_t(index) < data->images_count ? &data->images[index] : nullptr;
    }
    return nullptr;
  }

  int32_t texture(const cgltf_texture_view& view, bool srgb, bool normalMap = false) {
    if (!view.texture || !view.texture->image) return -1;
    const cgltf_image* img = view.texture->image;
    const cgltf_sampler* smp = view.texture->sampler;
    auto key = std::make_tuple(img, srgb, smp ? smp->wrap_s : 10497, smp ? smp->wrap_t : 10497);
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
      // Imagem original ausente: usa a versão DDS (MSFT_texture_dds), se houver — o Bistro só traz as DDS em BC7.
      TextureData t;
      const cgltf_image* dds = ddsImage(*view.texture);
      if (dds && dds->uri) {
        std::string uri = dds->uri;
        cgltf_decode_uri(uri.data());
        uri.resize(std::strlen(uri.c_str()));
        if (loadDds((dir / uri).string(), t.precompressed)) {
          t.width = t.precompressed.width;
          t.height = t.precompressed.height;
          t.srgb = srgb;
          t.normalMap = normalMap;
          t.name = dds->uri;
          setWrap(t, smp);
          return textureCache[key] = int32_t(scene.addTexture(std::move(t)));
        }
      }
      LOG_WARN("Textura não carregada: %s", img->uri ? img->uri : "(embutida)");
      return textureCache[key] = -1;
    }
    TextureData t;
    t.width = uint32_t(w);
    t.height = uint32_t(h);
    t.srgb = srgb;
    t.normalMap = normalMap;
    t.name = img->uri ? img->uri : (img->name ? img->name : "(embutida)");
    setWrap(t, smp);
    t.rgba.assign(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);
    return textureCache[key] = int32_t(scene.addTexture(std::move(t)));
  }

  void loadMaterials() {
    for (size_t i = 0; i < data->materials_count; ++i) {
      const cgltf_material& src = data->materials[i];
      Material m;
      if (src.has_pbr_specular_glossiness) {
        // Fluxo especular/glossiness: usado nativamente (sem conversão com perda).
        const auto& sg = src.pbr_specular_glossiness;
        m.flags |= kMaterialSpecularGlossiness;
        m.baseColor = glm::make_vec4(sg.diffuse_factor);
        m.baseColorTex = texture(sg.diffuse_texture, true);
        m.specularColor = glm::make_vec3(sg.specular_factor);
        m.roughness = 1.0f - sg.glossiness_factor;
        m.metalRoughTex = texture(sg.specular_glossiness_texture, true);  // RGB em sRGB, A linear
      } else if (src.has_pbr_metallic_roughness) {
        const auto& pbr = src.pbr_metallic_roughness;
        m.baseColor = glm::make_vec4(pbr.base_color_factor);
        m.metallic = pbr.metallic_factor;
        m.roughness = pbr.roughness_factor;
        m.baseColorTex = texture(pbr.base_color_texture, true);
        m.metalRoughTex = texture(pbr.metallic_roughness_texture, false);
      }
      if (src.has_specular) {
        m.specular = src.specular.specular_factor;
        m.specularColor = glm::make_vec3(src.specular.specular_color_factor);
        m.specularTex = texture(src.specular.specular_texture, false);
        m.specularColorTex = texture(src.specular.specular_color_texture, true);
      }
      if (src.has_ior) m.ior = src.ior.ior;
      if (src.double_sided) m.flags |= kMaterialDoubleSided;
      if (src.has_clearcoat && src.clearcoat.clearcoat_factor > 0.0f) {
        m.flags |= kMaterialClearcoat;
        m.clearcoat = src.clearcoat.clearcoat_factor;
        m.clearcoatRoughness = src.clearcoat.clearcoat_roughness_factor;
        m.clearcoatTex = texture(src.clearcoat.clearcoat_texture, false);
        m.clearcoatRoughnessTex = texture(src.clearcoat.clearcoat_roughness_texture, false);
      }
      {  // KHR_texture_transform da cor base (ou, sem ela, da primeira textura com transformação)
        const cgltf_texture_view* views[] = {&src.pbr_metallic_roughness.base_color_texture, &src.pbr_specular_glossiness.diffuse_texture,
                                             &src.normal_texture, &src.emissive_texture};
        for (const cgltf_texture_view* v : views)
          if (v->texture && v->has_transform) {
            m.uvOffset = glm::make_vec2(v->transform.offset);
            m.uvScale = glm::make_vec2(v->transform.scale);
            m.uvRotation = v->transform.rotation;
            break;
          }
      }
      if (src.has_volume && src.volume.thickness_factor > 0.0f) {
        m.thickness = src.volume.thickness_factor;
        m.attenuationColor = glm::make_vec3(src.volume.attenuation_color);
        m.attenuationDistance = std::isfinite(src.volume.attenuation_distance) ? src.volume.attenuation_distance : 0.0f;
      }
      if (src.has_transmission && src.transmission.transmission_factor > 0.0f) {
        m.flags |= kMaterialTransmissive;
        m.transmission = src.transmission.transmission_factor;
        m.transmissionTex = texture(src.transmission.transmission_texture, false);
      }
      // glTF não define unidade para emissão; 1,0 = 1.000 nits (tela/letreiro típico).
      m.emissive = glm::make_vec3(src.emissive_factor) * kGltfEmissiveNits;
      if (src.has_emissive_strength) m.emissive *= src.emissive_strength.emissive_strength;
      m.emissiveTex = texture(src.emissive_texture, true);
      m.normalTex = texture(src.normal_texture, false, true);
      m.occlusionTex = texture(src.occlusion_texture, false);
      m.normalScale = src.normal_texture.texture ? src.normal_texture.scale : 1.0f;
      if (src.alpha_mode == cgltf_alpha_mode_mask) {
        m.flags |= kMaterialAlphaMask;
        m.alphaCutoff = src.alpha_cutoff;
      } else if (src.alpha_mode == cgltf_alpha_mode_blend) {
        // Misturado no G-buffer (passe de decals); nos raios, recorte por alfa.
        m.flags |= kMaterialAlphaMask | kMaterialBlend;
        m.alphaCutoff = 0.5f;
      }
      if (m.flags & kMaterialAlphaMask) scene.setTextureAlphaCutoff(m.baseColorTex, m.alphaCutoff);
      // Recorte por alfa + dois lados: folhas, galhos, cortinas — balançam com o vento.
      if ((m.flags & kMaterialAlphaMask) && (m.flags & kMaterialDoubleSided)) m.flags |= kMaterialFoliage;
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
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *tan = nullptr, *jnt = nullptr, *wgt = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a) {
          const cgltf_attribute& attr = prim.attributes[a];
          if (attr.type == cgltf_attribute_type_position) pos = attr.data;
          else if (attr.type == cgltf_attribute_type_normal) nrm = attr.data;
          else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv = attr.data;
          else if (attr.type == cgltf_attribute_type_tangent) tan = attr.data;
          else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) jnt = attr.data;
          else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) wgt = attr.data;
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
        if (jnt && wgt) {
          md.joints.resize(pos->count);
          md.weights.resize(pos->count);
          for (size_t v = 0; v < pos->count; ++v) {
            cgltf_uint j[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(jnt, v, j, 4);
            md.joints[v] = glm::uvec4(j[0], j[1], j[2], j[3]);
            glm::vec4 w(0.0f);
            cgltf_accessor_read_float(wgt, v, &w.x, 4);
            float sum = w.x + w.y + w.z + w.w;
            md.weights[v] = sum > 0.0f ? w / sum : glm::vec4(1, 0, 0, 0);
          }
        }
        if (!nrm) computeNormals(md);
        if (!tan) computeTangents(md);
        uint32_t material = prim.material ? materialMap[size_t(prim.material - data->materials)] : defaultMaterial;
        meshPrims[mi].push_back({std::move(md), material, -1});
      }
    }
  }

  // Hierarquia, esqueletos e clipes — só para arquivos animados.
  void loadModel(const glm::mat4& root) {
    if (data->animations_count == 0 && data->skins_count == 0) return;
    Model m;
    m.root = root;
    auto nodeIndex = [&](const cgltf_node* n) { return uint32_t(n - data->nodes); };
    m.nodes.resize(data->nodes_count);
    for (size_t i = 0; i < data->nodes_count; ++i) {
      const cgltf_node& n = data->nodes[i];
      ModelNode& mn = m.nodes[i];
      mn.parent = n.parent ? int(nodeIndex(n.parent)) : -1;
      if (n.has_matrix) {
        glm::mat4 mat = glm::make_mat4(n.matrix);
        glm::vec3 skew;
        glm::vec4 persp;
        glm::decompose(mat, mn.scale, mn.rotation, mn.translation, skew, persp);
      } else {
        if (n.has_translation) mn.translation = glm::make_vec3(n.translation);
        if (n.has_rotation) mn.rotation = glm::quat(n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]);
        if (n.has_scale) mn.scale = glm::make_vec3(n.scale);
      }
    }
    for (size_t i = 0; i < data->skins_count; ++i) {
      const cgltf_skin& sk = data->skins[i];
      Skin skin;
      for (size_t j = 0; j < sk.joints_count; ++j) {
        skin.joints.push_back(nodeIndex(sk.joints[j]));
        glm::mat4 ibm(1.0f);
        if (sk.inverse_bind_matrices) cgltf_accessor_read_float(sk.inverse_bind_matrices, j, &ibm[0][0], 16);
        skin.inverseBind.push_back(ibm);
      }
      m.skins.push_back(std::move(skin));
    }
    for (size_t i = 0; i < data->animations_count; ++i) {
      const cgltf_animation& src = data->animations[i];
      Animation a;
      a.name = src.name ? src.name : ("anim" + std::to_string(i));
      for (size_t si = 0; si < src.samplers_count; ++si) {
        const cgltf_animation_sampler& smp = src.samplers[si];
        AnimationSampler as;
        as.step = smp.interpolation == cgltf_interpolation_type_step;
        bool cubic = smp.interpolation == cgltf_interpolation_type_cubic_spline;
        as.times.resize(smp.input->count);
        for (size_t k = 0; k < smp.input->count; ++k) cgltf_accessor_read_float(smp.input, k, &as.times[k], 1);
        size_t comps = cgltf_num_components(smp.output->type);
        as.values.resize(as.times.size(), glm::vec4(0.0f));
        for (size_t k = 0; k < as.times.size(); ++k)  // CUBICSPLINE: (tangente, valor, tangente) por chave
          cgltf_accessor_read_float(smp.output, cubic ? k * 3 + 1 : k, &as.values[k].x, std::min<size_t>(comps, 4));
        if (!as.times.empty()) a.duration = std::max(a.duration, as.times.back());
        a.samplers.push_back(std::move(as));
      }
      for (size_t ci = 0; ci < src.channels_count; ++ci) {
        const cgltf_animation_channel& ch = src.channels[ci];
        if (!ch.target_node) continue;
        AnimationChannel c;
        c.node = nodeIndex(ch.target_node);
        c.sampler = uint32_t(ch.sampler - src.samplers);
        if (ch.target_path == cgltf_animation_path_type_translation) c.path = AnimationChannel::kTranslation;
        else if (ch.target_path == cgltf_animation_path_type_rotation) c.path = AnimationChannel::kRotation;
        else if (ch.target_path == cgltf_animation_path_type_scale) c.path = AnimationChannel::kScale;
        else continue;  // morph targets ainda não suportados
        a.channels.push_back(c);
      }
      m.animations.push_back(std::move(a));
    }
    scene.models.push_back(std::move(m));
    model = int(scene.models.size() - 1);
  }

  // KHR_lights_punctual: pontuais e spots viram luzes do ReSTIR (intensidade em candela, como no motor); a direcional
  // dá a direção do sol (a iluminância continua a da atmosfera física). A luz aponta para -Z do nó.
  uint32_t loadLights(const glm::mat4& root) {
    uint32_t count = 0;
    for (size_t i = 0; i < data->nodes_count; ++i) {
      const cgltf_node& node = data->nodes[i];
      if (!node.light) continue;
      float world[16];
      cgltf_node_transform_world(&node, world);
      const glm::mat4 m = root * glm::make_mat4(world);
      const cgltf_light& src = *node.light;
      const glm::vec3 axis = glm::normalize(glm::vec3(m * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
      if (src.type == cgltf_light_type_directional) {
        scene.sunDirection = -axis;
        scene.sunFromAsset = true;
        continue;
      }
      PointLight l;
      l.position = glm::vec3(m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
      l.intensity = glm::make_vec3(src.color) * src.intensity;
      // Sem alcance no arquivo (= infinito): corta onde a iluminância cai a 0,01 lux.
      const float peak = std::max({l.intensity.r, l.intensity.g, l.intensity.b});
      l.range = src.range > 0.0f ? src.range : std::max(1.0f, std::sqrt(peak / 0.01f));
      if (src.type == cgltf_light_type_spot) {
        const float cosOuter = std::cos(src.spot_outer_cone_angle);
        const float cosInner = std::cos(src.spot_inner_cone_angle);
        l.direction = axis;
        l.spotScale = 1.0f / std::max(1e-3f, cosInner - cosOuter);
        l.spotOffset = -cosOuter * l.spotScale;
      }
      scene.addLight(l);
      ++count;
    }
    return count;
  }

  void loadCamera(const glm::mat4& root) {
    for (size_t i = 0; i < data->nodes_count && !scene.assetCamera.valid; ++i) {
      const cgltf_node& node = data->nodes[i];
      if (!node.camera || node.camera->type != cgltf_camera_type_perspective) continue;
      float world[16];
      cgltf_node_transform_world(&node, world);
      const glm::mat4 m = root * glm::make_mat4(world);
      auto& c = scene.assetCamera;
      c.valid = true;
      c.position = glm::vec3(m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
      c.forward = glm::normalize(glm::vec3(m * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));  // a câmera olha para -Z
      c.fovY = node.camera->data.perspective.yfov;
      c.nearPlane = std::max(node.camera->data.perspective.znear, 1e-3f);
    }
  }

  void loadNodes(const glm::mat4& root) {
    for (size_t i = 0; i < data->nodes_count; ++i) {
      const cgltf_node& node = data->nodes[i];
      if (!node.mesh) continue;
      float world[16];
      cgltf_node_transform_world(&node, world);
      glm::mat4 m = root * glm::make_mat4(world);
      for (Primitive& p : meshPrims[size_t(node.mesh - data->meshes)]) {
        bool skinned = model >= 0 && node.skin && !p.data.joints.empty();
        uint32_t mesh;
        if (skinned) {
          // Cada personagem tem sua própria cópia deformável da malha.
          MeshData copy = p.data;
          copy.skinModel = model;
          copy.skin = int(node.skin - data->skins);
          mesh = scene.addMesh(std::move(copy));
        } else {
          if (p.sceneMesh < 0) {
            MeshData copy = p.data;
            copy.joints.clear();
            copy.weights.clear();
            p.sceneMesh = int(scene.addMesh(std::move(copy)));
          }
          mesh = uint32_t(p.sceneMesh);
        }
        uint32_t object = scene.addObject(mesh, m, p.material);
        if (model >= 0) {
          Model& md = scene.models[size_t(model)];
          if (skinned) md.skinnedObjects.push_back(object);
          else md.rigidObjects.push_back({uint32_t(i), object});
        }
      }
    }
  }
};

}  // namespace

bool loadGltf(Scene& scene, const std::string& path, const glm::mat4& rootTransform, int* modelOut) {
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
  Loader l{scene, data, std::filesystem::path(path).parent_path(), {}, {}, {}, 0, -1};
  l.loadMaterials();
  l.loadMeshes();
  l.loadModel(rootTransform);
  l.loadNodes(rootTransform);
  const uint32_t lights = l.loadLights(rootTransform);
  l.loadCamera(rootTransform);
  if (modelOut) *modelOut = l.model;
  LOG_INFO("glTF: %s (%zu nós, %zu materiais, %zu esqueletos, %zu animações, %u luzes)", path.c_str(), data->nodes_count,
           data->materials_count, data->skins_count, data->animations_count, lights);
  cgltf_free(data);
  return true;
}

}  // namespace scene
