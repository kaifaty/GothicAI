#include "testbench_internal.h"
#include "testbench.h"

#include <cmath>

#include "ui/gamemenu.h"
#include "navbench.h"
#include "navdemos.h"
#include "game/gamescript.h"
#include "game/definitions/sounddefinitions.h"
#include "game/archerai.h"
#include "game/playercontrol.h"
#include "game/bodytracesbench.h"
#include "game/bloodstainsbench.h"
#include "game/navigationeyebench.h"
#include "game/navigationmemorybench.h"
#include "game/knownroadsbench.h"
#include "game/npckeysbench.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/lipsync.h"
#include "graphics/mesh/talkgestures.h"
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/eyes.h"
#include "graphics/mesh/lean.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/animblend.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/mesh/flinch.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "world/aiqueue.h"
#include "gothic.h"
#include "marvin.h"

using namespace Tempest;
using namespace testbench_detail;

namespace {

void replaceAll(std::string& s, std::string_view from, std::string_view to) {
  for(size_t p=s.find(from); p!=std::string::npos; p=s.find(from,p+to.size()))
    s.replace(p,from.size(),to);
  }

// foreach slope <min deg> <max deg> <count> | foreach stairs <count> | foreach wp <name part> <count>
// ... end: the body runs at each spot, after a teleport; $NAME, $I, $ANGLE are replaced
void expandForeach(size_t at, World& w, Npc& pl) {
  const auto a     = split(bench.prog[at]);
  size_t     end   = at+1;
  int        depth = 1;
  for(; end<bench.prog.size(); ++end) {
    const auto c = split(bench.prog[end]);
    if(c[0]=="foreach")
      ++depth;
    if(c[0]=="end" && --depth==0)
      break;
    }
  std::vector<std::string> body(bench.prog.begin()+long(at)+1, bench.prog.begin()+long(std::min(end,bench.prog.size())));

  std::vector<Spot> spots;
  const std::string kind = a.size()>1 ? a[1] : "";
  if(kind=="slope")
    spots = findSpots(w,pl,K_Slope,num(a,2,10),num(a,3,35),"",size_t(num(a,4,5)));
  else if(kind=="stairs")
    spots = findSpots(w,pl,K_Stairs,0,0,"",size_t(num(a,2,5)));
  else if(kind=="wp" && a.size()>2)
    spots = findSpots(w,pl,K_Name,0,90,a[2],size_t(num(a,3,5)));
  else
    say("foreach: unknown kind '" + kind + "'");

  std::vector<std::string> out;
  for(size_t i=0; i<spots.size(); ++i) {
    char ang[32];
    std::snprintf(ang,sizeof(ang),"%.0f",spots[i].angle);
    char info[256];
    std::snprintf(info,sizeof(info),"say spot %zu %s angle=%.1f rough=%.1f dist=%.0f",i,spots[i].name.c_str(),spots[i].angle,spots[i].rough,spots[i].dist);
    out.push_back(info);
    out.push_back("goto " + spots[i].name);
    out.push_back("wait 300");
    for(auto s:body) {
      replaceAll(s,"$NAME",spots[i].name);
      replaceAll(s,"$ANGLE",ang);
      replaceAll(s,"$I",std::to_string(i));
      out.push_back(s);
      }
    }
  say("foreach " + kind + ": " + std::to_string(spots.size()) + " spots");
  const size_t last = std::min(end+1,bench.prog.size());
  bench.prog.erase(bench.prog.begin()+long(at),bench.prog.begin()+long(last));
  bench.prog.insert(bench.prog.begin()+long(at),out.begin(),out.end());
  bench.pc = at;
  }

}

