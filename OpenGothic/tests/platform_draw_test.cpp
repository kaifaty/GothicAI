// The platform draws a glTF mesh with no Gothic base, no sound and no Gothic object (engine layers, step 8.4:
// reports/engine/2026-11-13-empty-island/README.md). A GLB of two quads goes the engine's own way: meshes.json in
// OPENGOTHIC_CONTENT, the material from glTF, the texture by the material's name (a PNG in OPENGOTHIC_TEXTURES for
// the first, none for the second, which takes the default texture built in code), WorldView::addStaticView and
// Renderer::draw into an image without a window. The frame is read back and judged by regions.
//
//   gothic-platform-draw-test [--frame <file.png>]   # --frame also saves the frame
//
// Every include here is the platform's (scripts/engine_layers.py layer_of) or a third-party library's.

#include "platform_services.h"

#include "camera.h"
#include "resources.h"
#include "graphics/renderer.h"
#include "graphics/worldview.h"

#include <Tempest/CommandBuffer>
#include <Tempest/Device>
#include <Tempest/Fence>
#include <Tempest/Pixmap>
#if defined(__APPLE__)
#include <Tempest/MetalApi>
#else
#include <Tempest/VulkanApi>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

void textFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), std::streamsize(text.size()));
  require(bool(out), "unable to write "+path.string());
  }

// The PNG's colour: green, which neither the sky, the sun's light nor a grey checker can give.
constexpr std::array<uint8_t,3> PngColor = {32, 192, 64};

// The two quads lie on the ground (glTF y=0, faces up), side by side along x, in metres:
// the first material on x in [-1.6,-0.2], the second on [0.2,1.6], both on z in [-0.7,0.7].
// The engine reads them in centimetres with x mirrored (gltfmesh.cpp).
struct Quad {
  float x0, x1;
  };
constexpr Quad Quads[2] = {{-1.6f, -0.2f}, {0.2f, 1.6f}};
constexpr float QuadZ = 0.7f;

void word(std::vector<uint8_t>& out, uint32_t value) {
  for(int i=0; i<4; ++i)
    out.push_back(uint8_t(value >> (8*i)));
  }

void real(std::vector<uint8_t>& out, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  word(out, bits);
  }

// One mesh, two primitives: a quad of material 0 named `first` and a quad of material 1 named `second`.
void glbFile(const std::filesystem::path& path, std::string_view first, std::string_view second) {
  std::vector<uint8_t> bin;
  std::string views, accessors, primitives;
  for(size_t q=0; q<2; ++q) {
    const auto& quad = Quads[q];
    // counter-clockwise seen from above: the front face looks up (+y)
    const float pos[4][3] = {{quad.x0,0,-QuadZ},{quad.x0,0,QuadZ},{quad.x1,0,QuadZ},{quad.x1,0,-QuadZ}};
    const float uv [4][2] = {{0,0},{0,1},{1,1},{1,0}};
    const uint16_t index[6] = {0,1,2, 0,2,3};
    const size_t at[4] = {bin.size(), bin.size()+48, bin.size()+96, bin.size()+128};
    for(auto& p:pos)   for(float v:p) real(bin, v);
    for(size_t i=0; i<4; ++i) { real(bin, 0); real(bin, 1); real(bin, 0); }
    for(auto& t:uv)    for(float v:t) real(bin, v);
    for(uint16_t i:index) { bin.push_back(uint8_t(i)); bin.push_back(uint8_t(i>>8)); }
    const size_t length[4] = {48, 48, 32, 12};
    for(size_t v=0; v<4; ++v) {
      if(!views.empty()) views += ',';
      views += "{\"buffer\":0,\"byteOffset\":"+std::to_string(at[v])+",\"byteLength\":"+std::to_string(length[v])+"}";
      }
    const auto id = [&](size_t v) { return std::to_string(q*4+v); };
    if(!accessors.empty()) accessors += ',';
    accessors += "{\"bufferView\":"+id(0)+",\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
                 "\"min\":["+std::to_string(quad.x0)+",0,"+std::to_string(-QuadZ)+"],"
                 "\"max\":["+std::to_string(quad.x1)+",0,"+std::to_string(QuadZ)+"]},"
                 "{\"bufferView\":"+id(1)+",\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
                 "{\"bufferView\":"+id(2)+",\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
                 "{\"bufferView\":"+id(3)+",\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}";
    if(!primitives.empty()) primitives += ',';
    primitives += "{\"attributes\":{\"POSITION\":"+id(0)+",\"NORMAL\":"+id(1)+",\"TEXCOORD_0\":"+id(2)+"},"
                  "\"indices\":"+id(3)+",\"material\":"+std::to_string(q)+",\"mode\":4}";
    }
  std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                     "\"buffers\":[{\"byteLength\":"+std::to_string(bin.size())+"}],"
                     "\"bufferViews\":["+views+"],\"accessors\":["+accessors+"],"
                     "\"materials\":[{\"name\":\""+std::string(first)+"\",\"alphaMode\":\"OPAQUE\"},"
                                    "{\"name\":\""+std::string(second)+"\",\"alphaMode\":\"OPAQUE\"}],"
                     "\"meshes\":[{\"primitives\":["+primitives+"]}]}";
  while(json.size()%4!=0) json += ' ';
  while(bin.size()%4!=0)  bin.push_back(0);
  std::vector<uint8_t> out;
  word(out, 0x46546c67); word(out, 2); word(out, uint32_t(12+8+json.size()+8+bin.size()));
  word(out, uint32_t(json.size())); word(out, 0x4e4f534a);
  out.insert(out.end(), json.begin(), json.end());
  word(out, uint32_t(bin.size())); word(out, 0x004e4942);
  out.insert(out.end(), bin.begin(), bin.end());
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  require(bool(file), "unable to write "+path.string());
  }

