#include "lipsync.h"

#include <Tempest/Sound>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint64_t StepMs  = 10;
constexpr float    MinPeak = -45.f;  // dB: a line quieter than this is not stretched to a wide open mouth

// the tone (YIN): the sound at 8 kHz, a window of 30 ms compared with itself shifted by one period of 60..400 Hz
constexpr uint32_t ToneRate  = 8000;
constexpr size_t   ToneStep  = ToneRate*StepMs/1000;  // samples a step
constexpr size_t   ToneWin   = 240;                   // 30 ms
constexpr size_t   TauMin    = ToneRate/400;          // the shortest period: 400 Hz
constexpr size_t   TauMax    = ToneRate/60+1;         // the longest: 59.7 Hz
constexpr size_t   ToneSpan  = ToneWin+TauMax+1;      // samples a step looks at, centred on the step's middle
constexpr size_t   TonePad   = ToneSpan/2;            // silence before the sound: the first steps have their span
constexpr float    ToneThr   = 0.15f;  // of the normalized difference: a period below it is clear, else no tone
constexpr float    ToneQuiet = 1e-8f;  // mean power of the window below which it is silence (-80 dB)
constexpr float    ToneCut   = 1000.f; // Hz: the harmonics above do not help to find the period

// the parameters can be changed at runtime (LipSync::setParam, bench `lips param <name> <value>`)
float Range   = 24.f;   // dB below the loudest moment of the line where the mouth starts to open
float Top     = 2.f;    // dB below the loudest moment where it is open as wide as it gets
float Gamma   = 1.5f;   // >1: quiet sounds open the mouth less
float Lead    = 40.f;   // ms, the mouth moves this much ahead of the sound
float Attack  = 25.f;   // ms, time constant of opening
float Release = 60.f;   // ms, of closing
float Look    = 150.f;  // ms, how far ahead a new syllable looks for its loudest moment
float Blend   = 60.f;   // ms, from one mouth shape to the next
float Dip     = 0.6f;   // a syllable ends when the voice falls below this part of its loudest moment...
float Rise    = 0.15f;  // ...and the next one starts when it rises this much above the quietest moment
float MinOpen = 0.15f;  // shapes that open the mouth less than this are not used
float Fit     = 0.5f;   // a shape may open this part of what the syllable needs (more shapes to pick from)

struct Param final {
  std::string_view name;
  float*           value;
  };

const Param params[] = {
  {"Range", &Range}, {"Top", &Top}, {"Gamma", &Gamma}, {"Lead", &Lead},
  {"Attack", &Attack}, {"Release", &Release}, {"Look", &Look}, {"Blend", &Blend},
  {"Dip", &Dip}, {"Rise", &Rise}, {"MinOpen", &MinOpen}, {"Fit", &Fit},
  };

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_LIPSYNC");
  return v==nullptr || v[0]!='0';
  }();

// approach `target` with time constant `tau` (ms)
float smooth(float v, float target, uint64_t dt, float tau) {
  if(tau<=0)
    return target;
  return v + (target-v)*(1.f - std::exp(-float(dt)/tau));
  }

}

LipSync::Probe LipSync::subjectProbe;
const void*    LipSync::probeNpc = nullptr;

bool LipSync::isEnabled() {
  return enabled;
  }

void LipSync::setEnabled(bool e) {
  enabled = e;
  }

void LipSync::syllableRule(float& dip, float& rise) {
  dip  = Dip;
  rise = Rise;
  }

bool LipSync::setParam(std::string_view name, float value) {
  for(auto& p:params)
    if(p.name==name) {
      *p.value = value;
      return true;
      }
  return false;
  }

std::string LipSync::paramList() {
  std::string ret;
  for(auto& p:params) {
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s%.*s=%g",ret.empty() ? "" : " ",int(p.name.size()),p.name.data(),double(*p.value));
    ret += buf;
    }
  return ret;
  }


float LipSync::Track::Biquad::run(float x) {
  const float y = b0*x + z1;
  z1 = b1*x - a1*y + z2;
  z2 = b2*x - a2*y;
  return y;
  }

