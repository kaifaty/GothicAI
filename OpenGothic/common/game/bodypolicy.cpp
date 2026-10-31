#include "bodypolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace {
using Value = rapidjson::Value;

constexpr const char* selfKeys[] = {
  "hp_fraction", "weapon_state", "weapon_length_cm", "skill", "skill_talent", "skill_valid",
  "dead", "unconscious", "down", "in_air", "swim", "dive", "attack_anim", "prehit",
  "defence_active", "jumpback_active", "def_window", "par_window", "interruptible", "velocity_valid"
  };
constexpr const char* targetKeys[] = {
  "present", "visible", "current_valid", "yaw_error_rad", "fight_distance_cm", "attack_range_cm",
  "g_range_cm", "closeup_range_cm", "in_w_range", "in_g_range", "in_closeup", "focus_30",
  "focus_5", "melee_line_clear", "hp_fraction", "weapon_state", "attack_anim", "prehit",
  "defence_active", "jumpback_active", "down", "last_known_age_ms"
  };
constexpr const char* gateKeys[] = {
  "fai_wait_remaining_ms", "ani_wait_remaining_ms", "ai_wait_remaining_ms", "output_wait_remaining_ms"
  };
constexpr const char* hiddenTargetKeys[] = {
  "position_world_cm", "relative_position_cm", "linear_velocity_cm_s", "relative_velocity_cm_s",
  "yaw_error_rad", "fight_distance_cm", "attack_range_cm", "g_range_cm", "closeup_range_cm",
  "in_w_range", "in_g_range", "in_closeup", "focus_30", "focus_5", "melee_line_clear", "hp", "hp_max",
  "hp_fraction", "weapon_state", "body_state_raw", "attack_anim", "prehit", "defence_active",
  "jumpback_active", "down"
  };
constexpr const char* actions[] = {
  "continue", "approach", "turn_to_target", "strafe_left", "strafe_right", "jump_back",
  "attack_front", "attack_left", "attack_right", "block", "wait_200", "wait_300"
  };

void require(bool condition, const std::string& reason) {
  if(!condition)
    throw std::runtime_error(reason);
  }

const Value& get(const Value& object, const char* key) {
  static const Value null;
  if(!object.IsObject())
    return null;
  const auto it = object.FindMember(key);
  return it==object.MemberEnd() ? null : it->value;
  }

bool has(const Value& object, const char* key) {
  return object.IsObject() && object.HasMember(key);
  }

const Value& section(const Value& object, const char* key) {
  static const Value empty(rapidjson::kObjectType);
  if(!has(object,key))
    return empty;
  const auto& result = get(object,key);
  require(result.IsObject(), std::string(key)+" must be object");
  return result;
  }

bool text(const Value& value, const char* expected) {
  return value.IsString() && std::string(value.GetString(),value.GetStringLength())==expected;
  }

bool yes(const Value& value) { return value.IsBool() && value.GetBool(); }
bool no(const Value& value) { return value.IsBool() && !value.GetBool(); }

bool truthy(const Value& value) {
  if(value.IsNull()) return false;
  if(value.IsBool()) return value.GetBool();
  if(value.IsNumber()) return value.GetDouble()!=0;
  if(value.IsString()) return value.GetStringLength()!=0;
  if(value.IsArray()) return !value.Empty();
  return value.MemberCount()!=0;
  }

bool equalNumber(const Value& value, double expected) {
  return value.IsNumber() ? value.GetDouble()==expected : value.IsBool() && double(value.GetBool())==expected;
  }

bool sameObservation(const Value& left, const Value& right) {
  // Python's dict/list equality also compares boolean and numeric values.
  // Mask validation happens after this anchor check and still requires booleans.
  if(left.IsBool() && right.IsNumber())
    return double(left.GetBool())==right.GetDouble();
  if(left.IsNumber() && right.IsBool())
    return left.GetDouble()==double(right.GetBool());
  if(left.IsArray() && right.IsArray()) {
    if(left.Size()!=right.Size()) return false;
    for(rapidjson::SizeType i=0; i<left.Size(); ++i)
      if(!sameObservation(left[i],right[i])) return false;
    return true;
    }
  if(left.IsObject() && right.IsObject()) {
    if(left.MemberCount()!=right.MemberCount()) return false;
    for(const auto& member:left.GetObject()) {
      const auto found = right.FindMember(member.name);
      if(found==right.MemberEnd() || !sameObservation(member.value,found->value)) return false;
      }
    return true;
    }
  return left==right;
  }

