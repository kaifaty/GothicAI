#include "game/bodypolicy.h"

#include <rapidjson/document.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace {
void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

const rapidjson::Value& field(const rapidjson::Value& value, const char* name) {
  require(value.IsObject() && value.HasMember(name), std::string("missing golden field: ")+name);
  return value[name];
  }

uint64_t integer(const rapidjson::Value& value, const char* name) {
  const auto& result = field(value,name);
  require(result.IsUint64(), std::string("invalid golden integer: ")+name);
  return result.GetUint64();
  }

bool flag(const rapidjson::Value& value, const char* name) {
  const auto& result = field(value,name);
  require(result.IsBool(), std::string("invalid golden boolean: ")+name);
  return result.GetBool();
  }

bool textEquals(const rapidjson::Value& value, const std::string& actual) {
  return value.IsString() && actual==value.GetString();
  }

bool actionEquals(const rapidjson::Value& value, std::optional<int> actual) {
  return actual ? value.IsInt() && value.GetInt()==*actual : value.IsNull();
  }

bool timeEquals(const rapidjson::Value& value, std::optional<uint64_t> actual) {
  return actual ? value.IsUint64() && value.GetUint64()==*actual : value.IsNull();
  }

bool maskEquals(const rapidjson::Value& value, const BodyNet::Mask& actual) {
  require(value.IsArray() && value.Size()==actual.size(), "invalid golden action mask");
  for(size_t i=0; i<actual.size(); ++i) {
    const auto& bit = value[rapidjson::SizeType(i)];
    if(!bit.IsBool() || bit.GetBool()!=actual[i]) return false;
    }
  return true;
  }

struct Golden final {
  gzFile stream = nullptr;
  explicit Golden(const char* path):stream(gzopen(path,"rb")) {
    require(stream!=nullptr, std::string("cannot open golden: ")+path);
    }
  ~Golden() { if(stream) gzclose(stream); }
  bool line(std::string& result) {
    result.clear();
    char buffer[16384];
    while(gzgets(stream,buffer,sizeof(buffer))) {
      result += buffer;
      if(!result.empty() && result.back()=='\n')
        return true;
      }
    int code = Z_OK;
    const char* message = gzerror(stream,&code);
    require(code==Z_OK || code==Z_STREAM_END, std::string("cannot read golden: ")+message);
    return !result.empty();
    }
  };

template<class Values>
bool vectorEquals(const rapidjson::Value& expected, const Values& actual, double tolerance, double& maximum) {
  if(expected.IsNull())
    return false;
  require(expected.IsArray() && expected.Size()==actual.size(), "invalid golden vector length");
  bool same = true;
  for(size_t i=0; i<actual.size(); ++i) {
    const auto& value = expected[rapidjson::SizeType(i)];
    require(value.IsNumber() && std::isfinite(value.GetDouble()), "nonfinite/nonnumeric golden vector");
    const double error = std::abs(value.GetDouble()-double(actual[i]));
    maximum = std::max(maximum,error);
    same = same && std::isfinite(actual[i]) && error<=tolerance;
    }
  return same;
  }

int oneCpu() {
#if defined(__linux__)
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  require(sched_getaffinity(0,sizeof(allowed),&allowed)==0, "cannot read benchmark CPU affinity");
  for(int cpu=0; cpu<CPU_SETSIZE; ++cpu) {
    if(!CPU_ISSET(cpu,&allowed))
      continue;
    cpu_set_t selected;
    CPU_ZERO(&selected);
    CPU_SET(cpu,&selected);
    require(sched_setaffinity(0,sizeof(selected),&selected)==0, "cannot pin benchmark to one CPU");
    return cpu;
    }
  throw std::runtime_error("benchmark has no available CPU");
#else
  // Inference is serial on every platform; Linux additionally pins the CPU.
  return -1;
#endif
  }

double percentile(const std::vector<double>& sorted, double fraction) {
  return sorted[size_t(std::ceil(double(sorted.size())*fraction))-1];
  }

