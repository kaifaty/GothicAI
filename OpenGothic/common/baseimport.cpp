#include "baseimport.h"
#include "asset/fromzenkit.h"
#include "basemesh.h"
#include "fontdata.h"
#include "modeldata.h"
#include "worldchunksruntime.h"
#include "worlddata.h"
#include "gothicglb.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <zenkit/Font.hh>
#include <zenkit/Logger.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Model.hh>
#include <zenkit/ModelAnimation.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/MorphMesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/Texture.hh>
#include <zenkit/World.hh>
#include <zenkit/addon/texcvt.hh>

// miniz otherwise #defines crc32 and other zlib names.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace BaseImport {
namespace {

thread_local std::string* importError=nullptr;

struct ImportErrors {
  std::string error;
  std::string* previous=std::exchange(importError,&error);
  ~ImportErrors() { importError=previous; }
  void check() const {
    if(!error.empty()) throw std::runtime_error("ZenKit "+error);
    }
  };

void require(bool ok, const std::string& what) {
  if(!ok)
    throw std::runtime_error(what);
  }

std::string upper(std::string s) {
  for(auto& c:s)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
  }

struct Archive {
  std::string name; // upper case file name
  uint64_t    size;
  };

struct SourceWorld {
  WorldChunkSource source;
  asset::WorldMesh mesh;
  WorldData::Data  data;
  std::vector<Archive> archives;
  };

// Every Data/*.vdf with OLDER, as the engine mounts them (resources.cpp):
// for each name the newest archive's copy wins, whatever the mount order.
std::vector<Archive> mountArchives(zenkit::Vfs& vfs, const fs::path& gothic) {
  std::map<std::string,fs::path> archives;
  for(const auto& e:fs::directory_iterator(gothic/"Data"))
    if(e.is_regular_file() && upper(e.path().extension().string())==".VDF")
      archives[upper(e.path().filename().string())] = e.path();
  require(!archives.empty(),"no archives in "+(gothic/"Data").string());
  std::vector<Archive> mounted;
  for(const auto& [name,path]:archives) {
    vfs.mount_disk(path,zenkit::VfsOverwriteBehavior::OLDER);
    mounted.push_back({name,uint64_t(fs::file_size(path))});
    }
  return mounted;
  }

std::vector<std::byte> readAll(const zenkit::VfsNode& file) {
  std::vector<std::byte> bytes;
  auto in = file.open_read();
  std::byte buf[1<<16];
  for(;;) {
    const auto n = in->read(buf,sizeof(buf));
    if(n==0)
      break;
    bytes.insert(bytes.end(),buf,buf+n);
    }
  return bytes;
  }

// The engine checks size and CRC32 of the .ZEN it opened against world.json.
SourceWorld loadWorld(const fs::path& gothic, const std::string& name) {
  zenkit::Vfs vfs;
  const auto archives = mountArchives(vfs,gothic);
  const auto* file = vfs.find(name);
  require(file!=nullptr,"missing world "+name);

  const auto bytes = readAll(*file);
  SourceWorld r;
  r.source.world = upper(name);
  r.source.size  = bytes.size();
  r.source.crc32 = worldChunkCrc32(bytes.data(),bytes.size());

  zenkit::World world;
  auto in = zenkit::Read::from(&bytes);
  try {
    ImportErrors errors;
    world.load(in.get(),zenkit::GameVersion::GOTHIC_2);
    errors.check();
    }
  catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
  r.data = WorldData::fromWorld(world,r.source);
  r.archives = archives;
  r.mesh = asset::fromZenKit(world.world_mesh);
  return r;
  }

int verify(const asset::WorldMesh& source, const fs::path& dir, const fs::path& overrideDir) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto expected = canonicalWorldMesh(source);
  const auto actual   = readWorldChunks(dir,overrideDir);
  const auto diff     = compareWorldMeshes(expected,actual);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << "triangles " << actual.polygons.material_indices.size()
            << ", vertices " << actual.vertices.size()
            << ", features " << actual.features.size()
            << ", materials " << actual.materials.size()
            << " (read + compare " << ms << " ms)\n";
  if(!diff.empty()) {
    std::cout << "MISMATCH: " << diff << "\n";
    return 1;
    }
  std::cout << "equal to the source mesh\n";
  return 0;
  }

