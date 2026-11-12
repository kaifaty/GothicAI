#include "speechbrows.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace {
constexpr int64_t StepMs = 10;  // of LipSync::Track

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_SPEECH_BROWS");
  return v==nullptr || v[0]!='0';
  }();
}

bool SpeechBrows::isEnabled() {
  return enabled;
  }

void SpeechBrows::setEnabled(bool e) {
  enabled = e;
  }

void SpeechBrows::start(std::shared_ptr<LipSync::Track> t, uint32_t seed, uint64_t now) {
  // a new line: what was found of the last one goes, the mood of a line stays (its voice may be this one)
  std::string        line = std::move(moodLine);
  TalkGestures::Mood mood = lineMood;
  *this     = SpeechBrows();
  moodLine  = std::move(line);
  lineMood  = mood;
  track     = std::move(t);
  startTick = now;
  rnd       = seed*2654435761u | 1u;
  }

void SpeechBrows::stop() {
  track = nullptr;
  }

void SpeechBrows::setMood(TalkGestures::Mood m, std::string_view line) {
  moodLine = std::string(line);
  lineMood = m;
  }

bool SpeechBrows::lowered() const {
  if(track==nullptr || moodLine.empty() || !lowers(lineMood))
    return false;
  return track->isOf(moodLine);
  }

uint32_t SpeechBrows::play(FaceRig& face, uint64_t now) {
  const auto* profile = face.faceProfile();
  if(profile==nullptr || !profile->brows.has_value())
    return 0;
  uint32_t ret = 0;
  Start    s;
  while(update(*profile->brows,now,s)) {
    face.startBrows(s.down,s.strength,s.at,now);
    ++ret;
    }
  return ret;
  }

uint64_t SpeechBrows::lineTime(uint64_t now) const {
  const int64_t played = track->playedMs();
  if(played>=0)
    return uint64_t(played);
  return now>startTick ? now-startTick : 0;
  }

float SpeechBrows::random() {
  rnd ^= rnd<<13;
  rnd ^= rnd>>17;
  rnd ^= rnd<<5;
  return float(rnd>>8)/float(1u<<24);
  }

void SpeechBrows::scan(const FaceProfile::Brows&) {
  // the syllables of what has arrived: the voice rises out of a dip (LipSync::update, by the steps of the track)
  float dip = 0, rise = 0;
  LipSync::syllableRule(dip,rise);
  const size_t steps = size_t(track->durationMs()/uint64_t(StepMs));
  for(; scanned<steps; ++scanned) {
    const float l = track->level(uint64_t(scanned)*uint64_t(StepMs));
    if(!inDip) {
      sylMax = std::max(sylMax,l);
      if(l<sylMax*dip) {
        inDip  = true;
        dipMin = l;
        }
      continue;
      }
    dipMin = std::min(dipMin,l);
    if(l>dipMin+rise) {
      inDip  = false;
      sylMax = l;
      syllables.push_back(int64_t(scanned)*StepMs);
      }
    }
  }

float SpeechBrows::riseAt(const FaceProfile::Brows& b, int64_t from, int64_t to) const {
  // the most the tone rises to a step of [from, to) from a step at most rise_ms before it, in semitones; a jump of
  // an octave is a mistake of the tone, not a rise
  const auto st = [&](int64_t ms, float& v) {
    const float hz = ms>=0 ? track->toneHz(uint64_t(ms)) : 0.f;
    if(hz<=0)
      return false;
    v = 12.f*std::log2(hz);
    return true;
    };
  const int64_t back = std::max<int64_t>(1,int64_t(b.riseMs)/StepMs);
  float ret = 0;
  for(int64_t bt=from; bt<to; bt+=StepMs) {
    float hi = 0;
    if(!st(bt,hi))
      continue;
    for(int64_t k=1; k<=back; ++k) {
      float lo = 0;
      if(st(bt-k*StepMs,lo) && hi-lo<OctaveSt)
        ret = std::max(ret,hi-lo);
      }
    }
  return ret;
  }

bool SpeechBrows::judge(const FaceProfile::Brows& b, int64_t syllable) {
  // its rise and loudness need the tone and the sound of the syllable_ms after its start
  const int64_t end = syllable+int64_t(b.syllableMs);
  if(int64_t(track->toneKnownMs())<end && !track->isFinished())
    return false;
  Accent a;
  a.ms   = syllable;
  a.rise = riseAt(b,syllable,end);
  a.loud = track->peak(uint64_t(syllable),uint64_t(end));
  if(a.rise<b.riseSt || a.loud<b.loud)
    return true;
  // past the gap after the last move, by chance
  const int64_t at = std::max<int64_t>(0,syllable-int64_t(b.leadMs));
  if(!anyMove || at-lastMove>=int64_t(b.gapMs)) {
    a.moves = random()<b.chance;
    if(a.moves) {
      lastMove = at;
      anyMove  = true;
      }
    }
  found.push_back(a);
  return true;
  }

bool SpeechBrows::update(const FaceProfile::Brows& b, uint64_t now, Start& out) {
  if(track==nullptr)
    return false;
  scan(b);
  while(judged<syllables.size() && judge(b,syllables[judged]))
    ++judged;
  if(track->playedMs()==0)
    return false;  // a streamed voice that has not started playing yet
  const int64_t t = int64_t(lineTime(now));
  for(; next<found.size(); ++next) {
    auto& a = found[next];
    if(!a.moves)
      continue;
    const int64_t at = std::max<int64_t>(0,a.ms-int64_t(b.leadMs));
    if(t<at)
      return false;
    const int64_t late = t-at;
    if(late<=int64_t(LateMs))
      out.at = now-std::min<uint64_t>(now,uint64_t(late));
    else if(t<=a.ms)
      out.at = now;  // found late (the voice came late): it starts now, still before its syllable
    else
      continue;      // its syllable has gone by
    // a larger rise moves the brows more: from half of the weights at rise_st to all of them at twice that
    out.strength = std::clamp(a.rise/(2.f*b.riseSt),0.5f,1.f);
    out.down     = lowered();  // the mood as known now: a line's mood may come after its voice
    a.shown      = true;
    ++moved;
    ++next;
    return true;
    }
  return false;
  }
