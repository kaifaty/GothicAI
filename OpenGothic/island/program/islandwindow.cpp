#include "islandwindow.h"

#include "islandhero.h"
#include "islandservices.h"
#include "islandview.h"
#include "islandworld.h"

#include "physics/dynamicworld.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <Tempest/Pixmap>
#include <Tempest/SystemApi>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

using namespace Tempest;

namespace island {

namespace {
constexpr uint64_t StepsPerSecond = 60;
// at most so many steps per frame: a window held back (dragged, hidden) does not run the island in a rush after
constexpr uint64_t MaxStepsPerFrame = 8;

uint64_t stepEnd(uint64_t step) {
  return step*1000/StepsPerSecond;
  }
}

Window::Window(Device& device, const Flags& flags, Services& services, World& world, View& view, Hero& hero)
  : Tempest::Window(Normal), device(device), flags(flags), services(services), world(world), view(view), hero(hero),
    swapchain(SystemApi::isOffscreen() || !flags.capture.empty() ? Swapchain() : Swapchain(device, hwnd())),
    events(flags.events) {
  setWindowTitle("engine-island");
  if(!flags.capture.empty())
    std::filesystem::create_directories(flags.capture);
  if(!flags.capture.empty() || SystemApi::isOffscreen())
    offscreen = device.attachment(TextureFormat::RGBA8, flags.width, flags.height);
  control = flags.demo ? static_cast<ControlSource*>(&demo) : &keys;
  if(!flags.script.empty()) {
    script = std::make_unique<ScriptTrack>(flags.script);
    hero.teleport(script->start(), script->yaw());
    // Establish the contact before the journal starts; track coordinates stay independent of World::start.
    hero.tick({}, 16);
    control = script.get();
    }
  events.watch(view.residency(), hero, time);
  inAir = !hero.onGround();
  events.emit("start", 0, hero, {{"frames", "0"}, {"frame_median_ms", "0"}, {"frame_p95_ms", "0"}, {"exit_code", "0"}});

  services.cam    = &camera;
  services.target = &hero;
  setViewport();
  camera.reset(&hero);
  renderer.onWorldChanged();
  chunk = ChunkId::fromPosition(hero.position().x, hero.position().z);
  // Baseline publications belong to startup; later texture events must prove work during the route.
  view.setHero(hero.bodyMatrix());
  view.updateLandscape(camera, hero.position());
  events.observe(0, hero);
  setFocus(true);
  }

Window::~Window() {
  for(auto& f:fence)
    f.wait();
  device.waitIdle();
  events.finish(time, hero, frames, frameMs, exitCode(), control->finished(time));
  // Exclude the first 60 frame intervals, which wait for shaders and textures.
  std::vector<uint32_t> ms(frameMs.size()>60 ? frameMs.begin()+60 : frameMs.end(), frameMs.end());
  if(!ms.empty()) {
    std::sort(ms.begin(), ms.end());
    const auto median = ms[ms.size()/2];
    const auto p95    = ms[std::min(ms.size()-1, ms.size()*95/100)];
    std::printf("island: %llu frames of %ux%u, %.1f s of island time; frame time median %u ms, p95 %u ms over %zu "
                "frames\n", static_cast<unsigned long long>(frames), frameW, frameH, double(time)*0.001, median, p95,
                ms.size());
    }
  else {
    std::printf("island: %llu frames, %.1f s of island time\n", static_cast<unsigned long long>(frames),
                double(time)*0.001);
    }
  std::fflush(stdout);
  }

Attachment& Window::target() {
  if(!offscreen.isEmpty())
    return offscreen;
  return swapchain[swapchain.currentImage()];
  }

void Window::setViewport() {
  frameW = offscreen.isEmpty() ? swapchain.w() : uint32_t(offscreen.w());
  frameH = offscreen.isEmpty() ? swapchain.h() : uint32_t(offscreen.h());
  camera.setViewport(frameW, frameH);
  }

uint64_t Window::stepLength() const {
  return stepEnd(steps+1)-stepEnd(steps);
  }

void Window::step() {
  const uint64_t dt = stepLength();
  const Control  c  = control->next(time, dt);
  hero.tick(c, dt);
  world.physics().tick(dt);
  services.tick(dt);

  // the camera turns with the hero, as the game's follows the player; the mouse tilts it
  auto spin = camera.spin();
  spin.x += c.pitch;
  spin.y  = hero.yaw();
  camera.setSpin(spin);
  camera.setTarget(hero.cameraTargetBone());
  camera.tick(dt);

  time += dt;
  ++steps;
  report(c);
  }

void Window::report(const Control&) {
  const auto& at = hero.position();
  const auto  id = ChunkId::fromPosition(at.x, at.z);
  if(!(id==chunk)) {
    Log::i("island: ", double(time)*0.001, " s: chunk ", chunk.x, ",", chunk.z, " -> ", id.x, ",", id.z);
    events.emit("chunk", time, hero, {{"from", std::to_string(chunk.x)+","+std::to_string(chunk.z)},
                                      {"to", std::to_string(id.x)+","+std::to_string(id.z)}});
    chunk = id;
    }
  if(hero.jumping() && !inAir)
    Log::i("island: ", double(time)*0.001, " s: jump at ", at.x, " ", at.y, " ", at.z);
  if(inAir && hero.onGround())
    Log::i("island: ", double(time)*0.001, " s: on the ground at ", at.x, " ", at.y, " ", at.z);
  if(hero.jumping() && !wasJumping)
    events.emit("jump", time, hero);
  if(inAir && hero.onGround())
    events.emit("land", time, hero);
  if(time>=nextPosition) {
    events.emit("position", time, hero);
    nextPosition = (time/100+1)*100;
    }
  wasJumping = hero.jumping();
  inAir = !hero.onGround();
  }

void Window::save(const Pixmap& frame) {
  Pixmap pixels = frame.format()==TextureFormat::RGBA8 ? frame : Pixmap(frame, TextureFormat::RGBA8);
  if(!flags.capture.empty()) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame-%05llu.png", static_cast<unsigned long long>(frames));
    const auto file = flags.capture/name;
    pixels.save(file.string().c_str());
    if(!std::filesystem::is_regular_file(file))
      throw std::runtime_error("unable to write "+file.string());
    }
  while(script && nextShot<script->shots().size() && script->shots()[nextShot].time<=time) {
    const auto& shot = script->shots()[nextShot++];
    const auto directory = !flags.capture.empty() ? flags.capture.parent_path() : flags.events.parent_path();
    const auto path = directory/(shot.name+".png");
    pixels.save(path.string().c_str());
    if(!std::filesystem::is_regular_file(path))
      throw std::runtime_error("unable to write "+path.string());
    events.emit("shot", time, hero, {{"name", shot.name}, {"file", path.string()}});
    }
  }

