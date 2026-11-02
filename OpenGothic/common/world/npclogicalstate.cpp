#include "npclogicalstate.h"

#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/world.h"
#include "frameprofile.h"
#include "npcdayjournal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace {
NpcLogicAudit::Stats totals;
using Clock=std::chrono::steady_clock;

// All entry points run on the main thread. Nested admissions/events are timed
// once, so wallNs measures total shadow work rather than elapsed sums.
struct AuditTimer final {
  bool active=NpcLogicAudit::enabled() || NpcDayJournal::running();
  bool outer=active && depth++==0;
  Clock::time_point start=outer ? Clock::now() : Clock::time_point{};
  FrameProfile::Scope zone{FrameProfile::Zone::NpcLogicShadow,outer};
  static inline unsigned depth=0;
  ~AuditTimer() {
    if(outer)
      totals.wallNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
    if(active) --depth;
    }
  };

uint64_t deadline(uint64_t start, double duration, float speed) {
  // Pose compares an integer animation clock strictly past the duration.
  return start+uint64_t(std::ceil((std::floor(duration)+1.)/double(speed)));
  }

Action canonical(const Animation::Sequence& sequence, BodyState state) {
  const auto& name=sequence.name;
  for(const auto& event:sequence.data->events) {
    if(event.type==asset::MdsEventType::SET_FIGHT_MODE)
      return event.fight_mode==asset::MdsFightMode::NONE ? AI_RemoveWeapon : AI_DrawWeapon;
    if(event.type==asset::MdsEventType::ITEM_INSERT || event.type==asset::MdsEventType::ITEM_REMOVE)
      return AI_UseItemToState;
    }
  if(name.find("_2_STAND")!=std::string::npos || name.find("WOUNDED_2_")!=std::string::npos)
    return AI_StandUp;
  return state==BS_NONE ? AI_PlayAnim : AI_PlayAnimBs;
  }

bool bodyAction(Action action) {
  switch(action) {
    case AI_PlayAnim: case AI_PlayAnimBs: case AI_StandUp: case AI_StandUpQuick:
    case AI_RemoveWeapon: case AI_DrawWeapon: case AI_DrawWeaponMelee:
    case AI_DrawWeaponRange: case AI_DrawSpell: case AI_UseItemToState:
    case AI_UseItem: case AI_UseMob: case AI_Dodge: case AI_Attack:
      return true;
    default: return false;
    }
  }

std::string jsonQuoted(std::string_view value) {
  std::ostringstream out;
  out<<std::quoted(std::string(value));
  return out.str();
  }

uint64_t quantile(const std::map<int64_t,uint64_t>& values, double part) {
  uint64_t count=0;
  for(const auto& [value,n]:values) { (void)value; count+=n; }
  const auto rank=uint64_t(std::ceil(double(count)*part));
  uint64_t seen=0;
  for(const auto& [value,n]:values) {
    seen+=n;
    if(seen>=rank) return uint64_t(value);
    }
  return 0;
  }

std::string distribution(const std::map<int64_t,uint64_t>& values) {
  std::ostringstream out;
  out<<"{\"p50\":"<<quantile(values,.5)<<",\"p95\":"<<quantile(values,.95)
     <<",\"max\":"<<(values.empty() ? 0 : values.rbegin()->first)<<'}';
  return out.str();
  }

std::string bucketJson(const NpcLogicAudit::Bucket& bucket) {
  std::ostringstream out;
  out<<"{\"count\":"<<bucket.count<<",\"early\":"<<bucket.early<<",\"late\":"<<bucket.late
     <<",\"wait_count\":"<<bucket.waitCount<<",\"wait_early\":"<<bucket.waitEarly<<",\"wait_late\":"<<bucket.waitLate
     <<",\"wait_error_ms\":"<<distribution(bucket.waitErrorMs)
     <<",\"unpredictable_admissions\":"<<bucket.unpredictable<<",\"interrupted\":"<<bucket.interrupted<<",\"end_error_ms\":"<<distribution(bucket.endErrorMs)
     <<",\"events\":"<<bucket.events<<",\"event_mismatch\":"<<bucket.eventMismatch
     <<",\"event_error_ms\":"<<distribution(bucket.eventErrorMs)<<'}';
  return out.str();
  }
}

