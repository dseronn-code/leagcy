cmake_minimum_required(VERSION 3.30)  # o NRD exige 3.30
project(LegacyEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED COMPONENTS glslangValidator)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Vulkan Memory Allocator (AMD) — alocador padrão da indústria.
# (mesmo commit que o NRI usa: uma cópia só para os dois)
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG 3aa921224c154a0d2c43912bc88e1c42ce1f7607)
FetchContent_MakeAvailable(vma)

# GLM — matemática compatível com GLSL.
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

# cgltf (leitor glTF 2.0) e stb (decodificação de imagens) — só headers.
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG v1.14
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _sem_cmake)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d
  SOURCE_SUBDIR _sem_cmake)
FetchContent_MakeAvailable(cgltf stb)

# GLFW — usa a instalação do sistema se existir, senão baixa.
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)
endif()

# NVIDIA NRD (denoisers ReLAX/ReBLUR, os do Cyberpunk 2077 com path tracing) + NRI (camada que embrulha o
# nosso VkDevice). Licença "NVIDIA RTX SDKs" (uso comercial permitido, crédito à NVIDIA obrigatório — ver
# docs/auditoria.md). As dependências do NRD são declaradas aqui por git (têm prioridade sobre os .zip que o
# NRD declara). O ShaderMake baixa o DXC da Microsoft para compilar os shaders HLSL do NRD em SPIR-V.
set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRD_NRI ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_SUPPORTS_QUAD_INTRINSICS OFF CACHE BOOL "" FORCE)  # exigiria VK_KHR_compute_shader_derivatives
set(NRD_NORMAL_ENCODING "2" CACHE STRING "" FORCE)    # R10G10B10A2 octaédrico: o recomendado e testado pela NVIDIA
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "" FORCE) # rugosidade linear (a nossa)
set(NRI_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NONE_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_XLIB_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_WAYLAND_SUPPORT OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_TOOL ON CACHE BOOL "" FORCE)
FetchContent_Declare(shadermake
  GIT_REPOSITORY https://github.com/NVIDIA-RTX/ShaderMake.git
  GIT_TAG 3f01623a76b092ac127d2debd79a672f55d77665)
FetchContent_Declare(mathlib
  GIT_REPOSITORY https://github.com/NVIDIA-RTX/MathLib.git
  GIT_TAG v11
  GIT_SHALLOW TRUE)
FetchContent_Declare(vulkan_headers  # usados só dentro do NRI
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.365
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _sem_cmake)
FetchContent_Declare(nri
  GIT_REPOSITORY https://github.com/NVIDIA-RTX/NRI.git
  GIT_TAG v190
  GIT_SHALLOW TRUE)
FetchContent_Declare(nrd
  GIT_REPOSITORY https://github.com/NVIDIA-RTX/NRD.git
  GIT_TAG v4.18.0
  GIT_SHALLOW TRUE)

# Upscalers dos fabricantes pela extensão Upscaler do NRI (src/renderer/upscaler.cpp). Sem eles o motor usa o
# próprio TAA/TAAU; com eles, --upscaler dlss|fsr escolhe em tempo de execução (volta ao TAAU se a GPU não suportar).
#  - DLSS: SDK oficial da NVIDIA (licença NVIDIA DLSS: logo na tela de abertura e aviso à NVIDIA antes de lançar um
#    produto comercial — ver docs/licencas.md). Download de ~1,6 GB (repositório com bibliotecas de todas as plataformas).
#  - FSR 3: AMD FidelityFX SDK (MIT). O NRI só o compila no Windows.
option(LEGACY_DLSS "NVIDIA DLSS Super Resolution (GPUs RTX)" ON)
option(LEGACY_FSR "AMD FSR 3 (FidelityFX SDK; só Windows)" ${WIN32})
set(NRI_ENABLE_NGX_SDK ${LEGACY_DLSS} CACHE BOOL "" FORCE)
set(NRI_ENABLE_FFX_SDK ${LEGACY_FSR} CACHE BOOL "" FORCE)
# NIS (espacial, qualquer GPU, shaders compilados pelo ShaderMake do NRD): testa o caminho do upscaler sem RTX.
set(NRI_ENABLE_NIS_SDK ON CACHE BOOL "" FORCE)
if(LEGACY_DLSS)
  FetchContent_Declare(ngx  # o NRI declara um .zip do GitHub; por git funciona em mais redes
    GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
    GIT_TAG v310.9.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _sem_cmake)
endif()
FetchContent_MakeAvailable(nrd)

# Compressão de texturas BC7/BC5 (bc7enc_rdo, Rich Geldreich — MIT ou domínio público). Só os codificadores e o
# decodificador; o CMakeLists do projeto (ferramenta de linha de comando, ISPC) não é usado.
FetchContent_Declare(bc7enc
  GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
  GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
  SOURCE_SUBDIR _sem_cmake)
