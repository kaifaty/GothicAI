#pragma once

#include <cstdint>
#include <array>
#include "game/definitions/definitiontypes.h"
#include <Tempest/Vec>

class Npc;
class GameScript;
class Serialize;

class FightAlgo final {
  public:
    FightAlgo();

    void load(Serialize& fin);
    void save(Serialize& fout);

    enum Action : uint8_t {
      MV_NULL     = 0,
      MV_MOVEG    = 1,
      MV_MOVE     = 2,
      MV_JUMPBACK = 3,
      MV_ATTACK   = 4,
      MV_ATTACKL  = 5,
      MV_ATTACKR  = 6,
      MV_STRAFEL  = 7,
      MV_STRAFER  = 8,
      MV_BLOCK    = 9,
      MV_WAIT     = 10,
      MV_WAITLONG = 11,
      MV_TURN2HIT = 12,
      MV_TURN     = 13,
      MV_TURNG    = 14,
      MV_STRAFE_E = 15,

      MV_MAX      = 6
      };

    // A value-only resolver: it never reads queues, draws RNG, or starts an animation.
    enum class PolicyAction : uint8_t {
      Continue, Approach, TurnToTarget, StrafeLeft, StrafeRight, JumpBack,
      AttackFront, AttackLeft, AttackRight, Block, Wait200, Wait300, Count
      };
    struct ResolverState final {
      bool eligible=false, targetValid=false, sword=false, inWRange=false, inCloseup=false;
      bool focus5=false, focus30=false, lineClear=false, parade=false, running=false;
      bool hasMove=false, hasLeft=false, hasRight=false, hasBack=false;
      bool hasAttack=false, hasAttackLeft=false, hasAttackRight=false, hasBlock=false;
      bool canRotate=false;
      };
    struct Resolution final {
      Action primitive=MV_NULL;
      bool allowed=false;
      const char* reason="unsupported_action";
      };
    static Resolution resolve(PolicyAction action, const ResolverState& state);
    // Native normalization is diagnostic only; the executor retains native consume/skip semantics.
    static Action resolveNative(Action action, const ResolverState& state);
    static Action resolveDownTactics(bool enabled, bool knockdown, bool rising, bool inRange, bool focus);
    static std::array<bool,12> actionMask(const ResolverState& state);

    Action nextFromQueue(Npc &npc, Npc &tg, GameScript& owner);
    void   consumeAction();
    void   onClearTarget();
    void   onTargetKnockdown();
    void   onTakeHit();

    bool   hasInstructions() const;
    Action queuedAction() const { return tr[0]; }
    uint64_t traceChoiceId() const { return traceChoice; }
    uint64_t traceEntrySerial() const { return traceEntry; }
    int traceParentInstruction() const { return traceInstruction; }
    std::array<Action,MV_MAX> queueSnapshot() const;
    bool   fetchInstructions(Npc &npc, Npc &tg, GameScript& owner);
    float  qDistTo(const Npc& npc, const Npc& tg) const;

    float  baseDistance           (const Npc &npc, const Npc &tg, GameScript &owner) const;
    float  prefferedAttackDistance(const Npc &npc, const Npc &tg, GameScript &owner) const;
    float  prefferedGDistance     (const Npc &npc, const Npc &tg, GameScript &owner) const;
    float  attackFinishDistance   (GameScript &owner) const;

    bool   isInAttackRange        (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInFinishRange        (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInCloseupRange       (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInWRange             (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInGRange             (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInFocusAngle         (const Npc &npc, const Npc &tg) const;
    bool   isInFocusAngle         (const Npc &npc, const Npc &tg, float ang) const;
    bool   isInJumpBackAngle      (const Npc &npc, const Npc &tg) const;

  private:
    void   fillQueue(Npc &npc, Npc &tg, GameScript& owner);
    bool   fillQueue(const Npc& npc, GameScript& owner, const Def::FightAi& src);

    static bool   angleTest(const Npc& npc, const Npc& tg, float cosMax);
    static float  weaponRange(GameScript &owner,const Npc &npc);

    Def::FightMove queueId = Def::FightMove::NOP;
    Action              tr   [MV_MAX]={};
    bool                hitFlg=false;
    uint64_t            traceChoice=0; // bench telemetry only, never serialized or used for decisions
    uint64_t            traceEntry=0; // identity of a materialized/consumed queue entry, telemetry only
    int                 traceInstruction=0;
  };
