#include "speechcues.h"

#include "facerig.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

namespace {

bool isSpace(char c) {
  // the separators of Python's str.split() that a game codepage line can hold (0xA0: a no-break space)
  return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\v' || c=='\f' || uint8_t(c)==0xA0;
  }

int64_t ms(float s) {
  return int64_t(std::llround(double(s)*1000.0));
  }

}

SpeechCues::Plan::Plan(std::string_view text) {
  chars = uint32_t(text.size());
  for(size_t i=0; i<text.size(); ++i)
    if(!isSpace(text[i]) && (i==0 || isSpace(text[i-1])))
      wordChar.push_back(uint32_t(i));
  words.resize(wordChar.size());
  }

void SpeechCues::Plan::setWord(size_t index, float start, float end) {
  if(index>=words.size() || !std::isfinite(start) || !std::isfinite(end) || start<0 || end<start)
    return;
  words[index] = {start,end};
  }

uint32_t SpeechCues::seedOf(std::string_view s) {
  uint32_t h = 2166136261u;
  for(char c:s) {
    h ^= uint8_t(c);
    h *= 16777619u;
    }
  return h;
  }

void SpeechCues::Out::clear() {
  cues.clear();
  leak      = false;
  leakFelt.clear();
  leakLower = false;
  leakAt    = 0;
  blinks    = 0;
  reserve.clear();
  posed     = false;
  }

void SpeechCues::start(std::shared_ptr<const Plan> p, bool voiced, uint64_t now) {
  plan     = std::move(p);
  planTick = now;
  bound    = false;
  fired.assign(plan!=nullptr ? plan->cues.size() : 0,0);
  leakFired     = false;
  leakStart     = -1;
  blinksPlanned = false;
  blinks.clear();
  blinksFired   = 0;
  if(plan==nullptr)
    return;
  if(!voiced) {
    // no sound: the time of the line from now
    track     = nullptr;
    trackUsed = true;
    bound     = true;
    return;
    }
  // its voice has just started (a streamed line starts it first)
  if(track!=nullptr && !trackUsed && now-trackTick<=BindMs) {
    trackUsed = true;
    bound     = true;
    }
  }

void SpeechCues::voice(std::shared_ptr<LipSync::Track> t, uint64_t now) {
  track     = std::move(t);
  trackTick = now;
  trackUsed = false;
  if(plan==nullptr)
    return;
  if(!bound && now-planTick<=BindMs) {
    // the voice of the line that has just started (a line in the dialog starts it right after)
    trackUsed = true;
    bound     = true;
    return;
    }
  // another voice: the line before is over or cut, its gestures not started yet are not
  plan = nullptr;
  }

void SpeechCues::stop() {
  plan  = nullptr;
  track = nullptr;
  }

uint64_t SpeechCues::lineTime(uint64_t now) const {
  if(track==nullptr)
    return now>planTick ? now-planTick : 0;
  const int64_t played = track->playedMs();
  if(played>=0)
    return uint64_t(played);
  return now>trackTick ? now-trackTick : 0;
  }

uint64_t SpeechCues::heardMs() const {
  return track!=nullptr ? track->durationMs() : plan->silentMs;
  }

bool SpeechCues::complete() const {
  return track==nullptr || track->isFinished();
  }

int64_t SpeechCues::wordMs(size_t word) const {
  if(plan==nullptr || word>=plan->words.size())
    return -1;
  if(plan->words[word].start>=0)
    return ms(plan->words[word].start);
  // an estimate by its characters
  const uint64_t heard = heardMs();
  const uint64_t at    = plan->wordChar[word];
  if(plan->streamed && track!=nullptr)
    return int64_t(std::min<uint64_t>(uint64_t(std::llround(double(at)*1000.0/double(CharsPerS))),heard));
  if(!complete() || plan->chars==0)
    return -1;
  return int64_t(heard*at/plan->chars);
  }

