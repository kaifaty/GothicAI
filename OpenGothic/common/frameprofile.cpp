#include "frameprofile.h"

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {
constexpr size_t Zones=size_t(FrameProfile::Zone::Count), Amounts=size_t(FrameProfile::Amount::Count), MaxAttempts=20000, MaxActors=20;
const char* names[Zones]={"tick","finish_step","camera","observations","bench","fence_poll","recycle","ui",
  "encode","submit","present","limiter","world_logic","prepare","physics","sync","world_misc",
  "parallel_animation","native_export","forced_sync_animation","actor_animation_elapsed_sum","pose_elapsed_sum","palette_elapsed_sum",
  "storage_join","storage_commit","storage_pack","storage_dispatch","storage_grow","storage_upload_execution_elapsed",
  "bodynet","cloth","water","destruction","ai_bridge","sound",
  "encode_shadows","encode_main","encode_water","encode_post","encode_ui"};
const char* amountNames[Amounts]={"palette_calls","palette_bytes","identical_calls","identical_bytes",
  "patch_calls","patch_bytes","patch_blocks","grow_bytes","upload_execution_bytes","draw_calls"};
uint64_t ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
struct Counter {
  std::atomic<uint64_t> sum{0}, calls{0}, first{0}, last{0};
  };
struct Record {
  uint64_t begin=0,end=0,observer=0,dt=0,frame=0;
  uint8_t commandId=0;
  uint32_t flags=0;
  FrameProfile::Outcome outcome=FrameProfile::Outcome::Unfinished;
  char segment[64]={};
  Counter zones[Zones];
  std::atomic<uint64_t> amounts[Amounts]{};
  std::atomic<uint64_t> gpuNs{0};
  std::atomic<uint32_t> gpuBuffers{0}, gpuCompleted{0}, gpuValid{0};
  std::atomic<uint64_t> triangles{0};
  std::atomic<uint32_t> geometryBuffers{0}, geometryCompleted{0}, geometryValid{0};
  };
struct Recorder {
  std::string path;
  std::unique_ptr<Record[]> records;
  size_t count=0,overflow=0;
  std::atomic<bool> closed{false};
  std::atomic<Record*> current{nullptr};
  std::atomic<Record*> latest{nullptr};
  std::array<const Npc*,MaxActors> actors{};
  size_t actorCount=0;
  Recorder() {
    const auto* value=std::getenv("OPENGOTHIC_FRAME_PROFILE");
    if(value!=nullptr && value[0]!=0) { path=value; records=std::make_unique<Record[]>(MaxAttempts); }
    }
  void flush() {
    if(path.empty() || closed.exchange(true)) return;
    current.store(nullptr); latest.store(nullptr);
    // Rendering has stopped; late GPU callbacks may still update their records.
    // This is the only output operation.
    if(auto* file=std::fopen(path.c_str(),"w")) {
      std::fprintf(file,"attempt,cmd_id,dt,segment,frame,begin_ns,observer_ns,end_ns,outcome,flags,overflow");
      for(const auto* name:names) std::fprintf(file,",%s_ns,%s_calls,%s_first_ns,%s_last_ns",name,name,name,name);
      for(const auto* name:amountNames) std::fprintf(file,",%s",name);
      std::fprintf(file,",gpu_ns,gpu_done,gpu_buffers,triangles,geometry_done\n");
      for(size_t i=0;i<count;++i) {
        const auto& r=records[i];
        std::fprintf(file,"%zu,%u,%llu,%s,%llu,%llu,%llu,%llu,%u,%u,%zu",i,unsigned(r.commandId),
          (unsigned long long)r.dt,r.segment,(unsigned long long)r.frame,(unsigned long long)r.begin,
          (unsigned long long)r.observer,(unsigned long long)r.end,unsigned(r.outcome),r.flags,overflow);
        for(const auto& z:r.zones) std::fprintf(file,",%llu,%llu,%llu,%llu",(unsigned long long)z.sum.load(),
          (unsigned long long)z.calls.load(),(unsigned long long)z.first.load(),(unsigned long long)z.last.load());
        for(const auto& amount:r.amounts) std::fprintf(file,",%llu",(unsigned long long)amount.load());
        const auto completed=r.gpuCompleted.load(std::memory_order_acquire);
        const auto buffers=r.gpuBuffers.load(std::memory_order_relaxed);
        const bool done=buffers>0 && completed==buffers && r.gpuValid.load(std::memory_order_relaxed)==buffers;
        std::fprintf(file,",%llu,%u,%u",(unsigned long long)(done ? r.gpuNs.load(std::memory_order_relaxed) : 0),
                     unsigned(done),unsigned(buffers));
        const auto geometryCompleted=r.geometryCompleted.load(std::memory_order_acquire);
        const auto geometryBuffers=r.geometryBuffers.load(std::memory_order_relaxed);
        const bool geometryDone=geometryBuffers>0 && geometryCompleted==geometryBuffers &&
                                r.geometryValid.load(std::memory_order_relaxed)==geometryBuffers;
        std::fprintf(file,",%llu,%u\n",(unsigned long long)r.triangles.load(std::memory_order_relaxed),unsigned(geometryDone));
        }
      std::fclose(file);
      }
    }
  };
// Process-lifetime storage protects a late async upload scope during shutdown.
// The bounded diagnostic buffer is explicitly flushed after rendering stops.
Recorder& recorder() { static Recorder* value=new Recorder; return *value; }
thread_local unsigned actorDepth=0;
}

