#include "onnx.hpp"

#include <mutex>

// The header spells the (x86-only, ignored on x64) calling convention the MSVC way.
#if defined(_WIN32) && defined(__GNUC__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include "onnxruntime_c_api.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mh {

namespace {

std::mutex gMutex;
const OrtApi* gApi = nullptr;
OrtEnv* gEnv = nullptr;

#ifdef _WIN32
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
#endif

// Loads the runtime and creates the environment once per process.
bool loadRuntime(const std::string& path, std::string* error) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (gApi && gEnv) return true;
  using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
  GetApiBase getBase = nullptr;
#ifdef _WIN32
  // LOAD_WITH_ALTERED_SEARCH_PATH: the runtime's own dependencies are found next to it.
  HMODULE lib = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (lib) getBase = reinterpret_cast<GetApiBase>(reinterpret_cast<void*>(GetProcAddress(lib, "OrtGetApiBase")));
#else
  void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (lib) getBase = reinterpret_cast<GetApiBase>(dlsym(lib, "OrtGetApiBase"));
#endif
  if (!getBase) {
    if (error) *error = "cannot load ONNX Runtime: " + path;
    return false;
  }
  gApi = getBase()->GetApi(ORT_API_VERSION);
  if (!gApi) {
    if (error) *error = "ONNX Runtime is too old for this build";
    return false;
  }
  if (OrtStatus* status = gApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "midihands", &gEnv)) {
    if (error) *error = gApi->GetErrorMessage(status);
    gApi->ReleaseStatus(status);
    gEnv = nullptr;
    return false;
  }
  return true;
}

// Turns a status into an error message; true when there was none.
bool ok(OrtStatus* status, std::string* error) {
  if (!status) return true;
  if (error) *error = gApi->GetErrorMessage(status);
  gApi->ReleaseStatus(status);
  return false;
}

}  // namespace

struct OnnxModel::Impl {
  OrtSession* session = nullptr;
  OrtMemoryInfo* memory = nullptr;
};

std::unique_ptr<OnnxModel> OnnxModel::load(const std::string& runtime, const std::string& model, std::string* error) {
  if (!loadRuntime(runtime, error)) return nullptr;
  std::unique_ptr<OnnxModel> m(new OnnxModel());
  m->impl_ = new Impl();
  OrtSessionOptions* options = nullptr;
  if (!ok(gApi->CreateSessionOptions(&options), error)) return nullptr;
  // Two threads per model: fast enough for camera rates, and Live's audio keeps its CPU.
  if (!ok(gApi->SetIntraOpNumThreads(options, 2), error) || !ok(gApi->SetInterOpNumThreads(options, 1), error) ||
      !ok(gApi->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL), error)) {
    gApi->ReleaseSessionOptions(options);
    return nullptr;
  }
#ifdef _WIN32
  const std::wstring path = widen(model);
  const bool created = ok(gApi->CreateSession(gEnv, path.c_str(), options, &m->impl_->session), error);
#else
  const bool created = ok(gApi->CreateSession(gEnv, model.c_str(), options, &m->impl_->session), error);
#endif
  gApi->ReleaseSessionOptions(options);
  if (!created) return nullptr;
  if (!ok(gApi->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m->impl_->memory), error)) return nullptr;

  OrtAllocator* allocator = nullptr;
  if (!ok(gApi->GetAllocatorWithDefaultOptions(&allocator), error)) return nullptr;
  char* name = nullptr;
  if (!ok(gApi->SessionGetInputName(m->impl_->session, 0, allocator, &name), error)) return nullptr;
  m->input_ = name;
  allocator->Free(allocator, name);
  OrtTypeInfo* type = nullptr;
  const OrtTensorTypeAndShapeInfo* tensor = nullptr;
  size_t dims = 0;
  if (!ok(gApi->SessionGetInputTypeInfo(m->impl_->session, 0, &type), error)) return nullptr;
  if (ok(gApi->CastTypeInfoToTensorInfo(type, &tensor), error) && tensor && ok(gApi->GetDimensionsCount(tensor, &dims), error)) {
    m->inputShape_.assign(dims, 0);
    if (!ok(gApi->GetDimensions(tensor, m->inputShape_.data(), dims), error)) m->inputShape_.clear();
  }
  gApi->ReleaseTypeInfo(type);
  size_t outputs = 0;
  if (!ok(gApi->SessionGetOutputCount(m->impl_->session, &outputs), error)) return nullptr;
  for (size_t i = 0; i < outputs; ++i) {
    if (!ok(gApi->SessionGetOutputName(m->impl_->session, i, allocator, &name), error)) return nullptr;
    m->outputs_.push_back(name);
    allocator->Free(allocator, name);
  }
  return m;
}

OnnxModel::~OnnxModel() {
  if (!impl_) return;
  if (impl_->session) gApi->ReleaseSession(impl_->session);
  if (impl_->memory) gApi->ReleaseMemoryInfo(impl_->memory);
  delete impl_;
}

bool OnnxModel::run(const float* input, const std::vector<int64_t>& shape, std::vector<Tensor>* outputs,
                    std::string* error) {
  size_t count = 1;
  for (int64_t d : shape) count *= size_t(d);
  OrtValue* in = nullptr;
  if (!ok(gApi->CreateTensorWithDataAsOrtValue(impl_->memory, const_cast<float*>(input), count * sizeof(float),
                                                shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in),
          error))
    return false;
  const char* inName = input_.c_str();
  std::vector<const char*> outNames;
  for (const std::string& n : outputs_) outNames.push_back(n.c_str());
  std::vector<OrtValue*> out(outputs_.size(), nullptr);
  const bool ran = ok(gApi->Run(impl_->session, nullptr, &inName, &in, 1, outNames.data(), outNames.size(), out.data()),
                      error);
  gApi->ReleaseValue(in);
  if (!ran) return false;
  outputs->resize(out.size());
  bool good = true;
  for (size_t i = 0; i < out.size(); ++i) {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    size_t dims = 0, elements = 0;
    float* data = nullptr;
    if (good && ok(gApi->GetTensorTypeAndShape(out[i], &info), error)) {
      good = ok(gApi->GetDimensionsCount(info, &dims), error);
      (*outputs)[i].shape.assign(dims, 0);
      good = good && ok(gApi->GetDimensions(info, (*outputs)[i].shape.data(), dims), error) &&
             ok(gApi->GetTensorShapeElementCount(info, &elements), error);
      gApi->ReleaseTensorTypeAndShapeInfo(info);
      if (good && ok(gApi->GetTensorMutableData(out[i], reinterpret_cast<void**>(&data)), error))
        (*outputs)[i].data.assign(data, data + elements);
      else
        good = false;
    } else {
      good = false;
    }
    gApi->ReleaseValue(out[i]);
  }
  return good;
}

}  // namespace mh
