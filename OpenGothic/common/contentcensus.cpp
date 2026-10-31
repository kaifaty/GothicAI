#include "contentcensus.h"
#include "sha256.h"
#include "programdata.h"
#include "outputunits.h"
#include "audiodata.h"
#include "musicdata.h"

#include <Tempest/Log>
#include <Tempest/TextCodec>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <zenkit/Stream.hh>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace {
std::string upper(std::string_view text) {
  std::string ret(text);
  for(auto& c:ret) {
    if(c>='a' && c<='z')
      c = char(c-'a'+'A');
    if(c=='\\')
      c = '/';
    }
  return ret;
  }

std::string basename(std::string_view name) {
  const auto pos = name.find_last_of("/\\");
  return upper(pos==std::string_view::npos ? name : name.substr(pos+1));
  }

// Keep the path within _work, without leaking the installation's host prefix.
std::string sourcePath(std::string_view source) {
  auto key = upper(source);
  auto pos = key.find("/_WORK/");
  if(pos!=std::string::npos)
    source.remove_prefix(pos+1);
  std::string ret(source);
  std::replace(ret.begin(),ret.end(),'\\','/');
  return ret;
  }

const char* kind(std::string_view name, std::string_view source) {
  const auto pos = name.find_last_of('.');
  const auto ext = pos==std::string_view::npos ? std::string_view{} : name.substr(pos);
  const auto src = upper(source);
  if(src.find("/MUSIC/")!=std::string::npos || ext==".SGT" || ext==".STY" || ext==".DLS")
    return "music";
  if(ext==".TGA" || ext==".TEX" || ext==".DDS" || ext==".PNG" || ext==".BMP" || ext==".JPG")
    return "texture";
  if(ext==".MRM" || ext==".MSH" || ext==".3DS")
    return "mesh";
  if(ext==".MDM" || ext==".MDL" || ext==".MDS" || ext==".MSB" || ext==".ASC")
    return "model";
  if(ext==".MDH")
    return "skeleton";
  if(ext==".MAN")
    return "anim";
  if(ext==".MMB" || ext==".MMS")
    return "morph";
  if(ext==".ZEN")
    return "world";
  if(ext==".WAV" || ext==".OGG" || ext==".MP3" || ext==".FLAC")
    return src.find("SPEECH")!=std::string::npos ? "speech" : "sound";
  if(src.find("/CUTSCENE/")!=std::string::npos || name=="OU.BIN" || name=="OU.DAT" || ext==".CSL")
    return "cutscene";
  if(ext==".DAT" || ext==".SRC" || ext==".D")
    return "script";
  if(ext==".FNT")
    return "font";
  if(ext==".BIK" || ext==".BK2" || ext==".AVI" || ext==".MP4")
    return "video";
  return "other";
  }
}

ContentCensus::ContentCensus(const char* output) : output(Tempest::TextCodec::toUtf16(output)) {
  }

void ContentCensus::opened(std::string_view name, std::string_view source) noexcept {
  try {
    std::lock_guard<std::mutex> guard(sync);
    ++files[{basename(name),sourcePath(source)}];
    }
  catch(...) {
    Tempest::Log::e("census: unable to record file");
    }
  }

void ContentCensus::replaced(std::string_view name, std::string_view replacement) noexcept {
  try {
    std::lock_guard<std::mutex> guard(sync);
    ++replacements[{basename(name),std::string(replacement)}];
    }
  catch(...) {
    Tempest::Log::e("census: unable to record replacement");
    }
  }

void ContentCensus::missing(std::string_view name, std::string_view reason) noexcept {
  try {
    std::lock_guard<std::mutex> guard(sync);
    ++misses[{basename(name),std::string(reason)}];
    }
  catch(...) { Tempest::Log::e("census: unable to record base miss"); }
  }

void ContentCensus::profile(std::string_view game, std::string_view outputUnits, int language) noexcept {
  try {
    std::lock_guard<std::mutex> guard(sync);
    this->game = game;
    this->outputUnitsPrefix = outputUnits;
    this->language = language;
    hasProfile = true;
    }
  catch(...) { Tempest::Log::e("census: unable to record active profile"); }
  }