namespace testbench_detail {

// This fixture changes only preparation. After the native stand-up is queued,
// NPC samples and completion checks are passive: no path, state or movement command.
void exec(const std::string& line, World& w, Npc& pl, PlayerControl& ctrl) {
  const auto a = split(line);
  if(a.empty())
    return;
  const std::string& c = a[0];

  if(c=="healing") {
    try {healingCommand(bench.healing,a,w,pl,ctrl,bench.melee.meleeShadowActors);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    return;
  }
  if(c=="roads") {
    try {
      event("roads_command","\"sample\":"+KnownRoadsBench::command(a,w,outDir));
    } catch(const std::exception& error) {
      event("roads_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="memory") {
    try {
      event("memory_command","\"sample\":"+NavigationMemoryBench::command(a,w,pl,ctrl,outDir));
    } catch(const std::exception& error) {
      event("memory_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="body_traces") {
    try {
      event("body_traces","\"sample\":"+BodyTracesBench::command(a,w,pl,outDir));
    } catch(const std::exception& error) {
      event("body_traces_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="blood_stains") {
    try {
      event("blood_stains","\"sample\":"+BloodStainsBench::command(a,w,pl,outDir));
    } catch(const std::exception& error) {
      event("blood_stains_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="keys") {
    try {
      event("keys_command","\"sample\":"+NpcKeysBench::command(a,w,pl,ctrl,outDir));
    } catch(const std::exception& error) {
      event("keys_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  // Navigation-eye commands are independent of navigation mesh and body rays.
  if(c=="eye") {
    try {
      if(a.size()>1 && a[1]=="setup") NavigationMemoryBench::reset(true);
      event("eye_command","\"sample\":"+NavigationEyeBench::command(a,w,pl,outDir));
    } catch(const std::exception& error) {
      event("eye_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="navdemos") {
    const auto* live=std::getenv("OPENGOTHIC_NAV_LIVE");
    // Only the explicit live stand may record navigation in a rendered bench.
    if(!Resources::isHeadless() && (live==nullptr || std::string_view(live)!="1"))
      throw std::runtime_error("navdemos is runner-only");
    bench.navigationOnly = true;
    NavDemos::command(a,w,pl,outDir);
    return;
  }
  // MSVC allows at most 128 nested blocks, including else-if clauses (C1061).
  // Split the ladder into runs while preserving the first matching command.
  bool dispatched=true;
  if(c=="navmesh") {
    bench.navigationOnly = true;
    if(a.size()!=2 || a[1]!="wait") NavBench::command(a,w,pl,outDir);
    }
  else if(monsterCharacterCommand(bench.monster,monsterCharacterContext(),a,w,pl)) return;
  else if(c=="monstercharacter" && a.size()>=2 && a[1]=="camera" && cameraCommand(cameraContext(),line,a,w,pl,ctrl)) return;
  else if(c=="monstercharacter" && a.size()==5 && a[1]=="follow" && cameraCommand(cameraContext(),line,a,w,pl,ctrl)) return;
  else if(characterCommand(bench.character,characterContext(),a,w,pl,ctrl)) return;
  else if(Resources::isHeadless() && c=="cheat" && a.size()==2 && a[1]=="god") {
    // Marvin also draws a notification with a GPU font. The CPU runner needs the same game flag.
    Gothic::inst().setGodMode(!Gothic::inst().isGodMode());
    say("cheat god");
    }
  else if(itemWakeCommand(a,w,pl)) return;
  else if(placedItemsCommand(bench.placedItems,a,w,pl,ctrl)) return;
  else if(itemHullsCommand(bench.itemHulls,a,w,pl)) return;
  else if(c=="physics" && worldCommand(worldContext(),line,a,w,pl,ctrl)) return;
  else if(c=="traces" && a.size()>=2) {
    std::string command=a[1],value,error;
    for(size_t i=2;i<a.size();++i)command+=' '+a[i];
    if(!TestBench::physicsTracesDebug(w,pl,command,value,error)) {arenaAbort(error);return;}
    event("physics_traces_debug","\"command\":"+jsonStr(command)+",\"sample\":"+value);
    }
  else if(destructionCommand(bench.destruction,destructionContext(),a,w,pl,ctrl)) return;
  else if(fracturePiecesCommand(bench.destruction.destructionIds,bench.destruction.destructionMeleeActor,bench.destruction.destructionMeleeStand,bench.destruction.destructionMeleeFacing,bench.cam,bench.cutPending,a,w)) return;
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if(c=="water-film" && waterFilmCommand(waterFilmContext(),a,w,pl)) return;
  else if(c=="cloth" && clothCommand(clothContext(),a,w,pl)) return;
  else if(waterCommand(bench.water,waterContext(),a,w,pl,ctrl)) return;
  else if(c=="item-save" && a.size()==2 && a[1]=="initial") {
    arenaAbort("item save initial must be the first scenario command before simulation");
    }
  else if(c=="item-save" && a.size()==2 && a[1]=="report") {
    try {savedItemReport(w);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(noiseCommand(movablesContext(),a,w,pl,ctrl)) return;
  else if(c=="world-data" && worldCommand(worldContext(),line,a,w,pl,ctrl)) return;
  else if(ownMeshCommand(ownMeshContext(),a,w,pl,ctrl)) return;


  else if(furniturePartsCommand(bench.furnitureParts,a,w,pl)) return;
  else if(movableCommand(movablesContext(),a,w,pl,ctrl)) return;
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if(spellCommand(movablesContext(),a,w,pl,ctrl)) return;
  else if(bodyImpactCommand(bench.bodyImpact,bodyImpactContext(),a,w,pl)) return;
  else if((c=="sticking" || c=="arrow") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(livingCommand(livingContext(),a,w,pl,ctrl)) return;
  else if(meleeCommand(meleeContext(),a,w,pl,ctrl)) return;
  else if(c=="cadence" && worldCommand(worldContext(),line,a,w,pl,ctrl)) return;
  else if(ragdollCommand(ragdollContext(),line,a,w,pl,ctrl)) return;


  else if(routineCommand(routineContext(),a,w,pl,ctrl)) return;
  else if((c=="waitragdoll" || c=="standup") && worldCommand(worldContext(),line,a,w,pl,ctrl)) return;


  else if(c=="ragdoll" && a.size()==2 && a[1]=="frame20") {
    if(!frameRagdollArena()) arenaAbort("twenty loaded models required before framing arena");
    }


  else if((c=="crowd" || c=="down" || c=="push") && worldCommand(worldContext(),line,a,w,pl,ctrl)) return;
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if((c=="world" || c=="world-check" || c=="perf" || c=="streaming" || c=="deterministic") && worldCommand(worldContext(),line,a,w,pl,ctrl)) return;
  else if((c=="wait" || c=="waitarrive" || c=="say" || c=="log" || c=="label" || c=="goto" || c=="route" || c=="walkto" || c=="near" || c=="approach" || c=="walkmode" || c=="face" || c=="turn" || c=="wheel" || c=="press" || c=="release" || c=="tap" || c=="hold") && controlCommand(controlContext(),line,a,w,pl,ctrl)) return;
  else if(cameraCommand(cameraContext(),line,a,w,pl,ctrl)) return;
  else if(overlayCommand(overlayContext(),a)) return;
  else if(c=="diary") {
    // diary chronicle [feat] [open]: opens the diary on its «Летопись» tab, with that feat selected
    // (counting the other fates' ones after this fate's), shown in full with `open`. The game pauses in
    // the diary and the bench with it: shoot the window from outside (screencapture -l).
    GameMenu::benchDiary.chronicle = a.size()>1 && a[1]=="chronicle";
    GameMenu::benchDiary.feat      = int32_t(num(a,2,-1));
    GameMenu::benchDiary.open      = a.size()>3 && a[3]=="open";
    if(TestBench::openMenu)
      TestBench::openMenu("MENU_LOG"); else
      say("diary: no menu hook");
    }
  else if(c=="ui") {
    bench.ui = !(a.size()>1 && a[1]=="off");
    }
  else if(c=="anims") {
    // anims: what the watched one (or the hero) plays, its body state, weapon and target
    Npc& s = subjectOf(pl);
    char buf[160];
    std::snprintf(buf,sizeof(buf),"anims %s bs=%u ws=%d target=%s: ",symName(w,&s).c_str(),unsigned(s.bodyStateMasked() & BS_MAX),
                  int(s.weaponState()),symName(w,s.target()).c_str());
    say(buf + s.animNames());
    }
  else if(c=="cansee" && aimSpotsCommand(bench.hasPrev,line,a,w,pl)) return;
  else if((c=="hp" || c=="give" || c=="equip" || c=="disarm" || c=="draw" || c=="put" || c=="put-at" || c=="pin" || c=="npcgo") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if(c=="bowstrength" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="sfx") {
    // sfx <name>...: the files a sound effect plays (its variants _A1.. too), whether they load
    for(size_t k=1; k<a.size(); ++k) {
      std::string out = "sfx " + a[k] + ":";
      for(int v=0; v<10; ++v) {
        const std::string name = v==0 ? a[k] : a[k]+"_A"+std::to_string(v);
        auto& d = Gothic::sfx()[name];
        if(d.file.empty())
          break;
        out += " " + d.file + (Resources::loadSoundBuffer(d.file).isEmpty() ? " (missing)" : "") +
               " vol " + std::to_string(d.vol);
        }
      say(out);
      }
    }
  else if(c=="animspeed") {
    // animspeed <anim> <k>: the hero's animation of that name plays k times as fast from its next start
    // (the reload's tempo is set by the manual aim from dexterity: this overrides it until then)
    if(a.size()<3) {
      say("animspeed: <anim> <k>");
      return;
      }
    pl.setAnimSpeed(a[1],num(a,2,1));
    say("animspeed " + a[1] + " " + a[2]);
    }
  else if((c=="bowdraw" || c=="bowrelease") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="animinfo") {
    // animinfo <name>...: the hero's animations of those names (frames, the nodes, how far they go, the
    // hips)
    for(size_t i=1; i<a.size(); ++i)
      say("animinfo " + pl.animInfo(a[i]));
    }
  else if(c=="bladepath") {
    // bladepath <name>|<overlay.MDS>...: the tip of the hero's weapon at each frame of those animations (model
    // frame); an overlay (HUMANS_1HST2.MDS) is put on for the names after it
    const auto* weapon = pl.currentMeleeWeapon();
    const auto* mesh = weapon!=nullptr ? Resources::loadMesh(weapon->handle().visual) : nullptr;
    const float length = mesh!=nullptr ? -mesh->bbox()[0].x : 0.f;
    for(size_t i=1; i<a.size(); ++i)
      if(a[i].size()>4 && a[i].substr(a[i].size()-4)==".MDS")
        pl.addOverlay(a[i],0); else
        say("bladepath " + pl.bladePath(a[i],length));
    }
  else if(c=="aimwalk" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="bowgeom") {
    // bowgeom: the bow and the bow hand in the arrow's frame, as posed now
    say(pl.bowDump());
    }
  else if((c=="stats" || c=="npcday" || c=="seed") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if(npcSimCommand(bench.npcDuel,bench.npcTierTest,bench.npcSpeedTest,bench.pc,bench.prog,a,w,pl)) return;
  else if(bodyCommand(bodyContext(),a,w,pl)) return;
  else if(c=="knockdown-launch-equip") {
    auto* npc=findNpc(w,pl,"SLD_826_SOELDNER");
    const auto weapon=w.script().findSymbolIndex("ITMW_2H_SLD_AXE");
    if(npc==nullptr || weapon==size_t(-1)) {arenaAbort("knockdown launch actors or weapon missing");return;}
    npc->setToFightMode(weapon);
    if(npc->weaponState()!=WeaponState::W2H || npc->currentMeleeWeapon()==nullptr ||
       npc->currentMeleeWeapon()->clsId()!=weapon) {arenaAbort("knockdown launch requires the exact drawn two-handed axe");return;}
    event("launch_weapon","\"actor\":"+jsonStr(symName(w,npc))+",\"actor_id\":"+std::to_string(w.npcId(npc))+
          ",\"weapon\":\"ITMW_2H_SLD_AXE\",\"state\":\"W2H\"");
    }
  else if(c=="mds") {
    // mds <npc|player> <OVERLAY.MDS> [off]: an overlay of the model script on it or off (a gait: HUMANS_MILITIA.MDS)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr || a.size()<3) {
      say("mds: no npc or overlay " + line);
      return;
      }
    if(a.size()>3 && a[3]=="off")
      n->delOverlay(a[2]); else
      n->addOverlay(a[2],0);
    say("mds " + symName(w,n) + " " + a[2] + (a.size()>3 ? " " + a[3] : ""));
    }
  else if(c=="quiver") {
    // quiver place <deg behind the right side> <min push> <tilt back> <tilt out> [tilt right] [height] [side] |
    // quiver place off | quiver still on|off: a probe of other places (research.md of the quiver report),
    // the hero and the npcs fitted anew; still: held at rest, no swing
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="still") {
      Quiver::still = a.size()>2 && a[2]=="on";
      say(std::string("quiver still ") + (Quiver::still ? "on" : "off"));
      return;
      }
    if(m!="place") {
      say("quiver: place|still " + line);
      return;
      }
    Quiver::Place p;
    if(a.size()>2 && a[2]!="off") {
      p.on        = true;
      p.around    = num(a,2,0);
      p.minPush   = num(a,3,Quiver::MinOut);
      p.tiltBack  = num(a,4,Quiver::TiltBack);
      p.tiltOut   = num(a,5,Quiver::TiltOut);
      p.tiltRight = num(a,6,0);
      p.height    = num(a,7,Quiver::MouthHeight);
      p.side      = num(a,8,0);
      p.back      = 0;
      }
    Quiver::place = p;
    w.detectNpc(pl.position(),100000.f,[](Npc& n) { n.refitQuiver(); });
    pl.refitQuiver();
    const auto q = pl.quiverProbe();
    char buf[160];
    std::snprintf(buf,sizeof(buf),"quiver place %s out %.1f mouth %.2f %.2f %.2f",p.on ? "on" : "off",double(q.out),
                  double(q.mouth.x),double(q.mouth.y),double(q.mouth.z));
    say(buf);
    }
  else if(c=="arrows" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if((c=="npcequip" || c=="wander" || c=="stuck" || c=="fight") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if(c=="arc" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if((c=="waitdown" || c=="npcaim" || c=="still" || c=="zones" || c=="open" || c=="volley" || c=="immortal" || c=="stun-drift" || c=="stun" || c=="dodge" || c=="clear" || c=="flinch" || c=="hit" || c=="strike") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if((c=="archers" || c=="archer") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="aimat" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if((c=="aim" || c=="aimlean") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="lean") {
    // lean on|off: the lean into turns and on slopes (graphics/mesh/lean)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Lean::setEnabled(m=="on"); else
      say("unknown lean command: " + line);
    }
  else if(c=="blend") {
    // blend on|off, blend param <min|scale|max|turn|move|out|side|blow> <value>: transitions between animations
    // (AnimBlend)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      AnimBlend::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(AnimBlend::setParam(a[2],num(a,3,0)) ? "blend param " : "blend: unknown param ") + a[2] + " " + a[3]);
    else
      say("unknown blend command: " + line);
    }
  else if(c=="ik") {
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      FootIk::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(FootIk::setParam(a[2],num(a,3,0)) ? "ik param " : "ik: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("ik params: " + FootIk::paramList());
    else
      say("unknown ik command: " + line);
    }
  else if(c=="foreach") {
    expandForeach(bench.pc-1,w,pl);
    }
  else if(c=="end") {
    }
  else if((c=="slopes" || c=="stairs" || c=="ledges" || c=="corner" || c=="walls" || c=="perches" || c=="edge" || c=="edgedist") &&
          aimSpotsCommand(bench.hasPrev,line,a,w,pl)) return;
  else if(c=="findwp") {
    for(auto& s:findSpots(w,pl,K_Name,0,90,rest(line),40)) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"wp %s angle=%.1f rough=%.1f dist=%.0f",s.name.c_str(),s.angle,s.rough,s.dist);
      say(buf);
      }
    }
  else if(c=="pos") {
    const Vec3 p = pl.position();
    event("position","\"world_tick_ms\":"+std::to_string(w.tickCount())+
          ",\"world_time_ms\":"+std::to_string(w.time().toInt())+",\"position_cm\":"+ragdollVector(p));
    char buf[256];
    std::snprintf(buf,sizeof(buf),"pos %.0f %.0f %.0f rot=%.1f bs=%u air=%d swim=%d slide=%d mob=%d guild=%u aligned=%d",p.x,p.y,p.z,pl.rotation(),
                  unsigned(pl.bodyStateMasked()),int(pl.isInAir()),int(pl.isSwim()),int(pl.isSlide()),int(pl.interactive()!=nullptr),
                  unsigned(pl.guild()),int(w.script().guildVal().surface_align[pl.guild()]));
    say(buf);
    }
  else if((c=="talk" || c=="speak" || c=="dialog" || c=="choose" || c=="hear" || c=="think" || c=="nearby" || c=="state" || c=="come" || c=="pair" || c=="where" || c=="ptt" || c=="waitreply" || c=="waitquiet" || c=="waitai" || c=="mark" || c=="ai") && dialogCommand(dialogContext(),line,a,w,pl)) return;
  else if(c=="faceanim") {
    // faceanim <npc|nearest|player> <anim> [ms] [intensity]: a face animation of the head (S_ANGRY, S_FRIENDLY...)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3) {
      say("faceanim: no npc or anim " + line);
      return;
      }
    n->startFaceAnim(a[2],num(a,4,1),uint64_t(num(a,3,3000)));
    say("faceanim " + nameUtf8(*n) + " " + a[2]);
    }
  else if(c=="facecue") {
    // facecue <npc|nearest|player> <gesture> [L|R]: a facial gesture of its own head (flash, doubt, smirk...; the
    // profile's cues); a one-sided one without a side goes on a side by the seed
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    const char side = a.size()>3 && (a[3]=="L" || a[3]=="R") ? a[3][0] : 0;
    if(n==nullptr || a.size()<3 || (a.size()>3 && side==0)) {
      say("facecue: no npc, gesture or side L|R " + line);
      return;
      }
    if(!n->startCue(a[2],side)) {
      say("facecue: no gesture " + a[2] + " on the head of " + nameUtf8(*n));
      return;
      }
    say("facecue " + nameUtf8(*n) + " " + a[2] + (side ? std::string(" ")+side : std::string()));
    }
  else if(c=="faceleak") {
    // faceleak <npc|nearest|player> <felt> <upper|lower>: a leak of the felt emotion (friendly, angry, hostile, afraid,
    // pain) on its own head now, as a line that hides a lie: with the liar's two blinks after it
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<4 || (a[3]!="upper" && a[3]!="lower")) {
      say("faceleak: no npc, felt or half upper|lower " + line);
      return;
      }
    if(!n->startLeak(a[2],a[3]=="lower")) {
      say("faceleak: no leak " + a[2] + " " + a[3] + " on the head of " + nameUtf8(*n));
      return;
      }
    say("faceleak " + nameUtf8(*n) + " " + a[2] + " " + a[3]);
    }
  else if(c=="ani") {
    // ani <npc|nearest|player> <anim> [force]: it plays an animation of its model script (T_YES,
    // T_DIALOGGESTURE_03); force cuts what plays now, as a hit reaction or a stun's stumble that a stand would wait for
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3) {
      say("ani: no npc or anim " + line);
      return;
      }
    auto sq = n->playAnimByName(a[2],BS_STAND,a.size()>3 && a[3]=="force");
    say("ani " + symName(w,n) + " " + a[2] + (sq==nullptr ? " failed" : ""));
    }
  else if(c=="lips") {
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      LipSync::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(LipSync::setParam(a[2],num(a,3,0)) ? "lips param " : "lips: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("lips params: " + LipSync::paramList());
    else
      say("unknown lips command: " + line);
    }
  else if(c=="gestures") {
    // gestures on|off|param <name> <value>|params: the hands while one speaks (off: a random gesture per line)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      TalkGestures::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(TalkGestures::setParam(a[2],num(a,3,0)) ? "gestures param " : "gestures: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("gestures params: " + TalkGestures::paramList());
    else
      say("unknown gestures command: " + line);
    }
  else if(c=="blink") {
    // blink on|off|param <name> <value>|params: blinking (off: nobody blinks, as OpenGothic was)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      EyeBlink::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(EyeBlink::setParam(a[2],num(a,3,0)) ? "blink param " : "blink: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("blink params: " + EyeBlink::paramList());
    else
      say("unknown blink command: " + line);
    }
  else if(c=="watch") {
    // watch <npc|nearest|player>: the camera and the gaze metrics follow this one
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    bench.subject  = (n==&pl) ? nullptr : n;
    Gaze::probeNpc = bench.subject;
    LipSync::probeNpc     = n;
    LipSync::subjectProbe = LipSync::Probe();
    TalkGestures::probeNpc     = n;
    TalkGestures::subjectProbe = TalkGestures::Probe();
    EyeBlink::probeNpc         = n;
    EyeBlink::subjectProbe     = EyeBlink::Probe();
    FaceRig::probeNpc          = n;
    FaceRig::subjectProbe      = FaceRig::Probe();
    Eyes::probeNpc             = n;
    Eyes::subjectProbe         = Eyes::Probe();
    Lean::probeNpc = bench.subject;
    Pose::probeNpc = bench.subject;
    bench.anim1.clear();
    bench.anim2.clear();
    Aim::probeNpc  = bench.subject;
    ArcherAi::probeNpc     = bench.subject;
    ArcherAi::subjectProbe = ArcherAi::Probe();
    Flinch::probeNpc = bench.subject;
    char buf[160] = "watch nobody";
    if(n!=nullptr)
      if(auto sym = w.script().findSymbol(size_t(n->instanceSymbol())))
        std::snprintf(buf,sizeof(buf),"watch %s dist=%.0f",sym->name().c_str(),double((n->position()-pl.position()).length()));
    say(buf);
    }
  else if(c=="gazeinfo") {
    Npc&         s = subjectOf(pl);
    const auto&  g = &s==&pl ? Gaze::playerProbe : Gaze::subjectProbe;
    Vec3         at;
    const auto   src = s.gazeTarget(at);
    char buf[300];
    std::snprintf(buf,sizeof(buf),"gazeinfo %s source=%s partner=%s script=%s attention=%s probe valid=%d active=%d w=%.2f err=%.1f hero focus=%s",
                  symName(w,&s).c_str(),gazeSource(src),symName(w,s.talkPartner()).c_str(),symName(w,s.lookAtTarget()).c_str(),
                  symName(w,s.attentionTarget()).c_str(),int(g.valid),int(g.active),double(g.weight),double(g.error),
                  symName(w,pl.target()).c_str());
    say(buf);
    }
  else if(c=="gazeat") {
    // gazeat <npc|nearest|player> <yaw> <pitch> <ms> [dist]: it looks at a point that many degrees aside (+ as
    // its rotation goes) and up from its facing, dist cm (100) from its eyes, for ms
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr) {
      say("gazeat: no npc " + line);
      return;
      }
    const float yaw = (n->rotation()+num(a,2,0))*float(M_PI)/180.f, pitch = num(a,3,0)*float(M_PI)/180.f;
    const Vec3  dir = Vec3(std::cos(yaw)*std::cos(pitch),std::sin(pitch),std::sin(yaw)*std::cos(pitch));
    n->lookAtPoint(n->mapEyes() + dir*num(a,5,100),w.tickCount()+uint64_t(num(a,4,3000)));
    say("gazeat " + symName(w,n) + " " + (a.size()>3 ? a[2]+" "+a[3] : std::string("0 0")));
    }
  else if(c=="lookat") {
    // lookat <npc|nearest> <npc|player|off>: as the script AI_LookAtNpc, AI_StopLookAt
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("lookat: no npc " + line);
      return;
      }
    const std::string t = a.size()>2 ? a[2] : "player";
    if(t=="off") {
      n->aiPush(AiQueue::aiStopLookAt());
      say("lookat " + symName(w,n) + " off");
      return;
      }
    Npc* o = findNpc(w,pl,t);
    if(o==nullptr || o==n) {
      say("lookat: no target " + line);
      return;
      }
    n->aiPush(AiQueue::aiLookAtNpc(o));
    say("lookat " + symName(w,n) + " " + symName(w,o));
    }
  else if(c=="freeze") {
    // freeze <npc|nearest> [off]: it leaves what it does and stands still, like a body the AI server attaches
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("freeze: no npc " + line);
      return;
      }
    if(a.size()>2 && a[2]=="off") {
      n->setExternalControl(false);
      say("freeze off");
      return;
      }
    n->setInteraction(nullptr,true);
    n->clearAiQueue();
    n->clearState(false);
    n->setExternalControl(true);
    say("freeze " + symName(w,n));
    }
  else if(c=="ani") {
    // ani <npc|nearest|player> <name>: it plays that animation, as play_ani does (T_STAND_2_PEE goes on to
    // its loop S_PEE till something else plays)
    Npc* n = a.size()>1 && a[1]=="player" ? &pl : findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3 || !n->hasAnim(a[2])) {
      say("ani: no npc or animation " + line);
      return;
      }
    n->aiPush(AiQueue::aiPlayAnim(a[2]));
    say("ani " + symName(w,n) + " " + a[2]);
    }
  else if(c=="heropose") {
    // heropose kneel|none|check: set the surrender pose, or exercise its real input paths.
    if(a.size()>1 && a[1]=="check") {
      if(!pl.isPlayerKneeling())
        throw std::runtime_error("heropose check requires a kneeling hero");
      const float rotation = pl.rotation();
      ctrl.onRotateMouse(15.f,5.f);
      ctrl.tickMove(16);
      if(!pl.isPlayerKneeling())
        throw std::runtime_error("heropose: mouse rotation ended kneel");
      if(std::abs(std::remainder(pl.rotation()-rotation-15.f,360.f))>0.01f)
        throw std::runtime_error("heropose: mouse rotation did not turn the hero");
      std::string reply;
      if(!aiCommand("ai ptt start",reply))
        throw std::runtime_error("heropose: no AI bridge for ptt_start");
      const bool speakingKneeling = pl.isPlayerKneeling();
      aiCommand("ai ptt cancel",reply);
      if(!speakingKneeling)
        throw std::runtime_error("heropose: ptt_start ended kneel");
      press(ctrl,KeyCodec::Forward);
      release(ctrl,KeyCodec::Forward);
      if(pl.isPlayerKneeling())
        throw std::runtime_error("heropose: Forward did not end kneel");
      say("heropose check passed: mouse and ptt_start preserve kneel; Forward ends kneel");
      return;
      }
    const bool kneel = a.size()>1 && a[1]=="kneel";
    say(std::string("heropose ") + (pl.setPlayerPose(kneel,"testbench none") ? (kneel ? "kneel" : "none") : "refused"));
    }
  else if(c=="transform") {
    // the player becomes a creature, like the shape change spell; "transform back"
    const std::string name = a.size()>1 ? a[1] : "";
    if(name=="back") {
      pl.transformBack();
      say("transform back");
      return;
      }
    const size_t id = w.script().findSymbolIndex(name);
    const bool   ok = id!=size_t(-1) && pl.transformInto(id);
    bench.hasPrev = false;
    say(std::string(ok ? "transform " : "transform failed: ") + name);
    }
  else if(c=="bones") {
    // skeleton of the player: index, parent, name
    auto sk = FootIk::playerProbe.skeleton;
    if(sk==nullptr) {
      say("bones: no skeleton");
      return;
      }
    say("bones: " + std::to_string(sk->nodes.size()) + " nodes, foot ik " + (FootIk::playerProbe.valid ? "valid" : "not valid"));
    for(size_t i=0; i<sk->nodes.size(); ++i) {
      char buf[256];
      const auto& t = sk->tr[i];
      std::snprintf(buf,sizeof(buf),"bone %zu parent %d %s at %.0f %.0f %.0f",i,int(sk->nodes[i].parent==size_t(-1) ? -1 : int(sk->nodes[i].parent)),
                    sk->nodes[i].name.c_str(),t.at(3,0),t.at(3,1),t.at(3,2));
      say(buf);
      }
    }
  else if(c=="caption") {
    // text on the screen of a short from here (gothic-shorts cut): caption <text>, caption off
    event("caption","\"text\":" + jsonStr(rest(line)=="off" ? "" : rest(line)));
    }
  else if(c=="quit") {
    bench.pc = bench.prog.size();
    }
  else {
    const bool ok = marvin().exec(line);
    say(std::string(ok ? "marvin: " : "marvin failed: ") + line);
    }
  }

}
