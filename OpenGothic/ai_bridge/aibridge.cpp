#include "aibridge.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <Tempest/Timer>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/gamescript.h"
#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/bodyenginepolicy.h"
#include "game/inventory.h"
#include "game/herodeed.h"
#include "game/serialize.h"
#include "camera.h"
#include "commandline.h"
#include "physics/dynamicworld.h"
#include "physics/physicsnoise.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "world/worldsound.h"
#include "npcchests.h"
#include "gothic.h"
#include "frameprofile.h"

#include "beasts.h"
#include "chatoverlay.h"
#include "dialoggraph.h"
#include "storycast.h"
#include "storyruntime.h"
#include "storytriggers.h"
#include "storyworld.h"
#include "stucklogger.h"
#include "jsonread.h"
#include "jsonwriter.h"
#include "world/waypoint.h"
#include "linelibrary.h"
#include "necromancy.h"
#include "npccontroller.h"
#include "npcfind.h"
#include "npcobserver.h"
#include "npcview.h"
#include "textcodec.h"
#include "voicebuffer.h"
#include "testbench.h"

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

using namespace Tempest;

namespace {

constexpr int         ProtocolVersion = 54;
constexpr const char* SaveEntry       = "ai_bridge/session";  // own entry in save games
constexpr const char* DefaultUrl      = "ws://127.0.0.1:47817";
constexpr float       IntroCm         = 3000;  // -nointro: those this near the hero at the start

constexpr uint32_t    MoodHoldMs      = 2000;  // the face of a line stays this long after it, then fades (a second)

// the face of the game's heads for a mood of a line (v9)
std::string_view moodFace(std::string_view mood) {
  if(mood=="neutral")  return "S_NEUTRAL";
  if(mood=="friendly") return "S_FRIENDLY";
  if(mood=="angry")    return "S_ANGRY";
  if(mood=="hostile")  return "S_HOSTILE";
  if(mood=="afraid")   return "S_FRIGHTENED";
  if(mood=="pain")     return "T_HURT";
  return "";
  }

std::string lower(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return r;
  }

std::string upperSymbol(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

std::string npcSymbol(World& world, Npc& npc) {
  if(auto sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return {};
  }

// random id of a line of play (see Session below)
std::string newBranchId() {
  static std::mt19937_64 rng{std::random_device{}() ^ uint64_t(std::time(nullptr))};
  static const char*     hex = "0123456789abcdef";
  std::string id;
  for(int i=0; i<2; ++i) {
    uint64_t v = rng();
    for(int k=0; k<16; ++k, v>>=4)
      id += hex[v&0xF];
    }
  return id;
  }

std::vector<uint8_t> fromBase64(std::string_view in) {
  static const auto table = [] {
    std::array<int8_t,256> t{};
    t.fill(-1);
    const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(int i=0; i<64; ++i)
      t[uint8_t(abc[i])] = int8_t(i);
    return t;
    }();
  std::vector<uint8_t> out;
  out.reserve(in.size()*3/4);
  uint32_t acc  = 0;
  int      bits = 0;
  for(char c:in) {
    const int8_t v = table[uint8_t(c)];
    if(v<0)
      continue;  // padding, line breaks
    acc   = (acc<<6) | uint32_t(v);
    bits += 6;
    if(bits>=8) {
      bits -= 8;
      out.push_back(uint8_t(acc>>bits));
      }
    }
  return out;
  }

// listening to the hero's voice, thinking of an answer (tickListener)
constexpr float    ListenRange  = 1500;   // cm: who listens to the hero's voice is this near (talk.py PARTNER_CM)
constexpr float    ListenFront  = 0.34f;  // ...and in front of him (cos of 70 degrees), unless he looks at it
constexpr uint64_t ListenAfter  = 4000;   // ms after the key: it listens on until the final transcript, at most
constexpr uint64_t ThinkMax     = 12000;  // ms: no answer by then, it stops thinking
constexpr uint64_t ThinkVoice   = 2500;   // ms after the text is done (idle): the voice may come that late

// any npc of the world by script symbol (attached or not)
Npc* npcBySymbol(World& world, std::string_view symbol) {
  const size_t id = world.script().findSymbolIndex(symbol);
  return id==size_t(-1) ? nullptr : world.findNpcByInstance(id);
  }

std::vector<std::string> split(std::string_view s) {
  std::vector<std::string> ret;
  std::istringstream       in{std::string(s)};
  std::string              tok;
  while(in >> tok)
    ret.push_back(tok);
  return ret;
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
    if(auto world = Gothic::inst().world())
      observer.cancelDeeds(*world);
    observer.clear();
    voices.clear();
    resetBodyObservations();
    }
  void onWorldLoaded();
  bool takePendingSession();
  void skipIntro(World& world);
  void startBranch(std::string parent, int64_t parentSeq, std::string_view reason);
  void sendSession();
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
  std::string                  listener;       // the npc listening to the hero's voice
  std::string                  lastAddressee;  // whom the server said the hero spoke to last
  std::string                  thinker;        // the npc thinking of an answer
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

AiBridge::Impl::Impl() {
  const char* envUrl = std::getenv("GOTHIC_AI_URL");
  url = (envUrl!=nullptr && envUrl[0]!='\0') ? envUrl : DefaultUrl;

  auto& gothic = Gothic::inst();
  gothic.onTick              .bind(this,&Impl::onTick);
  gothic.onStartLoading      .bind(this,&Impl::onStartLoading);
  // the voices play on the session's sound device: let them go before it (quitting the game, back to the menu)
  gothic.onSessionUnload     .bind(this,&Impl::onSessionUnload);
  gothic.onWorldLoaded       .bind(this,&Impl::onWorldLoaded);
  gothic.onExternalPerception.bind(this,&Impl::onPerception);
  gothic.onObservedPerception.bind(this,&Impl::onObservedPerception);
  gothic.onPhysicsNoiseHeard .bind(this,&Impl::onPhysicsNoiseHeard);
  gothic.onCombatHit         .bind(this,&Impl::onCombatHit);
  gothic.onHeroDeed          .bind(this,&Impl::onHeroDeed);
  gothic.interceptPerception = [this](Npc& self, Npc& other, Npc* victim, PercType perc, const HeroDeed* deed) {
    World* world = Gothic::inst().world();
    // the wolves' own (they leave the hero be) without the server too: the console drives them on the bench
    return world!=nullptr &&
           (beasts.intercept(*world,self,other,victim,perc) ||
            (connected && observer.intercept(*world,self,other,victim,perc,deed,
                                            deed!=nullptr ? session.branch+"/deed-"+std::to_string(deed->id) : "")));
    };
  gothic.interceptSvm = [this](Npc& npc, std::string_view svm, std::string_view line, Npc* target, bool overlay) {
    return onSvmLine(npc,svm,line,target,overlay);
    };
  gothic.consoleCommandFn = [this](std::string_view cmd, std::string& reply) {
    return onConsoleCommand(cmd,reply);
    };
  gothic.externalMessageFn = [this](std::string_view id, std::string_view& text, uint32_t& time) {
    if(lines.lookup(id,text,time))
      return true;
    if(!originalLines.empty())
      onOriginalLine(id);
    return false;
    };
  gothic.externalSoundFn = [this](std::string_view file, std::vector<uint8_t>& wav) {
    return lines.sound(file,wav);
    };
  // both run on the loading thread
  gothic.saveExtFn = [this](Serialize& fout) {
    std::lock_guard<std::mutex> guard(saveSync);
    fout.setEntry(SaveEntry);
    fout.write(saveRecord);
    };
  gothic.sessionStartFn = [this](Serialize* fin) {
    std::string rec;
    if(fin!=nullptr && fin->setEntry(SaveEntry))
      fin->read(rec);
    std::lock_guard<std::mutex> guard(saveSync);
    pendingSession  = true;
    pendingFromSave = fin!=nullptr;
    pendingRecord   = std::move(rec);
    };
  debugTimer.timeout.bind(this,&Impl::runDebug);
  bodyTimer.timeout.bind(this,&Impl::pumpBodyControl);
  bodyTimer.start(50);
  if(TestBench::isActive())
    TestBench::aiReached = [this](std::string_view key, std::string_view stage) {
      return key=="necro" && necro.reached(stage);
      };
  if(TestBench::isActive())
    TestBench::aiBusy = [this]() {
      for(auto& v:voices)
        if(v.drainedAt==0)
          return true;
      // still thinking of the answer: the "Хм..." said at once was not it
      if(auto w = Gothic::inst().world(); w!=nullptr && !thinker.empty() &&
         w->tickCount()<thinkUntil && thinkUntil<=w->tickCount()+ThinkMax)
        return true;
      return lines.pending();
      };
  lines.onStart   = [this](const LineLibrary::Line& l) { onLineStarted(l); };
  graph.chosen    = [this](Npc& npc, const std::string& node) {
    if(auto world = Gothic::inst().world())
      triggers.onDialog(*world,npc,node);
    };
  ctrl.onFollowEnded = [this](const std::string& npc, std::string_view) {
    // following on its own ended: back to its routine (the server takes it again if it wants)
    std::string err;
    if(auto world = Gothic::inst().world())
      ctrl.detach(*world,npc,"follow_ended",err);
    };

  ix::initNetSystem();
  ws.setUrl(url);
  ws.setMinWaitBetweenReconnectionRetries(500);
  ws.setMaxWaitBetweenReconnectionRetries(3000);
  ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    // websocket thread: hand over to the main thread
    Inbound in;
    switch(msg->type) {
      case ix::WebSocketMessageType::Open:
        in.kind = Inbound::Open;
        break;
      case ix::WebSocketMessageType::Close:
        in.kind = Inbound::Close;
        in.text = msg->closeInfo.reason;
        break;
      case ix::WebSocketMessageType::Error:
        in.kind = Inbound::Error;
        in.text = msg->errorInfo.reason;
        break;
      case ix::WebSocketMessageType::Message:
        in.kind = Inbound::Message;
        in.text = msg->str;
        break;
      default:
        return;
      }
    std::lock_guard<std::mutex> guard(sync);
    inbound.push_back(std::move(in));
    });
  ws.start();
  Log::i("[ai] bridge started, server: ",url);
  }

AiBridge::Impl::~Impl() {
  bodyTimer.stop();
  auto& gothic = Gothic::inst();
  gothic.onTick              .ubind(this,&Impl::onTick);
  gothic.onStartLoading      .ubind(this,&Impl::onStartLoading);
  gothic.onSessionUnload     .ubind(this,&Impl::onSessionUnload);
  gothic.onWorldLoaded       .ubind(this,&Impl::onWorldLoaded);
  gothic.onExternalPerception.ubind(this,&Impl::onPerception);
  gothic.onObservedPerception.ubind(this,&Impl::onObservedPerception);
  gothic.onPhysicsNoiseHeard .ubind(this,&Impl::onPhysicsNoiseHeard);
  gothic.onCombatHit         .ubind(this,&Impl::onCombatHit);
  gothic.onHeroDeed          .ubind(this,&Impl::onHeroDeed);
  gothic.interceptPerception = nullptr;
  gothic.interceptSvm       = nullptr;
  gothic.consoleCommandFn  = nullptr;
  gothic.externalMessageFn = nullptr;
  gothic.externalSoundFn   = nullptr;
  gothic.saveExtFn         = nullptr;
  gothic.sessionStartFn    = nullptr;
  TestBench::aiBusy        = nullptr;
  TestBench::aiReached     = nullptr;
  BodyObservations::disable();
  voices.clear();  // before the game's sound device goes
  chat.reset();
  ws.stop();
  ix::uninitNetSystem();
  }

void AiBridge::Impl::onTick(uint64_t /*dt*/) {
  BodyController::pause(Gothic::inst().isPause());
  BodyController::pumpWatchdogs();
  finishBodyReset(Gothic::inst().world());
  std::vector<Inbound> queue;
  World* world = nullptr;
  {
    FrameProfile::Scope timing(FrameProfile::Zone::AiBridge);
    {
      std::lock_guard<std::mutex> guard(sync);
      queue.swap(inbound);
      }
    world = Gothic::inst().world();
    for(auto& in:queue)
      processInbound(world,in);
    }

  if(world!=nullptr) {
    ctrl.tick(*world);
    beasts.tick(*world);  // its reports go nowhere without the server; the console drives it then
    triggers.tick(*world);  // the director's story goes on without the server too
    if(connected) {
      observer.tick(*world);
      view.tick(*world);
      story.tick(*world);
      // Samples are taken by SimulationStep after world/player/animation updates. This bridge
      // callback precedes those updates, so transport sends the previous completed sample here.
      // A slow socket leaves rows in the bounded builder queue: its counters expose overflow.
      // Do not keep appending this optional stream to IXWebSocket's unbounded transmit buffer.
      if(BodyObservations::enabled() && BodyObservations::queuedCount()>0 && ws.bufferedAmount()<65536) {
        auto packet = BodyObservations::drainJson();
        if(ws.sendText(JsonWriter("body_observations").raw("packet",packet).finish()).success)
          BodyObservations::markDrainedSent();
        }
      if(BodyObservations::enabled() && ws.bufferedAmount()<65536) {
        auto events = BodyController::drainJson();
        // The controller event queue is bounded and shared with the runner encoder.
        rapidjson::Document envelope;
        envelope.Parse(events.c_str(),events.size());
        if(envelope.HasMember("events") && !envelope["events"].Empty())
          send(std::move(events));
        for(auto& report:BodyController::drainPolicyJson())
          send(std::move(report));
        }
      }
    tickVoices(*world);
    tickDeal(*world);
    tickListener(*world);
    necro.tick(*world);
    }
  ensureChat();
  chat->setHidden(necro.hidesChat());
  chat->tick();
  }

void AiBridge::Impl::processInbound(World* world, Inbound& in) {
  switch(in.kind) {
    case Inbound::Open:
      replaceSvm = false;
      // Reconnection invalidates old requests immediately, while an old owner's current locked
      // animation reaches its safe boundary before observation IDs are reset.
      BodyController::disconnect("");
      bodyResetPending = true;
      finishBodyReset(world);
      BodyController::pause(Gothic::inst().isPause());
      connected = true;
      lastError.clear();
      Log::i("[ai] connected to ",url);
      send(JsonWriter("hello")
             .integer("protocol",ProtocolVersion)
             .str("client","opengothic-ai-bridge")
             .str("codepage","cp1251")
             .raw("body_net",BodyEnginePolicy::netStatusJson())
             .finish());
      session.reason = "connect";
      sendSession();
      if(world!=nullptr)
        sendWorld("world_loaded",world);
      break;
    case Inbound::Close:
    case Inbound::Error:
      replaceSvm = false;
      BodyController::disconnect("");
      if(connected) {
        Log::i("[ai] disconnected: ",in.text);
        Gothic::inst().onPrint(TextCodec::fromUtf8("Связь с ИИ-сервером потеряна"));
        // nobody drives the npc's anymore: hand them back to their routines, threats to their scripts
        if(world!=nullptr) {
          if(auto* pl = world->player())
            pl->setPlayerPose(false);
          ctrl.detachAll(*world,"server_disconnected");
          observer.releaseAll(*world);
          beasts.releaseAll(*world);
          }
        observer.setHoldThreats(false);
        observer.setHoldDeeds(false);
        observer.setHoldBandits(nullptr,false);
        if(chat!=nullptr)
          chat->hideDeal("");  // nobody is there to carry it out
        }
      else if(in.kind==Inbound::Error && in.text!=lastError) {
        Log::i("[ai] connection failed: ",in.text);
        }
      connected = false;
      if(in.kind==Inbound::Error)
        lastError = in.text;
      break;
    case Inbound::Message:
      handle(world,in.text);
      break;
    }
  }

void AiBridge::Impl::onStartLoading() {
  if(Gothic::inst().checkLoading()==Gothic::LoadState::Saving) {
    // the same session continues after saving: npc's stay attached
    saving = true;
    return;
    }
  saving = false;
  // Deliver cancellation with the old identity before reset invalidates and clears its
  // bounded event stream. The loading notification follows it on the same transport.
  BodyController::cancelPending("loading");
  if(connected)
    send(BodyController::drainJson());
  resetBodyObservations();
  if(chat!=nullptr) {
    chat->closeInput();
    chat->hideDeal("");
    }
  voices.clear();  // the world and its sound device are about to go
  lastLine.clear();
  originalLines.clear();
  originalTo.clear();
  lines.clear();
  listener.clear();
  thinker.clear();
  pttHeld = false;
  listenUntil = thinkUntil = 0;
  if(auto world = Gothic::inst().world())
    observer.cancelDeeds(*world);
  observer.clear();
  view.clear();
  Log::i("[story] ",graph.status(),"; ",triggers.status());
  // the server sends the patches of the new branch
  graph.clear();
  triggers.clear();
  triggers.clearWorld();
  runtime.clear();
  story.clear();
  necro.clear();
  if(auto world = Gothic::inst().world()) {
    ctrl.detachAll(*world,"loading");
    beasts.releaseAll(*world);
    } else {
    ctrl.dropAll("loading");
    }
  beasts.clear();
  send(JsonWriter("loading").finish());
  }

void AiBridge::Impl::onWorldLoaded() {
  if(saving) {
    saving = false;
    send(JsonWriter("saved").str("branch",session.branch).integer("seq",session.seq).finish());
    return;
    }
  const bool newGame = takePendingSession();
  if(newGame && !CommandLine::inst().doIntro() && Gothic::inst().world()!=nullptr)
    skipIntro(*Gothic::inst().world());
  sendWorld("world_loaded",Gothic::inst().world());
  }

void AiBridge::Impl::skipIntro(World& world) {
  // -nointro (autotests): the important dialogs of those around the hero at the start count as told, as
  // `debug skip_important` does: Xardas does not stop him for his first talk (and the addon's intro video)
  Npc* pl = world.player();
  if(pl==nullptr)
    return;
  uint32_t near = 0;
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    Npc* npc = world.npcById(i);
    if(npc==nullptr || npc==pl || npc->qDistTo(*pl)>IntroCm*IntroCm)
      continue;
    world.script().skipImportantInfos(*npc,*pl);
    near++;
    }
  Log::i("[ai] no intro: the important dialogs of ",near," npc's around the hero count as told");
  }

