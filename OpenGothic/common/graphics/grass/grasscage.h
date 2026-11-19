#pragma once

#include <cstdint>
#include <string_view>

namespace Tempest {
class CommandBuffer;
class ComputePipeline;
class RenderPipeline;
template<class T> class Encoder;
}

namespace GrassCage {

int32_t parseSteps(std::string_view value);
void    setSteps(int32_t steps);

void bind(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const Tempest::ComputePipeline& regular,
          const Tempest::ComputePipeline& coarse);
void bind(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const Tempest::RenderPipeline& regular,
          const Tempest::RenderPipeline& coarse);

}
