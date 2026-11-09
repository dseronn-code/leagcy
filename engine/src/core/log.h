#pragma once
#include <cstdio>
#include <cstdlib>

// stdout com flush a cada linha: ferramentas e testes leem o log enquanto o motor roda (por um pipe, sem isso as
// linhas ficavam no buffer até juntar alguns KB; no Windows o modo "linha a linha" do setvbuf não existe).
#define LOG_INFO(...)  do { std::fprintf(stdout, "[info] "  __VA_ARGS__); std::fputc('\n', stdout); std::fflush(stdout); } while (0)
#define LOG_WARN(...)  do { std::fprintf(stderr, "[warn] "  __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define LOG_ERROR(...) do { std::fprintf(stderr, "[error] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define FATAL(...)     do { LOG_ERROR(__VA_ARGS__); std::abort(); } while (0)
