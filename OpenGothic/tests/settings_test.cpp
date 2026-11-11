#include "settings.h"
#include "utils/inifile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
size_t assertions = 0;

void require(bool value, const char* message) {
  ++assertions;
  if(!value)
    throw std::runtime_error(message);
  }

struct Fixture {
  std::filesystem::path root;

  Fixture() {
    auto pattern = (std::filesystem::temp_directory_path()/"gothic-settings-XXXXXX").string();
    const auto* path = mkdtemp(pattern.data());
    require(path!=nullptr,"create temporary settings directory");
    root = path;
    }

  ~Fixture() {
    std::filesystem::remove_all(root);
    }
  };

struct Changes {
  int count = 0;
  void changed() { ++count; }
  };
}

int main() {
  try {
    Fixture fixture;
    const auto systemIni = fixture.root/"system.ini";
    const auto userIni = fixture.root/"user.ini";
    {
      std::ofstream system(systemIni);
      system << "[GAME]\nenableMouse=0\ninvCatOrder=SYSTEM\nmouseSensitivity=0.25\n"
                "[SOUND]\nsoundVolume=0.75\n"
                "[SYSTEM_ONLY]\nvalue=1\n";
      std::ofstream user(userIni);
      user << "[GAME]\nenableMouse=2\ninvCatOrder=USER\nmouseSensitivity=0.125\n";
      require(bool(system) && bool(user),"write temporary ini files");
    }

    Settings settings;
    settings.load(systemIni.u16string(),userIni.u16string());
    // Gothic owns these refresh points: its settings handler and the start of a world tick.
    settings.onSettingsChanged.bind(&settings,&Settings::refreshPhysicsFlags);
    settings.refreshPhysicsFlags();
    Changes changes;
    settings.onSettingsChanged.bind(&changes,&Changes::changed);

    require(Settings::getI("GAME","enableMouse")==2,"user integer overrides system and defaults");
    require(Settings::getF("GAME","mouseSensitivity")==0.125f,"user float overrides system and defaults");
    require(Settings::getS("GAME","invCatOrder")=="USER","user string overrides system and defaults");
    require(Settings::getF("SOUND","soundVolume")==0.75f,"system overrides default volume");
    require(Settings::getI("GAME","ragdoll")==1,"fall back to defaults");
    require(Settings::has("GAME") && Settings::has("SYSTEM_ONLY"),"has reads user and system sections");
    require(!Settings::has("SKY_OUTDOOR"),"has ignores default sections");
    require(Settings::physicsFlags().meleeStagger && Settings::physicsFlags().meleeBeastPoise,"default composite physics flags");

    Settings::setI("GAME","melee_posture",0);
    require(changes.count==1,"integer setter emits settings change");
    require(!Settings::physicsFlags().meleePosture && !Settings::physicsFlags().meleeStagger &&
            !Settings::physicsFlags().meleeBeastPoise,"posture gates stagger and beast poise");
    Settings::setI("GAME","melee_posture",1);
    Settings::setI("GAME","melee_stagger",0);
    require(Settings::physicsFlags().meleePosture && !Settings::physicsFlags().meleeStagger &&
            !Settings::physicsFlags().meleeBeastPoise,"stagger gates beast poise");
    Settings::setI("GAME","melee_stagger",1);
    Settings::setI("GAME","melee_beast_poise",0);
    require(Settings::physicsFlags().meleeStagger && !Settings::physicsFlags().meleeBeastPoise,"beast poise has its own switch");
    Settings::setI("GAME","melee_beast_poise",1);
    require(Settings::physicsFlags().meleeBeastPoise,"composite flags reenable synchronously");
    Settings::setS("GAME","arrow_physics","0");
    require(changes.count==7 && !Settings::physicsFlags().arrowPhysics,"string setter emits and refreshes physics flags");
    Settings::setF("SOUND","soundVolume",0.25f);
    require(changes.count==8 && Settings::soundVolume()==0.25f,"float setter emits and updates sound volume");
    Settings::setI("SOUND","soundEnabled",0);
    require(Settings::soundVolume()==0.f,"disabled sound has zero volume");
    Settings::flush();
    IniFile persisted(userIni.u16string());
    require(persisted.getI("GAME","melee_stagger")==1 && persisted.getF("SOUND","soundVolume")==0.25f,
            "flush persists user changes");

    // With an empty user file the same keys resolve through the system file.
    settings.load(systemIni.u16string(),(fixture.root/"empty.ini").u16string());
    require(Settings::getI("GAME","enableMouse")==0,"system integer overrides default");
    require(Settings::getF("GAME","mouseSensitivity")==0.25f,"system float overrides default");
    require(Settings::getS("GAME","invCatOrder")=="SYSTEM","system string overrides default");
    require(Settings::getF("VIDEO","zVidGamma")==0.5f && Settings::getS("SKY_OUTDOOR","zSunName")=="unsun5.tga",
            "float and string defaults remain available");
    std::cout << "settings: " << assertions << " assertions passed\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "settings: " << error.what() << '\n';
    return 1;
    }
  }
