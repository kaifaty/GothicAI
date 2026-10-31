#include "fightalgo.h"

#include <Tempest/Log>
#include <algorithm>

#include "game/definitions/fightaidefinitions.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "gothic.h"
#include "serialize.h"
#include "testbench.h"

// According to Gothic1 scripts:
// W  - Weapon Range (FIGHT_RANGE_FIST * 3)
// G  - Walking range (3 * W). Buffer for ranged fighters in which they should switch to a melee weapon.
// FK - Long-range combat range (30m)

FightAlgo::FightAlgo() {
  }

FightAlgo::Action FightAlgo::resolveDownTactics(bool enabled, bool knockdown, bool rising, bool inRange, bool focus) {
  if(!enabled || !knockdown)
    return MV_NULL;
  if(!inRange)
    return MV_MOVE;
  if(!focus)
    return MV_TURN2HIT;
  return rising ? MV_WAIT : MV_ATTACK;
  }

void FightAlgo::load(Serialize &fin) {
  fin.read(reinterpret_cast<int32_t&>(queueId));
  for(int i=0;i<MV_MAX;++i)
    fin.read(reinterpret_cast<uint8_t&>(tr[i]));
  fin.read(hitFlg);
  }

void FightAlgo::save(Serialize &fout) {
  fout.write(int32_t(queueId));
  for(int i=0;i<MV_MAX;++i)
    fout.write(uint8_t(tr[i]));
  fout.write(hitFlg);
  }

void FightAlgo::fillQueue(Npc &npc, Npc &tg, GameScript& owner) {
  auto& ai = Gothic::fai()[size_t(npc.handle().fight_tactic)];
  auto  ws = npc.weaponState();

  if(hitFlg) {
    hitFlg = false;
    if(fillQueue(npc,owner,ai.my_w_strafe))
      return;
    }

  const bool focus = isInFocusAngle(npc,tg);

  if(tg.isPrehit() && isInWRange(tg,npc,owner) && isInFocusAngle(tg,npc) && focus){
    if(tg.bodyStateMasked()==BS_RUN)
      if(fillQueue(npc,owner,ai.enemy_stormprehit))
        return;
    if(fillQueue(npc,owner,ai.enemy_prehit))
      return;
    }

  if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
    if(isInWRange(npc,tg,owner)) {
      if(focus && npc.bodyStateMasked()==BS_RUN)
        if(fillQueue(npc,owner,ai.my_w_runto))
          return;
      if(focus && npc.bodyStateMasked()!=BS_RUN)
        if(fillQueue(npc,owner,ai.my_w_focus))
          return;
      if(fillQueue(npc,owner,ai.my_w_nofocus))
        return;
      }

    if(isInGRange(npc,tg,owner)) {
      if(focus && npc.bodyStateMasked()==BS_RUN)
        if(fillQueue(npc,owner,ai.my_g_runto))
          return;
      if(focus && npc.bodyStateMasked()!=BS_RUN)
        if(fillQueue(npc,owner,ai.my_g_focus))
          return;
      }
    }

  if(ws==WeaponState::Mage) {
    if(isInWRange(npc,tg,owner))
      if(fillQueue(npc,owner,ai.my_fk_focus_mag))
        return;
    if(fillQueue(npc,owner,ai.my_fk_nofocus_mag))
      return;
    }

  if(isInWRange(npc,tg,owner) && focus)
    if(fillQueue(npc,owner,ai.my_fk_focus_far))
      return;
  if(fillQueue(npc,owner,ai.my_fk_nofocus_far))
    return;

  fillQueue(npc,owner,ai.my_w_nofocus);
  }

bool FightAlgo::fillQueue(const Npc& npc, GameScript& owner, const Def::FightAi& src) {
  uint32_t sz=0;
  for(size_t i=0; i<Def::FightAi::move_count; ++i){
    if(src.move[i]==Def::FightMove::NOP)
      break;
    sz++;
    }
  if(sz==0)
    return false;
  queueId = Def::FightMove(src.move[owner.rand(sz,&npc)]);
  return queueId!=Def::FightMove::NOP;
  }

