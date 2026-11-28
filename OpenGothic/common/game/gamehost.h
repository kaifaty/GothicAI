#pragma once

#include <Tempest/SoundEffect>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

#include "camera.h"
#include "gametime.h"
#include "rpgvocabulary.h"

class AiOuputPipe;
class GameScript;
class GameSession;
class Npc;
class SoundFx;
struct HeroDeed;
struct PhysicsNoise;
namespace modkit {
class ContentRegistry;
class IdGenerator;
}

// Services supplied by the game program to the RPG framework. Without a host,
// modes and dialogs are off, sounds are empty and notifications have no listeners.
class GameHost {
  public:
    using ParticleNameCheck = std::function<bool(std::string_view, const std::filesystem::path&,
                                               const std::filesystem::path&, std::vector<std::string>&)>;
    virtual ~GameHost() { if(instance==this) instance = nullptr; }

    static GameHost& inst() {
      static GameHost empty(nullptr);
      return instance!=nullptr ? *instance : empty;
      }

    virtual bool isMarvinEnabled() const { return false; }
    virtual bool isBenchmarkMode() const { return false; }
    virtual bool isGodMode() const { return false; }
    virtual bool doMeleeBladeDraw() const { return false; }
    virtual bool doMeleeBladeLabels() const { return false; }

    virtual bool isNpcInDialog(const Npc&) const { return false; }
    virtual bool isInDialog() const { return false; }
    virtual bool isDialogActive() const { return false; }
    virtual ParticleNameCheck particleNameCheck() { return {}; }
    virtual void openDialogPipe(Npc&, Npc&, AiOuputPipe*& pipe) { pipe = nullptr; }
    AiOuputPipe* openDlgOuput(Npc& player, Npc& npc) {
      AiOuputPipe* pipe = nullptr;
      openDialogPipe(player,npc,pipe);
      return pipe;
      }

    virtual void emitHeroDeed(const HeroDeed&) {}
    virtual void emitCombatHit(Npc&, Npc&, int32_t, std::string_view) {}
    virtual void emitObservedPerception(Npc&, Npc&, Npc*, PercType) {}
    virtual void emitExternalPerception(Npc&, Npc&, Npc*, PercType) {}
    virtual void emitPhysicsNoiseHeard(Npc&, const PhysicsNoise&) {}
    virtual void emitPrint(std::string_view) {}
    virtual bool takePerception(Npc&, Npc&, Npc*, PercType, const HeroDeed*) { return false; }

    virtual SoundFx* loadSoundFx(std::string_view) { return nullptr; }
    virtual SoundFx* loadSoundWavFx(std::string_view) { return nullptr; }
    virtual void emitGlobalSound(const Tempest::Sound&) {}
    virtual bool setMusic(std::string_view, uint8_t) { return false; }

    virtual const std::vector<ItmFlags>& inventoryCategoryOrder() const {
      static const std::vector<ItmFlags> empty;
      return empty;
      }
    virtual void tick(uint64_t) {}
    virtual void updateAnimation(uint64_t) {}

    // A loading session need not be the current session. Keep its identity;
    // Gothic's session and script types cross this boundary only opaquely.
    virtual Camera* sessionCamera(GameSession&) { return nullptr; }
    virtual GameScript* sessionScript(GameSession&) { return nullptr; }
    virtual uint64_t sessionTickCount(GameSession&) const { return 0; }
    virtual gtime sessionTime(GameSession&) const { return gtime(); }
    virtual void setSessionTime(GameSession&, gtime) {}
    virtual void changeWorld(GameSession&, std::string_view, std::string_view) {}
    virtual Tempest::SoundEffect loadSound(GameSession&, const Tempest::Sound&) { return {}; }
    virtual Tempest::SoundEffect loadSound(GameSession&, const SoundFx&, bool& looped) { looped = false; return {}; }
    virtual void updateListenerPos(GameSession&, const Camera::ListenerPos&) {}
    virtual modkit::IdGenerator* sessionModIds(GameSession&) { return nullptr; }
    virtual modkit::ContentRegistry* sessionContentRegistry(GameSession&) { return nullptr; }
    virtual const modkit::ContentRegistry* sessionContentRegistry(const GameSession&) const { return nullptr; }

  protected:
    GameHost() { instance = this; }

  private:
    explicit GameHost(std::nullptr_t) {}
    inline static GameHost* instance = nullptr;
  };
