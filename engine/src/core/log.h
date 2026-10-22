#pragma once
#include <cstdio>
#include <cstdlib>

#define LOG_INFO(...)  do { std::fprintf(stdout, "[info] "  __VA_ARGS__); std::fputc('\n', stdout); } while (0)
#define LOG_WARN(...)  do { std::fprintf(stderr, "[warn] "  __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define LOG_ERROR(...) do { std::fprintf(stderr, "[error] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define FATAL(...)     do { LOG_ERROR(__VA_ARGS__); std::abort(); } while (0)
