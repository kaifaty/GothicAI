#include "settings.h"

#include <algorithm>
#include <cassert>

#include "resources.h"
#include "utils/inifile.h"

Settings* Settings::instance = nullptr;

Settings::Settings() {
  instance = this;
  }

Settings::~Settings() {
  instance = nullptr;
  }

Settings& Settings::inst() {
  assert(instance!=nullptr);
  return *instance;
  }

void Settings::loadSystemPackOptions(IniFile& systemPack) {
  opts.hideFocus = systemPack.getI("PARAMETERS","HideFocus");
  opts.cameraFov = systemPack.getF("PARAMETERS","VerticalFOV");
  opts.fpsLimit  = std::max(0, systemPack.getI("PARAMETERS", "FPS_Limit", 0));
  if(opts.cameraFov<1.f) {
    opts.cameraFov = 67.5;
    }
  opts.interfaceScale = systemPack.getF("INTERFACE","Scale",1);
  if(opts.interfaceScale<=0) {
    opts.interfaceScale = 1;
    }
  opts.inventoryCellSize = systemPack.getI("INTERFACE","InventoryCellSize",opts.inventoryCellSize);
  opts.inventoryCellSize = std::max(opts.inventoryCellSize, 10);

  opts.newChapterSize.w    = systemPack.getI("INTERFACE","NewChapterSizeX",opts.newChapterSize.w);
  opts.newChapterSize.h    = systemPack.getI("INTERFACE","NewChapterSizeY",opts.newChapterSize.h);

  opts.saveGameImageSize.w = systemPack.getI("INTERFACE","SaveGameImageSizeX",opts.saveGameImageSize.w);
  opts.saveGameImageSize.h = systemPack.getI("INTERFACE","SaveGameImageSizeY",opts.saveGameImageSize.h);

  opts.showHealthBar     = !(systemPack.getI("INTERFACE","HideHealthBar",0)!=0);
  opts.showManaBar       = uint8_t(std::clamp(systemPack.getI("INTERFACE","ShowManaBar",1), 0, 2));
  opts.showSwimBar       = uint8_t(std::clamp(systemPack.getI("INTERFACE","ShowSwimBar",1), 0, 2));
  }

void Settings::setupOptions() {
  if(Resources::hasGraphicsDevice() && Resources::device().properties().raytracing.rayQuery) {
    opts.doGi = CommandLine::inst().isRtGi();
    // GI probes by default on a discrete GPU: ≈0.7 ms on an RTX 3080, an M1 is at 99 % with them
    // (reports/render-features/2026-10-18); the renderer drops them without -rt
    if(!CommandLine::inst().hasRtGiArg() && Resources::device().properties().type==Tempest::DeviceType::Discrete)
      opts.doGi = GiMethod::Probes;
    }

  opts.aaPreset = CommandLine::inst().aaPreset();
  opts.taa      = CommandLine::inst().doTaa();
  opts.motionVectors = CommandLine::inst().motionVectors();
  }

