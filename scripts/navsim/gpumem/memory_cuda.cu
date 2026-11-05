// PyTorch extension: launches the memory kernels on the current stream.
// Compiled by gpumem.py with --fmad=false and without --use_fast_math.
#include <torch/extension.h>
#include <ATen/cuda/CUDAContext.h>

#include "memory_kernels.h"
#include "passage_kernels.h"

namespace {
constexpr int Warp = 32;

template <int L> __global__ void prepareKernel(gm::Ctx c) {
  const int64_t thread = int64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  const int64_t a = thread / L;
  if (a < c.N) gm::prepareAgent<L>(c, a, int(thread % L));
}

template <int L> __global__ void ingestKernel(gm::Ctx c) {
  const int64_t thread = int64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  const int64_t worker = thread / L;
  if (worker < 2 * c.N) gm::ingestAgent<L>(c, worker / 2, int(worker % 2), int(thread % L));
}

__global__ void readKernel(gm::Ctx c, const int32_t* agents, int64_t count, float* near, float* far) {
  const int64_t thread = int64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  if (thread >= count * gm::Outputs) return;
  const int64_t k = thread / gm::Outputs;
  gm::readOutput(c, agents[k], k, int(thread % gm::Outputs), near, far);
}

__global__ void passageKernel(gm::Ctx c, const int32_t* agents, const float* segments,
                              int64_t count, uint8_t* verdicts) {
  const int64_t q = int64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  if (q < count) verdicts[q] = gm::passage::check(c, agents[q], segments + 8 * q);
}

__global__ void scatterKernel(gm::Ctx c, gm::Table t, const int32_t* updates) {
  const int64_t thread = int64_t(blockIdx.x) * blockDim.x + threadIdx.x;
  const int64_t a = thread / Warp;
  if (a < c.N) gm::scatterAgent<Warp>(c, t, a, updates[a], int(thread % Warp));
}

gm::Ctx context(int64_t address) { return *reinterpret_cast<const gm::Ctx*>(address); }
int64_t blocks(int64_t threads, int64_t block) { return (threads + block - 1) / block; }

void check() {
  const cudaError_t error = cudaGetLastError();
  TORCH_CHECK(error == cudaSuccess, "memory kernel launch failed: ", cudaGetErrorString(error));
}

// One warp per agent, then one warp per agent and grid.
void memoryUpdate(int64_t address) {
  const auto c = context(address);
  const auto stream = at::cuda::getCurrentCUDAStream();
  const int64_t block = 128;
  prepareKernel<Warp><<<blocks(c.N * Warp, block), block, 0, stream>>>(c);
  ingestKernel<Warp><<<blocks(2 * c.N * Warp, block), block, 0, stream>>>(c);
  check();
}

void memoryRead(int64_t address, torch::Tensor agents, torch::Tensor near, torch::Tensor far) {
  TORCH_CHECK(agents.is_cuda() && agents.dtype() == torch::kInt32 && agents.is_contiguous(), "agents: cuda int32");
  TORCH_CHECK(near.is_cuda() && near.dtype() == torch::kFloat32 && near.is_contiguous(), "near: cuda float32 CHW");
  TORCH_CHECK(far.is_cuda() && far.dtype() == torch::kFloat32 && far.is_contiguous(), "far: cuda float32 CHW");
  const int64_t count = agents.numel();
  TORCH_CHECK(near.numel() >= count * 10 * gm::NearOutputs && far.numel() >= count * 10 * gm::FarOutputs,
              "output too small");
  if (count == 0) return;
  const int64_t block = 256;
  readKernel<<<blocks(count * gm::Outputs, block), block, 0, at::cuda::getCurrentCUDAStream()>>>(
      context(address), agents.data_ptr<int32_t>(), count, near.data_ptr<float>(), far.data_ptr<float>());
  check();
}

void memoryPassage(int64_t address, torch::Tensor agents, torch::Tensor segments, torch::Tensor verdicts) {
  TORCH_CHECK(agents.is_cuda() && agents.dtype() == torch::kInt32 && agents.is_contiguous(), "agents: cuda int32");
  TORCH_CHECK(segments.is_cuda() && segments.dtype() == torch::kFloat32 && segments.is_contiguous(),
              "segments: cuda float32 [Q,8]");
  TORCH_CHECK(verdicts.is_cuda() && verdicts.dtype() == torch::kUInt8 && verdicts.is_contiguous(),
              "verdicts: cuda uint8");
  const int64_t count = agents.numel();
  TORCH_CHECK(segments.numel() == count * 8 && verdicts.numel() == count, "one segment and verdict per query");
  if (count == 0) return;
  // A half has only ~1,200 queries at N=2,048. One warp per block
  // spreads independent serial scans across SMs instead of launching ~10 blocks.
  const int64_t block = Warp;
  passageKernel<<<blocks(count, block), block, 0, at::cuda::getCurrentCUDAStream()>>>(
      context(address), agents.data_ptr<int32_t>(), segments.data_ptr<float>(), count, verdicts.data_ptr<uint8_t>());
  check();
}

void memoryScatter(int64_t address, int64_t table, torch::Tensor updates) {
  TORCH_CHECK(updates.is_cuda() && updates.dtype() == torch::kInt32 && updates.is_contiguous(), "updates: cuda int32");
  const auto c = context(address);
  TORCH_CHECK(updates.numel() == c.N, "one update per agent");
  const int64_t block = 128;
  scatterKernel<<<blocks(c.N * Warp, block), block, 0, at::cuda::getCurrentCUDAStream()>>>(
      c, *reinterpret_cast<const gm::Table*>(table), updates.data_ptr<int32_t>());
  check();
}
}  // namespace

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("update", &memoryUpdate);
  m.def("read", &memoryRead);
  m.def("scatter", &memoryScatter);
  m.def("passage", &memoryPassage);
  m.def("ctx_bytes", [] { return int64_t(sizeof(gm::Ctx)); });
  m.def("table_bytes", [] { return int64_t(sizeof(gm::Table)); });
}
