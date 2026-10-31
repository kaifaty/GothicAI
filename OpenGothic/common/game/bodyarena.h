#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Npc;
class World;

// Bench-only episode state. Setup runs once after a fresh control-save load; no state setters
// are exposed during an episode. All actions then pass through the ordinary body authority.
class BodyArena final {
  public:
    struct Config final {
      std::string scenario = "native_duel";
      std::string weapon = "1h";
      uint32_t seed = 17;
      unsigned side = 0;
      float distanceCm = 300;
      uint64_t timeoutMs = 60000;
      float yawOffsetDeg = 0, opponentYawOffsetDeg = 0, separationYawDeg = 0;
      std::array<float,2> centerOffsetCm = {0,0}; // forward/lateral relative to the save's observer
      std::string geometry = "open";
      std::optional<float> wallClearanceCm; // reset-only wall normal distance; absent retains legacy search-ray120cm
      std::optional<int> wallTangentSign; // near-wall + explicit clearance only; absent retains candidate order
      bool loadFixture = false; // four real actors, raised HP, no combat-quality credit
      bool reactionHold = false; // block/reset_stress common preparation, before any defender decision
      bool opponentOwned = false; // native_duel: the opponent is owned too (self-play), no native ZS_ATTACK
      };
    struct ReactionRequest final {
      uint64_t session=0,world=0,primary=0,opponent=0,primaryGeneration=0,opponentGeneration=0,observation=0,game=0;
      std::string provider="scripted",mode="lockstep";
      };
    struct ReactionGate final {
      ReactionRequest actual;
      bool enabled=false,armed=false,finished=false,owned=false,idle=false,eligible=false;
      bool visible=false,inRange=false,attack=false,prehit=false,started=false,unhurt=false,periodic=false,fresh=false;
      };
    static const char* validateReactionActivation(const ReactionGate& gate,const ReactionRequest& request);
    static bool acceptsReactionScenario(std::string_view scenario);
    static void activateReaction(const ReactionRequest& request);
    static bool reactionHeld();
    static bool reactionArmed();
    static bool reactionConfigured();
    static void configure(const Config& config, uint64_t runnerGeneration);
    static void disable();
    static bool enabled();
    // Training opt-in (OPENGOTHIC_FREEZE_WORLD=1): while an arena is configured, only its actors
    // and the hero tick; the rest of the world stands still and is not heard.
    static bool frozenWorld();
    static bool isActor(const Npc& npc);
    static bool ready();
    static bool started();
    static bool complete();
    static uint32_t seed();
    static void setup(World& world, Npc& player);
    static void sealInitialState();
    static std::vector<Npc*> actors();
    static void start(std::string_view provider);
    static void postStep();
    static void interrupt(std::string_view reason);
    static void setupFailed(std::string_view reason);
    static void noteCombat(const Npc& actor, const Npc* target, std::string_view phase,
                           const std::string& fields);
    // The runner calls this after durably writing the authority's native event. Only record
    // the boundary here; final sampling belongs to postStep after the real world/animation step.
    static void noteControl(std::string_view row);
    static std::string statusJson();
    static void journal(std::string_view kind);
    static void setSink(std::function<void(std::string_view)> sink);

  private:
    static std::string actorJson(Npc& actor, unsigned logicalSide);
    static std::string actorsJson();
    static std::string setupJson();
    static std::string reactionJson();
    static void finish(std::string_view outcome, std::string_view reason);
    static ReactionGate reactionGate();
  };
