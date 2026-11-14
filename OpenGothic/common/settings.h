#pragma once

#include <Tempest/Signal>
#include <Tempest/Size>
#include <memory>
#include <string_view>

#include "commandline.h"

class IniFile;

class Settings final {
  public:
    Settings();
    ~Settings();

    static Settings& inst();

    struct PhysicsFlags {
      bool ragdoll       = false;
      bool questGuard    = false;
      bool livingBodies  = false;
      bool arrowPhysics  = false;
      bool arrowSticking = false;
      bool spellPhysics  = false;
      bool movableVobs   = false;
      bool physicsNoise  = false;
      bool destruction   = false;
      bool buoyancy      = true;
      bool cloth         = true;
      bool itemWake      = true;
      bool characterVirtual = false;
      bool npcCharacterVirtual = false;
      bool monsterCharacterVirtual = false;
      bool meleeBladeShadow = true;
      bool meleePhysicsHits = true;
      bool meleeReactions = true;
      bool meleePosture = true;
      bool meleeStagger = true;
      bool meleeBeastPoise = true;
      bool meleeBackstab = true;
      bool meleeDownTactics = true;
      bool itemHulls = true;
      bool itemDeepestContact = true;
      bool ragdollBirthGuard = true;
      bool meleeHitStop = true;
      bool meleeStamina = true;
      bool meleeParry = true;
      bool meleeParryAi = true;
      bool furnitureParts = true;
      bool placedItems = true;
      bool bodyImpactDamage = true;
      bool meleeDodge = true;
      bool meleeCharge = true;
      bool meleeHealInterrupt = true;
      bool meleeUseInHand = true;
      bool foodOverTime = true;
      bool meleeAiDodge = true;
      bool meleeAiCharge = true;
      bool bodyTraces = true;
      bool blood = true;

      bool allowsMonsterCharacterVirtual(bool down, bool flying) const {
        return monsterCharacterVirtual && !down && !flying;
        }
      };

    struct Options {
      GiMethod doGi              = GiMethod::None;
      uint32_t aaPreset          = 0;
      bool     taa               = true;
      int      motionVectors     = -1;   // -mv 0|1; -1: [INTERNAL] motionVectors decides

      bool     hideFocus         = false;
      float    cameraFov         = 67.5f;
      int      fpsLimit          = 0;
      float    interfaceScale    = 1;
      int      inventoryCellSize = 70;

      Tempest::Size newChapterSize = {800, 600};

      Tempest::Size saveGameImageSize = {0, 0};

      bool     showHealthBar     = true;
      uint8_t  showManaBar       = 2;
      uint8_t  showSwimBar       = 1;
      };

    void loadSystemPackOptions(IniFile& systemPack);
    void setupOptions();
    void load(std::u16string_view systemIni, std::u16string_view userIni);
    void refreshPhysicsFlags();

    static const Options& options() { return inst().opts; }
    // Refreshed by the game on settings changes and before each world tick.
    static const PhysicsFlags& physicsFlags() { return inst().physicsFlagsCache; }
    static bool has(std::string_view sec);
    static int getI(std::string_view sec, std::string_view name);
    static void setI(std::string_view sec, std::string_view name, int val);
    static float getF(std::string_view sec, std::string_view name);
    static void setF(std::string_view sec, std::string_view name, float val);
    static std::string_view getS(std::string_view sec, std::string_view name);
    static void setS(std::string_view sec, std::string_view name, std::string_view val);
    static float soundVolume();
    static void flush();

    Tempest::Signal<void()> onSettingsChanged;

  private:
    Options opts;
    PhysicsFlags physicsFlagsCache;
    std::unique_ptr<IniFile> defaults;
    std::unique_ptr<IniFile> baseIniFile;
    std::unique_ptr<IniFile> iniFile;

    static Settings* instance;
  };
