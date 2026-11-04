#include "backends.h"
#include <barrier>
#include <iomanip>
#include <thread>

constexpr int Repeats=3;
#include "character.h"
Input box() {
  Input in;
  for(int axis=0;axis<3;++axis)for(int sign:{-1,1}) {
    Vec p[4];
    for(int j=0;j<4;++j) {
      float xyz[3]{};xyz[axis]=float(sign*100);
      xyz[(axis+1)%3]=(j==0||j==3)?-100.f:100.f;
      xyz[(axis+2)%3]=j<2?-100.f:100.f;p[j]={xyz[0],xyz[1],xyz[2]};
    }
    if(sign<0)std::swap(p[1],p[3]);
    in.triangles.push_back({{p[0],p[1],p[2]}});in.triangles.push_back({{p[0],p[2],p[3]}});
    in.categories.insert(in.categories.end(),2,1);
  }
  return in;
}
template<class Scene> void checkBox(const Scene& scene,int width) {
  std::array<Vec,17> o,d,ends;std::array<float,17> len;std::array<Hit,17> h;
  for(int j=0;j<17;++j) {
    int axis=j%3,sign=j%2?1:-1;float xyz[3]={11,13,17};xyz[axis]=float(sign*200);
    o[j]={xyz[0],xyz[1],xyz[2]};xyz[0]=xyz[1]=xyz[2]=0;xyz[axis]=float(-sign);
    d[j]={xyz[0],xyz[1],xyz[2]};len[j]=300;
  }
  for(size_t j=0;j<o.size();++j)ends[j]=o[j]+d[j]*len[j];
  scene.cast(o.data(),ends.data(),h.size(),h.data(),width,true);
  for(auto hit:h)require((hit.flags&1)&&std::abs(hit.distance*300-100)<1.e-3f,"Box front distance failed");
  for(auto& origin:o)origin={11,13,17};
  for(size_t j=0;j<o.size();++j)ends[j]=o[j]+d[j]*len[j];
  scene.cast(o.data(),ends.data(),h.size(),h.data(),width,true);
  for(auto hit:h)require(!(hit.flags&1),"Box backface rejection failed");
  scene.cast(o.data(),ends.data(),h.size(),h.data(),width,false);
  for(size_t j=0;j<h.size();++j) {
    const float delta=d[j].x*11+d[j].y*13+d[j].z*17;
    require((h[j].flags&1)&&std::abs(h[j].distance*300-(100-delta))<1.e-3f,"Box two-sided distance failed");
  }
  // Exercise the near/far origin shift, incomplete miss and packet tail.
  Update u{};u.origin={-200,11,13};u.far=true;
  for(auto& r:u.rays) {r.direction={1,0,0};r.range=300;}
  std::array<Hit,RayCount> hits;
  require(eye(scene,u,50,hits.data(),width,true)==2*RayCount,"Far cast count failed");
  for(auto hit:hits)require(hit.flags==3&&std::abs(hit.distance-100)<1.e-3f,"Far distance offset failed");
  u.far=false;require(eye(scene,u,50,hits.data(),width,true)==RayCount,"Near cast count failed");
  for(auto hit:hits)require(hit.flags==0&&hit.distance==50,"Incomplete near miss failed");
}

template<class Scene> void throughput(const Scene& scene,const Input& in,const std::string& name,
                                     int width,bool cull,const std::vector<int>& allowed) {
  std::vector<size_t> counts={1,4,8,16,allowed.size()};std::sort(counts.begin(),counts.end());counts.erase(std::unique(counts.begin(),counts.end()),counts.end());
  for(size_t count:counts)if(count<=allowed.size()) {
    for(int repeat=-1;repeat<Repeats;++repeat) {
      std::barrier ready(int(count)+1),done(int(count)+1);
      std::vector<std::thread> workers;std::vector<uint64_t> casts(count);std::vector<double> checksum(count);
      for(size_t t=0;t<count;++t)workers.emplace_back([&,t] {
        pin(allowed[t]);std::array<Hit,RayCount> local;
        ready.arrive_and_wait();
        for(size_t i=t;i<in.updates.size();i+=count) {
          casts[t]+=eye(scene,in.updates[i],in.nearRange,local.data(),width,cull);
          checksum[t]+=local[i%RayCount].distance;
        }
        done.arrive_and_wait();
      });
      const auto load=loadAverage();
      auto start=Clock::now();ready.arrive_and_wait();done.arrive_and_wait();double seconds=us(start)*1.e-6;
      for(auto& worker:workers)worker.join();
      if(repeat<0)continue;
      uint64_t n=0;double sum=0;for(size_t t=0;t<count;++t) {n+=casts[t];sum+=checksum[t];}
      std::cout<<"{\"type\":\"throughput\",\"backend\":"<<std::quoted(name)<<",\"threads\":"<<count<<",\"repeat\":"<<repeat
               <<",\"updates_per_second\":"<<in.updates.size()/seconds<<",\"casts_per_second\":"<<n/seconds
               <<",\"eye_rays_per_second\":"<<in.updates.size()*RayCount/seconds<<",\"seconds\":"<<seconds<<",\"checksum\":"<<sum;loadJson(load);std::cout<<"}\n"<<std::flush;
    }
  }
}