void pngFile(const std::filesystem::path& path) {
  Tempest::Pixmap pm(16, 16, Tempest::TextureFormat::RGBA8);
  auto* p = reinterpret_cast<uint8_t*>(pm.data());
  for(size_t i=0; i<16*16; ++i) {
    p[i*4+0] = PngColor[0];
    p[i*4+1] = PngColor[1];
    p[i*4+2] = PngColor[2];
    p[i*4+3] = 255;
    }
  pm.save(path.string().c_str());
  require(std::filesystem::is_regular_file(path), "unable to write "+path.string());
  }

// A temporary root with the content and textures of the test, and empty folders in place of the installation and
// every base location. Nothing else of the machine is read: GOTHIC_DIR is unset, the base, its cache and store are
// empty folders here.
struct Fixture {
  std::filesystem::path root;

  Fixture() {
    std::string pattern = (std::filesystem::temp_directory_path()/"gothic-platform-draw-XXXXXX").string();
    if(mkdtemp(pattern.data())==nullptr)
      throw std::runtime_error("unable to create a temporary folder");
    root = pattern;
    for(const auto* dir:{"game", "base", "base-cache", "base-store", "content/meshes/platform", "textures"})
      std::filesystem::create_directories(root/dir);
    unsetenv("GOTHIC_DIR");
    unsetenv("OPENGOTHIC_BASE_FINGERPRINT");
    unsetenv("OPENGOTHIC_VIDEOS_FINGERPRINT");
    unsetenv("OPENGOTHIC_CENSUS");
    setenv("OPENGOTHIC_BASE",       (root/"base").c_str(),       1);
    setenv("OPENGOTHIC_BASE_CACHE", (root/"base-cache").c_str(), 1);
    setenv("OPENGOTHIC_BASE_STORE", (root/"base-store").c_str(), 1);
    setenv("OPENGOTHIC_CONTENT",    (root/"content").c_str(),    1);
    setenv("OPENGOTHIC_TEXTURES",   (root/"textures").c_str(),   1);

    glbFile(root/"content/meshes/platform/draw.glb", "PLATFORM_DRAW_PNG", "PLATFORM_DRAW_NO_TEXTURE");
    textFile(root/"content/meshes/meshes.json", R"({"PLATFORM_DRAW":{"glb":"platform/draw.glb"}})");
    pngFile(root/"textures/platform_draw_png.png");
    }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    }
  };

constexpr uint32_t FrameW = 320, FrameH = 240;

struct Rgb {
  float r = 0, g = 0, b = 0;
  };

struct Window {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  std::string name;
  };