uint64_t integer(const Value& value, const std::string& name, uint64_t minimum = 0) {
  require(value.IsUint64() && value.GetUint64()>=minimum, "invalid integer "+name);
  return value.GetUint64();
  }

BodyNet::Mask maskValue(const Value& value, const char* name) {
  require(value.IsArray() && value.Size()==BodyNet::actionCount, std::string("invalid ")+name);
  BodyNet::Mask mask{};
  for(size_t i=0; i<mask.size(); ++i) {
    require(value[rapidjson::SizeType(i)].IsBool(), std::string("invalid ")+name);
    mask[i] = value[rapidjson::SizeType(i)].GetBool();
    }
  return mask;
  }

bool any(const BodyNet::Mask& mask, size_t start = 0) {
  return std::any_of(mask.begin()+start,mask.end(),[](bool v) { return v; });
  }

void validateObservation(const Value& row) {
  require(equalNumber(get(row,"schema_version"),1), "unsupported body observation schema");
  BodyNet::actorIdentity(row);
  integer(get(row,"observation_seq"),"observation_seq",1);
  const auto now = integer(get(row,"t_game_ms"),"t_game_ms");
  const auto simStep = integer(get(row,"sim_step"),"observation sim_step");
  integer(get(row,"dt_game_ms"),"dt_game_ms");
  const auto& phase = get(row,"phase");
  require(text(phase,"post_simulation_post_animation") || text(phase,"pre_native_primitive"), "unknown observation phase");
  require(!has(row,"debug_oracle") && !has(row,"oracle"), "oracle mixed into actor observation");
  for(const auto* key : {"can_decide", "native_decision_eligible", "action_mask_valid"})
    require(get(row,key).IsBool(), std::string("observation missing boolean ")+key);
  const auto availability = maskValue(get(row,"availability_mask"),"availability mask");
  const auto& action = get(row,"action_mask");
  if(!action.IsNull() || yes(get(row,"action_mask_valid")))
    maskValue(action,"observation action mask");
  if(yes(get(row,"action_mask_valid")))
    require(maskValue(action,"observation action mask")==availability && text(get(row,"mask_kind"),"shared_melee_resolver"),
            "shared observation masks differ");
  require(!yes(get(row,"can_decide")) || yes(get(row,"action_mask_valid")), "eligible observation lacks action mask");
  const auto& target = section(row,"target");
  if(no(get(target,"current_valid")))
    for(const auto* key : hiddenTargetKeys)
      require(get(target,key).IsNull(), std::string("hidden target has current feature ")+key);
  const auto& lastSeen = get(target,"last_seen_game_ms");
  require(lastSeen.IsNull() || integer(lastSeen,"last_seen_game_ms")<=now, "future target feature last_seen_game_ms");
  const auto& last = section(row,"last_result");
  if(truthy(get(last,"valid"))) {
    require(integer(get(last,"source_game_ms"),"result source time")<=now, "future result in observation");
    require(integer(get(last,"source_sim_step"),"result source step")<=simStep, "future result step in observation");
    }
  if(has(row,"events")) {
    const auto& events = get(row,"events");
    require(events.IsArray(), "events must be array");
    for(const auto& event : events.GetArray()) {
      require(integer(get(event,"source_game_ms"),"event source time")<=now, "future event in observation");
      require(integer(get(event,"source_sim_step"),"event source step")<=simStep, "future event step in observation");
      }
    }
  }

