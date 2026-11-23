// a2f_engine: Audio2Face-3D diffusion (v3.0) for the gothic-a2f service, loaded once, one line at a time.
// The service calls it through ctypes (src/gothic_a2f/engine.py). The run is the same as a2f-weights
// (reports/content/2026-11-hero-head/a2f/a2f-weights.cpp): one track, the same bundle, window and noise;
// only the skin weights (no tongue) leave, at the network's 60 frames per second.
#include "audio2face/audio2face.h"
#include "audio2x/cuda_utils.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct Destroyer { template <typename T> void operator()(T* o) const { if (o) o->Destroy(); } };
template <typename T> using UniquePtr = std::unique_ptr<T, Destroyer>;

struct Engine {
  UniquePtr<nva2f::IBlendshapeExecutorBundle> bundle;
  bool gpuSolver = false;
  std::size_t weights = 0;
  std::size_t emotions = 0;
  std::vector<std::string> names;
  // the run in progress: frames by their 60 fps index
  std::mutex m;
  std::vector<float> out;
  std::vector<char> seen;
  std::size_t frames = 0;
  cudaStream_t copyStream = nullptr;  // the stream of the device copies, synchronised after the run
  std::string error;
};

void setError(char* err, std::size_t len, const std::string& text) {
  if (err && len) std::snprintf(err, len, "%s", text.c_str());
}

// the 60 fps frame index of a result timestamp (samples at 16 kHz, rounded by the SDK)
long long frameIndex(long long t) { return t < 0 ? -1 : (t * 60 + 8000) / 16000; }

// the frame's place in the output, nullptr (with the error set) when the result does not fit the line
float* slot(Engine& e, long long t, std::size_t n) {
  const long long i = frameIndex(t);
  if (n != e.weights) { e.error = "unexpected weight count " + std::to_string(n); return nullptr; }
  if (i < 0 || static_cast<std::size_t>(i) >= e.frames) {
    e.error = "frame outside the line at sample " + std::to_string(t);
    return nullptr;
  }
  e.seen[static_cast<std::size_t>(i)] = 1;
  return e.out.data() + static_cast<std::size_t>(i) * n;
}

bool storeDevice(Engine& e, const nva2f::IBlendshapeExecutor::DeviceResults& r) {
  std::lock_guard<std::mutex> l(e.m);
  float* dst = slot(e, static_cast<long long>(r.timeStampCurrentFrame), r.weights.Size());
  if (!dst) return false;
  // the solver reuses one device buffer per track: the copy is queued on its stream before the next solve
  auto ec = nva2x::CopyDeviceToHost(nva2x::HostTensorFloatView{dst, r.weights.Size()}, r.weights, r.cudaStream);
  if (ec) { e.error = "copy: " + ec.message(); return false; }
  e.copyStream = r.cudaStream;
  return true;
}

void storeHost(Engine& e, const nva2f::IBlendshapeExecutor::HostResults& r, std::error_code code) {
  std::lock_guard<std::mutex> l(e.m);
  if (code) { e.error = "solver: " + code.message(); return; }
  float* dst = slot(e, static_cast<long long>(r.timeStampCurrentFrame), r.weights.Size());
  if (dst) std::memcpy(dst, r.weights.Data(), r.weights.Size() * sizeof(float));
}

}  // namespace