bool NpcLogicAudit::enabled() {
  static const bool active=[] {
    const auto* value=std::getenv("OPENGOTHIC_NPC_LOGIC_AUDIT");
    return value!=nullptr && std::string_view(value)=="1";
    }();
  return active;
  }

const NpcLogicAudit::Stats& NpcLogicAudit::stats() { return totals; }
void NpcLogicAudit::reset() { totals={}; }

std::string NpcLogicAudit::actionName(Action action) {
#define LOGIC_ACTION(name) case name: return #name
  switch(action) {
    LOGIC_ACTION(AI_None); LOGIC_ACTION(AI_PlayAnim); LOGIC_ACTION(AI_PlayAnimBs);
    LOGIC_ACTION(AI_StandUp); LOGIC_ACTION(AI_StandUpQuick); LOGIC_ACTION(AI_RemoveWeapon);
    LOGIC_ACTION(AI_DrawWeapon); LOGIC_ACTION(AI_DrawWeaponMelee); LOGIC_ACTION(AI_DrawWeaponRange);
    LOGIC_ACTION(AI_DrawSpell); LOGIC_ACTION(AI_UseItemToState); LOGIC_ACTION(AI_UseItem);
    LOGIC_ACTION(AI_UseMob); LOGIC_ACTION(AI_Dodge); LOGIC_ACTION(AI_Attack);
    LOGIC_ACTION(AI_GoToNpc); LOGIC_ACTION(AI_GoToPoint); LOGIC_ACTION(AI_GoToNextFp);
    LOGIC_ACTION(AI_TurnToNpc); LOGIC_ACTION(AI_TurnAway); LOGIC_ACTION(AI_WhirlToNpc);
    LOGIC_ACTION(AI_Output); LOGIC_ACTION(AI_OutputSvm); LOGIC_ACTION(AI_OutputSvmOverlay);
    LOGIC_ACTION(AI_Wait); LOGIC_ACTION(AI_Flee); LOGIC_ACTION(AI_GotoItem);
    LOGIC_ACTION(AI_AlignToFp); LOGIC_ACTION(AI_AlignToWp); LOGIC_ACTION(AI_ProcessInfo);
    default: return "AI_"+std::to_string(uint32_t(action));
    }
#undef LOGIC_ACTION
  }

uint64_t NpcLogicAudit::Stats::reanchoredCount() const {
  uint64_t count=0;
  for(const auto& entry:reanchored) count+=entry.second;
  return count;
  }

std::string NpcLogicAudit::Stats::probeFields() const {
  std::ostringstream out;
  uint64_t inputs=0;
  for(const auto& [kind,count]:reanchored) { (void)kind; inputs+=count; }
  out<<"npc_logic_body_samples="<<bodySamples
     <<" npc_logic_body_interactive_samples="<<bodyInteractiveSamples
     <<" npc_logic_body_external_samples="<<bodyExternalSamples
     <<" npc_logic_weapon_samples="<<weaponSamples
     <<" npc_logic_wait_early="<<waitEarly<<" npc_logic_wait_late="<<waitLate<<" npc_logic_reanchored="<<inputs
     <<" npc_logic_samples="<<ticks<<" npc_logic_body_mismatch="<<bodyMismatch
     <<" npc_logic_weapon_mismatch="<<weaponMismatch<<" npc_logic_early="<<early
     <<" npc_logic_late="<<late<<" npc_logic_events="<<events
     <<" npc_logic_event_mismatch="<<eventMismatch<<" npc_logic_wall_ms="<<double(wallNs)/1e6;
  return out.str();
  }