SmallNet::Linear layer(const SmallNet::TensorFile& file, const std::string& name, size_t inputs, size_t outputs,
                       const char* weightSuffix = ".weight", const char* biasSuffix = ".bias") {
  SmallNet::Linear result;
  result.inputs = inputs;
  result.outputs = outputs;
  result.weight = file.tensor(name+weightSuffix,{outputs,inputs}).values;
  result.bias = file.tensor(name+biasSuffix,{outputs}).values;
  return result;
  }
}

std::vector<std::string> BodyNet::featureNames() {
  std::vector<std::string> names;
  const auto add = [&](const std::string& name) {
    names.push_back(name+".valid");
    names.push_back(name);
    };
  add("dt_game_ms");
  for(const auto* key : selfKeys) add(std::string("self.")+key);
  for(const auto* key : targetKeys) add(std::string("target.")+key);
  for(const auto* key : {"self.linear_velocity_cm_s", "target.relative_position_cm", "target.relative_velocity_cm_s"})
    for(size_t axis=0; axis<3; ++axis) add(std::string(key)+"."+std::to_string(axis));
  for(const auto* key : gateKeys) add(std::string("self.gates.")+key);
  for(const auto* key : {"native_decision_eligible", "goal.native_combat", "goal.native_idle", "goal.move_hint"}) add(key);
  for(size_t i=0; i<16; ++i) {
    add("ray."+std::to_string(i)+".distance_cm");
    add("ray."+std::to_string(i)+".hit");
    }
  return names;
  }

BodyNet::BodyNet(const std::string& path) {
  SmallNet::TensorFile file(path);
  const auto& h = file.header;
  require(text(get(h,"format"),"gothic-body-net") && get(h,"version").IsUint64() && get(h,"version").GetUint64()==1,
          "unsupported bodynet format/version");
  require(text(get(h,"byte_order"),"little"), "unsupported bodynet byte order");
  require(text(get(h,"offset_origin"),"first byte after JSON header newline"), "unsupported bodynet offset origin");
  const auto& order = get(h,"actions");
  require(order.IsArray() && order.Size()==actionCount, "bodynet action order mismatch");
  for(size_t i=0; i<actionCount; ++i)
    require(text(order[rapidjson::SizeType(i)],actions[i]), "bodynet action order mismatch");
  require(text(get(h,"feature_version"),"body-periodic-numeric-v1"), "unsupported bodynet feature version");
  const auto names = featureNames();
  const auto& headerNames = get(h,"feature_names");
  require(names.size()==featureCount && headerNames.IsArray() && headerNames.Size()==names.size(), "bodynet feature order mismatch");
  for(size_t i=0; i<names.size(); ++i)
    require(text(headerNames[rapidjson::SizeType(i)],names[i].c_str()), "bodynet feature order mismatch");
  const auto& sha = get(h,"checkpoint_sha256");
  require(sha.IsString() && sha.GetStringLength()==64, "invalid bodynet checkpoint SHA256");
  checkpoint.assign(sha.GetString(),sha.GetStringLength());
  require(std::all_of(checkpoint.begin(),checkpoint.end(),[](char c) { return (c>='0' && c<='9') || (c>='a' && c<='f'); }),
          "invalid bodynet checkpoint SHA256");
  require(file.tensors.size()==12, "bodynet actor tensor names mismatch");
  const auto enc = file.tensors.find("encoder.0.bias"), hd = file.tensors.find("head.0.bias");
  require(enc!=file.tensors.end() && hd!=file.tensors.end(), "bodynet encoder/head tensors missing");
  const size_t encoderSize = enc->second.values.size(), headSize = hd->second.values.size();
  encoder = layer(file,"encoder.0",featureCount,encoderSize);
  gruInput = layer(file,"memory",encoderSize,3*hiddenSize,".weight_ih",".bias_ih");
  gruHidden = layer(file,"memory",hiddenSize,3*hiddenSize,".weight_hh",".bias_hh");
  head = layer(file,"head.0",hiddenSize,headSize);
  output = layer(file,"head.2",headSize,actionCount);
  const auto& means = file.tensor("normalizer.mean",{featureCount}).values;
  const auto& scales = file.tensor("normalizer.std",{featureCount}).values;
  require(std::all_of(scales.begin(),scales.end(),[](float v) { return v>0; }), "bodynet normalizer std must be positive");
  std::copy(means.begin(),means.end(),mean.begin());
  std::copy(scales.begin(),scales.end(),scale.begin());
  }

