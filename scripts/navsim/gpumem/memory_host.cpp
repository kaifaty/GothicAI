// Host build of memory_kernels.h (one lane): the same code on a CPU, loaded
// through ctypes to check the port against the engine without a GPU.
#include "memory_kernels.h"

extern "C" {
int64_t gm_ctx_bytes() { return int64_t(sizeof(gm::Ctx)); }
int64_t gm_table_bytes() { return int64_t(sizeof(gm::Table)); }

void gm_update(const gm::Ctx* c) {
  for (int64_t a = 0; a < c->N; ++a) gm::prepareAgent<1>(*c, a, 0);
  for (int64_t a = 0; a < c->N; ++a)
    for (int grid = 0; grid < 2; ++grid) gm::ingestAgent<1>(*c, a, grid, 0);
}

void gm_read(const gm::Ctx* c, const int32_t* agents, int64_t count, float* near, float* far) {
  for (int64_t k = 0; k < count; ++k)
    for (int r = 0; r < gm::Outputs; ++r) gm::readOutput(*c, agents[k], k, r, near, far);
}

void gm_scatter(const gm::Ctx* c, const gm::Table* t, const int32_t* updates) {
  for (int64_t a = 0; a < c->N; ++a) gm::scatterAgent<1>(*c, *t, a, updates[a], 0);
}
}