LipSync::Track::Track(uint32_t r)
  :rate(std::max<uint32_t>(r,1000)) {
  perStep = std::max<size_t>(1, size_t(rate*StepMs/1000));
  // the speech band (RBJ filters, Q = 0.707): the jaw follows the vowels, not rumble or hiss
  auto mk = [this](Biquad& f, float freq, bool high) {
    const float w = 2.f*float(M_PI)*std::min(freq, 0.45f*float(rate))/float(rate);
    const float c = std::cos(w), a = std::sin(w)/(2.f*0.7071f);
    const float n = 1.f/(1.f+a);
    if(high) {
      f.b0 = (1.f+c)*0.5f*n; f.b1 = -(1.f+c)*n; f.b2 = f.b0;
      } else {
      f.b0 = (1.f-c)*0.5f*n; f.b1 = (1.f-c)*n;  f.b2 = f.b0;
      }
    f.a1 = -2.f*c*n;
    f.a2 = (1.f-a)*n;
    };
  mk(hp, 120.f,  true);
  mk(lp, 2500.f, false);
  for(auto& f:toneLp)
    mk(f, ToneCut, false);
  decStep = double(rate)/double(ToneRate);
  dec.assign(TonePad,0.f);
  }

std::shared_ptr<LipSync::Track> LipSync::Track::fromSound(const Tempest::Sound& snd, std::string_view name) {
  size_t   count = 0;
  uint16_t ch    = 1;
  uint32_t freq  = 0;
  auto*    pcm   = snd.samples16(count,ch,freq);
  if(pcm==nullptr || count==0 || freq==0)
    return nullptr;
  auto t = std::make_shared<Track>(freq);
  t->lineName = std::string(name);
  t->append(pcm,count,ch);
  t->finish();
  return t;
  }

void LipSync::Track::append(const int16_t* pcm, size_t count, uint16_t channels) {
  std::lock_guard<std::mutex> guard(sync);
  if(channels==0)
    return;
  for(size_t i=0; i+channels<=count; i+=channels) {
    float x = 0;
    for(uint16_t c=0; c<channels; ++c)
      x += float(pcm[i+c]);
    x = x/(32768.f*float(channels));
    pushTone(x);
    x = lp.run(hp.run(x));
    acc += double(x*x);
    if(++accN<perStep)
      continue;
    db.push_back(float(10.0*std::log10(acc/double(accN) + 1e-10)));
    acc  = 0;
    accN = 0;
    // a click is not the loudest moment: the peak is taken over 30 ms
    if(db.size()>=3) {
      const size_t n = db.size();
      peakDb = std::max(peakDb, (db[n-1]+db[n-2]+db[n-3])/3.f);
      }
    }
  findTones(false);
  }

void LipSync::Track::pushTone(float x) {
  x = toneLp[1].run(toneLp[0].run(x));
  // the samples at 8 kHz between the last one and this, by a straight line
  while(decAt<=1.0) {
    dec.push_back(decLast + (x-decLast)*float(decAt));
    decAt += decStep;
    }
  decAt  -= 1.0;
  decLast = x;
  }

void LipSync::Track::findTones(bool flush) {
  // step i looks at [i*ToneStep + ToneStep/2 - ToneSpan/2, +ToneSpan) of the sound, TonePad of silence before it
  while(hz.size()<db.size()) {
    const size_t at = hz.size()*ToneStep + ToneStep/2 + TonePad - ToneSpan/2;
    if(at+ToneSpan>decBase+dec.size()) {
      if(!flush)
        break;
      dec.resize(at+ToneSpan-decBase,0.f);  // the end of the sound: silence after it
      }
    const float f = yin(at);
    hz.push_back(f);
    if(f>0)
      ++toned;
    }
  // the samples no step looks at any more
  const size_t next = hz.size()*ToneStep + ToneStep/2 + TonePad - ToneSpan/2;
  if(next>decBase+4096) {
    const size_t drop = std::min(next-decBase,dec.size());
    dec.erase(dec.begin(),dec.begin()+ptrdiff_t(drop));
    decBase += drop;
    }
  }

