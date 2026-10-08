// Local-only MJPEG server for the camera picture. Pages show
// http://127.0.0.1:<port>/cam/<stream>.mjpg[?w=<width>] in an <img> (or as a
// WebGL texture: CORS is open); frames are only encoded while somebody
// watches, at the largest width any viewer asked for (default 640).
#pragma once

#include <memory>
#include <string>

#include "../core/preview.hpp"

namespace mh {

class PreviewServer {
 public:
  static PreviewServer& shared();

  // Starts the server on first use. Empty string if it could not start.
  std::string urlFor(const std::string& stream);
  // http://127.0.0.1:<port>; also serves /font/<file> from the host app's
  // bundled fonts (Live's Ableton Sans) so the editor can match Live.
  std::string baseUrl();
  bool hasClients(const std::string& stream) const;
  // Largest picture width the viewers of `stream` asked for.
  int requestedWidth(const std::string& stream) const;
  // Encodes and sends one frame to every client of `stream` that is ready.
  void publish(const std::string& stream, const GrayImage& image);
  // Color frame (CVPixelBufferRef), mirrored and scaled to `width`, encoded
  // on the server's own queue; skipped while the previous one is encoding.
  void publishPixels(const std::string& stream, const void* pixelBuffer, int width);

  struct Impl;  // public so the encoder helpers in the .mm can reach it

 private:
  PreviewServer();
  std::unique_ptr<Impl> impl_;
};

}  // namespace mh
