#include "houselamps.h"

#include <Tempest/Log>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <rapidjson/document.h>

HouseLamps::HouseLamps(const std::filesystem::path& worldContentDir) {
  if(const char* e = std::getenv("OPENGOTHIC_HOUSES"); e!=nullptr && std::string_view(e)=="0")
    return;
  const auto file = worldContentDir/"houses.json";
  std::error_code ec;
  if(worldContentDir.empty() || !std::filesystem::is_regular_file(file,ec))
    return;
  std::ifstream in(file, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(text.data(), text.size());
  std::string error;
  if(doc.HasParseError())
    error = "not a json document";
  if(!error.empty() || !HouseLights::parse(doc,list,error)) {
    Tempest::Log::e("houses: ", file.generic_string(), ": ", error);
    list.clear();
    }
  }

void HouseLamps::adopt(LightGroup::Light& light, const Tempest::Vec3& pos, uint32_t id, bool fire) {
  const float p[3] = {pos.x, pos.y, pos.z};
  const int   h    = HouseLights::houseAt(list, p, -HouseLights::LampWall);
  if(h<0)
    return;
  std::lock_guard<std::mutex> guard(sync);
  Lamp l;
  l.light = &light;
  l.pos   = pos;
  l.house = uint32_t(h);
  l.id    = id;
  l.fire  = fire;
  lamps.push_back(l);
  ranked = false;
  }

void HouseLamps::drop(LightGroup::Light& light) {
  if(list.empty())
    return;
  std::lock_guard<std::mutex> guard(sync);
  const auto it = std::remove_if(lamps.begin(), lamps.end(), [&](const Lamp& l) { return l.light==&light; });
  if(it==lamps.end())
    return;
  lamps.erase(it, lamps.end());
  ranked = false;
  }

void HouseLamps::adoptFlame(const void* owner, const Tempest::Vec3& pos, std::function<void(bool)> show) {
  const float p[3] = {pos.x, pos.y, pos.z};
  if(HouseLights::houseAt(list, p, 0)<0)
    return;
  std::lock_guard<std::mutex> guard(sync);
  Flame f;
  f.owner = owner;
  f.pos   = pos;
  f.show  = std::move(show);
  flames.push_back(std::move(f));
  ranked = false;
  }

void HouseLamps::dropFlame(const void* owner) {
  if(list.empty())
    return;
  std::lock_guard<std::mutex> guard(sync);
  const auto it = std::remove_if(flames.begin(), flames.end(), [&](const Flame& f) { return f.owner==owner; });
  if(it==flames.end())
    return;
  flames.erase(it, flames.end());
  ranked = false;
  }

// Lamps of a house that stand together are one fixture. The fixtures take their turns evenly by their ids'
// numbers: the one with turn 0 goes out last, at the bedtime itself, so the house's windows never outlive its
// lamps. A fixture with a fire in it keeps burning. A flame follows the lamp nearest to it
void HouseLamps::rank() {
  ranked = true;
  const float reach2 = FixtureReach*FixtureReach;
  // the fixture of a lamp: the first lamp of its house it stands together with, through its neighbours
  std::vector<size_t> fixture(lamps.size());
  for(size_t i=0; i<lamps.size(); ++i)
    fixture[i] = i;
  auto root = [&](size_t i) {
    while(fixture[i]!=i)
      i = fixture[i];
    return i;
    };
  for(size_t i=0; i<lamps.size(); ++i)
    for(size_t r=0; r<i; ++r)
      if(lamps[i].house==lamps[r].house && (lamps[i].pos-lamps[r].pos).quadLength()<=reach2) {
        const size_t a = root(i), b = root(r);
        fixture[std::max(a,b)] = std::min(a,b);
        }
  std::vector<bool> fire(lamps.size(), false);
  for(size_t i=0; i<lamps.size(); ++i)
    if(lamps[i].fire)
      fire[root(i)] = true;

  std::vector<size_t> order;
  for(size_t i=0; i<lamps.size(); ++i)
    if(fixture[i]==i && !fire[i])
      order.push_back(i);
  auto number = [&](size_t i) { return HouseLights::mix(lamps[i].id*0x9E3779B1u + 0x85EBCA77u); };
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if(lamps[a].house!=lamps[b].house)
      return lamps[a].house<lamps[b].house;
    if(number(a)!=number(b))
      return number(a)<number(b);
    return lamps[a].id<lamps[b].id;
    });
  for(size_t i=0; i<order.size();) {
    size_t end = i;
    while(end<order.size() && lamps[order[end]].house==lamps[order[i]].house)
      ++end;
    const size_t count = end-i;
    for(size_t k=0; k<count; ++k)
      lamps[order[i+k]].turn = count>1 ? float(k)/float(count-1) : 0.f;
    i = end;
    }
  for(size_t i=0; i<lamps.size(); ++i) {
    const size_t r = root(i);
    lamps[i].turn = lamps[r].turn;
    lamps[i].fire = fire[r];
    }

  for(auto& f:flames) {
    f.lamp = size_t(-1);
    float best = reach2;
    for(size_t i=0; i<lamps.size(); ++i) {
      const float d = (lamps[i].pos-f.pos).quadLength();
      if(d<=best && !lamps[i].fire) {
        best   = d;
        f.lamp = i;
        }
      }
    }
  }

void HouseLamps::tick(float dayHour) {
  if(list.empty())
    return;
  std::lock_guard<std::mutex> guard(sync);
  if(!ranked)
    rank();
  for(auto& l:lamps) {
    const bool on = l.fire || HouseLights::lampOn(list[l.house], l.turn, dayHour);
    if(on==l.on)
      continue;
    l.on = on;
    l.light->setEnabled(on);
    }
  for(auto& f:flames) {
    const bool on = f.lamp==size_t(-1) || lamps[f.lamp].on;
    if(on==f.on)
      continue;
    f.on = on;
    f.show(on);
    }
  }
