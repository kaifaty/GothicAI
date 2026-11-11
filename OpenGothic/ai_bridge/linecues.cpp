#include "linecues.h"

#include <Tempest/Log>

#include "graphics/mesh/facerig.h"
#include "jsonread.h"

using namespace Tempest;

void LineCues::readWords(const rapidjson::Value& msg, SpeechCues::Plan& plan) {
  if(!msg.IsObject())
    return;
  const auto it = msg.FindMember("words");
  if(it==msg.MemberEnd() || !it->value.IsArray())
    return;
  for(const auto& w:it->value.GetArray()) {
    if(!w.IsArray() || w.Size()<3 || !w[0].IsInt() || w[0].GetInt()<0 || !w[1].IsNumber() || !w[2].IsNumber())
      continue;
    plan.setWord(size_t(w[0].GetInt()),float(w[1].GetDouble()),float(w[2].GetDouble()));
    }
  }

std::shared_ptr<SpeechCues::Plan> LineCues::read(const rapidjson::Value& msg, const std::string& npc, const std::string& line,
                                                 std::string_view text, bool streamed) {
  if(!msg.IsObject())
    return nullptr;
  auto plan = std::make_shared<SpeechCues::Plan>(text);
  const auto cues = msg.FindMember("cues");
  if(cues!=msg.MemberEnd() && cues->value.IsArray()) {
    for(const auto& c:cues->value.GetArray()) {
      const auto name = JsonRead::str(c,"cue");
      const auto word = JsonRead::integer(c,"word",-1);
      if(name.empty() || word<0 || size_t(word)>=plan->words.size()) {
        Log::e("[ai] say: a gesture without its word: ",line," ",name," ",word);
        continue;
        }
      const auto seed = SpeechCues::seedOf(npc+"/"+line+"/"+std::to_string(plan->cues.size()));
      plan->cues.push_back({std::string(name),uint32_t(word),SpeechCues::sideOf(seed)});
      }
    }
  const auto leak = msg.FindMember("leak");
  if(leak!=msg.MemberEnd() && leak->value.IsObject()) {
    const auto felt = JsonRead::str(leak->value,"felt");
    const auto half = JsonRead::str(leak->value,"half");
    const auto word = JsonRead::integer(leak->value,"word",-1);
    if(!FaceProfile::isFelt(felt) || (half!="upper" && half!="lower") || word<0 || size_t(word)>=plan->words.size()) {
      Log::e("[ai] say: a wrong leak: ",line," ",felt," ",half," ",word);
      } else {
      plan->leak = SpeechCues::Plan::Leak{std::string(felt),half=="lower",uint32_t(word)};
      plan->seed = SpeechCues::seedOf(npc+"/"+line+"/leak");
      }
    }
  if(plan->cues.empty() && !plan->leak.has_value())
    return nullptr;
  plan->friendly = JsonRead::str(msg,"mood")=="friendly";
  plan->streamed = streamed;
  readWords(msg,*plan);
  return plan;
  }
