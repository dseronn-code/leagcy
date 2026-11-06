# Biblioteca nativa do app: o Legacy Engine sem as bibliotecas da NVIDIA (LEGACY_NRD=OFF), o jogo e o
# android_main. Mesmas dependências e versões do CMakeLists.txt do motor.
cmake_minimum_required(VERSION 3.22.1)
project(penalti LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ENGINE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

include(FetchContent)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG 3aa921224c154a0d2c43912bc88e1c42ce1f7607)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1 GIT_SHALLOW TRUE)
FetchContent_Declare(cgltf GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git GIT_TAG v1.14 GIT_SHALLOW TRUE SOURCE_SUBDIR _sem_cmake)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d SOURCE_SUBDIR _sem_cmake)
FetchContent_Declare(bc7enc GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0 SOURCE_SUBDIR _sem_cmake)
FetchContent_Declare(mikktspace GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git GIT_TAG 3e895b49d05ea07e4c2133156cfa94369e19e409 SOURCE_SUBDIR _sem_cmake)
FetchContent_MakeAvailable(vma glm cgltf stb bc7enc mikktspace)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(bc7enc STATIC ${bc7enc_SOURCE_DIR}/bc7enc.cpp ${bc7enc_SOURCE_DIR}/bc7decomp.cpp ${bc7enc_SOURCE_DIR}/rgbcx.cpp)
target_include_directories(bc7enc SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
add_library(mikktspace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(mikktspace SYSTEM PUBLIC ${mikktspace_SOURCE_DIR})

file(GLOB_RECURSE ENGINE_SOURCES ${ENGINE}/src/*.cpp)
list(REMOVE_ITEM ENGINE_SOURCES ${ENGINE}/src/main.cpp ${ENGINE}/src/renderer/nrd_denoiser.cpp ${ENGINE}/src/renderer/upscaler.cpp)
add_library(penalti SHARED ${ENGINE_SOURCES} ${ENGINE}/sem_nrd/sem_nrd.cpp android_main.cpp)
target_include_directories(penalti PRIVATE ${ENGINE}/src)
target_include_directories(penalti SYSTEM PRIVATE ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})
target_compile_definitions(penalti PRIVATE
  GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS VK_USE_PLATFORM_ANDROID_KHR LEGACY_MIKKTSPACE=1
  SHADER_DIR="shaders" ASSET_DIR="assets" GAME_DIR="game")
target_link_libraries(penalti PRIVATE native_app_glue GPUOpen::VulkanMemoryAllocator glm::glm bc7enc mikktspace vulkan android log)
# A NativeActivity procura ANativeActivity_onCreate: não deixar o linker jogar fora o ponto de entrada da glue.
set_target_properties(penalti PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