extern "C" {

// model: model.json of the diffusion model; identity: index in its network_info.json "identities".
void* a2f_open(const char* model, int identity, int gpuSolver, char* err, std::size_t errLen) {
  auto ec = nva2x::SetCudaDeviceIfNeeded(0);
  if (ec) { setError(err, errLen, "cuda device: " + ec.message()); return nullptr; }
  auto e = std::make_unique<Engine>();
  e->gpuSolver = gpuSolver != 0;
  e->bundle.reset(nva2f::ReadDiffusionBlendshapeSolveExecutorBundle(
      1, model, nva2f::IGeometryExecutor::ExecutionOption::Skin, e->gpuSolver,
      static_cast<std::size_t>(identity), false, nullptr, nullptr));
  if (!e->bundle) { setError(err, errLen, std::string("cannot load the model ") + model); return nullptr; }
  auto& ex = e->bundle->GetExecutor();
  const auto expected = e->gpuSolver ? nva2f::IBlendshapeExecutor::ResultsType::DEVICE
                                     : nva2f::IBlendshapeExecutor::ResultsType::HOST;
  if (ex.GetResultType() != expected) { setError(err, errLen, "unexpected result type of the solver"); return nullptr; }
  std::size_t num = 0, den = 0;
  ex.GetFrameRate(num, den);
  if (ex.GetSamplingRate() != 16000 || num != 60 * den) {
    setError(err, errLen, "the model is not 16 kHz in, 60 fps out");
    return nullptr;
  }
  e->weights = ex.GetWeightCount();
  nva2f::IBlendshapeSolver* solver = nullptr;
  ec = nva2f::GetExecutorSkinSolver(ex, 0, &solver);
  if (ec || !solver) { setError(err, errLen, "skin solver: " + ec.message()); return nullptr; }
  if (static_cast<std::size_t>(solver->NumBlendshapePoses()) != e->weights) {
    setError(err, errLen, "solver poses do not match the weights");
    return nullptr;
  }
  for (std::size_t i = 0; i < e->weights; ++i) e->names.emplace_back(solver->GetPoseName(i));
  e->emotions = e->bundle->GetEmotionAccumulator(0).GetEmotionSize();
  Engine* raw = e.get();
  if (e->gpuSolver)
    ec = ex.SetResultsCallback(
        [](void* ud, const nva2f::IBlendshapeExecutor::DeviceResults& r) -> bool {
          return storeDevice(*static_cast<Engine*>(ud), r);
        },
        raw);
  else
    ec = ex.SetResultsCallback(
        [](void* ud, const nva2f::IBlendshapeExecutor::HostResults& r, std::error_code code) {
          storeHost(*static_cast<Engine*>(ud), r, code);
        },
        raw);
  if (ec) { setError(err, errLen, "callback: " + ec.message()); return nullptr; }
  return e.release();
}

std::size_t a2f_weight_count(void* h) { return static_cast<Engine*>(h)->weights; }
std::size_t a2f_emotion_count(void* h) { return static_cast<Engine*>(h)->emotions; }
const char* a2f_pose_name(void* h, std::size_t i) { return static_cast<Engine*>(h)->names.at(i).c_str(); }

// Frames at 60 fps for `samples` (16 kHz mono float) with one emotion vector for the whole line.
// Writes up to `capacity` frames of a2f_weight_count() floats to `out`; returns the frame count, -1 on error.
long long a2f_run(void* h, const float* samples, std::size_t count, const float* emotion, float* out,
                  std::size_t capacity, char* err, std::size_t errLen) {
  auto& e = *static_cast<Engine*>(h);
  auto& b = *e.bundle;
  auto& ex = b.GetExecutor();
  auto fail = [&](const std::string& text) { setError(err, errLen, text); return -1LL; };
  auto check = [&](std::error_code ec, const char* what) {
    if (ec) setError(err, errLen, std::string(what) + ": " + ec.message());
    return !ec;
  };
  cudaStream_t stream = b.GetCudaStream().Data();
  // a fresh line: the noise of the diffusion restarts too, so a line gives the same weights every time
  if (!check(ex.Reset(0), "reset") || !check(b.GetAudioAccumulator(0).Reset(), "reset audio") ||
      !check(b.GetEmotionAccumulator(0).Reset(), "reset emotion"))
    return -1;
  if (!check(b.GetEmotionAccumulator(0).Accumulate(0, nva2x::HostTensorFloatConstView{emotion, e.emotions}, stream),
             "emotion") ||
      !check(b.GetEmotionAccumulator(0).Close(), "emotion close"))
    return -1;
  if (!check(b.GetAudioAccumulator(0).Accumulate(nva2x::HostTensorFloatConstView{samples, count}, stream), "audio") ||
      !check(b.GetAudioAccumulator(0).Close(), "audio close"))
    return -1;
  const std::size_t total = ex.GetTotalNbFrames(0);
  if (total > capacity) return fail("the line needs " + std::to_string(total) + " frames");
  {
    std::lock_guard<std::mutex> l(e.m);
    e.frames = total;
    e.out.assign(total * e.weights, 0.0f);
    e.seen.assign(total, 0);
    e.error.clear();
    e.copyStream = nullptr;
  }
  while (nva2x::GetNbReadyTracks(ex) > 0)
    if (!check(ex.Execute(nullptr), "execute")) return -1;
  if (!check(ex.Wait(0), "wait")) return -1;
  std::lock_guard<std::mutex> l(e.m);
  if (e.copyStream && cudaStreamSynchronize(e.copyStream) != cudaSuccess) return fail("synchronize the copies");
  if (!e.error.empty()) return fail(e.error);
  const auto missing = std::count(e.seen.begin(), e.seen.end(), 0);
  if (missing) return fail(std::to_string(missing) + " of " + std::to_string(total) + " frames missing");
  std::memcpy(out, e.out.data(), e.out.size() * sizeof(float));
  return static_cast<long long>(total);
}

void a2f_close(void* h) { delete static_cast<Engine*>(h); }

}  // extern "C"
