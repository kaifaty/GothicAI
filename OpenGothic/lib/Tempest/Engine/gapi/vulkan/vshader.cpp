#if defined(TEMPEST_BUILD_VULKAN)

#include "vshader.h"

#include "vdevice.h"

#include <algorithm>
#include <libspirv/libspirv.h>

using namespace Tempest::Detail;

VShader::VShader(VDevice& device, const void *source, size_t src_size)
  :Shader(source, src_size), device(device.device.impl) {
  const auto* words     = reinterpret_cast<const uint32_t*>(source);
  const auto  wordCount = src_size/sizeof(uint32_t);
  if(!device.props.hasDescriptorHeap && stage!=ShaderReflection::Compute &&
     std::any_of(lay.begin(), lay.end(), [](const Binding& slot) { return slot.isArray(); })) {
    splitSource.assign(words, words + wordCount);
    }

  impl = createModule(words, wordCount);

  VkDebugMarkerObjectNameInfoEXT nameInfo = {VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT};
  nameInfo.objectType   = VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT;
  nameInfo.object       = impl;
  nameInfo.pObjectName  = this->dbgShortName();
  device.vkDebugMarkerSetObjectName(device.device.impl, &nameInfo);
  }

VShader::VShader(VDevice& device)
  :Shader(), device(device.device.impl) {
  }

VShader::~VShader() {
  if(splitModule!=VK_NULL_HANDLE)
    vkDestroyShaderModule(device,splitModule,nullptr);
  if(impl!=VK_NULL_HANDLE)
    vkDestroyShaderModule(device,impl,nullptr);
  }

VkShaderModule VShader::createModule(const uint32_t* words, size_t wordCount) const {
  VkShaderModuleCreateInfo info = {};
  info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = wordCount*sizeof(uint32_t);
  info.pCode    = words;

  VkShaderModule module = VK_NULL_HANDLE;
  if(vkCreateShaderModule(device,&info,nullptr,&module)!=VK_SUCCESS)
    throw std::system_error(Tempest::GraphicsErrc::InvalidShaderModule, "\"" + dbg.source + "\"");
  return module;
  }

VkShaderModule VShader::splitDescriptorsModule() const {
  std::lock_guard<std::mutex> guard(splitSync);
  if(splitModule!=VK_NULL_HANDLE)
    return splitModule;
  if(splitSource.empty())
    return impl;

  libspirv::MutableBytecode code(splitSource.data(), splitSource.size());
  const auto begin = code.findSection(libspirv::Bytecode::S_Annotations);
  const auto end   = code.findSection(begin, libspirv::Bytecode::S_Types);
  for(const auto& slot:lay) {
    if(!slot.isArray())
      continue;

    auto decoration = begin;
    for(; decoration!=end; ++decoration) {
      const auto& op = *decoration;
      if(op.op()==spv::OpDecorate && op.length()==4 &&
         op[1]==uint32_t(slot.spvId) && op[2]==spv::DecorationDescriptorSet)
        break;
      }
    if(decoration==end || (*decoration)[3]!=0) {
      const auto binding = std::to_string(slot.layout);
      throw std::system_error(Tempest::GraphicsErrc::InvalidShaderModule,
                              "Expected a direct set 0 decoration for binding " + binding + " in \"" + dbg.source + "\"");
      }

    decoration.set(3, 1u);
    }

  splitModule = createModule(code.opcodes(), code.size());
  std::vector<uint32_t>().swap(splitSource);
  return splitModule;
  }

#endif
