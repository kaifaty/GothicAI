// nav-sim stand library, task S1: static world in Embree and the engine's eye core.
// The front-face rule and the BVH settings are the ones that matched the engine on
// 99.97-99.996% of the rays in scripts/navsim/raybench (research §3.2).
#include "navsim.h"

#include "game/navigationeye.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#define NAVSIM_API extern "C" __attribute__((visibility("default")))

namespace {
using Tempest::Vec3;
using Kind = NavigationEye::HitKind;

constexpr int32_t Rays = 238, Slots = 2*Rays;
constexpr uint64_t MaxGameMs = 0xFFFFFFFEull; // gpumem stores sampled time + 1 in 32 bits
// Same constant and float arithmetic as perception-replay (navperceptionbench.cpp).
constexpr float Deg = 3.14159265358979323846f/180.f;

NavigationEye::Config eyeConfig() {
  NavigationEye::Config config;
  config.layout = 250; // 238 rays; the walking gaze of the live NPCs stays on
  return config;
  }

struct Mesh final {
  std::vector<float> vertices; // [T*9]
  std::vector<uint8_t> categories;
  };

Mesh readStatic(const char* path) {
  if(path==nullptr)
    throw std::invalid_argument("static.bin path is null");
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if(!in)
    throw std::runtime_error(std::string("cannot open ")+path);
  const auto size = uint64_t(in.tellg());
  in.seekg(0);
  char magic[8] = {};
  uint32_t header[2] = {};
  in.read(magic,8);
  in.read(reinterpret_cast<char*>(header),sizeof(header));
  if(!in || std::memcmp(magic,"NAVSTAT1",8)!=0 || header[1]!=0)
    throw std::runtime_error("invalid NAVSTAT1 header");
  const uint64_t count = header[0];
  if(size!=16+count*37)
    throw std::runtime_error("invalid NAVSTAT1 size");
  Mesh mesh;
  mesh.vertices.resize(count*9);
  mesh.categories.resize(count);
  in.read(reinterpret_cast<char*>(mesh.vertices.data()),std::streamsize(count*36));
  in.read(reinterpret_cast<char*>(mesh.categories.data()),std::streamsize(count));
  if(!in)
    throw std::runtime_error("truncated NAVSTAT1");
  for(auto c:mesh.categories)
    if(c<1 || c>4)
      throw std::runtime_error("invalid NAVSTAT1 category");
  return mesh;
  }

Kind categoryKind(unsigned category) {
  switch(category) {
    case 1: return Kind::World;
    case 2: return Kind::StaticVob;
    case 3: return Kind::Water;
    case 4: return Kind::MovableVob; // kinematic C_Object: isMovable() in the engine
    }
  return Kind::None;
  }

// Back faces are invisible to the eye, as in the engine's navigation ray.
void frontFaces(const RTCFilterFunctionNArguments* a) {
  for(unsigned i=0; i<a->N; ++i) {
    if(!a->valid[i])
      continue;
    const float dot = RTCRayN_dir_x(a->ray,a->N,i)*RTCHitN_Ng_x(a->hit,a->N,i)
                    + RTCRayN_dir_y(a->ray,a->N,i)*RTCHitN_Ng_y(a->hit,a->N,i)
                    + RTCRayN_dir_z(a->ray,a->N,i)*RTCHitN_Ng_z(a->hit,a->N,i);
    if(dot>=0.f)
      a->valid[i] = 0;
    }
  }

class EmbreeWorld final : public NavigationEye::RayCaster {
  public:
    EmbreeWorld(const Mesh& mesh, int32_t threads) {
      const std::string config = "threads="+std::to_string(std::max(threads,1))+",set_affinity=0";
      device = rtcNewDevice(config.c_str());
      if(device==nullptr)
        throw std::runtime_error("Embree device creation failed");
      scene = rtcNewScene(device);
      rtcSetSceneFlags(scene,RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
      const size_t count = mesh.categories.size();
      for(unsigned c=1; c<=4; ++c) {
        const size_t n = size_t(std::count(mesh.categories.begin(),mesh.categories.end(),uint8_t(c)));
        if(n==0)
          continue;
        auto g = rtcNewGeometry(device,RTC_GEOMETRY_TYPE_TRIANGLE);
        auto* v = static_cast<float*>(rtcSetNewGeometryBuffer(g,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT3,
                                                              3*sizeof(float),n*3));
        auto* idx = static_cast<uint32_t*>(rtcSetNewGeometryBuffer(g,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,
                                                                   3*sizeof(uint32_t),n));
        size_t j = 0;
        for(size_t i=0; i<count; ++i) {
          if(mesh.categories[i]!=c)
            continue;
          std::memcpy(v+j*9,mesh.vertices.data()+i*9,9*sizeof(float));
          for(int k=0; k<3; ++k)
            idx[j*3+k] = uint32_t(j*3+k);
          ++j;
          }
        rtcSetGeometryEnableFilterFunctionFromArguments(g,true);
        rtcCommitGeometry(g);
        rtcAttachGeometryByID(scene,g,c);
        rtcReleaseGeometry(g);
        }
      rtcCommitScene(scene);
      if(rtcGetDeviceError(device)!=RTC_ERROR_NONE)
        throw std::runtime_error("Embree BVH construction failed");
      triangles = int64_t(count);
      }

    ~EmbreeWorld() override {
      if(scene!=nullptr)
        rtcReleaseScene(scene);
      if(device!=nullptr)
        rtcReleaseDevice(device);
      }

    NavigationEye::RayHit cast(const Vec3& from, const Vec3& to, const Npc*) const override {
      RTCRayHit rh{};
      rh.ray.org_x = from.x; rh.ray.org_y = from.y; rh.ray.org_z = from.z;
      rh.ray.dir_x = to.x-from.x; rh.ray.dir_y = to.y-from.y; rh.ray.dir_z = to.z-from.z;
      rh.ray.tnear = 0.f;
      rh.ray.tfar = 1.f; // the segment from->to
      rh.ray.mask = ~0u;
      rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
      rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
      RTCIntersectArguments args;
      rtcInitIntersectArguments(&args);
      args.filter = frontFaces;
      rtcIntersect1(scene,&rh,&args);
      NavigationEye::RayHit result;
      if(rh.hit.geomID==RTC_INVALID_GEOMETRY_ID)
        return result;
      const float t = rh.ray.tfar;
      result.hit = true;
      result.point = from+(to-from)*t;
      const float nx = rh.hit.Ng_x, ny = rh.hit.Ng_y, nz = rh.hit.Ng_z;
      const float length = std::sqrt(nx*nx+ny*ny+nz*nz);
      result.normalY = length>0.f ? std::clamp(ny/length,-1.f,1.f) : 0.f;
      result.kind = categoryKind(rh.hit.geomID);
      return result;
      }

    int64_t triangles = 0;

  private:
    RTCDevice device = nullptr;
    RTCScene scene = nullptr;
  };

// Fixed workers that split [0, items) in chunks; the caller blocks until done.
class Pool final {
  public:
    Pool(int32_t threads, const int32_t* cpus) {
      for(int32_t i=0; i<threads; ++i)
        workers.emplace_back([this,cpu=cpus!=nullptr ? cpus[i] : -1] { loop(cpu); });
      }

