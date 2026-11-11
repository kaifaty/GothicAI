#include "talkgestures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

float Tail      = 500.f;   // ms: the voice is over this long (and no line waits): the hands rest
float Wait      = 400.f;   // ms: a gesture due waits this long at most for a syllable to start on
float MinLeft   = 1200.f;  // ms: with less of the line left no gesture starts
float Slack     = 600.f;   // ms: a gesture longer than the line left by more than this is unlikely...
float Long      = 0.15f;   // ...this many times as likely as one that fits
float RestScale = 1.f;     // of the rests between gestures
// the head
float Nod       = 5.f;     // degrees down on a stressed syllable (the loudest ones; less on the others)
float NodHz     = 2.5f;    // the nod is down in 1/(2 pi NodHz) s, back in about four times that
float Stress    = 1.1f;    // a syllable this many times as loud as the ones before it is stressed
float NodGap    = 350.f;   // ms, nods at most this often
float SwayYaw   = 2.5f;    // degrees aside, at most, while the voice lasts
float SwayRoll  = 2.f;
float SwayTime  = 1300.f;  // ms, a new sway this often (+-40%)...
float SwayLag   = 450.f;   // ...reached with this time constant
// listening and thinking
float ListenSway = 0.6f;   // of the sway while it listens (0.4 while it thinks)
float ListenNod  = 4.f;    // degrees, a nod as the words of the other one come...
float ListenHz   = 1.6f;   // ...slower than one on a stressed syllable
float ListenGap  = 1500.f; // ms, such nods at most this often...
float ListenProb = 0.45f;  // ...and with this chance when words come
float DoubleNod  = 0.25f;  // the chance of a second nod right after (350 ms)
float ThinkDelay = 350.f;  // ms after it starts to think the eyes go aside...
float ThinkYaw   = 18.f;   // ...this far (degrees, to a side at random)...
float ThinkPitch = 8.f;    // ...and up
float ThinkTilt  = 5.f;    // degrees the head tilts while it thinks
float AsideTime  = 250.f;  // ms, the time constant of the eyes going aside and back

struct Param final {
  std::string_view name;
  float*           value;
  };

const Param params[] = {
  {"Tail", &Tail}, {"Wait", &Wait}, {"MinLeft", &MinLeft}, {"Slack", &Slack}, {"Long", &Long},
  {"RestScale", &RestScale}, {"Nod", &Nod}, {"NodHz", &NodHz}, {"Stress", &Stress}, {"NodGap", &NodGap},
  {"SwayYaw", &SwayYaw}, {"SwayRoll", &SwayRoll}, {"SwayTime", &SwayTime}, {"SwayLag", &SwayLag},
  {"ListenSway", &ListenSway}, {"ListenNod", &ListenNod}, {"ListenHz", &ListenHz}, {"ListenGap", &ListenGap},
  {"ListenProb", &ListenProb}, {"DoubleNod", &DoubleNod}, {"ThinkDelay", &ThinkDelay}, {"ThinkYaw", &ThinkYaw},
  {"ThinkPitch", &ThinkPitch}, {"ThinkTilt", &ThinkTilt}, {"AsideTime", &AsideTime},
  };

constexpr float Deg = 3.14159265f/180.f;

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_GESTURES");
  return v==nullptr || v[0]!='0';
  }();

// How lively the Gothic II gestures are, by how fast the arms turn (mean over the gesture, the
// fastest tenth): 06 is 9.6 s of slow hands and head, 07 and 09 are 6 s, 08 and 10 have quick jabs
constexpr uint8_t G2Kind[10] = {
  0 /*01 calm*/, 1 /*02 lively*/, 1 /*03 lively*/, 0 /*04 calm*/, 1 /*05 lively*/,
  0 /*06 calm*/, 0 /*07 calm*/,   2 /*08 sharp*/,  0 /*09 calm*/, 2 /*10 sharp*/,
  };

struct MoodStyle final {
  float    weight[3];  // of a calm, a lively and a sharp gesture
  uint32_t restMin, restMax;  // ms between two gestures
  };

