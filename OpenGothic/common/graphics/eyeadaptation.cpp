#include "eyeadaptation.h"

#include <Tempest/Device>
#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

#include "commandline.h"

using namespace Tempest;

EyeAdaptation::~EyeAdaptation() {
  if(logFile!=nullptr)
    std::fclose(logFile);
  }

bool EyeAdaptation::enabledByDefault() {
  return CommandLine::inst().doEye();
  }

StorageBuffer& EyeAdaptation::result(uint8_t fId) {
  auto& b = buf[fId];
  if(b.isEmpty())
    b = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, ResultWords*sizeof(uint32_t));
  return b;
  }

EyeAdaptation::Measure EyeAdaptation::read(uint8_t fId) {
  Measure m;
  if(!written[fId] || buf[fId].isEmpty())
    return m;
  written[fId] = false;

  uint32_t v[ResultWords] = {};
  Resources::device().readBytes(buf[fId], v, sizeof(v));
  const uint32_t* cnt[2] = {v, v+Bins};
  const int32_t*  sum[2] = {reinterpret_cast<const int32_t*>(v+2*Bins), reinterpret_cast<const int32_t*>(v+3*Bins)};

  // the mean of log2 over the bins of the given histograms, the samples below lo and above hi (shares) left out;
  // a bin cut through keeps its share of its own mean
  auto mean = [&](bool sky, float lo, float hi, float& out) {
    double n = 0;
    for(uint32_t i=0; i<Bins; ++i)
      n += cnt[0][i] + (sky ? cnt[1][i] : 0);
    if(n<=0)
      return false;
    const double a = n*lo, b = n*hi;
    double at = 0, w = 0, s = 0;
    for(uint32_t i=0; i<Bins; ++i) {
      const double c  = cnt[0][i] + (sky ? cnt[1][i] : 0);
      const double sb = double(sum[0][i]) + (sky ? double(sum[1][i]) : 0.0);
      if(c<=0)
        continue;
      const double keep = std::max(0.0, std::min(at+c, b) - std::max(at, a));
      w  += keep;
      s  += sb/256.0 * (keep/c);
      at += c;
      }
    if(w<=0)
      return false;
    out = float(s/w - std::log2(0.18));
    return true;
    };

  double scene = 0, sky = 0;
  for(uint32_t i=0; i<Bins; ++i) {
    scene += cnt[0][i];
    sky   += cnt[1][i];
    }
  m.skyShare = (scene+sky)>0 ? float(sky/(scene+sky)) : 0.f;
  m.exposure = *reinterpret_cast<const float*>(v+4*Bins+0);
  m.lumMed   = *reinterpret_cast<const float*>(v+4*Bins+1);
  // a frame of sky alone (or nearly) says nothing of the place
  if(scene < 0.05*(scene+sky))
    return m;
  m.valid = mean(false, 0.f, 1.f, m.scene) && mean(false, 0.1f, 0.9f, m.sceneTrim) &&
            mean(true,  0.f, 1.f, m.all)   && mean(true,  0.1f, 0.9f, m.allTrim);
  return m;
  }

void EyeAdaptation::off() {
  mulV    = 1;
  ev      = 0;
  snap    = SnapFrames;
  hasLast = false;
  for(auto& w:written)
    w = false;
  }

void EyeAdaptation::update(uint8_t fId, uint64_t tick, int64_t dayTime, const Vec3& cam) {
  if(hasLast) {
    const bool clockBack = tick<lastTick;
    const bool dayJump   = lastDay>=0 && dayTime>=0 && std::abs(dayTime-lastDay)>10*60*1000;
    const bool camJump   = (cam-lastCam).length()>300.f;
    if(clockBack || dayJump || camJump)
      reset();
    }
  const float dt = (hasLast && tick>=lastTick) ? float(tick-lastTick)/1000.f : 0.f;
  hasLast  = true;
  lastTick = tick;
  lastDay  = dayTime;
  lastCam  = cam;

  const Measure m = read(fId);
  if(m.valid) {
    // the night's share of the exposure: exposure*NightL is 0 at noon and 1 in the dark
    const float dayW = std::clamp(1.f - m.exposure*NightL, 0.f, 1.f);
    const float key  = KeyNight + (KeyDay-KeyNight)*dayW;
    target = std::clamp(key - m.scene, 0.f, MaxEv);
    log(tick, dayTime, cam, m, dayW, key, target);
    }

  if(snap>0) {
    ev = target;
    --snap;
    }
  else if(target>ev)
    ev = std::min(target, ev + UpEvPerS*dt);
  else
    ev = std::max(target, ev - DownEvPerS*dt);
  mulV = std::exp2(ev);
  }

void EyeAdaptation::log(uint64_t tick, int64_t dayTime, const Vec3& cam, const Measure& m, float dayW, float key,
                        float tgt) {
  if(!logTried) {
    logTried = true;
    if(const char* e = std::getenv("OPENGOTHIC_EYE_LOG"); e!=nullptr && *e!='\0') {
      std::string path = e;
      const char* out = std::getenv("OPENGOTHIC_BENCH_OUT");
      if(path.front()!='/' && out!=nullptr && *out!='\0')
        path = std::string(out) + "/" + path;
      logFile = std::fopen(path.c_str(), "w");
      if(logFile!=nullptr) {
        std::fprintf(logFile, "tick,day_min,cam_x,cam_y,cam_z,exposure,lum_med,day_w,key,scene,scene_trim,all,all_trim,"
                              "sky_share,target,ev\n");
        Log::i("eye: log ", path);
        }
      }
    }
  if(logFile==nullptr)
    return;
  // ev of the frame before: this one moves after the row
  std::fprintf(logFile, "%llu,%.3f,%.0f,%.0f,%.0f,%.6g,%.6g,%.4f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               static_cast<unsigned long long>(tick), double(dayTime%(24*60*60*1000))/60000.0, double(cam.x), double(cam.y),
               double(cam.z), double(m.exposure), double(m.lumMed), double(dayW), double(key), double(m.scene),
               double(m.sceneTrim), double(m.all), double(m.allTrim), double(m.skyShare), double(tgt), double(ev));
  if(tick>=logFlush+1000) {
    std::fflush(logFile);
    logFlush = tick;
    }
  }