template<class Scene> void benchmark(const Scene& scene,const Input& in,const std::filesystem::path& output,
                                    const std::string& name,int width,bool cull,const std::vector<int>& allowed) {
  std::array<Hit,RayCount> hits;
  // One complete untimed replay warms the scene and code.
  pin(allowed.front());
  for(const auto& u:in.updates)eye(scene,u,in.nearRange,hits.data(),width,cull);
  for(int repeat=0;repeat<Repeats;++repeat) {
    std::ofstream predictions;
    if(repeat==0)predictions.open(output/(name+".hits"),std::ios::binary);
    std::ofstream raw(output/(name+"-"+std::to_string(repeat)+".times"),std::ios::binary);
    std::vector<double> all,near,full,perCast,nearPerCast,fullPerCast;uint64_t casts=0;
    for(const auto& u:in.updates) {
      auto start=Clock::now();auto n=eye(scene,u,in.nearRange,hits.data(),width,cull);double cost=us(start);
      casts+=n;all.push_back(cost);(u.far?full:near).push_back(cost);perCast.push_back(cost/n);(u.far?fullPerCast:nearPerCast).push_back(cost/n);
      raw.write(reinterpret_cast<const char*>(&cost),sizeof cost);
      if(predictions.is_open())predictions.write(reinterpret_cast<const char*>(hits.data()),sizeof hits);
    }
    require(bool(raw)&&(!predictions.is_open()||bool(predictions)),"Cannot write raw timings/predictions");
    std::cout<<"{\"type\":\"single\",\"backend\":"<<std::quoted(name)<<",\"width\":"<<width<<",\"cull\":"<<cull
             <<",\"repeat\":"<<repeat<<",\"casts\":"<<casts<<",\"updates\":"<<all.size()<<",\"eye_us\":";distribution(std::cout,all);
    std::cout<<",\"near_only_us\":";distribution(std::cout,near);std::cout<<",\"full_us\":";distribution(std::cout,full);
    std::cout<<",\"cast_us\":";distribution(std::cout,perCast);std::cout<<",\"near_only_cast_us\":";distribution(std::cout,nearPerCast);
    std::cout<<",\"full_cast_us\":";distribution(std::cout,fullPerCast);std::cout<<"}\n"<<std::flush;
  }
  throughput(scene,in,name,width,cull,allowed);
}

int main(int argc,char** argv) {
  try {
    std::cout<<std::setprecision(12);
    JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();
    if(argc==2&&std::string(argv[1])=="selftest") {
      auto in=box();JoltScene jolt(in);EmbreeScene embree(in);
      checkBox(jolt,1);jolt.physics.OptimizeBroadPhase();checkBox(jolt,1);
      for(int width:{1,4,8,16})checkBox(embree,width);
      JoltScene copy(jolt);checkBox(copy,1);
      std::cout<<"{\"selftest\":\"passed\",\"backends\":2,\"embree_widths\":[1,4,8,16],\"distance_tolerance_cm\":0.001,\"backfaces\":\"rejected\"}\n";
    } else {
      const bool scaling=argc==5&&std::string(argv[1])=="scaling";
      require(argc==4||scaling,"Usage: raybench selftest | [scaling] INPUT_ROOT OUTPUT_ROOT THREADS");
      const int first=scaling?2:1;
      auto allowed=cpus();size_t threads=std::stoul(argv[first+2]);require(threads>0&&threads<=allowed.size(),"Requested CPUs unavailable");allowed.resize(threads);
      auto in=load(argv[first]);std::filesystem::path out=argv[first+1];require(out.is_absolute(),"Output must be absolute");std::filesystem::create_directories(out);
      std::cout<<"{\"type\":\"metadata\",\"triangles\":"<<in.triangles.size()<<",\"updates\":"<<in.updates.size()
               <<",\"jolt\":\"5.6.0\",\"embree\":\""<<RTC_VERSION_STRING<<"\",\"compiler\":"<<std::quoted(__VERSION__)<<",\"cpus\":[";
      for(size_t i=0;i<allowed.size();++i) {if(i)std::cout<<',';std::cout<<allowed[i];}std::cout<<"]}\n";
      if(scaling) {
        JoltScene jolt(in);jolt.physics.OptimizeBroadPhase();
        characters(jolt,in,out,allowed,true);
        EmbreeScene embree(in);throughput(embree,in,"embree1-cull",1,true,allowed);
      } else {
      {
        std::unique_ptr<JoltScene> holder;
        for(int repeat=0;repeat<Repeats;++repeat) {
          holder.reset();holder=std::make_unique<JoltScene>(in);
          std::cout<<"{\"type\":\"build\",\"backend\":\"jolt\",\"repeat\":"<<repeat<<",\"us\":"<<holder->buildUs<<",\"bodies\":"<<holder->ids.size()<<"}\n";
          if(repeat<Repeats-1) {
            auto start=Clock::now();holder->physics.OptimizeBroadPhase();
            std::cout<<"{\"type\":\"optimize\",\"us\":"<<us(start)<<"}\n";
          }
        }
        auto& jolt=*holder;
        benchmark(jolt,in,out,"jolt-added",1,true,allowed);
        auto start=Clock::now();jolt.physics.OptimizeBroadPhase();
        std::cout<<"{\"type\":\"optimize\",\"us\":"<<us(start)<<"}\n";
        benchmark(jolt,in,out,"jolt-optimized",1,true,allowed);
        characters(jolt,in,out,allowed);
      }
      {
        std::unique_ptr<EmbreeScene> holder;
        for(int repeat=0;repeat<Repeats;++repeat) {
          holder.reset();holder=std::make_unique<EmbreeScene>(in);
          std::cout<<"{\"type\":\"build\",\"backend\":\"embree\",\"repeat\":"<<repeat<<",\"us\":"<<holder->buildUs<<"}\n";
        }
        auto& embree=*holder;
        for(bool cull:{true,false})for(int width:{1,4,8,16})benchmark(embree,in,out,"embree"+std::to_string(width)+(cull?"-cull":"-two-sided"),width,cull,allowed);
      }
      }
    }
    JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;return 0;
  } catch(const std::exception& e) {std::cerr<<"raybench: "<<e.what()<<'\n';return 1;}
}