std::string NpcLogicAudit::Stats::json() const {
  std::ostringstream out;
  out<<"{\"enabled\":"<<(enabled() ? "true" : "false")<<",\"samples\":"<<ticks
     <<",\"body_samples\":"<<bodySamples
     <<",\"body_interactive_samples\":"<<bodyInteractiveSamples
     <<",\"body_external_samples\":"<<bodyExternalSamples
     <<",\"simplified_body_samples\":"<<simplifiedBodySamples
     <<",\"simplified_body_mismatch\":"<<simplifiedBodyMismatch
     <<",\"weapon_samples\":"<<weaponSamples
     <<",\"wait_early\":"<<waitEarly<<",\"wait_late\":"<<waitLate
     <<",\"body_mismatch\":"<<bodyMismatch<<",\"weapon_mismatch\":"<<weaponMismatch
     <<",\"early\":"<<early<<",\"late\":"<<late<<",\"events\":"<<events
     <<",\"event_mismatch\":"<<eventMismatch<<",\"wall_ns\":"<<wallNs<<",\"by_action\":{";
  bool comma=false;
  for(const auto& [kind,bucket]:byAction) {
    if(comma) out<<',';
    comma=true;
    out<<jsonQuoted(actionName(kind))<<':'<<bucketJson(bucket);
    }
  out<<"},\"by_animation\":{";
  comma=false;
  for(const auto& [name,bucket]:byAnimation) {
    if(comma) out<<',';
    comma=true;
    out<<jsonQuoted(name)<<':'<<bucketJson(bucket);
    }
  out<<"},\"deferred_admissions\":{";
  comma=false;
  for(const auto& [reason,count]:deferred) {
    if(comma) out<<',';
    comma=true;
    out<<jsonQuoted(reason)<<':'<<count;
    }
  out<<"},\"reanchored_inputs\":{";
  comma=false;
  for(const auto& [reason,count]:reanchored) {
    if(comma) out<<',';
    comma=true;
    out<<jsonQuoted(reason)<<':'<<count;
    }
  out<<"},\"body_mismatch_kinds\":{";
  comma=false;
  for(const auto& [kind,count]:bodyMismatchKinds) {
    if(comma) out<<',';
    comma=true;
    out<<jsonQuoted(kind)<<':'<<count;
    }
  out<<"},\"weapon_mismatch_kinds\":{";
  comma=false;
  for(const auto& [kind,count]:weaponMismatchKinds) {
    if(comma) out<<',';
    comma=true;
    out<<jsonQuoted(kind)<<':'<<count;
    }
  out<<"}}";
  return out.str();
  }

bool NpcLogicalState::hasState(BodyState value) const {
  if(executorState && (value==BS_STAND || value==BS_WALK || value==BS_RUN || value==BS_SNEAK))
    return value==*executorState;
  for(const auto& layer:layers)
    if(!layer.finished && layer.state==value) return true;
  return interactiveState && *interactiveState==value;
  }

bool NpcLogicalState::hasStateFlag(BodyState value) const {
  if(executorState && (*executorState&(BS_FLAG_MASK|BS_MOD_MASK))==value) return true;
  for(const auto& layer:layers) {
    const auto state=BodyState(layer.state&(BS_MAX|BS_FLAG_MASK));
    if(executorState && (state==BS_STAND || state==BS_WALK || state==BS_RUN || state==BS_SNEAK)) continue;
    if(!layer.finished && (layer.state&(BS_FLAG_MASK|BS_MOD_MASK))==value) return true;
    }
  return interactiveState && (*interactiveState&(BS_FLAG_MASK|BS_MOD_MASK))==value;
  }

BodyState NpcLogicalState::bodyStateMasked() const { return BodyState(body&(BS_MAX|BS_FLAG_MASK)); }

float NpcLogicalState::animProgress(std::string_view name, uint64_t now) const {
  for(const auto& layer:layers) {
    if(layer.finished || layer.sequence->name!=name) continue;
    const float total=layer.sequence->totalTime();
    if(total<=0.f) return 1;
    const auto played=now>layer.start ? uint64_t(double(now-layer.start)*double(layer.speed)) : 0;
    return std::clamp(float(played)/total,0.f,1.f);
    }
  return -1;
  }

