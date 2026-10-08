#include "../platform/shell.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "paths.hpp"

namespace mh {

void revealFile(const std::string& path) {
  const std::wstring args = L"/select,\"" + widen(path) + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace mh