bool isDxt(zenkit::TextureFormat f) {
  return f==zenkit::TextureFormat::DXT1 || f==zenkit::TextureFormat::DXT2 || f==zenkit::TextureFormat::DXT3 ||
         f==zenkit::TextureFormat::DXT4 || f==zenkit::TextureFormat::DXT5;
  }

void collectFiles(const zenkit::VfsNode& node, std::vector<const zenkit::VfsNode*>& out) {
  for(const auto& child:node.children()) {
    if(child.type()==zenkit::VfsNodeType::DIRECTORY)
      collectFiles(child,out);
    else
      out.push_back(&child);
    }
  }

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size()>=tail.size() && s.compare(s.size()-tail.size(),tail.size(),tail)==0;
  }

bool modelSource(const fs::path& path) {
  const auto ext=upper(path.extension().string());
  return ext==".MDH" || ext==".MDM" || ext==".MDL" || ext==".MAN" ||
         ext==".MSB" || ext==".MDS" || ext==".MMB";
  }

std::vector<std::byte> readHost(const fs::path& path) {
  std::ifstream f(path,std::ios::binary|std::ios::ate);
  require(bool(f),"cannot open "+path.string());
  std::vector<std::byte> b(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(b.data()),std::streamsize(b.size()));
  require(bool(f),"cannot read "+path.string());
  return b;
  }

void writeHost(const fs::path& path, const void* data, size_t size) {
  const auto tmp = path.string()+".tmp";
  {
  std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
  f.write(static_cast<const char*>(data),std::streamsize(size));
  require(bool(f),"cannot write "+tmp);
  }
  fs::rename(tmp,path);
  }

std::vector<uint8_t> encodePng(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(),int(w),int(h),4,&len,MZ_DEFAULT_LEVEL,MZ_FALSE);
  require(png!=nullptr,"PNG encoding failed");
  std::vector<uint8_t> out(static_cast<uint8_t*>(png),static_cast<uint8_t*>(png)+len);
  mz_free(png);
  return out;
  }

// Decodes the PNGs encodePng writes (8-bit RGBA, filter 0) to check the round trip.
std::vector<uint8_t> decodePng(const std::vector<std::byte>& png, uint32_t& w, uint32_t& h) {
  auto u32 = [&](size_t o) {
    return (uint32_t(png[o])<<24)|(uint32_t(png[o+1])<<16)|(uint32_t(png[o+2])<<8)|uint32_t(png[o+3]);
    };
  require(png.size()>8+25,"PNG too short");
  std::vector<uint8_t> idat;
  size_t o = 8;
  while(o+12<=png.size()) {
    const uint32_t len = u32(o);
    const std::string type(reinterpret_cast<const char*>(png.data()+o+4),4);
    require(o+12+len<=png.size(),"PNG chunk outside the file");
    if(type=="IHDR") {
      w = u32(o+8);
      h = u32(o+12);
      require(uint8_t(png[o+16])==8 && uint8_t(png[o+17])==6,"PNG must be 8-bit RGBA");
      } else if(type=="IDAT") {
      idat.insert(idat.end(),reinterpret_cast<const uint8_t*>(png.data()+o+8),reinterpret_cast<const uint8_t*>(png.data()+o+8+len));
      }
    o += 12+len;
    }
  size_t outLen = 0;
  void* raw = tinfl_decompress_mem_to_heap(idat.data(),idat.size(),&outLen,TINFL_FLAG_PARSE_ZLIB_HEADER);
  require(raw!=nullptr,"PNG inflate failed");
  const auto* r = static_cast<const uint8_t*>(raw);
  const size_t stride = size_t(w)*4;
  std::vector<uint8_t> rgba;
  if(outLen==(stride+1)*h) {
    rgba.reserve(stride*h);
    for(uint32_t y=0; y<h; ++y) {
      if(r[y*(stride+1)]!=0) {
        mz_free(raw);
        throw std::runtime_error("PNG row filter is not 0");
        }
      rgba.insert(rgba.end(),r+y*(stride+1)+1,r+(y+1)*(stride+1));
      }
    }
  mz_free(raw);
  require(rgba.size()==stride*h,"PNG size does not match IHDR");
  return rgba;
  }

