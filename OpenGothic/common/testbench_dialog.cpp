#include "testbench_internal.h"
#include "texturedump.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>

#include "game/gamescript.h"
#include "gothic.h"
#include "loadprobe.h"
#include "world/aiqueue.h"
#include "world/chunkresidency.h"
#include "world/npcsimtier.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;
using namespace testbench_detail;

namespace {

constexpr uint64_t SpeakTailMs = 400;  // the talk key is held this long after the recording ends

// length of a 16-bit PCM wav in ms, 0 if it is not one
uint64_t wavMs(const std::string& path) {
  std::ifstream f(path,std::ios::binary);
  char riff[12];
  if(!f.read(riff,12) || std::memcmp(riff,"RIFF",4)!=0 || std::memcmp(riff+8,"WAVE",4)!=0)
    return 0;
  uint32_t byteRate = 0;
  char     hdr[8];
  while(f.read(hdr,8)) {
    uint32_t size = 0;
    std::memcpy(&size,hdr+4,4);
    if(std::memcmp(hdr,"fmt ",4)==0) {
      std::vector<char> fmt(size);
      if(!f.read(fmt.data(),size) || size<16)
        return 0;
      std::memcpy(&byteRate,fmt.data()+8,4);
      }
    else if(std::memcmp(hdr,"data",4)==0) {
      return byteRate>0 ? uint64_t(size)*1000/byteRate : 0;
      }
    else {
      f.seekg(size + (size&1),std::ios::cur);
      }
    }
  return 0;
  }

// whom the gaze of that source looks at, "-" for none or a point
std::string gazeWho(World& w, const Npc& n, Gaze::Source s) {
  return symName(w,s==Gaze::Source::Talk ? n.talkPartner() : s==Gaze::Source::Script ? n.lookAtTarget() :
                   s==Gaze::Source::Attention ? n.attentionTarget() : nullptr);
  }

}