    ~Pool() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
      }
      wake.notify_all();
      for(auto& w:workers)
        w.join();
      }

    size_t size() const { return workers.size(); }

    void run(size_t items, const std::function<void(size_t,size_t)>& work) {
      if(items==0)
        return;
      std::unique_lock<std::mutex> lock(mutex);
      job = &work;
      total = items;
      chunk = std::max<size_t>(1,items/(workers.size()*8));
      next.store(0);
      pending = workers.size();
      ++generation;
      wake.notify_all();
      finished.wait(lock,[this] { return pending==0; });
      job = nullptr;
      }

  private:
    void loop(int cpu) {
#if defined(__linux__)
      if(cpu>=0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu,&set);
        pthread_setaffinity_np(pthread_self(),sizeof(set),&set);
        }
#else
      (void)cpu;
#endif
      uint64_t seen = 0;
      for(;;) {
        const std::function<void(size_t,size_t)>* work = nullptr;
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock,[&] { return stop || generation!=seen; });
          if(stop)
            return;
          seen = generation;
          work = job;
        }
        for(;;) {
          const size_t begin = next.fetch_add(chunk);
          if(begin>=total)
            break;
          (*work)(begin,std::min(total,begin+chunk));
          }
        std::lock_guard<std::mutex> lock(mutex);
        if(--pending==0)
          finished.notify_one();
        }
      }

    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake, finished;
    const std::function<void(size_t,size_t)>* job = nullptr;
    std::atomic<size_t> next{0};
    size_t total = 0, chunk = 1, pending = 0;
    uint64_t generation = 0;
    bool stop = false;
  };

struct Agent final {
  NavigationEye eye{eyeConfig()};
  NavigationEye::Pose pose;
  uint64_t gameMs = 0;
  bool posed = false;
  };

