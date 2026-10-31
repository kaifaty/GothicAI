#include "cameradefinitions.h"

#include <Tempest/Log>
#include "asset/worldtypes.h"

#include "gothic.h"
#include "worlddata.h"
#include "utils/string_frm.h"

using namespace Tempest;

CameraDefinitions::CameraDefinitions() {
  auto vm = Gothic::inst().createPhoenixVm("Camera.dat");

  vm->enumerate_instances_by_class_name("CCAMSYS", [this, &vm](zenkit::DaedalusSymbol& s) {
    try {
      auto cam = vm->init_instance<Def::Camera>(&s);
      cameras.emplace_back(s.name(), *cam);
      }
    catch(const zenkit::DaedalusScriptError&) {
      // There was an error initializing the Def::Camera. Ignore it.
      }
    });

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
