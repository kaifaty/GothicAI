// Windows that glow at night (reports/render-features/2026-11-15-night-interiors): the `window` field of a record,
// its bits for the shader and the evening of a window - dark by day, lit as the rooms go dark, out by its hour, the
// share of the windows that are lit at all, the all-night ones until dawn. And the house of step 2
// (graphics/houselights.h): its record, its box, the lamps that go out before its bedtime and the windows that
// fade with them. CPU only: no device, no game data.
#include "graphics/windowglow.h"
#include "graphics/houselights.h"

#include <rapidjson/document.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

bool parsed(const char* json, WindowGlow::Params& out) {
  rapidjson::Document d;
  d.Parse(json);
  require(!d.HasParseError(), std::string("the test's own json: ")+json);
  return WindowGlow::parse(d,out);
  }

void records() {
  WindowGlow::Params p;
  require(!p.on, "a material without the field has no window");
  require(parsed("{}",p) && p.on && p.lit==0.6f && p.until==23.5f, "an empty window object must take the defaults");
  require(parsed(R"({"lit":0.35,"until":29})",p) && p.on && p.lit==0.35f && p.until==29.f,
          "window must carry the record's numbers");
  require(parsed(R"({"lit":0,"until":18})",p) && p.lit==0.f && p.until==18.f, "the low ends of the ranges are allowed");
  require(parsed(R"({"lit":1,"until":36})",p) && p.lit==1.f && p.until==36.f, "the high ends of the ranges are allowed");

  const WindowGlow::Params kept = p;
  for(const char* bad:{"1", "true", "[]", "\"on\"", "null", R"({"lit":"1"})", R"({"lit":1.1})", R"({"lit":-0.1})",
                       R"({"until":17.9})", R"({"until":36.5})", R"({"until":[22]})"}) {
    WindowGlow::Params out = kept;
    require(!parsed(bad,out), std::string("a window of a wrong type or with a number out of its range must be refused: ")+bad);
    require(out==kept, std::string("a refused window leaves the record's numbers alone: ")+bad);
    }
  }

void bucketBits() {
  require(WindowGlow::pack(WindowGlow::Params())==0, "no window packs as zero");
  WindowGlow::Params p;
  p.on = true;
  for(float lit:{0.f, 0.35f, 0.6f, 1.f})
    for(float until:{18.f, 23.5f, 24.f, 29.f, 36.f}) {
      p.lit   = lit;
      p.until = until;
      const uint32_t bits = WindowGlow::pack(p);
      require(bits<=0xFFFFu, "a window takes 16 bits of the slot");
      require(std::fabs(WindowGlow::packedLit(bits)-lit)<=0.5f/255.f+1e-6f, "the share survives its 8 bits");
      require(std::fabs(WindowGlow::packedUntil(bits)-until)<=0.5f*18.f/255.f+1e-5f, "the hour survives its 8 bits");
      }
  }

// the numbers of window k of n, spread evenly and apart from one another
void numbers(int k, int n, float r[3]) {
  r[0] = (float(k)+0.5f)/float(n);
  r[1] = std::fmod(float(k)*0.61803f, 1.f);
  r[2] = std::fmod(float(k)*0.75488f, 1.f);
  }