FetchContent_MakeAvailable(bc7enc)
add_library(bc7enc STATIC ${bc7enc_SOURCE_DIR}/bc7enc.cpp ${bc7enc_SOURCE_DIR}/bc7decomp.cpp ${bc7enc_SOURCE_DIR}/rgbcx.cpp)
target_include_directories(bc7enc SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
set_target_properties(bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON)

# MikkTSpace (Morten Mikkelsen; licença zlib): tangentes no padrão do Blender, Substance, Unreal e Unity — o mesmo que o
# glTF manda usar quando o arquivo não traz TANGENT.
# Ligado: medido igual às tangentes do Blender (--tangent-test: 0,000° em média). OFF = a aproximação própria.
option(LEGACY_MIKKTSPACE "Tangentes pela biblioteca MikkTSpace (baixada do GitHub)" ON)
if(LEGACY_MIKKTSPACE)
  FetchContent_Declare(mikktspace
    GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git
    GIT_TAG 3e895b49d05ea07e4c2133156cfa94369e19e409
    SOURCE_SUBDIR _sem_cmake)
  FetchContent_MakeAvailable(mikktspace)
  add_library(mikktspace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)
  target_include_directories(mikktspace SYSTEM PUBLIC ${mikktspace_SOURCE_DIR})
  set_target_properties(mikktspace PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ---- Shaders: GLSL -> SPIR-V em tempo de build ----
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.task
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.mesh)
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SPIRV_FILES)
foreach(SRC ${SHADER_SOURCES})
  get_filename_component(NAME ${SRC} NAME)
  set(OUT ${SHADER_OUT_DIR}/${NAME}.spv)
  add_custom_command(
    OUTPUT ${OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
    COMMAND Vulkan::glslangValidator --target-env vulkan1.3 -V ${SRC} -o ${OUT}
    DEPENDS ${SRC} ${SHADER_INCLUDES}
    COMMENT "GLSL -> SPIR-V: ${NAME}")
  list(APPEND SPIRV_FILES ${OUT})
endforeach()
add_custom_target(shaders DEPENDS ${SPIRV_FILES})

# ---- Camada de jogo (motor completo) ----
# EnTT: registro de entidades e componentes (ECS), só headers, MIT (usado no Minecraft Bedrock).
FetchContent_Declare(entt
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v3.13.2
  GIT_SHALLOW TRUE)
# nlohmann/json: formato das cenas em arquivo, só headers, MIT.
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
# Jolt Physics: corpos rígidos, personagem e veículos, MIT (o motor de física do Horizon Forbidden West e do
# Death Stranding 2). Runtime do MSVC dinâmico (/MD), igual ao resto do motor.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jolt
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.2.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(entt nlohmann_json jolt)

# Editor: Dear ImGui (ramo docking: janelas encaixáveis) + ImGuizmo (alças de mover/girar/escalar), MIT.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.8-docking
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _sem_cmake)
FetchContent_Declare(imguizmo
  GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
  GIT_TAG 105e4fde45e4e74aae8973d98750820ef7aca7e2
  SOURCE_SUBDIR _sem_cmake)
FetchContent_MakeAvailable(imgui imguizmo)
add_library(legacy_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(legacy_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(legacy_imgui PUBLIC Vulkan::Vulkan glfw)

# Scripts: Lua 5.4 (MIT, a linguagem de Roblox, WoW e Defold) + sol2 (MIT, ponte C++ <-> Lua, só headers).
FetchContent_Declare(lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG v5.4.7
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _sem_cmake)
FetchContent_Declare(sol2
  GIT_REPOSITORY https://github.com/ThePhD/sol2.git
  GIT_TAG v3.5.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _sem_cmake)
FetchContent_MakeAvailable(lua sol2)
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(legacy_lua STATIC ${LUA_SOURCES})
set_target_properties(legacy_lua PROPERTIES LINKER_LANGUAGE C)
target_include_directories(legacy_lua SYSTEM PUBLIC ${lua_SOURCE_DIR} ${sol2_SOURCE_DIR}/include)
target_compile_definitions(legacy_lua PUBLIC SOL_ALL_SAFETIES_ON=1)

# ---- Motor ----
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(legacy ${ENGINE_SOURCES})
add_dependencies(legacy shaders)
target_include_directories(legacy PRIVATE src)
target_include_directories(legacy SYSTEM PRIVATE ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})
target_link_libraries(legacy PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator glm::glm glfw NRD NRI bc7enc
                                     EnTT::EnTT nlohmann_json::nlohmann_json Jolt legacy_imgui legacy_lua)
target_include_directories(legacy SYSTEM PRIVATE ${nrd_SOURCE_DIR}/Include ${nrd_SOURCE_DIR}/Integration ${nri_SOURCE_DIR}/Include)
if(LEGACY_MIKKTSPACE)
  target_link_libraries(legacy PRIVATE mikktspace)
  target_compile_definitions(legacy PRIVATE LEGACY_MIKKTSPACE=1)
endif()
# As bibliotecas de execução do DLSS/FSR precisam ficar ao lado do executável (o NRI as copia para a pasta dele).
if(LEGACY_DLSS)
  FetchContent_GetProperties(ngx)
  # nvngx_dlss = Super Resolution; nvngx_dlssd = Ray Reconstruction (o "d" é de denoiser).
  if(WIN32)
    set(LEGACY_DLSS_RUNTIME "${ngx_SOURCE_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll"
                            "${ngx_SOURCE_DIR}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")
  else()
    set(LEGACY_DLSS_RUNTIME "${ngx_SOURCE_DIR}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1"
                            "${ngx_SOURCE_DIR}/lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.1")
  endif()
  add_custom_command(TARGET legacy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LEGACY_DLSS_RUNTIME} "$<TARGET_FILE_DIR:legacy>" VERBATIM)
endif()
if(LEGACY_FSR AND WIN32)
  FetchContent_GetProperties(ffx)
  add_custom_command(TARGET legacy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ffx_SOURCE_DIR}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
            "$<TARGET_FILE_DIR:legacy>" VERBATIM)
endif()
target_compile_definitions(legacy PRIVATE
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_RADIANS
  SHADER_DIR="${SHADER_OUT_DIR}"
  ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
if(MSVC)
  # /utf-8: fontes e mensagens em UTF-8 (português); /MP: compilação paralela.
  target_compile_options(legacy PRIVATE /W4 /permissive- /utf-8 /MP)
  target_compile_definitions(legacy PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(legacy PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wshadow=local)
endif()
