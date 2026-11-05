// a2f-weights: wav (16 kHz mono) -> per-frame ARKit skin + tongue weights as CSV.
// usage: a2f-weights <regression|diffusion> <model.json> <identity> <in.wav> <out.csv>
#include "audio2face/audio2face.h"
#include "audio2x/cuda_utils.h"
#include "AudioFile.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define CHECK_RESULT(func) { std::error_code e = (func); if (e) { std::cerr << "fail " << #func << ": " << e.message() << "\n"; std::exit(1); } }
#define CHECK_ERROR(x) { if (!(x)) { std::cerr << "fail " << #x << "\n"; std::exit(1); } }

struct Destroyer { template <typename T> void operator()(T* o) const { o->Destroy(); } };
template <typename T> using UniquePtr = std::unique_ptr<T, Destroyer>;

struct Frame { long long t; std::vector<float> w; };
struct Sink { std::mutex m; std::vector<Frame> frames; };

int main(int argc, char** argv) {
  if (argc != 6) { std::cerr << "usage: a2f-weights <regression|diffusion> <model.json> <identity> <in.wav> <out.csv>\n"; return 2; }
  const std::string mode = argv[1];
  const std::size_t identity = std::strtoul(argv[3], nullptr, 10);
  CHECK_RESULT(nva2x::SetCudaDeviceIfNeeded(0));

  AudioFile<float> af;
  CHECK_ERROR(af.load(argv[4]));
  CHECK_ERROR(af.getSampleRate() == 16000);
  const auto& audio = af.samples[0];

  auto t0 = std::chrono::steady_clock::now();
  UniquePtr<nva2f::IBlendshapeExecutorBundle> bundle;
  if (mode == "regression")
    bundle.reset(nva2f::ReadRegressionBlendshapeSolveExecutorBundle(1, argv[2], nva2f::IGeometryExecutor::ExecutionOption::SkinTongue, false, 30, 1, nullptr, nullptr));
  else
    bundle.reset(nva2f::ReadDiffusionBlendshapeSolveExecutorBundle(1, argv[2], nva2f::IGeometryExecutor::ExecutionOption::SkinTongue, false, identity, false, nullptr, nullptr));
  CHECK_ERROR(bundle);
  auto t1 = std::chrono::steady_clock::now();

  Sink sink;
  auto& ex = bundle->GetExecutor();
  CHECK_ERROR(ex.GetResultType() == nva2f::IBlendshapeExecutor::ResultsType::HOST);
  CHECK_RESULT(ex.SetResultsCallback([](void* ud, const nva2f::IBlendshapeExecutor::HostResults& r, std::error_code ec) {
    if (ec) { std::cerr << "callback: " << ec.message() << "\n"; return; }
    auto& s = *static_cast<Sink*>(ud);
    std::lock_guard<std::mutex> l(s.m);
    s.frames.push_back({(long long)r.timeStampCurrentFrame, std::vector<float>(r.weights.Data(), r.weights.Data() + r.weights.Size())});
  }, &sink));

  auto& emo = bundle->GetEmotionAccumulator(0);
  std::vector<float> neutral(emo.GetEmotionSize(), 0.0f);
  CHECK_RESULT(emo.Accumulate(0, nva2x::HostTensorFloatConstView{neutral.data(), neutral.size()}, bundle->GetCudaStream().Data()));
  CHECK_RESULT(emo.Close());

  CHECK_RESULT(bundle->GetAudioAccumulator(0).Accumulate(nva2x::HostTensorFloatConstView{audio.data(), audio.size()}, bundle->GetCudaStream().Data()));
  CHECK_RESULT(bundle->GetAudioAccumulator(0).Close());

  auto t2 = std::chrono::steady_clock::now();
  while (nva2x::GetNbReadyTracks(ex) > 0) CHECK_RESULT(ex.Execute(nullptr));
  CHECK_RESULT(ex.Wait(0));
  auto t3 = std::chrono::steady_clock::now();

  FILE* f = std::fopen(argv[5], "w");
  CHECK_ERROR(f);
  for (auto& fr : sink.frames) {
    std::fprintf(f, "%lld", fr.t);
    for (float w : fr.w) std::fprintf(f, ",%.4f", w);
    std::fprintf(f, "\n");
  }
  std::fclose(f);
  auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  std::printf("audio %.2f s, frames %zu, weights/frame %zu, load %.0f ms, run %.1f ms\n",
              audio.size() / 16000.0, sink.frames.size(), sink.frames.empty() ? 0 : sink.frames[0].w.size(), ms(t0, t1), ms(t2, t3));
  return 0;
}