std::string jsonName(const std::string& s) {
  for(const char c:s)
    require(static_cast<unsigned char>(c)>=0x20 && static_cast<unsigned char>(c)<0x80 && c!='"' && c!='\\',
            "texture name outside plain ASCII: "+s);
  return "\""+s+"\"";
  }

std::string archivesJson(const std::vector<Archive>& archives) {
  std::string m = "[";
  for(size_t i=0; i<archives.size(); ++i)
    m += std::string(i==0 ? "\n    " : ",\n    ") + "{\"name\": " + jsonName(archives[i].name) +
         ", \"size\": " + std::to_string(archives[i].size) + "}";
  return m + "\n  ]";
  }

void addWorldArchives(const fs::path& dir, const std::vector<Archive>& archives) {
  const auto bytes=readHost(dir/"world.json");
  std::string text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!text.empty() && text[0]=='{',"invalid world.json");
  text.insert(1,"\n  \"archives\": "+archivesJson(archives)+",");
  writeHost(dir/"world.json",text.data(),text.size());
  }

rapidjson::Document parseJson(const fs::path& file) {
  const auto bytes=readHost(file);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError() && doc.IsObject(),"invalid JSON: "+file.string());
  return doc;
  }

int verifyWorld(const SourceWorld& world, const fs::path& dir, const fs::path& overrideDir) {
  const auto t0=std::chrono::steady_clock::now();
  if(verify(world.mesh,dir,overrideDir)!=0) return 1;
  const auto manifest=readWorldChunkManifest(dir);
  require(manifest.source.world==world.source.world && manifest.source.size==world.source.size &&
          manifest.source.crc32==world.source.crc32,"world.json source differs");
  const auto json=parseJson(dir/"world.json");
  rapidjson::Document archives;
  archives.Parse(archivesJson(world.archives).c_str());
  require(json.HasMember("archives") && json["archives"]==archives,"world.json archives differ");
  const auto data=WorldData::read(dir);
  const auto diff=WorldData::compare(world.data,data);
  require(diff.empty(),world.source.world+": "+diff);
  uint64_t bytes=0;
  for(const auto* file:{"vobs.json","waynet.json","sectors.json","bsp.bin","rooms.bin"}) bytes+=fs::file_size(dir/file);
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << world.source.world << ": vobs " << WorldData::vobCount(data.vobs)
            << ", points " << (data.waynet ? data.waynet->points.size() : 0)
            << ", edges " << (data.waynet ? data.waynet->edges.size() : 0)
            << ", sectors " << data.bsp.sectors.size() << ", nodes " << data.bsp.nodes.size()
            << ", leaves " << data.bsp.leaf_node_indices.size() << "; data " << bytes
            << " bytes; ZenKit ASCII, BSP and rooms equal (" << ms << " ms)\n";
  return 0;
  }

}

void onZenkitLog(zenkit::LogLevel level, const char* component, const char* message) {
  if(level==zenkit::LogLevel::ERROR && importError!=nullptr && importError->empty())
    *importError=std::string(component)+": "+message;
  }

