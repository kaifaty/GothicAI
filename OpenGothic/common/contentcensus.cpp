#include "contentcensus.h"

#include <Tempest/Log>
#include <Tempest/TextCodec>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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
      json.Key("kind"); string(key.second.compare(0,7,"meshes/")==0 ? "mesh" : kind(key.first,{}));
      json.Key("replacement"); string(key.second);
      json.Key("opens"); json.Uint64(count);
      json.EndObject();
      }
    json.EndArray();
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