// neutral, friendly, angry, hostile, afraid, pain (no gestures)
constexpr MoodStyle Styles[] = {
  {{5,4,1},  400, 1600},
  {{3,6,1},  300, 1100},
  {{1,4,5},    0,  500},
  {{2,3,5},  200,  900},
  {{8,2,0},  900, 2600},
  {{0,0,0},    0,    0},
  };

}

TalkGestures::Probe TalkGestures::subjectProbe;
const void*         TalkGestures::probeNpc = nullptr;

bool TalkGestures::isEnabled() {
  return enabled;
  }

void TalkGestures::setEnabled(bool e) {
  enabled = e;
  }

bool TalkGestures::setParam(std::string_view name, float value) {
  for(auto& p:params)
    if(p.name==name) {
      *p.value = value;
      return true;
      }
  return false;
  }

std::string TalkGestures::paramList() {
  std::string ret;
  for(auto& p:params) {
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s%.*s=%g",ret.empty() ? "" : " ",int(p.name.size()),p.name.data(),double(*p.value));
    ret += buf;
    }
  return ret;
  }

TalkGestures::Mood TalkGestures::moodOf(std::string_view m) {
  if(m=="friendly") return Mood::Friendly;
  if(m=="angry")    return Mood::Angry;
  if(m=="hostile")  return Mood::Hostile;
  if(m=="afraid")   return Mood::Afraid;
  if(m=="pain")     return Mood::Pain;
  return Mood::Neutral;
  }

std::string_view TalkGestures::moodName(Mood m) {
  switch(m) {
    case Mood::Neutral:  return "neutral";
    case Mood::Friendly: return "friendly";
    case Mood::Angry:    return "angry";
    case Mood::Hostile:  return "hostile";
    case Mood::Afraid:   return "afraid";
    case Mood::Pain:     return "pain";
    }
  return "neutral";
  }

void TalkGestures::setGestures(std::vector<uint32_t> durationMs) {
  duration = std::move(durationMs);
  kind.resize(duration.size());
  for(size_t i=0; i<kind.size(); ++i)
    kind[i] = (i<std::size(G2Kind)) ? G2Kind[i] : Lively;
  }

void TalkGestures::lineStart(uint64_t now) {
  if(!engaged && current==0)
    nextAt = now;  // the first gesture starts with the voice
  engaged    = true;
  lastSpeech = now;
  }

void TalkGestures::stop() {
  if(current!=0)
    ++stops;
  engaged = false;
  current = 0;
  mood    = Mood::Neutral;
  }

TalkGestures::Decision TalkGestures::update(const Input& in) {
  Decision d;
  speaking = in.speaking;
  if(!engaged) {
    if(!in.playing)
      current = 0;
    return d;
    }

  if(in.speaking || in.more)
    lastSpeech = in.now;
  const bool over = in.now > lastSpeech + uint64_t(std::max(0.f,Tail));
  if(!in.allowed || over || mood==Mood::Pain) {
    if(in.playing && current!=0) {
      d.act = Act::Stop;
      ++stops;
      }
    current = 0;
    if(over) {
      engaged = false;
      mood    = Mood::Neutral;
      }
    return d;
    }

  if(in.playing && current!=0)
    return d;
  if(current!=0) {
    // it has played to its end: the hands rest a moment
    current = 0;
    nextAt  = in.now + rest();
    }
  if(!in.speaking || in.now<nextAt)
    return d;
  // a moment for a syllable to start on: the stroke of the hand goes with the voice
  if(in.voiced && !in.syllable && in.now<nextAt+uint64_t(std::max(0.f,Wait)))
    return d;
  // too little of the line left: the next line, if one comes, starts a gesture of its own
  if(in.left>0 && float(in.left)<MinLeft)
    return d;

  const uint8_t g = pick(in.left,in.more);
  if(g==0)
    return d;
  d.act     = Act::Start;
  d.gesture = g;
  current   = g;
  recent[1] = recent[0];
  recent[0] = g;
  ++starts;
  return d;
  }