int convertZen(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0=std::chrono::steady_clock::now();
  std::string openedSource;
  zenkit::Vfs vfs;
  vfs.set_open_observer([&](std::string_view, std::string_view source) { openedSource=source; });
  const auto archives=mountArchives(vfs,gothic);
  // Match Resources::mountWork, including its priority and VFS name lookup.
  for(const auto& entry:fs::directory_iterator(gothic))
    if(entry.is_directory() && upper(entry.path().filename().string())=="_WORK") {
      vfs.mkdir("/_work");
      vfs.mount_host(fs::absolute(entry.path()),"/_work",zenkit::VfsOverwriteBehavior::NONE);
      break;
      }
  std::vector<const zenkit::VfsNode*> files;
  collectFiles(vfs.root(),files);
  std::map<std::string,const zenkit::VfsNode*> zens;
  for(const auto* file:files)
    if(endsWith(upper(file->name()),".ZEN")) zens[upper(file->name())]=vfs.find(file->name());
  if(!verifyOnly) fs::create_directories(out);
  std::string entries="{", loose="[";
  uint64_t bytes=0;
  size_t count=0, worlds=0, looseCount=0;
  for(const auto& [name,node]:zens) {
    if(name=="LENSFLARE.ZEN") {
      std::cout << "skipped LENSFLARE.ZEN: not consumed by the engine\n";
      continue;
      }
    const auto sourceBytes=readAll(*node);
    WorldData::ZenFile zen;
    const WorldChunkSource source{name,sourceBytes.size(),worldChunkCrc32(sourceBytes.data(),sourceBytes.size())};
    try {
      ImportErrors errors;
      const bool meshless=WorldData::loadZen(sourceBytes,source,zen);
      errors.check();
      if(!meshless) { ++worlds; continue; }
      const auto file=name.substr(0,name.size()-4)+".json";
      if(!verifyOnly) WorldData::writeZen(zen,out/file);
      const auto diff=WorldData::compareZen(zen,WorldData::readZen(out/file));
      require(diff.empty(),diff);
      if(count++) entries+=",";
      entries+="\n    "+gothicglb::jsonString(name)+":"+gothicglb::jsonString(file);
      if(fs::path(openedSource).is_absolute()) {
        const auto path=fs::path(openedSource).lexically_relative(fs::absolute(gothic)).generic_string();
        if(looseCount++) loose+=",";
        loose+="\n    {\"path\":"+gothicglb::jsonString(path)+",\"size\":"+std::to_string(sourceBytes.size())+"}";
        }
      bytes+=fs::file_size(out/file);
      }
    catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
    }
  const auto index="{\n  \"format\":1,\n  \"archives\":"+archivesJson(archives)+
                   ",\n  \"loose_files\":"+loose+"\n  ],\n  \"files\":"+entries+"\n  }\n}\n";
  if(!verifyOnly) writeHost(out/"zen.json",index.data(),index.size());
  const auto actual=parseJson(out/"zen.json");
  rapidjson::Document expected; expected.Parse(index.c_str());
  require(actual==expected,"zen.json files, archives or loose files differ");
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written and verified ") << count << " ZEN files, " << worlds
            << " mesh worlds excluded; " << bytes << " bytes; 0 failed (" << ms << " ms)\n";
  return 0;
  }

// Writes (or, with verifyOnly, re-reads and compares) every texture.
int convertTextures(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0 = std::chrono::steady_clock::now();
  zenkit::Vfs vfs;
  const auto archives = mountArchives(vfs,gothic);
  std::vector<const zenkit::VfsNode*> files;
  collectFiles(vfs.root(),files);
  std::map<std::string,const zenkit::VfsNode*> compiled, raw;
  for(const auto* f:files) {
    const auto name = upper(f->name());
    if(endsWith(name,"-C.TEX"))
      compiled[name.substr(0,name.size()-6)] = f;
    else if(endsWith(name,".TGA"))
      raw[name.substr(0,name.size()-4)] = f;
    }
  if(!verifyOnly)
    fs::create_directories(out);

  std::map<std::string,std::string> index;
  size_t dds = 0, png = 0, tga = 0, bad = 0;
  uint64_t bytes = 0;
  for(const auto& [stem,node]:compiled) {
    try {
      ImportErrors errors;
      zenkit::Texture tex;
      auto in = node->open_read();
      tex.load(in.get());
      errors.check();
      if(isDxt(tex.format())) {
        const auto data = zenkit::to_dds(tex);
        const auto file = stem+".dds";
        if(verifyOnly)
          require(readHost(out/file)==data,"differs: "+file);
        else
          writeHost(out/file,data.data(),data.size());
        index[stem] = file;
        bytes += data.size();
        ++dds;
        } else {
        const auto rgba = tex.as_rgba8(0);
        const auto file = stem+".png";
        if(!verifyOnly) {
          const auto data = encodePng(rgba,tex.width(),tex.height());
          writeHost(out/file,data.data(),data.size());
          }
        uint32_t w = 0, h = 0;
        const auto back = decodePng(readHost(out/file),w,h);
        require(w==tex.width() && h==tex.height() && back==rgba,"differs: "+file);
        index[stem] = file;
        bytes += fs::file_size(out/file);
        ++png;
        }
      }
    catch(const std::exception& e) {
      std::cerr << stem << "-C.TEX: " << e.what() << "\n";
      ++bad;
      }
    }
  for(const auto& [stem,node]:raw) {
    if(compiled.count(stem))
      continue; // the engine reads the compiled texture first
    try {
      const auto data = readAll(*node);
      const auto file = stem+".tga";
      if(verifyOnly)
        require(readHost(out/file)==data,"differs: "+file);
      else
        writeHost(out/file,data.data(),data.size());
      index[stem] = file;
      bytes += data.size();
      ++tga;
      }
    catch(const std::exception& e) {
      std::cerr << stem << ".TGA: " << e.what() << "\n";
      ++bad;
      }
    }

  if(!verifyOnly) {
    std::string m = "{\n  \"format\": \"gothic-textures\",\n  \"version\": 1,\n  \"archives\": " + archivesJson(archives) +
                    ",\n  \"textures\": {";
    bool first = true;
    for(const auto& [stem,file]:index) {
      m += std::string(first ? "\n    " : ",\n    ") + jsonName(stem) + ": " + jsonName(file);
      first = false;
      }
    m += "\n  }\n}\n";
    writeHost(out/"textures.json",m.data(),m.size());
    }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written ") << index.size() << " textures: " << dds << " dds, " << png
            << " png, " << tga << " tga; " << (bytes>>20) << " MiB; " << bad << " failed (" << ms << " ms)\n";
  return bad==0 ? 0 : 1;
  }

