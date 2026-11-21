#include "activitydefinitions.h"

#include "definitionjson.h"

#include <fstream>
#include <stdexcept>

namespace {
using Json = rapidjson::Value;
using DefinitionJson::require;
using DefinitionJson::at;
using DefinitionJson::string;
using DefinitionJson::bytes;
using DefinitionJson::integer;
ActivityDefinitions::MobActivity mob(const Json& json) {
  using namespace ActivityDefinitions;
  require(json.IsObject() && json.MemberCount()==9,"unexpected parameter field");
  MobActivity a;
  const auto& perceptions=at(json,"perceptions");
  require(perceptions.IsObject() && perceptions.MemberCount()==2 && at(perceptions,"deferred").IsBool() &&
          at(perceptions,"deferred").GetBool(),"perceptions must be a deferred helper");
  a.perceptions=string(at(perceptions,"function"));
  require(!a.perceptions.empty(),"empty perception helper");
  a.walkMode=integer(at(json,"walk_mode"));
  const auto approach=string(at(json,"approach"));
  require(approach=="nearest_waypoint" || approach=="distance","unsupported approach "+approach);
  a.approach=approach=="distance" ? Approach::Distance : Approach::NearestWaypoint;
  a.approachDistance=integer(at(json,"approach_distance"));
  a.scheme=bytes(at(json,"scheme"));
  require(!a.scheme.empty(),"empty object scheme");
  a.busyBodyState=integer(at(json,"busy_body_state"));
  a.useState=integer(at(json,"use_state"));
  a.leaveState=integer(at(json,"leave_state"));
  a.loopResult=integer(at(json,"loop_result"));
  return a;
  }
ActivityDefinitions::PointActivity point(const Json& json) {
  using namespace ActivityDefinitions;
  require(json.IsObject() && json.MemberCount()==13,"unexpected point parameter field");
  PointActivity a;
  a.perceptions=string(at(json,"perceptions"));
  a.walkMode=integer(at(json,"walk_mode"));
  a.approachDistance=integer(at(json,"approach_distance"));
  a.point=bytes(at(json,"point"));
  a.occupied=string(at(json,"occupied"));
  require(!a.perceptions.empty() && !a.point.empty() && a.occupied=="align_waypoint","unsupported point approach");
  a.enterAnimation=bytes(at(json,"enter_animation"));
  a.leaveAnimation=bytes(at(json,"leave_animation"));
  a.timerSeconds=integer(at(json,"timer_seconds"));
  a.rollRange=integer(at(json,"roll_range"));
  a.loopResult=integer(at(json,"loop_result"));
  require(string(at(json,"approach"))=="distance","unsupported point approach");
  const auto& stages=at(json,"aivar_19");
  require(stages.IsObject() && stages.MemberCount()==4,"unexpected stage field");
  a.initialStage=integer(at(stages,"initial"));
  a.walkingStage=integer(at(stages,"walking"));
  a.arrivedStage=integer(at(stages,"arrived"));
  a.poseStage=integer(at(stages,"pose"));
  const auto& gestures=at(json,"gestures");
  require(a.rollRange>0 && gestures.IsArray() && gestures.Size()==uint32_t(a.rollRange),"invalid gesture range");
  for(const auto& gesture:gestures.GetArray()) {
    require(gesture.IsObject() && gesture.MemberCount()==2 && at(gesture,"reset_timer").IsBool(),"invalid gesture");
    a.gestures.push_back({bytes(at(gesture,"animation")),at(gesture,"reset_timer").GetBool()});
    }
  return a;
  }
}

ActivityDefinitions::Library ActivityDefinitions::read(const std::filesystem::path& base, std::string_view programSha256) {
  const auto file=base/"definitions"/programSha256/"activities.json";
  try {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot open file");
    const std::string text{std::istreambuf_iterator<char>(input),{}};
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    require(!doc.HasParseError(),"invalid JSON");
    require(string(at(doc,"format"))=="gothic-activity-definitions","unsupported format");
    const auto& version=at(doc,"version");
    require(version.IsInt() && version.GetInt()==2,"unsupported version");
    require(!string(at(doc,"program_name")).empty(),"empty program name");
    require(string(at(doc,"program_sha256"))==programSha256,"program SHA differs");
    require(string(at(doc,"string_encoding"))=="unicode-byte-mapping","unsupported string encoding");
    const auto& records=at(doc,"records");
    require(records.IsArray(),"records must be an array");
    require(doc.MemberCount()==6,"unexpected envelope field");
    Library result;
    for(const auto& record:records.GetArray()) {
      const auto name=string(at(record,"symbol"));
      try {
        require(name.starts_with("ZS_") && !result.activities.contains(name) && !result.points.contains(name) && !result.uncaptured.contains(name),
                "duplicate or invalid state symbol");
        const auto status=string(at(record,"status"));
        if(status=="uncaptured") {
          require(record.MemberCount()==4,"unexpected uncaptured activity field");
          result.uncaptured.emplace(name,string(at(record,"reason"))+": "+bytes(at(record,"detail")));
          continue;
          }
        require(status=="captured","unsupported capture status");
        require(record.MemberCount()==4,"unexpected captured activity field");
        const auto type=string(at(record,"template"));
        require(type=="mob" || type=="point","unsupported activity template");
        if(type=="point") result.points.emplace(name,point(at(record,"parameters")));
        else result.activities.emplace(name,mob(at(record,"parameters")));
        }
      catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
      }
    return result;
    }
  catch(const std::exception& e) { throw std::runtime_error("activity definitions: "+file.string()+": "+e.what()); }
  }
