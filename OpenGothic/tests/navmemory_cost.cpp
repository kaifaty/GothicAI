#define NAV_MEMORY_COST_REPLAY_ONLY
#include "navmemorycosttrace.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>

namespace {
using Clock=std::chrono::steady_clock;
struct Event {
  uint8_t kind=0;
  NavigationMemory::Pose pose;
  uint32_t rays=0;
  std::vector<std::pair<uint32_t,NavigationEye::Ray>> changed;
  };
struct Capture {
  NavigationMemory::FamiliarMap map;
  NavigationMemory::FamiliarMask mask;
  std::vector<Event> events;
  uint64_t updates=0,views=0;
  double durationMs=0;
  };
struct Timing {
  double updateUs=0,viewUs=0,checksum=0;
  uint64_t updates=0,views=0;
  Timing& operator+=(const Timing& other) {
    updateUs+=other.updateUs;viewUs+=other.viewUs;checksum+=other.checksum;
    updates+=other.updates;views+=other.views;
    return *this;
    }
  };

Capture load(const std::string& path) {
  std::ifstream input(path,std::ios::binary);
  if(!input)throw std::runtime_error("cannot open navigation memory cost capture: "+path);
  std::array<char,8> magic;
  NavMemoryCost::read(input,magic);
  if(magic!=NavMemoryCost::Magic)throw std::runtime_error("invalid navigation memory cost capture");
  uint32_t poseBytes=0,rayBytes=0,tileBytes=0;
  NavMemoryCost::read(input,poseBytes);NavMemoryCost::read(input,rayBytes);NavMemoryCost::read(input,tileBytes);
  if(poseBytes!=sizeof(NavigationMemory::Pose) || rayBytes!=sizeof(NavigationEye::Ray) ||
     tileBytes!=sizeof(NavigationMemory::FamiliarTile))
    throw std::runtime_error("navigation memory cost capture ABI differs");
  Capture result;
  uint64_t tiles=0,words=0;
  NavMemoryCost::read(input,tiles);NavMemoryCost::read(input,words);
  if(words!=(tiles+63)/64)throw std::runtime_error("navigation memory cost capture mask differs from atlas");
  result.map.tiles.resize(size_t(tiles));result.mask.resize(size_t(words));
  for(auto& tile:result.map.tiles)NavMemoryCost::read(input,tile);
  for(auto& word:result.mask)NavMemoryCost::read(input,word);
  result.map.finish();
  uint64_t first=0,last=0;
  while(input.peek()!=std::char_traits<char>::eof()) {
    Event event;
    NavMemoryCost::read(input,event.kind);
    if(event.kind==NavMemoryCost::Update) {
      NavMemoryCost::read(input,event.pose);NavMemoryCost::read(input,event.rays);
      uint32_t changed=0;NavMemoryCost::read(input,changed);
      if(changed>event.rays)throw std::runtime_error("invalid navigation memory cost capture ray count");
      event.changed.resize(changed);
      for(auto& [index,ray]:event.changed) {
        NavMemoryCost::read(input,index);NavMemoryCost::read(input,ray);
        if(index>=event.rays)throw std::runtime_error("invalid navigation memory cost capture ray slot");
        }
      if(result.updates++==0)first=event.pose.gameMs;
      last=event.pose.gameMs;
      }
    else if(event.kind==NavMemoryCost::View)++result.views;
    else throw std::runtime_error("invalid navigation memory cost capture event");
    result.events.push_back(std::move(event));
    }
  if(result.updates==0 || result.views==0)throw std::runtime_error("navigation memory cost capture is empty");
  result.durationMs=double(last-first);
  return result;
  }

Timing replay(const Capture& capture,bool familiar) {
  NavigationMemory memory;
  memory.setJourneyMemory(true);
  if(familiar && !capture.mask.empty())memory.setFamiliarMap(&capture.map,capture.mask);
  NavigationMemory::View view;
  std::vector<NavigationEye::Ray> rays;
  Timing result;
  for(const auto& event:capture.events) {
    if(event.kind==NavMemoryCost::Update) {
      rays.resize(event.rays);
      for(const auto& [index,ray]:event.changed)rays[index]=ray;
      const auto start=Clock::now();
      memory.update(rays,event.pose);
      result.updateUs+=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
      ++result.updates;
      }
    else {
      const auto start=Clock::now();
      memory.view(view);
      result.viewUs+=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
      ++result.views;
      // Consume output outside the timed readout; no observer cost is measured.
      result.checksum+=view.traveledCm;
      for(const auto& layer:{&view.near,&view.far}) {
        const auto& cell=layer->cells[layer->cells.size()/2];
        result.checksum+=cell.confidence+cell.familiar+cell.water;
        }
      }
    }
  return result;
  }

void json(const Timing& value) {
  std::cout<<"{\"update_us\":"<<value.updateUs/double(value.updates)
           <<",\"view_us\":"<<value.viewUs/double(value.views)
           <<",\"updates\":"<<value.updates<<",\"views\":"<<value.views
           <<",\"checksum\":"<<value.checksum<<'}';
  }
}