bool AiBridge::Impl::takePendingSession() {
  bool        pending  = false;
  bool        fromSave = false;
  std::string rec;
  {
    std::lock_guard<std::mutex> guard(saveSync);
    std::swap(pending,pendingSession);
    fromSave = pendingFromSave;
    rec      = std::move(pendingRecord);
  }
  if(!pending) {
    // a world change: same line of play
    if(!session.branch.empty())
      return false;
    startBranch("",0,"new_game");
    return true;
    }
  std::string parent;
  int64_t     parentSeq = 0;
  if(fromSave && !rec.empty()) {
    rapidjson::Document doc;
    doc.Parse(rec.c_str(),rec.size());
    if(!doc.HasParseError() && doc.IsObject()) {
      parent    = std::string(JsonRead::str(doc,"branch"));
      parentSeq = JsonRead::integer(doc,"seq",0);
      }
    }
  startBranch(std::move(parent),parentSeq,fromSave ? "load" : "new_game");
  return !fromSave;
  }

void AiBridge::Impl::startBranch(std::string parent, int64_t parentSeq, std::string_view reason) {
  session.branch    = newBranchId();
  session.parent    = std::move(parent);
  session.parentSeq = session.parent.empty() ? 0 : parentSeq;
  session.seq       = session.parentSeq;
  session.reason    = std::string(reason);
  updateSaveRecord();
  if(session.parent.empty())
    Log::i("[ai] session ",session.branch," (",reason,")"); else
    Log::i("[ai] session ",session.branch," (",reason," from ",session.parent,"@",session.parentSeq,")");
  sendSession();
  }

void AiBridge::Impl::sendSession() {
  if(session.branch.empty())
    return;
  JsonWriter j("session");
  j.str("branch",session.branch);
  if(!session.parent.empty())
    j.str("parent",session.parent).integer("parent_seq",session.parentSeq);
  j.str("reason",session.reason);
  send(j.finish());
  }

void AiBridge::Impl::updateSaveRecord() {
  auto rec = JsonWriter("ai_session").str("branch",session.branch).integer("seq",session.seq).finish();
  std::lock_guard<std::mutex> guard(saveSync);
  saveRecord = std::move(rec);
  }

void AiBridge::Impl::onPerception(Npc& self, Npc& other, Npc* victim, PercType perc) {
  if(auto world = Gothic::inst().world())
    ctrl.perception(*world,self,other,victim,perc);
  }

void AiBridge::Impl::onObservedPerception(Npc& self, Npc& other, Npc* victim, PercType perc) {
  if(!connected)
    return;
  if(auto world = Gothic::inst().world())
    observer.perception(*world,self,other,victim,perc);
  }

void AiBridge::Impl::onHeroDeed(const HeroDeed& d) {
  if(!connected)
    return;
  const bool npcDeed = d.culprit.npc!="PLAYER";
  JsonWriter j(npcDeed ? "npc_deed" : "hero_deed");
  if(npcDeed)
    j.str("culprit",d.culprit.npc).integer("culprit_guild",d.culprit.guild)
     .integer("culprit_npctype",d.culprit.npctype);
  j.str("id",session.branch+"/deed-"+std::to_string(d.id))
   .str("kind",d.kind).str("item",d.item).integer("count",int64_t(d.count))
   .str("owner",d.owner.npc).integer("owner_guild",d.ownerGuild)
   .integer("owner_npc_guild",d.owner.guild).integer("owner_npctype",d.owner.npctype)
   .boolean("item_dropped",d.itemDropped)
   .str("room",d.room).str("room_owner",d.roomOwner.npc).integer("room_guild",d.roomGuild)
   .integer("room_owner_guild",d.roomOwner.guild).integer("room_owner_npctype",d.roomOwner.npctype)
   .str("chest",d.chest).str("chest_owner",d.chestOwner.npc).integer("chest_guild",d.chestGuild)
   .integer("chest_owner_guild",d.chestOwner.guild).integer("chest_owner_npctype",d.chestOwner.npctype)
   .str("victim",d.victim.npc).integer("victim_guild",d.victim.guild).integer("victim_npctype",d.victim.npctype)
   .boolean("victim_is_monster",d.victimIsMonster).boolean("victim_is_sheep",d.victimIsSheep)
   .boolean("victim_tough_guy",d.victimToughGuy).boolean("victim_in_fight",d.victimInFight)
   .boolean("victim_enemy",d.victimEnemy).boolean("victim_started_fight",d.victimStartedFight)
   .integer("hero_guild",d.heroGuild)
   .object("position").num("x",d.position.x).num("y",d.position.y).num("z",d.position.z).end()
   .str("waypoint",d.waypoint).integer("day",d.day).integer("hour",d.hour).integer("minute",d.minute)
   .array("witnesses");
  for(auto& witness:d.witnesses)
    j.item(witness.npc);
  j.endArray().array("witnesses_metadata");
  for(auto& witness:d.witnesses)
    j.object().str("npc",witness.npc).integer("guild",witness.guild).integer("npctype",witness.npctype).end();
  j.endArray();
  if(d.kind=="pickpocket") {
    const auto inventory = [&](std::string_view key, const std::vector<HeroDeed::InventoryItem>& items) {
      j.array(key);
      for(auto& item:items)
        j.object().str("item",item.item).integer("count",int64_t(item.count)).end();
      j.endArray();
      };
    inventory("inventory_before",d.inventoryBefore);
    inventory("inventory_after",d.inventoryAfter);
    j.integer("picked_pocket_before",d.pickedPocketBefore).integer("picked_pocket_after",d.pickedPocketAfter)
     .boolean("caught",d.caught);
    }
  send(j.finish());
  }

void AiBridge::Impl::onPhysicsNoiseHeard(Npc& self, const PhysicsNoise& noise) {
  if(!connected)
    return;
  if(auto world = Gothic::inst().world())
    observer.physicsNoise(*world,self,noise);
  }

void AiBridge::Impl::onCombatHit(Npc& npc, Npc& by, int32_t damage, std::string_view kind) {
  auto& gothic = Gothic::inst();
  auto* world = gothic.world();
  auto* hero = world!=nullptr ? world->player() : nullptr;
  if(!connected || hero==nullptr || gothic.isInDialog() || damage<=0)
    return;
  const auto nearbyHuman = [&](const Npc& who) {
    return !who.isPlayer() && who.guild()<GIL_SEPERATOR_HUM && who.qDistTo(*hero)<=3000.f*3000.f;
    };
  if(!nearbyHuman(npc) && !nearbyHuman(by))
    return;
  send(JsonWriter("combat_hit")
         .str("npc",npc.isPlayer() ? "PLAYER" : npcSymbol(*world,npc))
         .str("by",by.isPlayer() ? "PLAYER" : npcSymbol(*world,by))
         .integer("damage",damage).integer("hp",npc.attribute(ATR_HITPOINTS))
         .integer("hp_max",npc.attribute(ATR_HITPOINTSMAX))
         .boolean("down",npc.isDown()).boolean("dead",npc.isDead()).str("kind",kind).finish());
  }

bool AiBridge::Impl::onSvmLine(Npc& npc, std::string_view svm, std::string_view line, Npc* target, bool overlay) {
  auto* world = Gothic::inst().world();
  if(world==nullptr || npc.isPlayer() || npc.guild()>=GIL_SEPERATOR_HUM)
    return false;
  const auto symbol = npcSymbol(*world,npc);
  const auto duration = world->script().messageTime(line);
  if(!connected || !replaceSvm) {
    Log::i("[ai] native svm ",symbol," ",svm," line=",line," game_ms=",world->tickCount(),
           " duration_ms=",duration);
    return false;
    }
  send(JsonWriter("svm_line")
         .str("id","svm-"+std::to_string(++svmLines))
         .str("npc",symbol)
         .str("svm",svm)
         .str("text",TextCodec::toUtf8(world->script().messageByName(line)))
         .str("target",target!=nullptr ? npcSymbol(*world,*target) : "")
         .boolean("overlay",overlay)
         .finish());
  Log::i("[ai] svm_line ",symbol," ",svm," game_ms=",world->tickCount()," duration_ms=",duration);
  return true;
  }

std::string_view AiBridge::Impl::npcOwner(std::string_view symbol) const {
  if(ctrl.isAttached(symbol))
    return "attached";
  if(auto world = Gothic::inst().world()) {
    if(auto npc = npcBySymbol(*world,symbol); npc!=nullptr && BodyController::owns(*npc))
      return "body";
    }
  return {};
  }

