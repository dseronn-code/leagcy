#pragma once
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
// Android: o console é o logcat; e, como no celular ninguém vê o log, o erro fatal vira exceção, que o android_main
// mostra numa caixa de diálogo antes de fechar o app.
#include <android/log.h>
#include <stdexcept>
// legacyLog: logcat + as últimas linhas guardadas na memória (android_main mostra se o jogo travar).
void legacyLog(int priority, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG_INFO(...)  legacyLog(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOG_WARN(...)  legacyLog(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOG_ERROR(...) legacyLog(ANDROID_LOG_ERROR, __VA_ARGS__)
#define FATAL(...)                                                         \
  do {                                                                     \
    char fatalMessage_[1024];                                              \
    std::snprintf(fatalMessage_, sizeof(fatalMessage_), __VA_ARGS__);      \
    LOG_ERROR("%s", fatalMessage_);                                        \
    throw std::runtime_error(fatalMessage_);                               \
  } while (0)
#else
// stdout com flush a cada linha: ferramentas e testes leem o log enquanto o motor roda (por um pipe, sem isso as
// linhas ficavam no buffer até juntar alguns KB; no Windows o modo "linha a linha" do setvbuf não existe).
#define LOG_INFO(...)  do { std::fprintf(stdout, "[info] "  __VA_ARGS__); std::fputc('\n', stdout); std::fflush(stdout); } while (0)
#define LOG_WARN(...)  do { std::fprintf(stderr, "[warn] "  __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define LOG_ERROR(...) do { std::fprintf(stderr, "[error] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define FATAL(...)     do { LOG_ERROR(__VA_ARGS__); std::abort(); } while (0)
#endif