BodyNet::Identity BodyNet::actorIdentity(const Value& row) {
  Identity result{};
  const char* keys[] = {"session_epoch", "world_epoch", "entity_id", "control_generation"};
  for(size_t i=0; i<result.size(); ++i)
    result[i] = integer(get(row,keys[i]),"observation identity");
  return result;
  }

BodyNet::Mask BodyNet::actionMask(const Value& row) {
  auto mask = maskValue(get(row,"availability_mask"),"policy availability mask");
  require(get(row,"native_decision_eligible").IsBool(), "policy physical eligibility missing");
  if(!any(mask))
    mask[0] = true;
  else if(yes(get(row,"native_decision_eligible")) && any(mask,1))
    mask[0] = false;
  return mask;
  }

BodyNet::Features BodyNet::packFeatures(const Value& row, uint64_t sequenceDtGameMs) const {
  validateObservation(row);
  require(text(get(row,"phase"),"post_simulation_post_animation") && yes(get(row,"periodic")), "model memory accepts periodic snapshots only");
  std::array<double,featureCount> raw{};
  size_t index = 0;
  const auto addNumber = [&](double value, bool valid = true) {
    require(std::isfinite(value), "nonfinite/nonnumeric feature");
    raw.at(index++) = valid ? 1.0 : 0.0;
    raw.at(index++) = value;
    };
  const auto add = [&](const Value& value) {
    if(value.IsNull()) addNumber(0,false);
    else if(value.IsBool()) addNumber(value.GetBool() ? 1 : 0);
    else {
      require(value.IsNumber(), "nonfinite/nonnumeric feature");
      addNumber(value.GetDouble());
      }
    };
  addNumber(double(sequenceDtGameMs));
  const auto& own = section(row,"self");
  const auto& target = section(row,"target");
  for(const auto* key : selfKeys) add(get(own,key));
  for(const auto* key : targetKeys) add(get(target,key));
  const Value* sections[] = {&own,&target,&target};
  const char* vectorKeys[] = {"linear_velocity_cm_s", "relative_position_cm", "relative_velocity_cm_s"};
  for(size_t v=0; v<3; ++v) {
    const auto& vector = get(*sections[v],vectorKeys[v]);
    require(vector.IsNull() || (vector.IsArray() && vector.Size()==3), "invalid body feature vector");
    for(rapidjson::SizeType axis=0; axis<3; ++axis)
      add(vector.IsNull() ? vector : vector[axis]);
    }
  const auto& gates = section(own,"gates");
  for(const auto* key : gateKeys) add(get(gates,key));
  add(get(row,"native_decision_eligible"));
  const auto& goal = section(row,"goal");
  const auto& kind = get(goal,"kind");
  addNumber(text(kind,"combat") || text(kind,"native_combat") ? 1 : 0);
  addNumber(text(kind,"idle") || text(kind,"native_idle") ? 1 : 0);
  add(get(goal,"move_hint"));
  const auto& obstacles = section(row,"local_obstacles");
  const auto& rays = get(obstacles,"rays");
  require(!has(obstacles,"rays") || (rays.IsArray() && (rays.Size()==0 || rays.Size()==16)), "local obstacle ray count must be 0 or 16");
  for(rapidjson::SizeType i=0; i<16; ++i) {
    if(!rays.IsArray() || rays.Empty()) {
      addNumber(0,false);
      addNumber(0,false);
      }
    else {
      require(rays[i].IsObject(), "obstacle ray must be object");
      add(get(rays[i],"distance_cm"));
      add(get(rays[i],"hit"));
      }
    }
  require(index==featureCount, "body feature count mismatch");
  Features result{};
  for(size_t i=0; i<featureCount; ++i) {
    const double normalized = (raw[i]-double(mean[i]))/double(scale[i]);
    require(std::isfinite(normalized), "nonfinite/nonnumeric normalized feature");
    result[i] = float(normalized);
    }
  return result;
  }