void AiBridge::Impl::handle(World* world, const std::string& text) {
  rapidjson::Document doc;
  doc.Parse(text.c_str(),text.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    Log::e("[ai] malformed message: ",text);
    return;
    }

  const auto type = JsonRead::str(doc,"type");
  const auto req  = JsonRead::str(doc,"req");

  if(type=="ping") {
    send(JsonWriter("pong").str("req",req).finish());
    return;
    }
  if(type=="player_pose") {
    auto* pl = world!=nullptr ? world->player() : nullptr;
    const auto pose = JsonRead::str(doc,"pose");
    const bool valid = pose=="kneel" || pose=="none";
    const bool ok = pl!=nullptr && valid && pl->setPlayerPose(pose=="kneel");
    sendResult(req,ok,"player_pose requires an available hero and pose kneel or none");
    return;
    }
  if(type=="svm_mode") {
    replaceSvm = JsonRead::boolean(doc,"replace",false);
    return;
    }
  if(type=="memory") {
    if(JsonRead::str(doc,"branch")==session.branch) {
      session.seq = JsonRead::integer(doc,"seq",session.seq);
      updateSaveRecord();
      }
    return;
    }
  if(type=="debug") {
    const auto cmd = JsonRead::str(doc,"cmd");
    if(cmd=="dialogs" || cmd=="choices" || cmd=="choose" || cmd=="eval" || cmd=="check" || cmd=="talk" ||
       cmd=="menu" || cmd=="menu_pick" || cmd=="triggers")
      onDialogDebug(world,doc); else
      onDebug(doc);
    return;
    }
  if(type=="graph_patch") {
    // patches don't need a world: names are checked when there is one
    auto errors = applyPatch(doc);
    std::string all;
    for(auto& e : errors)
      all += (all.empty() ? "" : "; ") + e;
    if(!req.empty())
      sendResult(req,errors.empty(),all);
    return;
    }
  if(type=="threat_control") {
    // the server's behavioral model decides threats to free npc's first (held from the scripts)
    if(doc.HasMember("hold"))
      observer.setHoldThreats(JsonRead::boolean(doc,"hold",false));
    if(doc.HasMember("deeds"))
      observer.setHoldDeeds(JsonRead::boolean(doc,"deeds",false));
    if(doc.HasMember("bandits"))
      observer.setHoldBandits(world,JsonRead::boolean(doc,"bandits",false));
    return;
    }

  if(world==nullptr) {
    if(type=="action")
      send(JsonWriter("action_result")
             .str("id",JsonRead::str(doc,"id"))
             .str("npc",JsonRead::str(doc,"npc"))
             .str("status","rejected")
             .str("detail","no world loaded")
             .finish()); else
      sendResult(req,false,"no world loaded");
    return;
    }

  if(type=="attach") {
    std::string err;
    const bool  ok = ctrl.attach(*world,JsonRead::str(doc,"npc"),err);
    sendResult(req,ok,err);
    }
  else if(type=="body_watch") {
    onBodyWatch(*world,doc);
    }
  else if(type=="body_acquire" || type=="body_goal" || type=="body_action" ||
          type=="body_heartbeat" || type=="body_release" || type=="body_disconnect" ||
          type=="body_control_status" || type=="body_events") {
    onBodyControl(*world,doc);
    }
  else if(type=="detach") {
    std::string err;
    const bool  ok = ctrl.detach(*world,JsonRead::str(doc,"npc"),"server",err);
    sendResult(req,ok,err);
    }
  else if(type=="action") {
    ctrl.action(*world,doc);
    }
  else if(type=="snapshot") {
    // any npc since v7: attached ones on the next tick, free ones at once
    if(!ctrl.requestSnapshot(*world,JsonRead::str(doc,"npc")))
      Log::e("[ai] snapshot of unknown npc: ",JsonRead::str(doc,"npc"));
    }
  else if(type=="story_spawn") {
    std::string err;
    const int   made = story.spawn(*world,doc,err);
    sendResult(req,made>0,err,std::to_string(made));
    }
  else if(type=="story_watch") {
    story.watch(doc);
    }
  else if(type=="story_nearby") {
    auto list = story.nearby(*world,doc);
    sendResult(req,!list.empty(),"no npc " + std::string(JsonRead::str(doc,"npc")),list);
    }
  else if(type=="story_state") {
    sendResult(req,true,"",story.state(*world,doc));
    }
  else if(type=="npc_chests") {
    // v28: the npc's chests and what lies in them
    if(Npc* npc = npcBySymbol(*world,JsonRead::str(doc,"npc")))
      sendResult(req,true,"",NpcChests::of(*world,*npc)); else
      sendResult(req,false,"no npc " + std::string(JsonRead::str(doc,"npc")));
    }
  else if(type=="story_people") {
    sendResult(req,true,"",story.people(*world));
    }
  else if(type=="story_tag") {
    const int found = story.tag(*world,doc);
    sendResult(req,true,"",std::to_string(found));
    }
  else if(type=="story_eval") {
    onStoryEval(*world,doc);
    }
  else if(type=="story_program") {
    // the program a dialog's function runs now (v23), or the scripts' own (`own`, v25), for the director to edit
    std::string err;
    auto json = graph.program(JsonRead::str(doc,"node"),JsonRead::str(doc,"part"),JsonRead::boolean(doc,"own",false),err);
    sendResult(req,!json.empty(),err,json);
    }
  else if(type=="story_npc") {
    // v34: a character of the director's, made once (docs/director.md, stage 2, layer 2)
    std::string err;
    auto        value = StoryCast::npc(*world,doc,err);
    sendResult(req,!value.empty(),err,value);
    }
  else if(type=="story_effect") {
    if(auto npc = npcBySymbol(*world,JsonRead::str(doc,"npc"));
       npc!=nullptr && BodyController::owns(*npc)) {
      BodyController::takeover(*npc,"story_takeover");
      if(BodyController::owns(*npc)) {
        sendResult(req,false,"body control is releasing at a safe boundary; retry story effect");
        return;
        }
      }
    std::string err;
    const bool  ok = story.effects(*world,doc,err);
    sendResult(req,ok,err);
    }
  else if(type=="raise_dead") {
    std::string err;
    const bool  ok = onRaiseDead(*world,doc,err);
    sendResult(req,ok,err);
    }
  else if(type=="npc_act") {
    observer.act(*world,doc);
    }
  else if(type=="npc_view") {
    auto it = doc.FindMember("npcs");
    if(it==doc.MemberEnd())
      sendResult(req,false,"no npcs"); else
      sendResult(req,true,"",view.views(*world,it->value));
    }
  else if(type=="routine") {
    observer.routine(*world,doc);
    }
  else if(type=="hold_verdict") {
    observer.verdict(*world,doc);
    }
  else if(type=="beast") {
    std::string err;
    const bool  ok = beasts.command(*world,doc,err);
    sendResult(req,ok,err);
    }
  else if(type=="say") {
    onSay(*world,doc);
    }
  else if(type=="chat_text") {
    // the reply as it is generated: shown right away, its lines voice it later
    ensureChat();
    const auto symbol = JsonRead::str(doc,"npc");
    const auto text   = TextCodec::fromUtf8(JsonRead::str(doc,"text"));
    Npc*       npc    = npcBySymbol(*world,symbol);
    chat->upsertLine(JsonRead::str(doc,"reply"),
                     npc!=nullptr ? std::string(npc->displayName()) : std::string(symbol),
                     text,LineLibrary::durationFor(text));
    }
  else if(type=="say_audio" || type=="say_end") {
    onSayAudio(*world,doc);
    }
  else if(type=="chat_status") {
    onChatStatus(*world,doc);
    }
  else if(type=="feat_toast") {
    // a feat of the hero (v53): a quiet toast, nothing answers it
    ensureChat();
    chat->showFeat(TextCodec::fromUtf8(JsonRead::str(doc,"title")),
                   TextCodec::fromUtf8(JsonRead::str(doc,"text")),
                   std::string(JsonRead::str(doc,"icon")));
    }
  else if(type=="transcript") {
    onTranscript(doc);
    }
  else if(type=="hush") {
    onHush(*world,doc);
    }
  else if(type=="deal_show") {
    onDealShow(*world,doc);
    }
  else if(type=="deal_hide") {
    if(chat!=nullptr)
      chat->hideDeal(JsonRead::str(doc,"deal"));
    }
  else {
    sendResult(req,false,"unknown message type: " + std::string(type));
    }
  }

void AiBridge::Impl::resetBodyObservations() {
  bodyResetPending = false;
  BodyObservations::reset(++bodyGeneration);
  }

void AiBridge::Impl::finishBodyReset(World* world) {
  if(!bodyResetPending)
    return;
  if(world!=nullptr) {
    for(uint32_t i=0;i<world->npcCount();++i) {
      auto npc = world->npcById(i);
      if(npc!=nullptr && BodyController::owns(*npc))
        return;
      }
    }
  resetBodyObservations();
  }

void AiBridge::Impl::pumpBodyControl() {
  // MainWindow does not tick the world while paused. The application timer still runs on the
  // same main thread, so pause and wall timeout invalidate pending commands without simulation.
  if(Gothic::inst().checkLoading()!=Gothic::LoadState::Idle)
    return;
  BodyController::pause(Gothic::inst().isPause());
  BodyController::pumpWatchdogs();
  finishBodyReset(Gothic::inst().world());
  if(!Gothic::inst().isPause())
    return;
  std::vector<Inbound> queue;
  {
    std::lock_guard<std::mutex> guard(sync);
    for(auto it=inbound.begin();it!=inbound.end();) {
      bool body = false;
      if(it->kind==Inbound::Message) {
        rapidjson::Document msg;
        msg.Parse(it->text.c_str(),it->text.size());
        body = !msg.HasParseError() && msg.IsObject() && JsonRead::str(msg,"type").starts_with("body_");
        }
      if(it->kind!=Inbound::Message || body) {
        queue.push_back(std::move(*it));
        it = inbound.erase(it);
        } else {
        ++it;
        }
      }
  }
  // Reply to body requests and close the transport during pause; legacy gameplay commands keep
  // their existing behavior of waiting for the next world tick.
  auto world = Gothic::inst().world();
  for(auto& in:queue)
    processInbound(world,in);
  }

void AiBridge::Impl::onBodyControl(World& world, const rapidjson::Value& msg) {
  BodyController::pause(Gothic::inst().isPause());
  const auto req = JsonRead::str(msg,"req");
  if(req.empty())
    return;
  if(JsonRead::str(msg,"type")=="body_acquire") {
    if(JsonRead::str(msg,"mode")!="realtime") {
      sendResult(req,false,"ordinary game body control requires realtime mode");
      return;
      }
    auto id = msg.FindMember("entity_id");
    auto npc = id!=msg.MemberEnd() && id->value.IsUint64()
               ? BodyObservations::findEntity(id->value.GetUint64()) : nullptr;
    if(npc!=nullptr) {
      const auto symbol = npcSymbol(world,*npc);
      if(ctrl.isAttached(symbol) || observer.isActing(symbol) || npc->isTalk() ||
         Gothic::inst().isNpcInDialog(*npc)) {
        sendResult(req,false,"NPC already has conversation or soft-action body ownership");
        return;
        }
      }
    }
  // Exactly the same parser, scope checks, lease and resolver as the headless IPC adapter.
  send(JsonWriter("result").str("req",req).boolean("ok",true)
       .raw("value",BodyController::handleJson(msg,true)).finish());
  }

void AiBridge::Impl::onBodyWatch(World& world, const rapidjson::Value& msg) {
  const auto req = JsonRead::str(msg,"req");
  auto npcs = msg.FindMember("npcs");
  auto rate = msg.FindMember("rate_hz");
  if(req.empty() || npcs==msg.MemberEnd() || !npcs->value.IsArray() || npcs->value.Size()>4 ||
     (rate!=msg.MemberEnd() && (!rate->value.IsInt() ||
                              (rate->value.GetInt()!=10 && rate->value.GetInt()!=20)))) {
    sendResult(req,false,"body_watch needs req, up to 4 npc symbols and rate_hz 10 or 20");
    return;
    }
  std::vector<Npc*> watched;
  for(const auto& name:npcs->value.GetArray()) {
    if(!name.IsString() || name.GetStringLength()==0 || name.GetStringLength()>128) {
      sendResult(req,false,"invalid body_watch npc symbol");
      return;
      }
    const auto symbol = AiNpc::upper(std::string_view(name.GetString(),name.GetStringLength()));
    Npc* npc = nullptr;
    if(symbol=="PLAYER" || symbol=="HERO") {
      npc = world.player();
      }
    else {
      const auto instance = world.script().findSymbolIndex(symbol);
      if(instance!=size_t(-1)) {
        npc = world.findNpcByInstance(instance);
        // Script symbols identify types; refuse copies instead of silently watching the first.
        if(world.findNpcByInstance(instance,1)!=nullptr) {
          sendResult(req,false,"ambiguous body_watch npc symbol: "+symbol);
          return;
          }
        }
      }
    if(npc==nullptr || std::find(watched.begin(),watched.end(),npc)!=watched.end()) {
      sendResult(req,false,npc==nullptr ? "unknown body_watch npc: "+symbol :
                                         "duplicate body_watch npc: "+symbol);
      return;
      }
    watched.push_back(npc);
    }
  const uint64_t periodMs = (rate==msg.MemberEnd() || rate->value.GetInt()==20) ? 50 : 100;
  if(BodyController::hasOwners()) {
    const auto reason = BodyController::updateWatch(watched,periodMs);
    if(std::string_view(reason)!="allowed") {
      sendResult(req,false,reason);
      return;
      }
    } else {
    resetBodyObservations();
    if(!watched.empty()) {
      BodyObservations::Config config;
      config.periodMs = periodMs;
      config.maxActors = 4;
      config.queueCapacity = 16;
      BodyObservations::configure(config);
      BodyObservations::watch(watched);
      }
    BodyController::setActors(watched);
    }
  BodyController::pause(Gothic::inst().isPause());
  // Observation alone does not attach a controller, clear AI queues or claim body ownership.
  sendResult(req,true,"",BodyObservations::statsJson());
  }