void Window::render() {
  if(quitting)
    return;
  try {
    auto& sync = fence[cmdId];
    sync.wait();

    const uint64_t now = Application::tickCount();
    if(frames==0) {
      started = lastFrame = now;
      }
    else {
      frameMs.push_back(uint32_t(now-lastFrame));
      lastFrame = now;
      }

    if(!flags.capture.empty()) {
      // Capture at 30 Hz, or every second frame at 15 Hz; simulation stays at 60 Hz.
      for(uint32_t i=0; i<2*flags.captureEvery && !control->finished(time); ++i)
        step();
      }
    else {
      const uint64_t due = now-started;
      uint64_t n = 0;
      while(time+stepLength()<=due && n<MaxStepsPerFrame) {
        step();
        ++n;
        }
      if(n==MaxStepsPerFrame && time+stepLength()<=due)
        started = now-time; // fell behind: the island waits rather than hurries
      }

    view.setHero(hero.bodyMatrix());
    view.updateLandscape(camera, hero.position());
    events.observe(time, hero);
    Resources::resetRecycled(cmdId);

    CommandBuffer& cmd = commands[cmdId];
    {
    auto enc = cmd.startEncoding(device);
    renderer.draw(target(), enc, cmdId, view.view(), camera);
    }
    sync = device.submit(cmd);
    const bool shotDue = script && nextShot<script->shots().size() && script->shots()[nextShot].time<=time;
    if(!flags.capture.empty() || shotDue) {
      sync.wait();
      save(device.readPixels(target()));
      }
    if(offscreen.isEmpty())
      device.present(swapchain);
    cmdId = uint8_t((cmdId+1u)%Resources::MaxFramesInFlight);
    ++frames;

    if((flags.frames>0 && frames>=flags.frames) || control->finished(time))
      quit();
    }
  catch(const SwapchainSuboptimal&) {
    Log::e("island: the swapchain is outdated - reset");
    device.waitIdle();
    swapchain.reset();
    setViewport();
    }
  catch(const std::exception& e) {
    Log::e("island: ", e.what());
    failed = true;
    quit();
    }
  }

void Window::resizeEvent(SizeEvent&) {
  for(auto& f:fence)
    f.wait();
  if(offscreen.isEmpty())
    swapchain.reset();
  setViewport();
  }

void Window::keyDownEvent(KeyEvent& event) {
  if(event.key==KeyEvent::K_ESCAPE) {
    quit();
    return;
    }
  keys.key(event.key, true);
  }

void Window::keyUpEvent(KeyEvent& event) {
  keys.key(event.key, false);
  }

// The mouse looks while its left button is held, as in Gothic's window: the cursor stays the user's.
void Window::mouseDownEvent(MouseEvent& event) {
  if(flags.demo || script || event.button!=Event::ButtonLeft)
    return;
  looking = true;
  lastMouse = event.pos();
  }

void Window::mouseUpEvent(MouseEvent& event) {
  if(event.button==Event::ButtonLeft)
    looking = false;
  }

void Window::mouseDragEvent(MouseEvent& event) {
  if(!looking)
    return;
  keys.mouse(event.pos().x-lastMouse.x, event.pos().y-lastMouse.y);
  lastMouse = event.pos();
  }

void Window::focusEvent(FocusEvent& event) {
  if(!event.in) {
    looking = false;
    keys.clear();
    }
  }

void Window::closeEvent(CloseEvent& event) {
  quit();
  Tempest::Window::closeEvent(event);
  }

void Window::quit() {
  if(quitting)
    return;
  quitting = true;
  SystemApi::exit();
  }

}
