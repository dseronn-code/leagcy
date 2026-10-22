cmake_minimum_required(VERSION 3.24)
project(LegacyEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED COMPONENTS glslangValidator)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Vulkan Memory Allocator (AMD) — alocador padrão da indústria.
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.1.0
  GIT_SHALLOW TRUE)
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

# ---- Shaders: GLSL -> SPIR-V em tempo de build ----
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)
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

# ---- Motor ----
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(legacy ${ENGINE_SOURCES})
add_dependencies(legacy shaders)
target_include_directories(legacy PRIVATE src)
target_include_directories(legacy SYSTEM PRIVATE ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})
target_link_libraries(legacy PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator glm::glm glfw)
target_compile_definitions(legacy PRIVATE
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_RADIANS
  SHADER_DIR="${SHADER_OUT_DIR}"
  ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
if(MSVC)
  target_compile_options(legacy PRIVATE /W4 /permissive-)
else()
  target_compile_options(legacy PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
endif()
