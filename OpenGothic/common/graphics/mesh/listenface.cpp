#include "listenface.h"

#include <cstdlib>

namespace {
bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_LISTEN_FACE");
  return v==nullptr || v[0]!='0';
  }();
}

bool ListenFace::isEnabled() {
  return enabled;
  }

void ListenFace::setEnabled(bool e) {
  enabled = e;
  }

std::string_view ListenFace::name(React r) {
  switch(r) {
    case React::None:       return "none";
    case React::Flash:      return "flash";
    case React::Smile:      return "smile";
    case React::SmileFlash: return "smile+flash";
    case React::Frown:      return "frown";
    }
  return "none";
  }

float ListenFace::random() {
  if(rnd==0)
    seed(1);
  rnd ^= rnd<<13;
  rnd ^= rnd>>17;
  rnd ^= rnd<<5;
  return float(rnd>>8)/float(1u<<24);
  }

ListenFace::React ListenFace::nod(const FaceProfile::Listen& l, TalkGestures::Mood mood, bool busy, uint64_t now) {
  ++nodCount;
  // the face is taken, or too soon after the last one: the nod goes alone, nothing drawn by chance
  if(busy || (any && float(now-last)<l.gapMs))
    return React::None;
  if(random()>=l.chance)
    return React::None;
  React r = React::Flash;
  if(mood==TalkGestures::Mood::Angry || mood==TalkGestures::Mood::Hostile)
    r = React::Frown;  // no smile, no flash
  else if(mood==TalkGestures::Mood::Friendly && random()<l.smileChance)
    r = random()<l.smileFlash ? React::SmileFlash : React::Smile;
  last = now;
  any  = true;
  ++faceCount;
  return r;
  }

void ListenFace::play(FaceRig& face, React r, uint64_t now) {
  using K = FaceRig::ListenKind;
  if(r==React::Flash || r==React::SmileFlash)
    face.startListen(K::Flash,now,now);
  if(r==React::Smile || r==React::SmileFlash)
    face.startListen(K::Smile,now,now);
  if(r==React::Frown)
    face.startListen(K::Frown,now,now);
  }
