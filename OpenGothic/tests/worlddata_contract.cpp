#include "asset/fromzenkit.h"
#include <zenkit/World.hh>
#include <zenkit/Vfs.hh>
#include "commandline.h"
#include "gothic.h"
#include "resources.h"
#include "sha256.h"
#include "worldbase.h"
#include "worldchunksruntime.h"
#include "world/world.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "game/gamesession.h"
#include "gamemusic.h"
#include "graphics/mesh/protomesh.h"
#include "utils/workers.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace fs = std::filesystem;

struct WorldDataContract {
  static zenkit::Vfs& sourceVfs() {
    static zenkit::Vfs vfs;
    static const bool mounted=[] {
      fs::path work;
      BaseImport::mountRuntime(vfs,fs::path(CommandLine::inst().rootPath()),work);
      return true;
      }();
    (void)mounted;
    return vfs;
    }

  using Rows = std::vector<std::pair<std::string,std::string>>;
  static void require(bool ok, const std::string& message) {
    if(!ok) throw std::runtime_error(message);
    }
  template<class T> static void field(Rows& rows, const std::string& key, const T& value) {
    std::ostringstream text;
    text << std::setprecision(9) << value;
    rows.emplace_back(key,text.str());
    }
  static void vector(Rows& rows, const std::string& key, const Tempest::Vec3& v) {
    field(rows,key+".x",v.x); field(rows,key+".y",v.y); field(rows,key+".z",v.z);
    }
  static void compare(const Rows& a, const Rows& b, const std::string& world) {
    require(a.size()==b.size(),world+": runtime field count "+std::to_string(a.size())+" vs "+std::to_string(b.size()));
    for(size_t i=0;i<a.size();++i)
      require(a[i]==b[i],world+": "+a[i].first+" ZEN="+a[i].second+" base="+b[i].second+" ("+b[i].first+")");
    }
  static void vob(Rows& rows, const Vob& v, const std::string& key, std::map<int,size_t>& counts) {
    ++counts[int(v.vobType)];
    field(rows,key+".class",typeid(v).name()); field(rows,key+".type",int(v.vobType));
    field(rows,key+".id",v.vobObjectID);
    Vob::AttachmentSnapshot attachment;
    v.fillAttachmentSnapshot(attachment);
    field(rows,key+".visual",attachment.visual);
    field(rows,key+".parent",attachment.parentZenId);
    for(size_t i=0;i<16;++i) {
      field(rows,key+".transform["+std::to_string(i)+"]",v.transform().data()[i]);
      field(rows,key+".local["+std::to_string(i)+"]",v.localTransform().data()[i]);
      }
    if(const auto* mob=dynamic_cast<const Interactive*>(&v)) {
      field(rows,key+".name",mob->vobNameStr()); field(rows,key+".state",mob->stateId());
      const auto* mesh=mob->objVisual().protoMesh();
      field(rows,key+".mesh",mesh ? mesh->fname : "");
      }
    if(const auto* trigger=dynamic_cast<const AbstractTrigger*>(&v))
      field(rows,key+".name",trigger->name());
    field(rows,key+".children",v.child.size());
    for(size_t i=0;i<v.child.size();++i)
      vob(rows,*v.child[i],key+"/"+std::to_string(i),counts);
    }
  static Rows snapshot(World& world, const WorldData::Data& original) {
    Rows rows;
    auto& obj=world.wobj;
    field(rows,"roots",obj.rootVobs.size());
    field(rows,"statics",obj.objStatic.size());
    field(rows,"interactives",obj.mobsiCount());
    field(rows,"items",obj.itmCount()); field(rows,"npcs",obj.npcCount());
    field(rows,"triggers",obj.triggers.size()); field(rows,"collisionZones",obj.collisionZn.size());
    std::map<int,size_t> counts;
    for(size_t i=0;i<obj.rootVobs.size();++i)
      vob(rows,*obj.rootVobs[i],"vob["+std::to_string(i)+"]",counts);
    for(auto [type,count]:counts) field(rows,"count[type="+std::to_string(type)+"]",count);
    for(size_t i=0;i<obj.itmCount();++i) {
      const auto& item=obj.itm(i);
      field(rows,"item["+std::to_string(i)+"].class",item.clsId());
      vector(rows,"item["+std::to_string(i)+"].position",item.position());
      }
    auto& matrix=*world.wmatrix;
    std::map<const WayPoint*,std::string> indices;
    const auto points=[&](const auto& list,const std::string& kind) {
      field(rows,kind+".size",list.size());
      for(size_t i=0;i<list.size();++i) {
        const auto& p=list[i]; const auto key=kind+"["+std::to_string(i)+"]";
        indices.emplace(&p,key);
        field(rows,key+".name",p.name); field(rows,key+".underwater",p.underWater);
        field(rows,key+".free",p.freePoint);
        vector(rows,key+".pos",p.pos); vector(rows,key+".ground",p.groundPos); vector(rows,key+".dir",p.dir);
        }
      };
    points(matrix.wayPoints,"waypoint"); points(matrix.freePoints,"freepoint"); points(matrix.startPoints,"startpoint");
    for(size_t i=0;i<matrix.edges.size();++i) {
      field(rows,"edge["+std::to_string(i)+"].a",matrix.edges[i].a);
      field(rows,"edge["+std::to_string(i)+"].b",matrix.edges[i].b);
      }
    for(size_t i=0;i<matrix.indexPoints.size();++i)
      field(rows,"index["+std::to_string(i)+"]",indices.at(matrix.indexPoints[i]));
    for(const auto& p:matrix.wayPoints)
      for(size_t i=0;i<p.connections().size();++i) {
        const auto key=indices.at(&p)+".connection["+std::to_string(i)+"]";
        field(rows,key+".to",indices.at(p.connections()[i].point));
        field(rows,key+".length",p.connections()[i].len);
        }
    auto bsp=original;
    bsp.bsp.nodes=world.bsp.nodes; bsp.bsp.sectors=world.bsp.sectors;
    bsp.bsp.leaf_node_indices=world.bsp.leaf_node_indices;
    const auto diff=WorldData::compare(original,bsp);
    require(diff.empty(),std::string(world.name())+": runtime BSP: "+diff);
    const auto bounds=original.bsp.nodes.empty() ? original.bbox : original.bsp.nodes.front().bbox;
    const auto nx=size_t(std::ceil(double(bounds.max.x-bounds.min.x)/400.));
    const auto nz=size_t(std::ceil(double(bounds.max.z-bounds.min.z)/400.));
    size_t samples=0,indoor=0;
    for(size_t x=0;x<=nx;++x) for(size_t z=0;z<=nz;++z) for(unsigned h=0;h<3;++h) {
      const zenkit::Vec3 p{bounds.min.x+(bounds.max.x-bounds.min.x)*float(double(x)/double(std::max(size_t(1),nx))),
                           bounds.min.y+(bounds.max.y-bounds.min.y)*float(h+1)/4.f,
                           bounds.min.z+(bounds.max.z-bounds.min.z)*float(double(z)/double(std::max(size_t(1),nz)))};
      const bool expected=WorldData::isIndoor(original.bsp.mode,original.rooms,p);
      const bool actual=world.isIndoor({p.x,p.y,p.z});
      require(expected==actual,std::string(world.name())+": isIndoor at "+std::to_string(p.x)+","+
              std::to_string(p.y)+","+std::to_string(p.z)+" ZEN="+std::to_string(expected)+" base="+std::to_string(actual));
      ++samples; indoor+=actual;
      }
    const auto key=NavMesh::cacheKey(world.physic()->navigationGeometry(),NavMesh::Params{},std::string(world.name()));
    field(rows,"navmesh.key",key);
    std::printf("%s: runtime %zu roots, %zu static, %zu interactive, %zu items, %zu NPCs, %zu types; %zu points, %zu edges; %zu indoor samples (%zu inside); nav key %llu\n",
                std::string(world.name()).c_str(),obj.rootVobs.size(),obj.objStatic.size(),obj.mobsiCount(),obj.itmCount(),obj.npcCount(),counts.size(),
                matrix.wayPoints.size(),matrix.edges.size(),samples,indoor,static_cast<unsigned long long>(key));
    return rows;
    }
  static rapidjson::Document json(const fs::path& path) {
    std::ifstream in(path); require(bool(in),"cannot read "+path.string());
    const std::string text{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    rapidjson::Document doc; doc.Parse(text.c_str()); require(!doc.HasParseError(),"invalid "+path.string()); return doc;
    }
  static uint64_t zenOpens(const fs::path& census) {
    require(Resources::writeCensus(),"cannot write census");
    const auto doc=json(census);
    uint64_t count=0;
    for(const auto& file:doc["files"].GetArray())
      if(std::string_view(file["name"].GetString()).ends_with(".ZEN"))
        count+=file["opens"].GetUint64();
    return count;
    }
  static void runtimeSources(const fs::path& census, const fs::path& gothicDir, Resources& resources, Gothic& gothic) {
    // The existing headless session constructs the six definition programs and
    // the game program. Cover the menu input without starting another VM/world.
    const auto menu=gothic.loadScript("MENU.DAT",ScriptLang::NONE);
    require(!menu.symbols().empty(),"native menu program must contain symbols");
    require(Resources::writeCensus(),"cannot write runtime source census");
    const auto doc=json(census);
    const auto& profile=doc["active_profile"];
    const auto language=gothic.settingsGetI("GAME","language");
    require(std::string_view(profile["game"].GetString())==gothic.defaultGameDatFile() &&
            std::string_view(profile["output_units"].GetString())==gothic.defaultOutputUnits() &&
            profile["language"].GetInt()==language,"runtime census active profile differs from Gothic");
    const auto& sources=doc["runtime_sources"];
    require(std::string_view(sources["coverage"].GetString())=="observed_successful_parses" &&
            !sources["complete_corpus"].GetBool(),"runtime census must distinguish observed parses from the declared corpus");
    const auto& programs=doc["runtime_programs"];
    require(std::string_view(programs["coverage"].GetString())=="loaded_base_records" &&
            !programs["complete_corpus"].GetBool(),"runtime native programs must not claim original parses or full corpus coverage");
    bool game=false;
    std::set<std::string> loaded;
    for(const auto& row:programs["selections"].GetArray()) {
      const std::string request=row["request"].GetString(),branch=row["branch"].GetString();
      require(std::string_view(row["consumer"].GetString())=="Gothic::loadScript",request+": native program consumer");
      const bool gameRequest=request==gothic.defaultGameDatFile();
      game|=gameRequest;
      loaded.insert(request);
      require(row["language"].GetInt()==(gameRequest ? language : int(ScriptLang::NONE)),
              request+": native census must retain the language argument");
      const auto& imported=row["imported_source"];
      const ProgramData::Source* selected=nullptr;
      if(branch=="vfs_find_request" || branch=="vfs_find_basename") {
        std::string_view selectedBranch;
        selected=resources.basePrograms.findVfs(request,&selectedBranch);
        require(selectedBranch==branch,request+": native VFS branch differs from the index");
        const auto lookup=branch=="vfs_find_basename" ? fs::path(request).filename().string() : request;
        const auto* node=sourceVfs().resolve(imported["path"].GetString());
        require(node!=nullptr && node==sourceVfs().find(lookup),
                request+": native provenance must retain the full selected VFS path");
        }
      else {
        require(branch=="host_ordinary" || branch=="host_localized_full" || branch=="host_localized_basename",
                request+": unexpected native host branch");
        selected=resources.basePrograms.findHost(imported["path"].GetString());
        }
      require(selected!=nullptr && selected->origin==imported["origin"].GetString() &&
              selected->path==imported["path"].GetString() && selected->size==imported["size"].GetUint64() &&
              selected->sha256==imported["sha256"].GetString(),request+": imported program identity differs from the index");
      require(selected->archive.empty() ? imported["archive"].IsNull() :
              imported["archive"].IsString() && selected->archive==imported["archive"].GetString(),
              request+": native provenance archive differs from the index");
      const auto file=resources.basePrograms.path(*selected);
      auto bin=file; bin.replace_extension(".bin");
      const auto& record=row["record"];
      require(std::string(record["json"].GetString())=="base/programs/"+file.filename().string() &&
              std::string(record["bin"].GetString())=="base/programs/"+bin.filename().string(),
              request+": native census must identify the loaded JSON and BIN");
      const auto read=[](const fs::path& path) {
        std::ifstream input(path,std::ios::binary);
        require(bool(input),"cannot read native program oracle: "+path.string());
        return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        };
      const auto digest=Sha256::hash(read(file)+read(bin));
      require(digest==record["sha256"].GetString() && digest==selected->recordSha256,
              request+": native census SHA-256 differs from actual JSON+BIN bytes");
      }
    require(game,"existing GameSession construction must load the actual game program");
    for(const auto* name:{"Camera.dat","Fight.dat","Sfx.dat","Music.dat","ParticleFx.dat","VisualFx.dat","MENU.DAT"})
      require(loaded.count(name)==1,std::string(name)+": all eight inputs must load native programs");
    require(sources["selections"].Empty(),"native programs and OU must not report original source parses");
    const auto basename=[](const char* path) {
      auto name=fs::path(path).filename().string();
      for(auto& c:name) if(c>='a' && c<='z') c=char(c-'a'+'A');
      return name;
      };
    const auto binName=std::string(gothic.defaultOutputUnits())+".BIN";
    const auto datName=std::string(gothic.defaultOutputUnits())+".DAT";
    for(const auto& row:doc["files"].GetArray()) {
      const std::string_view kind=row["kind"].GetString(),name=row["name"].GetString();
      require(kind!="script","native runtime must add zero original DAT opens");
      require(kind!="cutscene" && name!=basename(binName.c_str()) && name!=basename(datName.c_str()),
              "native runtime must add zero original OU opens");
      }
    const auto& outputUnits=doc["runtime_outputunits"];
    require(std::string_view(outputUnits["coverage"].GetString())=="loaded_base_records" &&
            !outputUnits["complete_corpus"].GetBool(),"runtime native OU must distinguish base reads from full corpus coverage");
    require(!outputUnits["selections"].Empty(),"existing GameSession construction must load native OU");
    const auto relative=[](const fs::path& path) {
      if(path.empty() || path.is_absolute()) return false;
      for(const auto& part:path) if(part=="..") return false;
      return true;
      };
    const auto checkBytes=[](zenkit::Read& reader, const OutputUnits::Source& selected) {
      reader.seek(0,zenkit::Whence::END);
      std::string bytes(reader.tell(),'\0');
      reader.seek(0,zenkit::Whence::BEG);
      require(reader.read(bytes.data(),bytes.size())==bytes.size(),"incomplete imported OU oracle read");
      require(selected.size==bytes.size() && selected.sha256==Sha256::hash(bytes),
              selected.path+": imported OU provenance differs from independent source bytes");
      };
    std::vector<BaseImport::RuntimeArchive> archives;
    std::vector<std::shared_ptr<asset::CutsceneBlock>> blocks;
    for(const auto& row:outputUnits["selections"].GetArray()) {
      const std::string request=row["request"].GetString(),branch=row["branch"].GetString();
      require(std::string_view(row["consumer"].GetString())=="GameScript::loadDialogOU" &&
              (request==binName || request==datName) && row["language"].GetInt()==language,
              request+": native OU consumer/request/language differs");
      const auto& imported=row["imported_source"];
      const fs::path path=imported["path"].GetString();
      require(relative(path),request+": imported OU path must remain installation-relative");
      const OutputUnits::Source* selected=nullptr;
      if(branch=="vfs_find_request" || branch=="vfs_find_basename") {
        std::string_view selectedBranch;
        selected=resources.baseOutputUnits.findVfs(request,&selectedBranch);
        require(selectedBranch==branch,request+": native OU VFS branch differs from the index");
        const auto& vfs=sourceVfs();
        const auto* node=vfs.resolve(path.generic_string());
        const auto lookup=branch=="vfs_find_basename" ? fs::path(request).filename().string() : request;
        require(node!=nullptr && node==vfs.find(lookup),request+": native OU provenance must retain the selected VFS path");
        }
      else {
        require(branch=="host_ordinary" || branch=="host_localized",request+": unexpected native OU host branch");
        selected=resources.baseOutputUnits.findHost(path.generic_string());
        }
      require(selected!=nullptr && selected->skipped.empty() &&
              selected->origin==imported["origin"].GetString() && selected->path==path.generic_string() &&
              selected->size==imported["size"].GetUint64() && selected->sha256==imported["sha256"].GetString(),
              request+": imported OU identity differs from the index");
      require(selected->archive.empty() ? imported["archive"].IsNull() :
              imported["archive"].IsString() && selected->archive==imported["archive"].GetString(),
              request+": imported OU archive differs from the index");
      const auto file=resources.baseOutputUnits.path(*selected);
      std::ifstream native(file,std::ios::binary);
      require(bool(native),request+": cannot read native OU oracle");
      const std::string nativeBytes{std::istreambuf_iterator<char>(native),std::istreambuf_iterator<char>()};
      const auto digest=Sha256::hash(nativeBytes);
      require(std::string(row["record"]["json"].GetString())=="base/outputunits/"+file.filename().string() &&
              digest==row["record"]["sha256"].GetString() && digest==selected->recordSha256 &&
              resources.baseOutputUnits.records.at(selected->record)==digest,
              request+": native OU identity differs from actual JSON bytes");
      blocks=OutputUnits::sortedBlocks(OutputUnits::readLibrary(file));
      // Original reads below are independent test oracles, after zero runtime opens were checked.
      if(selected->origin=="host") {
        auto reader=zenkit::Read::from(gothicDir/path);
        require(reader!=nullptr,request+": imported OU host source is missing");
        checkBytes(*reader,*selected);
        }
      else {
        if(archives.empty()) archives=BaseImport::runtimeArchives(gothicDir);
        const auto found=std::find_if(archives.begin(),archives.end(),[&](const auto& archive) {
          return archive.name==selected->archive;
          });
        require(found!=archives.end(),request+": imported OU archive is not an installation source");
        zenkit::Vfs archive;
        archive.mount_disk(found->path);
        const auto* node=archive.resolve(path.generic_string());
        require(node!=nullptr,request+": imported OU path is absent from its archive");
        auto reader=node->open_read();
        checkBytes(*reader,*selected);
        }
      }
    messageBehavior(gothic,blocks);
    std::puts("Runtime census: active profile, eight native programs and native OU identities verified; zero Resources DAT/OU opens");
    }
  static void messageBehavior(Gothic& gothic, const std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks) {
    const auto named=std::find_if(blocks.begin(),blocks.end(),[&](const auto& block) {
      const auto selected=OutputUnits::blockByName(blocks,block->name);
      const auto message=OutputUnits::message(*selected);
      return !block->name.empty() && message!=nullptr && !message->text.empty();
      });
    require(named!=blocks.end(),"message behavior needs a named native text line");
    const auto message=OutputUnits::message(*OutputUnits::blockByName(blocks,(*named)->name));
    const auto first=OutputUnits::message(*blocks.front());
    auto* session=gothic.gameSession();
    require(session!=nullptr && session->script()!=nullptr,"message behavior needs the existing GameSession");
    auto& script=*session->script();
    auto oldMessage=std::move(gothic.externalMessageFn);
    auto oldSound=std::move(gothic.externalSoundFn);
    require(script.messageByName((*named)->name)==message->text,"messageByName must retain raw native OU bytes");
    require(script.messageByName("")==(first!=nullptr ? std::string_view(first->text) : std::string_view{}),
            "messageByName must retain lower_bound's first result");
    // Fixed mono PCM16 at 1000 Hz; zero frames selects the existing text-duration fallback.
    const auto pcm=[](uint32_t frames) {
      static constexpr std::array<uint8_t,44> header{
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
        0xE8,0x03,0,0,0xD0,0x07,0,0,2,0,16,0,'d','a','t','a',0,0,0,0};
      std::vector<uint8_t> bytes(header.size()+2*size_t(frames));
      std::copy(header.begin(),header.end(),bytes.begin());
      for(size_t i=0;i<4;++i) {
        bytes[4+i]=uint8_t((36+2*frames)>>(8*i));
        bytes[40+i]=uint8_t((2*frames)>>(8*i));
        }
      return bytes;
      };
    size_t soundCalls=0;
    std::string soundRequest;
    gothic.externalSoundFn=[&](std::string_view request,std::vector<uint8_t>& bytes) {
      ++soundCalls; soundRequest=request; bytes=pcm(1000); return true;
      };
    require(script.messageTime("")==1000 && soundCalls==1 && soundRequest==".WAV",
            "messageTime must load requested ID.WAV, independently of the OU audio name");
    const std::string externalText="External OU text";
    gothic.externalMessageFn=[&](std::string_view,std::string_view& text,uint32_t& duration) {
      text=externalText; duration=4321; return true;
      };
    require(script.messageByName("")==externalText && script.messageTime("")==4321 && soundCalls==1,
            "external message text and duration must precede native OU, cached duration and audio loading");
    gothic.externalMessageFn={};
    gothic.externalSoundFn=[&](std::string_view request,std::vector<uint8_t>& bytes) {
      ++soundCalls; soundRequest=request; bytes=pcm(0); return true;
      };
    const auto* time=script.getVm().find_symbol_by_name("VIEW_TIME_PER_CHAR");
    float perChar=time!=nullptr ? time->get_float() : 550.f;
    if(perChar<=0.f) perChar=550.f;
    const auto expected=std::min(uint32_t(float(message->text.length())*perChar),16000u);
    require(script.messageTime((*named)->name)==expected && soundCalls==2 &&
            soundRequest==(*named)->name+".WAV","messageTime must retain capped raw-byte text duration fallback");
    gothic.externalMessageFn=std::move(oldMessage);
    gothic.externalSoundFn=std::move(oldSound);
    std::puts("OU messages: native byte text, external text/duration priority, requested WAV id and text fallback verified");
    }
  static void write(const fs::path& path, const rapidjson::Document& doc) {
    rapidjson::StringBuffer text; rapidjson::Writer<rapidjson::StringBuffer> writer(text); doc.Accept(writer);
    std::ofstream out(path); out << text.GetString(); require(bool(out),"cannot write "+path.string());
    }
  static void run(const fs::path& root, const fs::path& gothicDir) {
    const auto base=root/"base";
    const std::array<std::string,4> worlds={"NEWWORLD","OLDWORLD","ADDONWORLD","DRAGONISLAND"};
    const auto* supplied=std::getenv("OPENGOTHIC_BASE");
    require(supplied && *supplied,"OPENGOTHIC_BASE must point to build-base.sh output");
    fs::copy(supplied,base,fs::copy_options::recursive);
    const auto reference=root/"archive-reference";
    fs::create_directory(reference);
    for(const char* part:{"base.json","textures","meshes","zen","sources"})
      fs::create_symlink(base/part,reference/part);
    setenv("OPENGOTHIC_CONTENT","",1); setenv("OPENGOTHIC_TEXTURES","",1);
    setenv("OPENGOTHIC_FRACTURE","",1); setenv("OPENGOTHIC_PHYSICS_THREADS","0",1);
    Workers::setSerialExecution(true);
    for(const auto& name:worlds) {
      const auto file=name+".ZEN";
      WorldData::Data original;
      Rows expected;
      for(const bool useBase:{false,true}) {
        setenv("OPENGOTHIC_BASE",base.c_str(),1);
        const auto census=root/(name+(useBase ? "-base.json" : "-zen.json"));
        setenv("OPENGOTHIC_CENSUS",census.c_str(),1);
        Resources resources;
        Gothic gothic;
        gothic.isNpcInDialogFn=[](const Npc*) { return false; };
        gothic.setupGlobalScripts();
        GameMusic music;
        WorldData::Data loaded;
        asset::WorldMesh loadedMesh;
        const auto manifest=readWorldChunkManifest(base/"worlds"/name);
        if(!useBase) {
          auto reader=sourceVfs().find(file)->open_read();
          zenkit::World archive; archive.load(reader.get(),zenkit::GameVersion::GOTHIC_2);
          original=WorldData::fromWorld(archive,manifest.source);
          loadedMesh=asset::fromZenKit(archive.world_mesh);
          // Feed the same runtime consumers from an independently decoded archive
          // fixture. There is no archive selection mode in the engine any more.
          const auto dir=reference/"worlds"/name;
          writeWorldChunks(loadedMesh,manifest.source,dir);
          WorldData::write(original,dir);
          auto index=json(dir/"world.json");
          const auto sourceIndex=json(base/"worlds"/name/"world.json");
          rapidjson::Value archives;
          archives.CopyFrom(sourceIndex["archives"],index.GetAllocator());
          index.AddMember("archives",archives,index.GetAllocator());
          write(dir/"world.json",index);
          resources.automaticBase.path=reference;
          } else {
          Resources::loadWorld(loaded,loadedMesh,file);
          require(zenOpens(census)==0,file+": base loader opened ZEN");
          auto restored=WorldData::read(base/"worlds"/name);
          const auto diff=WorldData::compare(original,restored);
          require(diff.empty(),file+": "+diff);
          auto reader=sourceVfs().find(file)->open_read(); zenkit::World zen;
          zen.load(reader.get(),zenkit::GameVersion::GOTHIC_2);
          const auto meshDiff=compareWorldMeshes(canonicalWorldMesh(asset::fromZenKit(zen.world_mesh)),canonicalWorldMesh(loadedMesh));
          require(meshDiff.empty(),file+": mesh "+meshDiff);
          }
        const auto before=zenOpens(census);
        gothic.setGame(std::make_unique<GameSession>(file));
        if(useBase)
          require(zenOpens(census)==before,file+": base GameSession opened ZEN");
        if(name==worlds.front() && useBase)
          runtimeSources(census,gothicDir,resources,gothic);
        const auto actual=snapshot(*gothic.world(),original);
        if(useBase) compare(expected,actual,file); else expected=actual;
        gothic.clearGame().reset();
        }
      std::printf("%s: complete headless World / Vob::load / WorldObjects / WayMatrix equal (%zu fields); 0 engine ZEN opens with base\n",file.c_str(),expected.size());
      }
    // The actual selection function must reject a mismatched archive size,
    // and accept complete vob/waynet files from content ahead of the base.
    setenv("OPENGOTHIC_BASE",base.c_str(),1);
    setenv("OPENGOTHIC_CENSUS","",1);
    {
    Resources resources; Gothic gothic;
    const auto dir=base/"worlds/NEWWORLD";
    auto index=json(dir/"world.json");
    const auto size=index["archives"][0]["size"].GetUint64();
    index["archives"][0]["size"].SetUint64(size+1); write(dir/"world.json",index);
    WorldData::Data fallback;
    asset::WorldMesh fallbackMesh;
    bool rejected=false;
    try { Resources::loadWorld(fallback,fallbackMesh,"NEWWORLD.ZEN"); }
    catch(const BaseStore::Error&) { rejected=true; }
    require(rejected,"archive size +1 must reject the world");
    index["archives"][0]["size"].SetUint64(size); write(dir/"world.json",index);
    Resources::loadWorld(fallback,fallbackMesh,"NEWWORLD.ZEN");
    const auto content=root/"content"; const auto overrides=content/"worlds/NEWWORLD";
    fs::create_directories(overrides);
    auto vobs=json(dir/"vobs.json");
    vobs["vobs"].Clear(); write(overrides/"vobs.json",vobs);
    auto waynet=json(dir/"waynet.json");
    waynet["waynet"]["edges"].Clear(); write(overrides/"waynet.json",waynet);
    setenv("OPENGOTHIC_CONTENT",content.c_str(),1);
    WorldData::Data changed;
    asset::WorldMesh changedMesh;
    Resources::loadWorld(changed,changedMesh,"NEWWORLD.ZEN");
    require(changed.vobs.empty() && !fallback.vobs.empty(),"content vobs.json must replace the whole base tree");
    require(changed.waynet->edges.empty() && !fallback.waynet->edges.empty(),"content waynet.json must replace the whole base graph");
    const auto zenIndex=json(base/"zen/zen.json");
    size_t zenCount=0;
    for(const auto& entry:zenIndex["files"].GetObject()) {
      const auto name=std::string(entry.name.GetString());
      auto source=sourceVfs().find(name)->open_read();
      source->seek(0,zenkit::Whence::END);
      std::vector<std::byte> bytes(source->tell()); source->seek(0,zenkit::Whence::BEG);
      require(source->read(bytes.data(),bytes.size())==bytes.size(),"incomplete archive ZEN");
      WorldData::ZenFile zen,converted;
      require(WorldData::loadZen(bytes,{name,bytes.size(),worldChunkCrc32(bytes.data(),bytes.size())},zen),"meshless ZEN expected");
      require(Resources::loadBaseZen(name,zen.kind,converted),"base ZEN selection failed: "+name);
      const auto diff=WorldData::compareZen(zen,converted); require(diff.empty(),name+": "+diff); ++zenCount;
      }
    std::printf("Selection: archive size %llu -> %llu rejected; content vobs/waynet win; %zu base ZEN files equal\n",
                static_cast<unsigned long long>(size),static_cast<unsigned long long>(size+1),zenCount);
    }
    // A stale loose-file identity disables the ZEN index just like stale archives.
    auto zenIndex=json(base/"zen/zen.json");
    require(zenIndex["loose_files"].Size()>0,"installation must include loose ZEN presets");
    auto& loose=zenIndex["loose_files"][0];
    const auto size=loose["size"].GetUint64();
    loose["size"].SetUint64(size+1); write(base/"zen/zen.json",zenIndex);
    const auto census=root/"loose-mismatch.json";
    setenv("OPENGOTHIC_CENSUS",census.c_str(),1);
    bool rejected=false;
    try { Resources resources; Gothic gothic; }
    catch(const BaseStore::Error&) { rejected=true; }
    require(rejected,"loose size +1 must reject the base");
    std::printf("Selection: loose file %s size %llu -> %llu rejected\n",loose["path"].GetString(),
                static_cast<unsigned long long>(size),static_cast<unsigned long long>(size+1));
    }
  };

int main() {
  const auto root=fs::current_path()/("worlddata-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  int result=0;
  try {
    const auto* gothic=std::getenv("GOTHIC_DIR");
    WorldDataContract::require(gothic && *gothic,"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[]={"gothic-worlddata-contract","-g",gothic,"-g2"};
    CommandLine commandLine(4,args);
    fs::create_directories(root);
    WorldDataContract::run(root,gothic);
    std::puts("World data contract passed");
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"World data contract: %s\n",error.what()); result=1;
    }
  std::error_code error; fs::remove_all(root,error);
  return result;
  }