void evening() {
  WindowGlow::Params home;
  home.on = true;
  const uint32_t bits = WindowGlow::pack(home);
  const int n = 400;

  // dark by day and through the afternoon, whatever the window
  for(float hour:{5.5f, 8.f, 12.f, 15.f, 19.f, 21.f}) {
    for(int k=0; k<n; ++k) {
      float r[3];
      numbers(k,n,r);
      require(WindowGlow::light(bits,r,hour)==0.f, "no window burns at "+std::to_string(hour));
      }
    }

  // once the last candle is lit every window that is lit at all has come up, and about `lit` of them are
  int lit = 0, never = 0;
  for(int k=0; k<n; ++k) {
    float r[3];
    numbers(k,n,r);
    const float l = WindowGlow::light(bits,r,WindowGlow::OnFrom+WindowGlow::OnSpread+WindowGlow::Fade);
    require(l>=0.f && l<=1.f, "the light stays within 0..1");
    if(r[0]>=WindowGlow::packedLit(bits)) {
      ++never;
      for(float hour=12.f; hour<36.f; hour+=0.25f)
        require(WindowGlow::light(bits,r,hour>=24.f ? hour-24.f : hour)==0.f, "an unlit window stays dark all evening");
      }
    if(l>0.f)
      ++lit;
    }
  require(std::abs(lit+never-n)<=n/8, "once the last candle is lit nearly every window that is lit at all burns");
  require(std::abs(float(lit)/float(n)-home.lit)<0.12f, "about the record's share of the windows is lit");

  // the windows do not come up or go out together, and none is left by 01:00 with `until` 23:30
  int early = 0, later = 0, atUntil = 0, at0100 = 0;
  for(int k=0; k<n; ++k) {
    float r[3];
    numbers(k,n,r);
    early   += WindowGlow::light(bits,r,WindowGlow::OnFrom+0.3f)>0.f;
    later   += WindowGlow::light(bits,r,WindowGlow::OnFrom+0.8f)>0.f;
    atUntil += WindowGlow::light(bits,r,home.until)>0.3f;
    at0100  += WindowGlow::light(bits,r,1.f)>0.f;
    }
  require(early>0 && early<later && later<lit+1, "the windows come up one after another");
  require(atUntil>0 && atUntil<lit, "by `until` some are out and some still burn");
  require(at0100==0, "a home is dark at one in the morning");

  // a lit window is never dimmer than Dimmest of the brightest once it is up
  for(int k=0; k<n; ++k) {
    float r[3];
    numbers(k,n,r);
    const float on = WindowGlow::OnFrom + r[1]*WindowGlow::OnSpread + WindowGlow::Fade;
    if(r[0]<WindowGlow::packedLit(bits)) {
      const float l = WindowGlow::light(bits,r,on+0.05f);
      require(l>=WindowGlow::Dimmest-1e-4f && l<=1.f, "a window that is up shines between Dimmest and 1");
      }
    }
  }

void allNight() {
  WindowGlow::Params tavern;
  tavern.on    = true;
  tavern.lit   = 1.f;
  tavern.until = 29.f;
  const uint32_t bits = WindowGlow::pack(tavern);
  for(int k=0; k<100; ++k) {
    float r[3];
    numbers(k,100,r);
    require(WindowGlow::light(bits,r,23.f)>0.f, "an all-night window burns at 23:00");
    require(WindowGlow::light(bits,r,2.f)>0.f,  "an all-night window burns at 02:00");
    require(WindowGlow::light(bits,r,4.f)>0.f,  "an all-night window burns at 04:00");
    require(WindowGlow::light(bits,r,5.25f)==0.f, "nothing burns into the day");
    require(WindowGlow::light(bits,r,12.f)==0.f, "nothing burns at noon");
    }
  }

void fades() {
  WindowGlow::Params p;
  p.on  = true;
  p.lit = 1.f;
  const uint32_t bits = WindowGlow::pack(p);
  const float r[3] = {0.2f, 0.4f, 0.5f};
  float before = 0.f;
  bool  rose = false;
  for(float hour=WindowGlow::OnFrom-0.25f; hour<=WindowGlow::OnFrom+WindowGlow::OnSpread+0.4f; hour+=0.01f) {
    const float l = WindowGlow::light(bits,r,hour);
    require(l>=before-1e-5f, "a window comes up without a dip");
    require(l-before<0.15f, "a window comes up over the fade, not in one step");
    rose  |= l>0.f;
    before = l;
    }
  require(rose && before>=WindowGlow::Dimmest, "the window is up once the last candle is lit");
  }

}

bool parsedHouses(const char* json, std::vector<HouseLights::House>& out, std::string& error) {
  rapidjson::Document d;
  d.Parse(json);
  require(!d.HasParseError(), std::string("the test's own json: ")+json);
  return HouseLights::parse(d,out,error);
  }