int main(int argc,char** argv) {
  try {
    std::string implementation="cached";
    unsigned repetitions=4;
    std::vector<std::string> paths;
    for(int i=1;i<argc;++i) {
      const std::string argument=argv[i];
      if(argument=="--implementation" && i+1<argc)implementation=argv[++i];
      else if(argument=="--repetitions" && i+1<argc)repetitions=unsigned(std::stoul(argv[++i]));
      else if(argument.starts_with("--"))throw std::runtime_error("unknown navigation memory cost option: "+argument);
      else paths.push_back(argument);
      }
    if(paths.empty() || repetitions<2 || repetitions%2!=0)
      throw std::runtime_error("usage: navmemory-cost [--implementation NAME] [--repetitions EVEN_NUMBER] CAPTURE...");
    std::cout<<std::setprecision(10)<<"{\"implementation\":"<<std::quoted(implementation)
             <<",\"repetitions\":"<<repetitions<<",\"warmup_passes_per_mode\":1,\"captures\":[";
    std::array<Timing,2> aggregate;
    for(size_t pathIndex=0;pathIndex<paths.size();++pathIndex) {
      const auto capture=load(paths[pathIndex]);
      replay(capture,false);replay(capture,true);
      if(pathIndex)std::cout<<',';
      std::cout<<"{\"path\":"<<std::quoted(paths[pathIndex])<<",\"atlas_tiles\":"<<capture.map.tiles.size()
               <<",\"personal_mask_words\":"<<capture.mask.size()<<",\"duration_ms\":"<<capture.durationMs
               <<",\"trajectory_updates\":"<<capture.updates<<",\"trajectory_views\":"<<capture.views
               <<",\"rounds\":[";
      std::array<Timing,2> total;
      for(unsigned round=0;round<repetitions;++round) {
        std::array<Timing,2> measured;
        // AB/BA cancels which mode runs first on each identical trajectory.
        const unsigned first=round%2;
        measured[first]=replay(capture,first!=0);
        measured[1-first]=replay(capture,first==0);
        for(size_t mode=0;mode<2;++mode)total[mode]+=measured[mode];
        if(round)std::cout<<',';
        std::cout<<"{\"first\":"<<std::quoted(first?"familiar":"journey")<<",\"journey\":";
        json(measured[0]);std::cout<<",\"familiar\":";json(measured[1]);std::cout<<'}';
        }
      std::cout<<"],\"journey\":";json(total[0]);std::cout<<",\"familiar\":";json(total[1]);std::cout<<'}';
      for(size_t mode=0;mode<2;++mode)aggregate[mode]+=total[mode];
      }
    std::cout<<"],\"aggregate\":{\"journey\":";json(aggregate[0]);
    std::cout<<",\"familiar\":";json(aggregate[1]);
    std::cout<<",\"familiar_view_over_journey\":"
             <<(aggregate[1].viewUs/double(aggregate[1].views))/(aggregate[0].viewUs/double(aggregate[0].views))
             <<"}}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
    }
  }
