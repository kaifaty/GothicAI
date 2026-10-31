#include "game/bodyenginepolicy.h"
#include "game/bodycontroller.h"

#include <rapidjson/document.h>
#include <zlib.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Json=rapidjson::Document;
constexpr const char* actionNames[BodyNet::actionCount]={
  "continue", "approach", "turn_to_target", "strafe_left", "strafe_right", "jump_back",
  "attack_front", "attack_left", "attack_right", "block", "wait_200", "wait_300"
  };
size_t assertions=0,decisions=0;
void expect(bool value,const char* message) {
  ++assertions;
  if(!value) throw std::runtime_error(message);
  }
Json parse(const std::string& text) {
  Json value;
  value.Parse(text.data(),text.size());
  if(value.HasParseError()) throw std::runtime_error("invalid fixture JSON");
  return value;
  }
void expectActions(const rapidjson::Value& actual,const std::array<uint64_t,BodyNet::actionCount>& expected) {
  expect(actual.IsObject() && actual.MemberCount()==BodyNet::actionCount,"action counters contain every network action");
  size_t i=0;
  for(const auto& counter:actual.GetObject()) {
    expect(std::string_view(counter.name.GetString())==actionNames[i],"action counters preserve network action names and order");
    expect(counter.value.GetUint64()==expected[i],"action counters match direct decisions and accepted commands");
    ++i;
    }
  }
Json firstObservation() {
  gzFile input=gzopen(GOTHIC_BODY_ENGINE_GOLDEN,"rb");
  if(!input) throw std::runtime_error("cannot open body engine golden fixture");
  std::string line;
  char buffer[16384];
  while(gzgets(input,buffer,sizeof(buffer))) {
    line+=buffer;
    if(line.back()!='\n') continue;
    auto record=parse(line);
    line.clear();
    if(std::string_view(record["kind"].GetString())!="step") continue;
    Json result;
    result.CopyFrom(record["observation"],result.GetAllocator());
    gzclose(input);
    return result;
    }
  gzclose(input);
  throw std::runtime_error("fixture has no periodic observations");
  }
FightAlgo::ResolverState ready() {
  FightAlgo::ResolverState state;
  state.targetValid=state.eligible=state.sword=true;
  state.focus5=state.focus30=state.inWRange=state.lineClear=true;
  state.hasMove=state.hasLeft=state.hasRight=state.hasBack=true;
  state.hasAttack=state.hasAttackLeft=state.hasAttackRight=state.hasBlock=state.canRotate=true;
  return state;
  }
// Fixture targets may omit observed fields; add the field rather than writing a missing member.
void set(Json& row,const char* side,const char* key,bool value) {
  auto& section=row[side];
  if(section.HasMember(key)) section[key].SetBool(value);
  else section.AddMember(rapidjson::Value(key,row.GetAllocator()),rapidjson::Value(value),row.GetAllocator());
  }
void set(Json& row,const char* side,const char* key,int64_t value) {
  auto& section=row[side];
  if(section.HasMember(key)) section[key].SetInt64(value);
  else section.AddMember(rapidjson::Value(key,row.GetAllocator()),rapidjson::Value(value),row.GetAllocator());
  }
void setMask(Json& row,bool eligible) {
  row["can_decide"].SetBool(eligible);
  row["native_decision_eligible"].SetBool(eligible);
  row["action_mask_valid"].SetBool(true);
  const auto mask=FightAlgo::actionMask(ready());
  for(rapidjson::SizeType i=0;i<12;++i) {
    const bool value=eligible ? mask[i] : i==0;
    row["action_mask"][i].SetBool(value);
    row["availability_mask"][i].SetBool(value);
    }
  }
