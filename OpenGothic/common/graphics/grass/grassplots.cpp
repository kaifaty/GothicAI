#include "grassplots.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include <rapidjson/document.h>

namespace Grass {

namespace {

// An array of finite numbers
bool numbers(const rapidjson::Value& patch, const char* field, std::vector<float>& out) {
  const auto it = patch.FindMember(field);
  if(it==patch.MemberEnd() || !it->value.IsArray())
    return false;
  out.clear();
  out.reserve(it->value.Size());
  for(const auto& v:it->value.GetArray()) {
    if(!v.IsNumber() || !std::isfinite(v.GetDouble()) || !std::isfinite(float(v.GetDouble())))
      return false;
    out.push_back(float(v.GetDouble()));
    }
  return true;
  }

}

bool Plots::parse(std::string_view json, const SlotOf& slotOf, std::string& error) {
  error.clear();
  rapidjson::Document doc;
  doc.Parse(json.data(), json.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    error = "not a json object";
    return false;
    }
  const auto format = doc.FindMember("format");
  if(format==doc.MemberEnd() || !format->value.IsInt() || format->value.GetInt()!=1) {
    error = "`format` is not 1";
    return false;
    }
  const auto patches = doc.FindMember("patches");
  if(patches==doc.MemberEnd() || !patches->value.IsArray()) {
    error = "no array `patches`";
    return false;
    }

  std::vector<std::pair<ChunkId,Tri>> taken;
  std::string        unknown;
  std::vector<float> pos, uv;
  for(rapidjson::SizeType i=0; i<patches->value.Size(); ++i) {
    const auto& p    = patches->value[i];
    const auto  fail = [&](const char* what) {
      error = "patch "+std::to_string(i)+": "+what;
      return false;
      };
    if(!p.IsObject())
      return fail("not an object");
    const auto material = p.FindMember("material");
    if(material==p.MemberEnd() || !material->value.IsString())
      return fail("no string `material`");
    if(!numbers(p,"positions",pos) || pos.empty() || pos.size()%3!=0)
      return fail("`positions` is not an array of x, y, z");
    if(!numbers(p,"uvs",uv) || uv.size()/2!=pos.size()/3 || uv.size()%2!=0)
      return fail("`uvs` is not an array of u, v per vertex");
    const auto triangles = p.FindMember("triangles");
    if(triangles==p.MemberEnd() || !triangles->value.IsArray() || triangles->value.Size()%3!=0)
      return fail("`triangles` is not an array of three indices each");
    for(const auto& v:triangles->value.GetArray())
      if(!v.IsUint() || v.GetUint()>=pos.size()/3)
        return fail("`triangles` has an index out of `positions`");

    const std::string_view name(material->value.GetString(), material->value.GetStringLength());
    const uint16_t         slot = slotOf(name);
    if(slot==uint16_t(-1)) {
      if(unknown.empty())
        unknown = "patch "+std::to_string(i)+": material \""+std::string(name)+"\" has no record with `grass`";
      continue;
      }
    for(rapidjson::SizeType k=0; k+2<triangles->value.Size(); k+=3) {
      Tri   t;
      float cx = 0, cz = 0;
      t.slot = slot;
      for(rapidjson::SizeType c=0; c<3; ++c) {
        const size_t v = triangles->value[k+c].GetUint();
        t.pos[c][0] = pos[v*3]; t.pos[c][1] = pos[v*3+1]; t.pos[c][2] = pos[v*3+2];
        t.uv[c][0]  = uv[v*2];  t.uv[c][1]  = uv[v*2+1];
        cx += t.pos[c][0]/3.f;
        cz += t.pos[c][2]/3.f;
        }
      taken.emplace_back(ChunkId::fromPosition(cx,cz),t);
      }
    }
  for(const auto& [id,t]:taken)
    byChunk[{id.x,id.z}].push_back(t);
  error = unknown;
  return true;
  }

void Plots::load(const std::filesystem::path& worldContentDir, const SlotOf& slotOf) {
  const auto dir = worldContentDir/"grass.d";
  std::error_code ec;
  if(worldContentDir.empty() || !std::filesystem::is_directory(dir,ec))
    return;
  // every part of the content brings its ground as its own file; sorted, as the vob layer's files
  std::vector<std::filesystem::path> files;
  for(const auto& entry:std::filesystem::directory_iterator(dir,ec))
    if(entry.is_regular_file(ec) && entry.path().extension()==".json")
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  for(const auto& file:files) {
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    parse(text,slotOf,error);
    if(!error.empty())
      Tempest::Log::e("grass: ", file.generic_string(), ": ", error);
    }
  }

void Plots::addTo(Summary& out, ChunkId chunk) const {
  const auto it = byChunk.find({chunk.x,chunk.z});
  if(it==byChunk.end())
    return;
  for(const auto& t:it->second) {
    // the plane's normal, turned up
    const float ax = t.pos[1][0]-t.pos[0][0], ay = t.pos[1][1]-t.pos[0][1], az = t.pos[1][2]-t.pos[0][2];
    const float bx = t.pos[2][0]-t.pos[0][0], by = t.pos[2][1]-t.pos[0][1], bz = t.pos[2][2]-t.pos[0][2];
    float       n[3] = {ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx};
    const float l    = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if(!(l>0.f))
      continue;
    const float k = (n[1]<0.f ? -1.f : 1.f)/l;
    const float normal[3][3] = {{n[0]*k,n[1]*k,n[2]*k}, {n[0]*k,n[1]*k,n[2]*k}, {n[0]*k,n[1]*k,n[2]*k}};
    addGrass(out,t.pos,t.uv,normal,t.slot);
    }
  }

}
