// Windows: the camera picture as MJPEG on 127.0.0.1, as mac/preview_server.mm
// does on the Mac. Winsock, one thread per viewer (a slow viewer skips frames,
// it never builds a queue), JPEG from stb_image_write.
#include "../platform/preview_server.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "../third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop
#include "paths.hpp"

namespace mh {

namespace {

struct Client {
  SOCKET socket = INVALID_SOCKET;
  std::string stream;
  int width = 640;  // ?w= in the request
  std::mutex mutex;
  std::condition_variable wake;
  std::shared_ptr<const std::string> next;  // the newest frame not yet sent
  bool closed = false;
};

void sendAll(SOCKET s, const std::string& data, bool* ok) {
  size_t sent = 0;
  while (*ok && sent < data.size()) {
    const int n = send(s, data.data() + sent, int(std::min<size_t>(data.size() - sent, 1 << 20)), 0);
    if (n <= 0) *ok = false;
    else sent += size_t(n);
  }
}

void appendBytes(void* context, void* data, int size) {
  static_cast<std::string*>(context)->append(static_cast<const char*>(data), size_t(size));
}

std::string part(const std::string& jpeg) {
  return "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg.size()) + "\r\n\r\n" + jpeg + "\r\n";
}

// Live's fonts, if this Live has them where we look (the editor falls back to system fonts).
std::string readFont(const std::string& name) {
  wchar_t exe[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]));
  std::wstring dir(exe, n);
  dir.resize(dir.find_last_of(L"\\/"));
  const std::wstring parent = dir.substr(0, dir.find_last_of(L"\\/"));
  for (const std::wstring& base : {parent + L"\\Resources\\Fonts\\", dir + L"\\Resources\\Fonts\\", dir + L"\\Fonts\\"}) {
    HANDLE file = CreateFileW((base + widen(name)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    std::string data;
    char buf[65536];
    DWORD read = 0;
    while (ReadFile(file, buf, sizeof buf, &read, nullptr) && read > 0) data.append(buf, read);
    CloseHandle(file);
    return data;
  }
  return "";
}

}  // namespace

struct PreviewServer::Impl {
  std::mutex mutex;
  std::vector<std::shared_ptr<Client>> clients;
  SOCKET listener = INVALID_SOCKET;
  uint16_t port = 0;
  bool started = false;

  // Color frames wait here for the encoder thread.
  std::mutex encodeMutex;
  std::condition_variable encodeWake;
  std::vector<uint8_t> rgb;  // width * height * 3, mirrored and scaled
  int rgbWidth = 0, rgbHeight = 0;
  std::string rgbStream;
  std::atomic<bool> encoding{false};

  bool start() {
    std::lock_guard<std::mutex> lock(mutex);
    if (started) return port != 0;
    started = true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // the picture never leaves this computer
    address.sin_port = 0;
    int length = sizeof address;
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(listener, 8) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      closesocket(listener);
      listener = INVALID_SOCKET;
      return false;
    }
    port = ntohs(address.sin_port);
    std::thread([this] { acceptLoop(); }).detach();
    std::thread([this] { encodeLoop(); }).detach();
    return true;
  }

  void acceptLoop() {
    for (;;) {
      SOCKET s = accept(listener, nullptr, nullptr);
      if (s == INVALID_SOCKET) continue;
      std::thread([this, s] { serve(s); }).detach();
    }
  }

  void serve(SOCKET s) {
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
    std::string request;
    char buf[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
      const int n = recv(s, buf, sizeof buf, 0);
      if (n <= 0) break;
      request.append(buf, size_t(n));
    }
    bool ok = true;
    const std::string fontPrefix = "GET /font/";
    if (request.compare(0, fontPrefix.size(), fontPrefix) == 0) {
      const size_t end = request.find(' ', fontPrefix.size());
      const std::string name = request.substr(fontPrefix.size(), end == std::string::npos ? std::string::npos : end - fontPrefix.size());
      const bool safe = name.size() > 4 && name.find_first_of("/\\") == std::string::npos && name.find("..") == std::string::npos &&
                        (name.substr(name.size() - 4) == ".ttf" || name.substr(name.size() - 4) == ".otf");
      const std::string data = safe ? readFont(name) : "";
      sendAll(s, data.empty() ? "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"
                              : "HTTP/1.1 200 OK\r\nContent-Type: font/ttf\r\nContent-Length: " + std::to_string(data.size()) +
                                    "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: max-age=86400\r\nConnection: close\r\n\r\n" + data,
              &ok);
      closesocket(s);
      return;
    }
    const size_t a = request.find("/cam/"), b = request.find(".mjpg");
    if (a == std::string::npos || b == std::string::npos || b < a) {
      sendAll(s, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n", &ok);
      closesocket(s);
      return;
    }
    auto client = std::make_shared<Client>();
    client->socket = s;
    client->stream = request.substr(a + 5, b - a - 5);
    const size_t w = request.find("?w=", b);
    if (w != std::string::npos && w < request.find(' ', b)) client->width = std::clamp(std::atoi(request.c_str() + w + 3), 160, 1920);
    sendAll(s,
            "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
            &ok);
    {
      std::lock_guard<std::mutex> lock(mutex);
      clients.push_back(client);
    }
    while (ok) {
      std::shared_ptr<const std::string> frame;
      {
        std::unique_lock<std::mutex> lock(client->mutex);
        client->wake.wait(lock, [&] { return client->next || client->closed; });
        if (client->closed) break;
        frame = std::move(client->next);
        client->next.reset();
      }
      sendAll(s, *frame, &ok);
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
    }
    closesocket(s);
  }

  // Hands a finished part to every viewer of the stream (replacing an unsent older one).
  void distribute(const std::string& stream, const std::string& jpeg) {
    auto frame = std::make_shared<const std::string>(part(jpeg));
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& c : clients) {
      if (c->stream != stream) continue;
      std::lock_guard<std::mutex> l(c->mutex);
      c->next = frame;
      c->wake.notify_one();
    }
  }

