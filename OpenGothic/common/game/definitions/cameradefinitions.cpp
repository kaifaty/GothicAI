#include "cameradefinitions.h"
#include "cameradefinitiondata.h"
#include "basescripts.h"
#include <Tempest/Log>
#include "asset/worldtypes.h"

#include "basestore.h"
#include "contentcensus.h"
#include "programdata.h"
#include "resources.h"
#include "worlddata.h"
#include "utils/string_frm.h"

using namespace Tempest;

CameraDefinitions::CameraDefinitions() {
  const auto& index = baseProgramIndex();
  const auto* source = ProgramData::cameraProgram(index);
  if(source==nullptr || source->recordSha256!=CameraDefinitionData::stockProgram)
    throw BaseStore::Error("camera presets require the supported prepared catalog");

  const auto base = index.dir.parent_path();
  const auto section = CameraDefinitionData::section(base,source->recordSha256);
  try {
    cameras = CameraDefinitionData::read(base,source->recordSha256);
    }
  catch(const std::runtime_error& error) {
    throw BaseStore::Error(section.string()+": "+error.what());
    }
  if(cameras.empty())
    throw BaseStore::Error(section.string()+": empty camera catalog");
  if(auto* census = Resources::contentCensus())
    census->replaced("Camera.dat","base/"+section.lexically_relative(base).generic_string());

  camModDialog    = getCam("CAMMODDIALOG");
  camModInventory = getCam("CAMMODINVENTORY");
  camModNormal    = getCam("CAMMODNORMAL");
  camModBack      = getCam("CAMMODLOOKBACK");
  camModFp        = getCam("CAMMODFIRSTPERSON");
  camModDeath     = getCam("CAMMODDEATH");
  camModMelee     = getCam("CAMMODMELEE");
  camModRange     = getCam("CAMMODRANGED");
  camModMage      = getCam("CAMMODMAGIC");
  camModSwim      = getCam("CAMMODSWIM");
  camModDive      = getCam("CAMMODDIVE");
  camModFall      = getCam("CAMMODFALL");

  // dialog presets: unused yet
  std::vector<asset::VCutsceneCamera> cameras;
  try {
    WorldData::ZenFile base;
    if(Resources::loadBaseZen("DialogCams.ZEN","cameras",base)) {
      for(const auto& camera:base.vobs)
        cameras.push_back(*std::dynamic_pointer_cast<asset::VCutsceneCamera>(camera));
      return;
      }
    Log::e("unable to load base dialog cameras");
    }
  catch(...) {
    Log::e("unable to load Zen-file: \"DialogCams.ZEN\"");
    }
  }

const Def::Camera& CameraDefinitions::mobsiCam(std::string_view tag, std::string_view pos) const {
  if(!pos.empty()) {
    string_frm name("CAMMODMOB",tag,'_',pos);
    if(auto* c = find(name))
      return *c;
    }

  string_frm name("CAMMODMOB",tag);
  if(auto* c = find(name))
    return *c;
  if(auto* c = find("CAMMODMOBDEFAULT"))
    return *c;
  return camModNormal;
  }

Def::Camera CameraDefinitions::getCam(std::string_view name) {
  for(auto& i:cameras)
    if(i.first==name)
      return i.second;

  return {};
  }

const Def::Camera* CameraDefinitions::find(std::string_view name) const {
  for(auto& i:cameras)
    if(i.first==name)
      return &i.second;
  return nullptr;
  }