BodyNet::Step BodyNet::step(const Features& features, const Mask& mask, const std::vector<float>& state) const {
  require(std::all_of(features.begin(),features.end(),[](float v) { return std::isfinite(v); }), "expected 184 finite body features");
  require(any(mask), "expected nonempty boolean body action mask");
  std::vector<float> hidden = state.empty() ? std::vector<float>(hiddenSize,0) : state;
  require(hidden.size()==hiddenSize && std::all_of(hidden.begin(),hidden.end(),[](float v) { return std::isfinite(v); }),
          "expected 256 finite GRU state values");
  const auto encoded = SmallNet::linear(features.data(),encoder,true);
  Step result;
  result.hidden = SmallNet::gruCell(encoded.data(),hidden,gruInput,gruHidden);
  const auto projected = SmallNet::linear(result.hidden.data(),head,true);
  const auto logits = SmallNet::linear(projected.data(),output);
  for(size_t i=0; i<actionCount; ++i)
    result.logits[i] = mask[i] ? logits[i] : std::numeric_limits<float>::lowest();
  return result;
  }

BodyPolicyRuntime::BodyPolicyRuntime(const BodyNet& network, uint64_t maxAgeGameMs,
                                     uint64_t maxDtGameMs, bool allowStaleMemoryOnly)
  : network(network), maxAgeGameMs(maxAgeGameMs), maxDtGameMs(maxDtGameMs), allowStaleMemoryOnly(allowStaleMemoryOnly) {
  }

void BodyPolicyRuntime::beginEpisode(uint64_t sessionEpoch, uint64_t worldEpoch, uint64_t initial) {
  states.clear();
  active.clear();
  actorClocks.clear();
  expectedEpochs = std::array<uint64_t,2>{sessionEpoch,worldEpoch};
  initialGameMs = initial;
  episodeClosed = false;
  }

BodyPolicyRuntime::Result BodyPolicyRuntime::endEpisode(uint64_t sessionEpoch, uint64_t worldEpoch) {
  Result result;
  if(!expectedEpochs || *expectedEpochs!=std::array<uint64_t,2>{sessionEpoch,worldEpoch}) {
    result.reason = "stale_episode_end";
    return result;
    }
  states.clear();
  active.clear();
  actorClocks.clear();
  expectedEpochs.reset();
  initialGameMs.reset();
  episodeClosed = true;
  result.accepted = result.hiddenReset = true;
  result.reason = "episode_ended";
  return result;
  }

void BodyPolicyRuntime::resetActor(const BodyNet::Identity& identity) {
  states.erase(identity);
  }

std::vector<float> BodyPolicyRuntime::hiddenState(const Value& row) const {
  const auto state = states.find(BodyNet::actorIdentity(row));
  if(state==states.end() || state->second.hidden.empty())
    return std::vector<float>(BodyNet::hiddenSize,0);
  return state->second.hidden;
  }

