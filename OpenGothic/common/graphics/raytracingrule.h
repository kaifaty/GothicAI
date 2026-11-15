#pragma once

#include <Tempest/AbstractGraphicsApi>

#include <cstdint>
#include <string>
#include <string_view>

// Who gets ray tracing (the ray scene, and with it the GI probes and the eye's adaptation) when nothing says it
// explicitly: a discrete NVIDIA RTX on Vulkan (the user's rule, 2026-11-14,
// reports/render-features/2026-11-08-rt-revisit/step-default.md). An announced ray query alone is not enough: some
// drivers announce it without RT cores (GTX 1660), and our GI shaders ran only on an RTX 3080 and an M1 Pro.
namespace RayTracingRule {
  enum class Backend : uint8_t {
    Vulkan,
    DirectX12,
    Metal,
    };

  struct Device {
    Backend             backend  = Backend::Vulkan;
    Tempest::DeviceType type     = Tempest::DeviceType::Unknown;
    std::string_view    name;
    bool                rayQuery = false;
    };

  struct Decision {
    bool        on = false;
    std::string reason;
    };

  // The rule itself: pure, no device or settings.
  Decision byDevice(const Device& dev);

  // Who decides, strongest first: -rt 0|1, OPENGOTHIC_RT=0|1, [INTERNAL] rayTracing=0|1 in Gothic.ini, the rule.
  // -1 means "not given". Explicit "on" still needs the GPU's ray query; the reason names the source.
  Decision choose(int flag, int env, int ini, const Device& dev);
  }