void writeSlots(const NavigationEye& eye, size_t agent, const navsim_slots& out) {
  const auto& rays = eye.memorySamples();
  for(size_t k=0; k<rays.size(); ++k) {
    const auto& r = rays[k];
    const size_t i = agent*size_t(Slots)+k;
    if(out.origin)    { out.origin[3*i] = r.origin.x; out.origin[3*i+1] = r.origin.y; out.origin[3*i+2] = r.origin.z; }
    if(out.point)     { out.point[3*i] = r.point.x; out.point[3*i+1] = r.point.y; out.point[3*i+2] = r.point.z; }
    if(out.direction) { out.direction[3*i] = r.direction.x; out.direction[3*i+1] = r.direction.y; out.direction[3*i+2] = r.direction.z; }
    if(out.distance)  out.distance[i] = r.distanceCm;
    if(out.normal_y)  out.normal_y[i] = r.normalY;
    if(out.kind)      out.kind[i] = uint8_t(r.kind);
    if(out.hit)       out.hit[i] = r.hit;
    if(out.complete)  out.complete[i] = r.complete;
    if(out.time)      out.time[i] = uint32_t(r.sampledAtMs);
    }
  }

bool finite(const float* v, size_t n) {
  for(size_t i=0; i<n; ++i)
    if(!std::isfinite(v[i]))
      return false;
  return true;
  }

Vec3 vec(const float* v, int32_t i) {
  return Vec3(v[3*i],v[3*i+1],v[3*i+2]);
  }
}

struct navsim_world {
  std::unique_ptr<EmbreeWorld> rays;
  std::vector<Agent> agents;
  std::unique_ptr<Pool> pool;
  std::string error;

  bool range(int32_t first, int32_t count) {
    if(first<0 || count<0 || int64_t(first)+count>int64_t(agents.size())) {
      error = "agent range out of bounds";
      return false;
      }
    return true;
    }
  };

namespace {
template<class F>
auto guarded(navsim_world* world, F&& f, decltype(f()) failure) -> decltype(f()) {
  if(world==nullptr)
    return failure;
  try {
    world->error.clear();
    return f();
    }
  catch(const std::exception& e) {
    world->error = e.what();
    }
  catch(...) {
    world->error = "unknown error";
    }
  return failure;
  }

void copyError(char* error, size_t size, const char* message) {
  if(error!=nullptr && size>0) {
    std::strncpy(error,message,size-1);
    error[size-1] = '\0';
    }
  }
}

NAVSIM_API int32_t navsim_eye_rays(void) { return Rays; }
NAVSIM_API int32_t navsim_eye_slots(void) { return Slots; }

NAVSIM_API navsim_world* navsim_world_create(const char* static_bin, int32_t agents, int32_t threads,
                                             char* error, size_t error_size) {
  try {
    if(agents<0 || threads<1)
      throw std::invalid_argument("agents must be >= 0 and threads >= 1");
    const auto mesh = readStatic(static_bin);
    auto world = std::make_unique<navsim_world>();
    world->rays = std::make_unique<EmbreeWorld>(mesh,threads);
    world->agents.resize(size_t(agents));
    world->pool = std::make_unique<Pool>(threads,nullptr);
    for(const auto& a:world->agents)
      if(size_t(a.eye.rayCount())!=size_t(Rays))
        throw std::logic_error("eye layout changed: expected 238 rays");
    return world.release();
    }
  catch(const std::exception& e) {
    copyError(error,error_size,e.what());
    }
  catch(...) {
    copyError(error,error_size,"unknown error");
    }
  return nullptr;
  }

NAVSIM_API void navsim_world_destroy(navsim_world* world) {
  delete world;
  }

NAVSIM_API int32_t navsim_world_agents(const navsim_world* world) {
  return world!=nullptr ? int32_t(world->agents.size()) : -1;
  }

NAVSIM_API int64_t navsim_world_triangles(const navsim_world* world) {
  return world!=nullptr ? world->rays->triangles : -1;
  }

NAVSIM_API const char* navsim_last_error(const navsim_world* world) {
  return world!=nullptr ? world->error.c_str() : "null world";
  }

NAVSIM_API int32_t navsim_set_threads(navsim_world* world, int32_t threads, const int32_t* cpus) {
  return guarded(world,[&]() -> int32_t {
    if(threads<1)
      throw std::invalid_argument("threads must be >= 1");
    world->pool.reset();
    world->pool = std::make_unique<Pool>(threads,cpus);
    return 0;
    },-1);
  }

