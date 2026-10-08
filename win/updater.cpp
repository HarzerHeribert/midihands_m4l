// Windows: release check over WinHTTP, update through install.ps1. The update
// cannot replace files while Live has them loaded, so it opens a PowerShell
// window that waits for Live to quit and then installs.
#include "../platform/updater.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "../core/version.hpp"
#include "paths.hpp"

namespace mh {

namespace {
std::mutex gCacheMutex;
UpdateCheck gCached;
double gCachedAt = -1e9;

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// GET https://api.github.com<path>; returns the HTTP status (0 when offline).
int get(const std::wstring& path, std::string* body) {
  int status = 0;
  const std::wstring agent = L"midihands/" + widen(kVersion);
  HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connection = session ? WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                 : nullptr;
  if (request) {
    WinHttpSetTimeouts(request, 10000, 10000, 10000, 10000);
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    if (WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD code = 0, size = sizeof code;
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &code, &size, WINHTTP_NO_HEADER_INDEX);
      status = int(code);
      DWORD available = 0;
      while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
        body->append(chunk.data(), read);
      }
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  if (session) WinHttpCloseHandle(session);
  return status;
}

// "tag_name": "v0.3.0" -> 0.3.0 (the answer is GitHub's JSON; one field is all we need).
std::string tagName(const std::string& json) {
  const size_t key = json.find("\"tag_name\"");
  if (key == std::string::npos) return "";
  const size_t a = json.find('"', json.find(':', key) + 1), b = a == std::string::npos ? a : json.find('"', a + 1);
  if (b == std::string::npos) return "";
  std::string tag = json.substr(a + 1, b - a - 1);
  if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
  return tag;
}
}  // namespace

void checkForUpdate(std::function<void(const UpdateCheck&)> done) {
  {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (gCached.ok && now() - gCachedAt < 3600.0) {
      const UpdateCheck cached = gCached;
      std::thread([done, cached] { done(cached); }).detach();
      return;
    }
  }
  std::thread([done] {
    UpdateCheck result;
    std::string body;
    const int status = get(L"/repos/" + widen(kRepository) + L"/releases/latest", &body);
    if (status == 0) {
      result.error = "offline";
    } else if (status == 404) {
      result.ok = true;  // no release published yet
    } else if (status != 200) {
      result.error = "GitHub answered " + std::to_string(status);
    } else if ((result.latest = tagName(body)).empty()) {
      result.error = "unexpected answer from GitHub";
    } else {
      result.ok = true;
      result.newer = compareVersions(result.latest, kVersion) > 0;
    }
    if (result.ok) {
      std::lock_guard<std::mutex> lock(gCacheMutex);
      gCached = result;
      gCachedAt = now();
    }
    done(result);
  }).detach();
}

void installUpdate(std::function<void(bool, const std::string&)> done) {
  const std::string script = std::string("https://github.com/") + kRepository + "/releases/latest/download/install.ps1";
  std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"& ([scriptblock]::Create((irm '" +
                         widen(script) + L"'))) -Yes -WaitForLive\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startup,
                      &process)) {
    done(false, "could not start PowerShell: " + errorText(long(GetLastError())));
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  done(true, "Quit Live to finish the update");
}

}  // namespace mh