float LipSync::Track::yin(size_t at) const {
  const float* x = dec.data()+(at-decBase);
  float power = 0;
  for(size_t j=0; j<ToneWin; ++j)
    power += x[j]*x[j];
  if(power<ToneQuiet*float(ToneWin))
    return 0;
  // the difference of the window and the window a lag on (summed over the window in the outer loop: the lags are
  // independent sums and go in vectors), normalized by its mean over the shorter lags
  float d[TauMax+2] = {};
  for(size_t j=0; j<ToneWin; ++j) {
    const float  xj = x[j];
    const float* y  = x+j;
    for(size_t tau=1; tau<=TauMax+1; ++tau) {
      const float e = xj-y[tau];
      d[tau] += e*e;
      }
    }
  d[0] = 1;
  float sum = 0;
  for(size_t tau=1; tau<=TauMax+1; ++tau) {
    sum   += d[tau];
    d[tau] = sum>0 ? d[tau]*float(tau)/sum : 1.f;
    }
  // the first lag clearly periodic, down to the bottom of its dip; else none
  size_t tau = TauMin;
  while(tau<=TauMax && d[tau]>=ToneThr)
    ++tau;
  if(tau>TauMax)
    return 0;
  while(tau<TauMax && d[tau+1]<d[tau])
    ++tau;
  // between the lags: the bottom of the parabola through the three around it
  const float a = d[tau-1], b = d[tau], c = d[tau+1];
  const float k = a - 2.f*b + c;
  const float shift = k>0 ? std::clamp(0.5f*(a-c)/k, -1.f, 1.f) : 0.f;
  return float(ToneRate)/(float(tau)+shift);
  }

void LipSync::Track::finish() {
  std::lock_guard<std::mutex> guard(sync);
  if(accN>perStep/2)
    db.push_back(float(10.0*std::log10(acc/double(accN) + 1e-10)));
  acc      = 0;
  accN     = 0;
  finished = true;
  findTones(true);
  }

float LipSync::Track::toneHz(uint64_t ms) const {
  std::lock_guard<std::mutex> guard(sync);
  const size_t i = size_t(ms/StepMs);
  return i<hz.size() ? hz[i] : 0.f;
  }

float LipSync::Track::implMedian() const {
  if(medianOf!=toned) {
    std::vector<float> st;
    st.reserve(toned);
    for(auto f:hz)
      if(f>0)
        st.push_back(f);
    if(!st.empty()) {
      auto mid = st.begin()+ptrdiff_t(st.size()/2);
      std::nth_element(st.begin(),mid,st.end());
      medianSt = 12.f*std::log2(*mid);
      }
    medianOf = toned;
    }
  return medianSt;
  }

bool LipSync::Track::tone(uint64_t ms, float& semitones) const {
  std::lock_guard<std::mutex> guard(sync);
  const size_t i = size_t(ms/StepMs);
  if(i>=hz.size() || hz[i]<=0)
    return false;
  semitones = 12.f*std::log2(hz[i]) - implMedian();
  return true;
  }

uint64_t LipSync::Track::toneKnownMs() const {
  std::lock_guard<std::mutex> guard(sync);
  return hz.size()*StepMs;
  }

void LipSync::Track::setName(std::string_view n) {
  std::lock_guard<std::mutex> guard(sync);
  lineName = std::string(n);
  }

std::string LipSync::Track::name() const {
  std::lock_guard<std::mutex> guard(sync);
  return lineName;
  }

bool LipSync::Track::isOf(std::string_view line) const {
  if(line.empty())
    return false;
  const std::string n = name();
  const auto same = [](std::string_view a, std::string_view b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char x, char y) {
      return std::tolower(uint8_t(x))==std::tolower(uint8_t(y));
      });
    };
  const std::string_view v = n;
  if(same(v,line))
    return true;
  return v.size()==line.size()+4 && same(v.substr(0,line.size()),line) && same(v.substr(line.size()),".WAV");
  }