void houseRecords() {
  std::vector<HouseLights::House> houses;
  std::string error;
  require(parsedHouses(R"({"format":1,"houses":[]})",houses,error) && houses.empty(), "no houses is a valid file");
  require(parsedHouses(R"({"format":1,"houses":[
            {"key":"a","center":[100,200,300],"size":[1000,400,600],"yaw_deg":90,"until":23.5},
            {"key":"b","center":[0,0,0],"size":[2,2,2]}]})",houses,error) && houses.size()==2,
          "two houses: "+error);
  const auto& a = houses[0];
  require(a.key=="a" && a.centre[0]==100.f && a.centre[1]==200.f && a.centre[2]==300.f, "the centre is the record's");
  require(a.half[0]==500.f && a.half[1]==200.f && a.half[2]==300.f, "the half size is half the record's size");
  require(std::fabs(a.yaw-1.5707963f)<1e-5f && a.until==23.5f && a.bed==23.5f, "the yaw and the bedtime are the record's");
  const auto& b = houses[1];
  require(b.yaw==0.f && b.until<0.f, "a house without yaw and bedtime");
  require(b.bed>=HouseLights::BedFrom && b.bed<HouseLights::BedFrom+HouseLights::BedSpread,
          "a house without a bedtime goes to bed by its key");
  require(a.wake>=HouseLights::WakeFrom && a.wake<HouseLights::WakeFrom+HouseLights::WakeSpread,
          "a house wakes within the morning hour");

  const auto kept = houses;
  for(const char* bad:{R"([])", R"({"format":2,"houses":[]})", R"({"format":1})", R"({"format":1,"houses":{}})",
                       R"({"format":1,"houses":[{"center":[0,0,0],"size":[1,1,1]}]})",
                       R"({"format":1,"houses":[{"key":"","center":[0,0,0],"size":[1,1,1]}]})",
                       R"({"format":1,"houses":[{"key":"a","center":[0,0],"size":[1,1,1]}]})",
                       R"({"format":1,"houses":[{"key":"a","center":[0,0,0],"size":[1,0,1]}]})",
                       R"({"format":1,"houses":[{"key":"a","center":[0,0,0],"size":[1,1,1],"yaw_deg":"n"}]})",
                       R"({"format":1,"houses":[{"key":"a","center":[0,0,0],"size":[1,1,1],"until":17}]})",
                       R"({"format":1,"houses":[{"key":"a","center":[0,0,0],"size":[1,1,1]},
                                               {"key":"a","center":[9,9,9],"size":[1,1,1]}]})"}) {
    error.clear();
    require(!parsedHouses(bad,houses,error) && !error.empty(), std::string("a bad houses file must be refused: ")+bad);
    require(houses.size()==kept.size() && houses[0].key==kept[0].key, "a refused file leaves the houses alone");
    }
  std::string many = R"({"format":1,"houses":[)";
  for(size_t i=0; i<=HouseLights::MaxHouses; ++i)
    many += std::string(i==0 ? "" : ",")+R"({"key":"h)"+std::to_string(i)+R"(","center":[0,0,0],"size":[1,1,1]})";
  many += "]}";
  require(!parsedHouses(many.c_str(),houses,error), "more houses than the scene block carries are refused");

  // the files of houses.d one after another
  std::vector<HouseLights::House> all = kept, more;
  require(parsedHouses(R"({"format":1,"houses":[{"key":"c","center":[5,5,5],"size":[1,1,1]}]})",more,error), "a second file");
  require(HouseLights::append(all,more,error) && all.size()==3 && all[0].key=="a" && all[2].key=="c",
          "the houses of a later file stand after those of the files before it");
  require(!HouseLights::append(all,more,error) && all.size()==3 && !error.empty(), "a key of a file before is refused");
  std::vector<HouseLights::House> crowd;
  for(size_t i=0; i<HouseLights::MaxHouses-2; ++i) {
    HouseLights::House h;
    h.key = "n"+std::to_string(i);
    crowd.push_back(h);
    }
  require(!HouseLights::append(all,crowd,error) && all.size()==3, "files that together pass the scene block's count are refused");
  crowd.pop_back();
  require(HouseLights::append(all,crowd,error) && all.size()==HouseLights::MaxHouses, "up to the count they fit");
  }

