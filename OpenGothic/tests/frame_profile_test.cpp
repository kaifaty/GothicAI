#include "frameprofile.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if(!condition)
    throw std::runtime_error(message);
  }

std::vector<std::string_view> columns(std::string_view line) {
  std::vector<std::string_view> result;
  size_t begin=0;
  for(size_t end=0;end<=line.size();++end)
    if(end==line.size() || line[end]==',') {
      result.push_back(line.substr(begin,end-begin));
      begin=end+1;
      }
  return result;
  }

void check(size_t attempts, size_t expectedRows, size_t overflow) {
  char directory[]="/tmp/gothic-frame-profile-XXXXXX";
  require(mkdtemp(directory)!=nullptr,"cannot create temporary directory");
  const auto path=std::filesystem::path(directory)/"frames.csv";
  try {
    require(setenv("OPENGOTHIC_FRAME_PROFILE",path.c_str(),1)==0,"cannot set profile path");
    require(FrameProfile::enabled(),"profiler must be enabled before allocating a chunk");
    void* first=nullptr;
    void* geometry=nullptr;
    void* passes=nullptr;
    for(size_t i=0;i<attempts;++i) {
      FrameProfile::Attempt attempt(uint8_t(i%3));
      FrameProfile::sample("segment-"+std::to_string(i/4096),i);
      FrameProfile::Scope scope(FrameProfile::Zone::Tick);
      scope.add(FrameProfile::Amount::DrawCalls,i);
      attempt.outcome(FrameProfile::Outcome::Submitted);
      auto* record=FrameProfile::gpuSubmitted();
      require((record!=nullptr)==(i<expectedRows),"attempt capacity mismatch");
      if(i==0) {
        first=record;
        geometry=FrameProfile::geometrySubmitted();
        passes=FrameProfile::gpuPassesSubmitted();
        }
      else
        FrameProfile::gpuCompletedNs(record,100+i,true);
      }
    // Complete the first chunk's work only after all subsequent chunks exist.
    FrameProfile::gpuCompletedNs(first,123456789,true);
    FrameProfile::geometryCompleted(geometry,42,true);
    FrameProfile::gpuPassesCompleted(passes,{{"render","first chunk",987,0,0,1,0,true}});
    FrameProfile::flush();
    require(!FrameProfile::enabled(),"profiler must be closed after flush");
    // Shutdown callbacks must retain valid records even after output is closed.
    FrameProfile::gpuCompletedNs(first,1,true);
    FrameProfile::geometryCompleted(geometry,1,true);
    FrameProfile::gpuPassesCompleted(passes,{{"render","after flush",1,0,0,1,0,true}});

    std::ifstream csv(path);
    std::string header;
    require(bool(std::getline(csv,header)),"missing CSV header");
    const auto names=columns(header);
    const auto column=[&](std::string_view name) {
      const auto found=std::find(names.begin(),names.end(),name);
      require(found!=names.end(),"missing CSV column");
      return size_t(found-names.begin());
      };
    const auto gpu=column("gpu_ns"), done=column("gpu_done"), tick=column("tick_calls");
    const auto draws=column("draw_calls"), triangles=column("triangles"), geometryDone=column("geometry_done");
    size_t rows=0;
    for(std::string line;std::getline(csv,line);++rows) {
      const auto cells=columns(line);
      require(cells.size()==names.size(),"wrong CSV column count");
      const auto number=[&](size_t index) { return std::stoull(std::string(cells[index])); };
      require(number(0)==rows && number(1)==rows%3 && number(4)==rows,"nonsequential attempt/frame");
      require(cells[3]=="segment-"+std::to_string(rows/4096),"wrong segment across chunk boundary");
      require(number(8)==unsigned(FrameProfile::Outcome::Submitted),"wrong attempt outcome");
      require(number(10)==overflow,"wrong overflow count");
      require(number(tick)==1 && number(draws)==rows,"scope recorded in wrong attempt");
      require(number(done)==1 && number(gpu)==(rows==0 ? 123456789 : 100+rows),"GPU callback recorded in wrong attempt");
      require(number(triangles)==(rows==0 ? 42u : 0u) && number(geometryDone)==(rows==0 ? 1u : 0u),
              "geometry callback recorded in wrong attempt");
      }
    require(rows==expectedRows,"wrong CSV row count");
    std::ifstream sidecar(path.string()+".gpu.csv");
    std::string line;
    require(bool(std::getline(sidecar,line)),"missing GPU sidecar header");
    require(bool(std::getline(sidecar,line)) && line.find("0,0,segment-0,0,0,render,\"first chunk\",987,")==0,
            "GPU passes recorded in wrong attempt");
    require(!std::getline(sidecar,line),"unexpected GPU pass row");
    std::cout << "frame profile: " << rows << " rows, overflow=" << overflow << ", callbacks preserved\n";
    }
  catch(...) {
    std::filesystem::remove_all(directory);
    throw;
    }
  std::filesystem::remove_all(directory);
  }
}

int main() {
  // Each process sets its path before the first access to the process-lifetime recorder.
  for(const auto attempts:{size_t(25000),size_t(262145)}) {
    const auto child=fork();
    if(child==0) {
      try {
        check(attempts,std::min(attempts,size_t(262144)),attempts>262144 ? 1 : 0);
        std::cout.flush();
        _exit(0);
        }
      catch(const std::exception& error) {
        std::cerr << "frame profile: " << error.what() << '\n';
        _exit(1);
        }
      }
    int status=0;
    if(child<0 || waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=0)
      return 1;
    }
  return 0;
  }