namespace {
WeaponState fightMode(asset::MdsFightMode mode) {
  switch(mode) {
    case asset::MdsFightMode::FIST: return WeaponState::Fist;
    case asset::MdsFightMode::SINGLE_HANDED: return WeaponState::W1H;
    case asset::MdsFightMode::DUAL_HANDED: return WeaponState::W2H;
    case asset::MdsFightMode::BOW: return WeaponState::Bow;
    case asset::MdsFightMode::CROSSBOW: return WeaponState::CBow;
    case asset::MdsFightMode::MAGIC: return WeaponState::Mage;
    default: return WeaponState::NoWeapon;
    }
  }
std::optional<BodyState> externalBody(const Npc& npc) {
  if(npc.isDead()) return BS_DEAD;
  if(npc.isUnconscious()) return BS_UNCONSCIOUS;
  if(npc.isFalling()) return BS_FALL;
  return {};
  }
void endSample(NpcLogicAudit::Bucket& bucket, int64_t error, bool wait) {
  if(wait) {
    ++bucket.waitCount;
    ++bucket.waitErrorMs[std::abs(error)];
    if(error<0) ++bucket.waitEarly;
    if(error>0) ++bucket.waitLate;
    } else {
    ++bucket.count;
    ++bucket.endErrorMs[std::abs(error)];
    if(error<0) ++bucket.early;
    if(error>0) ++bucket.late;
    }
  }
}

void NpcLogicalState::updateBody() {
  uint32_t state=BS_NONE;
  for(const auto& layer:layers)
    if(!layer.finished) state=std::max(state,uint32_t(layer.state&(BS_MAX|BS_FLAG_MASK)));
  if(executorState) state=uint32_t(*executorState);
  if(interactiveState) state=uint32_t(*interactiveState);
  if(externalState) state=uint32_t(*externalState);
  body=BodyState(state);
  }

void NpcLogicalState::schedule(Layer& layer) {
  const auto& sequence=*layer.sequence;
  layer.expectedEnd=sequence.animCls==Animation::Loop ? 0 :
      deadline(layer.start,std::min(sequence.atkTotalTime(layer.combo),sequence.totalTime()),layer.speed);
  layer.events.clear();
  if(NpcLogicAudit::enabled()) {
    std::vector<Animation::LogicEvent> events;
    sequence.logicEvents(events);
    for(const auto& event:events)
      layer.events.push_back({event,layer.start+uint64_t(std::ceil(double(event.time)/double(layer.speed)))});
    }
  // Weapon prediction needs only SET_FIGHT_MODE, also with audit disabled.
  // Full item/sound/PFX/window schedules are allocated only for the audit.
  layer.weaponEvents.clear();
  for(size_t i=0;i<sequence.data->events.size();++i) {
    const auto& event=sequence.data->events[i];
    if(event.type!=asset::MdsEventType::SET_FIGHT_MODE) continue;
    const auto local=sequence.logicTag(i).time;
    layer.weaponEvents.emplace_back(layer.start+uint64_t(std::ceil(double(local)/double(layer.speed))),fightMode(event.fight_mode));
    }
  }

