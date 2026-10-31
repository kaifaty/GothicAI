#include "appcadence.h"
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
namespace AppCadence {
namespace {
constexpr size_t Capacity=100000;
constexpr uint64_t Duration=30000000000ull;
struct Counts { uint32_t models=0,dead=0,parts=0,asleep=0,down=0; };
struct State {
  bool enabled=false,armed=false,complete=false,on=false,flushed=false;
  bool logFps=false;
  uint64_t entry=0,start=0,end=0,overflow=0;
  uint64_t logStart=0,logFrames=0;
  size_t used=0;
  std::string output;
  std::vector<uint64_t> frames;
  Counts before,after;
  State() {
    const auto* value=std::getenv("OPENGOTHIC_FPS_LOG");
    logFps=value!=nullptr && value[0]=='1' && value[1]=='\0';
    }
  };
State& state() { static State s;return s; }
uint64_t now() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}
void enable(std::string output) {
  auto& s=state();s.output=std::move(output);s.frames.resize(Capacity);s.enabled=true;
  }
bool normalClock() { return state().enabled; }
void beginAttempt() { auto& s=state();if(s.enabled && !s.complete)s.entry=now(); }
bool arm(bool ragdoll) {
  auto& s=state();if(!s.enabled || s.armed || s.entry==0)return false;
  s.armed=true;s.on=ragdoll;s.start=s.entry;s.end=s.start+Duration;return true;
  }
void appFrame() {
  auto& s=state();if((!s.armed || s.complete) && !s.logFps)return;
  const auto time=now();
  if(s.logFps) {
    if(s.logStart==0) {
      s.logStart=time;
      } else {
      ++s.logFrames;
      const auto elapsed=time-s.logStart;
      if(elapsed>=10000000000ull) {
        std::fprintf(stderr,"[app-cadence] mean_fps=%.2f frames=%llu elapsed_s=%.3f\n",
          double(s.logFrames)*1e9/double(elapsed),(unsigned long long)s.logFrames,double(elapsed)/1e9);
        std::fflush(stderr);
        s.logStart=time;s.logFrames=0;
        }
      }
    }
  if(!s.armed || s.complete)return;
  if(s.used==Capacity) { ++s.overflow;s.complete=true;return; }
  s.frames[s.used++]=time;
  if(time>=s.end)s.complete=true; // preserve the whole final interval, including its tail
  }
bool done() { return state().complete; }
void counts(bool before,uint32_t models,uint32_t dead,uint32_t parts,uint32_t asleep,uint32_t down) {
  auto& s=state();(before ? s.before : s.after)=Counts{models,dead,parts,asleep,down};
  }
void flush() {
  auto& s=state();if(!s.armed || s.flushed)return;s.flushed=true;
  auto f=std::fopen(s.output.c_str(),"w");if(f==nullptr)return;
  std::fprintf(f,"frame,end_ns,interval_ns\n");uint64_t prior=s.start;
  for(size_t i=0;i<s.used;++i) { std::fprintf(f,"%zu,%llu,%llu\n",i,(unsigned long long)s.frames[i],(unsigned long long)(s.frames[i]-prior));prior=s.frames[i]; }
  std::fclose(f);f=std::fopen((s.output+".json").c_str(),"w");if(f==nullptr)return;
  std::fprintf(f,"{\"schema_version\":1,\"mode\":\"%s\",\"clock\":\"normal\",\"start_ns\":%llu,\"window_end_ns\":%llu,\"final_end_ns\":%llu,\"frames\":%zu,\"overflow\":%llu,\"complete\":%s,\"before\":{\"models\":%u,\"dead\":%u,\"parts\":%u},\"after\":{\"models\":%u,\"dead\":%u,\"parts\":%u,\"asleep\":%u,\"down\":%u}}\n",s.on?"on":"off",(unsigned long long)s.start,(unsigned long long)s.end,(unsigned long long)prior,s.used,(unsigned long long)s.overflow,s.complete?"true":"false",s.before.models,s.before.dead,s.before.parts,s.after.models,s.after.dead,s.after.parts,s.after.asleep,s.after.down);
  std::fclose(f);
  }
}
#endif
