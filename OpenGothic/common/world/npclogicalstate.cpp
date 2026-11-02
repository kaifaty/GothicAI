#include "npclogicalstate.h"

#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/world.h"
#include "frameprofile.h"

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
  bool active=NpcLogicAudit::enabled();
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
      return name.find("_2_STAND")!=std::string::npos ? AI_RemoveWeapon : AI_DrawWeapon;
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

std::string quoted(std::string_view value) {
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
    LOGIC_ACTION(AI_Wait);
    default: return "AI_"+std::to_string(uint32_t(action));
    }
#undef LOGIC_ACTION
  }

std::string NpcLogicAudit::Stats::probeFields() const {
  std::ostringstream out;
  out<<"npc_logic_samples="<<ticks<<" npc_logic_body_mismatch="<<bodyMismatch
     <<" npc_logic_weapon_mismatch="<<weaponMismatch<<" npc_logic_early="<<early
     <<" npc_logic_late="<<late<<" npc_logic_events="<<events
     <<" npc_logic_event_mismatch="<<eventMismatch<<" npc_logic_wall_ms="<<double(wallNs)/1e6;
  return out.str();
  }

std::string NpcLogicAudit::Stats::json() const {
  std::ostringstream out;
  out<<"{\"enabled\":"<<(enabled() ? "true" : "false")<<",\"samples\":"<<ticks
     <<",\"body_mismatch\":"<<bodyMismatch<<",\"weapon_mismatch\":"<<weaponMismatch
     <<",\"early\":"<<early<<",\"late\":"<<late<<",\"events\":"<<events
     <<",\"event_mismatch\":"<<eventMismatch<<",\"wall_ns\":"<<wallNs<<",\"by_action\":{";
  bool comma=false;
  for(const auto& [kind,bucket]:byAction) {
    if(comma) out<<',';
    comma=true;
    out<<quoted(actionName(kind))<<':'<<bucketJson(bucket);
    }
  out<<"},\"by_animation\":{";
  comma=false;
  for(const auto& [name,bucket]:byAnimation) {
    if(comma) out<<',';
    comma=true;
    out<<quoted(name)<<':'<<bucketJson(bucket);
    }
  out<<"},\"deferred_admissions\":{";
  comma=false;
  for(const auto& [reason,count]:deferred) {
    if(comma) out<<',';
    comma=true;
    out<<quoted(reason)<<':'<<count;
    }
  out<<"}}";
  return out.str();
  }

bool NpcLogicalState::hasState(BodyState value) const {
  for(const auto& layer:layers)
    if(layer.state==value) return true;
  return interactiveState && *interactiveState==value;
  }

bool NpcLogicalState::hasStateFlag(BodyState value) const {
  for(const auto& layer:layers)
    if((layer.state&(BS_FLAG_MASK|BS_MOD_MASK))==value) return true;
  return interactiveState && (*interactiveState&(BS_FLAG_MASK|BS_MOD_MASK))==value;
  }

BodyState NpcLogicalState::bodyStateMasked() const { return BodyState(body&(BS_MAX|BS_FLAG_MASK)); }

float NpcLogicalState::animProgress(std::string_view name, uint64_t now) const {
  for(const auto& layer:layers) {
    if(layer.sequence->name!=name) continue;
    const float total=layer.sequence->totalTime();
    if(total<=0.f) return 1;
    const auto played=now>layer.start ? uint64_t(double(now-layer.start)*double(layer.speed)) : 0;
    return std::clamp(float(played)/total,0.f,1.f);
    }
  return -1;
  }