void Settings::load(std::u16string_view systemIni, std::u16string_view userIni) {
  baseIniFile.reset(new IniFile(systemIni));
  iniFile    .reset(new IniFile(userIni));
  // 75 % where MetalFX upscales it (as sharp as full resolution, +15-20 FPS on an M1 Pro), full resolution elsewhere
  const int vidResDefault = (Resources::hasGraphicsDevice() && Resources::device().properties().spatialUpscale) ? 1 : 0;
  if(!iniFile->has("INTERNAL", "vidResIndex")) {
    iniFile->set("INTERNAL", "vidResIndex", vidResDefault);
    }
  {
  defaults.reset(new IniFile());
  defaults->set("GAME", "enableMouse",         1);
  defaults->set("GAME", "mouseSensitivity",    0.5f);
  defaults->set("GAME", "invCatOrder",         "COMBAT,POTION,FOOD,ARMOR,MAGIC,RUNE,DOCS,OTHER,NONE");
  defaults->set("GAME", "invMaxColumns",       5);
  defaults->set("GAME", "animatedWindows",     1);
  defaults->set("GAME", "useGothic1Controls",  1);
  defaults->set("GAME", "highlightMeleeFocus", 0);
  defaults->set("GAME", "useQuickSaveKeys",    1);
  defaults->set("GAME", "bowDrawByStrength",   1); // strength sets a bow's draw speed and limit
  defaults->set("GAME", "npcManualAim",        1); // npc archers aim by hand too (game/archerai)
  defaults->set("GAME", "ragdoll",             1); // physical death/KO bodies; GAME/ragdoll=0 keeps animation-only behavior
  defaults->set("GAME", "ragdoll_quest_guard", 1);
  defaults->set("GAME", "living_bodies",       1);
  defaults->set("GAME", "arrow_physics",       1);
  defaults->set("GAME", "arrow_sticking",      1);
  defaults->set("GAME", "spell_physics",       1);
  defaults->set("GAME", "body_impact_damage",  1);
  defaults->set("GAME", "body_traces",         1); // dead people as physical traces; bodies leave after 3 game days unseen
  defaults->set("GAME", "blood",               1); // blood stains in the world, aging on game time; evidence as `blood` traces
  defaults->set("GAME", "movable_vobs",        1);
  defaults->set("GAME", "item_hulls",          1);
  defaults->set("GAME", "item_deepest_contact",1);
  defaults->set("GAME", "ragdoll_birth_guard",1);
  defaults->set("GAME", "furniture_parts",     1);
  defaults->set("GAME", "physics_noise",       1);
  defaults->set("GAME", "destruction",         1);
  defaults->set("GAME", "buoyancy",            1);
  defaults->set("GAME", "cloth",               1);
  defaults->set("GAME", "item_wake",           1);
  defaults->set("GAME", "placed_items",        1);
  defaults->set("GAME", "static_collision_split",1);
  defaults->set("GAME", "character_virtual",   1);
  defaults->set("GAME", "npc_character_virtual",1);
  defaults->set("GAME", "monster_character_virtual",1);
  defaults->set("GAME", "melee_blade_shadow",    1);
  defaults->set("GAME", "melee_physics_hits",    1);
  defaults->set("GAME", "melee_reactions",       1);
  defaults->set("GAME", "melee_posture",         1);
  defaults->set("GAME", "melee_stagger",         1);
  defaults->set("GAME", "melee_beast_poise",     1);
  defaults->set("GAME", "melee_backstab",        1);
  defaults->set("GAME", "melee_down_tactics",    1);
  defaults->set("GAME", "melee_hitstop",         1);
  defaults->set("GAME", "melee_stamina",         1);
  defaults->set("GAME", "melee_parry",           1);
  defaults->set("GAME", "melee_parry_ai",        1);
  defaults->set("GAME", "melee_dodge",           1);
  defaults->set("GAME", "melee_barge",           1);
  defaults->set("GAME", "melee_charge",          1);
  defaults->set("GAME", "melee_riposte",         1);
  defaults->set("GAME", "melee_heal_interrupt",  1);
  defaults->set("GAME", "melee_use_in_hand",     1);
  defaults->set("GAME", "food_over_time",        1);
  defaults->set("GAME", "melee_ai_dodge",        1);
  defaults->set("GAME", "melee_ai_riposte",      1);
  defaults->set("GAME", "melee_ai_charge",       1);
  defaults->set("GAME", "melee_ai_barge",        1);

  defaults->set("GAME", "animatedWindows",     1);
  defaults->set("GAME", "subTitles",           1);
  defaults->set("GAME", "subTitlesPlayer",     1);

  // switch related language options
  defaults->set("GAME", "language", -1);
  defaults->set("GAME", "voice",    -1);
  defaults->set("GAME", "scaleVideos", 1);

  defaults->set("SKY_OUTDOOR", "zSunName",   "unsun5.tga");
  defaults->set("SKY_OUTDOOR", "zSunSize",   200);
  defaults->set("SKY_OUTDOOR", "zSunAlpha",  230);
  defaults->set("SKY_OUTDOOR", "zMoonName",  "moon.tga");
  defaults->set("SKY_OUTDOOR", "zMoonSize",  400);
  defaults->set("SKY_OUTDOOR", "zMoonAlpha", 255);

  defaults->set("RENDERER_D3D", "zFogRadial", 1); // sunshafts
  defaults->set("ENGINE",       "zEnvMappingEnabled", 1); // reflections
  defaults->set("ENGINE",       "zCloudShadowScale", Resources::hasGraphicsDevice() &&
                Resources::device().properties().type==Tempest::DeviceType::Discrete); // ssao
  defaults->set("INTERNAL",     "vidResIndex", vidResDefault);
  defaults->set("INTERNAL",     "halfResAoFog", 1); // SSAO and fog shadows at half resolution
  defaults->set("INTERNAL",     "spatialUpscale", 1); // MetalFX for reduced resolution, where supported
  // temporal AA by the camera (Renderer::setupSettings: also -taa 0 and OPENGOTHIC_TAA) and its sharpening, 0..1
  defaults->set("INTERNAL",     "taa", 1);
  // motion vectors of objects into the G-buffer (graphics plan 1b), on since the user accepted the clips
  // (2026-11-10): -mv 0, [INTERNAL] motionVectors=0 or OPENGOTHIC_MOTION_VECTORS=0 turn them off
  // (Renderer::setupSettings)
  defaults->set("INTERNAL",     "motionVectors", 1);
  // the TAA reactive mask of particles and animated translucent (graphics plan 1v), on by default since the user
  // accepted the clips (2026-11-10): [INTERNAL] taaReactive=0 or OPENGOTHIC_TAA_REACTIVE=0 turn it off
  // (Renderer::setupSettings)
  defaults->set("INTERNAL",     "taaReactive", 1);
  defaults->set("INTERNAL",     "taaSharpness", 0.75f);
  // VSM lamp shadows: the soft PCSS, or 0 hard in one gather (Renderer::setupSettings); hard on Metal, where the PCSS
  // costs ~11 ms GPU indoors on an M1 Pro
#if defined(__APPLE__)
  defaults->set("INTERNAL",     "vsmLampSoft", 0);
#else
  defaults->set("INTERNAL",     "vsmLampSoft", 1);
#endif
  // VSM: how many lamps cast a shadow, the most important near the camera; 0 every lamp (Renderer::setupSettings).
  // On Metal the nearest few: the lamps' page drawing is VSM's night cost on an M1 Pro
#if defined(__APPLE__)
  defaults->set("INTERNAL",     "vsmLampShadows", 3);
#else
  defaults->set("INTERNAL",     "vsmLampShadows", 0);
#endif

  defaults->set("VIDEO", "zVidBrightness", 0.5f);
  defaults->set("VIDEO", "zVidContrast",   0.5f);
  defaults->set("VIDEO", "zVidGamma",      0.5f);

  defaults->set("SOUND", "musicEnabled",  1);
  defaults->set("SOUND", "musicVolume",   0.5f);
  defaults->set("SOUND", "soundEnabled",  1);
  defaults->set("SOUND", "soundVolume",   0.5f);
  // the world's own sound objects (zCVobSound: a place's loops and stingers); 0 for the shorts, which lay a bed
  defaults->set("SOUND", "worldSounds",   1);

  //defaults->set("ENGINE", "zEnvMappingEnabled", 0);
  //defaults->set("ENGINE", "zCloudShadowScale",  0);
  defaults->set("ENGINE", "zWindEnabled",       1);
  defaults->set("ENGINE", "zWindCycleTime",     4);
  defaults->set("ENGINE", "zWindCycleTimeVar",  6);

  defaults->set("KEYS", "keyEnd",         "0100");
  defaults->set("KEYS", "keyHeal",        "2300");
  defaults->set("KEYS", "keyPotion",      "1900");
  defaults->set("KEYS", "keyLockTarget",  "4f00");
  defaults->set("KEYS", "keyParade",      "cf000d02");
  defaults->set("KEYS", "keyActionRight", "d100");
  defaults->set("KEYS", "keyActionLeft",  "d300");
  defaults->set("KEYS", "keyUp",          "c8001100");
  defaults->set("KEYS", "keyDown",        "d0001f00");
  defaults->set("KEYS", "keyLeft",        "cb001000");
  defaults->set("KEYS", "keyRight",       "cd001200");
  defaults->set("KEYS", "keyStrafeLeft",  "d3001e00");
  defaults->set("KEYS", "keyStrafeRight", "d1002000");
  defaults->set("KEYS", "keyAction",      "1d000c02");
  defaults->set("KEYS", "keySlow",        "2a003600");
  defaults->set("KEYS", "keySMove",       "38009d00");
  defaults->set("KEYS", "keyWeapon",      "39000e02");
  defaults->set("KEYS", "keySneak",       "2d00");
  defaults->set("KEYS", "keyLook",        "13005200");
  defaults->set("KEYS", "keyLookFP",      "21005300");
  defaults->set("KEYS", "keyInventory",   "0f000e00");
  defaults->set("KEYS", "keyShowStatus",  "30002e00");
  defaults->set("KEYS", "keyShowLog",     "31002600");
  defaults->set("KEYS", "keyShowMap",     "3200");
  }

  }

