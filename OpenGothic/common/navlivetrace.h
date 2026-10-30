#pragma once

#include "navpolicy.h"
#include "physics/navmesh.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

// Stand-only serialization. Reads snapshots and already encoded policy features;
// it never observes the world, advances policy state, or draws random numbers.
class NavLiveTrace final {
  public:
    NavLiveTrace(const std::filesystem::path& output,const NavPolicy* policy,
                 const NavMesh::Path& detour,size_t rayCount,bool playerTarget,
                 const std::vector<NavMesh::Point>& targetRoute,bool playerGuide):policy(policy) {
      decisions.open(output/"live-decisions.jsonl");
      observations=gzopen((output/"live-observations.bin.gz").string().c_str(),"wb1");
      if(!decisions || observations==nullptr)throw std::runtime_error("nav live: cannot open trace");
      if(gzbuffer(observations,256*1024)!=0)throw std::runtime_error("nav live: cannot buffer trace");
      decisions<<std::setprecision(9)<<std::boolalpha;
      std::ofstream format(output/"live-format.json");
      if(!format)throw std::runtime_error("nav live: cannot write format");
      const bool hd=policy && policy->weightVersion()>=3;
      const size_t channels=policy?(hd?(policy->weightVersion()==4?11u:10u):9u):8u;
      format<<std::setprecision(9)<<std::boolalpha
            <<"{\"schema\":1,\"observation_period_ms\":400,\"decision_period_ms\":100,\"ray_count\":"<<rayCount
            <<",\"map_source\":\""<<(policy?"policy_encoded":"teacher_memory")<<"\",\"maps\":{";
      const auto map=[&](const char* name,size_t rows,size_t columns,float cell,float minForward) {
        format<<'\"'<<name<<"\":{\"shape\":["<<channels<<','<<rows<<','<<columns
              <<"],\"cell_cm\":"<<cell<<",\"min_forward_cm\":"<<minForward
              <<",\"min_right_cm\":"<<-float(columns)*cell*.5f
              <<",\"channels\":[\"confidence\",\"free\",\"wall\",\"ledge\",\"drop\",\"water\",\"movable\"";
        if(policy) {
          format<<(hd?",\"relative_ground_height_max\"":",\"relative_ground_height\"");
          if(hd)format<<",\"relative_ground_height_min\"";
          format<<",\"height_valid_fraction\"";
          if(channels==11)format<<",\"familiar\"";
        } else format<<",\"familiar\"";
        format<<"],\"height_channels\":[";
        if(policy) {format<<7;if(hd)format<<",8";}
        format<<"]}";
      };
      map("near",policy && !hd?32u:64u,policy && !hd?32u:64u,policy && !hd?50.f:25.f,-800.f);
      format<<',';
      map("far",policy?(hd?50u:25u):100u,policy && !hd?25u:100u,policy && !hd?800.f:200.f,hd?0.f:-10000.f);
      format<<"},\"quantization\":{\"probability\":[0,1],\"height\":[-10,10],\"formula\":\"round(255*(value-min)/(max-min))\"}"
            <<",\"clearance_probe\":{\"directions\":16,\"radius_cm\":500,\"edge_step_cm\":10,\"edge_min_drop_cm\":100,\"water_step_cm\":25,\"water_kind\":\"depth_at_least_guild_chest\",\"censored_value_cm\":500}"
            <<",\"contact_source\":\"CharacterVirtual diagnostics since previous decision\""
            <<",\"safety_source\":\"CharacterVirtual stairBlockedByPolicy; peer clipping stored separately\""
            <<",\"detour_complete\":"<<detour.complete<<",\"detour_cm\":[";
      for(size_t i=0;i<detour.corners.size();++i) {
        if(i)format<<',';
        const auto& p=detour.corners[i];format<<'['<<p[0]*100.f<<','<<p[1]*100.f<<','<<p[2]*100.f<<']';
      }
      format<<"],\"target_script\":{\"source\":\""<<(playerTarget?"player_detour_interpolation":"native")
            <<"\",\"pause_ms\":2000,\"turn_back_deg\":180,\"turn_next_after_ms\":1000,\"walk_cm_s\":200,\"run_cm_s\":450,\"run_leg_index\":2,\"run_leg_prefix_ms\":2000,\"route_cm\":[";
      for(size_t i=0;i<targetRoute.size();++i) {
        if(i)format<<',';
        const auto& p=targetRoute[i];format<<'['<<p[0]*100.f<<','<<p[1]*100.f<<','<<p[2]*100.f<<']';
      }
      format<<"]},\"guide_script\":{\"enabled\":"<<playerGuide
            <<",\"source\":\"player_delayed_npc_trail\",\"trail_lag_cm\":300,\"walk_animation\":true}}\n";
      if(!format)throw std::runtime_error("nav live: format write failed");
      buffer.insert(buffer.end(),{'N','A','V','L','I','V','E',0});u32(1);write();
    }
    ~NavLiveTrace() {if(observations)gzclose(observations);}
    NavLiveTrace(const NavLiveTrace&)=delete;
    NavLiveTrace& operator=(const NavLiveTrace&)=delete;

