# FCLegacy — núcleo da simulação da partida. C++ puro, sem renderizador: roda no celular, no servidor (validador do
# online) e nos testes. Determinístico: mesmas entradas, mesmo resultado bit a bit (base do rollback e dos replays).
cmake_minimum_required(VERSION 3.20)
project(fcsim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(fcsim STATIC src/ball.cpp)
target_include_directories(fcsim PUBLIC include)
# Sem fusão de multiplicação+soma (FMA) nem matemática "rápida": o resultado não pode depender do compilador.
if(NOT MSVC)
  target_compile_options(fcsim PUBLIC -ffp-contract=off -fno-fast-math -Wall -Wextra)
endif()

enable_testing()
add_executable(fcsim_tests tests/test_ball.cpp)
target_link_libraries(fcsim_tests PRIVATE fcsim)
add_test(NAME fcsim_tests COMMAND fcsim_tests)