int convertMeshes(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0 = std::chrono::steady_clock::now();
  zenkit::Vfs vfs;
  const auto archives = mountArchives(vfs,gothic);
  std::vector<const zenkit::VfsNode*> files;
  collectFiles(vfs.root(),files);
  std::map<std::string,const zenkit::VfsNode*> meshes;
  for(const auto* f:files) {
    const auto name = upper(f->name());
    if(endsWith(name,".MRM"))
      meshes[name.substr(0,name.size()-4)] = f;
    }
  if(!verifyOnly)
    fs::create_directories(out);
  std::map<std::string,std::string> index;
  size_t bad = 0, triangles = 0;
  uint64_t bytes = 0;
  for(const auto& [stem,node]:meshes) {
    try {
      ImportErrors errors;
      zenkit::MultiResolutionMesh archiveMesh;
      auto in = node->open_read();
      archiveMesh.load(in.get());
      errors.check();
      const auto mrm = asset::fromZenKit(archiveMesh);
      const auto file = stem+".glb";
      if(!verifyOnly)
        writeBaseMesh(mrm,stem,out/file);
      const auto diff = compareBaseMeshes(mrm,readBaseMesh(out/file));
      require(diff.empty(),diff);
      for(const auto& sm:mrm.sub_meshes)
        triangles += sm.triangles.size();
      index[stem] = file;
      bytes += fs::file_size(out/file);
      }
    catch(const std::exception& e) {
      std::cerr << stem << ".MRM: " << e.what() << "\n";
      ++bad;
      }
    }
  if(!verifyOnly) {
    std::string m = "{\n  \"format\": \"gothic-meshes\",\n  \"version\": 1,\n  \"archives\": " + archivesJson(archives) +
                    ",\n  \"meshes\": {";
    bool first = true;
    for(const auto& [stem,file]:index) {
      m += std::string(first ? "\n    " : ",\n    ") + jsonName(stem) + ": " + jsonName(file);
      first = false;
      }
    m += "\n  }\n}\n";
    writeHost(out/"meshes.json",m.data(),m.size());
    }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written ") << index.size() << " meshes, " << triangles << " triangles; "
            << (bytes>>20) << " MiB; " << bad << " failed (" << ms << " ms)\n";
  return bad==0 ? 0 : 1;
  }

