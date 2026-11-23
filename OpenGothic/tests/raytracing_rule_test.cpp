// Who gets ray tracing by default (graphics/raytracingrule.h): the device table of the rule and who decides over it.
// CPU only: no device, no game data.
#include "graphics/raytracingrule.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace RayTracingRule;
using Tempest::DeviceType;

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

void expect(const Device& dev, bool on, const char* what) {
  const auto d = byDevice(dev);
  require(d.on==on, std::string(what)+": expected "+(on ? "on" : "off")+", got "+d.reason);
  require(d.reason.find(std::string(dev.name))!=std::string::npos, std::string(what)+": the reason names the GPU");
  }
}

int main() {
  try {
    const auto vk = Backend::Vulkan;
    const auto discrete = DeviceType::Discrete;
    expect({vk, discrete, "NVIDIA GeForce RTX 3080", true},             true,  "RTX 3080");
    expect({vk, discrete, "NVIDIA RTX A4000", true},                    true,  "RTX A4000");
    expect({vk, discrete, "Quadro RTX 5000", true},                     true,  "Quadro RTX 5000");
    expect({vk, discrete, "NVIDIA TITAN RTX", true},                    true,  "TITAN RTX");
    expect({vk, discrete, "NVIDIA GeForce GTX 1660 Ti", true},          false, "GTX 1660 Ti with ray query");
    expect({vk, discrete, "AMD Radeon RX 6800 XT", true},               false, "RX 6800 XT");
    expect({vk, discrete, "Intel(R) Arc(TM) A770 Graphics", true},      false, "Arc A770");
    expect({vk, DeviceType::Integrated, "NVIDIA GeForce RTX 3080", true}, false, "integrated");
    expect({Backend::Metal, discrete, "Apple M1 Pro", true},            false, "Apple on Metal");
    expect({vk, discrete, "Apple M1 Pro", true},                        false, "Apple on Vulkan");
    expect({Backend::DirectX12, discrete, "NVIDIA GeForce RTX 3080", true}, false, "DX12");
    expect({vk, discrete, "NVIDIA GeForce RTX 3080", false},            false, "no ray query");

    // who decides: -rt, then OPENGOTHIC_RT, then [INTERNAL] rayTracing, then the rule
    const Device rtx = {vk, discrete, "NVIDIA GeForce RTX 3080", true};
    const Device rx  = {vk, discrete, "AMD Radeon RX 6800 XT", true};
    const Device gtx = {vk, discrete, "NVIDIA GeForce GTX 1060 6GB", false};
    require(choose(-1,-1,-1,rtx).on && choose(-1,-1,-1,rtx).reason.rfind("rule:",0)==0, "nothing given: the rule");
    require(!choose(0,1,1,rtx).on && choose(0,1,1,rtx).reason.rfind("-rt=0",0)==0, "-rt 0 beats the rest");
    require(choose(1,-1,0,rx).on, "-rt 1 turns it on outside the rule, as before");
    require(!choose(-1,0,1,rtx).on && choose(-1,0,1,rtx).reason.rfind("OPENGOTHIC_RT=0",0)==0, "env beats the ini");
    require(!choose(-1,-1,0,rtx).on && choose(-1,-1,0,rtx).reason.rfind("[INTERNAL] rayTracing=0",0)==0,
            "the ini beats the rule");
    require(choose(-1,-1,1,rx).on, "the ini turns it on outside the rule");
    require(!choose(1,-1,-1,gtx).on && choose(1,-1,-1,gtx).reason.find("no ray query")!=std::string::npos,
            "-rt 1 without ray query stays off and says why");

    std::cout << "raytracing rule: " << assertions << " assertions passed\n";
    return 0;
    }
  catch(const std::exception& e) {
    std::cerr << "raytracing rule: FAILED: " << e.what() << '\n';
    return 1;
    }
  }