void ContentCensus::selected(std::string_view consumer, std::string_view request, int language,
                             std::string_view branch, std::string_view location, std::string_view path,
                             std::string_view source, zenkit::Read& read) noexcept {
  // Hash the same successfully parsed stream, without changing the parser's cursor.
  struct RestorePosition {
    zenkit::Read& read;
    size_t position;
    ~RestorePosition() { read.seek(static_cast<std::ptrdiff_t>(position),zenkit::Whence::BEG); }
    } restore{read,read.tell()};
  try {
    read.seek(0,zenkit::Whence::END);
    const auto size = read.tell();
    read.seek(0,zenkit::Whence::BEG);
    Sha256 hash;
    char buffer[65536];
    size_t remaining = size;
    while(remaining>0) {
      const auto count = read.read(buffer,std::min(remaining,sizeof(buffer)));
      if(count==0)
        throw std::runtime_error("short source read");
      hash.update(buffer,count);
      remaining -= count;
      }
    Selection entry;
    entry.consumer = consumer;
    entry.request = request;
    entry.language = language;
    entry.branch = branch;
    entry.location = location;
    entry.path = path.empty() ? "unknown" : path;
    entry.source = source.empty() ? "unknown" : source;
    entry.sha256 = hash.finish();
    entry.size = size;
    std::lock_guard<std::mutex> guard(sync);
    selections.push_back(std::move(entry));
    }
  catch(...) { Tempest::Log::e("census: unable to record parsed source"); }
  }

void ContentCensus::program(std::string_view consumer, std::string_view request, int language,
                            std::string_view branch, std::string_view record, std::string_view sha256,
                            const ProgramData::Source& source) noexcept {
  try {
    ProgramSelection entry;
    entry.consumer = consumer;
    entry.request = request;
    entry.language = language;
    entry.branch = branch;
    entry.record = record;
    entry.sha256 = sha256;
    entry.sourceOrigin = source.origin;
    entry.sourcePath = source.path;
    entry.sourceArchive = source.archive;
    entry.sourceSha256 = source.sha256;
    entry.sourceSize = source.size;
    std::lock_guard<std::mutex> guard(sync);
    programs.push_back(std::move(entry));
    }
  catch(...) { Tempest::Log::e("census: unable to record base program"); }
  }

void ContentCensus::outputUnits(std::string_view consumer, std::string_view request, int language,
                                std::string_view branch, std::string_view record, std::string_view sha256,
                                const OutputUnits::Source& source) noexcept {
  try {
    ProgramSelection entry;
    entry.consumer = consumer;
    entry.request = request;
    entry.language = language;
    entry.branch = branch;
    entry.record = record;
    entry.sha256 = sha256;
    entry.sourceOrigin = source.origin;
    entry.sourcePath = source.path;
    entry.sourceArchive = source.archive;
    entry.sourceSha256 = source.sha256;
    entry.sourceSize = source.size;
    std::lock_guard<std::mutex> guard(sync);
    outputUnitsSelections.push_back(std::move(entry));
    }
  catch(...) { Tempest::Log::e("census: unable to record base output units"); }
  }

void ContentCensus::audioLoaded(std::string_view request, const AudioData::Sound& source) noexcept {
  try {
    AudioSelection entry;
    entry.request = request;
    entry.cls = source.cls;
    entry.payloadFile = "base/audio/"+source.file;
    entry.payloadSha256 = source.payload;
    entry.sourceOrigin = source.origin;
    entry.sourcePath = source.path;
    entry.sourceArchive = source.archive;
    entry.sourceSha256 = source.sourceSha256;
    entry.size = source.size;
    std::lock_guard<std::mutex> guard(sync);
    audioSelections.push_back(std::move(entry));
    }
  catch(...) { Tempest::Log::e("census: unable to record base audio"); }
  }

void ContentCensus::musicLoaded(std::string_view provider, std::string_view request,
                               const MusicData::Source& source, const MusicData::Outcome& outcome) noexcept {
  try {
    MusicSelection entry;
    entry.provider = provider;
    entry.request = request;
    entry.kind = source.kind;
    entry.record = "base/music/"+outcome.record;
    entry.sha256 = outcome.identity;
    entry.sourceOrigin = source.origin;
    entry.sourcePath = source.path;
    entry.sourceArchive = source.archive;
    entry.sourceSha256 = source.sha256;
    entry.sourceSize = source.size;
    std::lock_guard<std::mutex> guard(sync);
    musicSelections.push_back(std::move(entry));
    }
  catch(...) { Tempest::Log::e("census: unable to record base music"); }
  }