float LipSync::Track::implLevel(size_t step) const {
  if(step>=db.size())
    return 0;
  const float ref = std::max(peakDb, MinPeak);
  const float lo  = ref - Range;
  const float hi  = ref - std::min(Top, Range-1.f);
  const float v   = std::clamp((db[step]-lo)/(hi-lo), 0.f, 1.f);
  return std::pow(v, Gamma);
  }

float LipSync::Track::level(uint64_t ms) const {
  std::lock_guard<std::mutex> guard(sync);
  const size_t i = size_t(ms/StepMs);
  const float  k = float(ms%StepMs)/float(StepMs);
  return implLevel(i)*(1.f-k) + implLevel(i+1)*k;
  }

float LipSync::Track::peak(uint64_t from, uint64_t to) const {
  std::lock_guard<std::mutex> guard(sync);
  float ret = 0;
  for(size_t i=size_t(from/StepMs); i<=size_t(to/StepMs) && i<db.size(); ++i)
    ret = std::max(ret, implLevel(i));
  return ret;
  }

uint64_t LipSync::Track::durationMs() const {
  std::lock_guard<std::mutex> guard(sync);
  return db.size()*StepMs;
  }

bool LipSync::Track::quietRun(uint64_t from, float below, uint64_t minMs, uint64_t& start, uint64_t& end) const {
  std::lock_guard<std::mutex> guard(sync);
  size_t i = size_t(from/StepMs);
  while(i<db.size() && implLevel(i)<below)
    ++i;
  while(i<db.size()) {
    while(i<db.size() && implLevel(i)>=below)
      ++i;
    const size_t q = i;
    while(i<db.size() && implLevel(i)<below)
      ++i;
    if(i>q && uint64_t(i-q)*StepMs>=minMs) {
      start = q*StepMs;
      end   = i*StepMs;
      return true;
      }
    }
  return false;
  }

bool LipSync::Track::isFinished() const {
  std::lock_guard<std::mutex> guard(sync);
  return finished;
  }


void LipSync::setShapes(std::vector<float> o) {
  if(o==open)
    return;  // the same head: the mouth goes on from the shape it shows
  open = std::move(o);
  cur  = 0;
  prev = 0;
  if(open.empty())
    active = false;
  }

void LipSync::start(std::shared_ptr<Track> t, uint64_t now) {
  if(!enabled || open.empty() || t==nullptr)
    return;
  track     = std::move(t);
  startTick = now;
  active    = true;
  inDip     = true;
  dipMin    = lvl;
  sylMax    = 0;
  picks     = 0;
  rnd       = 0x9E3779B9u;  // the same line moves the mouth the same way every time
  }

void LipSync::stop() {
  track = nullptr;
  }

uint64_t LipSync::lineTime(uint64_t now) const {
  const int64_t played = track->playedMs();
  if(played>=0)
    return uint64_t(played);
  return now>startTick ? now-startTick : 0;
  }

uint64_t LipSync::voiceLeft(uint64_t now) const {
  if(track==nullptr || !track->isFinished())
    return 0;
  const uint64_t t   = lineTime(now);
  const uint64_t len = track->durationMs();
  return len>t ? len-t : 0;
  }

bool LipSync::isSpeaking(uint64_t now) const {
  if(track==nullptr)
    return false;
  const uint64_t t   = lineTime(now);
  const uint64_t len = track->durationMs();
  if(!track->isFinished())
    return t < len + 5000;  // a voice that stopped arriving does not hold the mouth forever
  if(track->playedMs()>=0)
    return t+StepMs < len;  // a streamed voice is played up to its end and no further (its last step may be partial)
  return t < len + 100;
  }