void NpcLogicalState::advance(uint64_t now) {
  for(size_t i=0,count=layers.size();i<count;++i) {
    auto& layer=layers[i];
    if(layer.finished) continue;
    for(auto& [at,mode]:layer.weaponEvents) {
      if(at>now) continue;
      weapon=mode;
      if(layer.sequence->animCls==Animation::Loop && layer.sequence->totalTime()>0) {
        const auto period=double(layer.sequence->totalTime())/layer.speed;
        at+=uint64_t((std::floor(double(now-at)/period)+1)*period);
        } else at=UINT64_MAX;
      }
    if(layer.expectedEnd==0 || now<layer.expectedEnd) continue;
    const auto* next=layer.sequence->nextPtr;
    // These resolutions depend on executor state, not just Sequence metadata.
    // Retain them until an observed mutation and report that re-anchor.
    if(layer.sequence->isRotate() || (layer.state&BS_ITEMINTERACT)==BS_ITEMINTERACT || next==layer.sequence) continue;
    layer.finished=true;
    if(next!=nullptr) {
      Layer successor;
      successor.sequence=next;
      successor.origin=successor.start=now;
      successor.state=layer.state;
      const auto base=layer.state&BS_MAX;
      if(base==BS_STUMBLE || base==BS_PARADE) successor.state=BS_NONE;
      successor.combo=layer.combo;
      successor.action=canonical(*next,successor.state);
      if(const auto found=speeds.find(next->name);found!=speeds.end()) successor.speed=found->second;
      schedule(successor);
      layers.push_back(std::move(successor));
      }
    }
  updateBody();
  }

void NpcLogicalState::admit(Npc& npc, Action kind, const Animation::Sequence* requested) {
  AuditTimer timer;
  admissions.push_back({kind,requested,npc.world().tickCount(),npc.aniWaitTime});
  if(!NpcLogicAudit::enabled() || bodyAction(kind)) return;
  const char* reason=nullptr;
  switch(kind) {
    case AI_GoToNpc: case AI_GoToPoint: case AI_GoToNextFp: case AI_GotoItem: case AI_Flee:
      reason="path_or_physics_6c"; break;
    case AI_TurnToNpc: case AI_TurnAway: case AI_WhirlToNpc: case AI_AlignToFp: case AI_AlignToWp:
      reason="turn_by_angle_6c"; break;
    case AI_Output: case AI_OutputSvm: case AI_OutputSvmOverlay: case AI_ProcessInfo:
      reason="dialogue_output_6d"; break;
    default: break;
    }
  if(reason) { ++totals.byAction[kind].unpredictable; ++totals.deferred[reason]; }
  }

