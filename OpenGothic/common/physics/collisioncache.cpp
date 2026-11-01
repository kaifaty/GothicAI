#include "collisioncache.h"

#include "sha256.h"
#include "worldchunks.h"

#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/UnorderedMap.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Tempest/Log>

#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
constexpr std::string_view Magic = "GOTHIC-COLLISION-1\n";
constexpr size_t HeaderSize = Magic.size()+64+sizeof(uint32_t);
constexpr size_t MaxFileSize = 256*1024*1024;
constexpr auto JoltVersion=[] { using namespace JPH; return JPH_VERSION_ID; }();
#if defined(JPH_CPU_ARM)
constexpr std::string_view Architecture="arm";
#else
constexpr std::string_view Architecture="x86";
#endif

class Input final : public JPH::StreamIn {
  public:
    explicit Input(std::string_view bytes):bytes(bytes) {}
    void ReadBytes(void* data, size_t size) override {
      if(size>bytes.size())
        throw std::runtime_error("truncated collision cache");
      std::memcpy(data,bytes.data(),size);
      bytes.remove_prefix(size);
      }
    bool IsEOF() const override { return false; }
    bool IsFailed() const override { return false; }
    bool empty() const { return bytes.empty(); }

  private:
    std::string_view bytes;
  };

class Output final : public JPH::StreamOut {
  public:
    explicit Output(std::ofstream& file):file(file) {}
    void WriteBytes(const void* data, size_t size) override {
      file.write(static_cast<const char*>(data),std::streamsize(size));
      checksum=worldChunkCrc32(data,size,checksum);
      }
    bool IsFailed() const override { return !file; }
    uint32_t checksum = 0;

  private:
    std::ofstream& file;
  };
}

CollisionCache::CollisionCache(const fs::path& cacheRoot, const Key& key) {
  const auto* enabled=std::getenv("OPENGOTHIC_COLLISION_CACHE");
  if((enabled && std::string_view(enabled)=="0") || cacheRoot.empty() || key.base.empty() || key.content.empty())
    return;
  if(key.base.find_first_not_of("0123456789abcdef")!=std::string::npos ||
     key.world.empty() || key.world.find_first_of("/\\")!=std::string::npos || key.world=="..")
    return;
  // Jolt serializes native mesh blocks. Keep differing versions, feature flags,
  // endianness and word sizes out of the same derived file.
  const std::string bytes=key.base+"\n"+key.world+"\n"+key.content+"\n"+
      std::to_string(key.collisionFlags)+"\n"+std::to_string(key.version)+"\n"+
      std::to_string(JoltVersion)+"\n"+std::to_string(sizeof(size_t))+"\n"+
      std::to_string(sizeof(JPH::Vec3))+"\n"+std::string(Architecture)+"\n"+
      (std::endian::native==std::endian::little ? "le" : "be");
  identity=Sha256::hash(bytes);
  file=cacheRoot/"derived"/key.base/(key.world+".collision");
  }

bool CollisionCache::read(JPH::ShapeRefC& landscape, JPH::ShapeRefC& water) const {
  if(file.empty())
    return false;
  try {
    std::error_code error;
    const auto size=fs::file_size(file,error);
    if(error || size<HeaderSize || size>MaxFileSize)
      return false;
    std::ifstream input(file,std::ios::binary);
    std::string bytes(size_t(size),'\0');
    if(!input.read(bytes.data(),std::streamsize(bytes.size())))
      return false;
    std::string_view view=bytes;
    if(!view.starts_with(Magic))
      return false;
    view.remove_prefix(Magic.size());
    if(view.substr(0,64)!=identity)
      return false;
    view.remove_prefix(64);
    uint32_t checksum=0;
    std::memcpy(&checksum,view.data(),sizeof(checksum));
    view.remove_prefix(sizeof(checksum));
    // Validate before Jolt interprets any nested counts or native shape data.
    if(worldChunkCrc32(view.data(),view.size())!=checksum)
      return false;
    Physics::initialize();
    Input stream(view);
    JPH::Shape::IDToShapeMap shapes;
    JPH::Shape::IDToMaterialMap materials;
    const auto land=JPH::Shape::sRestoreWithChildren(stream,shapes,materials);
    if(land.HasError())
      return false;
    const auto liquid=JPH::Shape::sRestoreWithChildren(stream,shapes,materials);
    if(liquid.HasError() || !stream.empty())
      return false;
    landscape=land.Get();
    water=liquid.Get();
    return true;
    }
  catch(const std::exception&) {
    // A derived file can always be rebuilt from the original CPU mesh.
    return false;
    }
  }

void CollisionCache::save(const JPH::Shape* landscape, const JPH::Shape* water) const {
  if(file.empty())
    return;
  fs::path temporary;
  try {
    fs::create_directories(file.parent_path());
    temporary=file;
    temporary+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
               "-"+std::to_string(std::random_device{}());
    {
      std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
      output.write(Magic.data(),std::streamsize(Magic.size()));
      output.write(identity.data(),std::streamsize(identity.size()));
      const uint32_t placeholder=0;
      output.write(reinterpret_cast<const char*>(&placeholder),sizeof(placeholder));
      Output stream(output);
      JPH::Shape::ShapeToIDMap shapes;
      JPH::Shape::MaterialToIDMap materials;
      for(const auto* shape:{landscape,water}) {
        if(shape!=nullptr)
          shape->SaveWithChildren(stream,shapes,materials);
        else
          stream.Write(~uint32_t(0));
        }
      output.seekp(std::streamoff(Magic.size()+identity.size()));
      output.write(reinterpret_cast<const char*>(&stream.checksum),sizeof(stream.checksum));
      output.close();
      if(!output)
        throw std::runtime_error("cannot write collision cache");
    }
    fs::rename(temporary,file);
    }
  catch(const std::exception& error) {
    if(!temporary.empty()) {
      std::error_code ignored;
      fs::remove(temporary,ignored);
      }
    Tempest::Log::e("collision cache write failed: ",error.what());
    }
  }
