#pragma once

// The island's window and its frame (engine layers, step 8.6): the loop of common/mainwindow.cpp without the game.
// Each frame waits for its slot's fence, steps the island, updates the chunks and the textures, recycles the slot's
// resources and draws through Renderer::draw into the swapchain (or, with --capture, into an image read back and
// saved as PNG). Frames in flight: Resources::MaxFramesInFlight.
//
// The island's time goes in fixed steps of 1/60 s (16 or 17 ms): the same track gives the same walk on any machine.
// In the window the steps follow the clock; with --capture every frame is two steps, 30 frames a second.

#include "islandcontrol.h"
#include "islandflags.h"
#include "islandscript.h"
#include "islandevents.h"

#include "camera.h"
#include "resources.h"
#include "worldchunkmanifest.h"
#include "graphics/renderer.h"

#include <Tempest/CommandBuffer>
#include <Tempest/Device>
#include <Tempest/Fence>
#include <Tempest/Swapchain>
#include <Tempest/Window>

#include <vector>

namespace island {

class Hero;
class Services;
class View;
class World;

class Window final : public Tempest::Window {
  public:
    Window(Tempest::Device& device, const Flags& flags, Services& services, World& world, View& view, Hero& hero);
    ~Window() override;

    int exitCode() const { return failed ? 1 : 0; }

  private:
    void render() override;
    void resizeEvent  (Tempest::SizeEvent&  event) override;
    void keyDownEvent (Tempest::KeyEvent&   event) override;
    void keyUpEvent   (Tempest::KeyEvent&   event) override;
    void mouseDownEvent(Tempest::MouseEvent& event) override;
    void mouseUpEvent  (Tempest::MouseEvent& event) override;
    void mouseDragEvent(Tempest::MouseEvent& event) override;
    void focusEvent   (Tempest::FocusEvent& event) override;
    void closeEvent   (Tempest::CloseEvent& event) override;

    void     step();               // one fixed step of the island
    uint64_t stepLength() const;   // milliseconds of the next step
    void     report(const Control& control);
    void     save(const Tempest::Pixmap& pixels);
    void     quit();
    Tempest::Attachment& target();
    void     setViewport();

    Tempest::Device&     device;
    const Flags&         flags;
    Services&            services;
    World&               world;
    View&                view;
    Hero&                hero;

    Tempest::Swapchain   swapchain;
    Tempest::Attachment  offscreen; // --capture, or a window that does not present
    Tempest::Fence       fence   [Resources::MaxFramesInFlight];
    Tempest::CommandBuffer commands[Resources::MaxFramesInFlight];
    uint8_t              cmdId = 0;

    Camera               camera;
    Renderer             renderer;
    Keys                 keys;
    DemoTrack            demo;
    std::unique_ptr<ScriptTrack> script;
    Events               events;
    size_t               nextShot = 0;
    uint64_t             nextPosition = 0;
    bool                 wasJumping = false;
    ControlSource*       control = nullptr;

    uint64_t             steps    = 0;  // fixed steps so far
    uint64_t             time     = 0;  // island milliseconds: the sum of the steps
    uint64_t             frames   = 0;
    uint32_t             frameW   = 0, frameH = 0;
    uint64_t             started  = 0;  // the clock at the first frame
    uint64_t             lastFrame = 0;
    std::vector<uint32_t> frameMs;      // the time between frames, for the report at the end
    ChunkId              chunk;
    bool                 inAir    = false;
    bool                 looking  = false; // the left button is held: the mouse looks
    Tempest::Point       lastMouse;
    bool                 quitting = false;
    bool                 failed   = false;
  };

}
