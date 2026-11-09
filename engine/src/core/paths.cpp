#include "core/paths.h"

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace core {

std::filesystem::path executableDir() {
  static const std::filesystem::path dir = [] {
    std::filesystem::path exe;
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n > 0 && n < buf.size()) exe = std::filesystem::path(std::wstring(buf.data(), n));
#else
    std::error_code ec;
    exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    return exe.empty() ? exe : exe.parent_path();
  }();
  return dir;
}

}  // namespace core