  void encodeLoop() {
    std::vector<uint8_t> pixels;
    for (;;) {
      int w = 0, h = 0;
      std::string stream;
      {
        std::unique_lock<std::mutex> lock(encodeMutex);
        encodeWake.wait(lock, [&] { return !rgb.empty(); });
        pixels.swap(rgb);
        rgb.clear();
        w = rgbWidth;
        h = rgbHeight;
        stream = rgbStream;
      }
      std::string jpeg;
      stbi_write_jpg_to_func(appendBytes, &jpeg, w, h, 3, pixels.data(), 80);
      if (!jpeg.empty()) distribute(stream, jpeg);
      encoding = false;
    }
  }
};

PreviewServer& PreviewServer::shared() {
  static PreviewServer* server = new PreviewServer();  // lives as long as the process
  return *server;
}

PreviewServer::PreviewServer() : impl_(std::make_unique<Impl>()) {}

std::string PreviewServer::urlFor(const std::string& stream) {
  if (!impl_->start()) return "";
  return "http://127.0.0.1:" + std::to_string(impl_->port) + "/cam/" + stream + ".mjpg";
}

std::string PreviewServer::baseUrl() {
  if (!impl_->start()) return "";
  return "http://127.0.0.1:" + std::to_string(impl_->port);
}

bool PreviewServer::hasClients(const std::string& stream) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& c : impl_->clients)
    if (c->stream == stream) return true;
  return false;
}

int PreviewServer::requestedWidth(const std::string& stream) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  int width = 0;
  for (const auto& c : impl_->clients)
    if (c->stream == stream) width = std::max(width, c->width);
  return width;
}

void PreviewServer::publishPixels(const std::string& stream, const void* frame, int width) {
  const auto* image = static_cast<const BgraImage*>(frame);
  if (!image || !image->data || width <= 0 || impl_->encoding.exchange(true)) return;
  // Mirror and scale on the capture thread (the frame is only valid now): each output
  // pixel averages its block of input pixels.
  const int w = std::min(width, image->width);
  const int h = std::max(1, int(int64_t(w) * image->height / std::max(1, image->width)));
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    const int y0 = y * image->height / h, y1 = std::max(y0 + 1, (y + 1) * image->height / h);
    for (int x = 0; x < w; ++x) {
      const int sx = w - 1 - x;  // mirrored
      const int x0 = sx * image->width / w, x1 = std::max(x0 + 1, (sx + 1) * image->width / w);
      unsigned sum[3] = {0, 0, 0}, n = 0;
      for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 3) {
        const uint8_t* row = image->data + int64_t(yy) * image->stride;
        for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 3, ++n) {
          sum[0] += row[xx * 4 + 2];
          sum[1] += row[xx * 4 + 1];
          sum[2] += row[xx * 4 + 0];
        }
      }
      uint8_t* out = &rgb[(size_t(y) * w + x) * 3];
      for (int c = 0; c < 3; ++c) out[c] = uint8_t(sum[c] / std::max(1u, n));
    }
  }
  {
    std::lock_guard<std::mutex> lock(impl_->encodeMutex);
    impl_->rgb.swap(rgb);
    impl_->rgbWidth = w;
    impl_->rgbHeight = h;
    impl_->rgbStream = stream;
  }
  impl_->encodeWake.notify_one();
}

void PreviewServer::publish(const std::string& stream, const GrayImage& image) {
  if (image.width <= 0 || !hasClients(stream)) return;
  std::string jpeg;
  stbi_write_jpg_to_func(appendBytes, &jpeg, image.width, image.height, 1, image.pixels.data(), 70);
  if (!jpeg.empty()) impl_->distribute(stream, jpeg);
}

}  // namespace mh
