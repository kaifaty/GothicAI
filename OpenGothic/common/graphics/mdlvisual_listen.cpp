#include "mdlvisual.h"

#include "world/objects/npc.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>

using namespace Tempest;

void MdlVisual::listenNod(Npc& npc, uint64_t now, bool speaking) {
  const auto* profile = face.faceProfile();
  if(!ListenFace::isEnabled() || profile==nullptr || !profile->listen.has_value())
    return;
  if(!listenFace.isSeeded())
    listenFace.seed(SpeechCues::seedOf("listen")^(uint32_t(npc.instanceSymbol())*2654435761u));
  // the mood it listens in: its own last line's while that holds, else its attitude to the one speaking
  const auto mood = now<lineMoodUntil ? lineMood : heardMood;
  const auto r    = listenFace.nod(*profile->listen,mood,speaking || face.listenBusy(now),now);
  ListenFace::play(face,r,now);
  if(&npc==FaceRig::probeNpc) {
    const auto m = TalkGestures::moodName(mood);
    const auto n = ListenFace::name(r);
    Log::i("[face] listen ",npc.displayName(),": ",r==ListenFace::React::None ? "nod" : n," (",m,")");
    }
  }

int32_t MdlVisual::voiceAccents(std::string_view line, const FaceProfile* listener, uint64_t now) {
  if(voiceNow==nullptr || line.empty())
    return -1;
  // the track of the line: its id, or the id with ".WAV" (case aside, as the game names sounds)
  const std::string name = voiceNow->name();
  const auto same = [](std::string_view a, std::string_view b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char x, char y) {
      return std::tolower(uint8_t(x))==std::tolower(uint8_t(y));
      });
    };
  const std::string_view n = name;
  if(!same(n,line) && !(n.size()==line.size()+4 && same(n.substr(0,line.size()),line) && same(n.substr(line.size()),".WAV")))
    return -1;

  // its own brows find them as it speaks (updateBrows); else here by the rule of the one it says it to
  const SpeechBrows* found = &voiceBrows;
  if(!voiceOwnBrows) {
    if(!heardRule.has_value()) {
      if(listener==nullptr || !listener->brows.has_value())
        return -1;
      heardRule = *listener->brows;
      heardBrows.start(voiceNow,SpeechCues::seedOf(name),voiceAt);
      }
    SpeechBrows::Start s;
    while(heardBrows.update(*heardRule,now,s))
      ;
    found = &heardBrows;
    }
  // reached: the line played up to its syllable
  const int64_t played = voiceNow->playedMs();
  const int64_t t      = played>=0 ? played : int64_t(now>voiceAt ? now-voiceAt : 0);
  int32_t ret = 0;
  for(auto& a:found->accents())
    if(a.ms<=t)
      ++ret;
  return ret;
  }
