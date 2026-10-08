#include "paths.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace mh {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string errorText(long code) {
  wchar_t* text = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 DWORD(code), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::string message = text ? narrow(text) : "";
  if (text) LocalFree(text);
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) message.pop_back();
  char hex[16];
  std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(code));
  return message.empty() ? std::string(hex) : message + " (" + hex + ")";
}

std::string packageFolder() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&packageFolder), &self))
    return "";
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(self, path, DWORD(sizeof path / sizeof path[0]));
  if (n == 0) return "";
  std::wstring p(path, n);
  for (int i = 0; i < 2; ++i) {  // ...\midihands\externals\mh.hands.mxe64 -> ...\midihands
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return "";
    p.resize(slash);
  }
  return narrow(p);
}

}  // namespace mh