bool Settings::has(std::string_view sec) {
  if(instance->iniFile->has(sec))
    return true;
  if(instance->baseIniFile->has(sec))
    return true;
  // no defaults
  return false;
  }

int Settings::getI(std::string_view sec, std::string_view name) {
  if(name.empty())
    return 0;
  if(sec=="GAME" && name=="language")
    return Resources::baseLanguage();
  if(instance->iniFile->has(sec,name))
    return instance->iniFile->getI(sec,name);
  if(instance->baseIniFile->has(sec,name))
    return instance->baseIniFile->getI(sec,name);
  if(instance->defaults->has(sec,name))
    return instance->defaults->getI(sec,name);
  return 0;
  }

void Settings::setI(std::string_view sec, std::string_view name, int val) {
  instance->iniFile->set(sec,name,val);
  instance->onSettingsChanged();
  }

std::string_view Settings::getS(std::string_view sec, std::string_view name) {
  if(name.empty())
    return "";
  if(instance->iniFile->has(sec,name))
    return instance->iniFile->getS(sec,name);
  if(instance->baseIniFile->has(sec,name))
    return instance->baseIniFile->getS(sec,name);
  if(instance->defaults->has(sec,name))
    return instance->defaults->getS(sec,name);
  return "";
  }

void Settings::setS(std::string_view sec, std::string_view name, std::string_view val) {
  instance->iniFile->set(sec,name,val);
  instance->onSettingsChanged();
  }