void NpcLogicalState::sync(Npc& npc, bool rebuild, Action admitted, const Animation::Sequence* requested) {
  AuditTimer timer;
  auto& pose=npc.visual.pose();
  const auto now=npc.world().tickCount();
  if(admitted!=AI_None) admit(npc,admitted,requested);
  const bool audit=NpcLogicAudit::enabled();
  auto mobState=npc.interactive() ? std::optional<BodyState>(BodyState(npc.interactive()->stateMask())) : std::nullopt;
  const auto external=externalBody(npc);
  const auto executor=npc.simplifiedBodyState();
  if(rebuild || !initialized) {
    layers.clear(); admissions.clear(); speeds.clear(); action={}; actionWait=false;
    for(const auto& [name,speed]:pose.logicalDefaultSpeeds()) speeds[name]=speed;
    for(size_t i=0;i<pose.logicalLayerCount();++i) {
      const auto source=pose.logicalLayer(i);
      Layer layer;
      layer.sequence=source.sequence; layer.start=source.start; layer.origin=source.origin;
      layer.speed=source.speed; layer.state=source.state; layer.combo=pose.comboLength();
      layer.action=canonical(*source.sequence,source.state);
      schedule(layer);
      for(auto& event:layer.weaponEvents) if(event.first<=now) event.first=UINT64_MAX;
      layers.push_back(std::move(layer));
      }
    interactiveState=mobState; externalState=external; executorState=executor; weapon=npc.weaponState();
    updateBody();
    pose.enableLogicalInputs(); pose.clearLogicalInputs(); initialized=true;
    // The saved wait identifies that a wait exists, never its predicted time.
    // Ambiguous layers stay unknown; selecting the longest is not evidence.
    {
      const Layer* own=nullptr;
      for(const auto& layer:layers) {
        if(layer.expectedEnd==0 || layer.expectedEnd<=now || layer.sequence->isRotate()) continue;
        if(own) { own=nullptr; break; }
        own=&layer;
        }
      if(own) {
        action={own->action,own->sequence,own->start,own->origin,own->expectedEnd,true};
        actionWait=npc.aniWaitTime>=now; nativeWaitEnd=npc.aniWaitTime;
        } else if(audit && npc.aniWaitTime>=now) ++totals.reanchored["load_action_unknown"];
      }
    return;
    }

  advance(now);
  const auto& inputs=pose.logicalInputs();
  bool bodyInput=mobState!=interactiveState || external!=externalState || executor!=executorState;
  bool weaponInput=false;
  if(audit && mobState!=interactiveState) ++totals.reanchored["interactive_mask_6d"];
  if(audit && external!=externalState) {
    const auto state=external ? *external : (externalState ? *externalState : BS_NONE);
    ++totals.reanchored[state==BS_FALL ? "falling_6c" : "script_state_6e"];
    }
  using Kind=Pose::LogicalInput::Kind;
  using Cause=Pose::LogicalInput::Cause;
  for(const auto& input:inputs) {
    if(input.cause==Cause::Natural) continue;
    if(input.kind==Kind::Weapon) weaponInput=true;
    else if(input.sequence!=nullptr || input.kind==Kind::Combo) bodyInput=true;
    if(audit) {
      const char* reason="layer_start";
      switch(input.kind) {
        case Kind::LayerRemove: reason="explicit_layer_remove"; break;
        case Kind::Speed: reason="speed_6e"; break;
        case Kind::HitStop: reason="hit_stop_6e"; break;
        case Kind::HitStopEnd: reason="hit_stop_end_6e"; break;
        case Kind::Combo: reason="combo_6e"; break;
        case Kind::Weapon: reason="explicit_weapon"; break;
        default: break;
        }
      ++totals.reanchored[reason];
      }
    }
  // Observe the forecast from the previous boundary before applying ANY of
  // this tick's executor inputs. Input-affected fields are excluded, not matches.
  auditTick(npc,bodyInput,weaponInput);

  for(const auto& input:inputs) {
    auto found=std::find_if(layers.begin(),layers.end(),[&](const Layer& layer) {
      return layer.sequence==input.sequence && layer.origin==input.origin;
      });
    if(input.cause==Cause::Natural) {
      if(input.kind==Kind::LayerRemove) {
        sequenceEnd(input.sequence,input.origin,input.at);
        if(found!=layers.end() && !found->finished) {
          if(audit) ++totals.reanchored["executor_next_6c_6d"];
          found->finished=true;
          }
        } else if(input.kind==Kind::LayerSet && found==layers.end()) {
        // Overlay lookup, item-use destination and rotation retention are
        // executor decisions. Never turn their observed result into a match.
        if(audit) ++totals.reanchored["executor_next_6c_6d"];
        layers.erase(std::remove_if(layers.begin(),layers.end(),[&](const Layer& layer) {
          return layer.sequence->layer==input.sequence->layer;
          }),layers.end());
        Layer layer;
        layer.sequence=input.sequence; layer.start=input.start; layer.origin=input.origin;
        layer.speed=input.speed; layer.state=input.state; layer.combo=input.combo;
        layer.action=canonical(*input.sequence,input.state); schedule(layer);
        layers.push_back(std::move(layer));
        }
      continue;
      }
    if(input.kind==Kind::Weapon) { weapon=input.weapon; continue; }
    if(input.kind==Kind::Speed && input.sequence==nullptr) { speeds[input.name]=input.speed; continue; }
    if(input.kind==Kind::Combo && input.sequence==nullptr) {
      for(auto& layer:layers) { layer.combo=input.combo; schedule(layer); }
      continue;
      }
    if(input.kind==Kind::LayerRemove) {
      if(found!=layers.end()) {
        if(audit && !found->finished) {
          ++totals.byAnimation[found->sequence->name].interrupted;
          ++totals.byAction[found->action].interrupted;
          }
        layers.erase(found);
        }
      continue;
      }
    if(input.kind==Kind::LayerSet) {
      layers.erase(std::remove_if(layers.begin(),layers.end(),[&](const Layer& layer) {
        return layer.sequence->layer==input.sequence->layer &&
            (layer.sequence!=input.sequence || layer.origin!=input.origin);
        }),layers.end());
      found=std::find_if(layers.begin(),layers.end(),[&](const Layer& layer) {
        return layer.sequence==input.sequence && layer.origin==input.origin;
        });
      }
    if(found==layers.end()) {
      if(input.kind!=Kind::LayerSet) continue;
      layers.emplace_back(); found=layers.end()-1;
      }
    auto& layer=*found;
    layer.sequence=input.sequence; layer.start=input.start; layer.origin=input.origin;
    layer.speed=input.speed; layer.state=input.state; layer.combo=input.combo;
    layer.finished=false; layer.action=canonical(*input.sequence,input.state);
    schedule(layer);
    if(input.kind!=Kind::LayerSet)
      for(auto& event:layer.weaponEvents) if(event.first<=input.at) event.first=UINT64_MAX;
    if(action.sequence==layer.sequence && action.origin==layer.origin)
      action.expectedEnd=layer.expectedEnd!=0 ? layer.expectedEnd : deadline(layer.start,layer.sequence->totalTime(),layer.speed);
    }
  pose.clearLogicalInputs();
  layers.erase(std::remove_if(layers.begin(),layers.end(),[](const Layer& l){return l.finished;}),layers.end());
  interactiveState=mobState; externalState=external; executorState=executor; updateBody();

  if(actionWait && npc.aniWaitTime!=nativeWaitEnd) {
    if(audit) ++totals.reanchored["wait_cancelled_or_replaced"];
    actionWait=false;
    }
  for(const auto& admission:admissions) {
    if(!bodyAction(admission.kind)) continue;
    if(actionWait && nativeWaitEnd==admission.wait) continue;
    const Layer* own=nullptr;
    for(const auto& layer:layers) {
      if(admission.sequence ? layer.sequence!=admission.sequence : layer.origin!=admission.at) continue;
      if(layer.sequence->isRotate()) continue;
      if(own) { own=nullptr; break; }
      own=&layer;
      }
    if(!own) {
      if(audit) ++totals.reanchored["admitted_action_layer_unknown"];
      continue;
      }
    // A loop never leaves Pose naturally. PlayAni admits one new logical
    // period, even when the executor reuses a loop that began long ago.
    const auto start=own->expectedEnd!=0 ? own->start : admission.at;
    const uint64_t end=own->expectedEnd!=0 ? own->expectedEnd : deadline(start,own->sequence->totalTime(),own->speed);
    action={admission.kind,own->sequence,start,own->origin,end,true};
    for(auto& layer:layers)
      if(layer.sequence==own->sequence && layer.origin==own->origin) layer.action=admission.kind;
    if(admission.wait>=now) { actionWait=true; nativeWaitEnd=admission.wait; }
    }
  admissions.clear();
  }

