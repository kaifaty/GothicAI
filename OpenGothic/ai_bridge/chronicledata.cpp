#include "chronicledata.h"

#include "jsonread.h"
#include "textcodec.h"

#include <utility>

namespace {
std::vector<ChronicleFeat> feats(const rapidjson::Value& object, const char* key) {
  std::vector<ChronicleFeat> result;
  const auto list = object.FindMember(key);
  if(list==object.MemberEnd() || !list->value.IsArray())
    return result;
  for(const auto& value:list->value.GetArray()) {
    if(!value.IsObject())
      continue;
    ChronicleFeat feat;
    feat.id        = JsonRead::str(value,"id");
    feat.title     = TextCodec::fromUtf8(JsonRead::str(value,"title"));
    feat.text      = TextCodec::fromUtf8(JsonRead::str(value,"text"));
    feat.category  = JsonRead::str(value,"category");
    feat.chapter   = JsonRead::integer(value,"chapter",1);
    feat.day       = JsonRead::integer(value,"day",0);
    feat.engraving = JsonRead::str(value,"engraving");
    feat.icon      = JsonRead::str(value,"icon");
    feat.epilogue  = TextCodec::fromUtf8(JsonRead::str(value,"epilogue"));
    result.push_back(std::move(feat));
    }
  return result;
  }

std::vector<ChronicleEnd> ends(const rapidjson::Value& object) {
  std::vector<ChronicleEnd> result;
  const auto list = object.FindMember("ends");
  if(list==object.MemberEnd() || !list->value.IsArray())
    return result;
  for(const auto& value:list->value.GetArray()) {
    if(!value.IsObject())
      continue;
    ChronicleEnd end;
    end.kind    = JsonRead::str(value,"kind");
    end.title   = TextCodec::fromUtf8(JsonRead::str(value,"title"));
    end.text    = TextCodec::fromUtf8(JsonRead::str(value,"text"));
    end.name    = TextCodec::fromUtf8(JsonRead::str(value,"name"));
    end.chapter = JsonRead::integer(value,"chapter",1);
    end.day     = JsonRead::integer(value,"day",0);
    result.push_back(std::move(end));
    }
  return result;
  }
}

ChronicleData parseChronicle(std::string_view json) {
  rapidjson::Document doc;
  doc.Parse(json.data(),json.size());
  if(doc.HasParseError() || !doc.IsObject())
    return {};
  ChronicleData result;
  result.fate = feats(doc,"fate");
  const auto others = doc.FindMember("others");
  if(others==doc.MemberEnd() || !others->value.IsArray())
    return result;
  for(const auto& value:others->value.GetArray()) {
    if(!value.IsObject())
      continue;
    ChronicleFork fork;
    fork.chapter = JsonRead::integer(value,"chapter",1);
    fork.day = JsonRead::integer(value,"day",0);
    fork.feats = feats(value,"feats");
    fork.ends = ends(value);
    result.others.push_back(std::move(fork));
    }
  return result;
  }

FateCard parseFateCard(std::string_view json) {
  rapidjson::Document doc;
  doc.Parse(json.data(),json.size());
  if(doc.HasParseError() || !doc.IsObject())
    return {};
  FateCard result;
  result.off     = JsonRead::boolean(doc,"off",false);
  result.title   = TextCodec::fromUtf8(JsonRead::str(doc,"title"));
  result.text    = TextCodec::fromUtf8(JsonRead::str(doc,"text"));
  result.feats   = feats(doc,"feats");
  result.chapter = JsonRead::integer(doc,"chapter",1);
  result.day     = JsonRead::integer(doc,"day",0);
  result.count   = JsonRead::integer(doc,"count",0);
  return result;
  }