bool SpeechCues::fits(int64_t from, int64_t to, float delay, float on, float hold, float off, Slot& s) const {
  // cues.py fit_mouth: the pause widened, the hold shortened to it
  const int64_t a    = from-int64_t(PauseBeforeMs);
  const int64_t b    = to  +int64_t(PauseAfterMs);
  const float   room = float(b-a)-delay-on-off;
  if(room<MinHoldMs)
    return false;
  s.start = std::max<int64_t>(0,a);
  s.limit = b;
  s.hold  = std::min(hold,room);
  return true;
  }

SpeechCues::Slot SpeechCues::mouthSlot(const Plan::Cue& c, float delay, float on, float hold, float off) const {
  Slot best;
  const int64_t w = wordMs(c.word);
  if(w<0)
    return best;
  // the first gap between two words after its word
  const auto& words = plan->words;
  for(size_t j=c.word; j+1<words.size(); ++j) {
    if(words[j].end<0 || words[j+1].start<0)
      continue;
    const int64_t s = ms(words[j].end), e = ms(words[j+1].start);
    if(s>=w && e-s>=int64_t(PauseMs) && fits(s,e,delay,on,hold,off,best))
      break;
    }
  // or the voice quiet as long, if it comes first
  if(track!=nullptr) {
    uint64_t from = uint64_t(w), s = 0, e = 0;
    for(int k=0; k<64 && track->quietRun(from,QuietLevel,PauseMs,s,e); ++k) {
      if(best.start>=0 && int64_t(s)-int64_t(PauseBeforeMs)>=best.start)
        break;
      Slot q;
      if(fits(int64_t(s),int64_t(e),delay,on,hold,off,q)) {
        best = q;
        break;
        }
      from = e;
      }
    }
  if(best.start>=0)
    return best;
  // no pause until the end of the sound: right after it
  if(complete()) {
    best.start = int64_t(heardMs());
    best.hold  = -1;
    }
  return best;
  }

void SpeechCues::planBlinks(int64_t from, int64_t lastBlink, float leakMs) {
  // at random by the seed of the npc and the line, in the sound from now on: away from each other, from the ordinary
  // blink and from the leak (cues.py: the blinks of a lying segment)
  blinks.clear();
  const int64_t lo = from;
  const int64_t hi = int64_t(heardMs())-int64_t(BlinkMs);
  const int64_t a  = leakStart-int64_t(LeakClearMs);
  const int64_t b  = leakStart+int64_t(std::lround(leakMs))+int64_t(LeakClearMs);
  const auto ok = [&](int64_t x) {
    if(x<lo || x>hi || (x+int64_t(BlinkMs)>a && x<b))
      return false;
    if(std::abs(x-lastBlink)<int64_t(BlinkGapMs))
      return false;
    for(auto y:blinks)
      if(std::abs(x-y)<int64_t(BlinkGapMs))
        return false;
    return true;
    };
  uint32_t r = plan->seed*2654435761u | 1u;
  for(int i=0; i<200 && blinks.size()<LiarBlinks && hi>=lo; ++i) {
    r ^= r<<13;
    r ^= r>>17;
    r ^= r<<5;
    const int64_t x = lo+int64_t(r%uint32_t(hi-lo+1));
    if(ok(x))
      blinks.push_back(x);
    }
  // a short line: the first places that are left, if any
  for(int64_t x=lo; x<=hi && blinks.size()<LiarBlinks; x+=10)
    if(ok(x))
      blinks.push_back(x);
  std::sort(blinks.begin(),blinks.end());
  }

