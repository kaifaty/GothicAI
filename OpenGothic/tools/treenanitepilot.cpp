#include "treenanitecompare.h"
#include "treenanitescene.h"
#include "treenanitevulkan.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace TreeNanite::Pilot {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t Width = 1280, Height = 720;
constexpr size_t Pixels = size_t(Width) * Height;
constexpr VkFormat OwnerFormat = VK_FORMAT_R32G32_UINT;
constexpr VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat EdgeFormat = VK_FORMAT_R8_UINT;
const std::vector<VkFormat> GbufferFormats{
  VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT
};
constexpr std::array Profiles{"empty", "sparse", "dense", "dense-high"};

enum class Mode { Direct, Visibility, VerifyDirect, VerifyVisibility };
enum Query : uint32_t { Start, AfterClear, AfterColorBarrier, AfterRaster, AfterReadBarrier, Finish, QueryCount };
enum class VerificationExit : int { Passed = 0, RasterMismatch = 1, MaterialMismatch = 3 };

struct Timing {
  double clear = 0, raster = 0, barrier = 0, resolve = 0, gpu = 0;
};

double msecs(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

std::vector<VkFormat> withDiagnostic(VkFormat diagnostic) {
  auto result = GbufferFormats;
  result.push_back(diagnostic);
  return result;
}

bool viaVisibility(Mode mode) {
  return mode == Mode::Visibility || mode == Mode::VerifyVisibility;
}

class Renderer {
  public:
    void setup(const Scene& scene, const std::string& shaderDir, std::string_view mode, std::ostream& out,
               const std::filesystem::path& textureDir = {}, bool cards = false) {
      const bool verify = mode == "verify";
      const bool route = mode == "route";
      gpu.open(route);
      gpu.deviceJson(out, "before");
      size_t drawCount = scene.meshlets.size(), instanceCount = 1;
      if(!route) {
        const auto capacity = scene.prepare("dense-high");
        drawCount = capacity.draws.size();
        instanceCount = capacity.instances.size();
      }
      if(verify) {
        for(const auto name : Scene::testNames()) {
          const auto prepared = scene.prepare(name);
          drawCount = std::max(drawCount, prepared.draws.size());
          instanceCount = std::max(instanceCount, prepared.instances.size());
        }
      }
      const auto ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      data[0] = staticBuffer(scene.vertices);
      data[1] = staticBuffer(scene.meshlets);
      data[2] = staticBuffer(scene.words);
      data[3] = gpu.buffer(std::max(drawCount, size_t(1)) * sizeof(DrawRecord),
                           ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      data[4] = gpu.buffer(std::max(instanceCount, size_t(1)) * sizeof(Instance),
                           ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      data[7] = gpu.buffer(sizeof(Frame) + (route ? sizeof(RouteUniform) : 0), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
      auto materialData = scene.materials;
      if(route && !cards)
        materialData[1].tint = {1.021f, 1.021f, 1.021f, 1.f};
      data[10] = staticBuffer(materialData);
      recordUpload = gpu.buffer(data[3].size + data[4].size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);

      constexpr VkImageUsageFlags ColorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      for(uint32_t i = 0; i < gbuffer.size(); ++i)
        gbuffer[i] = gpu.image(GbufferFormats[i], Width, Height, ColorUsage |
                               (route ? VK_IMAGE_USAGE_SAMPLED_BIT : 0));
      const VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      depth = gpu.image(DepthFormat, Width, Height, depthUsage | (route ? VK_IMAGE_USAGE_SAMPLED_BIT : 0));
      if(verify || mode == "vb" || route) {
        visibility = gpu.image(OwnerFormat, Width, Height, ColorUsage | VK_IMAGE_USAGE_SAMPLED_BIT);
        colorImage[6] = visibility;
      }
      if(route) {
        colorImage[5] = routeTexture(textureDir / "albedo.rgba", 2048u, VK_FORMAT_R8G8B8A8_SRGB);
        colorImage[8] = routeTexture(textureDir / "normal.rgba", 2048u, VK_FORMAT_R8G8B8A8_UNORM);
        colorImage[9] = routeTexture(textureDir / "orm.rgba", 1024u, VK_FORMAT_R8G8B8A8_UNORM);
        data[11] = staticBuffer(scene.skin);
        data[12] = gpu.buffer(2 * scene.bones.size() * sizeof(Pose), ssbo, true);
        data[13] = staticBuffer(scene.bones);
        shadowDepth = gpu.image(DepthFormat, Width, Height, depthUsage | VK_IMAGE_USAGE_SAMPLED_BIT);
        presentation = gpu.image(GbufferFormats[0], Width, Height, ColorUsage);
        colorImage[14] = gbuffer[0];
        colorImage[15] = gbuffer[1];
        colorImage[16] = depth;
        colorImage[17] = shadowDepth;
        colorImage[18] = gbuffer[2];
      } else {
        const auto materials = std::max(2u, uint32_t(scene.materials.size()));
        colorImage[5] = makeTexture(materials, 0);
        colorImage[8] = makeTexture(materials, 1);
        colorImage[9] = makeTexture(materials, 2);
      }
      bindResources(route);
      if(route) {
        pose = gpu.compute(shaderDir, "tree_nanite_pose.comp");
        raster = gpu.pipeline(shaderDir, "tree_nanite_route.mesh", "tree_nanite_route_visibility.frag", true,
                              {OwnerFormat}, DepthFormat);
        shadow = gpu.pipeline(shaderDir, "tree_nanite_route.mesh", "tree_nanite_route_shadow.frag", true,
                              {}, DepthFormat);
        resolve = gpu.pipeline(shaderDir, "tree_nanite_resolve.vert", "tree_nanite_route_resolve.frag", false,
                               GbufferFormats, VK_FORMAT_UNDEFINED);
        paint = gpu.pipeline(shaderDir, "tree_nanite_resolve.vert", "tree_nanite_route.frag", false,
                             {GbufferFormats[0]}, VK_FORMAT_UNDEFINED);
        readback[1] = gpu.buffer(Pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        gpu.deviceJson(out, "resident");
        return;
      }
      if(verify || mode == "direct")
        direct = gpu.pipeline(shaderDir, "tree_nanite_direct.mesh", "tree_nanite_direct.frag", true,
                              GbufferFormats, DepthFormat);
      if(verify || mode == "vb")
        raster = gpu.pipeline(shaderDir, "tree_nanite_raster.mesh", "tree_nanite_visibility.frag", true,
                              {OwnerFormat}, DepthFormat);
      if(mode == "vb") {
        resolve = gpu.pipeline(shaderDir, "tree_nanite_resolve.vert", "tree_nanite_resolve.frag", false,
                               GbufferFormats, VK_FORMAT_UNDEFINED);
        for(uint32_t i = 0; i < resolveVariants.size(); ++i)
          resolveVariants[i] = gpu.pipeline(shaderDir, "tree_nanite_resolve.vert", "tree_nanite_resolve.frag",
                                            false, GbufferFormats, VK_FORMAT_UNDEFINED, i + 1);
      }
      gpu.deviceJson(out, "resident");
      if(verify || mode == "direct")
        referenceOwner = gpu.image(OwnerFormat, Width, Height, ColorUsage);
      if(verify) {
        edge = gpu.image(EdgeFormat, Width, Height, ColorUsage);
        readback[0] = gpu.buffer(Pixels * 29, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        readback[1] = gpu.buffer(Pixels * 29, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      } else {
        readback[1] = gpu.buffer(Pixels * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      }
      if(verify || mode == "direct") {
        directVerify = gpu.pipeline(shaderDir, "tree_nanite_direct_verify.mesh", "tree_nanite_direct_verify.frag",
                                    true, withDiagnostic(OwnerFormat), DepthFormat);
      }
      if(verify)
        resolveVerify = gpu.pipeline(shaderDir, "tree_nanite_resolve.vert", "tree_nanite_resolve_verify.frag",
                                     false, withDiagnostic(EdgeFormat), VK_FORMAT_UNDEFINED);
      gpu.deviceJson(out, "resident_with_diagnostics");
    }

    void upload(const Submission& submission, const RouteUniform* route = nullptr) {
      const auto drawBytes = submission.draws.size() * sizeof(DrawRecord);
      const auto instanceBytes = submission.instances.size() * sizeof(Instance);
      if(drawBytes > data[3].size || instanceBytes > data[4].size)
        throw std::runtime_error("A draw record upload exceeds the pilot capacity");
      auto frame = submission.frame;
      frame.control[0] = gpu.properties.limits.subPixelPrecisionBits;
      std::memcpy(data[7].map, &frame, sizeof(Frame));
      if(route)
        std::memcpy(static_cast<uint8_t*>(data[7].map) + sizeof(Frame), route, sizeof(*route));
      gpu.begin();
      const auto copy = [&](const auto& source, size_t size, size_t offset, Buffer target) {
        if(size) {
          std::memcpy(static_cast<uint8_t*>(recordUpload.map) + offset, source, size);
          const VkBufferCopy region{offset, 0, size};
          vkCmdCopyBuffer(gpu.cmd, recordUpload.handle, target.handle, 1, &region);
        }
      };
      copy(submission.draws.data(), drawBytes, 0, data[3]);
      copy(submission.instances.data(), instanceBytes, data[3].size, data[4]);
      uploadBarrier();
      gpu.submit();
    }

    void drawRoute(const RouteFrame& route, std::vector<uint8_t>& rgb) {
      upload(route.shadow, &route.uniform);
      gpu.begin();
      computePose(route.view.frame.control[2]);
      clearDepth(shadowDepth);
      render({}, &shadowDepth, shadow, &route.shadow);
      depthSampleReady(shadowDepth);
      gpu.submit();

      upload(route.view, &route.uniform);
      gpu.begin();
      commonClear();
      clearColor(visibility, {});
      colorReady(visibility);
      render({visibility}, &depth, raster, &route.view);
      colorSampleReady(visibility);
      render({gbuffer[0], gbuffer[1], gbuffer[2], gbuffer[3]}, nullptr, resolve, nullptr);
      colorSampleReady(gbuffer[0]);
      colorSampleReady(gbuffer[1]);
      colorSampleReady(gbuffer[2]);
      depthSampleReady(depth);
      render({presentation}, nullptr, paint, nullptr, true);
      gpu.transition(presentation, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {Width, Height, 1};
      vkCmdCopyImageToBuffer(gpu.cmd, presentation.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback[1].handle, 1, &copy);
      gpu.memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
      gpu.submit();
      const auto* rgba = static_cast<const uint8_t*>(readback[1].map);
      for(size_t i = 0; i < Pixels; ++i)
        std::memcpy(rgb.data() + 3*i, rgba + 4*i, 3);
    }

    const Pose* readPose(const Frame& frame, const RouteUniform& uniform) {
      std::memcpy(data[7].map, &frame, sizeof(Frame));
      std::memcpy(static_cast<uint8_t*>(data[7].map) + sizeof(Frame), &uniform, sizeof(uniform));
      gpu.begin();
      computePose(frame.control[2]);
      gpu.submit();
      return static_cast<const Pose*>(data[12].map);
    }

    Timing draw(const Submission& submission, Mode mode, bool captured = false, bool fullCapture = false,
                VkPipeline diagnosticResolve = VK_NULL_HANDLE, bool counters = false) {
      const bool vb = viaVisibility(mode);
      const bool verify = mode == Mode::VerifyDirect || mode == Mode::VerifyVisibility;
      gpu.begin();
      vkCmdResetQueryPool(gpu.cmd, gpu.timer, 0, QueryCount);
      if(counters && gpu.statisticQuery)
        vkCmdResetQueryPool(gpu.cmd, gpu.statisticQuery, 0, 1);

      commonClear();
      gpu.stamp(Start);
      if(vb)
        clearColor(visibility, {});
      gpu.stamp(AfterClear);
      if(vb)
        colorReady(visibility);
      gpu.stamp(AfterColorBarrier);

      if(counters && gpu.statisticQuery)
        vkCmdBeginQuery(gpu.cmd, gpu.statisticQuery, 0, 0);
      if(vb)
        render({visibility}, &depth, raster, &submission);
      else if(verify)
        render({gbuffer[0], gbuffer[1], gbuffer[2], gbuffer[3], referenceOwner},
               &depth, directVerify, &submission, true);
      else
        render({gbuffer[0], gbuffer[1], gbuffer[2], gbuffer[3]}, &depth, direct, &submission);
      if(counters && gpu.statisticQuery)
        vkCmdEndQuery(gpu.cmd, gpu.statisticQuery, 0);
      gpu.stamp(AfterRaster);

      if(vb)
        gpu.transition(visibility, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
      gpu.stamp(AfterReadBarrier);
      if(mode == Mode::VerifyVisibility)
        render({gbuffer[0], gbuffer[1], gbuffer[2], gbuffer[3], edge}, nullptr, resolveVerify, nullptr, true);
      else if(vb)
        render({gbuffer[0], gbuffer[1], gbuffer[2], gbuffer[3]}, nullptr,
               diagnosticResolve ? diagnosticResolve : resolve, nullptr);
      gpu.stamp(Finish);
      if(captured)
        capture(mode, fullCapture);
      gpu.submit();

      std::array<uint64_t, QueryCount> ticks{};
      check(vkGetQueryPoolResults(gpu.device, gpu.timer, 0, QueryCount, sizeof(ticks), ticks.data(),
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "GPU timestamps");
      Timing result;
      result.gpu = gpu.ms(ticks[Start], ticks[Finish]);
      result.raster = gpu.ms(ticks[AfterColorBarrier], ticks[AfterRaster]);
      if(vb) {
        result.clear = gpu.ms(ticks[Start], ticks[AfterClear]);
        result.barrier = gpu.ms(ticks[AfterClear], ticks[AfterColorBarrier]) +
                         gpu.ms(ticks[AfterRaster], ticks[AfterReadBarrier]);
        result.resolve = gpu.ms(ticks[AfterReadBarrier], ticks[Finish]);
      }
      return result;
    }

    void stats(std::ostream& out) {
      if(!gpu.statisticQuery)
        return;
      std::array<uint64_t, 2> results{};
      check(vkGetQueryPoolResults(gpu.device, gpu.statisticQuery, 0, 1, sizeof(results), results.data(),
                                  sizeof(results), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "GPU counters");
      out << ",\"fragment_invocations\":" << results[0] << ",\"mesh_invocations\":" << results[1];
    }

    Readback imageData(uint32_t index) const {
      const auto* p = static_cast<const uint8_t*>(readback[index].map);
      return {p, p + Pixels * 8, p + Pixels * 12, p + Pixels * 16, p + Pixels * 20, p + Pixels * 24};
    }

    const uint8_t* edgeMask() const {
      return static_cast<const uint8_t*>(readback[1].map) + Pixels * 28;
    }

    const uint8_t* owners() const { return static_cast<const uint8_t*>(readback[1].map); }

    void memory(std::ostream& out, const char* phase) const { gpu.deviceJson(out, phase); }

    const std::array<VkPipeline, 3>& diagnostics() const { return resolveVariants; }

  private:
    template<class T>
    Buffer staticBuffer(const std::vector<T>& values) {
      if(values.empty())
        throw std::runtime_error("Static geometry buffer is empty");
      const auto size = values.size() * sizeof(T);
      if(size > gpu.properties.limits.maxStorageBufferRange)
        throw std::runtime_error("Static geometry exceeds maxStorageBufferRange");
      auto staging = gpu.buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
      auto target = gpu.buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      std::memcpy(staging.map, values.data(), size);
      gpu.begin();
      const VkBufferCopy range{0, 0, size};
      vkCmdCopyBuffer(gpu.cmd, staging.handle, target.handle, 1, &range);
      uploadBarrier();
      gpu.submit();
      return target;
    }

    void uploadBarrier() {
      gpu.memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    void computePose(uint32_t boneCount) {
      vkCmdBindPipeline(gpu.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pose);
      vkCmdBindDescriptorSets(gpu.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipelineLayout, 0, 1, &gpu.set, 0, nullptr);
      vkCmdDispatch(gpu.cmd, (boneCount + 31u) / 32u, 2, 1);
      gpu.memoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_HOST_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
    }

    Image makeTexture(uint32_t layers, int channel) {
      constexpr uint32_t Size = 128, Mips = 8;
      auto image = gpu.image(VK_FORMAT_R8G8B8A8_UNORM, Size, Size,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, layers, Mips);
      std::vector<uint8_t> texels;
      std::vector<VkBufferImageCopy> copies;
      for(uint32_t mip = 0; mip < Mips; ++mip) {
        const uint32_t side = Size >> mip;
        for(uint32_t layer = 0; layer < layers; ++layer) {
          VkBufferImageCopy copy{};
          copy.bufferOffset = texels.size();
          copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
          copy.imageExtent = {side, side, 1};
          copies.push_back(copy);
          for(uint32_t y = 0; y < side; ++y) {
            for(uint32_t x = 0; x < side; ++x) {
              const auto low = uint8_t((x * 8 / side + y * 8 / side + layer) % 2);
              const auto mipTint = uint8_t(7 * mip);
              if(channel == 0)
                texels.insert(texels.end(), {uint8_t(60 + 16 * low + mipTint), uint8_t(95 + 28 * low + mipTint),
                                             uint8_t(41 + 16 * low + mipTint), 255});
              if(channel == 1)
                texels.insert(texels.end(), {128, uint8_t(122 + 6 * low + mip), 255,
                                             uint8_t(126 + 8 * low + mip)});
              if(channel == 2)
                texels.insert(texels.end(), {uint8_t(210 - mipTint), uint8_t(64 + 17 * low + mipTint), 0, 255});
            }
          }
        }
      }
      uploadTexture(image, texels, copies);
      return image;
    }

    Image routeTexture(const std::filesystem::path& path, uint32_t size, VkFormat format) {
      constexpr uint32_t Layers = 2;
      const auto mips = uint32_t(std::bit_width(size));
      std::vector<VkBufferImageCopy> copies;
      size_t bytes = 0;
      for(uint32_t mip = 0; mip < mips; ++mip) {
        const uint32_t side = size >> mip;
        VkBufferImageCopy copy{};
        copy.bufferOffset = bytes;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, Layers};
        copy.imageExtent = {side, side, 1};
        copies.push_back(copy);
        bytes += size_t(side) * side * 4 * Layers;
      }
      std::vector<uint8_t> texels(bytes);
      std::ifstream stream(path, std::ios::binary);
      if(!stream.read(reinterpret_cast<char*>(texels.data()), std::streamsize(bytes)) ||
         stream.peek() != std::ifstream::traits_type::eof())
        throw std::runtime_error("Invalid route texture size: " + path.string());
      auto image = gpu.image(format, size, size, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             Layers, mips);
      uploadTexture(image, texels, copies);
      return image;
    }

    void uploadTexture(Image image, const std::vector<uint8_t>& texels,
                       const std::vector<VkBufferImageCopy>& copies) {
      const auto staging = gpu.buffer(texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
      std::memcpy(staging.map, texels.data(), texels.size());
      gpu.begin();
      gpu.transition(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      vkCmdCopyBufferToImage(gpu.cmd, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(copies.size()), copies.data());
      gpu.transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
      gpu.submit();
    }

    void bindResources(bool route) {
      constexpr auto ssbo = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      constexpr auto sample = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      std::vector types{ssbo, ssbo, ssbo, ssbo, ssbo, sample, sample,
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sample, sample, ssbo};
      if(route)
        types.insert(types.end(), {ssbo, ssbo, ssbo, sample, sample, sample, sample, sample});
      gpu.descriptors(types);
      const auto linear = gpu.sampler(true);
      const auto nearest = gpu.sampler(false);
      for(uint32_t i = 0; i < types.size(); ++i) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = gpu.set;
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = types[i];
        VkDescriptorBufferInfo buffer{data[i].handle, 0, data[i].size};
        VkDescriptorImageInfo image{i == 6 || i >= 14 ? nearest : linear, colorImage[i].view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if(types[i] == sample && !image.imageView)
          continue;
        if(types[i] == sample)
          write.pImageInfo = &image;
        else
          write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
      }
    }

    void clearColor(Image image, VkClearColorValue value) {
      gpu.transition(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(gpu.cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    }

    void colorReady(Image image) {
      gpu.transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }

    void colorSampleReady(Image image) {
      gpu.transition(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    void depthSampleReady(Image image) {
      gpu.transition(image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    }

    void clearDepth(Image image) {
      gpu.transition(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      const VkClearDepthStencilValue depthClear{1.f, 0};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      vkCmdClearDepthStencilImage(gpu.cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depthClear, 1, &range);
      gpu.transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }

    void commonClear() {
      VkClearColorValue ground{{0.13f, 0.25f, 0.33f, 0.125f}};
      VkClearColorValue packed{};
      packed.uint32[0] = 0x7fff7fffu;
      VkClearColorValue orm{{1.f, 1.f, 0.f, 0.f}};
      clearColor(gbuffer[0], ground);
      clearColor(gbuffer[1], packed);
      clearColor(gbuffer[2], orm);
      clearColor(gbuffer[3], {});
      for(auto image : gbuffer)
        colorReady(image);

      clearDepth(depth);
    }

    void render(std::initializer_list<Image> outputs, const Image* z, VkPipeline pipeline,
                const Submission* submission, bool clearLast = false) {
      if(!submission) {
        renderPhase(outputs, z, pipeline, nullptr, clearLast);
        return;
      }
      if(submission->phases.empty()) {
        const Phase empty{};
        renderPhase(outputs, z, pipeline, &empty, clearLast);
        return;
      }
      constexpr VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      constexpr VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      constexpr VkAccessFlags reads = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
      bool first = true;
      for(const auto& phase : submission->phases) {
        if(!first)
          gpu.memoryBarrier(stages, stages, writes, reads | writes);
        renderPhase(outputs, z, pipeline, &phase, first && clearLast);
        first = false;
      }
    }

    void renderPhase(std::initializer_list<Image> outputs, const Image* z, VkPipeline pipeline,
                     const Phase* phase, bool clearLast) {
      std::array<VkRenderingAttachmentInfo, 5> colors{};
      uint32_t index = 0;
      for(auto image : outputs) {
        auto& color = colors[index++];
        const bool clear = clearLast && index == outputs.size();
        if(clear)
          gpu.transition(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = image.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }
      VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      if(z) {
        depthInfo.imageView = z->view;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }
      VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
      info.renderArea = {{0, 0}, {Width, Height}};
      info.layerCount = 1;
      info.colorAttachmentCount = uint32_t(outputs.size());
      info.pColorAttachments = colors.data();
      info.pDepthAttachment = z ? &depthInfo : nullptr;
      vkCmdBeginRendering(gpu.cmd, &info);
      const VkViewport viewport{0.f, 0.f, float(Width), float(Height), 0.f, 1.f};
      vkCmdSetViewport(gpu.cmd, 0, 1, &viewport);
      vkCmdSetScissor(gpu.cmd, 0, 1, &info.renderArea);
      vkCmdBindPipeline(gpu.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBindDescriptorSets(gpu.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu.pipelineLayout, 0, 1, &gpu.set, 0, nullptr);
      if(phase) {
        const uint32_t limit = std::min(gpu.meshProperties.maxMeshWorkGroupCount[0],
                                        gpu.meshProperties.maxMeshWorkGroupTotalCount);
        for(uint32_t first = phase->first, end = phase->first + phase->count; first < end;) {
          const auto count = std::min(end - first, limit);
          vkCmdPushConstants(gpu.cmd, gpu.pipelineLayout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(first), &first);
          gpu.drawMesh(gpu.cmd, count, 1, 1);
          first += count;
        }
      } else {
        vkCmdDraw(gpu.cmd, 3, 1, 0, 0);
      }
      vkCmdEndRendering(gpu.cmd);
    }

    void capture(Mode mode, bool full) {
      const bool vb = viaVisibility(mode);
      const auto target = readback[mode == Mode::VerifyDirect && full ? 0 : 1];
      const auto copy = [&](Image image, VkImageLayout layout, VkDeviceSize offset) {
        gpu.transition(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {image.aspect, 0, 0, 1};
        region.imageExtent = {Width, Height, 1};
        vkCmdCopyImageToBuffer(gpu.cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.handle, 1, &region);
      };
      copy(vb ? visibility : referenceOwner, vb ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
      if(full) {
        copy(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, Pixels * 8);
        for(uint32_t i = 0; i < gbuffer.size(); ++i)
          copy(gbuffer[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Pixels * (12 + 4 * i));
        if(vb)
          copy(edge, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Pixels * 28);
      }
      gpu.memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    }

    Vulkan gpu;
    std::array<Buffer, 19> data{};
    std::array<Image, 19> colorImage{};
    Buffer recordUpload;
    std::array<Buffer, 2> readback{};
    std::array<Image, 4> gbuffer{};
    Image depth, edge, referenceOwner, visibility, shadowDepth, presentation;
    VkPipeline direct = VK_NULL_HANDLE, raster = VK_NULL_HANDLE, resolve = VK_NULL_HANDLE;
    VkPipeline directVerify = VK_NULL_HANDLE, resolveVerify = VK_NULL_HANDLE;
    VkPipeline pose = VK_NULL_HANDLE, shadow = VK_NULL_HANDLE, paint = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> resolveVariants{};
};

RouteInput readRouteInput(const std::string& line) {
  std::istringstream stream(line);
  std::array<float, 14> value{};
  for(auto& x : value)
    if(!(stream >> x) || !std::isfinite(x))
      throw std::runtime_error("A route row requires 14 finite numbers");
  std::string extra;
  const auto direction = [&](size_t offset) {
    return Point{value[offset], value[offset + 1], value[offset + 2]};
  };
  const RouteInput result{direction(0), direction(3), value[6], value[7], value[8], value[9], direction(10), value[13]};
  const float dx = result.eye[0] - result.target[0], dy = result.eye[1] - result.target[1];
  const float dz = result.eye[2] - result.target[2];
  if((stream >> extra) || dx*dx + dy*dy + dz*dz < 1e-6f || result.pressure < 0.f || result.pressure > 1.f ||
     result.windX*result.windX + result.windZ*result.windZ < 1e-6f || result.sun[1] <= 0.f || result.threshold < 0.f)
    throw std::runtime_error("Invalid route camera, sun, wind, or LOD threshold");
  return result;
}

Point poseVertex(const Point& rest, const Skin& skin, const Pose* pose) {
  const auto transform = [&](const Pose& bone) {
    const auto dot = [&](const std::array<float, 4>& row) {
      return row[0]*rest[0] + row[1]*rest[1] + row[2]*rest[2] + row[3];
    };
    return Point{dot(bone.x), dot(bone.y), dot(bone.z)};
  };
  const auto a = transform(pose[skin.first]);
  const auto b = transform(pose[skin.second]);
  const float w = float(skin.weight) / 255.f;
  return {b[0]*(1.f-w) + a[0]*w, b[1]*(1.f-w) + a[1]*w, b[2]*(1.f-w) + a[2]*w};
}

float distanceTo(const Point& a, const Point& b) {
  const float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
  return std::sqrt(dx*dx + dy*dy + dz*dz);
}

void checkWind(Renderer& renderer, const Asset& asset, const RouteFrame& route, std::ostream& log) {
  constexpr float toleranceCm = 0.002f;
  std::vector<float> envelope(asset.vertices.size());
  std::vector<Point> posed(asset.vertices.size());
  for(uint32_t id = 0; id < envelope.size(); ++id)
    envelope[id] = windTravel(asset, id);
  auto frame = route.view.frame;
  auto uniform = route.uniform;
  std::fill_n(uniform.windFade.begin(), 3, 1.f);
  uniform.previousWindFade = uniform.windFade;
  const std::array times{0.f, 0.31f, 0.67f, 1.11f, 1.55f, 2.09f, 2.73f, 3.47f};
  float maximumTravel = 0.f, maximumRestError = 0.f;
  for(size_t i = 0; i < times.size(); i += 2) {
    uniform.wind = {times[i], i == 0 ? 0.f : 1.f, 0.85f, 0.527f};
    uniform.previousWind = {times[i + 1], 1.f, -0.85f, -0.527f};
    const auto* palette = renderer.readPose(frame, uniform);
    for(size_t state = 0; state < 2; ++state) {
      const bool rest = i == 0 && state == 0;
      const auto* bones = palette + state * asset.bones.size();
      for(size_t id = 0; id < posed.size(); ++id) {
        const auto& vertex = asset.vertices[id];
        const auto point = poseVertex(vertex.position, asset.skin[id], bones);
        const auto distance = distanceTo(point, vertex.position);
        if(!(distance <= envelope[id] + toleranceCm))
          throw std::runtime_error("A posed vertex exceeds its analytical wind envelope");
        posed[id] = point;
        maximumTravel = std::max(maximumTravel, distance);
        if(rest)
          maximumRestError = std::max(maximumRestError, distance);
      }
      for(const auto& cluster : asset.clusters) {
        const auto inGroup = [&](const Point& point, uint32_t id) {
          if(id == NoGroup)
            return true;
          const auto& sphere = asset.groups[id].bounds;
          return distanceTo(point, sphere.center) <= sphere.radius + toleranceCm;
        };
        for(uint32_t m = cluster.firstMeshlet; m < cluster.firstMeshlet + cluster.meshletCount; ++m) {
          const auto& box = asset.meshlets[m];
          for(uint32_t v = box.firstVertex; v < box.firstVertex + box.vertexCount; ++v) {
            const auto& point = posed[asset.vertexIds[v]];
            if(!inGroup(point, cluster.parentGroup) || !inGroup(point, cluster.refinedGroup))
              throw std::runtime_error("A posed vertex exceeds a transition sphere");
            for(size_t axis = 0; axis < 3; ++axis)
              if(point[axis] < box.boxMin[axis] - toleranceCm || point[axis] > box.boxMax[axis] + toleranceCm)
                throw std::runtime_error("A posed vertex exceeds a meshlet box");
          }
        }
      }
    }
  }
  if(maximumRestError > toleranceCm || maximumTravel <= toleranceCm)
    throw std::runtime_error("Wind palette does not reproduce the rest pose and a nonzero gust");
  log << "{\"type\":\"wind_check\",\"poses\":" << times.size() << ",\"vertices\":" << asset.vertices.size()
      << ",\"bones\":" << asset.bones.size() << ",\"palette_bytes\":" << 2 * asset.bones.size() * sizeof(Pose)
      << ",\"max_travel_cm\":" << maximumTravel << ",\"max_rest_error_cm\":" << maximumRestError
      << ",\"meshlet_bounds_failures\":0,\"group_bounds_failures\":0}\n";
}

void recordRoute(const char* shaderDir, const char* assetPath, const char* textureDir, std::string_view appearance,
                 const char* framePath, const std::filesystem::path& output) {
  if(appearance != "cards" && appearance != "opaque")
    throw std::runtime_error("Route appearance must be cards or opaque");
  const bool cards = appearance == "cards";
  const auto asset = load(assetPath);
  if(asset.bones.empty())
    throw std::runtime_error("The route needs a rigged tree asset");
  std::ifstream frameFile(framePath);
  std::vector<RouteInput> inputs;
  for(std::string line; std::getline(frameFile, line);)
    inputs.push_back(readRouteInput(line));
  if(inputs.empty())
    throw std::runtime_error("The route needs at least one frame");
  std::ofstream metadata(output / "route.jsonl");
  if(!metadata)
    throw std::runtime_error("Cannot open route metadata output");
  metadata << std::setprecision(9);

  const Scene scene(&asset, Width, Height);
  Renderer renderer;
  renderer.setup(scene, shaderDir, "route", std::cerr, textureDir, cards);
  checkWind(renderer, asset, scene.prepareRoute(inputs.front(), inputs.front(), cards), std::cerr);
  std::vector<uint8_t> rgb(Pixels * 3);
  std::ios::sync_with_stdio(false);
  for(size_t i = 0; i < inputs.size(); ++i) {
    const auto route = scene.prepareRoute(inputs[i], inputs[i == 0 ? 0 : i - 1], cards);
    renderer.drawRoute(route, rgb);
    std::cout.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
    if(!std::cout)
      throw std::runtime_error("Cannot write route frame stream");
    const auto& view = route.view;
    metadata << "{\"frame\":" << i << ",\"cut_triangles\":" << view.selectedPrimitivesBeforeCull
             << ",\"triangles\":" << view.submittedPrimitives << ",\"cut_clusters\":" << view.selectedClusters
             << ",\"terminal_clusters\":" << view.terminalClusters
             << ",\"terminal\":" << (view.selectedClusters == view.terminalClusters ? "true" : "false")
             << ",\"shadow_triangles\":" << route.shadow.submittedPrimitives << "}\n";
  }
  if(!metadata.flush() || !std::cout.flush())
    throw std::runtime_error("Cannot flush route outputs");
}

VerificationExit verify(Renderer& pilot, const Scene& scene, std::ostream& out) {
  bool rasterPassed = true, materialPassed = true;
  for(const auto name : Scene::testNames()) {
    const auto prepared = scene.prepare(name);
    pilot.upload(prepared);
    pilot.draw(prepared, Mode::VerifyDirect, true, true);
    pilot.draw(prepared, Mode::VerifyVisibility, true, true, VK_NULL_HANDLE, true);
    const auto reference = pilot.imageData(0);
    const auto candidate = pilot.imageData(1);
    const auto result = compare(reference, candidate, Width, Height, pilot.edgeMask());
    const uint32_t center = visibilityAt(candidate.owner, size_t(Height / 2) * Width + Width / 2)[0];
    bool expected = result.candidate.won() > 0;
    expected &= prepared.expectedCenterOwner == 0 || center == prepared.expectedCenterOwner;
    expected &= !prepared.requireEdgePixel || result.subpixel.pixels > 0;
    expected &= !(prepared.requiredSideBits & 1) || result.candidate.back > 0;
    expected &= !(prepared.requiredSideBits & 2) || result.candidate.front > 0;
    if(std::string_view(name).starts_with("near-"))
      expected &= result.nearClip.pixels > 0;
    std::vector<bool> seen(prepared.draws.size() + 1);
    for(size_t pixel = 0; pixel < Pixels; ++pixel) {
      const auto winner = visibilityAt(candidate.owner, pixel)[0];
      if(winner >= seen.size())
        expected = false;
      else
        seen[winner] = true;
      if(winner != 0 && prepared.expectedCenterOwner != 0)
        expected &= winner == prepared.expectedCenterOwner;
    }
    for(const auto owner : prepared.requiredDrawRecords)
      expected &= owner < seen.size() && seen[owner];
    rasterPassed &= expected && result.all.ownerMismatches == 0 && result.all.depthMismatches == 0 &&
                    result.all.flagMismatches == 0 && result.backgroundMismatches == 0;
    materialPassed &= result.passed();
    out << "{\"type\":\"verification\",\"test\":\"" << name << "\",\"center_owner\":" << center
        << ",\"expectations_passed\":" << (expected ? "true" : "false") << ",\"submitted_primitives\":"
        << prepared.submittedPrimitives << ',';
    result.writeJsonFields(out);
    pilot.stats(out);
    out << "}\n";
    out.flush();
    if(result.lostPixels != 0 || result.replacedPixels != 0)
      break;
  }
  pilot.memory(out, "after_verification");
  if(!rasterPassed)
    return VerificationExit::RasterMismatch;
  return materialPassed ? VerificationExit::Passed : VerificationExit::MaterialMismatch;
}

void price(Renderer& pilot, const Scene& scene, Mode mode, double warmupSeconds, uint32_t samples, std::ostream& out) {
  const auto warmup = scene.prepare("dense");
  pilot.upload(warmup);
  const auto warmupStart = Clock::now();
  while(msecs(Clock::now() - warmupStart) < 1000 * warmupSeconds)
    pilot.draw(warmup, mode);
  pilot.memory(out, "after_warmup");

  for(const auto profile : Profiles) {
    const auto prepared = scene.prepare(profile);
    pilot.upload(prepared);
    // Readback and pipeline counters use their own submission so they cannot affect the timing samples.
    if(mode == Mode::Direct)
      pilot.draw(prepared, Mode::VerifyDirect, true);
    pilot.draw(prepared, mode, mode == Mode::Visibility, false, VK_NULL_HANDLE, true);
    const uint64_t won = countCoverage(pilot.owners(), Pixels).won();
    out << "{\"type\":\"profile\",\"profile\":\"" << profile << "\",\"selected_before_cull\":"
        << prepared.selectedBeforeCull << ",\"culled_meshlets\":" << prepared.culledMeshlets
        << ",\"selected_primitives_before_cull\":" << prepared.selectedPrimitivesBeforeCull
        << ",\"draw_records\":" << prepared.draws.size() << ",\"submitted_primitives\":" << prepared.submittedPrimitives
        << ",\"won_pixels\":" << won;
    pilot.stats(out);
    std::array<double, 4> resolveMeans{};
    if(mode == Mode::Visibility) {
      constexpr uint32_t DiagnosticSamples = 12;
      for(uint32_t iteration = 0; iteration < DiagnosticSamples; ++iteration) {
        for(uint32_t variant = 0; variant < resolveMeans.size(); ++variant) {
          const auto i = (variant + iteration) % uint32_t(resolveMeans.size());
          resolveMeans[i] += pilot.draw(prepared, mode, false, false,
                                        i < 3 ? pilot.diagnostics()[i] : VK_NULL_HANDLE).resolve / DiagnosticSamples;
        }
      }
      out << ",\"resolve_ids_ms\":" << resolveMeans[0] << ",\"resolve_attributes_total_ms\":" << resolveMeans[1]
          << ",\"resolve_gradients_total_ms\":" << resolveMeans[2] << ",\"resolve_full_diagnostic_ms\":"
          << resolveMeans[3];
    }
    out << "}\n";

    const auto begin = Clock::now();
    constexpr double TrimMs = 250;
    constexpr double WindowMs = 2500;
    const double sampleInterval = (WindowMs - TrimMs) / samples;
    double nextSample = TrimMs;
    do {
      const auto frameStart = Clock::now();
      const auto frame = scene.prepare(profile);
      const auto prepareEnd = Clock::now();
      const auto gpu = pilot.draw(frame, mode);
      const auto frameEnd = Clock::now();
      const auto elapsed = msecs(frameStart - begin);
      if(elapsed < nextSample)
        continue;
      nextSample = elapsed + sampleInterval;
      const auto mono = std::chrono::duration_cast<std::chrono::nanoseconds>(frameStart.time_since_epoch()).count();
      out << "{\"type\":\"frame\",\"profile\":\"" << profile << "\",\"mono_ns\":" << mono
          << ",\"elapsed_ms\":" << elapsed << ",\"frame_ms\":" << msecs(frameEnd - frameStart)
          << ",\"gpu_ms\":" << gpu.gpu << ",\"prepare_ms\":" << msecs(prepareEnd - frameStart)
          << ",\"clear_ms\":" << gpu.clear << ",\"raster_ms\":" << gpu.raster << ",\"barrier_ms\":" << gpu.barrier
          << ",\"resolve_ms\":" << gpu.resolve << ",\"submitted_primitives\":" << frame.submittedPrimitives
          << ",\"won_pixels\":" << won;
      if(mode == Mode::Visibility)
        out << ",\"attributes_ms\":" << resolveMeans[1] - resolveMeans[0]
            << ",\"gradients_ms\":" << resolveMeans[2] - resolveMeans[1]
            << ",\"material_ms\":" << resolveMeans[3] - resolveMeans[2];
      out << "}\n";
    } while(msecs(Clock::now() - begin) < WindowMs);
    out.flush();
  }
  pilot.memory(out, "after_profiles");
}

}
}

int main(int argc, char** argv) {
  using namespace TreeNanite;
  using namespace TreeNanite::Pilot;
  try {
    if(argc >= 2 && std::string_view(argv[1]) == "route") {
      if(argc != 8)
        throw std::runtime_error("Usage: tree-nanite-pilot route SHADERS ASSET TEXTURES cards|opaque FRAMES OUTPUTDIR");
      recordRoute(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
      return 0;
    }
    if(argc < 5 || argc > 7)
      throw std::runtime_error("Usage: tree-nanite-pilot SHADERS ASSET|- direct|vb|verify OUTPUT [WARMUP_SECONDS] [SAMPLES]");
    const std::string_view mode = argv[3];
    if(mode != "direct" && mode != "vb" && mode != "verify")
      throw std::runtime_error("Invalid pilot mode");
    const auto warmup = argc >= 6 ? std::stod(argv[5]) : 50.0;
    const auto samples = argc >= 7 ? std::stoul(argv[6]) : 256u;
    if(warmup < 0 || warmup > 50 || samples == 0 || samples > 2048)
      throw std::runtime_error("Invalid pilot warmup or sample count");
    std::ofstream out(argv[4]);
    if(!out)
      throw std::runtime_error("Cannot open pilot output");
    out << std::setprecision(9);
    const bool haveAsset = std::string_view(argv[2]) != "-";
    const Asset asset = haveAsset ? load(std::filesystem::path(argv[2])) : Asset{};
    const Scene scene(haveAsset ? &asset : nullptr, Width, Height);
    Renderer pilot;
    pilot.setup(scene, argv[1], mode, out);
    if(mode == "verify")
      return int(verify(pilot, scene, out));
    price(pilot, scene, mode == "direct" ? Mode::Direct : Mode::Visibility, warmup, uint32_t(samples), out);
    return 0;
  } catch(const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
