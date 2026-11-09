#include "speechcues.h"

#include "facerig.h"

#include <algorithm>
#include <cmath>

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

void SpeechCues::start(std::shared_ptr<const Plan> p, bool voiced, uint64_t now) {
  plan     = std::move(p);
  planTick = now;
  bound    = false;
  fired.assign(plan!=nullptr ? plan->cues.size() : 0,0);
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

void SpeechCues::update(const FaceProfile& profile, uint64_t now, std::vector<Start>& out) {
  if(plan==nullptr)
    return;
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
    out.push_back(st);
    fired[i] = 1;
    }
  if(done)
    plan = nullptr;
  }