void NpcLogicalState::auditTick(Npc& npc, bool bodyInput, bool weaponInput) {
  if(!NpcLogicAudit::enabled()) return;
  ++totals.ticks;
  const auto context=[&] {
    std::string key;
    for(const auto& layer:layers) if(!layer.finished) { key=layer.sequence->name; break; }
    key+="/";
    const auto& pose=npc.visual.pose();
    if(pose.logicalLayerCount()>0) key+=pose.logicalLayer(0).sequence->name;
    return key;
    };
  if(!bodyInput && externalState) {
    ++totals.bodyExternalSamples;
    } else if(!bodyInput && interactiveState) {
    ++totals.bodyInteractiveSamples;
    } else if(!bodyInput) {
    ++totals.bodySamples;
    const bool simplified=npc.usesSimplifiedMovement();
    if(simplified) ++totals.simplifiedBodySamples;
    // Executor roles must be checked against the puppet, not the same role
    // returned by Npc::bodyState. Mobsi and script overrides are counted above.
    const auto actual=executorState ? npc.visual.pose().bodyState() : npc.bodyState();
    if(body!=actual) {
      ++totals.bodyMismatch;
      if(simplified) ++totals.simplifiedBodyMismatch;
      ++totals.bodyMismatchKinds[std::to_string(uint32_t(body))+"->"+std::to_string(uint32_t(actual))+":"+context()];
      }
    }
  if(!weaponInput) {
    ++totals.weaponSamples;
    if(weapon!=npc.weaponState()) {
      ++totals.weaponMismatch;
      ++totals.weaponMismatchKinds[std::to_string(uint32_t(weapon))+"->"+std::to_string(uint32_t(npc.weaponState()))+":"+context()];
      }
    }
  }