bool SpeechCues::updateLeak(const FaceProfile& profile, uint64_t now, uint64_t t, uint64_t lastBlink, Out& out) {
  if(!plan->leak.has_value())
    return true;
  const auto& l    = *plan->leak;
  bool        done = true;
  if(!leakFired) {
    // as an upper gesture: a moment before its word
    const int64_t w = wordMs(l.word);
    const int64_t s = w>=0 ? std::max<int64_t>(0,w-int64_t(UpperLeadMs)) : -1;
    if(s<0 || int64_t(t)<s) {
      done = false;
      } else {
      const uint64_t late = t-uint64_t(s);
      const uint64_t back = late<=LateMs ? std::min(now,late) : 0;
      leakFired = true;
      leakStart = int64_t(t-back);
      if(profile.findLeak(l.felt,l.lower)!=nullptr) {
        out.leak      = true;
        out.leakFelt  = l.felt;
        out.leakLower = l.lower;
        out.leakAt    = now-back;
        }
      }
    }
  // the liar's blinks, once the time of the leak is sure (it started, or its word has its own time) and the length
  // of the sound is known
  if(!blinksPlanned) {
    const bool sure = leakFired || (l.word<plan->words.size() && plan->words[l.word].start>=0);
    if(!sure || !complete())
      return false;
    if(!leakFired)
      leakStart = std::max<int64_t>(0,wordMs(l.word)-int64_t(UpperLeadMs));
    const int64_t last = lastBlink==0 ? INT64_MIN/2 : int64_t(t)+(int64_t(lastBlink)-int64_t(now));
    planBlinks(int64_t(t),last,profile.leaks.empty() ? 0.f : profile.leakTime.span());
    blinksPlanned = true;
    for(auto b:blinks)
      out.reserve.push_back(now+uint64_t(b-int64_t(t)));
    }
  while(blinksFired<blinks.size() && int64_t(t)>=blinks[blinksFired]) {
    ++out.blinks;
    ++blinksFired;
    }
  return done && blinksFired==blinks.size();
  }

void SpeechCues::update(const FaceProfile& profile, uint64_t now, uint64_t lastBlink, Out& out) {
  if(plan==nullptr)
    return;
  out.posed = plan->posed();
  if(!bound) {
    if(now-planTick<=BindMs)
      return;
    // the voice never came: the line's time on screen
    track = nullptr;
    bound = true;
    }
  if(track!=nullptr && track->playedMs()==0)
    return;  // a streamed voice that has not started playing yet
  const uint64_t t    = lineTime(now);
  bool           done = true;
  for(size_t i=0; i<plan->cues.size(); ++i) {
    if(fired[i])
      continue;
    const auto& c   = plan->cues[i];
    const auto* cue = profile.findCue(c.name);
    if(cue==nullptr || cue->parts.empty()) {
      fired[i] = 1;  // not in the dictionary of this head
      continue;
      }
    const auto& p = cue->parts[0];
    Slot s;
    if(cue->mouth) {
      s = mouthSlot(c,p.delayMs,p.onMs,p.holdMs,p.offMs);
      } else {
      const int64_t w = wordMs(c.word);
      if(w>=0)
        s.start = std::max<int64_t>(0,w-int64_t(UpperLeadMs));
      }
    if(s.start<0 || int64_t(t)<s.start) {
      done = false;
      continue;
      }
    Start st;
    st.name   = cue->name;
    st.mirror = cue->side!=0 && c.side!=0 && c.side!=cue->side;
    st.hold   = s.hold;
    const uint64_t late = t-uint64_t(s.start);
    if(late<=LateMs) {
      st.at = now-std::min(now,late);
      } else {
      // found late (its words came late): it starts now, a mouth one still ends in its pause; its hold no longer
      // than the slot's and no shorter than MinHoldMs, or than its own hold if that is shorter still
      st.at = now;
      if(s.limit>=0) {
        const float hi = s.hold>=0 ? s.hold : p.holdMs;
        const float lo = std::min(MinHoldMs,hi);
        st.hold = std::clamp(float(s.limit-int64_t(t))-p.delayMs-p.onMs-p.offMs,lo,hi);
        }
      }
    out.cues.push_back(st);
    fired[i] = 1;
    }
  if(!updateLeak(profile,now,t,lastBlink,out))
    done = false;
  // a posed smile lasts as long as the line
  if(plan->posed() && !(complete() && t>=heardMs()))
    done = false;
  if(done)
    plan = nullptr;
  }