float Settings::getF(std::string_view sec, std::string_view name) {
  if(name.empty())
    return 0;
  if(instance->iniFile->has(sec,name))
    return instance->iniFile->getF(sec,name);
  if(instance->baseIniFile->has(sec,name))
    return instance->baseIniFile->getF(sec,name);
  if(instance->defaults->has(sec,name))
    return instance->defaults->getF(sec,name);
  return 0;
  }

void Settings::setF(std::string_view sec, std::string_view name, float val) {
  instance->iniFile->set(sec,name,val);
  instance->onSettingsChanged();
  }

float Settings::soundVolume() {
  const bool  soundEnabled = getI("SOUND","soundEnabled")!=0;
  const float soundVolume  = getF("SOUND","soundVolume");

  return soundEnabled ? soundVolume : 0.f;
  }

void Settings::flush() {
  instance->iniFile->flush();
  }

void Settings::refreshPhysicsFlags() {
  physicsFlagsCache = {
    getI("GAME","ragdoll")!=0,
    getI("GAME","ragdoll_quest_guard")!=0,
    getI("GAME","living_bodies")!=0,
    getI("GAME","arrow_physics")!=0,
    getI("GAME","arrow_sticking")!=0,
    getI("GAME","spell_physics")!=0,
    getI("GAME","movable_vobs")!=0,
    getI("GAME","physics_noise")!=0,
    getI("GAME","destruction")!=0,
    getI("GAME","buoyancy")!=0,
    getI("GAME","cloth")!=0,
    getI("GAME","item_wake")!=0,
    getI("GAME","character_virtual")!=0,
    getI("GAME","npc_character_virtual")!=0,
    getI("GAME","monster_character_virtual")!=0,
    getI("GAME","melee_blade_shadow")!=0,
    getI("GAME","melee_physics_hits")!=0,
    getI("GAME","melee_reactions")!=0,
    getI("GAME","melee_posture")!=0,
    getI("GAME","melee_posture")!=0 && getI("GAME","melee_stagger")!=0,
    getI("GAME","melee_posture")!=0 && getI("GAME","melee_stagger")!=0 && getI("GAME","melee_beast_poise")!=0,
    getI("GAME","melee_backstab")!=0,
    getI("GAME","melee_down_tactics")!=0,
    getI("GAME","item_hulls")!=0,
    getI("GAME","item_deepest_contact")!=0,
    getI("GAME","ragdoll_birth_guard")!=0,
    getI("GAME","melee_hitstop")!=0,
    getI("GAME","melee_stamina")!=0,
    getI("GAME","melee_parry")!=0,
    getI("GAME","melee_parry_ai")!=0,
    getI("GAME","furniture_parts")!=0,
    getI("GAME","placed_items")!=0,
    getI("GAME","body_impact_damage")!=0,
    getI("GAME","melee_dodge")!=0,
    getI("GAME","melee_dodge")!=0 && getI("GAME","melee_barge")!=0,
    getI("GAME","melee_charge")!=0,
    getI("GAME","melee_riposte")!=0,
    getI("GAME","melee_heal_interrupt")!=0,
    getI("GAME","melee_use_in_hand")!=0,
    getI("GAME","food_over_time")!=0,
    getI("GAME","melee_ai_dodge")!=0,
    getI("GAME","melee_ai_riposte")!=0,
    getI("GAME","melee_ai_charge")!=0,
    getI("GAME","melee_ai_barge")!=0,
    getI("GAME","body_traces")!=0,
    getI("GAME","blood")!=0
    };
  }