void NpcLogicalState::sync(Npc& npc, bool rebuild, Action admitted) {
  const bool audit=NpcLogicAudit::enabled();
  AuditTimer timer;
  const Pose& pose=npc.visual.pose();
  const uint64_t now=npc.world().tickCount();
  if(!rebuild && actionWait && now>nativeWaitEnd) recordActionEnd(now);
  if(rebuild) { layers.clear(); action={}; actionWait=false; nativeWaitEnd=0; }
  for(size_t i=0;i<layers.size();) {
    bool found=false;
    for(size_t p=0;p<pose.logicalLayerCount();++p) {
      const auto layer=pose.logicalLayer(p);
      if(layers[i].sequence==layer.sequence && layers[i].origin==layer.origin) { found=true; break; }
      }
    if(found) { ++i; continue; }
    if(audit && !layers[i].finished && layers[i].sequence->animCls!=Animation::Loop) {
      ++totals.byAction[layers[i].action].interrupted;
      ++totals.byAnimation[layers[i].sequence->name].interrupted;
      }
    if(!actionWait && action.sequence==layers[i].sequence && action.start==layers[i].origin)
      action={};
    layers.erase(layers.begin()+ptrdiff_t(i));
    }
  uint32_t state=BS_NONE;
  for(size_t p=0;p<pose.logicalLayerCount();++p) {
    const auto source=pose.logicalLayer(p);
    state=std::max(state,uint32_t(source.state&(BS_MAX|BS_FLAG_MASK)));
    auto found=std::find_if(layers.begin(),layers.end(),[&](const Layer& layer) {
      return layer.sequence==source.sequence && layer.origin==source.origin;
      });
    bool fresh=found==layers.end();
    if(fresh) {
      layers.emplace_back();
      found=layers.end()-1;
      found->sequence=source.sequence;
      found->origin=source.origin;
      found->action=bodyAction(admitted) ? admitted : canonical(*source.sequence,source.state);
      }
    auto& layer=*found;
    const bool changed=fresh || layer.start!=source.start || layer.speed!=source.speed;
    layer.start=source.start;
    layer.speed=source.speed;
    layer.state=source.state;
    layer.combo=pose.comboLength();
    layer.expectedEnd=source.sequence->animCls==Animation::Loop ? 0 :
        deadline(layer.start,double(std::min(source.sequence->atkTotalTime(layer.combo),source.sequence->totalTime())),layer.speed);
    if(!actionWait && action.sequence==layer.sequence && action.start==layer.origin)
      action.expectedEnd=layer.expectedEnd;
    if(changed) {
      std::vector<Animation::LogicEvent> schedule;
      source.sequence->logicEvents(schedule);
      layer.events.resize(schedule.size());
      for(size_t e=0;e<schedule.size();++e) {
        layer.events[e].source=schedule[e];
        layer.events[e].expected=layer.start+uint64_t(std::ceil(double(schedule[e].time)/double(layer.speed)));
        }
      }
    }
  interactiveState.reset();
  if(auto* mob=npc.interactive()) {
    interactiveState=BodyState(mob->stateMask());
    state=uint32_t(*interactiveState);
    }
  if(npc.isDead()) state=BS_DEAD;
  else if(npc.isUnconscious()) state=BS_UNCONSCIOUS;
  else if(npc.isFalling()) state=BS_FALL;
  body=BodyState(state);
  weapon=npc.weaponState();
  if(admitted!=AI_None && !bodyAction(admitted)) {
    const char* reason=nullptr;
    switch(admitted) {
      case AI_GoToNpc: case AI_GoToPoint: case AI_GoToNextFp: case AI_GotoItem: case AI_Flee:
        reason="path_or_physics_6c"; break;
      case AI_TurnToNpc: case AI_TurnAway: case AI_WhirlToNpc: case AI_AlignToFp: case AI_AlignToWp:
        reason="turn_by_angle_6c"; break;
      case AI_Output: case AI_OutputSvm: case AI_OutputSvmOverlay: case AI_ProcessInfo:
        reason="dialogue_output_6d"; break;
      default: break;
      }
    if(audit && reason!=nullptr) {
      ++totals.byAction[admitted].unpredictable;
      ++totals.deferred[reason];
      }
    }
  // A consumed AI_PlayAni can wait for one period of a loop. Its action
  // clock is finite even though that layer has no natural end.
  if((bodyAction(admitted) || rebuild) && npc.aniWaitTime>=now) {
    const Layer* chosen=nullptr;
    // PlayAni waits for the sequence it just admitted, not an unrelated long
    // layer (for example, a dialogue gesture). StandUp uses the longest layer.
    const bool play=admitted==AI_PlayAnim || admitted==AI_PlayAnimBs;
    for(const auto& layer:layers) {
      if(play && (layer.origin!=now || layer.action!=admitted)) continue;
      if(chosen==nullptr || layer.sequence->totalTime()/layer.speed>chosen->sequence->totalTime()/chosen->speed)
        chosen=&layer;
      }
    if(chosen!=nullptr) {
      const auto kind=rebuild ? chosen->action : admitted;
      // Native AI waits use totalTime without playback speed, except StandUp,
      // which uses Pose::animationTotalTime. Rebuild their admission time from
      // the saved wait and metadata; layer starts can be rebased by Pose::save.
      const float speed=kind==AI_StandUp || kind==AI_StandUpQuick ? chosen->speed : 1.f;
      const auto duration=uint64_t(chosen->sequence->totalTime()/speed);
      const uint64_t start=rebuild ? npc.aniWaitTime-std::min(npc.aniWaitTime,duration) : now;
      action={kind,chosen->sequence,start,
              start+duration+1,true};
      nativeWaitEnd=npc.aniWaitTime;
      actionWait=true;
      }
    }
  if(!actionWait && (action.sequence==nullptr || rebuild)) {
    action={};
    for(const auto& layer:layers) {
      if(layer.finished || layer.expectedEnd==0 || layer.expectedEnd<now) continue;
      if(action.sequence==nullptr || layer.origin>=action.start)
        action={layer.action,layer.sequence,layer.origin,layer.expectedEnd,true};
      }
    }
  if(action.sequence==nullptr) {
    if(npc.go2.flag!=Npc::GT_No) {
      action.kind=npc.go2.flag==Npc::GT_NextFp ? AI_GoToNextFp :
                  npc.go2.flag==Npc::GT_Flee ? AI_Flee :
                  npc.go2.flag==Npc::GT_Item ? AI_GotoItem : AI_GoToPoint;
      } else if(npc.outWaitTime>now) {
      action.kind=AI_Output;
      } else if(const auto* queued=npc.aiQueue.front()) {
      action.kind=queued->act;
      }
    }
  }