BodyPolicyRuntime::Result BodyPolicyRuntime::process(const Value& request) {
  const auto& row = get(request,"observation");
  require(row.IsObject(), "observation object required");
  const auto identity = BodyNet::actorIdentity(row);
  const uint64_t sequence = integer(get(request,"stream_seq"),"observation sequence",1);
  const uint64_t game = integer(get(row,"t_game_ms"),"game time");
  const uint64_t nativeSequence = integer(get(row,"observation_seq"),"observation sequence");
  uint64_t now = game;
  bool negativeNow = false;
  if(has(request,"now_game_ms")) {
    const auto& supplied = get(request,"now_game_ms");
    require(supplied.IsInt64() || supplied.IsUint64(), "now_game_ms must come from the engine clock");
    negativeNow = supplied.IsInt64() && supplied.GetInt64()<0;
    if(!negativeNow) now = supplied.GetUint64();
    }
  Result base;
  base.observationSeq = nativeSequence;
  base.streamSeq = sequence;
  base.gameMs = game;
  const auto fallback = [&](const char* reason, bool reset = false) {
    if(reset) {
      const auto state = states.find(identity);
      if(state!=states.end()) {
        state->second.hidden.clear();
        state->second.scores.reset();
        }
      }
    Result result = base;
    result.reason = reason;
    result.hiddenReset = reset;
    return result;
    };
  if(episodeClosed)
    return fallback("episode_ended");
  if(!yes(get(row,"periodic")) || !text(get(row,"phase"),"post_simulation_post_animation"))
    return fallback("unsupported_sampling_phase");
  if(!equalNumber(get(row,"sample_period_ms"),50))
    return fallback("unsupported_sampling_rate");
  const std::array<uint64_t,2> epochs{identity[0],identity[1]};
  if(expectedEpochs && epochs!=*expectedEpochs)
    return fallback("stale_epoch");
  const auto previous = active.find(identity[2]);
  if(previous!=active.end() && previous->second!=identity) {
    const auto& old = previous->second;
    if(std::tie(identity[0],identity[1],identity[3])<std::tie(old[0],old[1],old[3]))
      return fallback("stale_generation");
    states.erase(old);
    }
  auto stateIt = states.find(identity);
  if(stateIt==states.end()) {
    stateIt = states.try_emplace(identity).first;
    active[identity[2]] = identity;
    base.hiddenReset = true;
    }
  auto& state = stateIt->second;
  require(sequence!=state.sequence, "NumPy body runtime does not support duplicate stream sequences");
  if(sequence<state.sequence)
    return fallback("stale_sequence");
  std::optional<uint64_t> previousGame;
  const auto clock = actorClocks.find(identity[2]);
  if(clock!=actorClocks.end() && clock->second[0]==identity[0] && clock->second[1]==identity[1])
    previousGame = clock->second[2];
  if((previousGame && game<*previousGame) || negativeNow || now<game)
    return fallback("clock_regression",true);
  std::optional<uint64_t> boundary = initialGameMs;
  if(!boundary && get(request,"initial_game_ms").IsUint64())
    boundary = get(request,"initial_game_ms").GetUint64();
  if(!previousGame && (!boundary || *boundary>game))
    return fallback("missing_reset_time_boundary",true);
  const uint64_t dt = game-(previousGame ? *previousGame : *boundary);
  base.sequenceDtGameMs = dt;
  const bool gap = sequence!=state.sequence+1;
  const bool bucketGap = previousGame && game/50>*previousGame/50+1;
  const bool memoryOnly = yes(get(request,"memory_only"));
  const bool validMemoryOnly = memoryOnly && (no(get(row,"can_decide")) || no(get(request,"command_enabled")));
  const bool checkAge = !(allowStaleMemoryOnly && validMemoryOnly);
  const char* reason = gap || yes(get(request,"gap")) ? "unexpected_gap" :
                       bucketGap ? "unexpected_periodic_gap" :
                       checkAge && now-game>maxAgeGameMs ? "observation_too_old" :
                       previousGame && dt>maxDtGameMs ? "unexpected_time_gap" : nullptr;
  if(reason) {
    state.sequence = sequence;
    state.gameMs = game;
    actorClocks[identity[2]] = {identity[0],identity[1],game};
    state.response = fallback(reason,true);
    return state.response;
    }
  BodyNet::Mask rawMask;
  try {
    rawMask = maskValue(has(row,"action_mask") ? get(row,"action_mask") : get(row,"availability_mask"),"action mask");
    }
  catch(const std::runtime_error&) {
    return fallback("invalid_action_mask",true);
    }
  if(memoryOnly && !no(get(row,"can_decide")) && !no(get(request,"command_enabled")))
    return fallback("invalid_memory_only_context",true);
  if(!any(rawMask) && !memoryOnly)
    return fallback("empty_raw_mask_requires_memory_only",true);
  const bool eligible = yes(get(row,"can_decide")) && yes(get(row,"action_mask_valid")) && !memoryOnly;
  BodyNet::Mask modelMask;
  try {
    modelMask = BodyNet::actionMask(row);
    }
  catch(const std::runtime_error&) {
    return fallback("invalid_action_mask",true);
    }
  if(!any(modelMask))
    return fallback("invalid_action_mask",true);
  if(eligible && !any(modelMask,1))
    return fallback("no_available_primitive",true);
  try {
    const auto features = network.packFeatures(row,dt);
    const auto step = network.step(features,modelMask,state.hidden);
    if(!std::all_of(step.logits.begin(),step.logits.end(),[](float v) { return std::isfinite(v); }))
      return fallback("invalid_logits",true);
    state.hidden = step.hidden;
    state.scores = step.logits;
    size_t action = 0;
    bool found = false;
    for(size_t i=0; i<modelMask.size(); ++i)
      if(modelMask[i] && (!found || step.logits[i]>step.logits[action])) {
        action = i;
        found = true;
        }
    Result result = base;
    result.status = "processed";
    result.reason = eligible && action!=0 ? "decision" : "locked";
    if(eligible && action!=0) result.actionId = int(action);
    result.selectedActionId = int(action);
    result.modelProcessed = true;
    result.features = features;
    result.logits = step.logits;
    result.effectiveActionMask = modelMask;
    result.rawActionMask = rawMask;
    result.memoryOnly = memoryOnly;
    result.newDecision = eligible && action!=0;
    state.sequence = sequence;
    state.gameMs = game;
    state.response = result;
    rapidjson::Document observation;
    observation.CopyFrom(row,observation.GetAllocator());
    state.observation.Swap(observation);
    actorClocks[identity[2]] = {identity[0],identity[1],game};
    return result;
    }
  catch(const std::exception& error) {
    auto result = fallback("backend_error",true);
    result.error = std::string(error.what()).substr(0,512);
    return result;
    }
  }

