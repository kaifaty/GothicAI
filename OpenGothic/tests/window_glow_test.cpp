// Windows that glow at night (reports/render-features/2026-11-15-night-interiors): the `window` field of a record,
// its bits for the shader and the evening of a window - dark by day, lit as the rooms go dark, out by its hour, the
// share of the windows that are lit at all, the all-night ones until dawn. CPU only: no device, no game data.
#include "graphics/windowglow.h"

#include <rapidjson/document.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

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

int main() {
  try {
    records();
    bucketBits();
    evening();
    allNight();
    fades();
    std::cout << "{\"test\":\"window-glow\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& e) {
    std::cerr << "window glow test failed: " << e.what() << '\n';
    return 1;
    }
  }