void NpcLogicalState::auditTick(Npc& npc) {
  if(!NpcLogicAudit::enabled()) return;
  AuditTimer timer;
  ++totals.ticks;
  if(body!=npc.bodyState()) ++totals.bodyMismatch;
  if(weapon!=npc.weaponState()) ++totals.weaponMismatch;
  }

void NpcLogicalState::recordActionEnd(uint64_t now) {
  if(NpcLogicAudit::enabled() && action.sequence!=nullptr && action.predictable) {
    const int64_t error=int64_t(now)-int64_t(action.expectedEnd);
    const int64_t magnitude=std::abs(error);
    const auto record=[&](NpcLogicAudit::Bucket& bucket) {
      ++bucket.count;
      ++bucket.endErrorMs[magnitude];
      if(error<0) ++bucket.early;
      if(error>0) ++bucket.late;
      };
    record(totals.byAction[action.kind]);
    if(error<0) ++totals.early;
    if(error>0) ++totals.late;
    }
  action={};
  actionWait=false;
  nativeWaitEnd=0;
  }

void NpcLogicalState::sequenceEnd(const Animation::Sequence* sequence, uint64_t origin, uint64_t now) {
  AuditTimer timer;
  for(auto& layer:layers) {
    if(layer.sequence!=sequence || layer.origin!=origin || layer.finished || layer.expectedEnd==0) continue;
    layer.finished=true;
    if(NpcLogicAudit::enabled()) {
      const int64_t error=int64_t(now)-int64_t(layer.expectedEnd);
      auto& bucket=totals.byAnimation[sequence->name];
      ++bucket.count;
      ++bucket.endErrorMs[std::abs(error)];
      if(error<0) ++bucket.early;
      if(error>0) ++bucket.late;
      }
    if(!actionWait && action.sequence==sequence && action.start==origin) recordActionEnd(now);
    return;
    }
  }

void NpcLogicalState::eventFired(const Animation::Sequence* sequence, uint64_t origin,
                               const Animation::LogicEvent& source, uint64_t now) {
  if(!NpcLogicAudit::enabled()) return;
  AuditTimer timer;
  for(const auto& layer:layers) {
    if(layer.sequence!=sequence || layer.origin!=origin) continue;
    const auto scheduled=std::find_if(layer.events.begin(),layer.events.end(),[&](const Event& event) {
      return event.source.kind==source.kind && event.source.index==source.index && event.source.frame==source.frame;
      });
    if(scheduled==layer.events.end()) return;
    uint64_t expected=scheduled->expected;
    if(sequence->animCls==Animation::Loop && sequence->totalTime()>0) {
      const double period=double(sequence->totalTime())/double(layer.speed);
      if(now>expected) expected+=uint64_t(std::floor(double(now-expected)/period)*period);
      }
    const int64_t error=std::abs(int64_t(now)-int64_t(expected));
    ++totals.events;
    if(error!=0) ++totals.eventMismatch;
    const auto record=[&](NpcLogicAudit::Bucket& bucket) {
      ++bucket.events;
      if(error!=0) ++bucket.eventMismatch;
      ++bucket.eventErrorMs[error];
      };
    record(totals.byAction[layer.action]);
    record(totals.byAnimation[sequence->name]);
    return;
    }
  }

NpcLogicalState::Tick::Tick(Npc& value, bool count):npc(value),sample(count) { npc.syncLogicalBodyState(); }
NpcLogicalState::Tick::~Tick() {
  npc.syncLogicalBodyState();
  if(sample) npc.logicalBody.auditTick(npc);
  }
NpcLogicalState::Admission::~Admission() { npc.logicalBody.sync(npc,false,kind); }