    bool observationDue(uint64_t elapsed) const {return !observed || elapsed>=nextObservation;}
    void observation(const NavDemoWriter::Snapshot& sample,const NavPolicy::Features* features,
                     uint64_t elapsed,uint64_t decision) {
      if(!observationDue(elapsed))return;
      buffer.clear();u64(elapsed);u64(decision);
      if(policy) {
        if(features==nullptr)throw std::runtime_error("nav live: encoded features missing");
        const bool hd=policy->weightVersion()>=3;
        encoded(features->near,hd?64u*64u:32u*32u,hd);
        encoded(features->far,hd?50u*100u:25u*25u,hd);
      } else {
        memory(sample.memory->near);memory(sample.memory->far);
      }
      for(const auto& ray:*sample.eye) {
        vec(ray.origin);vec(ray.point);vec(ray.direction);f32(ray.distanceCm);f32(ray.normalY);
        buffer.push_back(uint8_t(ray.kind));buffer.push_back(uint8_t(ray.hit));buffer.push_back(uint8_t(ray.complete));
        u32(age(sample.gameMs,ray.sampledAtMs));u32(age(sample.gameMs,ray.nearSampledAtMs));
      }
      write();observed=true;nextObservation=elapsed+400;
    }
    void finish() {
      decisions.close();
      if(observations) {
        const auto stream=observations;observations=nullptr;
        if(gzclose(stream)!=Z_OK)throw std::runtime_error("nav live: trace close failed");
      }
      if(!decisions)throw std::runtime_error("nav live: decisions close failed");
    }
    std::ofstream decisions;

  private:
    void u32(uint32_t value) {for(unsigned i=0;i<4;++i)buffer.push_back(uint8_t(value>>(8*i)));}
    void u64(uint64_t value) {for(unsigned i=0;i<8;++i)buffer.push_back(uint8_t(value>>(8*i)));}
    void f32(float value) {
      if(!std::isfinite(value))throw std::runtime_error("nav live: non-finite ray");
      uint32_t bits=0;static_assert(sizeof(bits)==sizeof(value));std::memcpy(&bits,&value,sizeof(bits));u32(bits);
    }
    void vec(const Tempest::Vec3& value) {f32(value.x);f32(value.y);f32(value.z);}
    static uint32_t age(uint64_t now,uint64_t at) {
      if(at>now || now-at>uint64_t(UINT32_MAX))throw std::runtime_error("nav live: invalid ray timestamp");
      return uint32_t(now-at);
    }
    void probability(float value) {
      if(!std::isfinite(value) || value<0.f || value>1.f)throw std::runtime_error("nav live: invalid probability");
      buffer.push_back(uint8_t(std::lround(value*255.f)));
    }
    void encoded(const std::vector<float>& values,size_t area,bool hd) {
      for(size_t i=0;i<values.size();++i) {
        const size_t channel=i/area;
        if(channel==7 || (hd && channel==8))probability((values[i]+10.f)/20.f);
        else probability(values[i]);
      }
    }
    void memory(const NavigationMemory::Layer& layer) {
      for(unsigned channel=0;channel<8;++channel)for(const auto& cell:layer.cells) {
        const std::array<float,8> values={cell.confidence,cell.free,cell.wall,cell.ledge,cell.drop,cell.water,cell.movable,cell.familiar};
        probability(values[channel]);
      }
    }
    void write() {
      if(gzwrite(observations,buffer.data(),unsigned(buffer.size()))!=int(buffer.size()))
        throw std::runtime_error("nav live: observation write failed");
      buffer.clear();
    }
    const NavPolicy* policy=nullptr;
    gzFile observations=nullptr;
    std::vector<uint8_t> buffer;
    uint64_t nextObservation=0;
    bool observed=false;
};