void LipSync::pick(float need, uint64_t now) {
  need = std::max(need, MinOpen);
  // the shapes that open about as wide as the syllable needs, the closest first
  uint32_t cand[3] = {};
  float    dist[3] = {1e9f, 1e9f, 1e9f};
  size_t   n = 0;
  uint32_t widest = 0;
  for(uint32_t i=0; i<open.size(); ++i) {
    if(open[i]>open[widest])
      widest = i;
    if(open[i]<MinOpen || open[i]<need*Fit)
      continue;
    if(i==cur && open.size()>3)
      continue;  // another shape than the last syllable's
    const float d = std::abs(open[i]-need);
    for(size_t k=0; k<3; ++k) {
      if(d>=dist[k])
        continue;
      for(size_t j=2; j>k; --j) {
        cand[j] = cand[j-1];
        dist[j] = dist[j-1];
        }
      cand[k] = i;
      dist[k] = d;
      n = std::min<size_t>(n+1, 3);
      break;
      }
    }

  rnd ^= rnd<<13;
  rnd ^= rnd>>17;
  rnd ^= rnd<<5;
  const uint32_t next = n>0 ? cand[rnd%n] : widest;

  prev     = blend<0.5f ? prev : cur;  // from what is shown more now
  cur      = next;
  sylPeak  = need;
  blend    = (prev==cur) ? 1.f : 0.f;
  lastPick = now;
  picks++;
  }

bool LipSync::update(uint64_t now, uint64_t dt, Shape& out) {
  if(!active)
    return false;
  if(open.empty()) {
    active = false;
    return false;
    }

  const uint64_t lead = track!=nullptr && track->isLiveInput() ? 0 : uint64_t(std::max(0.f,Lead));
  float target = 0;
  if(track!=nullptr) {
    time = lineTime(now);
    const uint64_t t = time + lead;
    if(track->isFinished() && t>track->durationMs()+lead)
      track = nullptr;  // the line is over
    else if(track->playedMs()!=0)
      target = track->level(t);  // (a streamed voice that has not started playing yet: the mouth waits)
    if(track!=nullptr && !isSpeaking(now))
      track = nullptr;
    }
  lvl = target;

  // syllables: the voice rises out of a dip
  if(!inDip) {
    sylMax = std::max(sylMax, lvl);
    if(lvl<sylMax*Dip) {
      inDip  = true;
      dipMin = lvl;
      }
    } else {
    dipMin = std::min(dipMin, lvl);
    if(lvl>dipMin+Rise) {
      inDip  = false;
      sylMax = lvl;
      ++syllables;
      const uint64_t t = time + lead;
      sylLoud = track!=nullptr ? track->peak(t, t+uint64_t(std::max(0.f,Look))) : lvl;
      pick(sylLoud, now);
      }
    }

  if(dt>0)
    mouth = smooth(mouth, lvl, dt, lvl>mouth ? Attack : Release);
  // louder than the syllable looked: a shape for that
  if(!inDip && mouth>sylPeak*1.2f && now>=lastPick+uint64_t(std::max(0.f,Blend))) {
    const uint64_t t = time + lead;
    pick(std::max(mouth, track!=nullptr ? track->peak(t, t+uint64_t(std::max(0.f,Look))) : 0.f), now);
    }
  if(blend<1.f && dt>0)
    blend = Blend>0 ? std::min(1.f, blend + float(dt)/Blend) : 1.f;

  // the shape is shown fully at the loudest moment of the syllable and follows the voice below it
  const float o = open[prev] + (open[cur]-open[prev])*blend;
  shown.frame0    = prev;
  shown.frame1    = cur;
  shown.alpha     = blend;
  shown.intensity = std::min(1.f, mouth/std::max(sylPeak, 0.05f));
  if(track==nullptr && mouth<0.01f) {
    mouth           = 0;
    shown.intensity = 0;
    active          = false;  // closed: this is the last frame to show
    }
  shownOpen = o*shown.intensity;
  out = shown;
  return true;
  }

void LipSync::probe(Probe& p) const {
  p.valid  = !open.empty();
  p.active = active;
  p.level  = lvl;
  p.tone   = std::nanf("");
  if(track!=nullptr && !track->tone(time,p.tone))
    p.tone = std::nanf("");
  p.mouth  = mouth;
  p.open   = shownOpen;
  p.shape  = shown;
  p.picks  = picks;
  p.time   = time;
  }