void parityAndFallback() {
  const auto engine=parse("{\"policy\":\"engine\"}");
  expect(std::string_view(BodyEnginePolicy::validateAcquire(engine))=="allowed","engine acquisition with fixture weights succeeds");
  const auto status=parse(BodyEnginePolicy::netStatusJson());
  expect(status["loaded"].GetBool() && status["checkpoint_sha256"].IsString() && status["error"].IsNull(),"loaded network hello status");
  auto row=firstObservation();
  row["self"]["weapon_state"].SetUint64(2);
  const auto identity=BodyNet::actorIdentity(row);
  const BodyNet directNet(GOTHIC_BODY_ENGINE_NET);
  BodyPolicyRuntime direct(directNet,100,100,true);
  BodyEnginePolicy policy(identity[0],identity[1],identity[2],identity[3],0,0);
  BodyEnginePolicy other(identity[0],identity[1],identity[2],identity[3],0,0);
  expect(policy.report().empty(),"policy telemetry is rate limited before one second");
  direct.beginEpisode(identity[0],identity[1],0);
  uint64_t directSequence=0;
  size_t expectedDecisions=0,otherDecisions=0;
  std::array<uint64_t,BodyNet::actionCount> expectedActions{};
  uint64_t expectedResets=0;
  for(uint64_t tick=1;tick<=240;++tick) {
    row["observation_seq"].SetUint64(tick);
    row["t_game_ms"].SetUint64(tick*50);
    row["sim_step"].SetUint64(tick*3);
    row["self"]["hp_fraction"].SetDouble(0.3+0.001*double(tick));
    // A target swing every two seconds keeps the fight engaged: the stall limit is checked separately.
    set(row,"target","attack_anim",tick%40==0);
    setMask(row,tick%7!=0);
    const auto now=tick*50+(tick%13==0 ? 150 : 0);
    const bool eligible=row["can_decide"].GetBool() && now-tick*50<=100;
    Json request(rapidjson::kObjectType);
    auto& a=request.GetAllocator();
    request.AddMember("observation",rapidjson::Value(row,a),a);
    request.AddMember("stream_seq",++directSequence,a);
    request.AddMember("now_game_ms",now,a);
    request.AddMember("command_enabled",eligible,a);
    request.AddMember("memory_only",!eligible,a);
    const auto processed=direct.process(request);
    if(processed.hiddenReset) ++expectedResets;
    auto expected=processed;
    if(processed.status!="fallback" && eligible) expected=direct.arbitrate(row,directSequence);
    expect(expected.status!="fallback","direct runtime accepts the ordered fixture sequence");
    if(eligible && expected.status=="decision") {
      ++expectedDecisions;
      ++expectedActions[size_t(*expected.actionId)];
      }
    unsigned applied=0;
    const auto release=policy.process(row,now,[&](int action,const rapidjson::Value& observed) {
      ++applied; ++decisions;
      expect(expected.actionId && action==*expected.actionId,"engine policy action matches the direct runtime on the same row");
      BodyController::GateState gate;
      gate.session=identity[0]; gate.world=identity[1]; gate.generation=identity[3];
      gate.goal=gate.target=1; gate.observation=tick; gate.observedGame=gate.gameNow=tick*50;
      gate.targetValid=true; gate.resolver=ready();
      const BodyController::GateCommand command{identity[0],identity[1],identity[3],1,1,tick,
                                                observed["t_game_ms"].GetUint64(),100,FightAlgo::PolicyAction(action)};
      expect(std::string_view(BodyController::validate(gate,command))=="allowed","engine decision passes ordinary body_action validation with ttl 100");
      expect(FightAlgo::resolve(command.action,gate.resolver).allowed,"engine decision resolves through the native primitive gate");
      return std::string{};
      });
    if(!release.empty()) throw std::runtime_error("ordinary, locked and stale-memory rows retain the engine lease: "+release);
    ++assertions;
    expect(applied==(eligible && expected.status=="decision" ? 1u : 0u),"only eligible periodic decisions execute");
    // The other lease shares the weights, with its own identical recurrent history.
    expect(other.process(row,now,[&](int action,const rapidjson::Value&) {
      ++otherDecisions;
      expect(expected.actionId && action==*expected.actionId,"independent lease recurrent state matches");
      return std::string{};
      }).empty(),"second lease retains its independent state");
    expect(policy.process(row,now,[&](int,const rapidjson::Value&) {
      throw std::runtime_error("duplicate observation executed"); return std::string{};
      }).empty(),"duplicate rows are ignored");
    }
  expect(expectedDecisions==decisions && otherDecisions==decisions && decisions>100,"all direct decisions are applied exactly once on both leases");
  auto metrics=parse(policy.report(true));
  expect(metrics["decisions"].GetUint64()==decisions && metrics["steps"].GetUint64()==240,"policy metrics count applied decisions and steps");
  expectActions(metrics["selected_actions"],expectedActions);
  expectActions(metrics["accepted_actions"],expectedActions);
  expect(expectedResets==1 && metrics["hidden_reset"].GetUint64()==expectedResets,"hidden resets match direct runtime including the initial actor row");
  expect(metrics["p50_us"].GetDouble()>0 && metrics["p99_us"].GetDouble()>=metrics["p50_us"].GetDouble(),"policy step percentiles are measured");
  std::cout << "body_policy " << metrics["decisions"].GetUint64() << " decisions p50_us=" << metrics["p50_us"].GetDouble()
            << " p99_us=" << metrics["p99_us"].GetDouble() << '\n';
  metrics=parse(policy.report(true));
  expectActions(metrics["selected_actions"],{});
  expectActions(metrics["accepted_actions"],{});
  expect(metrics["hidden_reset"].GetUint64()==0,"hidden reset counter is drained with each report");
  row["observation_seq"].SetUint64(241);
  row["t_game_ms"].SetUint64(12050);
  row["sample_period_ms"].SetUint64(100);
  const auto release=policy.process(row,12050,[](int,const rapidjson::Value&) { return std::string{}; });
  expect(release=="policy_fallback:unsupported_sampling_rate","policy fallback requests the ordinary native handoff with its exact reason");
  metrics=parse(policy.report(true));
  expect(metrics["final"].GetBool() && metrics["fallbacks"]["unsupported_sampling_rate"].GetUint64()==1,"fallback telemetry immediately reports its refusal reason");
  expectActions(metrics["selected_actions"],{});
  expectActions(metrics["accepted_actions"],{});
  expect(metrics["hidden_reset"].GetUint64()==0,"a fallback without a runtime reset does not count as one");
  row["observation_seq"].SetUint64(242);
  row["t_game_ms"].SetUint64(12100);
  row["sample_period_ms"].SetUint64(50);
  expect(policy.process(row,12100,[](int,const rapidjson::Value&) { return std::string{}; })=="policy_fallback:unexpected_gap","a missing processed row triggers the runtime reset fallback");
  metrics=parse(policy.report(true));
  expect(metrics["hidden_reset"].GetUint64()==1,"a runtime fallback reset is included in interval telemetry");
  }