int convertModels(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0=std::chrono::steady_clock::now();
  std::string openedSource;
  zenkit::Vfs vfs;
  vfs.set_open_observer([&](std::string_view, std::string_view source) { openedSource=source; });
  const auto archives=mountArchives(vfs,gothic);
  for(const auto& entry:fs::directory_iterator(gothic))
    if(entry.is_directory() && upper(entry.path().filename().string())=="_WORK") {
      vfs.mkdir("/_work");
      vfs.mount_host(fs::absolute(entry.path()),"/_work",zenkit::VfsOverwriteBehavior::NONE);
      break;
      }
  std::vector<const zenkit::VfsNode*> files;
  collectFiles(vfs.root(),files);
  std::map<std::string,const zenkit::VfsNode*> models;
  for(const auto* file:files)
    if(modelSource(file->name())) models[upper(file->name())]=vfs.find(file->name());
  if(!verifyOnly) {
    fs::create_directories(out);
    fs::remove(out/"models.json");
    }

  std::string entries="{", loose="[";
  size_t count=0, looseCount=0;
  uint64_t bytes=0;
  for(const auto& [name,node]:models) {
    const auto file=name+".json";
    const auto path=out/file;
    const auto ext=fs::path(name).extension().string();
    try {
      ImportErrors errors;
      auto input=node->open_read();
      std::string diff;
      if(ext==".MDH") {
        zenkit::ModelHierarchy source {};
        source.load(input.get());
        errors.check();
        const auto own=asset::fromZenKit(source);
        if(!verifyOnly) ModelData::writeHierarchy(own,path);
        diff=ModelData::compare(own,ModelData::readHierarchy(path));
        }
      else if(ext==".MDM") {
        const auto order=asset::modelAttachmentOrder(input.get());
        zenkit::ModelMesh source {};
        source.load(input.get());
        errors.check();
        const auto own=asset::fromZenKit(source);
        if(!verifyOnly) ModelData::writeMesh(own,path,order);
        diff=ModelData::compare(own,ModelData::readMesh(path));
        }
      else if(ext==".MDL") {
        zenkit::Model source {};
        source.hierarchy.load(input.get());
        const auto order=asset::modelAttachmentOrder(input.get());
        source.mesh.load(input.get());
        errors.check();
        const auto own=asset::fromZenKit(source);
        if(!verifyOnly) ModelData::writeModel(own,path,order);
        diff=ModelData::compare(own,ModelData::readModel(path));
        }
      else if(ext==".MAN") {
        zenkit::ModelAnimation source {};
        source.load(input.get());
        errors.check();
        const auto encoded=asset::animationSamplesFromZenKit(input.get(),source);
        const auto own=asset::fromZenKit(source);
        if(!verifyOnly) ModelData::writeAnimation(own,path,encoded);
        diff=ModelData::compare(own,ModelData::readAnimation(path));
        }
      else if(ext==".MSB" || ext==".MDS") {
        zenkit::ModelScript source {};
        source.load(input.get());
        errors.check();
        const auto own=asset::fromZenKit(source);
        if(!verifyOnly) ModelData::writeScript(own,path);
        diff=ModelData::compare(own,ModelData::readScript(path));
        }
      else if(ext==".MMB") {
        zenkit::MorphMesh source {};
        source.load(input.get());
        errors.check();
        const auto own=asset::fromZenKit(source);
        if(!verifyOnly) ModelData::writeMorph(own,path);
        diff=ModelData::compare(own,ModelData::readMorph(path));
        }
      else {
        throw std::runtime_error("unsupported model resource extension");
        }
      require(diff.empty(),diff);
      if(count++) entries+=",";
      entries+="\n    "+gothicglb::jsonString(name)+":"+gothicglb::jsonString(file);
      if(fs::path(openedSource).is_absolute()) {
        const auto source=fs::path(openedSource).lexically_relative(fs::absolute(gothic)).generic_string();
        if(looseCount++) loose+=",";
        loose+="\n    {\"path\":"+gothicglb::jsonString(source)+",\"size\":"+std::to_string(fs::file_size(openedSource))+"}";
        }
      bytes+=fs::file_size(path);
      auto binary=path;
      binary.replace_extension(".bin");
      bytes+=fs::file_size(binary);
      }
    catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
    }
  const auto index="{\n  \"format\":\"gothic-models\",\n  \"version\":1,\n  \"archives\":"+archivesJson(archives)+
                   ",\n  \"loose_files\":"+loose+"\n  ],\n  \"files\":"+entries+"\n  }\n}\n";
  if(!verifyOnly) writeHost(out/"models.json",index.data(),index.size());
  const auto actual=parseJson(out/"models.json");
  rapidjson::Document expected;
  expected.Parse(index.c_str());
  require(actual==expected,"models.json files, archives or loose files differ");
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written and verified ") << count << " model resources, "
            << (bytes>>20) << " MiB; 0 failed (" << ms << " ms)\n";
  return 0;
  }