// The screen rectangle of a quad by the camera's projection.
Window screenRect(const Camera& camera, const Quad& quad, std::string name) {
  float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
  const auto vp = camera.viewProj();
  for(float gx:{quad.x0, quad.x1})
    for(float gz:{-QuadZ, QuadZ}) {
      Tempest::Vec3 p = {-gx*100.f, 0.f, gz*100.f}; // glTF metres to the engine's centimetres, x mirrored
      vp.project(p);
      require(std::abs(p.x)<1.f && std::abs(p.y)<1.f && p.z>0.f && p.z<1.f, "the quads must lie in the camera's view");
      const float px = (p.x*0.5f+0.5f)*float(FrameW);
      const float py = (p.y*0.5f+0.5f)*float(FrameH);
      xmin = std::min(xmin, px); xmax = std::max(xmax, px);
      ymin = std::min(ymin, py); ymax = std::max(ymax, py);
      }
  return {int(xmin), int(ymin), int(xmax), int(ymax), std::move(name)};
  }

// The inner half of a rectangle: pixels well inside the quad, clear of its edges and of TAA's sub-pixel jitter.
Window inner(const Window& r) {
  const int dx = (r.x1-r.x0)/4, dy = (r.y1-r.y0)/4;
  Window w = {r.x0+dx, r.y0+dy, r.x1-dx, r.y1-dy, r.name};
  require(w.x1-w.x0>=16 && w.y1-w.y0>=16, "the quad must cover at least 16x16 pixels inside: "+w.name);
  return w;
  }

struct Stats {
  Rgb   mean;
  float pngShare     = 0; // pixels where green clearly leads: the PNG's colour
  float neutralShare = 0; // pixels without a hue of their own: grey
  float lumaLow      = 0; // the 10th and 90th percentile of the brightness
  float lumaHigh     = 0;
  };

Stats measure(const Tempest::Pixmap& frame, const Window& w) {
  const auto* p = reinterpret_cast<const uint8_t*>(frame.data());
  Stats s;
  std::vector<float> luma;
  size_t png = 0, neutral = 0;
  for(int y=w.y0; y<w.y1; ++y)
    for(int x=w.x0; x<w.x1; ++x) {
      const auto* px = p + (size_t(y)*frame.w()+size_t(x))*4;
      const float r = px[0], g = px[1], b = px[2];
      s.mean.r += r; s.mean.g += g; s.mean.b += b;
      const float hi = std::max({r, g, b}), lo = std::min({r, g, b});
      if(g>=24.f && g>=1.5f*std::max(r, b))
        ++png;
      if(hi>=24.f && hi-lo<=0.2f*hi+8.f)
        ++neutral;
      luma.push_back(0.2126f*r+0.7152f*g+0.0722f*b);
      }
  const float n = float(luma.size());
  s.mean.r /= n; s.mean.g /= n; s.mean.b /= n;
  s.pngShare     = float(png)/n;
  s.neutralShare = float(neutral)/n;
  std::sort(luma.begin(), luma.end());
  s.lumaLow  = luma[size_t(n*0.1f)];
  s.lumaHigh = luma[size_t(n*0.9f)];
  return s;
  }

float distance(const Rgb& a, const Rgb& b) {
  return std::sqrt((a.r-b.r)*(a.r-b.r)+(a.g-b.g)*(a.g-b.g)+(a.b-b.b)*(a.b-b.b));
  }

std::string describe(const Window& w, const Stats& s) {
  char text[256];
  std::snprintf(text, sizeof(text), "%s [%d..%d]x[%d..%d]: mean %.0f %.0f %.0f, png %.2f, neutral %.2f, luma p10 %.0f p90 %.0f",
                w.name.c_str(), w.x0, w.x1, w.y0, w.y1, double(s.mean.r), double(s.mean.g), double(s.mean.b),
                double(s.pngShare), double(s.neutralShare), double(s.lumaLow), double(s.lumaHigh));
  return text;
  }