void stalledPolicyChoosesActivePrimitives() {
  auto row=firstObservation();
  row["self"]["weapon_state"].SetUint64(2);
  set(row,"target","attack_anim",false);
  set(row,"self","hp",int64_t(300));
  set(row,"target","hp",int64_t(300));
  const auto identity=BodyNet::actorIdentity(row);
  const BodyNet directNet(GOTHIC_BODY_ENGINE_NET);
  BodyPolicyRuntime direct(directNet,100,100,true);
  direct.beginEpisode(identity[0],identity[1],0);
  BodyEnginePolicy policy(identity[0],identity[1],identity[2],identity[3],0,0);
  const auto passive=[](int action) { return action==3 || action==4 || action==5 || action>=9; };
  size_t free=0,limited=0,redirected=0,rotated=0;
  uint32_t tried=0;
  for(uint64_t tick=1;tick<=400;++tick) {
    const uint64_t game=tick*50;
    row["observation_seq"].SetUint64(tick);
    row["t_game_ms"].SetUint64(game);
    row["sim_step"].SetUint64(tick);
    // Engaged at the first row, a hero hit at 10 s, then stalled again until the hero swings at 16 s.
    // The NPC's own swings without damage never end a stall.
    if(tick==200) set(row,"target","hp",int64_t(290));
    set(row,"self","attack_anim",tick%3==0);
    set(row,"target","attack_anim",tick==320);
    if(tick==200 || tick==320) tried=0;
    setMask(row,true);
    if(tick>=100 && tick<140) {
      // Only passive primitives are available: the policy's choice stands even when stalled.
      for(rapidjson::SizeType i:{1u,2u,6u,7u,8u}) {
        row["action_mask"][i].SetBool(false);
        row["availability_mask"][i].SetBool(false);
        }
      }
    Json request(rapidjson::kObjectType);
    auto& a=request.GetAllocator();
    request.AddMember("observation",rapidjson::Value(row,a),a);
    request.AddMember("stream_seq",tick,a);
    request.AddMember("now_game_ms",game,a);
    request.AddMember("command_enabled",true,a);
    request.AddMember("memory_only",false,a);
    expect(direct.process(request).status!="fallback","direct runtime accepts the stall sequence");
    const auto expected=direct.arbitrate(row,tick);
    const bool stalled=(tick>=81 && tick<200) || (tick>=280 && tick<320) || tick>=400;
    const bool onlyPassive=tick>=100 && tick<140;
    expect(policy.process(row,game,[&](int action,const rapidjson::Value&) {
      expect(expected.actionId.has_value(),"a direct decision exists for every applied engine action");
      if(!stalled || onlyPassive) {
        expect(action==*expected.actionId,"an engaged fight, or one without active primitives, keeps the policy's choice");
        ++free;
        } else {
        expect(!passive(action),"a stalled fight chooses approach, turn or an attack");
        ++limited;
        if(action!=*expected.actionId) ++redirected;
        if(action>=6 && action<=8) {
          // Every attack the mask allows is tried once before any of them repeats.
          if(tried==((1u<<6)|(1u<<7)|(1u<<8))) tried=0;
          expect((tried & (1u<<action))==0,"a stalled fight tries another attack before repeating one");
          if(tried!=0) ++rotated;
          tried|=1u<<action;
          }
        }
      return std::string{};
      }).empty(),"the stall limit retains the lease");
    }
  expect(free>0 && limited>0 && redirected>0 && rotated>0,"the sequence covers free, limited, redirected and rotated decisions");
  std::cout << "stall free=" << free << " limited=" << limited << " redirected=" << redirected
            << " rotated=" << rotated << '\n';
  }