void AiBridge::Impl::onDebug(const rapidjson::Value& msg) {
  const auto req = JsonRead::str(msg,"req");
  if(!CommandLine::inst().isDevMode()) {
    sendResult(req,false,"debug commands need -devmode");
    return;
    }
  const auto cmd = JsonRead::str(msg,"cmd");
  if(cmd=="spawn" || cmd=="player_weapon" || cmd=="player_equip" || cmd=="waypoints" || cmd=="player_goto" || cmd=="ptt" || cmd=="type" || cmd=="kill" ||
     cmd=="time" || cmd=="teleport" || cmd=="skip_important" || cmd=="deal" || cmd=="deal_accept" ||
     cmd=="deal_decline" || cmd=="npc_goto" || cmd=="npc_los" || cmd=="player_hp" || cmd=="player_drop" || cmd=="chest_take" || cmd=="npc_drop" || cmd=="player_hit" || cmd=="npc_hit" || cmd=="spawn_item" ||
     cmd=="player_take" || cmd=="own_npc" || cmd=="physics_noise" || cmd=="physics_traces" || cmd=="destruction_traces") {
    // world changes that are safe within the tick
    runWorldDebug(msg);
    return;
    }
  if(!debugReq.empty()) {
    sendResult(req,false,"another debug command is pending");
    return;
    }
  debugReq  = std::string(req);
  debugCmd  = std::string(JsonRead::str(msg,"cmd"));
  debugSlot = "save_slot_" + std::to_string(JsonRead::integer(msg,"slot",0)) + ".sav";
  debugTimer.start(1);
  }

void AiBridge::Impl::runDebug() {
  debugTimer.stop();
  const auto req  = std::move(debugReq);
  const auto cmd  = std::move(debugCmd);
  const auto slot = std::move(debugSlot);
  debugReq.clear();
  auto& gothic = Gothic::inst();
  if(gothic.checkLoading()!=Gothic::LoadState::Idle) {
    sendResult(req,false,"busy loading or saving");
    return;
    }
  if(cmd=="save") {
    // the game refuses to save during dialogs and cutscenes
    if(gothic.isInDialog() || (gothic.world()!=nullptr && gothic.world()->currentCs()!=nullptr)) {
      sendResult(req,false,"cannot save now (dialog or cutscene)");
      return;
      }
    gothic.save(slot,"AI test");
    sendResult(req,gothic.checkLoading()==Gothic::LoadState::Saving,"save did not start");
    }
  else if(cmd=="load") {
    gothic.load(slot);
    sendResult(req,true,"");
    }
  else {
    sendResult(req,false,"unknown debug command: " + cmd);
    }
  }