namespace testbench_detail {

bool aiCommand(const std::string& cmd, std::string& reply) {
  auto& fn = Gothic::inst().consoleCommandFn;
  return fn && fn(cmd,reply);
  }

// an npc's name as the player sees it, for the log and events.jsonl (UTF-8): the game's strings are cp1251
// (the russian localization). The engine has no codec (ai_bridge's TextCodec is above it); a name only
// needs the letters, other bytes above ASCII become '?'
std::string nameUtf8(const Npc& n) {
  std::string r;
  for(char ch:n.displayName()) {
    const auto b = uint8_t(ch);
    if(b<0x80) {
      r += ch;
      continue;
      }
    uint32_t cp = '?';
    if(b>=0xC0)
      cp = 0x410 + (uint32_t(b) - 0xC0);  // А..я
    else if(b==0xA8)
      cp = 0x401;                         // Ё
    else if(b==0xB8)
      cp = 0x451;                         // ё
    if(cp<0x80) {
      r += char(cp);
      continue;
      }
    r += char(0xC0 | (cp>>6));
    r += char(0x80 | (cp & 0x3F));
    }
  return r;
  }

const char* gazeSource(Gaze::Source s) {
  switch(s) {
    case Gaze::Source::None:      return "none";
    case Gaze::Source::Talk:      return "talk";
    case Gaze::Source::Script:    return "script";
    case Gaze::Source::Focus:     return "focus";
    case Gaze::Source::Attention: return "attention";
    case Gaze::Source::Input:     return "input";
    }
  return "?";
  }

bool dialogCommand(const DialogContext& ctx,const std::string& line,const std::vector<std::string>& a,
                   World& w,Npc& pl) {
  const auto& c=a[0];
  if(c=="talk") {
    // talk <npc|nearest> <ms>: the npc and the hero talk to each other (no words): they look at each other
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("talk: no npc " + line);
      return true;
      }
    const uint64_t ms = uint64_t(num(a,2,3000));
    n->setTalkPartner(&pl,ms);
    pl.setTalkPartner(n,ms);
    say("talk " + symName(w,n) + " " + std::to_string(ms));
    }
  else if(c=="speak") {
    // speak <npc|nearest|player> <output name|$SVM> [to]: it says a voiced line of the game, like AI_Output in a
    // script, or an SVM ($SMALLTALK01, like AI_OutputSvm). To another npc it turns to it first, as a line of
    // the AI said to an npc does (v15 say.to)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3) {
      say("speak: no npc or line " + line);
      return true;
      }
    Npc* to = (n==&pl) ? &ctx.subjectOf(pl) : &pl;
    if(a.size()>3) {
      to = findNpc(w,pl,a[3]);
      if(to==nullptr || to==n) {
        say("speak: no one to say it to " + line);
        return true;
        }
      if(to!=&pl)
        n->aiPush(AiQueue::aiTurnToNpc(to));
      }
    if(a[2].starts_with("$")) {
      std::string svm = a[2];
      for(auto& ch:svm)
        ch = char(std::toupper(uint8_t(ch)));
      n->aiPush(AiQueue::aiOutputSvm(*to,svm,0));
      } else {
      n->aiPush(AiQueue::aiOutput(*to,a[2],0));
      }
    say("speak " + nameUtf8(*n) + " " + a[2] + (a.size()>3 ? " -> " + symName(w,to) : ""));
    }
  else if(c=="dialog") {
    // dialog <npc|nearest>: the hero talks to it, as with the action key (important infos start by themselves)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("dialog: no npc " + line);
      return true;
      }
    n->startDialog(pl);
    say("dialog " + symName(w,n));
    }
  else if(c=="choose") {
    // choose <n>: runs the n-th dialog choice (from 1, as listed) of the watched npc, as if the player picked it
    Npc&       n   = ctx.subjectOf(pl);
    auto       chs = w.script().dialogChoices(pl.handlePtr(),n.handlePtr(),{},false);
    std::sort(chs.begin(),chs.end(),[](const GameScript::DlgChoice& l, const GameScript::DlgChoice& r){ return l.sort<r.sort; });
    const size_t i = size_t(num(a,1,1));
    if(i<1 || i>chs.size()) {
      say("choose: no choice " + std::to_string(i) + " of " + std::to_string(chs.size()));
      return true;
      }
    say("choose " + std::to_string(i) + " of " + std::to_string(chs.size()));
    Gothic::inst().dialogExec(chs[i-1],pl,n);
    }
  else if(c=="hear") {
    // hear <npc|nearest|player> <ms> [every ms]: the hero speaks to it that long, his words come every so often
    // (as the bridge does with his voice: it listens, nods now and then); player: the hero listens to the nearest
    Npc* n    = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    Npc* from = n==&pl ? findNpc(w,pl,"nearest") : &pl;
    if(n==nullptr || from==nullptr || from==n) {
      say("hear: no npc " + line);
      return true;
      }
    ctx.hearNpc   = n;
    ctx.hearWith  = from;
    ctx.hearUntil = ctx.time + uint64_t(num(a,2,4000));
    ctx.hearEvery = uint64_t(std::max(100.f,num(a,3,600)));
    ctx.hearNext  = ctx.time;
    say("hear " + symName(w,n));
    }
  else if(c=="think") {
    // think <npc|nearest|player> <ms>: it thinks of an answer that long (as on the AI server's `thinking`);
    // the hero thinks of an answer to the nearest
    Npc* n  = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    Npc* to = n==&pl ? findNpc(w,pl,"nearest") : &pl;
    if(n==nullptr || to==nullptr || to==n) {
      say("think: no npc " + line);
      return true;
      }
    ctx.thinkNpc   = n;
    ctx.thinkWith  = to;
    ctx.thinkUntil = ctx.time + uint64_t(num(a,2,2500));
    say("think " + symName(w,n));
    }
  else if(c=="nearby") {
    // nearby [radius]: the npcs around the hero, where they look and why
    const float r = num(a,1,1500);
    std::vector<std::pair<float,Npc*>> list;
    w.detectNpc(pl.position(),r,[&](Npc& n) {
      if(&n!=&pl)
        list.emplace_back((n.position()-pl.position()).length(),&n);
      });
    std::sort(list.begin(),list.end(),[](auto& x, auto& y) { return x.first<y.first; });
    say("nearby " + std::to_string(list.size()));
    for(auto& [d,n]:list) {
      const Vec3  dp  = pl.position()-n->position();
      const float ang = std::remainder(std::atan2(dp.z,dp.x)*180.f/float(M_PI) - n->rotation(), 360.f);
      Vec3 at;
      const auto src = n->gazeTarget(at);
      char buf[300];
      std::snprintf(buf,sizeof(buf),"  %-28s dist=%5.0f hero_at=%5.0f bs=%2u mob=%d look=%s at=%s",
                    symName(w,n).c_str(),double(d),double(ang),unsigned(n->bodyStateMasked() & BS_MAX),
                    int(n->interactive()!=nullptr),gazeSource(src),
                    symName(w,src==Gaze::Source::Talk ? n->talkPartner() : src==Gaze::Source::Script ? n->lookAtTarget() :
                                src==Gaze::Source::Attention ? n->attentionTarget() : nullptr).c_str());
      say(buf);
      }
    }
  else if(c=="state") {
    // state <npc> <ZS_*> <waypoint> [minutes 60]: it does that now at that place, as a routine entry would
    // (TA_*), for that many game minutes; its routine goes on afterwards (npc_act start_activity)
    Npc* n = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    std::string name = a.size()>2 ? a[2] : "";
    for(auto& ch:name)
      ch = char(std::toupper(uint8_t(ch)));
    const size_t fn = name.starts_with("ZS_") ? w.script().findSymbolIndex(name) : size_t(-1);
    auto         wp = a.size()>3 ? w.findPoint(a[3],false) : nullptr;
    if(n==nullptr || n==&pl || fn==size_t(-1) || wp==nullptr) {
      say("state: no npc, state or waypoint " + line);
      return true;
      }
    auto until = w.time();
    until.addMilis(uint64_t(std::clamp(num(a,4,60),1.f,24*60.f))*60*1000);
    const bool ok = n->startState(ScriptFn(fn),wp->name,until,false);
    say("state " + symName(w,n) + " " + name + " " + wp->name + (ok ? "" : " failed"));
    }
  else if(c=="come") {
    // come <npc>: it walks up to the hero and turns to him (npc_act go_to_npc): a free one goes back to its
    // routine afterwards
    Npc* n = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    if(n==nullptr || n==&pl) {
      say("come: no npc " + line);
      return true;
      }
    n->aiPush(AiQueue::aiGoToNpc(&pl));
    n->aiPush(AiQueue::aiTurnToNpc(&pl));
    char buf[160];
    std::snprintf(buf,sizeof(buf),"come %s dist=%.0f",symName(w,n).c_str(),double((n->position()-pl.position()).length()));
    say(buf);
    }
  else if(c=="pair") {
    // pair <a> <b>: how two npcs stand to each other: the distance; for each, where it faces (0: right at the
    // other), where it looks and why, its state and for how long (ms), its body state, a mob it uses, how much
    // of its AI runs
    Npc* x = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    Npc* y = a.size()>2 ? findNpc(w,pl,a[2]) : nullptr;
    if(x==nullptr || y==nullptr || x==y) {
      say("pair: no npcs " + line);
      return true;
      }
    auto one = [&](Npc& n, Npc& o) {
      const Vec3  dp  = o.position()-n.position();
      const float ang = std::remainder(std::atan2(dp.z,dp.x)*180.f/float(M_PI) - n.rotation(), 360.f);
      Vec3        at;
      const auto  src = n.gazeTarget(at);
      char buf[400];
      std::snprintf(buf,sizeof(buf),"%s at_other=%.0f look=%s at=%s state=%s t=%llu bs=%u mob=%d ai=%s",
                    symName(w,&n).c_str(),double(ang),gazeSource(src),gazeWho(w,n,src).c_str(),stateName(w,n).c_str(),
                    (unsigned long long)n.stateTime(),unsigned(n.bodyStateMasked() & BS_MAX),
                    int(n.interactive()!=nullptr),policyName(n.processPolicy()));
      return std::string(buf);
      };
    char head[64];
    std::snprintf(head,sizeof(head),"pair dist=%.0f | ",double((x->position()-y->position()).length()));
    say(head + one(*x,*y) + " | " + one(*y,*x));
    }
  else if(c=="where") {
    // where <npc> [waypoint]: where it is, how far from the hero (and from the waypoint), its state, whether
    // it is busy with its queue (walking there), how much of its AI runs
    Npc* n = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    if(n==nullptr) {
      say("where: no npc " + line);
      return true;
      }
    const Vec3 p = n->position();
    char buf[400];
    int  len = std::snprintf(buf,sizeof(buf),"where %s at %.0f %.0f %.0f hero=%.0f",symName(w,n).c_str(),
                             double(p.x),double(p.y),double(p.z),double((p-pl.position()).length()));
    if(a.size()>2 && len>0 && size_t(len)<sizeof(buf)) {
      if(auto wp = w.findPoint(a[2],false))
        len += std::snprintf(buf+len,sizeof(buf)-size_t(len)," %s=%.0f",wp->name.c_str(),double((p-wp->position()).length())); else
        len += std::snprintf(buf+len,sizeof(buf)-size_t(len)," %s=?",a[2].c_str());
      }
    if(len>0 && size_t(len)<sizeof(buf))
      std::snprintf(buf+len,sizeof(buf)-size_t(len)," state=%s t=%llu queue=%d bs=%u ai=%s",stateName(w,*n).c_str(),
                    (unsigned long long)n->stateTime(),int(!n->isAiQueueEmpty()),unsigned(n->bodyStateMasked() & BS_MAX),
                    policyName(n->processPolicy()));
    say(buf);
    }
  else if(c=="ptt") {
    // ptt <wav> [text]: the hero holds the talk key while the recording is said; the AI server takes the
    // file instead of the microphone (--voice-files), the npc's hear it through speech recognition.
    // The text only goes into events.jsonl (subtitles)
    const std::string wav = a.size()>1 ? a[1] : "";
    const uint64_t    ms  = wavMs(wav);
    if(ms==0) {
      say("ptt: not a 16-bit wav: " + wav);
      return true;
      }
    std::string reply;
    if(!aiCommand("ai ptt start " + wav,reply)) {
      say("ptt: no AI bridge");
      return true;
      }
    const std::string text = rest(rest(line));
    ctx.speaking   = true;
    ctx.speakUntil = ctx.time + ms + SpeakTailMs;
    event("hero","\"wav\":" + jsonStr(wav) + ",\"ms\":" + std::to_string(ms) + ",\"text\":" + jsonStr(text));
    say("ptt " + std::to_string(ms) + " ms: " + text + " (" + reply + ")");
    }
  else if(c=="waitreply" || c=="waitquiet") {
    // waitreply [timeout ms 30000] [quiet ms 1200]: until an npc of the AI has answered the hero's last
    // phrase and has been quiet that long; waitquiet [quiet ms 1200] [timeout ms 30000]: only the quiet
    const bool reply = c=="waitreply";
    ctx.waitReply  = true;
    ctx.replyNeed  = reply;
    ctx.replyFrom  = ctx.heroEndAt>0 ? ctx.heroEndAt : ctx.time;
    ctx.replyEnd   = ctx.time + uint64_t(num(a,reply ? 1 : 2,30000));
    ctx.replyQuiet = uint64_t(num(a,reply ? 2 : 1,1200));
    }
  else if(c=="waitai") {
    // waitai <key> <stage> [timeout ms 20000]: until a thing of the AI bridge has got that far (waitai necro risen)
    if(a.size()<3) {
      say("waitai: <key> <stage> [timeout]");
      return true;
      }
    ctx.waitAiKey   = a[1];
    ctx.waitAiStage = a[2];
    ctx.waitAiEnd   = ctx.time + uint64_t(num(a,3,20000));
    }
  else if(c=="mark") {
    const auto text=rest(line);
    event("mark","\"text\":" + jsonStr(text));
    if(LoadProbe::enabled() && ((text=="stream_warm" && w.chunkResidency()!=nullptr) || text=="stream_warm_5s")) {
      std::string pages;
      if(auto* chunks=w.chunkResidency()) {
        const auto& s=chunks->stats();
        pages="landscape_used_bytes="+std::to_string(s.landscapePageUsedBytes)+
              " landscape_free_bytes="+std::to_string(s.landscapePageFreeBytes)+
              " landscape_retired_bytes="+std::to_string(s.landscapePageRetiredBytes);
        pages+=" "+s.vobProbeFields();
        }
      pages+=(pages.empty() ? "" : " ")+w.npcSimTierStats().probeFields();
      pages+=" "+NpcLogicAudit::stats().probeFields();
      w.probeMemory(text);
      LoadProbe::mark(text,-1,pages);
      }
    TextureDump::write(text,w);
    }
  else if(c=="ai") {
    std::string reply;
    say(std::string(aiCommand(line,reply) ? "" : "no AI bridge: ") + line + " -> " + reply);
    }
  else {
    return false;
    }
  return true;
  }

}