void NpcLogicalState::waitReleased(uint64_t now) {
  if(!actionWait) return;
  recordActionEnd(now);
  }
void NpcLogicalState::recordActionEnd(uint64_t now) {
  if(NpcLogicAudit::enabled() && action.sequence && action.predictable) {
    const auto error=int64_t(now)-int64_t(action.expectedEnd);
    endSample(totals.byAction[action.kind],error,true);
    if(error<0) ++totals.waitEarly;
    if(error>0) ++totals.waitLate;
    }
  actionWait=false;
  }
void NpcLogicalState::sequenceEnd(const Animation::Sequence* sequence,uint64_t origin,uint64_t now) {
  if(!NpcLogicAudit::enabled()) return;
  const auto found=std::find_if(layers.begin(),layers.end(),[&](const Layer& l) {return l.sequence==sequence && l.origin==origin;});
  if(found==layers.end() || found->expectedEnd==0) return;
  const auto error=int64_t(now)-int64_t(found->expectedEnd);
  endSample(totals.byAnimation[sequence->name],error,false);
  endSample(totals.byAction[found->action],error,false);
  if(error<0) ++totals.early;
  if(error>0) ++totals.late;
  }

void NpcLogicalState::eventFired(const Animation::Sequence* sequence,uint64_t origin,
                               const Animation::LogicEvent& source,uint64_t now) {
  if(!NpcLogicAudit::enabled()) return;
  AuditTimer timer;
  for(const auto& layer:layers) {
    if(layer.sequence!=sequence || layer.origin!=origin) continue;
    const auto scheduled=std::find_if(layer.events.begin(),layer.events.end(),[&](const Event& event) {
      return event.source.kind==source.kind && event.source.index==source.index && event.source.frame==source.frame;
      });
    if(scheduled==layer.events.end()) return;
    uint64_t expected=scheduled->expected;
    if(sequence->animCls==Animation::Loop && sequence->totalTime()>0 && now>expected) {
      const double period=double(sequence->totalTime())/double(layer.speed);
      expected+=uint64_t(std::round(double(now-expected)/period)*period);
      }
    const auto error=std::abs(int64_t(now)-int64_t(expected));
    ++totals.events;
    const bool mismatch=uint64_t(error)>worldStep;
    if(mismatch) ++totals.eventMismatch;
    const auto record=[&](NpcLogicAudit::Bucket& bucket) {
      ++bucket.events;
      if(mismatch) ++bucket.eventMismatch;
      ++bucket.eventErrorMs[error];
      };
    record(totals.byAction[layer.action]); record(totals.byAnimation[sequence->name]);
    return;
    }
  ++totals.reanchored["event_before_layer_input"];
  }

NpcLogicalState::Tick::Tick(Npc& value,uint64_t dt):npc(value) {
  if(!npc.logicalBody.initialized) npc.syncLogicalBodyState(true);
  npc.logicalBody.worldStep=dt;
  }
NpcLogicalState::Admission::~Admission() { npc.logicalBody.admit(npc,kind,sequence); }

void Npc::suspendLogicalBodyState() {
  visual.pose().disableLogicalInputs();
  logicalBody={};
  }