void houseBox() {
  HouseLights::House h;
  h.centre = {1000.f, 500.f, -2000.f};
  h.half   = {400.f, 300.f, 100.f};
  h.yaw    = 0.5235988f; // 30 degrees: the long side runs along (cos,0,sin)
  const float c = std::cos(h.yaw), s = std::sin(h.yaw);
  auto at = [&](float along, float up, float across) {
    return std::array<float,3>{h.centre[0]+along*c-across*s, h.centre[1]+up, h.centre[2]+along*s+across*c};
    };
  require( HouseLights::inside(h,at(390,0,0).data(),0),    "a point near the end of the long side is inside");
  require(!HouseLights::inside(h,at(0,0,390).data(),0),    "the same distance across the short side is outside");
  require(!HouseLights::inside(h,at(410,0,0).data(),0),    "past the end is outside");
  require( HouseLights::inside(h,at(430,0,0).data(),HouseLights::WindowWall),  "a window on the outside of the wall is the house's");
  require(!HouseLights::inside(h,at(390,0,0).data(),-HouseLights::LampWall),   "a lamp in the wall is not the house's");
  require( HouseLights::inside(h,at(370,0,0).data(),-HouseLights::LampWall),   "a lamp inside is");
  require(!HouseLights::inside(h,at(0,310,0).data(),0) && HouseLights::inside(h,at(0,-290,0).data(),0), "the box has a floor and a roof");

  std::vector<HouseLights::House> two = {h,h};
  two[1].centre = {5000.f, 500.f, -2000.f};
  const float far[3] = {9000.f, 500.f, -2000.f};
  require(HouseLights::houseAt(two,at(0,0,0).data(),0)==0, "the first house a point is in");
  require(HouseLights::houseAt(two,two[1].centre.data(),0)==1, "the second house");
  require(HouseLights::houseAt(two,far,HouseLights::WindowWall)==-1, "a point of no house");

  float v[12] = {};
  HouseLights::settle(h);
  HouseLights::pack(h,v);
  require(v[0]==h.centre[0] && v[1]==h.centre[1] && v[2]==h.centre[2] && std::fabs(v[3]-c)<1e-6f, "the block's first vector: the centre and the cosine");
  require(v[4]==h.half[0]+HouseLights::WindowWall && v[6]==h.half[2]+HouseLights::WindowWall && std::fabs(v[7]-s)<1e-6f,
          "the block's second vector: the half size with the windows' margin and the sine");
  require(v[8]==h.bed, "the block's third vector: the bedtime");
  }

void houseEvening() {
  HouseLights::House h;
  h.key   = "market-house";
  h.until = 23.5f;
  HouseLights::settle(h);
  const float turns[] = {0.f, 0.25f, 0.5f, 1.f};

  for(float hour:{6.75f, 9.f, 12.f, 15.f, 20.f, 22.f, 23.3f})
    for(float r:turns)
      require(HouseLights::lampOn(h,r,hour), "the lamps of a house burn by day and through the evening, at "+std::to_string(hour));
  for(float hour:{23.51f, 0.f, 2.f, 4.f, 5.4f})
    for(float r:turns)
      require(!HouseLights::lampOn(h,r,hour), "no lamp of a sleeping house burns at "+std::to_string(hour));
  require( HouseLights::lampOn(h,0.f,23.49f) && !HouseLights::lampOn(h,1.f,23.49f), "the lamps go out one after another");
  require( HouseLights::lampOn(h,0.f,h.wake-24.f+0.01f) && !HouseLights::lampOn(h,1.f,h.wake-24.f+0.01f),
          "and come back one after another in the morning");

  // a window of the house is lit only while a lamp burns inside: the lamp with turn 0 outlives the windows
  float before = 1.f;
  for(float e=20.f; e<36.f; e+=0.005f) {
    const float hour  = e>=24.f ? e-24.f : e;
    const float share = HouseLights::windowShare(h,hour);
    require(share>=0.f && share<=1.f, "the share stays within 0..1");
    if(e<WindowGlow::Dawn) {
      require(share<=before+1e-6f, "the windows of a house only fade through the night");
      before = share;
      }
    if(e<WindowGlow::Dawn+WindowGlow::Fade)
      require(share<=0.f || HouseLights::lampOn(h,0.f,hour), "a lit window means a lamp burns inside, at "+std::to_string(hour));
    }
  require(HouseLights::windowShare(h,23.3f)==1.f, "before the lamps start to go out the windows are untouched");
  require(HouseLights::windowShare(h,23.5f)==0.f && HouseLights::windowShare(h,2.f)==0.f, "the windows of a sleeping house are dark");

  HouseLights::House tavern;
  tavern.key   = "tavern";
  tavern.until = 29.f;
  HouseLights::settle(tavern);
  for(float hour=0.f; hour<24.f; hour+=0.25f) {
    require(HouseLights::lampOn(tavern,1.f,hour), "an all-night house keeps its lamps");
    require(HouseLights::windowShare(tavern,hour)==1.f, "and leaves its windows their own evening");
    }
  float v[12] = {};
  HouseLights::pack(tavern,v);
  require(v[8]>100.f, "an all-night house has no bedtime in the block");
  }

int main() {
  try {
    records();
    bucketBits();
    evening();
    allNight();
    fades();
    houseRecords();
    houseBox();
    houseEvening();
    std::cout << "{\"test\":\"window-glow\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& e) {
    std::cerr << "window glow test failed: " << e.what() << '\n';
    return 1;
    }
  }