int contract(const char* weights, const char* goldenPath, bool benchmark) {
  const int cpu = benchmark ? oneCpu() : -1;
  const BodyNet network(weights);
  BodyPolicyRuntime runtime(network);
  Golden golden(goldenPath);
  size_t lineNumber = 0, steps = 0, episodes = 0, inferences = 0, decisions = 0, arbitrations = 0, mismatches = 0;
  double maxFeatures = 0, maxLogits = 0, maxHidden = 0;
  bool inEpisode = false;
  std::vector<double> timings;
  std::string line;
  const auto report = [&](const std::vector<std::string>& errors, const BodyPolicyRuntime::Result& result) {
    if(errors.empty()) return;
    ++mismatches;
    if(mismatches<=10) {
      std::cerr<<"golden line "<<lineNumber<<": mismatch in";
      for(const auto& error:errors)
        std::cerr<<' '<<error;
      std::cerr<<" (actual status="<<result.status<<", reason="<<result.reason<<")\n";
      }
    };
  while(golden.line(line)) {
    ++lineNumber;
    rapidjson::Document record;
    record.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(line.data(),line.size());
    require(!record.HasParseError() && record.IsObject(), "invalid golden JSON at line "+std::to_string(lineNumber));
    const auto& kind = field(record,"kind");
    if(textEquals(kind,"reset") || textEquals(kind,"end_episode")) {
      require(textEquals(field(record,"checkpoint_sha256"),network.checkpointSha256()), "golden checkpoint SHA differs from bodynet");
      const bool begin = textEquals(kind,"reset");
      require(begin!=inEpisode, "invalid golden episode boundary");
      if(begin) {
        runtime.beginEpisode(integer(record,"session_epoch"),integer(record,"world_epoch"),integer(record,"initial_game_ms"));
        ++episodes;
        }
      else {
        const auto ended = runtime.endEpisode(integer(record,"session_epoch"),integer(record,"world_epoch"));
        require(ended.accepted, "golden episode end rejected: "+ended.reason);
        }
      inEpisode = begin;
      continue;
      }
    if(textEquals(kind,"arbitrate")) {
      require(inEpisode, "golden arbitration inside an episode required");
      const auto& observation = field(record,"observation");
      const auto result = runtime.arbitrate(observation,integer(record,"stream_seq"),integer(record,"max_anchor_age_game_ms"));
      std::vector<std::string> errors;
      const auto compare = [&](bool same, const char* name) { if(!same) errors.emplace_back(name); };
      compare(textEquals(field(record,"status"),result.status),"status");
      compare(textEquals(field(record,"reason"),result.reason),"reason");
      compare(actionEquals(field(record,"action_id"),result.actionId),"action_id");
      compare(flag(record,"hidden_reset")==result.hiddenReset,"hidden_reset");
      compare(flag(record,"model_processed")==result.modelProcessed,"model_processed");
      if(record.HasMember("effective_action_mask"))
        compare(maskEquals(field(record,"effective_action_mask"),result.effectiveActionMask),"effective_action_mask");
      if(record.HasMember("raw_action_mask"))
        compare(maskEquals(field(record,"raw_action_mask"),result.rawActionMask),"raw_action_mask");
      compare(vectorEquals(field(record,"hidden_state"),runtime.hiddenState(observation),1.e-4,maxHidden),"hidden_state");
      ++arbitrations;
      report(errors,result);
      continue;
      }
    require(textEquals(kind,"step") && inEpisode, "golden step inside an episode required");
    const auto& observation = field(record,"observation");
    const bool eligible = flag(observation,"can_decide") && flag(observation,"action_mask_valid");
    rapidjson::Document request(rapidjson::kObjectType);
    auto& allocator = request.GetAllocator();
    rapidjson::Value row;
    row.CopyFrom(observation,allocator);
    request.AddMember("observation",row,allocator);
    request.AddMember("stream_seq",integer(record,"stream_seq"),allocator);
    rapidjson::Value now;
    now.CopyFrom(field(record,"now_game_ms"),allocator);
    request.AddMember("now_game_ms",now,allocator);
    const auto options = record.FindMember("process_options");
    request.AddMember("command_enabled",options==record.MemberEnd() ? eligible : flag(options->value,"command_enabled"),allocator);
    request.AddMember("memory_only",options==record.MemberEnd() ? !eligible : flag(options->value,"memory_only"),allocator);
    const auto started = std::chrono::steady_clock::now();
    const auto processed = runtime.process(request);
    auto result = processed;
    if(processed.status!="fallback") {
      if(eligible)
        result = runtime.arbitrate(observation,processed.streamSeq);
      else {
        result.status = "locked";
        result.actionId.reset();
        }
      }
    const auto ended = std::chrono::steady_clock::now();
    if(benchmark)
      timings.push_back(std::chrono::duration<double,std::micro>(ended-started).count());
    std::vector<std::string> errors;
    const auto compare = [&](bool same, const char* name) { if(!same) errors.emplace_back(name); };
    compare(textEquals(field(record,"status"),result.status),"status");
    compare(textEquals(field(record,"reason"),result.reason),"reason");
    compare(actionEquals(field(record,"action_id"),result.actionId),"action_id");
    compare(actionEquals(field(record,"selected_action_id"),processed.selectedActionId),"selected_action_id");
    compare(flag(record,"hidden_reset")==processed.hiddenReset,"hidden_reset");
    compare(flag(record,"model_processed")==processed.modelProcessed,"model_processed");
    compare(integer(record,"stream_seq")==processed.streamSeq,"stream_seq");
    compare(timeEquals(field(record,"sequence_dt_game_ms"),processed.sequenceDtGameMs),"sequence_dt_game_ms");
    const auto& resetReason = field(record,"reset_reason");
    compare(processed.hiddenReset ? textEquals(resetReason,processed.status=="fallback" ? processed.reason : "new_identity") : resetReason.IsNull(),"reset_reason");
    if(processed.modelProcessed) {
      compare(vectorEquals(field(record,"features"),processed.features,1.e-5,maxFeatures),"features");
      compare(vectorEquals(field(record,"logits"),processed.logits,1.e-4,maxLogits),"logits");
      ++inferences;
      }
    else {
      compare(field(record,"features").IsNull(),"features");
      compare(field(record,"logits").IsNull(),"logits");
      }
    compare(vectorEquals(field(record,"hidden_state"),runtime.hiddenState(observation),1.e-4,maxHidden),"hidden_state");
    decisions += result.status=="decision";
    ++steps;
    report(errors,result);
    }
  require(!inEpisode && steps>0, "golden stream must contain complete episodes and at least one step");
  std::cout<<std::setprecision(10)<<"{\"steps\":"<<steps<<",\"episodes\":"<<episodes
           <<",\"inferences\":"<<inferences<<",\"decisions\":"<<decisions<<",\"arbitrations\":"<<arbitrations<<",\"mismatches\":"<<mismatches
           <<",\"max_features_error\":"<<maxFeatures<<",\"max_logits_error\":"<<maxLogits
           <<",\"max_hidden_error\":"<<maxHidden<<"}\n";
  if(benchmark) {
    std::sort(timings.begin(),timings.end());
    std::cout<<"{\"benchmark\":\"bodypolicy\",\"steps\":"<<timings.size()<<",\"threads\":1,\"cpu\":"<<cpu
             <<",\"p50_us\":"<<percentile(timings,.50)<<",\"p99_us\":"<<percentile(timings,.99)<<"}\n";
    }
  return mismatches==0 ? 0 : 1;
  }
}

int main(int argc, char** argv) {
  try {
    bool benchmark = false;
    std::vector<const char*> positional;
    for(int i=1; i<argc; ++i) {
      if(std::strcmp(argv[i],"--benchmark")==0)
        benchmark = true;
      else
        positional.push_back(argv[i]);
      }
    require(positional.size()==2, "usage: gothic-bodypolicy-contract BODYNET GOLDEN.jsonl.gz [--benchmark]");
    return contract(positional[0],positional[1],benchmark);
    }
  catch(const std::exception& error) {
    std::cerr<<"bodypolicy contract: "<<error.what()<<'\n';
    return 1;
    }
  }