bool FrameProfile::enabled() { return recorder().records!=nullptr && !recorder().closed.load(); }
bool FrameProfile::actorContext() { return actorDepth>0; }
bool FrameProfile::watchedActor(const Npc* actor) {
  const auto& r=recorder();
  if(r.records==nullptr || r.closed.load() || actor==nullptr) return false;
  for(size_t i=0;i<r.actorCount;++i) if(r.actors[i]==actor) return true;
  return false;
  }
void FrameProfile::setActors(const std::vector<Npc*>& actors) {
  auto& r=recorder(); if(r.records==nullptr || r.closed.load()) return;
  r.actorCount=std::min(actors.size(),MaxActors);
  for(size_t i=0;i<r.actorCount;++i) r.actors[i]=actors[i];
  }
void FrameProfile::observer(uint64_t dt, std::chrono::steady_clock::time_point now) {
  auto* r=recorder().current.load(std::memory_order_relaxed); if(r==nullptr) return;
  r->dt=dt; r->observer=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
  }
void FrameProfile::sample(std::string_view segment, uint64_t frame) {
  auto* r=recorder().current.load(std::memory_order_relaxed); if(r==nullptr) return;
  const auto length=std::min(segment.size(),sizeof(r->segment)-1);
  std::memcpy(r->segment,segment.data(),length); r->segment[length]=0; r->frame=frame;
  }
FrameProfile::Scope::Scope(Zone zone, bool include):zone(zone) {
  if(!include || !enabled()) return;
  if(include) record=recorder().current.load(std::memory_order_relaxed);
  if(include && record==nullptr && zone==Zone::StorageUploadWorker)
    record=recorder().latest.load(std::memory_order_relaxed);
  if(record!=nullptr) begin=ns();
  if(record!=nullptr && zone==Zone::ActorAnimationCpu) { ++actorDepth; inActor=true; }
  }
FrameProfile::Scope::~Scope() {
  finish();
  }
void FrameProfile::Scope::finish() {
  if(record==nullptr) return;
  const auto end=ns(); auto& counter=static_cast<Record*>(record)->zones[size_t(zone)];
  counter.sum.fetch_add(end-begin,std::memory_order_relaxed); counter.calls.fetch_add(1,std::memory_order_relaxed);
  auto first=counter.first.load(std::memory_order_relaxed);
  while((first==0 || begin<first) && !counter.first.compare_exchange_weak(first,begin,std::memory_order_relaxed)) {}
  auto last=counter.last.load(std::memory_order_relaxed);
  while(end>last && !counter.last.compare_exchange_weak(last,end,std::memory_order_relaxed)) {}
  if(inActor) { --actorDepth; inActor=false; }
  record=nullptr;
  }
