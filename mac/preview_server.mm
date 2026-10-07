#include "preview_server.hpp"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Network/Network.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace mh {

namespace {

struct Client {
  nw_connection_t connection;
  std::string stream;
  std::atomic<bool> busy{false};
};

NSData* encodeJpeg(const GrayImage& image) {
  CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
  CFDataRef pixels = CFDataCreate(nullptr, image.pixels.data(), CFIndex(image.pixels.size()));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(pixels);
  CGImageRef cg = CGImageCreate(image.width, image.height, 8, 8, image.width, gray, kCGImageAlphaNone, provider,
                                nullptr, false, kCGRenderingIntentDefault);
  NSMutableData* out = [NSMutableData data];
  CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out,
                                                               (__bridge CFStringRef)UTTypeJPEG.identifier, 1, nil);
  NSDictionary* options = @{(id)kCGImageDestinationLossyCompressionQuality : @0.7};
  CGImageDestinationAddImage(dst, cg, (__bridge CFDictionaryRef)options);
  const bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  CGImageRelease(cg);
  CGDataProviderRelease(provider);
  CFRelease(pixels);
  CGColorSpaceRelease(gray);
  return ok ? out : nil;
}

dispatch_data_t dataFrom(const std::string& s) {
  return dispatch_data_create(s.data(), s.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
}

}  // namespace

struct PreviewServer::Impl {
  dispatch_queue_t queue = dispatch_queue_create("midihands.preview", DISPATCH_QUEUE_SERIAL);
  nw_listener_t listener = nil;
  uint16_t port = 0;
  bool started = false;
  mutable std::mutex mutex;
  std::vector<std::shared_ptr<Client>> clients;

  bool start() {
    if (started) return port != 0;
    started = true;
    nw_parameters_t params =
        nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    // Loopback only: the picture never leaves this Mac.
    nw_parameters_set_local_endpoint(params, nw_endpoint_create_host("127.0.0.1", "0"));
    listener = nw_listener_create(params);
    if (!listener) return false;
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    __block bool failed = false;
    nw_listener_set_queue(listener, queue);
    nw_listener_set_state_changed_handler(listener, ^(nw_listener_state_t state, nw_error_t) {
      if (state == nw_listener_state_ready || state == nw_listener_state_failed) {
        failed = state == nw_listener_state_failed;
        dispatch_semaphore_signal(ready);
      }
    });
    nw_listener_set_new_connection_handler(listener, ^(nw_connection_t connection) { accept(connection); });
    nw_listener_start(listener);
    dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    if (!failed) port = nw_listener_get_port(listener);
    return port != 0;
  }

  void remove(Client* client) {
    std::lock_guard<std::mutex> lock(mutex);
    clients.erase(std::remove_if(clients.begin(), clients.end(), [&](auto& c) { return c.get() == client; }),
                  clients.end());
  }

  void accept(nw_connection_t connection) {
    auto client = std::make_shared<Client>();
    client->connection = connection;
    Client* raw = client.get();
    nw_connection_set_queue(connection, queue);
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t) {
      if (state == nw_connection_state_failed || state == nw_connection_state_cancelled) remove(raw);
    });
    nw_connection_start(connection);
    // Read the request line, e.g. "GET /cam/abc.mjpg HTTP/1.1".
    nw_connection_receive(connection, 1, 8192,
                          ^(dispatch_data_t content, nw_content_context_t, bool, nw_error_t error) {
                            if (error || !content) {
                              nw_connection_cancel(connection);
                              return;
                            }
                            __block std::string request;
                            dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* buf, size_t size) {
                              request.append(static_cast<const char*>(buf), size);
                              return true;
                            });
                            const size_t a = request.find("/cam/"), b = request.find(".mjpg");
                            if (a == std::string::npos || b == std::string::npos || b < a) {
                              nw_connection_send(connection, dataFrom("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n"),
                                                 NW_CONNECTION_FINAL_MESSAGE_CONTEXT, true,
                                                 ^(nw_error_t) { nw_connection_cancel(connection); });
                              return;
                            }
                            client->stream = request.substr(a + 5, b - a - 5);
                            const std::string header =
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                "Cache-Control: no-cache\r\nConnection: close\r\n"
                                "Access-Control-Allow-Origin: *\r\n\r\n";
                            nw_connection_send(connection, dataFrom(header), NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT,
                                               false, ^(nw_error_t) {});
                            std::lock_guard<std::mutex> lock(mutex);
                            clients.push_back(client);
                          });
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

bool PreviewServer::hasClients(const std::string& stream) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& c : impl_->clients)
    if (c->stream == stream) return true;
  return false;
}

void PreviewServer::publish(const std::string& stream, const GrayImage& image) {
  std::vector<std::shared_ptr<Client>> ready;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& c : impl_->clients)
      if (c->stream == stream && !c->busy) ready.push_back(c);
  }
  if (ready.empty() || image.width <= 0) return;
  @autoreleasepool {
    NSData* jpeg = encodeJpeg(image);
    if (!jpeg) return;
    const std::string head = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                             std::to_string(jpeg.length) + "\r\n\r\n";
    dispatch_data_t body = dispatch_data_create(jpeg.bytes, jpeg.length, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    dispatch_data_t part = dispatch_data_create_concat(dispatch_data_create_concat(dataFrom(head), body), dataFrom("\r\n"));
    for (const auto& c : ready) {
      // A slow viewer skips frames instead of building a queue.
      c->busy = true;
      std::shared_ptr<Client> keep = c;
      nw_connection_send(c->connection, part, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false, ^(nw_error_t error) {
        keep->busy = false;
        if (error) nw_connection_cancel(keep->connection);
      });
    }
  }
}

}  // namespace mh