void TalkGestures::heard(uint64_t now) {
  if(!enabled || now<lastHeardNod+uint64_t(std::max(0.f,ListenGap)))
    return;
  if(float(random()%1000)>=ListenProb*1000.f)
    return;
  lastHeardNod = now;
  wantNod      = 0.5f + 0.5f*float(random()%1000)/1000.f;
  if(float(random()%1000)<DoubleNod*1000.f)
    secondNod = now + 350;
  }

void TalkGestures::think(uint64_t now, uint64_t until) {
  if(until==0) {
    thinkUntil = std::min(thinkUntil,now);
    return;
    }
  if(now>=thinkUntil) {
    // a new thought: to a side at random
    thinkFrom = now;
    tiltSign  = (random()%2)==0 ? 1.f : -1.f;
    }
  thinkUntil = until;
  }

void TalkGestures::updateHead(uint64_t now, uint64_t dt, bool speak, bool syllable, float loud) {
  lastNow = now;
  const bool  listening = enabled && !speak && now<listenUntil;
  const bool  thinking  = enabled && !speak && now<thinkUntil;
  const bool  on        = enabled && (speak || listening || thinking);
  const float t         = float(dt)/1000.f;
  const float swayW     = speak ? 1.f : (listening ? ListenSway : 0.4f);
  talkW = on ? std::min(swayW, talkW + t/0.3f) : std::max(0.f, talkW - t/0.5f);
  if(talkW>swayW)
    talkW = std::max(swayW, talkW - t/0.5f);

  // a stressed syllable: clearly louder than the ones before it in this line
  sinceNod += dt;
  if(enabled && speak && syllable && loud>0.f) {
    if(sylMean>0.f && loud>sylMean*Stress && float(sinceNod)>=NodGap) {
      const float s = std::clamp((loud/sylMean - 1.f)/0.5f, 0.3f, 1.f);
      nodHz     = NodHz;
      nodSpeed -= Nod*Deg*s*2.f*3.14159265f*nodHz*2.7182818f;  // a spring kicked so: its largest turn is speed/(w e)
      sinceNod  = 0;
      ++nods;
      }
    sylMean = sylMean>0.f ? sylMean + (loud-sylMean)*0.3f : loud;
    }
  if(!speak)
    sylMean = 0;

  // listening: a nod as the words come (asked for by heard), sometimes two
  if(secondNod!=0 && now>=secondNod) {
    secondNod = 0;
    if(listening)
      wantNod = std::max(wantNod,0.6f);
    }
  if(wantNod>0.f) {
    if(listening) {
      nodHz     = ListenHz;
      nodSpeed -= ListenNod*Deg*wantNod*2.f*3.14159265f*nodHz*2.7182818f;
      sinceNod  = 0;
      ++listenNods;
      }
    wantNod = 0;
    }

  // the nod: x(t) = (x0 + (v0 + w x0) t) e^(-w t)
  if(t>0.f) {
    const float w  = 2.f*3.14159265f*nodHz;
    const float c  = nodSpeed + w*nod;
    const float e  = std::exp(-w*t);
    const float x  = (nod + c*t)*e;
    nodSpeed = (c - w*(nod + c*t))*e;
    nod      = x;
    if(std::abs(nod)<1e-4f && std::abs(nodSpeed)<1e-3f)
      nod = nodSpeed = 0;
    }

  // the sway: now and then somewhere else aside, slowly
  if(on && swayLeft<=dt) {
    const float a = float(random()%2001)/1000.f - 1.f;
    const float b = float(random()%2001)/1000.f - 1.f;
    swayGoal[0] = a*SwayYaw*Deg;
    swayGoal[1] = b*SwayRoll*Deg;
    swayLeft    = uint64_t(SwayTime*(0.6f + 0.8f*float(random()%1001)/1000.f));
    } else {
    swayLeft = swayLeft>dt ? swayLeft-dt : 0;
    }
  const float k = SwayLag>0 ? 1.f - std::exp(-float(dt)/SwayLag) : 1.f;
  for(int i=0; i<2; ++i) {
    sway[i] += ((on ? swayGoal[i] : 0.f) - sway[i])*k;
    if(!on && std::abs(sway[i])<1e-4f)
      sway[i] = 0;
    }

  // thinking: a moment, then the eyes go aside and up, the head tilts; back when it answers
  const bool  away = thinking && now>=thinkFrom+uint64_t(std::max(0.f,ThinkDelay));
  const float ka   = AsideTime>0 ? 1.f - std::exp(-float(dt)/AsideTime) : 1.f;
  aside.weight += ((away ? 1.f : 0.f) - aside.weight)*ka;
  if(!away && aside.weight<0.01f)
    aside.weight = 0;
  aside.yaw   = tiltSign*ThinkYaw*Deg;
  aside.pitch = ThinkPitch*Deg;
  tilt        = tiltSign*ThinkTilt*Deg*aside.weight;

  headNow.pitch = nod;
  headNow.yaw   = sway[0]*talkW;
  headNow.roll  = sway[1]*talkW + tilt;
  headMoves     = talkW>0.f || nod!=0.f || nodSpeed!=0.f || sway[0]!=0.f || sway[1]!=0.f || aside.weight>0.f;
  }

