#include "graphics/grass/grasscage.h"
#include "graphics/shaders.h"

#include <Tempest/CommandBuffer>
#include <Tempest/Log>
#include <cstdlib>

#include "resources.h"
#include "shader.h"

using namespace Tempest;

namespace {

int32_t& grassCageSteps() {
  static int32_t steps = [] {
    const auto* value = std::getenv("OPENGOTHIC_GRASS_CAGE");
    if(value==nullptr)
      return 0;
    const auto parsed = GrassCage::parseSteps(value);
    if(parsed>=0)
      return parsed;
    Log::e("OPENGOTHIC_GRASS_CAGE expects 0, 1 or 2");
    return 0;
    }();
  return steps;
  }

template<class Pipeline>
void bindCagePipeline(Encoder<CommandBuffer>& cmd, const Pipeline& regular, const Pipeline& coarse) {
  const auto steps = grassCageSteps();
  if(steps>0 && !coarse.isEmpty()) {
    cmd.setPushData(steps);
    cmd.setPipeline(coarse);
    } else {
    cmd.setPipeline(regular);
    }
  }

}

int32_t GrassCage::parseSteps(std::string_view value) {
  if(value.size()==1 && value[0]>='0' && value[0]<='2')
    return value[0]-'0';
  return -1;
  }

void GrassCage::setSteps(int32_t steps) {
  grassCageSteps() = steps;
  }

void GrassCage::bind(Encoder<CommandBuffer>& cmd, const ComputePipeline& regular, const ComputePipeline& coarse) {
  bindCagePipeline(cmd, regular, coarse);
  }

void GrassCage::bind(Encoder<CommandBuffer>& cmd, const RenderPipeline& regular, const RenderPipeline& coarse) {
  bindCagePipeline(cmd, regular, coarse);
  }

void Shaders::compileGrassCage(const RenderState& state, const Shader& vs) {
  probeVoteGrassCage  = computeShader("probe_vote_grass_cage.comp.sprv");
  probeAllocGrassCage = computeShader("probe_allocation_grass_cage.comp.sprv");
  const auto sh = GothicShader::get("probe_ambient_grass_cage.frag.sprv");
  auto& device = Resources::device();
  auto fs = device.shader(sh.data, sh.len);
  probeAmbientGrassCage = device.pipeline(Triangles, state, vs, fs);
  }
