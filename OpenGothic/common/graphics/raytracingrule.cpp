#include "raytracingrule.h"

using namespace RayTracingRule;

static const char* backendName(Backend b) {
  switch(b) {
    case Backend::Vulkan:    return "Vulkan";
    case Backend::DirectX12: return "DirectX 12";
    case Backend::Metal:     return "Metal";
    }
  return "?";
  }

static std::string named(const Device& dev, std::string_view why) {
  std::string r(why);
  r += " (";
  r += dev.name.empty() ? std::string_view("unnamed GPU") : dev.name;
  r += ")";
  return r;
  }

Decision RayTracingRule::byDevice(const Device& dev) {
  if(dev.backend!=Backend::Vulkan)
    return {false, named(dev, std::string("rule: ")+backendName(dev.backend)+", not Vulkan")};
  if(dev.name.find("Apple")!=std::string_view::npos)
    return {false, named(dev, "rule: an Apple GPU")};
  if(dev.type!=Tempest::DeviceType::Discrete)
    return {false, named(dev, "rule: not a discrete GPU")};
  if(dev.name.find("RTX")==std::string_view::npos)
    return {false, named(dev, "rule: no \"RTX\" in the name")};
  if(!dev.rayQuery)
    return {false, named(dev, "rule: the GPU has no ray query")};
  return {true, named(dev, "rule: a discrete RTX on Vulkan with ray query")};
  }

Decision RayTracingRule::choose(int flag, int env, int ini, const Device& dev) {
  const char* source = nullptr;
  int         value  = -1;
  if(flag>=0) {
    source = "-rt";
    value  = flag;
    }
  else if(env>=0) {
    source = "OPENGOTHIC_RT";
    value  = env;
    }
  else if(ini>=0) {
    source = "[INTERNAL] rayTracing";
    value  = ini;
    }
  if(source==nullptr)
    return byDevice(dev);

  std::string why = std::string(source)+"="+(value!=0 ? "1" : "0");
  if(value!=0 && !dev.rayQuery)
    return {false, named(dev, why+", but the GPU has no ray query")};
  return {value!=0, named(dev, why)};
  }
