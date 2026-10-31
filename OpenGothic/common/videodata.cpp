#include "videodata.h"
#include "videoio.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <rapidjson/document.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace fs=std::filesystem;
namespace VideoData {
using namespace Detail;
namespace {
rapidjson::Document document(const fs::path& file) {
  const auto text=readText(file);
  constexpr std::string_view marker=",\"index_sha256\":\"", ending="\"}\n";
  const size_t trailer=marker.size()+64+ending.size();
  require(text.size()>=trailer,"missing index SHA-256");
  const size_t prefixSize=text.size()-trailer;
  require(text.compare(prefixSize,marker.size(),marker)==0 &&
          text.compare(text.size()-ending.size(),ending.size(),ending)==0,"invalid index SHA-256 trailer");
  require(Sha256::hash(std::string_view(text.data(),prefixSize))==text.substr(prefixSize+marker.size(),64),"index SHA-256 differs");
  rapidjson::Document doc; doc.Parse(text.data(),text.size()); require(!doc.HasParseError() && doc.IsObject(),"invalid videos.json");
  size_t checksums=0;
  for(auto member=doc.MemberBegin();member!=doc.MemberEnd();++member)
    if(std::string_view(member->name.GetString(),member->name.GetStringLength())=="index_sha256") ++checksums;
  require(checksums==1,"repeated index SHA-256");
  return doc;
  }
const rapidjson::Value& member(const rapidjson::Value& object,const char* key) { require(object.IsObject() && object.HasMember(key),std::string("missing ")+key); return object[key]; }
std::string str(const rapidjson::Value& object,const char* key) { const auto& v=member(object,key); require(v.IsString(),std::string("invalid ")+key); return {v.GetString(),v.GetStringLength()}; }
uint64_t number(const rapidjson::Value& object,const char* key) { const auto& v=member(object,key); require(v.IsUint64(),std::string("invalid ")+key); return v.GetUint64(); }
Info parseInfo(const rapidjson::Value& object) {
  Info info; info.width=uint32_t(number(object,"width")); info.height=uint32_t(number(object,"height")); info.frames=uint32_t(number(object,"frames")); info.fpsNum=uint32_t(number(object,"fps_num")); info.fpsDen=uint32_t(number(object,"fps_den"));
  const auto& planes=member(object,"planes"); require(planes.IsArray() && planes.Size()==4,"invalid planes");
  for(rapidjson::SizeType i=0;i<4;++i) { const auto& p=planes[i]; require(p.IsArray() && p.Size()==2 && p[0].IsUint() && p[1].IsUint(),"invalid plane dimensions"); info.planes[i]={p[0].GetUint(),p[1].GetUint()}; }
  const auto& tracks=member(object,"tracks"); require(tracks.IsArray(),"invalid tracks");
  for(const auto& t:tracks.GetArray()) info.tracks.push_back({uint32_t(number(t,"sample_rate")),uint32_t(number(t,"channels")),number(t,"samples")});
  return info;
  }
}

std::string formatSha256() { return VideoFormatSha256; }
Identity identifyFromBase(const fs::path& base,const std::atomic_bool* cancelled) {
  const auto text=readText(base/"sources"/"sources.json");
  rapidjson::Document doc; doc.Parse(text.data(),text.size());
  require(!doc.HasParseError() && doc.IsObject(),"invalid native source census");
  require(str(doc,"format")=="gothic-sources" && number(doc,"version")==1,"unsupported native source census");
  const auto& files=member(doc,"files"); require(files.IsArray(),"invalid native source list");
  std::map<std::string,Source> selected;
  for(const auto& file:files.GetArray()) {
    checkCancelled(cancelled);
    const auto name=str(file,"request");
    if(upper(fs::path(name).extension().string())!=".BIK") continue;
    Source source{name,str(file,"path"),number(file,"size"),str(file,"sha256")};
    require(name==upper(name) && selected.emplace(name,std::move(source)).second,"invalid or repeated native video source");
    }
  std::vector<Source> list;
  for(auto& [name,source]:selected) list.push_back(std::move(source));
  return identityOf(list);
  }
std::vector<Entry> readIndex(const fs::path& dir) {
  const auto doc=document(dir/"videos.json"); require(number(doc,"version")==1,"unsupported index version"); const auto& list=member(doc,"videos"); require(list.IsArray(),"invalid video list");
  std::vector<Entry> result; std::set<std::string> names;
  for(const auto& v:list.GetArray()) {
    Entry e; e.source={str(v,"name"),str(v,"path"),number(v,"source_size"),str(v,"source_sha256")}; e.file=str(v,"file"); e.sha256=str(v,"sha256"); e.size=number(v,"size"); e.info=parseInfo(member(v,"info"));
    require(!e.source.name.empty() && e.source.name==upper(e.source.name) && fs::path(e.source.name).filename()==e.source.name && e.file==e.source.name+".gvideo" && names.insert(e.source.name).second,"invalid or repeated video name");
    require(e.sha256.size()==64 && e.source.sha256.size()==64,"invalid SHA-256"); result.push_back(std::move(e));
    }
  return result;
  }
bool matches(const fs::path& dir,const Identity& identity,bool verifyFiles,const std::atomic_bool* cancelled) {
  try {
    const auto doc=document(dir/"videos.json"); if(str(doc,"fingerprint")!=identity.fingerprint || str(doc,"identity")!=identity.json) return false;
    const auto entries=readIndex(dir); std::vector<Source> list;
    for(const auto& e:entries) {
      checkCancelled(cancelled);
      if(fs::file_size(dir/e.file)!=e.size || (verifyFiles && fileHash(dir/e.file,cancelled)!=e.sha256)) return false;
      list.push_back(e.source);
      }
    return identityOf(list).json==identity.json;
    } catch(const std::exception&) { return false; }
  }
Reader::Reader(const fs::path& file):input(file,std::ios::binary) {
  require(bool(input),"cannot open "+file.string()); char magic[8]; readBytes(input,magic,8); require(std::memcmp(magic,"GVIDEO2\0",8)==0 && get32(input)==2,"invalid container header");
  payloadStart=get32(input); fileSize=get64(input); require(fs::file_size(file)==fileSize && payloadStart<=fileSize,"container size differs");
  char expectedHash[64]; readBytes(input,expectedHash,64); payloadSha256.assign(expectedHash,64); meta.width=get32(input); meta.height=get32(input); meta.frames=get32(input); meta.fpsNum=get32(input); meta.fpsDen=get32(input); const auto tracks=get32(input); char reserved[16]; readBytes(input,reserved,16);
  require(meta.width>0 && meta.height>0 && meta.width<65536 && meta.height<65536 && meta.frames>0 && meta.fpsNum>0 && meta.fpsDen>0 && tracks<=255,"invalid video metadata");
  require(uint64_t(payloadStart)==HeaderSize+32+16*uint64_t(tracks)+84*uint64_t(meta.frames),"invalid frame index size");
  for(size_t p=0;p<4;++p) { auto& plane=meta.planes[p]; plane={get32(input),get32(input)}; const bool chroma=p==1 || p==2; require(plane.stride==aligned(chroma ? meta.width/2 : meta.width) && plane.rows==aligned(chroma ? meta.height/2 : meta.height),"invalid plane dimensions"); }
  for(uint32_t t=0;t<tracks;++t) { Track track{get32(input),get32(input),get64(input)}; require(track.sampleRate>0 && (track.channels==1 || track.channels==2),"invalid audio metadata"); meta.tracks.push_back(track); }
  uint64_t end=payloadStart;
  for(uint32_t f=0;f<meta.frames;++f) {
    Position p{get64(input),get64(input),get32(input),{}}; char hash[64]; readBytes(input,hash,64); p.sha256.assign(hash,64);
    require((p.flags&~15u)==0 && (f%KeyframeInterval!=0 || p.flags==0),"invalid plane predictor flags");
    require(p.offset==end && p.size<=fileSize-p.offset,"invalid frame offset"); end+=p.size; index.push_back(p);
    }
  require(end==fileSize,"unindexed payload bytes");
  }
Frame Reader::readFrame(uint32_t number) {
  require(number<meta.frames,"frame number out of range");
  std::array<uint32_t,4> first; first.fill(number);
  if(!havePrevious || number!=next)
    for(size_t p=0;p<4;++p)
      while(index[first[p]].flags&(1u<<p)) --first[p];
  havePrevious=false;
  Frame ret; ret.audio.resize(meta.tracks.size());
  for(uint32_t frame=*std::min_element(first.begin(),first.end());frame<=number;++frame) {
    const auto& position=index[frame]; input.clear(); input.seekg(std::streamoff(position.offset));
    std::string encoded(size_t(position.size),'\0'); readBytes(input,encoded.data(),encoded.size());
    require(frameHash(position.flags,encoded)==position.sha256,"frame payload SHA-256 differs");
    std::istringstream frameInput(encoded,std::ios::binary|std::ios::in); uint64_t remaining=position.size;
    for(size_t p=0;p<4;++p) {
      require(remaining>=4,"truncated plane size"); const uint32_t size=get32(frameInput); remaining-=4; require(size<=remaining,"truncated plane"); remaining-=size;
      if(frame<first[p]) { frameInput.seekg(size,std::ios::cur); continue; }
      std::vector<uint8_t> compressed(size); readBytes(frameInput,compressed.data(),size); const auto& pi=meta.planes[p];
      std::vector<uint8_t> plane(size_t(pi.stride)*pi.rows); mz_ulong decoded=mz_ulong(plane.size());
      require(mz_uncompress(plane.data(),&decoded,compressed.data(),size)==MZ_OK && decoded==plane.size(),"invalid deflate plane");
      if(position.flags&(1u<<p)) {
        require(previousPlanes[p].size()==plane.size(),"missing temporal reference plane");
        for(size_t i=0;i<plane.size();++i) plane[i]=uint8_t(plane[i]+previousPlanes[p][i]);
        } else {
        for(size_t i=pi.stride;i<plane.size();++i) plane[i]=uint8_t(plane[i]+plane[i-pi.stride]);
        }
      previousPlanes[p]=std::move(plane);
      }
    for(auto& track:ret.audio) {
      require(remaining>=4,"truncated PCM size"); const uint32_t samples=get32(frameInput); remaining-=4; require(uint64_t(samples)*2<=remaining,"truncated PCM"); remaining-=uint64_t(samples)*2;
      if(frame!=number) { frameInput.seekg(std::streamoff(uint64_t(samples)*2),std::ios::cur); continue; }
      std::vector<uint8_t> pcmBytes(size_t(samples)*2); readBytes(frameInput,pcmBytes.data(),pcmBytes.size()); track.resize(samples);
      for(size_t i=0;i<samples;++i) track[i]=int16_t(uint16_t(pcmBytes[2*i])|(uint16_t(pcmBytes[2*i+1])<<8));
      }
    require(remaining==0,"trailing frame bytes");
    }
  ret.planes=previousPlanes; next=number+1; havePrevious=true; return ret;
  }
Frame Reader::nextFrame() { return readFrame(next); }
void Reader::verifyPayload() { input.clear(); input.seekg(std::streamoff(payloadStart)); require(hashRange(input,fileSize-payloadStart)==payloadSha256,"payload SHA-256 differs"); }
}