FightAlgo::Action FightAlgo::nextFromQueue(Npc& npc, Npc& tg, GameScript& owner) {
  using Def::FightMove;

  if(tr[0]==MV_NULL) {
    const auto selected = queueId;
    switch(queueId) {
      case FightMove::TURN:
        tr[0] = MV_TURN;
        break;
      case FightMove::RUN:{
        tr[0] = MV_MOVE;
        break;
        }
      case FightMove::RUN_BACK:{
        tr[0] = MV_NULL; //TODO
        break;
        }
      case FightMove::JUMP_BACK:{
        tr[0] = MV_JUMPBACK;
        break;
        }
      case FightMove::STRAFE:{
        tr[0] = owner.rand(2,&npc) ? MV_STRAFEL : MV_STRAFER;
        tr[1] = FightAlgo::MV_STRAFE_E;
        break;
        }
      case FightMove::ATTACK:{
        tr[0] = MV_ATTACK;
        break;
        }
      case FightMove::ATTACK_SIDE:{
        tr[0] = MV_ATTACKL;
        tr[1] = MV_ATTACKR;
        break;
        }
      case FightMove::ATTACK_FRONT:{
        tr[0] = owner.rand(2,&npc) ? MV_ATTACKL : MV_ATTACKR;
        tr[1] = MV_ATTACK;
        break;
        }
      case FightMove::ATTACK_TRIPLE:{
        if(owner.rand(2,&npc)){
          tr[0] = MV_ATTACK;
          tr[1] = MV_ATTACKR;
          tr[2] = MV_ATTACKL;
          } else {
          tr[0] = MV_ATTACKL;
          tr[1] = MV_ATTACKR;
          tr[2] = MV_ATTACK;
          }
        break;
        }
      case FightMove::ATTACK_WHIRL:{
        tr[0] = MV_ATTACKL;
        tr[1] = MV_ATTACKR;
        tr[2] = MV_ATTACKL;
        tr[3] = MV_ATTACKR;
        break;
        }
      case FightMove::ATTACK_MASTER:{
        tr[0] = MV_ATTACKL;
        tr[1] = MV_ATTACKR;
        tr[2] = MV_ATTACK;
        tr[3] = MV_ATTACK;
        tr[4] = MV_ATTACK;
        tr[5] = MV_ATTACK;
        break;
        }
      case FightMove::TURN_TO_HIT:{
        tr[0] = MV_TURN2HIT;
        break;
        }
      case FightMove::PARRY:{
        tr[0] = MV_BLOCK;
        break;
        }
      case FightMove::STAND_UP:{
        break;
        }
      case FightMove::WAIT:
      case FightMove::WAIT_EXT:{
        tr[0] = MV_WAIT;
        break;
        }
      case FightMove::WAIT_LONGER:{
        tr[0] = MV_WAITLONG;
        break;
        }
      default: {
        static std::set<FightMove> inst;
        if(inst.find(queueId)==inst.end()) {
          Tempest::Log::d("unrecognized FAI instruction: ", int(queueId));
          inst.insert(queueId);
          }
        }
      }
    traceInstruction=int(selected); ++traceEntry;
    queueId = FightMove::NOP;
    if(TestBench::bodyTracing()) {
      std::string moves = "[";
      for(size_t i=0; i<MV_MAX; ++i) {
        if(i!=0)
          moves += ",";
        moves += std::to_string(int(tr[i]));
        }
      moves += "]";
      TestBench::bodyEvent(npc,&tg,"native_choice",npc.world().tickCount(),
                          "\"choice_id\":" + std::to_string(++traceChoice) +
                          ",\"fai_instruction\":" + std::to_string(int(selected)) +
                          ",\"primitive\":" + std::to_string(int(tr[0])) + ",\"queue\":" + moves);
      }
    }
  return tr[0];
  }

bool FightAlgo::hasInstructions() const {
  return tr[0]!=MV_NULL;
  }

bool FightAlgo::fetchInstructions(Npc &npc, Npc &tg, GameScript& owner) {
  fillQueue(npc,tg,owner);
  if(queueId==Def::FightMove::NOP)
    return false;
  nextFromQueue(npc,tg,owner);
  return true;
  }

void FightAlgo::consumeAction() {
  ++traceEntry;
  for(size_t i=1;i<MV_MAX;++i)
    tr[i-1]=tr[i];
  tr[MV_MAX-1]=MV_NULL;
  }

void FightAlgo::onClearTarget() {
  ++traceEntry;
  queueId = Def::FightMove::NOP;
  tr[0]   = MV_NULL;
  }

void FightAlgo::onTargetKnockdown() {
  onClearTarget();
  for(auto& action:tr)
    action = MV_NULL;
  }

void FightAlgo::onTakeHit() {
  hitFlg = true;
  for(auto& i:tr)
    i = MV_NULL;
  queueId = Def::FightMove::NOP;
  }