void TalkGestures::started(uint8_t gesture, bool overPlaying) {
  current = gesture;
  ++starts;
  if(overPlaying)
    ++cuts;
  }

uint8_t TalkGestures::pick(uint64_t left, bool more) {
  const auto& st = Styles[size_t(mood)];
  // the weight of a kind is shared by its gestures: five calm ones are not five times as likely
  float n[3] = {};
  for(size_t i=0; i<duration.size(); ++i)
    if(duration[i]>0)
      n[kind[i]] += 1.f;
  float w[256] = {};
  float sum    = 0;
  for(size_t i=0; i<duration.size() && i<255; ++i) {
    const uint8_t g = uint8_t(i+1);
    if(duration[i]==0 || g==recent[0] || g==recent[1])
      continue;
    float wi = st.weight[kind[i]]/n[kind[i]];
    if(!more && left>0 && float(duration[i])>float(left)+Slack)
      wi *= Long;  // it would be cut off
    w[i] = wi;
    sum += wi;
    }
  if(sum<=0.f)
    return 0;
  float r = float(random()%10000)/10000.f*sum;
  for(size_t i=0; i<duration.size() && i<255; ++i) {
    if(w[i]<=0.f)
      continue;
    if(r<w[i])
      return uint8_t(i+1);
    r -= w[i];
    }
  for(size_t i=duration.size(); i>0; --i)
    if(w[i-1]>0.f)
      return uint8_t(i);
  return 0;
  }

uint64_t TalkGestures::rest() {
  const auto&    st = Styles[size_t(mood)];
  const uint32_t r  = st.restMin + (st.restMax>st.restMin ? random()%(st.restMax-st.restMin) : 0);
  return uint64_t(float(r)*std::max(0.f,RestScale));
  }

uint32_t TalkGestures::random() {
  if(rnd==0) {
    // npcs that start talking in the same order pick the same gestures each run (the test bench)
    static uint32_t seed = 0;
    rnd = (++seed)*2654435761u | 1u;
    }
  // xorshift: an LCG stepped a few times per pick gave runs of short gaps
  rnd ^= rnd<<13;
  rnd ^= rnd>>17;
  rnd ^= rnd<<5;
  return rnd;
  }

void TalkGestures::probe(Probe& p) const {
  p.valid    = !duration.empty() || current!=0;
  p.enabled  = enabled;
  p.engaged  = engaged;
  p.speaking = speaking;
  p.gesture  = current;
  p.mood     = mood;
  p.starts   = starts;
  p.cuts     = cuts;
  p.stops    = stops;
  p.nods     = nods;
  p.listenNods = listenNods;
  p.listening  = lastNow<listenUntil;
  p.thinking   = lastNow<thinkUntil;
  p.head     = headNow;
  p.aside    = aside;
  }
