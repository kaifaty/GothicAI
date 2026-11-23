#include "testbench_internal.h"
#include "gothic.h"
#include "npcdayjournal.h"
#include "game/serialize.h"
#include "world/npcactivityprobe.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include <Tempest/MemWriter>
#include <Tempest/Pixmap>
#include <cstring>
#include <fstream>
#include <memory>

namespace testbench_detail {
namespace {
class Checkpoint final : public NpcActivityProbe {
  public:
    Npc* npc=nullptr;
    bool ready=false;
    std::vector<bool> loops;
    NpcDayJournal::Options options;
    int64_t deadline=0;
    void phaseBegin(const Npc* actor,std::string_view symbol) override {
      loops.push_back(actor==npc && symbol=="ZS_SMITH_ANVIL_LOOP" && npc->interactive()!=nullptr &&
                      npc->interactive()->schemeName()=="BSANVIL");
      }
    void phaseEnd(int) override {ready=ready || loops.back();loops.pop_back();}
    void random(const Npc*,uint32_t,uint32_t,bool) override {}
    void queued(const Npc*,const AiQueue::AiAction&) override {}
    void point(const Npc&,const WayPoint*,const WayPoint*) override {}
    void mob(const Npc&,const Interactive&,bool) override {}
  };
std::unique_ptr<Checkpoint> checkpoint;
}

bool npcDayCheckpointRunning() {return bool(checkpoint);}

void npcDayCheckpointReset() {
  if(!checkpoint) return;
  if(NpcActivityProbe::instance==checkpoint.get()) NpcActivityProbe::instance=nullptr;
  checkpoint.reset();
  }

void npcDayCheckpointForget(const Npc* npc) {
  if(!checkpoint || checkpoint->npc!=npc) return;
  npcDayCheckpointReset();
  if(!bench.done)
    arenaAbort("activity checkpoint: the smith left the world before the save");
  }

bool npcDayCheckpointTick(World& w) {
  if(!checkpoint) return false;
  // By events (world/activityevents.h) the smith runs no Loop: the save point is his hold of the anvil.
  auto* smith=checkpoint->npc;
  if(smith->activityStage()==ActivityEvents::Stage::Hold && smith->interactive()!=nullptr &&
     smith->interactive()->schemeName()=="BSANVIL")
    checkpoint->ready=true;
  if(!checkpoint->ready) {
    if(w.time().toInt()>=checkpoint->deadline) {
      NpcActivityProbe::instance=nullptr;
      checkpoint.reset();
      arenaAbort("activity checkpoint did not reach a smith anvil Loop or hold within one game day");
      }
    return true;
    }
  auto options=checkpoint->options;
  NpcActivityProbe::instance=nullptr;
  checkpoint.reset();
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    Gothic::inst().gameSession()->save(out,"activity checkpoint",preview);
  }
  std::ofstream file(options.directory+"/session-activity.sav",std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file) {arenaAbort("cannot write activity checkpoint");return true;}
  std::string error;
  if(!NpcDayJournal::start(w,options,error)) arenaAbort(error);
  bench.npcCommands.npcDayStart=w.time().toInt();
  bench.npcCommands.npcDayMinutes=options.minutes;
  return true;
  }

bool npcDayCommand(const NpcCommandsContext& context,const std::vector<std::string>& a,World& w) {
  NpcDayJournal::Options opt;
  opt.directory=outDir;
  opt.seed=context.scenarioSeed;
  const bool saving=a[0]=="npcday-save";
  if(saving) opt.minutes=60;
  size_t positional=0;
  bool ok=a.size()>1;
  for(size_t i=saving ? 2 : 1;i<a.size();++i) {
    if(a[i]=="probe") opt.scriptProbe=true;
    else if(a[i].starts_with("trace=")) {
      const auto names=a[i].substr(6);
      if(names.empty()) ok=false;
      for(size_t at=0;at<names.size();) {
        const auto end=names.find(',',at);
        opt.traceNpcs.push_back(names.substr(at,end-at));
        at=end==std::string::npos ? names.size() : end+1;
        }
      }
    else if(a[i]=="shared") opt.isolate=false;
    else if(isNumber(a[i]) && positional==0 && num(a,i,0)>0) opt.minutes=uint64_t(num(a,i,0)),++positional;
    else if(isNumber(a[i]) && positional==1 && num(a,i,0)>0) opt.period=uint32_t(num(a,i,0)),++positional;
    else ok=false;
    }
  std::string error;
  if(saving && ok) {
    auto* npc=findNpc(w,*w.player(),a[1]);
    if(!npc || NpcDayJournal::running() || NpcActivityProbe::instance) {
      arenaAbort("activity checkpoint needs a live actor and no running day probe");return true;
      }
    opt.scriptProbe=true;
    opt.traceNpcs={std::string(w.rules().symbolName(npc->instanceSymbol()))};
    checkpoint=std::make_unique<Checkpoint>();
    checkpoint->npc=npc;
    checkpoint->options=opt;
    checkpoint->deadline=w.time().toInt()+24*60*60000;
    NpcActivityProbe::instance=checkpoint.get();
    return true;
    }
  if(!ok || !NpcDayJournal::start(w,opt,error)) {
    say("npcday failed: "+(ok ? error : "usage: npcday <minutes> [period] [shared] [probe] [trace=id,name]"));
    event("npcday_error","\"reason\":"+jsonStr(ok ? error : "usage"));
    return true;
    }
  context.npcCommands.npcDayStart=w.time().toInt();
  context.npcCommands.npcDayMinutes=opt.minutes;
  say("npcday "+std::to_string(opt.minutes)+" min every "+std::to_string(opt.period)+(opt.isolate ? "" : " shared"));
  event("npcday_start","\"minutes\":"+std::to_string(opt.minutes)+",\"period\":"+std::to_string(opt.period)+
        ",\"isolate\":"+(opt.isolate ? "true" : "false")+",\"seed\":"+std::to_string(opt.seed));
  return true;
  }
}