void AiBridge::Impl::runWorldDebug(const rapidjson::Value& msg) {
  const auto req   = JsonRead::str(msg,"req");
  const auto cmd   = JsonRead::str(msg,"cmd");
  World*     world = Gothic::inst().world();
  Npc*       pl    = world!=nullptr ? world->player() : nullptr;
  if(world==nullptr || pl==nullptr) {
    sendResult(req,false,"no world loaded");
    return;
    }
  if(cmd=="npc_los") {
    // Read-only scene preparation: the observer's real eye/terrain rays, independent of its ordinary
    // senses range and field of view. No target, pose or AI state is changed.
    Npc* observer = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    Npc* target = observer!=nullptr ? AiNpc::findNpc(*world,JsonRead::str(msg,"to"),observer) : nullptr;
    if(observer==nullptr || target==nullptr || observer==target) {
      sendResult(req,false,"npc_los needs distinct npc and to targets");
      return;
      }
    const float distance = std::sqrt(observer->qDistTo(*target));
    const float extra = std::max(0.f,5000.f-float(observer->handle().senses_range));
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> value(buf);
    value.StartObject();
    value.Key("distance_cm"); value.Double(distance);
    value.Key("center_clear"); value.Bool(distance<=5000 && observer->canRayHitPoint(target->centerPosition(),true,extra));
    value.Key("head_clear"); value.Bool(distance<=5000 && observer->canRayHitPoint(target->mapHeadBone(),true,extra));
    value.EndObject();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="physics_noise") {
    std::string value, error;
    const bool ok = TestBench::physicsNoiseDebug(*world,*pl,JsonRead::str(msg,"text"),value,error);
    sendResult(req,ok,error,value);
    return;
    }
  if(cmd=="physics_traces") {
    std::string value, error;
    const bool ok = TestBench::physicsTracesDebug(*world,*pl,JsonRead::str(msg,"text"),value,error);
    sendResult(req,ok,error,value);
    return;
    }
  if(cmd=="destruction_traces") {
    std::string value, error;
    const bool ok = TestBench::destructionTracesDebug(*world,*pl,JsonRead::str(msg,"text"),value,error);
    sendResult(req,ok,error,value);
    return;
    }
  if(cmd=="spawn") {
    // a creature next to an npc (or the player): monsters for tests of threats
    std::string inst(JsonRead::str(msg,"instance"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id   = world->script().findSymbolIndex(inst);
    const auto   near = JsonRead::str(msg,"near");
    Npc*         at   = near.empty() ? pl : npcBySymbol(*world,near);
    if(id==size_t(-1) || at==nullptr) {
      sendResult(req,false,id==size_t(-1) ? "unknown instance" : "unknown npc to spawn near");
      return;
      }
    // Prefer the old direction towards the player, else where the npc faces.
    const float dist = float(JsonRead::integer(msg,"distance",600));
    auto        pos  = at->position();
    float       dx   = std::cos(at->rotationRad()), dz = std::sin(at->rotationRad());
    if(at!=pl) {
      const auto  d   = pl->position()-pos;
      const float len = std::sqrt(d.x*d.x+d.z*d.z);
      if(len>1) {
        dx = d.x/len;
        dz = d.z/len;
        }
      }
    pos.x += dist*dx;
    pos.z += dist*dz;
    Npc* spawned = nullptr;
    if(at!=pl && dist>=0) {
      const auto hero = pl->position();
      const auto* physics = world->physic();
      int level = 0, supported = 0;
      const auto grounded = [&](Vec3& point) {
        const float px = point.x-hero.x, pz = point.z-hero.z;
        if(px*px+pz*pz<400.f*400.f)
          return false;
        const auto floor = physics->landRay({point.x,hero.y+100.f,point.z},200.f);
        if(!floor.hasCol || std::abs(floor.v.y-hero.y)>100.f)
          return false;
        // landRay deliberately leaves object normals zero. The footprint below
        // checks their support; a zero normal does not describe a steep slope.
        if(floor.n!=Vec3() && floor.n.y<.8f)
          return false;
        ++level;
        point = floor.v;
        // Support the body, not just its centre on the edge of a floor.
        const std::array<Vec3,4> footprint = {{{30,0,0},{-30,0,0},{0,0,30},{0,0,-30}}};
        for(const auto& offset:footprint) {
          const auto support = physics->landRay(point+offset+Vec3(0,50,0),100.f);
          if(!support.hasCol || std::abs(support.v.y-point.y)>40.f)
            return false;
          }
        if(physics->ray(point+Vec3(0,5,0),point+Vec3(0,100,0)).hasCol)
          return false;
        ++supported;
        // Keep a single creature while testing its actual animated centre: that
        // is the endpoint used by enemy perception, including for attached NPCs.
        if(spawned==nullptr) {
          spawned = world->addNpc(id,point);
          } else {
          spawned->setPosition(point);
          spawned->updateTransform();
          }
        return spawned!=nullptr && at->canRayHitPoint(spawned->centerPosition(),true);
        };
      bool found = grounded(pos);
      // A point on the NPC-to-hero ray can be beyond a wall or on another floor.
      // Search bounded rings around the hero without relaxing the height/LOS gates.
      for(int ring=0; ring<8 && !found; ++ring) {
        const float radius = std::max(dist,401.f)+float(ring)*100.f;
        for(int step=0; step<32 && !found; ++step) {
          const float angle = float(step)*(2.f*float(M_PI)/32.f);
          const float c = std::cos(angle), s = std::sin(angle);
          auto candidate = hero+Vec3(radius*(dx*c-dz*s),0,radius*(dx*s+dz*c));
          if(grounded(candidate)) {
            pos = candidate;
            found = true;
            }
          }
        }
      if(!found) {
        if(spawned!=nullptr)
          world->removeNpc(*spawned);
        sendResult(req,false,"no visible supported ground within 1 m of the hero's height and at least 4 m away "
                             "(level: "+std::to_string(level)+", supported: "+std::to_string(supported)+")");
        return;
        }
      }
    sendResult(req,spawned!=nullptr || world->addNpc(id,pos)!=nullptr,"could not spawn");
    return;
    }
  if(cmd=="own_npc") {
    // an npc of the director's (v33): a symbol of ours made like the script's npc `like`, put at a waypoint
    // or `distance` cm in front of the player. The save keeps both
    std::string why;
    auto*       sym = world->script().ownSymbols().addNpc(JsonRead::str(msg,"npc"),JsonRead::str(msg,"like"),why);
    if(sym==nullptr) {
      sendResult(req,false,why);
      return;
      }
    const auto wp  = JsonRead::str(msg,"waypoint");
    Npc*       npc = nullptr;
    if(!wp.empty()) {
      npc = world->addNpc(sym->index(),wp);
      } else {
      const float dist = float(JsonRead::integer(msg,"distance",300));
      auto        pos  = pl->position();
      pos.x += dist*std::cos(pl->rotationRad());
      pos.z += dist*std::sin(pl->rotationRad());
      npc = world->addNpc(sym->index(),pos);
      }
    if(npc==nullptr) {
      sendResult(req,false,"could not put " + sym->name() + " in the world");
      return;
      }
    if(npc->visualSkeleton()==nullptr) {
      // the code it is made like gave it no body: the world cannot have it (perception needs a head)
      world->removeNpc(*npc);
      sendResult(req,false,"the code of " + std::string(JsonRead::str(msg,"like")) + " gives " + sym->name() + " no body");
      return;
      }
    // {npc, index: its symbol, name: as the game shows it}
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    const auto                                 shown = TextCodec::toUtf8(npc->displayName());
    w.StartObject();
    w.Key("npc");   w.String(sym->name().data(),rapidjson::SizeType(sym->name().size()));
    w.Key("index"); w.Uint(sym->index());
    w.Key("name");  w.String(shown.data(),rapidjson::SizeType(shown.size()));
    w.EndObject();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="teleport") {
    // the player `distance` cm in front of an npc, facing it: the npc is in view
    Npc* at = npcBySymbol(*world,JsonRead::str(msg,"near"));
    if(at==nullptr || at==pl || !pl->setInteraction(nullptr)) {
      sendResult(req,false,at==nullptr || at==pl ? "unknown npc to teleport to" : "the player is busy");
      return;
      }
    const float dist = float(JsonRead::integer(msg,"distance",600));
    auto        pos  = at->position();
    pos.x += dist*std::cos(at->rotationRad());
    pos.z += dist*std::sin(at->rotationRad());
    // Search from the NPC's body height, not above an overhead floor or rock shelf.
    const auto land = world->physic()->landRay({pos.x,at->centerPosition().y,pos.z},1500);
    if(land.hasCol)
      pos.y = land.v.y;
    pl->setPosition(pos);
    pl->setDirection(at->position()-pos);
    pl->updateTransform();
    if(auto c = Gothic::inst().camera())
      c->reset(pl);
    sendResult(req,true,"");
    return;
    }
  if(cmd=="npc_goto") {
    // an npc steps to a waypoint (or `distance` cm in front of npc `to`), on the ground, facing `face`
    // (v15). It stays free and goes on with what it does: its routine may walk it back (start_activity
    // keeps it there). Unlike the bench's `put` it is not frozen
    const auto symbol = JsonRead::str(msg,"npc");
    Npc*       npc    = npcBySymbol(*world,symbol);
    if(npc==nullptr || npc==pl || npc->isDead() || npc->isDown() || npc->isTalk() ||
       ctrl.find(*world,symbol)!=nullptr) {
      sendResult(req,false,"npc not found or busy");
      return;
      }
    Tempest::Vec3 pos;
    if(auto name = JsonRead::str(msg,"waypoint"); !name.empty()) {
      auto wp = world->findPoint(name,false);
      if(wp==nullptr) {
        sendResult(req,false,"unknown waypoint");
        return;
        }
      pos = wp->groundPos;
      }
    else if(Npc* to = npcBySymbol(*world,JsonRead::str(msg,"to"))) {
      const float d = float(JsonRead::integer(msg,"distance",300));
      pos = to->position();
      pos.x += d*std::cos(to->rotationRad());
      pos.z += d*std::sin(to->rotationRad());
      const auto land = world->physic()->landRay({pos.x,to->centerPosition().y,pos.z},1500);
      if(land.hasCol)
        pos.y = land.v.y;
      }
    else {
      sendResult(req,false,"npc_goto needs waypoint or to");
      return;
      }
    npc->setInteraction(nullptr,true);  // off a bench or a stove first
    npc->clearAiQueue();                // a walk under way would take it back at once
    npc->setPosition(pos);
    if(Npc* face = npcBySymbol(*world,JsonRead::str(msg,"face")); face!=nullptr && face!=npc)
      npc->setDirection(face->position()-pos);
    npc->updateTransform();
    sendResult(req,true,"");
    return;
    }
  if(cmd=="skip_important") {
    // an npc's important dialogs count as told: it does not stop the hero to talk (the start of the game:
    // most npc's have one, and a test scene with them near the hero would end in it)
    Npc* npc = npcBySymbol(*world,JsonRead::str(msg,"near"));
    if(npc==nullptr || npc==pl) {
      sendResult(req,false,"unknown npc");
      return;
      }
    world->script().skipImportantInfos(*npc,*pl);
    sendResult(req,true,"");
    return;
    }
  if(cmd=="deal") {
    // what the deal window shows
    sendResult(req,true,"",dealJson());
    return;
    }
  if(cmd=="deal_accept") {
    // Y as the player presses it
    ensureChat();
    const std::string r = chat->canDeal() ? chat->pressDeal() : "blocked";
    sendResult(req,true,"","\"" + r + "\"");
    return;
    }
  if(cmd=="deal_decline") {
    // N as the player presses it
    ensureChat();
    const std::string r = chat->canDeal() ? chat->declineDeal() : "blocked";
    sendResult(req,true,"","\"" + r + "\"");
    return;
    }
  if(cmd=="time") {
    // the clock goes to `hour`:`minute`, the next day if that is earlier than now (like sleeping)
    world->setDayTime(int32_t(JsonRead::integer(msg,"hour",8)),int32_t(JsonRead::integer(msg,"minute",0)));
    sendResult(req,true,"");
    return;
    }
  if(cmd=="kill") {
    // `npc`: that npc dies, killed by no one (a quest giver for the director's repairs).
    // `instance`: the living creature of that instance (WOLF) closest to an npc (`near`, default the player)
    // dies by the hand of `by` (default the player), the way a deadly hit ends it (Npc::takeDamage): in melee,
    // right in front of the killer; npc's around, the killer too, perceive it (ASSESSMURDER)
    if(auto name = JsonRead::str(msg,"npc"); !name.empty()) {
      Npc* npc = npcBySymbol(*world,name);
      if(npc==nullptr || npc->isDead() || npc->isPlayer()) {
        sendResult(req,false,npc==nullptr ? "no such npc" : "it can't die now");
        return;
        }
      npc->changeAttribute(ATR_HITPOINTS,-npc->attribute(ATR_HITPOINTSMAX)-1,false);
      sendResult(req,npc->isDead(),"it did not die (immortal?)");
      return;
      }
    const auto near   = JsonRead::str(msg,"near");
    const auto byName = JsonRead::str(msg,"by");
    Npc*       at     = near.empty() ? pl : npcBySymbol(*world,near);
    Npc*       by     = byName.empty() || byName=="PLAYER" ? pl : npcBySymbol(*world,byName);
    Npc*       victim = at!=nullptr ? AiNpc::findNpc(*world,JsonRead::str(msg,"instance"),at) : nullptr;
    if(victim==nullptr || victim==pl || victim->isDead() || by==nullptr || by==victim) {
      sendResult(req,false,by==nullptr ? "no such killer" : "no such creature alive");
      return;
      }
    auto at1 = by->position();
    at1.x += 150*std::cos(by->rotationRad());
    at1.z += 150*std::sin(by->rotationRad());
    victim->setPosition(at1);
    victim->setLastHitBy(by);  // as a deadly hit leaves it: whose kill it is (hero_kill, v17)
    victim->changeAttribute(ATR_HITPOINTS,-victim->attribute(ATR_HITPOINTS),false);
    world->sendPassivePerc(*victim,*by,*victim,PERC_ASSESSMURDER);
    sendResult(req,victim->isDead(),"it did not die");
    return;
    }
  if(cmd=="player_goto") {
    // the hero steps to a waypoint (walking away from a conversation): a named one, the nearest one at
    // least `distance` cm away from an npc (`near`), or one next to an npc (`to`, `distance` cm off)
    const WayPoint* wp = nullptr;
    if(auto pos = msg.FindMember("pos"); pos!=msg.MemberEnd() && pos->value.IsArray() && pos->value.Size()==3) {
      // a point of the world (v21), on the ground there
      Tempest::Vec3 at(float(pos->value[0].GetDouble()),float(pos->value[1].GetDouble()),
                       float(pos->value[2].GetDouble()));
      const auto land = world->physic()->landRay(at+Tempest::Vec3(0,100,0),1500);
      if(land.hasCol)
        at.y = land.v.y;
      pl->setPosition(at);
      pl->updateTransform();
      sendResult(req,true,"");
      return;
      }
    if(auto name = JsonRead::str(msg,"waypoint"); !name.empty()) {
      wp = world->findPoint(name,false);
      }
    else if(Npc* to = npcBySymbol(*world,JsonRead::str(msg,"to"))) {
      const float off = float(JsonRead::integer(msg,"distance",300));
      const auto  at  = to->position();
      wp = world->findWayPoint(at,[&](const WayPoint& w) {
        return (w.position()-at).quadLength() >= off*off;
        });
      }
    else if(Npc* from = npcBySymbol(*world,JsonRead::str(msg,"near"))) {
      const float min  = float(JsonRead::integer(msg,"distance",1500));
      const auto  at   = from->position();
      wp = world->findWayPoint(pl->position(),[&](const WayPoint& w) {
        return (w.position()-at).quadLength() >= min*min;
        });
      }
    if(wp==nullptr) {
      sendResult(req,false,"unknown waypoint");
      return;
      }
    pl->setPosition(wp->groundPos);
    pl->updateTransform();
    sendResult(req,true,"");
    return;
    }
  if(cmd=="ptt") {
    // the T key as the player presses it (start), holds and lets go (stop) or taps it (cancel): the
    // focus is the hero's own
    const auto state = JsonRead::str(msg,"state");
    if(state!="start" && state!="stop" && state!="cancel") {
      sendResult(req,false,"ptt state: start, stop or cancel");
      return;
      }
    sendResult(req,true,"");
    sendPtt(state=="start" ? "ptt_start" : (state=="stop" ? "ptt_stop" : "ptt_cancel"));
    return;
    }
  if(cmd=="type") {
    // a line typed into the input line and sent
    sendResult(req,true,"");
    submitChat(TextCodec::fromUtf8(JsonRead::str(msg,"text")));
    return;
    }
  if(cmd=="waypoints") {
    // every waypoint and freepoint of the world: for the server's book of places (places.py). v32: the
    // waynet's links of each, how much of the sky above it is covered (`roof`: of five rays up, straight
    // and tilted, how many hit rock or a roof within RoofCm) and the room it is in (`room`, indoors)
    constexpr float RoofCm   = 2000;
    constexpr float RoofTilt = 0.57f;  // sin of 35°
    const auto*     phys     = world->physic();
    std::string     out      = "[";
    char            num[128];
    world->forEachWayPoint([&](const WayPoint& w) {
      const auto pos = w.position();
      int        roof = 0;
      const auto eye  = pos+Tempest::Vec3(0,50,0);
      const Tempest::Vec3 dirs[] = {{0,1,0},
                                    {RoofTilt,0.82f,0},{-RoofTilt,0.82f,0},{0,0.82f,RoofTilt},{0,0.82f,-RoofTilt}};
      for(auto& d:dirs)
        if(phys->ray(eye,eye+d*RoofCm).hasCol)
          ++roof;
      std::snprintf(num,sizeof(num),"\",\"x\":%.0f,\"y\":%.0f,\"z\":%.0f,\"fp\":%s,\"roof\":%d",
                    double(pos.x),double(pos.y),double(pos.z),w.isFreePoint() ? "true" : "false",roof);
      out += (out.size()>1 ? ",{\"name\":\"" : "{\"name\":\"") + w.name + num;
      const auto land = phys->landRay(eye,500);
      if(land.hasCol && land.sector!=nullptr && land.sector[0]!='\0')
        out += std::string(",\"room\":\"") + land.sector + "\"";
      if(!w.connections().empty()) {
        out += ",\"conn\":[";
        for(size_t i=0; i<w.connections().size(); ++i)
          out += (i ? ",\"" : "\"") + w.connections()[i].point->name + "\"";
        out += "]";
        }
      out += "}";
      });
    sendResult(req,true,"",out + "]");
    return;
    }
  if(cmd=="player_equip") {
    // the player gets an item and puts it on (armor of another guild, for the npc's eyes)
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    if(!pl->inventory().itemCount(id))
      pl->addItem(id,1);
    pl->useItem(id);
    sendResult(req,true,"");
    return;
    }
  if(cmd=="player_drop") {
    // the player throws a thing down, as with the game's own key (v22): he is given it first if he has none
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    if(!pl->inventory().itemCount(id))
      pl->addItem(id,1);
    const size_t before = pl->inventory().itemCount(id);
    pl->dropItem(id,1);
    const bool dropped = pl->inventory().itemCount(id)<before;
    sendResult(req,dropped,dropped ? "" : "the hero could not drop it now");
    return;
    }
  if(cmd=="spawn_item") {
    // A normal world item, with the script instance's own ownership. In particular, a public item
    // stays unowned; no test-only owner is manufactured here.
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    const float d = float(JsonRead::integer(msg,"distance",100));
    auto pos = pl->position();
    pos.x += d*std::cos(pl->rotationRad());
    pos.z += d*std::sin(pl->rotationRad());
    const auto land = world->physic()->landRay(pos+Tempest::Vec3(0,100,0),1500);
    if(land.hasCol)
      pos.y = land.v.y;
    Item* item = world->addItem(id,pos);
    if(item!=nullptr)
      item->setCount(size_t(std::max<int64_t>(1,JsonRead::integer(msg,"count",1))));
    sendResult(req,item!=nullptr,item==nullptr ? "the item could not be created" : "");
    return;
    }
  if(cmd=="npc_drop") {
    // The native drop stamps the item's owner with this NPC's script instance. A fixture must use
    // that path too: changing an item's owner in the bridge would conceal ownership bugs.
    Npc* npc = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(npc==nullptr || npc==pl || npc->isDead() || id==size_t(-1)) {
      sendResult(req,false,npc==nullptr ? "unknown npc" : "unknown item or unavailable npc");
      return;
      }
    const size_t n = size_t(std::max<int64_t>(1,JsonRead::integer(msg,"count",1)));
    if(npc->inventory().itemCount(id)<n)
      npc->addItem(id,n-npc->inventory().itemCount(id));
    const size_t before = npc->inventory().itemCount(id);
    npc->dropItem(id,n);
    const bool dropped = npc->inventory().itemCount(id)<before;
    sendResult(req,dropped,dropped ? "" : "the npc could not drop it now");
    return;
    }
  if(cmd=="player_hit") {
    // One real melee hit, including damage calculation and both native damage perceptions.
    Npc* victim = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    if(victim==nullptr || victim==pl || victim->isDown()) {
      sendResult(req,false,"unknown or unavailable victim");
      return;
      }
    if(pl->qDistTo(*victim)>500.f*500.f) {
      sendResult(req,false,"victim is more than five metres away");
      return;
      }
    const auto before = victim->attribute(ATR_HITPOINTS);
    victim->takeDamage(*pl,nullptr);
    const bool damaged = victim->attribute(ATR_HITPOINTS)<before;
    sendResult(req,damaged,damaged ? "" : "the hit caused no damage");
    return;
    }
  if(cmd=="npc_hit") {
    // A confirmed melee contact for a bench: only parry/backstep are bypassed. Native damage,
    // armour, hit reactions and perceptions still identify the real attacker.
    Npc* victim = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    Npc* by     = npcBySymbol(*world,JsonRead::str(msg,"by"));
    if(victim==nullptr || by==nullptr || victim==by || victim==pl || by==pl ||
       victim->isDown() || by->isDown() || victim->isImmortal()) {
      sendResult(req,false,"npc_hit needs distinct living npcs and a mortal victim");
      return;
      }
    if(by->qDistTo(*victim)>500.f*500.f) {
      sendResult(req,false,"victim is more than five metres from the attacker");
      return;
      }
    const auto mode = by->weaponState();
    const auto* weapon = by->inventory().activeWeapon();
    if((mode!=WeaponState::W1H && mode!=WeaponState::W2H) || weapon==nullptr) {
      sendResult(req,false,"npc_hit needs a drawn melee weapon");
      return;
      }
    bool vulnerable = false;
    for(size_t i=0; i<zenkit::DamageType::NUM; ++i)
      if((weapon->handle().damage_type&(1<<i))!=0 && victim->handle().protection[i]>=0)
        vulnerable = true;
    if(!vulnerable) {
      sendResult(req,false,"victim is immune to the attacker's damage");
      return;
      }
    const auto before = victim->attribute(ATR_HITPOINTS);
    const auto gameMs = world->tickCount();
    BodyObservations::noteEvent(*by,victim,"applied",gameMs);
    victim->takeDamage(*by,nullptr,true);
    const auto after = victim->attribute(ATR_HITPOINTS);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> value(buf);
    value.StartObject();
    value.Key("attacker"); value.String(npcSymbol(*world,*by).c_str());
    value.Key("victim"); value.String(npcSymbol(*world,*victim).c_str());
    value.Key("game_ms"); value.Uint64(gameMs);
    value.Key("hp_before"); value.Int(before);
    value.Key("hp_after"); value.Int(after);
    value.EndObject();
    sendResult(req,after<before,after<before ? "" : "the hit caused no damage",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="player_take") {
    // the hero picks up the nearest such thing lying within `distance` cm (v29), as with the game's own key
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    const float range = float(JsonRead::integer(msg,"distance",500));
    Item*       it    = nullptr;
    float       best  = range*range;
    world->detectItem(pl->position(),range,[&](Item& i) {
      const float d = (i.position()-pl->position()).quadLength();
      if(i.clsId()==id && d<best) {
        it   = &i;
        best = d;
        }
      });
    if(it==nullptr) {
      sendResult(req,false,"no " + inst + " lying nearby");
      return;
      }
    const bool taken = pl->takeItem(*it)!=nullptr;
    sendResult(req,taken,taken ? "" : "the hero could not pick it up now");
    return;
    }
  if(cmd=="chest_take") {
    // the hero takes things out of a chest unseen (v28): what its owner remembers is no longer there
    Interactive* chest = NpcChests::find(*world,JsonRead::str(msg,"chest"));
    std::string  inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(chest==nullptr || id==size_t(-1)) {
      sendResult(req,false,chest==nullptr ? "no such chest" : "unknown item " + inst);
      return;
      }
    const size_t n = std::min(chest->inventory().itemCount(id),size_t(std::max<int64_t>(1,JsonRead::integer(msg,"count",1))));
    if(n>0)
      pl->addItem(id,*chest,n);
    sendResult(req,n>0,"it is not in the chest",std::to_string(n));
    return;
    }
  if(cmd=="player_hp") {
    // the player's health and its maximum (v21): a test of creatures that bite lets him live through it
    const auto hp = int32_t(std::clamp<int64_t>(JsonRead::integer(msg,"hp",1000),1,100000));
    pl->changeAttribute(ATR_HITPOINTSMAX,hp-pl->attribute(ATR_HITPOINTSMAX),false);
    pl->changeAttribute(ATR_HITPOINTS,hp-pl->attribute(ATR_HITPOINTS),false);
    sendResult(req,true,"");
    return;
    }
  // player_weapon: the player draws ("melee") or puts away ("none") a weapon
  const auto state = JsonRead::str(msg,"state");
  const bool ok    = state=="none" ? pl->closeWeapon(false) : pl->drawWeaponMelee();
  sendResult(req,ok,"the player cannot switch weapons now");
  }

void AiBridge::Impl::send(std::string&& text) {
  if(!connected)
    return;
  ws.sendText(text);
  }

std::string AiBridge::Impl::storyLine(const std::string& npc, const std::string& text) {
  auto id = "AIB_G" + std::to_string(++graphLines);
  lines.add(id,npc,text);
  askVoice(id,npc,text);
  return id;
  }

std::vector<std::string> AiBridge::Impl::applyPatch(const rapidjson::Value& msg) {
  std::vector<std::string> errors;
  if(JsonRead::boolean(msg,"replace",false)) {
    graph.clear();
    triggers.clear();
    runtime.clear();
    }
  auto it = msg.FindMember("ops");
  if(it==msg.MemberEnd() || !it->value.IsArray())
    return errors;
  size_t i = 0;
  for(auto& op : it->value.GetArray()) {
    std::string err = !op.IsObject()                         ? std::string("not an object")
                    : JsonRead::str(op,"op")=="trigger" ? triggers.apply(op)
                                                             : graph.applyOp(op);
    if(!err.empty()) {
      errors.push_back("op " + std::to_string(i) + ": " + err);
      Log::e("[story] patch op ",i,": ",err);
      }
    ++i;
    }
  Log::i("[story] patch: ",i," operation(s), ",errors.size()," rejected; ",graph.summary(),"; ",triggers.status());
  return errors;
  }

void AiBridge::Impl::sendResult(std::string_view req, bool ok, std::string_view err, std::string_view value) {
  JsonWriter j("result");
  j.str("req",req).boolean("ok",ok);
  if(!ok)
    j.str("error",err);
  if(!value.empty())
    j.raw("value",value);
  send(j.finish());
  }

void AiBridge::Impl::onDialogDebug(World* world, const rapidjson::Value& msg) {
  const auto req = JsonRead::str(msg,"req");
  const auto cmd = JsonRead::str(msg,"cmd");
  if(!CommandLine::inst().isDevMode()) {
    sendResult(req,false,"debug commands need -devmode");
    return;
    }
  if(world==nullptr) {
    sendResult(req,false,"no world loaded");
    return;
    }
  if(cmd=="check") {
    sendResult(req,true,"",graph.checkWorld(*world));
    return;
    }
  if(cmd=="triggers") {
    sendResult(req,true,"",triggers.list(*world));
    return;
    }
  if(cmd=="menu") {
    // the dialog window as the player sees it: what it offers now, none while a line plays
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    if(Gothic::inst().dialogEntriesFn)
      for(auto& e : Gothic::inst().dialogEntriesFn()) {
        auto text = TextCodec::toUtf8(e);
        w.String(text.data(),rapidjson::SizeType(text.size()));
        }
    w.EndArray();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="menu_pick") {
    // picks the entry with this text, as the player does with the keys
    const auto text    = TextCodec::fromUtf8(JsonRead::str(msg,"text"));
    auto       entries = Gothic::inst().dialogEntriesFn ? Gothic::inst().dialogEntriesFn() : std::vector<std::string>();
    auto       at      = std::find(entries.begin(),entries.end(),text);
    const bool ok      = at!=entries.end() && Gothic::inst().dialogPickFn &&
                         Gothic::inst().dialogPickFn(size_t(at-entries.begin()));
    sendResult(req,ok,"the window does not offer it now");
    return;
    }
  const auto symbol = JsonRead::str(msg,"npc");
  Npc*       npc    = symbol.empty() ? nullptr : npcBySymbol(*world,symbol);
  if(npc==nullptr && (cmd!="eval" || !symbol.empty())) {
    sendResult(req,false,"no npc " + std::string(symbol));
    return;
    }
  std::string err;
  if(cmd=="dialogs") {
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for(auto& id : graph.offered(*world,*npc))
      w.String(id.data(),rapidjson::SizeType(id.size()));
    w.EndArray();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    }
  else if(cmd=="talk") {
    // the hero talks to it, as with the action key: the dialog window opens
    Npc* player = world->player();
    if(player!=nullptr)
      npc->startDialog(*player);
    sendResult(req,player!=nullptr,"no player");
    }
  else if(cmd=="choices") {
    // what the dialog menu offers after this dialog's lines: [{id, text}]
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for(auto& [id,text] : graph.choices(*world,*npc,JsonRead::str(msg,"node"))) {
      w.StartObject();
      w.Key("id");   w.String(id.data(),rapidjson::SizeType(id.size()));
      w.Key("text"); w.String(text.data(),rapidjson::SizeType(text.size()));
      w.EndObject();
      }
    w.EndArray();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    }
  else if(cmd=="choose") {
    const bool ok = graph.choose(*world,*npc,JsonRead::str(msg,"node"),err);
    sendResult(req,ok,err);
    }
  else if(cmd=="eval") {
    auto it = msg.FindMember("expr");
    std::string value;
    if(it==msg.MemberEnd() || !graph.evaluate(*world,npc,it->value,value,err))
      sendResult(req,false,err.empty() ? "no expr" : err); else
      sendResult(req,true,"",value);
    }
  }

bool AiBridge::Impl::onRaiseDead(World& world, const rapidjson::Value& msg, std::string& err) {
  const auto symbol = upperSymbol(JsonRead::str(msg,"npc"));
  Npc*       npc    = npcBySymbol(world,symbol);
  if(npc==nullptr) {
    err = "no npc " + symbol;
    return false;
    }
  Necromancy::Card card;
  if(auto it = msg.FindMember("card"); it!=msg.MemberEnd() && it->value.IsObject()) {
    card.image = std::string(JsonRead::str(it->value,"image"));
    card.title = TextCodec::fromUtf8(JsonRead::str(it->value,"title"));
    card.text  = TextCodec::fromUtf8(JsonRead::str(it->value,"text"));
    }
  auto spell = upperSymbol(JsonRead::str(msg,"spell"));
  if(spell.empty())
    spell = "SPL_SKULL";
  return necro.start(world,*npc,std::move(card),std::move(spell),err);
  }

void AiBridge::Impl::onStoryEval(World& world, const rapidjson::Value& msg) {
  // story graph expressions on the live VM, self = `npc`, other = the hero: what the director's rules ask
  const auto req = JsonRead::str(msg,"req");
  Npc*       npc = npcBySymbol(world,JsonRead::str(msg,"npc"));
  auto       it  = msg.FindMember("exprs");
  if(npc==nullptr || it==msg.MemberEnd() || !it->value.IsArray()) {
    sendResult(req,false,npc==nullptr ? "no npc " + std::string(JsonRead::str(msg,"npc")) : "no exprs");
    return;
    }
  std::string out = "[";
  for(auto& e : it->value.GetArray()) {
    std::string value, err;
    if(!graph.evaluate(world,npc,e,value,err))
      value = "null";
    out += (out.size()>1 ? "," : "") + value;
    }
  sendResult(req,true,"",out + "]");
  }

void AiBridge::Impl::sendWorld(std::string_view type, World* world) {
  if(world==nullptr)
    return;
  send(JsonWriter(type).str("world",world->name()).finish());
  }

bool AiBridge::Impl::onConsoleCommand(std::string_view cmd, std::string& reply) {
  auto args = split(cmd);
  if(args.empty() || lower(args[0])!="ai")
    return false;

  World*     world = Gothic::inst().world();
  const auto sub   = args.size()>1 ? lower(args[1]) : std::string("status");

  if(sub=="status") {
    reply  = std::string(connected ? "connected to " : "NOT connected to ") + url;
    reply += "; " + ctrl.status(world);
    reply += "; " + graph.status();
    reply += "; " + triggers.status();
    reply += "; necro " + necro.status();
    reply += "; " + beasts.status();
    return true;
    }

  if(world==nullptr) {
    reply = "no world loaded";
    return true;
    }

  std::string err;
  if(sub=="attach" && args.size()>2) {
    if(ctrl.attach(*world,args[2],err))
      reply = "attached " + args[2] + (connected ? "" : " (server is not connected!)"); else
      reply = err;
    return true;
    }
  if(sub=="detach") {
    if(args.size()>2) {
      reply = ctrl.detach(*world,args[2],"console",err) ? "detached " + args[2] : err;
      } else {
      ctrl.detachAll(*world,"console");
      reply = "detached all";
      }
    return true;
    }

  if(sub=="necro" && args.size()>2) {
    // ai necro <npc> [spell] [picture]: the necromancer's answer without the server (docs/xardas-wrath.md)
    Npc* npc = npcBySymbol(*world,upperSymbol(args[2]));
    if(npc==nullptr) {
      reply = "no npc " + args[2];
      return true;
      }
    Necromancy::Card card;
    card.image = args.size()>4 ? args[4] : "";
    card.title = TextCodec::fromUtf8("Конец");
    const auto spell = args.size()>3 ? upperSymbol(args[3]) : std::string("SPL_SKULL");
    reply = necro.start(*world,*npc,std::move(card),spell,err) ? "necro " + spell : err;
    return true;
    }
  if(sub=="beast" && args.size()>2) {
    // ai beast listen|calm|sit|flee|attack|resume [ms]: the word on the wolf nearest the hero (docs/beasts.md),
    // without the server; sit is calm sitting down
    const auto what = lower(args[2]);
    const auto ms   = args.size()>3 ? int64_t(std::atoll(args[3].c_str())) : int64_t(0);
    reply = beasts.console(*world,what=="sit" ? "calm" : what,ms,what=="sit");
    return true;
    }
  if(sub=="ptt" && args.size()>2) {
    // the T key without a keyboard (the test bench): ai ptt start [<wav> said for the hero] | stop | cancel
    const auto state = lower(args[2]);
    if(state!="start" && state!="stop" && state!="cancel") {
      reply = "usage: ai ptt start [<wav>] | stop | cancel";
      return true;
      }
    std::string audio;
    if(state=="start" && args.size()>3) {
      // the rest of the line: a path may have spaces
      const auto at = cmd.find(args[3]);
      audio = std::string(cmd.substr(at));
      }
    sendPtt(state=="start" ? "ptt_start" : (state=="stop" ? "ptt_stop" : "ptt_cancel"),audio);
    reply = connected ? "ptt " + state : "ptt " + state + " (server is not connected!)";
    return true;
    }

  if(sub=="dialogs" && args.size()>2) {
    auto symbol = args[2];
    for(auto& c : symbol)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    Npc* npc = npcBySymbol(*world,symbol);
    if(npc==nullptr) {
      reply = "no npc " + args[2];
      return true;
      }
    reply = args[2] + " offers:";
    for(auto& id : graph.offered(*world,*npc))
      reply += " " + id;
    return true;
    }
  if(sub=="skip" && args.size()>2) {
    // its important dialogs count as told, as `debug skip_important` does: a shot's npc does not stop the hero
    Npc* npc = npcBySymbol(*world,upperSymbol(args[2]));
    Npc* pl  = world->player();
    if(npc==nullptr || pl==nullptr || npc==pl) {
      reply = "no npc " + args[2];
      return true;
      }
    world->script().skipImportantInfos(*npc,*pl);
    reply = "skipped " + args[2];
    return true;
    }

  reply = "usage: ai status | ai attach <npc> | ai detach [npc] | ai dialogs <npc> | ai skip <npc> | ai ptt start [<wav>]|stop|cancel";
  return true;
  }

AiBridge::AiBridge() {
  stuck = StuckLogger::fromEnvironment();
  const char* env = std::getenv("GOTHIC_AI");
  if(env!=nullptr && std::string_view(env)=="0") {
    Log::i("[ai] bridge disabled (GOTHIC_AI=0)");
    return;
    }
  impl = std::make_unique<Impl>();
  }

AiBridge::~AiBridge() = default;

void AiBridge::Impl::askVoice(const std::string& line, const std::string& npc, const std::string& text) {
  World* world = Gothic::inst().world();
  Npc*   who   = world!=nullptr ? npcBySymbol(*world,npc) : nullptr;
  if(who==nullptr || who->isPlayer())
    return;  // the hero's lines of the director's stay subtitles: his voice is the game's own
  if(voicing.size()>256)
    voicing.clear();  // asked long ago and never answered: no voice service
  voicing.insert(line);
  send(JsonWriter("line_voice")
         .str("npc",npc)
         .str("name",TextCodec::toUtf8(who->displayName()))
         .str("line",line)
         .str("text",TextCodec::toUtf8(text))
         .finish());
  }

void AiBridge::Impl::voiceStoryLine(World& world, const rapidjson::Value& msg, const std::string& id) {
  // the voice comes to the line that is there: queued, or already started without one
  auto* l = lines.find(id);
  if(l==nullptr || l->voice!=nullptr || !JsonRead::boolean(msg,"stream",false))
    return;
  if(l->started && lastLine[l->npc]!=id)
    return;  // too late: the npc has moved on to its next line
  l->voice = std::make_shared<VoiceBuffer>(uint32_t(JsonRead::integer(msg,"rate",24000)));
  if(!l->started)
    return;  // it starts with its voice
  // it started as a subtitle: the voice starts now, and the npc waits for it (shortened once it is all in)
  startVoice(*l);
  if(Npc* npc = npcBySymbol(world,l->npc))
    npc->setAiOutputBarrier(LineLibrary::streamedTime(*l),false);
  }

void AiBridge::Impl::onSay(World& world, const rapidjson::Value& msg) {
  const auto symbol = std::string(JsonRead::str(msg,"npc"));
  const auto line   = std::string(JsonRead::str(msg,"line"));
  if(voicing.erase(line)>0) {
    voiceStoryLine(world,msg,line);
    return;
    }
  Npc*       pl     = world.player();
  Npc*       npc    = ctrl.find(world,symbol);
  if(npc==nullptr) {
    // a free npc (v6) may speak too, unless it is in a dialog or out of action
    npc = npcBySymbol(world,symbol);
    if(npc!=nullptr && (npc->isPlayer() || npc->isDead() || npc->isDown() || npc->isTalk() ||
                        Gothic::inst().isNpcInDialog(*npc)))
      npc = nullptr;
    }
  if(npc==nullptr || pl==nullptr) {
    Log::e("[ai] say: npc cannot speak now: ",symbol," ",line);
    return;
    }
  // whom it is said to (v15): another npc, or the hero
  const auto toSym = std::string(JsonRead::str(msg,"to"));
  Npc*       to    = pl;
  if(!toSym.empty() && toSym!="PLAYER") {
    to = npcBySymbol(world,toSym);
    if(to==nullptr || to==npc || to->isDead() || to->isDown() || Gothic::inst().isNpcInDialog(*to)) {
      Log::e("[ai] say: no one to say it to: ",symbol," -> ",toSym," ",line);
      return;
      }
    }
  const std::string toId = to!=pl ? npcSymbol(world,*to) : std::string();
  if(BodyController::owns(*npc) &&
     (!LineLibrary::isExternal(line) || !JsonRead::boolean(msg,"overlay",false))) {
    BodyController::legacyRejected(*npc,"legacy_queued_speech_rejected");
    Log::i("[ai] say: body provider owns queued speech/turns: ",symbol," ",line);
    return;
    }
  if(!LineLibrary::isExternal(line)) {
    // an original line of the game (v8): subtitle and voice come from the game's own files
    if(Gothic::inst().messageByName(line).empty()) {
      Log::e("[ai] say: no such line: ",line);
      return;
      }
    originalLines[line] = symbol;
    if(!toId.empty())
      originalTo[line] = toId; else
      originalTo.erase(line);
    if(to!=pl)
      npc->aiPush(AiQueue::aiTurnToNpc(to));  // the body turns to the one it speaks to
    npc->aiPush(AiQueue::aiOutput(*to,line,0));
    return;
    }
  auto wav   = fromBase64(JsonRead::str(msg,"audio"));
  auto voice = std::shared_ptr<VoiceBuffer>();
  if(JsonRead::boolean(msg,"stream",false))
    voice = std::make_shared<VoiceBuffer>(uint32_t(JsonRead::integer(msg,"rate",24000)));
  lines.add(line,symbol,TextCodec::fromUtf8(JsonRead::str(msg,"text")),std::move(wav),std::move(voice),
            std::string(JsonRead::str(msg,"reply")));
  lines.find(line)->mood   = std::string(JsonRead::str(msg,"mood"));
  lines.find(line)->filler = JsonRead::boolean(msg,"filler",false);
  lines.find(line)->to     = toId;
  if(!JsonRead::boolean(msg,"overlay",false)) {
    // to an npc it turns first: the line makes the two look at each other, but never turns the body
    if(to!=pl)
      npc->aiPush(AiQueue::aiTurnToNpc(to));
    npc->aiPush(AiQueue::aiOutput(*to,line,0));
    return;
    }
  // on the move (v6): not queued, so the npc keeps doing what it does (running, fighting)
  auto* l = lines.find(line);
  l->overlay = true;
  std::string_view text;
  uint32_t         time = 0;
  lines.lookup(line,text,time);  // starts it: voice stream, subtitle, line_started
  if(!l->wav.empty()) {
    uint64_t len = 0;
    std::shared_ptr<LipSync::Track> lips;
    world.addDlgSound(line + ".WAV",npc->mapHeadBone(),WorldSound::talkRange,len,&lips);
    npc->startSpeech(std::move(lips));
    }
  npc->startFaceAnim("VISEME",1,time);
  // it looks at the one it speaks to while it speaks, and that one at it (a line in the queue does that
  // itself); on the move the body does not turn
  npc->setTalkPartner(to,time+1000);
  to->setTalkPartner(npc,time+1000);
  }

void AiBridge::Impl::onSayAudio(World& world, const rapidjson::Value& msg) {
  auto* line = lines.find(JsonRead::str(msg,"line"));
  if(line==nullptr || line->voice==nullptr)
    return;
  if(JsonRead::str(msg,"type")=="say_audio") {
    const auto pcm = fromBase64(JsonRead::str(msg,"data"));
    line->voice->append(pcm.data(),pcm.size());
    return;
    }
  line->voice->finish();
  for(auto& v:voices)
    if(v.line==line->id)
      correctBarrier(world,v);
  }

void AiBridge::Impl::startVoice(const LineLibrary::Line& line) {
  World*       world   = Gothic::inst().world();
  GameSession* session = Gothic::inst().gameSession();
  Npc*         npc     = world!=nullptr ? npcBySymbol(*world,line.npc) : nullptr;
  if(npc==nullptr || session==nullptr)
    return;
  Voice v;
  v.line      = line.id;
  v.npc       = line.npc;
  v.buf       = line.voice;
  v.startedAt = world->tickCount();
  v.sized     = line.voice->isFinished() || line.overlay;  // nothing to correct then
  v.overlay   = line.overlay;
  v.effect    = session->loadSound(VoiceBuffer::producer(line.voice));
  if(!v.effect.isEmpty()) {
    v.effect.setPosition(npc->mapHeadBone());
    v.effect.setMaxDistance(WorldSound::talkRange);
    v.effect.setVolume(TestBench::voiceStems() ? 0.f : 2.f);  // as WorldSound does for dialog speech
    if(TestBench::voiceStems())
      line.voice->record(TestBench::aiVoiceStem(line.npc,line.id,line.voice->rate()));
    v.effect.play();
    npc->startSpeech(line.voice->lipTrack());  // the mouth follows what the audio thread plays
    }
  Log::i("[ai] voice ",line.id," starts with ",line.voice->durationMs()," ms of audio",
         line.voice->isFinished() ? " (complete)" : " (streaming)");
  voices.push_back(std::move(v));
  }

void AiBridge::Impl::correctBarrier(World& world, Voice& v) {
  if(v.sized)
    return;
  v.sized = true;
  Npc* npc = npcBySymbol(world,v.npc);
  if(npc==nullptr || lastLine[v.npc]!=v.line)
    return;  // the npc has moved on
  const uint64_t end = v.startedAt + v.buf->durationMs() + LineLibrary::VoicePauseMs;
  const uint64_t now = world.tickCount();
  npc->setAiOutputBarrier(end>now ? end-now : 0, false);
  }

void AiBridge::Impl::tickVoices(World& world) {
  const uint64_t now = Application::tickCount();
  for(auto& v:voices) {
    if(v.buf->isFinished() && !v.sized)
      correctBarrier(world,v);
    if(!v.effect.isEmpty())
      if(auto npc = npcBySymbol(world,v.npc))
        v.effect.setPosition(npc->mapHeadBone());
    if(v.drainedAt==0 && v.buf->isDrained()) {
      v.drainedAt = now;
      Log::i("[ai] voice ",v.line," played, ",v.buf->durationMs()," ms");
      TestBench::aiVoiceEnd(v.npc,v.buf->durationMs());
      }
    }
  // keep the source a little after the last sample: OpenAL still holds a buffer of it
  voices.erase(std::remove_if(voices.begin(),voices.end(),[now](const Voice& v) {
    return v.drainedAt!=0 && now>v.drainedAt+500;
    }),voices.end());
  }

void AiBridge::Impl::onChatStatus(World& world, const rapidjson::Value& msg) {
  const auto status = JsonRead::str(msg,"status");
  // the npc thinks of its answer until it starts (the eyes aside, tickListener)
  if(status=="thinking") {
    if(!thinker.empty() && thinker!=JsonRead::str(msg,"npc"))
      if(Npc* old = npcBySymbol(world,thinker))
        old->thinkOfAnswer(0);
    thinker    = std::string(JsonRead::str(msg,"npc"));
    thinkUntil = world.tickCount() + ThinkMax;
    Log::i("[ai] ",thinker," thinks of an answer");
    if(listener==thinker)
      listenUntil = 0;  // it heard him out
    }
  else if(status=="idle")
    thinkUntil = std::min(thinkUntil, world.tickCount() + ThinkVoice);
  else if(status=="error")
    thinkUntil = 0;

  if(chat==nullptr)
    return;
  if(status=="thinking") {
    Npc* npc = npcBySymbol(world,JsonRead::str(msg,"npc"));
    if(npc!=nullptr)
      chat->setStatus(std::string(npc->displayName()) + TextCodec::fromUtf8(" думает..."));
    }
  else if(status=="recognizing")
    chat->setStatus(TextCodec::fromUtf8("Распознаю речь..."));
  else if(status=="notice" || status=="error") {
    chat->setPartial("");
    const auto detail = TextCodec::fromUtf8(JsonRead::str(msg,"detail"));
    chat->setStatus(status=="error" ? TextCodec::fromUtf8("Ошибка: ") + detail : detail);
    }
  else
    chat->setStatus("");
  }

void AiBridge::Impl::onTranscript(const rapidjson::Value& msg) {
  // what the player says: partial while he speaks, the final one as his line, to whom the server decided
  ensureChat();
  const auto text = TextCodec::fromUtf8(JsonRead::str(msg,"text"));
  if(!JsonRead::boolean(msg,"final",true)) {
    chat->setPartial(text);
    // his words came: the one listening may nod
    if(World* world = Gothic::inst().world(); world!=nullptr && !listener.empty())
      if(Npc* npc = npcBySymbol(*world,listener))
        npc->heardWords();
    return;
    }
  if(const auto to = JsonRead::str(msg,"to"); !to.empty())
    lastAddressee = std::string(to);
  chat->setPartial("");
  chat->setStatus("");
  if(text.empty())
    return;
  std::string who = TextCodec::fromUtf8("Ты");
  const auto  to  = JsonRead::str(msg,"to_name");
  TestBench::aiHeard(JsonRead::str(msg,"text"),to);
  if(!to.empty())
    who += " -> " + TextCodec::fromUtf8(to);
  const auto time = LineLibrary::durationFor(text);
  chat->addLine(std::move(who),text,time);

  // the hero and the one he speaks to look at each other
  World* world = Gothic::inst().world();
  Npc*   pl    = world!=nullptr ? world->player() : nullptr;
  Npc*   npc   = world!=nullptr ? npcBySymbol(*world,std::string(JsonRead::str(msg,"to"))) : nullptr;
  if(pl!=nullptr && npc!=nullptr && npc!=pl) {
    pl->setTalkPartner(npc,time+1000);
    npc->setTalkPartner(pl,time+1000);
    }
  }

void AiBridge::Impl::onHush(World& world, const rapidjson::Value& msg) {
  const auto symbol = std::string(JsonRead::str(msg,"npc"));
  lines.hush(symbol);
  // the voice stops with its source
  voices.erase(std::remove_if(voices.begin(),voices.end(),[&](const Voice& v) { return v.npc==symbol; }),
               voices.end());
  if(Npc* npc = npcBySymbol(world,symbol)) {
    npc->setAiOutputBarrier(0,false);  // the line it was saying is over: its queue goes on
    npc->stopDlgAnim();                // gestures and lips
    }
  const auto reply  = JsonRead::str(msg,"reply");
  const auto lineId = JsonRead::str(msg,"line");
  if(chat!=nullptr) {
    // the subtitle keeps what was said: of a reply, or of a line said on its own (v15)
    if(!reply.empty())
      chat->cut(reply,TextCodec::fromUtf8(JsonRead::str(msg,"said"))); else
    if(!lineId.empty())
      chat->cut(lineId,TextCodec::fromUtf8(JsonRead::str(msg,"said")));
    }
  // the npc it was said to listens no more
  if(auto* l = lineId.empty() ? nullptr : lines.find(lineId); l!=nullptr && !l->to.empty())
    if(Npc* to = npcBySymbol(world,l->to))
      to->stopListening();
  }

void AiBridge::Impl::ensureChat() {
  if(chat!=nullptr)
    return;
  chat = std::make_unique<ChatOverlay>();
  chat->canSpeak    = [this]() { return canSpeak(); };
  chat->onSubmit    = [this](const std::string& text) { submitChat(text); };
  chat->onPttStart  = [this]() { sendPtt("ptt_start"); };
  chat->onPttStop   = [this]() {
    sendPtt("ptt_stop");
    chat->setStatus(TextCodec::fromUtf8("Распознаю речь..."));
    };
  chat->onPttCancel = [this]() { sendPtt("ptt_cancel"); };
  chat->canDeal     = [this]() {
    auto&  gothic = Gothic::inst();
    World* world  = gothic.world();
    return connected && world!=nullptr && world->player()!=nullptr && !gothic.isPause();
    };
  chat->onDealAccept = [this](const std::string& deal, int32_t version) {
    send(JsonWriter("deal_accept").str("deal",deal).integer("version",version).finish());
    };
  chat->onDealDecline = [this](const std::string& deal, int32_t version) {
    send(JsonWriter("deal_decline").str("deal",deal).integer("version",version).finish());
    };
  SystemApi::addOverlay(chat.get());
  }

void AiBridge::Impl::onDealShow(World& world, const rapidjson::Value& msg) {
  ensureChat();
  auto lines = [](const rapidjson::Value& m, const char* key) {
    std::vector<ChatOverlay::DealLine> out;
    if(auto it = m.FindMember(key); it!=m.MemberEnd() && it->value.IsArray())
      for(auto& x : it->value.GetArray()) {
        ChatOverlay::DealLine l;
        l.item  = std::string(JsonRead::str(x,"item"));
        l.count = int32_t(JsonRead::integer(x,"count",1));
        l.text  = TextCodec::fromUtf8(JsonRead::str(x,"text"));
        out.push_back(std::move(l));
        }
    return out;
    };
  const auto symbol = JsonRead::str(msg,"npc");
  Npc*       npc    = npcBySymbol(world,symbol);
  chat->showDeal(std::string(JsonRead::str(msg,"deal")),int32_t(JsonRead::integer(msg,"version",1)),
                 npc!=nullptr ? std::string(npc->displayName()) : std::string(symbol),
                 lines(msg,"npc_gives"),lines(msg,"hero_gives"));
  tickDeal(world);
  }

void AiBridge::Impl::tickDeal(World& world) {
  // what the hero lacks of what he gives: the window marks it and Y waits
  Npc* pl = world.player();
  if(chat==nullptr || chat->shownDeal().id.empty() || pl==nullptr)
    return;
  chat->markMissing([&](const ChatOverlay::DealLine& l) {
    if(l.item.empty())
      return false;  // not a thing given now: a lie forgiven, a debt to pay later
    const size_t id = world.script().findSymbolIndex(l.item);
    return id==size_t(-1) || pl->itemCount(id) < size_t(std::max(0,l.count));
    });
  }

std::string AiBridge::Impl::dealJson() {
  if(chat==nullptr || chat->shownDeal().id.empty())
    return "null";
  auto&      d = chat->shownDeal();
  JsonWriter j("deal_window");
  j.str("deal",d.id).integer("version",d.version).str("npc",TextCodec::toUtf8(d.npc));
  j.array("missing");
  for(auto& l:d.heroGives)
    if(l.missing)
      j.item(l.item);
  j.endArray();
  j.array("npc_gives");
  for(auto& l:d.npcGives)
    j.item(TextCodec::toUtf8(l.text));
  j.endArray();
  j.array("hero_gives");
  for(auto& l:d.heroGives)
    j.item(TextCodec::toUtf8(l.text));
  j.endArray();
  j.boolean("sent",d.sent);
  return j.finish();
  }

bool AiBridge::Impl::canSpeak() {
  auto&  gothic = Gothic::inst();
  World* world  = gothic.world();
  Npc*   pl     = world!=nullptr ? world->player() : nullptr;
  if(pl==nullptr || gothic.isPause() || gothic.isInDialog() || pl->isDown())
    return false;
  if(!connected) {
    gothic.onPrint(TextCodec::fromUtf8("ИИ-сервер не подключён"));
    return false;
    }
  return true;
  }

std::string AiBridge::Impl::focusSymbol(World& world) {
  Npc* pl  = world.player();
  Npc* npc = pl!=nullptr ? pl->target() : nullptr;
  if(npc==nullptr || npc->isPlayer() || npc->isDead())
    return {};
  return npcSymbol(world,*npc);
  }

void AiBridge::Impl::submitChat(const std::string& text) {
  World* world = Gothic::inst().world();
  if(world==nullptr || world->player()==nullptr || text.empty())
    return;
  world->player()->setPlayerPose(false);
  // shown once the server has decided whom it is said to (transcript)
  JsonWriter j("player_text");
  j.str("text",TextCodec::toUtf8(text));
  if(auto focus = focusSymbol(*world); !focus.empty())
    j.str("focus",focus);
  send(j.finish());
  }

Npc* AiBridge::Impl::pickListener(World& world) {
  Npc* pl = world.player();
  if(pl==nullptr)
    return nullptr;
  auto near = [&](Npc* n) {
    return n!=nullptr && n!=pl && !n->isDown() && n->isHuman() && (n->position()-pl->position()).length()<=ListenRange;
    };
  // the one he looks at, the one he spoke to last, else the nearest in front of him
  if(Npc* f = pl->target(); near(f))
    return f;
  if(Npc* n = lastAddressee.empty() ? nullptr : npcBySymbol(world,lastAddressee); near(n))
    return n;
  const float a    = pl->rotationRad();
  const Vec3  dir  = {std::cos(a), 0, std::sin(a)};
  Npc*        best = nullptr;
  float       dist = ListenRange;
  world.detectNpc(pl->position(),ListenRange,[&](Npc& n) {
    if(!near(&n))
      return;
    Vec3        d = n.position()-pl->position();
    const float l = std::sqrt(d.x*d.x + d.z*d.z);
    if(l<1.f || (d.x*dir.x + d.z*dir.z)/l<ListenFront || l>=dist)
      return;
    best = &n;
    dist = l;
    });
  return best;
  }

void AiBridge::Impl::tickListener(World& world) {
  const uint64_t now = world.tickCount();
  Npc*           pl  = world.player();
  if(!listener.empty()) {
    Npc* npc = npcBySymbol(world,listener);
    if(npc!=nullptr && pl!=nullptr && (pttHeld || (now<listenUntil && listenUntil<=now+ListenAfter))) {
      npc->listenTo(500);
      // the two look at each other while he speaks
      npc->setTalkPartner(pl,1000);
      pl->setTalkPartner(npc,1000);
      } else {
      if(npc!=nullptr)
        npc->stopListening();
      listener.clear();
      }
    }
  if(!thinker.empty()) {
    Npc* npc = npcBySymbol(world,thinker);
    if(npc!=nullptr && pl!=nullptr && now<thinkUntil && thinkUntil<=now+ThinkMax) {
      npc->thinkOfAnswer(500);
      npc->setTalkPartner(pl,1000);  // the eyes go aside from him, he stays the one it talks to
      } else {
      if(npc!=nullptr)
        npc->thinkOfAnswer(0);
      thinker.clear();
      }
    }
  }

void AiBridge::Impl::sendPtt(std::string_view type, std::string_view audio) {
  JsonWriter j(type);
  if(World* world = Gothic::inst().world())
    if(auto focus = focusSymbol(*world); !focus.empty())
      j.str("focus",focus);
  if(!audio.empty())
    j.str("audio",audio);
  send(j.finish());

  World* world = Gothic::inst().world();
  if(world==nullptr)
    return;
  if(type=="ptt_start") {
    if(auto* pl = world->player())
      pl->setPlayerPose(false);
    pttHeld = true;
    Npc* npc = pickListener(*world);
    if(npc!=nullptr && npcSymbol(*world,*npc)!=listener) {
      if(Npc* old = listener.empty() ? nullptr : npcBySymbol(*world,listener))
        old->stopListening();
      listener = npcSymbol(*world,*npc);
      Log::i("[ai] ",listener," listens to the hero");
      }
    }
  else if(type=="ptt_stop") {
    pttHeld     = false;
    listenUntil = world->tickCount() + ListenAfter;
    }
  else {
    pttHeld     = false;
    listenUntil = 0;
    }
  }

void AiBridge::Impl::onLineStarted(const LineLibrary::Line& line) {
  const bool voiced = !line.wav.empty() || line.voice!=nullptr;
  if(line.npc==thinker && !line.filler)
    thinkUntil = 0;  // it answers: the eyes come back (tickListener); "Хм..." is said thinking
  send(JsonWriter("line_started").str("npc",line.npc).str("line",line.id).boolean("voiced",voiced).finish());
  lastLine[line.npc] = line.id;
  if(line.voice!=nullptr)
    startVoice(line);
  World*      world = Gothic::inst().world();
  Npc*        npc   = world!=nullptr ? npcBySymbol(*world,line.npc) : nullptr;
  Npc*        to    = world!=nullptr && !line.to.empty() ? npcBySymbol(*world,line.to) : nullptr;
  std::string name  = npc!=nullptr ? std::string(npc->displayName()) : line.npc;
  TestBench::aiLine(line.npc,TextCodec::toUtf8(name),TextCodec::toUtf8(line.text),line.time,line.to,line.filler);
  // the face while the line is said, and a moment after (v8); the hands go with the mood too
  if(npc!=nullptr) {
    npc->setTalkMood(line.mood);
    if(auto face = moodFace(line.mood); !face.empty()) {
      npc->startFaceAnim(face,1,line.time+MoodHoldMs);
      Log::i("[ai] face ",line.npc," ",face," for ",line.time+MoodHoldMs," ms");
      }
    }
  if(to!=nullptr) {
    // said to an npc (v15): it listens, as the hero's addressee does (not one that fell since the say: its
    // head would nod on the ground)
    if(!to->isDown())
      to->listenTo(line.time);
    // two free ones standing in ZS_SMALLTALK would chatter into the line: their loop waits anew
    const ScriptFn smalltalk = AiNpc::scriptState(*world,"ZS_SMALLTALK");
    for(Npc* n:{npc,to})
      if(n!=nullptr && smalltalk.isValid() && n->isInState(smalltalk))
        n->setStateTime(0);
    name += " -> " + std::string(to->displayName());
    }
  ensureChat();
  if(!line.reply.empty()) {
    chat->extend(line.reply,line.time);  // its text is on screen already
    return;
    }
  // keyed by the line: a hush cuts it (v15)
  chat->upsertLine(line.id,std::move(name),line.text,line.time);
  }

void AiBridge::Impl::onOriginalLine(std::string_view id) {
  auto it = originalLines.find(std::string(id));
  if(it==originalLines.end())
    return;
  const auto npc = it->second;
  const auto key = it->first;
  originalLines.erase(it);
  send(JsonWriter("line_started").str("npc",npc).str("line",key).boolean("voiced",true).finish());
  lastLine[npc] = key;
  std::string name = npc;
  if(auto world = Gothic::inst().world())
    if(auto n = npcBySymbol(*world,npc))
      name = std::string(n->displayName());
  // said to an npc (v15)
  if(auto t = originalTo.find(key); t!=originalTo.end()) {
    if(auto world = Gothic::inst().world())
      if(auto n = npcBySymbol(*world,t->second))
        name += " -> " + std::string(n->displayName());
    originalTo.erase(t);
    }
  const auto text = std::string(Gothic::inst().messageByName(key));
  ensureChat();
  chat->addLine(std::move(name),text,LineLibrary::durationFor(text));
  }

