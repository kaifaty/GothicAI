#include "bodyenginepolicy.h"

#include <Tempest/Log>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace {
using Json = rapidjson::Document;
constexpr const char* actionNames[BodyNet::actionCount]={
  "continue", "approach", "turn_to_target", "strafe_left", "strafe_right", "jump_back",
  "attack_front", "attack_left", "attack_right", "block", "wait_200", "wait_300"
  };
struct Network final {
  std::unique_ptr<BodyNet> net;
  std::string error;
  Network() {
    try {
      std::string path;
      if(const char* configured=std::getenv("OPENGOTHIC_BODY_NET")) path=configured;
      else {
#ifdef _WIN32
        // the home of the launcher's and the services' Path.home(); HOME is usually unset on Windows
        const char* home=std::getenv("USERPROFILE");
        if(!home || !*home) throw std::runtime_error("USERPROFILE is unavailable for the default body network path");
#else
        const char* home=std::getenv("HOME");
        if(!home || !*home) throw std::runtime_error("HOME is unavailable for the default body network path");
#endif
        path=std::string(home)+"/.local/share/gothic-ai/body/nets/p30.bodynet";
        }
      net=std::make_unique<BodyNet>(path);
      } catch(const std::exception& e) {
      error=e.what();
      Tempest::Log::e("[body] network unavailable: ",error);
      }
    }
  };
Network& network() { static Network value; return value; }
const BodyNet& weights() {
  auto& loaded=network();
  if(!loaded.net) throw std::runtime_error("body_net_unavailable");
  return *loaded.net;
  }
std::string encode(const rapidjson::Value& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return {buffer.GetString(),buffer.GetSize()};
  }
bool flag(const rapidjson::Value& row,const char* key) {
  return row.HasMember(key) && row[key].IsBool() && row[key].GetBool();
  }
std::optional<int64_t> hitPoints(const rapidjson::Value& side) {
  return side.HasMember("hp") && side["hp"].IsInt64() ? std::optional<int64_t>(side["hp"].GetInt64()) : std::nullopt;
  }
bool transientAction(const std::string& reason) {
  for(const char* transient:{"stale_observation","expired","expired_wall","command_busy","target_invalid","masked","locked"})
    if(reason==transient) return true;
  return false;
  }
}

void BodyEnginePolicy::initialize() { (void)network(); }

std::string BodyEnginePolicy::netStatusJson() {
  const auto& loaded=network();
  Json result(rapidjson::kObjectType);
  auto& a=result.GetAllocator();
  result.AddMember("loaded",bool(loaded.net),a);
  result.AddMember("checkpoint_sha256",loaded.net ? rapidjson::Value(loaded.net->checkpointSha256().c_str(),a) : rapidjson::Value(),a);
  result.AddMember("error",loaded.error.empty() ? rapidjson::Value() : rapidjson::Value(loaded.error.c_str(),a),a);
  return encode(result);
  }

bool BodyEnginePolicy::requested(const rapidjson::Value& request) {
  return request.HasMember("policy") && request["policy"].IsString() &&
         std::string_view(request["policy"].GetString(),request["policy"].GetStringLength())=="engine";
  }

const char* BodyEnginePolicy::validateAcquire(const rapidjson::Value& request) {
  if(!request.HasMember("policy")) return "allowed";
  if(!requested(request)) return "invalid_policy";
  return network().net ? "allowed" : "body_net_unavailable";
  }

BodyEnginePolicy::BodyEnginePolicy(uint64_t session,uint64_t world,uint64_t entity,uint64_t generation,
                                 uint64_t initialGameMs,uint64_t minimumSequence)
  :runtime(weights(),100,100,true),session(session),world(world),minimumSequence(minimumSequence),
   identity(BodyNet::Identity{session,world,entity,generation}) {
  runtime.beginEpisode(session,world,initialGameMs);
  }

