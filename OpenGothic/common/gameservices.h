#pragma once

#include <Tempest/Signal>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "graphics/visualfx.h"
#include "records/definitions.h"

class World;
class WorldView;
class Npc;
class Camera;
class CameraDefinitions;
class DynamicWorld;

class GameServices {
  public:
    virtual ~GameServices();

    static GameServices& inst();

    virtual World* world() = 0;
    virtual WorldView* worldView() const = 0;
    virtual Npc* player() = 0;
    virtual Camera* camera() = 0;
    virtual bool isPause() const = 0;

    virtual int64_t worldTime() const = 0;
    virtual uint64_t worldTickCount() const = 0;
    virtual DynamicWorld* physicsWorld() const = 0;

    virtual const VisualFx* loadVisualFx(std::string_view name) = 0;
    virtual const ParticleFx* loadParticleFx(std::string_view name, bool relaxed=false) = 0;
    virtual const ParticleFx* loadParticleFx(const ParticleFx* base, const VisualFx::Key* key) = 0;
    virtual const Def::SoundEffect& soundEffect(std::string_view name) const = 0;
    virtual const CameraDefinitions& cameraDefinitions() const = 0;

    Tempest::Signal<void()> toggleGi, toggleVsm, togglePathtrace;
    // Runtime dialog voice: sound file name -> WAV bytes; false if the name is not external.
    std::function<bool(std::string_view,std::vector<uint8_t>&)> externalSoundFn;

  protected:
    GameServices();

  private:
    static GameServices* instance;
  };