NAVSIM_API int32_t navsim_set_poses(navsim_world* world, int32_t first, int32_t count,
                                    const float* origin, const float* forward, const float* right,
                                    const float* up, const float* position, const uint64_t* game_ms) {
  return guarded(world,[&]() -> int32_t {
    if(!world->range(first,count))
      return -1;
    if(!origin || !forward || !right || !up || !position || !game_ms)
      throw std::invalid_argument("null pose array");
    const size_t n = size_t(count)*3;
    if(!finite(origin,n) || !finite(forward,n) || !finite(right,n) || !finite(up,n) || !finite(position,n))
      throw std::invalid_argument("non-finite pose");
    for(int32_t i=0; i<count; ++i)
      if(game_ms[i]>MaxGameMs)
        throw std::invalid_argument("game time does not fit the 32-bit eye slots");
    for(int32_t i=0; i<count; ++i) {
      auto& a = world->agents[size_t(first+i)];
      a.pose.origin = vec(origin,i);
      a.pose.forward = vec(forward,i);
      a.pose.right = vec(right,i);
      a.pose.up = vec(up,i);
      a.pose.position = vec(position,i);
      a.gameMs = game_ms[i];
      a.posed = true;
      }
    return 0;
    },-1);
  }

NAVSIM_API int32_t navsim_set_poses_yaw(navsim_world* world, int32_t first, int32_t count,
                                        const float* origin, const float* position, const float* yaw_deg,
                                        const uint64_t* game_ms) {
  return guarded(world,[&]() -> int32_t {
    if(!world->range(first,count))
      return -1;
    if(!origin || !position || !yaw_deg || !game_ms)
      throw std::invalid_argument("null pose array");
    std::vector<float> forward(size_t(count)*3), right(size_t(count)*3), up(size_t(count)*3);
    for(int32_t i=0; i<count; ++i) {
      const float yaw = yaw_deg[i]*Deg;
      const Vec3 f(std::cos(yaw),0,std::sin(yaw)), r(-f.z,0,f.x);
      forward[3*size_t(i)] = f.x; forward[3*size_t(i)+1] = f.y; forward[3*size_t(i)+2] = f.z;
      right[3*size_t(i)] = r.x;   right[3*size_t(i)+1] = r.y;   right[3*size_t(i)+2] = r.z;
      up[3*size_t(i)+1] = 1.f;
      }
    return navsim_set_poses(world,first,count,origin,forward.data(),right.data(),up.data(),position,game_ms);
    },-1);
  }

NAVSIM_API int32_t navsim_reset_eyes(navsim_world* world, int32_t first, int32_t count) {
  return guarded(world,[&]() -> int32_t {
    if(!world->range(first,count))
      return -1;
    for(int32_t i=0; i<count; ++i) {
      auto& a = world->agents[size_t(first+i)];
      a.eye.reset();
      }
    return 0;
    },-1);
  }

NAVSIM_API int64_t navsim_eye_update(navsim_world* world, const navsim_slots* out,
                                     uint8_t* updated, uint8_t* far_updated, uint32_t* casts) {
  return guarded(world,[&]() -> int64_t {
    std::atomic<int64_t> total{0};
    std::mutex failure;
    std::string message;
    const NavigationEye::RayCaster& rays = *world->rays;
    world->pool->run(world->agents.size(),[&](size_t begin, size_t end) {
      int64_t local = 0;
      for(size_t i=begin; i<end; ++i) {
        auto& a = world->agents[i];
        NavigationEye::Update u;
        try {
          if(a.posed)
            u = a.eye.update(a.pose,rays,nullptr,a.gameMs);
          if(out!=nullptr)
            writeSlots(a.eye,i,*out);
          }
        catch(const std::exception& e) {
          std::lock_guard<std::mutex> lock(failure);
          if(message.empty())
            message = e.what();
          }
        if(updated)     updated[i] = u.updated;
        if(far_updated) far_updated[i] = u.farUpdated;
        if(casts)       casts[i] = uint32_t(u.casts);
        local += int64_t(u.casts);
        }
      total.fetch_add(local,std::memory_order_relaxed);
      });
    if(!message.empty())
      throw std::runtime_error(message);
    return total.load();
    },-1);
  }

NAVSIM_API int32_t navsim_eye_read(const navsim_world* world, int32_t first, int32_t count, const navsim_slots* out) {
  auto* mutableWorld = const_cast<navsim_world*>(world);
  return guarded(mutableWorld,[&]() -> int32_t {
    if(!mutableWorld->range(first,count))
      return -1;
    if(out==nullptr)
      throw std::invalid_argument("null slots");
    for(int32_t i=0; i<count; ++i)
      writeSlots(world->agents[size_t(first+i)].eye,size_t(i),*out);
    return 0;
    },-1);
  }
