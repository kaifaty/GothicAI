#include "gothic.h"

#include "game/definitions/musicdefinitions.h"
#include "gamemusic.h"

void Gothic::emitHeroDeed(const HeroDeed& deed) {
  onHeroDeed(deed);
  }

void Gothic::emitCombatHit(Npc& npc, Npc& other, int32_t lost, std::string_view kind) {
  onCombatHit(npc,other,lost,kind);
  }

void Gothic::emitObservedPerception(Npc& npc, Npc& player, Npc* victim, PercType type) {
  onObservedPerception(npc,player,victim,type);
  }

void Gothic::emitExternalPerception(Npc& npc, Npc& player, Npc* victim, PercType type) {
  onExternalPerception(npc,player,victim,type);
  }

void Gothic::emitPhysicsNoiseHeard(Npc& npc, const PhysicsNoise& noise) {
  onPhysicsNoiseHeard(npc,noise);
  }

void Gothic::emitPrint(std::string_view text) {
  onPrint(text);
  }

bool Gothic::takePerception(Npc& npc, Npc& player, Npc* victim, PercType type, const HeroDeed* deed) {
  return interceptPerception && interceptPerception(npc,player,victim,type,deed);
  }

bool Gothic::setMusic(std::string_view name, uint8_t tags) {
  if(auto* theme = musicDef()[name]) {
    GameMusic::inst().setMusic(*theme,GameMusic::Tags(tags));
    return true;
    }
  return false;
  }

const std::vector<ItmFlags>& Gothic::inventoryCategoryOrder() const {
  return invCatOrder();
  }

Camera* Gothic::sessionCamera(GameSession& session) {
  return &session.camera();
  }

GameScript* Gothic::sessionScript(GameSession& session) {
  return session.script();
  }

uint64_t Gothic::sessionTickCount(GameSession& session) const {
  return session.tickCount();
  }

gtime Gothic::sessionTime(GameSession& session) const {
  return session.time();
  }

void Gothic::setSessionTime(GameSession& session, gtime time) {
  session.setTime(time);
  }

void Gothic::changeWorld(GameSession& session, std::string_view world, std::string_view waypoint) {
  session.changeWorld(world,waypoint);
  }

Tempest::SoundEffect Gothic::loadSound(GameSession& session, const Tempest::Sound& sound) {
  return session.loadSound(sound);
  }

Tempest::SoundEffect Gothic::loadSound(GameSession& session, const SoundFx& sound, bool& looped) {
  return session.loadSound(sound,looped);
  }

void Gothic::updateListenerPos(GameSession& session, const Camera::ListenerPos& position) {
  session.updateListenerPos(position);
  }
