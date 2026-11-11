#pragma once

#include "aibridge.h"

#include <Tempest/SoundEffect>
#include <Tempest/Timer>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "beasts.h"
#include "chatoverlay.h"
#include "dialoggraph.h"
#include "fateend.h"
#include "game/bodycontroller.h"
#include "game/gamescript.h"
#include "gothic.h"
#include "linelibrary.h"
#include "necromancy.h"
#include "npccontroller.h"
#include "npcobserver.h"
#include "npcview.h"
#include "storyruntime.h"
#include "storytriggers.h"
#include "storyworld.h"
#include "world/objects/npc.h"
#include "world/world.h"

// last: on Windows these pull in <windows.h>, whose macros (ERROR, VOID, CONST, near, far)
// would rewrite the ZenKit and game declarations above and the names below
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#ifdef _WIN32
#undef ERROR
#undef VOID
#undef CONST
#undef near
#undef far
#endif

namespace {

std::string npcSymbol(World& world, Npc& npc) {
  if(auto sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return {};
  }

// any npc of the world by script symbol (attached or not)
Npc* npcBySymbol(World& world, std::string_view symbol) {
  const size_t id = world.script().findSymbolIndex(symbol);
  return id==size_t(-1) ? nullptr : world.findNpcByInstance(id);
  }

}

struct Voice final {
  std::string                  line;
  std::string                  npc;
  std::shared_ptr<VoiceBuffer> buf;
  Tempest::SoundEffect         effect;
  uint64_t                     startedAt = 0;  // world ticks
  uint64_t                     drainedAt = 0;  // app ticks
  bool                         sized     = false;  // the line got its real length
  bool                         overlay   = false;  // said on the move: no output barrier to correct
  };

struct AiBridge::Impl final {
  struct Inbound final {
    enum Kind : uint8_t { Open, Close, Error, Message };
    Kind        kind = Message;
    std::string text;
    };

  Impl();
  ~Impl();

  // A session is one line of play: a new game or a loaded save starts a new branch. The server
  // keeps npc memories per branch; the memory head (`seq`) it reports goes into every save, and a
  // load branches off what the save remembers. World changes keep the branch.
  struct Session final {
    std::string branch;
    std::string parent;
    int64_t     parentSeq = 0;
    int64_t     seq       = 0;
    std::string reason;
    };

  void onTick(uint64_t dt);
  void processInbound(World* world, Inbound& in);
  void onStartLoading();
  void onSessionUnload() {
    if(auto world = Gothic::inst().world()) {
      if(!saving)
        if(auto* pl = world->player())
          pl->setPlayerPose(false,"world reset");
      observer.cancelDeeds(*world);
      }
    observer.clear();
    voices.clear();
    resetBodyObservations();
    }
  void onWorldLoaded();
  bool takePendingSession();
  void skipIntro(World& world);
  void startBranch(std::string parent, int64_t parentSeq, std::string_view reason);
  void sendSession();
  void refreshChronicle();
  void updateSaveRecord();
  void onDebug(const rapidjson::Value& msg);
  void runDebug();
  void runWorldDebug(const rapidjson::Value& msg);
  void onPerception(Npc& self, Npc& other, Npc* victim, PercType perc);
  void onObservedPerception(Npc& self, Npc& other, Npc* victim, PercType perc);
  void onPhysicsNoiseHeard(Npc& self, const PhysicsNoise& noise);
  void onHeroDeed(const HeroDeed& deed);
  void onCombatHit(Npc& npc, Npc& by, int32_t damage, std::string_view kind);
  bool onSvmLine(Npc& npc, std::string_view svm, std::string_view line, Npc* target, bool overlay);
  // who drives the npc instead of its scripts: "attached" or nobody
  std::string_view npcOwner(std::string_view symbol) const;
  bool onConsoleCommand(std::string_view cmd, std::string& reply);

  void handle(World* world, const std::string& text);
  void onBodyWatch(World& world, const rapidjson::Value& msg);
  void onBodyControl(World& world, const rapidjson::Value& msg);
  void pumpBodyControl();
  void finishBodyReset(World* world);
  void resetBodyObservations();
  void onSay(World& world, const rapidjson::Value& msg);
  // the director's lines (story graph) get the npc's voice from the server (v26)
  void askVoice(const std::string& line, const std::string& npc, const std::string& text);
  // a line of the director's own (npc symbol, text in the game codepage): a runtime line, voiced; its id
  std::string storyLine(const std::string& npc, const std::string& text);
  // a graph_patch message: the operations in order, the triggers' to the triggers; the ones not applied
  std::vector<std::string> applyPatch(const rapidjson::Value& msg);
  void voiceStoryLine(World& world, const rapidjson::Value& msg, const std::string& line);
  void onSayAudio(World& world, const rapidjson::Value& msg);
  // streamed voices: playback starts with the line, the line is shortened to the real length
  void startVoice(const LineLibrary::Line& line);
  void tickVoices(World& world);
  void correctBarrier(World& world, struct Voice& v);
  void onChatStatus(World& world, const rapidjson::Value& msg);
  void onTranscript(const rapidjson::Value& msg);
  // v7: the npc is interrupted: its voice stops, its lines not started yet speak nothing
  void onHush(World& world, const rapidjson::Value& msg);