int convertWorld(const fs::path& gothic, const std::string& name, const fs::path& out) {
      const auto world = loadWorld(gothic,name);
      std::cout << world.source.world << ": " << world.source.size << " bytes, crc32 " << world.source.crc32 << "\n";
      const auto t0 = std::chrono::steady_clock::now();
      const auto manifest = writeWorldChunks(world.mesh,world.source,out);
      WorldData::write(world.data,out);
      addWorldArchives(out,world.archives);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
      std::cout << manifest.chunks.size() << " chunks written to " << out.string() << " (" << ms << " ms); unreferenced: "
                << manifest.unreferencedVertices << " vertices, " << manifest.unreferencedFeatures << " features\n";
      return verifyWorld(world,out,{});
  }

int convertFonts(const fs::path& gothic, const fs::path& out, bool verifyOnly) {
  const auto t0=std::chrono::steady_clock::now();
  auto root=fs::absolute(gothic);
  if(!root.has_filename()) root=root.parent_path();
  std::string openedSource;
  zenkit::Vfs vfs;
  vfs.set_open_observer([&](std::string_view, std::string_view source) { openedSource=source; });
  // Resources::implLoadFont asks the runtime VFS, so ties follow the engine's mount order,
  // not the alphabetical mountArchives of the other importers.
  fs::path work;
  std::vector<Archive> archives;
  for(const auto& a:mountRuntime(vfs,root,work))
    archives.push_back({a.name,uint64_t(a.size)});
  // The engine compares indexes with its mounted list sorted by name and size.
  std::sort(archives.begin(),archives.end(),[](const Archive& a, const Archive& b) {
    return std::tie(a.name,a.size)<std::tie(b.name,b.size);
    });
  std::vector<const zenkit::VfsNode*> files;
  collectFiles(vfs.root(),files);
  std::map<std::string,const zenkit::VfsNode*> fonts;
  for(const auto* file:files) {
    const auto name=upper(file->name());
    if(endsWith(name,".FNT")) fonts[name]=vfs.find(name);
    }
  if(!verifyOnly) {
    fs::create_directories(out);
    fs::remove(out/"fonts.json");
    }

  std::string entries="{", loose="[";
  size_t count=0, looseCount=0;
  for(const auto& [name,node]:fonts) {
    const auto file=name+".json";
    try {
      require(node!=nullptr,"Vfs::find does not resolve the font");
      openedSource.clear();
      const auto bytes=readAll(*node);
      // zenkit::Font reads past a short file without an error; check the table size first
      // with ZenKit's own line reader: header lines, height, count, then 17 bytes per glyph.
      {
      auto header=zenkit::Read::from(bytes.data(),bytes.size());
      header->read_line(true);
      header->read_line(false);
      const auto tables=header->tell();
      require(bytes.size()>=tables+8,"truncated font header");
      header->read_uint();
      const uint64_t glyphs=header->read_uint();
      const uint64_t end=uint64_t(tables)+8+glyphs*17;
      require(bytes.size()>=end,"truncated glyph tables");
      if(bytes.size()>end)
        std::cout << name << ": " << (bytes.size()-end) << " trailing bytes after the glyph tables (unused)\n";
      }
      ImportErrors errors;
      zenkit::Font source;
      auto input=zenkit::Read::from(bytes.data(),bytes.size());
      source.load(input.get());
      errors.check();
      const auto own=asset::fromZenKit(source);
      require(own.name==source.name && own.height==source.height && own.glyphs.size()==source.glyphs.size(),
              "font conversion lost the header");
      for(size_t i=0; i<own.glyphs.size(); ++i) {
        const auto& a=own.glyphs[i];
        const auto& b=source.glyphs[i];
        require(a.width==b.width && gothicglb::bits(a.uv[0].x)==gothicglb::bits(b.uv[0].x) &&
                gothicglb::bits(a.uv[0].y)==gothicglb::bits(b.uv[0].y) && gothicglb::bits(a.uv[1].x)==gothicglb::bits(b.uv[1].x) &&
                gothicglb::bits(a.uv[1].y)==gothicglb::bits(b.uv[1].y),"font conversion changed glyph "+std::to_string(i));
        }
      const auto atlas=FontData::atlasName(name);
      if(!verifyOnly) FontData::writeFont(own,atlas,out/file);
      std::string readAtlas;
      const auto diff=FontData::compare(own,FontData::readFont(out/file,&readAtlas));
      require(diff.empty(),diff);
      require(readAtlas==atlas,"font atlas differs: "+readAtlas+" != "+atlas);
      if(count++) entries+=",";
      entries+="\n    "+gothicglb::jsonString(name)+":"+gothicglb::jsonString(file);
      if(fs::path(openedSource).is_absolute()) {
        const auto path=fs::path(openedSource).lexically_relative(root).generic_string();
        if(looseCount++) loose+=",";
        loose+="\n    {\"path\":"+gothicglb::jsonString(path)+",\"size\":"+std::to_string(bytes.size())+"}";
        }
      }
    catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
    }
  const auto index="{\n  \"format\":\"gothic-fonts\",\n  \"version\":1,\n  \"archives\":"+archivesJson(archives)+
                   ",\n  \"loose_files\":"+loose+"\n  ],\n  \"fonts\":"+entries+"\n  }\n}\n";
  if(!verifyOnly) writeHost(out/"fonts.json",index.data(),index.size());
  const auto actual=parseJson(out/"fonts.json");
  rapidjson::Document expected;
  expected.Parse(index.c_str());
  require(!expected.HasParseError(),"cannot encode fonts.json");
  require(actual==expected,"fonts.json fonts, archives or loose files differ");
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
  std::cout << (verifyOnly ? "verified " : "written and verified ") << count << " fonts; 0 failed (" << ms << " ms)\n";
  return 0;
  }

