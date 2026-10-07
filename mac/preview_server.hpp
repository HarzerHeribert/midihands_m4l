// Local-only MJPEG server for the editor's camera picture. The editor page
// shows http://127.0.0.1:<port>/cam/<stream>.mjpg in an <img>; frames are
// only encoded while at least one editor is watching that stream.
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
  bool hasClients(const std::string& stream) const;
  // Encodes and sends one frame to every client of `stream` that is ready.
  void publish(const std::string& stream, const GrayImage& image);

 private:
  PreviewServer();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mh
