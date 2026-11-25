#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace TreeNanite::Pilot {

inline void check(VkResult result, const char* operation) {
  if(result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + ": Vulkan result " + std::to_string(result));
}

struct Buffer {
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* map = nullptr;
};

struct Image {
  VkImage handle = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

class Vulkan {
  public:
    Vulkan() = default;
    Vulkan(const Vulkan&) = delete;
    Vulkan& operator=(const Vulkan&) = delete;
    ~Vulkan() {
      if(device) {
        vkDeviceWaitIdle(device);
        for(auto pipeline : pipelines)
          vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        for(auto sampler : samplers)
          vkDestroySampler(device, sampler, nullptr);
        for(auto shader : shaders)
          vkDestroyShaderModule(device, shader, nullptr);
        vkDestroyQueryPool(device, statisticQuery, nullptr);
        vkDestroyQueryPool(device, timer, nullptr);
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        for(auto image : images) {
          vkDestroyImageView(device, image.view, nullptr);
          vkDestroyImage(device, image.handle, nullptr);
        }
        for(auto buffer : buffers)
          vkDestroyBuffer(device, buffer, nullptr);
        for(const auto& memory : allocations) {
          if(memory.mapped)
            vkUnmapMemory(device, memory.handle);
          vkFreeMemory(device, memory.handle, nullptr);
        }
        vkDestroyDevice(device, nullptr);
      }
      vkDestroyInstance(instance, nullptr);
    }

    void open() {
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
      app.pApplicationName = "Gothic tree visibility pilot";
      app.apiVersion = VK_API_VERSION_1_3;
      VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      ici.pApplicationInfo = &app;
      check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
      uint32_t count = 0;
      check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
      std::vector<VkPhysicalDevice> devices(count);
      check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
      for(auto candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if(props.vendorID != 0x10DE || props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
           std::strstr(props.deviceName, "RTX 3080") == nullptr ||
           props.apiVersion < VK_API_VERSION_1_3)
          continue;
        uint32_t extensionCount = 0;
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr),
              "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(extensionCount);
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()),
              "vkEnumerateDeviceExtensionProperties");
        const auto supports = [&](const char* name) {
          return std::any_of(extensions.begin(), extensions.end(), [&](const auto& ext) {
            return std::strcmp(ext.extensionName, name) == 0;
          });
        };
        if(supports(VK_EXT_MESH_SHADER_EXTENSION_NAME) && supports(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
          physical = candidate;
          break;
        }
      }
      if(!physical)
        throw std::runtime_error("An RTX 3080 with Vulkan 1.3, VK_EXT_mesh_shader and VK_EXT_memory_budget is required");

      VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props.pNext = &meshProperties;
      vkGetPhysicalDeviceProperties2(physical, &props);
      properties = props.properties;
      if(meshProperties.maxMeshOutputVertices < 64 || meshProperties.maxMeshOutputPrimitives < 64 ||
         meshProperties.maxMeshWorkGroupInvocations < 32 || meshProperties.maxMeshWorkGroupSize[0] < 32 ||
         properties.limits.maxColorAttachments < 5 || !properties.limits.timestampComputeAndGraphics)
        throw std::runtime_error("Device limits do not support the pilot raster");

      vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
      uint32_t familyCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
      std::vector<VkQueueFamilyProperties> families(familyCount);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
      const auto family = std::find_if(families.begin(), families.end(), [](const auto& queue) {
        return (queue.queueFlags & VK_QUEUE_GRAPHICS_BIT) && queue.timestampValidBits != 0;
      });
      if(family == families.end())
        throw std::runtime_error("No graphics queue with timestamps");
      const auto familyIndex = uint32_t(family - families.begin());
      timestampBits = family->timestampValidBits;

      VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
      VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      features13.pNext = &mesh;
      VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features.pNext = &features13;
      vkGetPhysicalDeviceFeatures2(physical, &features);
      if(!mesh.meshShader || !features13.dynamicRendering)
        throw std::runtime_error("Mesh shaders and dynamic rendering are required");
      statistics = mesh.meshShaderQueries && features.features.pipelineStatisticsQuery;
      mesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
      mesh.meshShader = VK_TRUE;
      mesh.meshShaderQueries = statistics;
      features13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      features13.pNext = &mesh;
      features13.dynamicRendering = VK_TRUE;
      features.features = {};
      features.features.pipelineStatisticsQuery = statistics;
      const float priority = 1.f;
      VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queueInfo.queueFamilyIndex = familyIndex;
      queueInfo.queueCount = 1;
      queueInfo.pQueuePriorities = &priority;
      const std::array extensions{VK_EXT_MESH_SHADER_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
      VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      dci.pNext = &features;
      dci.queueCreateInfoCount = 1;
      dci.pQueueCreateInfos = &queueInfo;
      dci.enabledExtensionCount = uint32_t(extensions.size());
      dci.ppEnabledExtensionNames = extensions.data();
      check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
      drawMesh = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksEXT"));
      if(!drawMesh)
        throw std::runtime_error("vkCmdDrawMeshTasksEXT is unavailable");
      vkGetDeviceQueue(device, familyIndex, 0, &queue);
      VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool.queueFamilyIndex = familyIndex;
      pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      check(vkCreateCommandPool(device, &pool, nullptr, &commandPool), "vkCreateCommandPool");
      VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ci.commandPool = commandPool;
      ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ci.commandBufferCount = 1;
      check(vkAllocateCommandBuffers(device, &ci, &cmd), "vkAllocateCommandBuffers");
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qi.queryCount = 8;
      check(vkCreateQueryPool(device, &qi, nullptr, &timer), "timestamp query pool");
      if(statistics) {
        qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qi.queryCount = 1;
        qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT |
                                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        check(vkCreateQueryPool(device, &qi, nullptr, &statisticQuery), "pipeline statistic query pool");
      }
    }