  // talking to the world (v7): T held is voice, a short tap opens the input line; nothing is modal,
  // the server decides whom the hero speaks to
  void ensureChat();
  bool canSpeak();
  // v10: an npc's offer of a deal in a window beside the game (docs/npc-tools.md); Y accepts it
  void onDealShow(World& world, const rapidjson::Value& msg);
  void tickDeal(World& world);
  std::string dealJson();
  // the npc the hero looks at (his focus), empty if none
  std::string focusSymbol(World& world);
  void submitChat(const std::string& text);
  void onLineStarted(const LineLibrary::Line& line);
  // an original line sent with `say` starts (the engine asks for its text)
  void onOriginalLine(std::string_view id);
  // `audio`: the test bench says this recording for the hero (the server takes it instead of the microphone)
  void sendPtt(std::string_view type, std::string_view audio = {});
  // the npc the hero speaks to listens while he holds the talk key, nods as his words come; the npc
  // the server's model answers for thinks until its answer starts
  Npc* pickListener(World& world);
  void tickListener(World& world);
  // an npc says a line to another one: it hears the accents of the voice (Npc::voiceAccents), a nod may come on each
  void tickAddressed(World& world);

  void send(std::string&& text);
  void sendResult(std::string_view req, bool ok, std::string_view err, std::string_view value = {});
  // debug commands about dialogs (-devmode): they run right away, in the game tick
  void onDialogDebug(World* world, const rapidjson::Value& msg);
  void onStoryEval(World& world, const rapidjson::Value& msg);
  // v13: the necromancer's answer (docs/xardas-wrath.md): `npc` kills the hero, raises him, the game ends
  bool onRaiseDead(World& world, const rapidjson::Value& msg, std::string& err);
  void sendWorld(std::string_view type, World* world);

  std::string          url;
  ix::WebSocket        ws;

  std::mutex           sync;
  std::vector<Inbound> inbound;

  bool                 connected = false;
  uint64_t             chronicleRequests = 0;
  std::unordered_map<std::string,std::function<void(const ChronicleData&)>> chronicleCallbacks;
  uint64_t             fateCardRequests = 0;
  std::unordered_map<std::string,std::function<void(const FateCard&)>> fateCardCallbacks;
  bool                 replaceSvm = false;
  uint64_t             svmLines = 0;
  uint64_t             bodyGeneration = 0;
  bool                 bodyResetPending = false;
  std::string          lastError;
  NpcController        ctrl{[this](std::string&& s){ send(std::move(s)); }};
  NpcObserver          observer{[this](std::string&& s){ send(std::move(s)); },
                                [this](std::string_view symbol){ return npcOwner(symbol); }};
  NpcView              view{[this](std::string&& s){ send(std::move(s)); }};
  // wolves hear the hero's voice (v21, docs/beasts.md)
  BeastWatch           beasts{[this](std::string&& s){ send(std::move(s)); }};

  Session              session;
  bool                 saving = false;  // a save is in progress: the same world comes back
  // shared with the loading thread (save-game hooks)
  std::mutex           saveSync;
  std::string          saveRecord;      // written into the next save
  bool                 pendingSession  = false;
  bool                 pendingFromSave = false;
  std::string          pendingRecord;   // read from the loaded save

  // debug save/load must not run inside the game tick (it clears the world mid-frame):
  // it waits for the application's event loop
  Tempest::Timer       debugTimer;
  Tempest::Timer       bodyTimer;
  std::string          debugReq, debugCmd, debugSlot;

  LineLibrary                  lines;
  std::vector<struct Voice>    voices;
  std::unordered_map<std::string,std::string> lastLine;  // npc -> line it speaks (or spoke) last
  std::unordered_set<std::string>             voicing;   // the director's lines the server is asked to voice
  std::unordered_map<std::string,std::string> originalLines;  // original line sent with `say` -> npc, until it starts
  std::unordered_map<std::string,std::string> originalTo;     // ...-> the npc it is said to (v15), if not the hero
  std::unique_ptr<ChatOverlay> chat;
  Necromancy                   necro;
  // v56: the final card at any end of the game (docs/achievements.md)
  FateEnd                      fate;
  std::string                  listener;       // the npc listening to the hero's voice
  std::string                  lastAddressee;  // whom the server said the hero spoke to last
  std::string                  thinker;        // the npc thinking of an answer
  struct Addressed final {
    std::string to, line;    // the npc it is said to, the line
    uint64_t    until = 0;   // tick: no longer
    int32_t     heard = 0;   // accents it heard
    };
  std::unordered_map<std::string,Addressed> addressed;  // speaker -> its line said to an npc now
  bool                         pttHeld     = false;
  uint64_t                     listenUntil = 0;  // after the key: until the final transcript, at most
  uint64_t                     thinkUntil  = 0;  // no answer by then: it stops thinking
  // dialogs of the story graph: the lines of the director's dialogs are runtime lines too
  uint64_t                     graphLines = 0;
  // what the director's programs and conditions run on, in dialogs and triggers (docs/director.md, stage 3)
  StoryRuntime                 runtime{[this](const std::string& npc, const std::string& text) {
                                         return storyLine(npc,text);
                                         }};
  DialogGraph                  graph{[this](std::string&& s){ send(std::move(s)); },
                                     [this](const std::string& npc, const std::string& text) {
                                       return storyLine(npc,text);
                                       },
                                     runtime};
  // the director's triggers: an event, a condition, a program (v35)
  StoryTriggers                triggers{runtime,[this](std::string&& s){ send(std::move(s)); }};
  // the director's hands in the world: tagged creatures, deaths, effects on npc's (v8)
  StoryWorld                   story{[this](std::string&& s){ send(std::move(s)); },
                                     [this](World& w, Npc& npc) {
                                       BodyController::takeover(npc,"story_takeover");
                                       std::string err;
                                       const auto  symbol = npcSymbol(w,npc);
                                       if(ctrl.isAttached(symbol))
                                         ctrl.detach(w,symbol,"story",err);
                                       }};
  };