BodyPolicyRuntime::Result BodyPolicyRuntime::arbitrate(const Value& row, uint64_t streamSeq, uint64_t maxAnchorAgeGameMs) const {
  const auto stateIt = states.find(BodyNet::actorIdentity(row));
  Result result;
  if(stateIt==states.end() || !stateIt->second.scores || stateIt->second.sequence!=streamSeq) {
    result.reason = "no_current_periodic_state";
    return result;
    }
  const auto& state = stateIt->second;
  if(state.response.memoryOnly) {
    result.status = "locked";
    result.reason = "memory_only";
    return result;
    }
  const auto& now = get(row,"t_game_ms");
  if(!now.IsUint64() || now.GetUint64()<state.gameMs || now.GetUint64()-state.gameMs>maxAnchorAgeGameMs) {
    result.reason = "periodic_anchor_too_old";
    return result;
    }
  const auto& nativeSequence = get(row,"observation_seq");
  if(!nativeSequence.IsUint64() || nativeSequence.GetUint64()!=state.response.observationSeq || now.GetUint64()!=state.gameMs ||
     !yes(get(row,"periodic")) || !sameObservation(row,state.observation)) {
    result.reason = "periodic_anchor_mismatch";
    return result;
    }
  if(!yes(get(row,"can_decide")) || !yes(get(row,"action_mask_valid"))) {
    result.status = "locked";
    result.reason = "locked";
    return result;
    }
  try {
    const auto mask = maskValue(get(row,"action_mask"),"arbitration mask");
    require(any(mask,1), "invalid arbitration mask");
    const auto modelMask = BodyNet::actionMask(row);
    size_t action = 0;
    for(size_t i=1; i<modelMask.size(); ++i)
      if(modelMask[i] && (action==0 || (*state.scores)[i]>(*state.scores)[action]))
        action = i;
    require(action!=0, "invalid arbitration mask");
    result.status = "decision";
    result.reason = "decision";
    result.actionId = int(action);
    result.effectiveActionMask = modelMask;
    result.rawActionMask = mask;
    }
  catch(const std::runtime_error&) {
    result.reason = "invalid_arbitration_mask";
    }
  return result;
  }