void transientActionRefusals() {
  auto row=firstObservation();
  row["self"]["weapon_state"].SetUint64(2);
  setMask(row,true);
  const auto identity=BodyNet::actorIdentity(row);
  BodyEnginePolicy policy(identity[0],identity[1],identity[2],identity[3],0,0);
  uint64_t sequence=0;
  std::array<uint64_t,BodyNet::actionCount> selected{},accepted{};
  const auto step=[&](const char* refusal) {
    row["observation_seq"].SetUint64(++sequence);
    row["t_game_ms"].SetUint64(sequence*50);
    row["sim_step"].SetUint64(sequence);
    unsigned applied=0;
    const auto reason=policy.process(row,sequence*50,[&](int action,const rapidjson::Value&) {
      ++applied;
      ++selected[size_t(action)];
      if(!*refusal) ++accepted[size_t(action)];
      return std::string(refusal);
      });
    expect(applied==1,"each retry uses a fresh policy decision");
    return reason;
    };
  for(unsigned i=0;i<9;++i)
    expect(step("command_busy").empty(),"nine transient action refusals retain the lease");
  expect(step("").empty(),"an accepted action breaks the transient refusal streak");
  for(unsigned i=0;i<9;++i)
    expect(step("stale_observation").empty(),"a new refusal streak receives all nine retries");
  expect(step("stale_observation")=="policy_fallback:body_action_rejected","the tenth consecutive action refusal ends the fight");
  const auto metrics=parse(policy.report(true));
  expectActions(metrics["selected_actions"],selected);
  expectActions(metrics["accepted_actions"],accepted);
  expect(metrics["decisions"].GetUint64()==1,"refused selections never increment accepted decisions");
  }
}

int main() {
  try {
    setenv("OPENGOTHIC_BODY_NET",GOTHIC_BODY_ENGINE_NET,1);
    BodyEnginePolicy::initialize();
    parityAndFallback();
    stalledPolicyChoosesActivePrimitives();
    transientActionRefusals();
    std::cout << "{\"type\":\"body_engine_test\",\"passed\":true,\"assertions\":" << assertions
              << ",\"decisions\":" << decisions << "}\n";
    return 0;
    } catch(const std::exception& error) {
    std::cerr << "body-engine-test failure: " << error.what() << '\n';
    return 1;
    }
  }