int verifyWorld(const fs::path& gothic, const std::string& name, const fs::path& out, const fs::path& overrides) {
  return verifyWorld(loadWorld(gothic,name),out,overrides);
  }

void build(const fs::path& gothic, const fs::path& out, const Progress& progress) {
  const auto identity=identify(gothic);
  fs::remove(out/"base.json");
  fs::remove(out/"base-files.json");
  for(const char* world:{"NEWWORLD","OLDWORLD","ADDONWORLD","DRAGONISLAND"}) {
    fs::remove_all(out/"worlds"/world);
    if(progress) progress(std::string("base import: ")+world);
    require(convertWorld(gothic,std::string(world)+".ZEN",out/"worlds"/world)==0,"world import failed");
    }
  for(const char* kind:{"zen","textures","meshes","models","fonts","programs","outputunits","audio","music","sources"}) fs::remove_all(out/kind);
  if(progress) progress("base import: ZEN files");
  require(convertZen(gothic,out/"zen",false)==0,"ZEN import failed");
  if(progress) progress("base import: textures");
  require(convertTextures(gothic,out/"textures",false)==0,"texture import failed");
  if(progress) progress("base import: meshes");
  require(convertMeshes(gothic,out/"meshes",false)==0,"mesh import failed");
  if(progress) progress("base import: models and animations");
  require(convertModels(gothic,out/"models",false)==0,"model import failed");
  if(progress) progress("base import: fonts");
  require(convertFonts(gothic,out/"fonts",false)==0,"font import failed");
  if(progress) progress("base import: programs");
  require(convertPrograms(gothic,out/"programs",false)==0,"program import failed");
  if(progress) progress("base import: dialog line library");
  require(convertOutputUnits(gothic,out/"outputunits",false)==0,"dialog line library import failed");
  if(progress) progress("base import: sounds and speech");
  require(convertAudio(gothic,out/"audio",false)==0,"sound import failed");
  if(progress) progress("base import: music");
  require(convertMusic(gothic,out/"music",false)==0,"music import failed");
  if(progress) progress("base import: source manifest");
  require(convertSources(gothic,out/"sources",false)==0,"source manifest failed");
  require(identify(gothic).json==identity.json,"installation changed during base import");
  writeBaseMetadata(out,identity);
  }

}