float FightAlgo::qDistTo(const Npc& npc, const Npc& tg) const {
  return npc.fightDistanceTo(tg).quadLength();
  }

float FightAlgo::baseDistance(const Npc& npc, const Npc& tg,  GameScript &owner) const {
  auto&  gv      = owner.guildVal();
  float  baseTg  = float(gv.fight_range_base[tg .guild()]);
  float  baseNpc = float(gv.fight_range_base[npc.guild()]);
  return baseTg + baseNpc;
  }

float FightAlgo::prefferedAttackDistance(const Npc& npc, const Npc& tg,  GameScript &owner) const {
  auto&  gv      = owner.guildVal();
  float  baseTg  = float(gv.fight_range_base[tg.guild()]);
  float  baseNpc = float(gv.fight_range_base[npc.guild()]);
  return baseTg + baseNpc + weaponRange(owner,npc);
  }

float FightAlgo::prefferedGDistance(const Npc& npc, const Npc& tg, GameScript &owner) const {
  auto   gl      = npc.guild();
  auto&  gv      = owner.guildVal();
  float  baseTg  = float(gv.fight_range_base[tg.guild()]);
  float  baseNpc = float(gv.fight_range_base[npc.guild()]);
  return float(baseTg + baseNpc + float(gv.fight_range_g[gl])) + weaponRange(owner,npc);
  }

float FightAlgo::attackFinishDistance(GameScript &owner) const {
  float NPC_ATTACK_FINISH_DISTANCE = 180;
  if(auto var = owner.findSymbol("NPC_ATTACK_FINISH_DISTANCE")) {
    if(var->type()==zenkit::DaedalusDataType::INT)
      NPC_ATTACK_FINISH_DISTANCE = float(var->get_int());
    else if(var->type()==zenkit::DaedalusDataType::FLOAT)
      NPC_ATTACK_FINISH_DISTANCE = var->get_float();
    }
  return NPC_ATTACK_FINISH_DISTANCE;
  }

bool FightAlgo::isInAttackRange(const Npc &npc, const Npc &tg, GameScript &owner) const {
  auto dist = qDistTo(npc, tg);
  auto pd   = prefferedAttackDistance(npc,tg,owner);
  return (dist<=pd*pd);
  }

bool FightAlgo::isInFinishRange(const Npc& npc, const Npc& tg, GameScript& owner) const {
  auto dist = qDistTo(npc, tg);
  auto pd   = attackFinishDistance(owner);
  return (dist<=pd*pd);
  }

bool FightAlgo::isInCloseupRange(const Npc& npc, const Npc& tg, GameScript& owner) const {
  // script: automatic movement of the figure (if too close) to 0.75*FightRange
  auto dist = qDistTo(npc, tg);
  auto pd   = baseDistance(npc,tg,owner) * 0.75f;
  return (dist<=pd*pd);
  }

bool FightAlgo::isInWRange(const Npc& npc, const Npc& tg, GameScript& owner) const {
  // tested in vanilla on Bloofly's:
  //  60 weapon range (Spiked club) is not enough to hit
  //  70 weapon range (Rusty Sword) is good to hit
  static float padding = 0; // padding
  auto dist = qDistTo(npc, tg);
  auto pd   = prefferedAttackDistance(npc,tg,owner) + padding;
  return (dist<=pd*pd);
  }

bool FightAlgo::isInGRange(const Npc &npc, const Npc &tg, GameScript &owner) const {
  auto dist = qDistTo(npc, tg);
  auto pd   = prefferedGDistance(npc,tg,owner);
  return (dist<=pd*pd);
  }

bool FightAlgo::isInFocusAngle(const Npc &npc, const Npc &tg) const {
  static const float maxAngle = std::cos(float(30.0*M_PI/180.0));
  return angleTest(npc, tg, maxAngle);
  }

bool FightAlgo::isInFocusAngle(const Npc& npc, const Npc& tg, float ang) const {
  const float maxAngle = std::cos(float(ang*M_PI/180.0));
  return angleTest(npc, tg, maxAngle);
  }

bool FightAlgo::isInJumpBackAngle(const Npc& npc, const Npc& tg) const {
  static const float maxAngle = std::cos(float(90.0*M_PI/180.0));
  return angleTest(npc, tg, maxAngle);
  }

