#pragma once

#include <Tempest/Vec>
#include <Tempest/Signal>
#include <string>
#include <vector>
#include "world/world.h"

class Marvin {
  public:
    Marvin();

    Tempest::Signal<void(std::string_view)> print;

    bool autoComplete(std::string& v);
    bool exec(std::string_view v);

    static bool chunksVisible() { return showChunks; }

  private:
    enum CmdType {
      C_None,
      C_Incomplete,
      C_Extra,
      C_Invalid,

      // gdb-like
      C_PrintVar,
      C_SetVar,

      // rendering
      C_ToggleFrame,
      C_ToggleChunks,
      C_ToggleShowRay,
      C_ToggleVobBox,
      C_ToggleMeleeBladeDraw,

      // game
      C_ToggleDesktop,
      C_ToggleTime,
      C_TimeMultiplyer,
      C_TimeRealtime,
      // npc
      C_CheatFull,
      C_CheatGod,
      C_Kill,
      // camera
      C_CamAutoswitch,
      C_CamMode,
      C_ToggleCamDebug,
      C_ToggleCamera,
      C_ToggleInertia,
      C_ZToggleTimeDemo,

      C_ZTrigger,
      C_ZUntrigger,

      C_AiGoTo,
      C_GoToPos,
      C_GoToVob,
      C_GoToWayPoint,
      C_GoToCamera,

      C_SetTime,

      C_Insert,
      C_PlayAni,

      // opengothic specific
      C_Census,
      C_PlayVideo,
      C_ToggleGI,
      C_ToggleVsm,
      C_ToggleRtsm,
      C_TogglePathtrace,
      C_ToggleFootIk,
      C_ToggleGaze,
      C_ToggleLipSync,
      C_ToggleLean,
      C_ToggleAniBlend,
      C_ToggleAim,
      C_ToggleGestures,
      C_ToggleBlink,
      C_ToggleManualAim,
      C_ToggleNpcManualAim,
      C_ToggleFlinch,
      C_ToggleRagdoll,
      C_ToggleRagdollQuestGuard,
      C_ToggleLivingBodies,
      C_ToggleArrowPhysics,
      C_ToggleArrowSticking,
      C_ToggleSpellPhysics,
      C_ToggleMovableVobs,
      C_ToggleItemHulls,
      C_ToggleFurnitureParts,
      C_TogglePhysicsNoise,
      C_ToggleDestruction,
      C_ToggleBuoyancy,
      C_ToggleCloth,
      C_ToggleItemWake,
      C_TogglePlacedItems,
      C_ToggleCharacterVirtual,
      C_ToggleNpcCharacterVirtual,
      C_ToggleMonsterCharacterVirtual,
      C_ToggleMeleeBladeShadow,
      C_ToggleMeleePhysicsHits,
      C_ToggleMeleeReactions,
      C_ToggleMeleePosture,
      C_ToggleMeleeDownTactics,
      C_ToggleMeleeHitStop,
      C_ToggleMeleeStamina,
      C_ToggleMeleeParry,
      };

    struct Cmd {
      std::string_view cmd  = "";
      CmdType          type = C_None;
      };

    struct CmdVal {
      CmdVal() = default;
      CmdVal(CmdType t){ cmd.type = t; };
      CmdVal(Cmd cmd):cmd(cmd) {};

      Cmd              cmd;
      std::string_view complete = "";
      bool             fullword = false;

      std::string_view argv[4]  = {};
      };

    CmdVal recognize(std::string_view v);
    CmdVal isMatch(std::string_view inp, const Cmd& cmd) const;
    auto   completeInstanceName(std::string_view inp, bool& fullword) const -> std::string_view;

    bool   addItemOrNpcBySymbolName(World* world, std::string_view name, const Tempest::Vec3& at);
    bool   printVariable           (World* world, std::string_view name);
    bool   setVariable             (World* world, std::string_view name, std::string_view value);
    bool   setTime                 (World& world, std::string_view hh, std::string_view mm);
    bool   goToVob                 (World& world, Npc& player, Camera& c, std::string_view name, size_t n);

    static bool showChunks;
    std::vector<Cmd> cmd;
  };