std::string BodyEnginePolicy::process(const rapidjson::Value& row,uint64_t nowGameMs,const Apply& apply) {
  if(!flag(row,"periodic")) return {};
  try {
    const auto current=BodyNet::actorIdentity(row);
    const auto sequence=row["observation_seq"].GetUint64();
    if(current!=identity ||
       sequence<=minimumSequence || sequence<=lastSequence) return {};
    lastSequence=sequence;
    const uint64_t game=row["t_game_ms"].GetUint64();
    const auto& own=row["self"];
    const auto& target=row["target"];
    const auto ownNow=hitPoints(own), targetNow=hitPoints(target);
    // A swing of its own that causes no damage does not end a stall: a blade that cannot
    // reach from this distance would otherwise restart the wait after every miss.
    if(!engagedGameMs || game<*engagedGameMs || flag(target,"attack_anim") ||
       (ownNow && ownHp && *ownNow<*ownHp) || (targetNow && targetHp && *targetNow<*targetHp)) {
      engagedGameMs=game;
      stallAttacks=0;
      }
    if(ownNow) ownHp=ownNow;
    if(targetNow) targetHp=targetNow;
    if(needsReset) {
      runtime.beginEpisode(session,world,game);
      streamSequence=0; needsReset=false;
      }
    const bool lagging=row["sim_step"].GetUint64()<acceptedSimStep || (nowGameMs>game && nowGameMs-game>100);
    const bool eligible=flag(row,"can_decide") && flag(row,"action_mask_valid") && !lagging;
    Json request(rapidjson::kObjectType);
    auto& a=request.GetAllocator();
    request.AddMember("observation",rapidjson::Value(row,a),a);
    request.AddMember("stream_seq",++streamSequence,a);
    request.AddMember("now_game_ms",nowGameMs,a);
    request.AddMember("command_enabled",eligible,a);
    request.AddMember("memory_only",!eligible,a);
    const auto started=Clock::now();
    const auto processed=runtime.process(request);
    if(processed.hiddenReset) ++hiddenResets;
    auto result=processed;
    if(processed.status!="fallback" && eligible)
      result=runtime.arbitrate(row,streamSequence);
    const bool stalled=game-*engagedGameMs>=StallGameMs;
    if(result.status=="decision" && result.actionId && stalled) {
      // The policy's own preference among the active primitives the same mask allows;
      // attacks tried in this stall wait until every allowed attack has been tried.
      uint32_t attacks=0;
      for(int action:{6,7,8})
        if(processed.effectiveActionMask[size_t(action)]) attacks|=1u<<action;
      if(attacks!=0 && (stallAttacks & attacks)==attacks) stallAttacks=0;
      std::optional<int> active;
      for(int action:{1,2,6,7,8})
        if(processed.effectiveActionMask[size_t(action)] && (stallAttacks & (1u<<action))==0 &&
           (!active || processed.logits[size_t(action)]>processed.logits[size_t(*active)]))
          active=action;
      if(active) result.actionId=active;
      }
    stepTimes.push_back(std::chrono::duration<double,std::micro>(Clock::now()-started).count());
    if(result.status=="fallback") {
      ++fallbacks[result.reason];
      if(result.reason=="observation_too_old") {
        if(processed.hiddenReset) needsReset=true;
        if(++transientRejections<10) return {};
        }
      return "policy_fallback:"+result.reason;
      }
    if(!eligible || result.status!="decision") return {};
    if(!result.actionId || *result.actionId<0 || *result.actionId>=12 ||
       !row["action_mask"][rapidjson::SizeType(*result.actionId)].GetBool()) {
      ++fallbacks["policy_error"];
      return "policy_fallback:policy_error";
      }
    ++selectedActions[size_t(*result.actionId)];
    const auto refusal=apply(*result.actionId,row);
    if(!refusal.empty()) {
      ++fallbacks[refusal];
      if(transientAction(refusal) && ++transientRejections<10) return {};
      return "policy_fallback:body_action_rejected";
      }
    transientRejections=0;
    if(stalled && *result.actionId>=6 && *result.actionId<=8)
      stallAttacks|=1u<<*result.actionId;
    acceptedSimStep=row["sim_step"].GetUint64();
    ++decisions;
    ++acceptedActions[size_t(*result.actionId)];
    return {};
  } catch(const std::exception&) {
    ++fallbacks["policy_error"];
    return "policy_fallback:policy_error";
    }
  }

std::string BodyEnginePolicy::report(bool force) {
  const auto now=Clock::now();
  if(!force && now-reportAt<std::chrono::seconds(1)) return {};
  Json result(rapidjson::kObjectType);
  auto& a=result.GetAllocator();
  result.AddMember("type","body_policy",a);
  result.AddMember("decisions",decisions,a);
  rapidjson::Value selected(rapidjson::kObjectType),accepted(rapidjson::kObjectType);
  for(size_t i=0;i<BodyNet::actionCount;++i) {
    selected.AddMember(rapidjson::Value(actionNames[i],a),rapidjson::Value(selectedActions[i]),a);
    accepted.AddMember(rapidjson::Value(actionNames[i],a),rapidjson::Value(acceptedActions[i]),a);
    }
  result.AddMember("selected_actions",selected,a);
  result.AddMember("accepted_actions",accepted,a);
  result.AddMember("hidden_reset",hiddenResets,a);
  rapidjson::Value reasons(rapidjson::kObjectType);
  for(const auto& reason:fallbacks)
    reasons.AddMember(rapidjson::Value(reason.first.c_str(),a),rapidjson::Value(reason.second),a);
  result.AddMember("fallbacks",reasons,a);
  std::sort(stepTimes.begin(),stepTimes.end());
  const auto percentile=[&](double fraction) {
    return stepTimes.empty() ? 0.0 : stepTimes[size_t(std::ceil(double(stepTimes.size())*fraction))-1];
    };
  result.AddMember("p50_us",percentile(0.5),a);
  result.AddMember("p99_us",percentile(0.99),a);
  result.AddMember("steps",uint64_t(stepTimes.size()),a);
  result.AddMember("final",force,a);
  decisions=0; selectedActions.fill(0); acceptedActions.fill(0); hiddenResets=0;
  fallbacks.clear(); stepTimes.clear(); reportAt=now;
  return encode(result);
  }