bool FightAlgo::angleTest(const Npc& npc, const Npc& tg, float cosMax) {
  // must be consistent with collisions
  const auto  dpos = tg.collosionCenter() - npc.collosionCenter();
  const float plAng = npc.rotationRad();

  const float da = plAng-std::atan2(dpos.z,dpos.x);
  const float c  = std::cos(da);

  if(c<cosMax)
    return false;
  return true;
  }

float FightAlgo::weaponRange(GameScript &owner, const Npc &npc) {
  /*
  NOTE: comments from G2 scripts:
    Bip01 bis BBox
    FAI_W = BASE + ItemRange (or Fist)
    FAI_G = BASE + ItemRange (or Fist) + G
  */
  auto  gl  = npc.guild();
  auto& gv  = owner.guildVal();
  auto  w   = npc.inventory().activeWeapon();
  int   add = w ? w->swordLength() : 0;
  auto  bR  = Gothic::inst().version().game==2 ? ReferenceBowRangeG2 : ReferenceBowRangeG1;

  switch(npc.weaponState()) {
    case WeaponState::W1H:
      return float(gv.fight_range_1ha[gl] + add);
    case WeaponState::W2H:
      return float(gv.fight_range_2ha[gl] + add);
    case WeaponState::NoWeapon:
    case WeaponState::Fist:
      return float(gv.fight_range_fist[gl]);
    case WeaponState::Bow:
    case WeaponState::CBow:
      return float(bR);
    case WeaponState::Mage:
      return float(MaxMagRange);
    }
  return 0;
  }


FightAlgo::Resolution FightAlgo::resolve(PolicyAction action, const ResolverState& s) {
  if(action==PolicyAction::Continue)
    return {MV_NULL,true,"continue"};
  if(!s.targetValid) return {MV_NULL,false,"target_invalid"};
  if(!s.sword) return {MV_NULL,false,"weapon_mode"};
  if(!s.eligible) return {MV_NULL,false,"locked"};
  Action primitive=MV_NULL;
  bool allowed=false;
  switch(action) {
    case PolicyAction::Approach: primitive=MV_MOVE; allowed=s.hasMove && !s.inWRange; break;
    case PolicyAction::TurnToTarget: primitive=MV_TURN2HIT; allowed=s.canRotate && !s.focus5; break;
    case PolicyAction::StrafeLeft: primitive=MV_STRAFEL; allowed=s.hasLeft; break;
    case PolicyAction::StrafeRight: primitive=MV_STRAFER; allowed=s.hasRight; break;
    case PolicyAction::JumpBack: primitive=MV_JUMPBACK; allowed=s.hasBack && s.focus30 && !s.parade; break;
    case PolicyAction::AttackFront: primitive=MV_ATTACK; allowed=s.hasAttack && s.focus5 && s.inWRange && s.lineClear; break;
    case PolicyAction::AttackLeft: primitive=MV_ATTACKL; allowed=s.hasAttackLeft && s.focus5 && s.inWRange && s.lineClear; break;
    case PolicyAction::AttackRight: primitive=MV_ATTACKR; allowed=s.hasAttackRight && s.focus5 && s.inWRange && s.lineClear; break;
    case PolicyAction::Block: primitive=MV_BLOCK; allowed=s.hasBlock && s.focus30 && !s.parade; break;
    // Normalize the native wait promotion rather than claiming a plain wait at close range.
    case PolicyAction::Wait200:
      primitive=resolveNative(MV_WAIT,s);
      allowed=primitive==MV_JUMPBACK ? s.hasBack && !s.parade : true;
      break;
    case PolicyAction::Wait300: primitive=MV_WAITLONG; allowed=true; break;
    default: break;
    }
  return {primitive,allowed,allowed ? "allowed" : "masked"};
  }

FightAlgo::Action FightAlgo::resolveNative(Action action, const ResolverState& s) {
  if((action==MV_WAIT || action==MV_TURN) && s.inCloseup && s.focus30)
    return MV_JUMPBACK;
  if((action==MV_ATTACK || action==MV_ATTACKL || action==MV_ATTACKR) && !s.focus5)
    return MV_TURN2HIT;
  return action;
  }

std::array<bool,12> FightAlgo::actionMask(const ResolverState& state) {
  std::array<bool,12> result{};
  for(size_t i=0;i<result.size();++i)
    result[i]=resolve(PolicyAction(i),state).allowed;
  return result;
  }

std::array<FightAlgo::Action,FightAlgo::MV_MAX> FightAlgo::queueSnapshot() const {
  std::array<Action,MV_MAX> result{};
  std::copy(std::begin(tr),std::end(tr),result.begin());
  return result;
  }
