// A minimal ONNX Runtime wrapper. The runtime library is loaded at run time
// from an explicit path (on Windows a DLL next to the external is not found
// otherwise: Live's own folders are searched, not ours), through its C API,
// so nothing links against it and a missing runtime is an error message.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mh {

class OnnxModel {
 public:
  struct Tensor {
    std::vector<float> data;
    std::vector<int64_t> shape;
  };

  // `runtime`: path of onnxruntime.dll / libonnxruntime.dylib (loaded once per
  // process). `model`: the .onnx file. Returns null and fills error on failure.
  static std::unique_ptr<OnnxModel> load(const std::string& runtime, const std::string& model, std::string* error);
  ~OnnxModel();

  // One float32 input; returns every output, in the model's order.
  bool run(const float* input, const std::vector<int64_t>& shape, std::vector<Tensor>* outputs, std::string* error);

  const std::string& inputName() const { return input_; }
  // The input's shape as the model declares it (-1 for free dimensions).
  const std::vector<int64_t>& inputShape() const { return inputShape_; }
  const std::vector<std::string>& outputNames() const { return outputs_; }

 private:
  OnnxModel() = default;
  struct Impl;
  Impl* impl_ = nullptr;
  std::string input_;
  std::vector<int64_t> inputShape_;
  std::vector<std::string> outputs_;
};

}  // namespace mh