void drawCase(Tempest::Device& device, const Fixture& fixture, const std::filesystem::path& frameFile) {
  Resources resources(device, Resources::Audio::Off);
  platform::Services services(fixture.root/"game");
  require(Resources::selectedBaseFingerprint().empty(), "no base is selected");

  {
  WorldView view(std::pair{Tempest::Vec3{-1000, -100, -1000}, Tempest::Vec3{1000, 100, 1000}}, "PLATFORM.ZEN");
  auto mesh = view.addStaticView("PLATFORM_DRAW.3DS");
  mesh.setObjMatrix(Tempest::Matrix4x4::mkIdentity());

  // 3 m above the middle of the quads, looking straight down, the quads' row across the screen
  Camera camera;
  camera.setViewport(FrameW, FrameH);
  camera.setPosition({0, 300, 0});
  camera.setAngles({90, 90});

  services.game.view = &view;
  services.game.cam  = &camera;

  Renderer renderer;
  renderer.onWorldChanged();
  auto frame = device.attachment(Tempest::TextureFormat::RGBA8, FrameW, FrameH);
  // a few frames: the exposure, TAA and SSAO settle on their history
  for(uint32_t i=0; i<8; ++i) {
    const auto fId = uint8_t(i%Resources::MaxFramesInFlight);
    Resources::resetRecycled(fId);
    Resources::updateTextures(nullptr);
    services.game.ticks += 16;
    Tempest::CommandBuffer cmd;
    {
    auto enc = cmd.startEncoding(device);
    renderer.draw(frame, enc, fId, view, camera);
    }
    device.submit(cmd).wait();
    }
  auto pixels = device.readPixels(frame);
  if(pixels.format()!=Tempest::TextureFormat::RGBA8)
    pixels = Tempest::Pixmap(pixels, Tempest::TextureFormat::RGBA8);
  if(!frameFile.empty())
    pixels.save(frameFile.string().c_str());

  // The quads lie side by side on the screen's middle rows, so the windows do not depend on whether NDC y points up
  // or down; the background is the band above them (or below, by the same token).
  const auto firstRect  = screenRect(camera, Quads[0], "PNG material");
  const auto secondRect = screenRect(camera, Quads[1], "material without a texture");
  require(std::max(firstRect.x0, secondRect.x0)>=std::min(firstRect.x1, secondRect.x1),
          "the quads must lie side by side on the screen");
  const auto first  = inner(firstRect);
  const auto second = inner(secondRect);
  const Window ground = {8, 8, int(FrameW)-8, std::min(firstRect.y0, secondRect.y0)-8, "background"};
  require(ground.y1-ground.y0>=16, "the background band above the quads must be at least 16 pixels high");
  const auto a  = measure(pixels, first);
  const auto b  = measure(pixels, second);
  const auto bg = measure(pixels, ground);
  std::cout << describe(first, a) << '\n' << describe(second, b) << '\n' << describe(ground, bg) << '\n';

  // Light and tonemapping move the colour, so the PNG is judged by its hue, not its value: green at least 1.5 times
  // red and blue in 90 % of the pixels, where the PNG has 6 times red and 3 times blue (2.6 times blue in the frame).
  require(a.pngShare>=0.9f, "the first material must show the PNG's green: "+describe(first, a));
  // The default texture is a grey checker: hueless (max-min channel within a fifth of the brightest plus 8 of 255,
  // room for the sun's and the sky's tint) in 90 % of the pixels, and two greys, its cells: 10th and 90th
  // percentiles of the brightness at least 24 of 255 apart (about 70 in the frame, the flat background 6).
  require(b.neutralShare>=0.9f && b.pngShare<=0.05f,
          "the material without a texture must be drawn grey by the default texture: "+describe(second, b));
  require(b.lumaHigh-b.lumaLow>=24.f, "the default texture's checker must show: "+describe(second, b));
  // The background, the sky below the horizon beyond the quads, is neither: 40 of 255 away from both means.
  require(distance(bg.mean, a.mean)>=40.f && distance(bg.mean, b.mean)>=40.f,
          "the background must differ from both materials: "+describe(ground, bg));

  // the same through the scene: two parts, the second with the texture built in code
  require(mesh.nodesCount()==2, "both materials must be drawn");
  const Tempest::Texture2d* textures[2] = {mesh.node(0).material().tex, mesh.node(1).material().tex};
  const auto* fallback = Resources::loadDefaultTexture();
  require(fallback!=nullptr && (textures[0]==fallback)!=(textures[1]==fallback),
          "exactly one material takes the default texture");
  device.waitIdle();
  }
  }
}

int main(int argc, char** argv) {
  try {
    std::filesystem::path frameFile;
    for(int i=1; i<argc; ++i) {
      if(std::string_view(argv[i])=="--frame" && i+1<argc)
        frameFile = std::filesystem::absolute(argv[++i]);
      else
        throw std::runtime_error("usage: gothic-platform-draw-test [--frame <file.png>]");
      }
    Fixture fixture;
    platform::Arguments arguments(fixture.root/"game");
#if defined(__APPLE__)
    Tempest::MetalApi  api;
#else
    Tempest::VulkanApi api;
#endif
    Tempest::Device device(api);
    drawCase(device, fixture, frameFile);
    device.waitIdle();
    std::cout << "{\"test\":\"platform_draw\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "platform draw test failed: " << error.what() << '\n';
    return 1;
    }
  }