void FrameProfile::Scope::add(Amount amount,uint64_t value) {
  if(record!=nullptr) static_cast<Record*>(record)->amounts[size_t(amount)].fetch_add(value,std::memory_order_relaxed);
  }
FrameProfile::Attempt::Attempt(uint8_t commandId) {
  auto& r=recorder(); if(r.records==nullptr || r.closed.load()) return;
  if(r.count==MaxAttempts) { ++r.overflow; return; }
  record=&r.records[r.count++]; auto* value=static_cast<Record*>(record);
  value->begin=ns(); value->commandId=commandId; r.current.store(value,std::memory_order_relaxed);
  r.latest.store(value,std::memory_order_relaxed);
  }
FrameProfile::Attempt::~Attempt() {
  if(record==nullptr) return;
  recorder().current.store(nullptr,std::memory_order_relaxed); static_cast<Record*>(record)->end=ns();
  }
void FrameProfile::Attempt::simulation(uint64_t dt) { if(record!=nullptr) static_cast<Record*>(record)->dt=dt; }
void FrameProfile::Attempt::outcome(Outcome value) { if(record!=nullptr) static_cast<Record*>(record)->outcome=value; }
void FrameProfile::Attempt::flag(Flag value) { if(record!=nullptr) static_cast<Record*>(record)->flags|=uint32_t(value); }
void FrameProfile::add(Amount amount,uint64_t value) {
  if(!enabled()) return;
  auto* r=recorder().current.load(std::memory_order_relaxed);
  if(r==nullptr && amount==Amount::UploadBytes) r=recorder().latest.load(std::memory_order_relaxed);
  if(r!=nullptr) r->amounts[size_t(amount)].fetch_add(value,std::memory_order_relaxed);
  }
void FrameProfile::flush() { recorder().flush(); }

void* FrameProfile::gpuSubmitted() {
  auto& r=recorder();
  if(r.closed.load(std::memory_order_relaxed)) return nullptr;
  auto* record=r.current.load(std::memory_order_relaxed);
  if(record!=nullptr) record->gpuBuffers.fetch_add(1,std::memory_order_relaxed);
  return record;
  }
void FrameProfile::gpuCompleted(void* record, double startSeconds, double endSeconds) {
  if(record==nullptr) return;
  auto& r=*static_cast<Record*>(record);
  if(std::isfinite(startSeconds) && std::isfinite(endSeconds) && startSeconds>0 && endSeconds>=startSeconds) {
    r.gpuNs.fetch_add(uint64_t((endSeconds-startSeconds)*1e9),std::memory_order_relaxed);
    r.gpuValid.fetch_add(1,std::memory_order_relaxed);
    }
  // Publish the sum only after every captured buffer has completed. The records
  // live for the process lifetime, including callbacks arriving after flush.
  r.gpuCompleted.fetch_add(1,std::memory_order_release);
  }

void* FrameProfile::geometrySubmitted() {
  if(!enabled()) return nullptr;
  auto* record=recorder().current.load(std::memory_order_relaxed);
  if(record!=nullptr) record->geometryBuffers.fetch_add(1,std::memory_order_relaxed);
  return record;
  }

void FrameProfile::geometryCompleted(void* record, uint64_t triangles, bool valid) {
  if(record==nullptr) return;
  auto& r=*static_cast<Record*>(record);
  if(valid) {
    r.triangles.fetch_add(triangles,std::memory_order_relaxed);
    r.geometryValid.fetch_add(1,std::memory_order_relaxed);
    }
  r.geometryCompleted.fetch_add(1,std::memory_order_release);
  }

#endif