bool ContentCensus::write() noexcept {
  try {
    std::lock_guard<std::mutex> guard(sync);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> json(buffer);
    auto string = [&](std::string_view value) {
      json.String(value.data(),rapidjson::SizeType(value.size()));
      };
    json.StartObject();
    json.Key("files");
    json.StartArray();
    for(auto& [key,count]:files) {
      json.StartObject();
      json.Key("name"); string(key.first);
      json.Key("kind"); string(kind(key.first,key.second));
      json.Key("source"); string(key.second);
      json.Key("opens"); json.Uint64(count);
      json.EndObject();
      }
    json.EndArray();
    json.Key("replacements");
    json.StartArray();
    for(auto& [key,count]:replacements) {
      json.StartObject();
      json.Key("name"); string(key.first);
      json.Key("kind"); string(key.second.compare(0,14,"base/textures/")==0 ? "basetexture" :
                               key.second.compare(0,12,"base/meshes/")==0 ? "basemesh" :
                               key.second.compare(0,12,"base/models/")==0 ? "basemodel" :
                               key.second.compare(0,11,"base/fonts/")==0 ? "basefont" :
                               key.second.compare(0,12,"base/videos/")==0 ? "basevideo" :
                               key.second.compare(0,14,"base/programs/")==0 ? "baseprogram" :
                               key.second.compare(0,17,"base/outputunits/")==0 ? "baseoutputunits" :
                               key.second.compare(0,12,"base/worlds/")==0 ? "baseworld" :
                               key.second.compare(0,9,"base/zen/")==0 ? "basezen" :
                               key.second.compare(0,7,"meshes/")==0 ? "mesh" :
                               key.second.compare(0,7,"worlds/")==0 ? "worldmesh" : kind(key.first,{}));
      json.Key("replacement"); string(key.second);
      json.Key("opens"); json.Uint64(count);
      json.EndObject();
      }
    for(const auto& entry:audioSelections) {
      json.StartObject();
      json.Key("name"); string(basename(entry.sourcePath));
      json.Key("kind"); string(entry.cls=="speech" ? "basespeech" : "basesound");
      json.Key("replacement"); string(entry.payloadFile);
      json.Key("opens"); json.Uint64(1);
      json.EndObject();
      }
    for(const auto& entry:musicSelections) {
      json.StartObject();
      json.Key("name"); string(basename(entry.sourcePath));
      json.Key("kind"); string("basemusic");
      json.Key("replacement"); string(entry.record+".json");
      json.Key("opens"); json.Uint64(1);
      json.EndObject();
      }
    json.EndArray();
    json.Key("base_misses");
    json.StartArray();
    for(const auto& [key,count]:misses) {
      json.StartObject();
      json.Key("name"); string(key.first);
      json.Key("kind"); string(kind(key.first,{}));
      json.Key("reason"); string(key.second);
      json.Key("count"); json.Uint64(count);
      json.EndObject();
      }
    json.EndArray();
    json.Key("active_profile");
    if(hasProfile) {
      json.StartObject();
      json.Key("game"); string(game);
      json.Key("output_units"); string(outputUnitsPrefix);
      json.Key("language"); json.Int(language);
      json.EndObject();
      }
    else {
      json.Null();
      }
    json.Key("runtime_sources");
    json.StartObject();
    json.Key("coverage"); string("observed_successful_parses");
    json.Key("complete_corpus"); json.Bool(false);
    json.Key("selections");
    json.StartArray();
    for(const auto& entry:selections) {
      json.StartObject();
      json.Key("consumer"); string(entry.consumer);
      json.Key("request"); string(entry.request);
      json.Key("language"); json.Int(entry.language);
      json.Key("branch"); string(entry.branch);
      json.Key("location"); string(entry.location);
      json.Key("path"); string(entry.path);
      json.Key("source"); string(entry.source);
      json.Key("size"); json.Uint64(entry.size);
      json.Key("sha256"); string(entry.sha256);
      json.EndObject();
      }
    json.EndArray();
    json.EndObject();
    json.Key("runtime_programs");
    json.StartObject();
    json.Key("coverage"); string("loaded_base_records");
    json.Key("complete_corpus"); json.Bool(false);
    json.Key("selections");
    json.StartArray();
    for(const auto& entry:programs) {
      json.StartObject();
      json.Key("consumer"); string(entry.consumer);
      json.Key("request"); string(entry.request);
      json.Key("language"); json.Int(entry.language);
      json.Key("branch"); string(entry.branch);
      json.Key("record");
      json.StartObject();
      json.Key("json"); string(entry.record);
      auto binary = std::filesystem::path(entry.record);
      binary.replace_extension(".bin");
      json.Key("bin"); string(binary.generic_string());
      json.Key("sha256"); string(entry.sha256);
      json.EndObject();
      json.Key("imported_source");
      json.StartObject();
      json.Key("origin"); string(entry.sourceOrigin);
      json.Key("path"); string(entry.sourcePath);
      json.Key("archive");
      if(entry.sourceArchive.empty()) json.Null(); else string(entry.sourceArchive);
      json.Key("size"); json.Uint64(entry.sourceSize);
      json.Key("sha256"); string(entry.sourceSha256);
      json.EndObject();
      json.EndObject();
      }
    json.EndArray();
    json.EndObject();
    json.Key("runtime_outputunits");
    json.StartObject();
    json.Key("coverage"); string("loaded_base_records");
    json.Key("complete_corpus"); json.Bool(false);
    json.Key("selections");
    json.StartArray();
    for(const auto& entry:outputUnitsSelections) {
      json.StartObject();
      json.Key("consumer"); string(entry.consumer);
      json.Key("request"); string(entry.request);
      json.Key("language"); json.Int(entry.language);
      json.Key("branch"); string(entry.branch);
      json.Key("record");
      json.StartObject();
      json.Key("json"); string(entry.record);
      json.Key("sha256"); string(entry.sha256);
      json.EndObject();
      json.Key("imported_source");
      json.StartObject();
      json.Key("origin"); string(entry.sourceOrigin);
      json.Key("path"); string(entry.sourcePath);
      json.Key("archive");
      if(entry.sourceArchive.empty()) json.Null(); else string(entry.sourceArchive);
      json.Key("size"); json.Uint64(entry.sourceSize);
      json.Key("sha256"); string(entry.sourceSha256);
      json.EndObject();
      json.EndObject();
      }
    json.EndArray();
    json.EndObject();
    json.Key("runtime_audio");
    json.StartObject();
    json.Key("coverage"); string("verified_base_payload_reads");
    json.Key("complete_corpus"); json.Bool(false);
    json.Key("selections");
    json.StartArray();
    for(const auto& entry:audioSelections) {
      json.StartObject();
      json.Key("request"); string(entry.request);
      json.Key("class"); string(entry.cls);
      json.Key("payload");
      json.StartObject();
      json.Key("file"); string(entry.payloadFile);
      json.Key("size"); json.Uint64(entry.size);
      json.Key("sha256"); string(entry.payloadSha256);
      json.EndObject();
      json.Key("imported_source");
      json.StartObject();
      json.Key("origin"); string(entry.sourceOrigin);
      json.Key("path"); string(entry.sourcePath);
      json.Key("archive");
      if(entry.sourceArchive.empty()) json.Null(); else string(entry.sourceArchive);
      json.Key("size"); json.Uint64(entry.size);
      json.Key("sha256"); string(entry.sourceSha256);
      json.EndObject();
      json.EndObject();
      }
    json.EndArray();
    json.EndObject();
    json.Key("runtime_music");
    json.StartObject();
    json.Key("coverage"); string("verified_base_record_reads");
    json.Key("complete_corpus"); json.Bool(false);
    json.Key("selections");
    json.StartArray();
    for(const auto& entry:musicSelections) {
      json.StartObject();
      json.Key("provider"); string(entry.provider);
      json.Key("request"); string(entry.request);
      json.Key("kind"); string(entry.kind);
      json.Key("record");
      json.StartObject();
      json.Key("json"); string(entry.record+".json");
      json.Key("bin"); string(entry.record+".bin");
      json.Key("sha256"); string(entry.sha256);
      json.EndObject();
      json.Key("imported_source");
      json.StartObject();
      json.Key("origin"); string(entry.sourceOrigin);
      json.Key("path"); string(entry.sourcePath);
      json.Key("archive");
      if(entry.sourceArchive.empty()) json.Null(); else string(entry.sourceArchive);
      json.Key("size"); json.Uint64(entry.sourceSize);
      json.Key("sha256"); string(entry.sourceSha256);
      json.EndObject();
      json.EndObject();
      }
    json.EndArray();
    json.EndObject();
    json.EndObject();

    if(!output.parent_path().empty())
      std::filesystem::create_directories(output.parent_path());
    auto temporary = output;
    temporary += ".tmp";
    {
    std::ofstream stream(temporary,std::ios::binary | std::ios::trunc);
    stream.write(buffer.GetString(),std::streamsize(buffer.GetSize()));
    stream.put('\n');
    stream.flush();
    if(!stream)
      throw std::runtime_error("write failed");
    }
#ifdef _WIN32
    std::filesystem::remove(output);
#endif
    std::filesystem::rename(temporary,output);
    return true;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("census: unable to write JSON: ",e.what());
    }
  return false;
  }