    void descriptors(const std::array<VkDescriptorType, 11>& types) {
      std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
      for(uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      li.bindingCount = uint32_t(bindings.size());
      li.pBindings = bindings.data();
      check(vkCreateDescriptorSetLayout(device, &li, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
      const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6},
                             VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                             VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}};
      VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pi.maxSets = 1;
      pi.poolSizeCount = uint32_t(sizes.size());
      pi.pPoolSizes = sizes.data();
      check(vkCreateDescriptorPool(device, &pi, nullptr, &descriptorPool), "vkCreateDescriptorPool");
      VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.descriptorPool = descriptorPool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &setLayout;
      check(vkAllocateDescriptorSets(device, &ai, &set), "vkAllocateDescriptorSets");
      VkPushConstantRange push{VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(uint32_t)};
      VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pci.setLayoutCount = 1;
      pci.pSetLayouts = &setLayout;
      pci.pushConstantRangeCount = 1;
      pci.pPushConstantRanges = &push;
      check(vkCreatePipelineLayout(device, &pci, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
    }

    VkPipeline pipeline(const std::string& dir, const char* stageName, const char* fragmentName, bool mesh,
                        std::vector<VkFormat> formats, VkFormat depth, uint32_t resolveMode = 0) {
      std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
      stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                   mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
                   shader(dir + "/" + stageName + ".spv"), "main", nullptr};
      stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                   shader(dir + "/" + fragmentName + ".spv"), "main", nullptr};
      const VkSpecializationMapEntry entry{0, 0, sizeof(resolveMode)};
      const VkSpecializationInfo specialization{1, &entry, sizeof(resolveMode), &resolveMode};
      if(resolveMode != 0)
        stages[1].pSpecializationInfo = &specialization;
      VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      viewport.viewportCount = 1;
      viewport.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      raster.polygonMode = VK_POLYGON_MODE_FILL;
      raster.cullMode = VK_CULL_MODE_NONE;
      raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
      raster.lineWidth = 1.f;
      VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      ds.depthTestEnable = ds.depthWriteEnable = depth != VK_FORMAT_UNDEFINED;
      ds.depthCompareOp = VK_COMPARE_OP_LESS;
      std::vector<VkPipelineColorBlendAttachmentState> colors(formats.size());
      for(auto& color : colors)
        color.colorWriteMask = 0xf;
      VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      blend.attachmentCount = uint32_t(colors.size());
      blend.pAttachments = colors.data();
      const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dynamic.dynamicStateCount = uint32_t(dynamicStates.size());
      dynamic.pDynamicStates = dynamicStates.data();
      VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
      rendering.colorAttachmentCount = uint32_t(formats.size());
      rendering.pColorAttachmentFormats = formats.data();
      rendering.depthAttachmentFormat = depth;
      VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pi.pNext = &rendering;
      pi.stageCount = uint32_t(stages.size());
      pi.pStages = stages.data();
      pi.pVertexInputState = mesh ? nullptr : &vertices;
      pi.pInputAssemblyState = mesh ? nullptr : &assembly;
      pi.pViewportState = &viewport;
      pi.pRasterizationState = &raster;
      pi.pMultisampleState = &samples;
      pi.pDepthStencilState = &ds;
      pi.pColorBlendState = &blend;
      pi.pDynamicState = &dynamic;
      pi.layout = pipelineLayout;
      VkPipeline result = VK_NULL_HANDLE;
      check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &result), fragmentName);
      pipelines.push_back(result);
      return result;
    }

    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = false) {
      Buffer result{};
      result.size = size;
      VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      info.size = size;
      info.usage = usage;
      check(vkCreateBuffer(device, &info, nullptr, &result.handle), "vkCreateBuffer");
      buffers.push_back(result.handle);
      VkMemoryRequirements req{};
      vkGetBufferMemoryRequirements(device, result.handle, &req);
      auto& mem = memory(req, host);
      check(vkBindBufferMemory(device, result.handle, mem.handle, 0), "vkBindBufferMemory");
      if(host) {
        check(vkMapMemory(device, mem.handle, 0, size, 0, &result.map), "vkMapMemory");
        mem.mapped = true;
      }
      return result;
    }

    Image image(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                uint32_t layers = 1, uint32_t mips = 1) {
      Image result{};
      result.format = format;
      result.aspect = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      VkImageFormatProperties supported{};
      check(vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                    usage, 0, &supported), "required image format");
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = format;
      info.extent = {width, height, 1};
      info.mipLevels = mips;
      info.arrayLayers = layers;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = usage;
      check(vkCreateImage(device, &info, nullptr, &result.handle), "vkCreateImage");
      images.push_back(result);
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(device, result.handle, &req);
      check(vkBindImageMemory(device, result.handle, memory(req, false).handle, 0), "vkBindImageMemory");
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = result.handle;
      vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange = {result.aspect, 0, mips, 0, layers};
      check(vkCreateImageView(device, &vi, nullptr, &result.view), "vkCreateImageView");
      images.back().view = result.view;
      return result;
    }

    VkSampler sampler(bool linear) {
      VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      si.minFilter = si.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      si.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
      si.addressModeU = si.addressModeV = si.addressModeW =
        linear ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.maxLod = VK_LOD_CLAMP_NONE;
      VkSampler result = VK_NULL_HANDLE;
      check(vkCreateSampler(device, &si, nullptr, &result), "vkCreateSampler");
      samplers.push_back(result);
      return result;
    }

    void begin() {
      check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
      VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      check(vkBeginCommandBuffer(cmd, &info), "vkBeginCommandBuffer");
    }

    void submit() {
      check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
      check(vkResetFences(device, 1, &fence), "vkResetFences");
      VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      info.commandBufferCount = 1;
      info.pCommandBuffers = &cmd;
      check(vkQueueSubmit(queue, 1, &info, fence), "vkQueueSubmit");
      check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }

    void transition(Image image, VkImageLayout oldLayout, VkImageLayout newLayout,
                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                    VkAccessFlags srcAccess, VkAccessFlags dstAccess) const {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = oldLayout;
      barrier.newLayout = newLayout;
      barrier.srcAccessMask = srcAccess;
      barrier.dstAccessMask = dstAccess;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image.handle;
      barrier.subresourceRange = {image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
      vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void memoryBarrier(VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                       VkAccessFlags srcAccess, VkAccessFlags dstAccess) const {
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = srcAccess;
      barrier.dstAccessMask = dstAccess;
      vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    void stamp(uint32_t query) const {
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timer, query);
    }

    double ms(uint64_t first, uint64_t last) const {
      const auto mask = timestampBits == 64 ? UINT64_MAX : (uint64_t(1) << timestampBits) - 1;
      return double((last - first) & mask) * double(properties.limits.timestampPeriod) / 1e6;
    }

    void deviceJson(std::ostream& out, const char* phase) const {
      VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
      VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
      memory.pNext = &budget;
      vkGetPhysicalDeviceMemoryProperties2(physical, &memory);
      out << "{\"type\":\"device\",\"phase\":\"" << phase << "\",\"device\":\"" << properties.deviceName
          << "\",\"driver_raw\":" << properties.driverVersion << ",\"api_raw\":" << properties.apiVersion
          << ",\"timestamp_period_ns\":" << properties.limits.timestampPeriod
          << ",\"maxMeshOutputMemorySize\":" << meshProperties.maxMeshOutputMemorySize
          << ",\"meshOutputPerVertexGranularity\":" << meshProperties.meshOutputPerVertexGranularity
          << ",\"meshOutputPerPrimitiveGranularity\":" << meshProperties.meshOutputPerPrimitiveGranularity
          << ",\"preferredMeshWorkGroupInvocations\":" << meshProperties.maxPreferredMeshWorkGroupInvocations
          << ",\"maxMeshWorkGroupInvocations\":" << meshProperties.maxMeshWorkGroupInvocations
          << ",\"maxMeshOutputVertices\":" << meshProperties.maxMeshOutputVertices
          << ",\"maxMeshOutputPrimitives\":" << meshProperties.maxMeshOutputPrimitives
          << ",\"maxMeshWorkGroupCountX\":" << meshProperties.maxMeshWorkGroupCount[0]
          << ",\"maxMeshWorkGroupTotalCount\":" << meshProperties.maxMeshWorkGroupTotalCount
          << ",\"pipeline_statistics\":" << (statistics ? "true" : "false") << ",\"heaps\":[";
      for(uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i) {
        if(i > 0)
          out << ',';
        out << "{\"device_local\":" << ((memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "true" : "false")
            << ",\"allocated_bytes\":" << heapAllocations[i]
            << ",\"usage_bytes\":" << budget.heapUsage[i] << ",\"budget_bytes\":" << budget.heapBudget[i] << '}';
      }
      out << "]}\n";
      out.flush();
    }

    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    VkDevice device = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkQueryPool timer = VK_NULL_HANDLE;
    VkQueryPool statisticQuery = VK_NULL_HANDLE;
    PFN_vkCmdDrawMeshTasksEXT drawMesh = nullptr;

  private:
    struct Allocation {
      VkDeviceMemory handle = VK_NULL_HANDLE;
      bool mapped = false;
    };

    Allocation& memory(VkMemoryRequirements req, bool host) {
      const VkMemoryPropertyFlags required = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      for(uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if((req.memoryTypeBits & (uint32_t(1) << i)) == 0 ||
           (memoryProperties.memoryTypes[i].propertyFlags & required) != required)
          continue;
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = req.size;
        info.memoryTypeIndex = i;
        Allocation allocation{};
        check(vkAllocateMemory(device, &info, nullptr, &allocation.handle), "vkAllocateMemory");
        allocations.push_back(allocation);
        heapAllocations[memoryProperties.memoryTypes[i].heapIndex] += req.size;
        return allocations.back();
      }
      throw std::runtime_error("Required Vulkan memory type is unavailable");
    }

    VkShaderModule shader(const std::string& path) {
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      const auto size = file.tellg();
      if(size <= 0 || size % 4 != 0)
        throw std::runtime_error("Invalid SPIR-V: " + path);
      std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
      file.seekg(0);
      file.read(reinterpret_cast<char*>(code.data()), size);
      if(!file)
        throw std::runtime_error("Cannot read SPIR-V: " + path);
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = size_t(size);
      info.pCode = code.data();
      VkShaderModule result = VK_NULL_HANDLE;
      check(vkCreateShaderModule(device, &info, nullptr, &result), "vkCreateShaderModule");
      shaders.push_back(result);
      return result;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heapAllocations{};
    bool statistics = false;
    uint32_t timestampBits = 0;
    std::vector<VkPipeline> pipelines;
    std::vector<VkShaderModule> shaders;
    std::vector<VkSampler> samplers;
    std::vector<VkBuffer> buffers;
    std::vector<Image> images;
    std::vector<Allocation> allocations;
};

}
