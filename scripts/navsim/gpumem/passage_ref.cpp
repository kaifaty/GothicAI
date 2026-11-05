// The unmodified engine replay and NavigationPassage share the same View.
// Including the reference keeps its private Replay type in this translation
// unit, without exposing or changing production engine code.
#include "../../../OpenGothic/tests/navsim_memory_ref.cpp"
#include "game/navigationpassage.h"

extern "C" {
void navref_passage(void* handle, const float* segments, int64_t count, uint8_t* verdicts) {
  const auto& view=static_cast<Replay*>(handle)->view;
  for(int64_t q=0;q<count;++q) {
    const float* s=segments+8*q;
    verdicts[q]=uint8_t(NavigationPassage::check(view,{s[0],s[1],s[2]},{s[3],s[4],s[5]},s[6],s[7]));
    }
  }

// Case generation only: retain actual hazard kinds from the complete layers,
// including far cells behind the walker, which the policy input crops away.
void navref_passage_view(void* handle, float* pose, uint8_t* flags) {
  const auto& view=static_cast<Replay*>(handle)->view;
  const auto& p=view.pose;
  const float values[]={p.position.x,p.position.y,p.position.z,p.forward.x,p.forward.y,p.forward.z};
  std::copy_n(values,6,pose);
  int64_t offset=0;
  const auto nonzero=[](float v) { return std::lround(std::clamp(v,0.f,1.f)*255.f)!=0; };
  for(const auto* layer:{&view.near,&view.far})
    for(const auto& c:layer->cells) {
      uint8_t value=0;
      if(!nonzero(c.familiar)) {
        if(nonzero(c.wall)) value|=1;
        if(nonzero(c.ledge)) value|=2;
        if(nonzero(c.drop)) value|=4;
        if(nonzero(c.water)) value|=8;
        if(NavigationPassage::seenFree(c)) value|=16;
        }
      flags[offset++]=value;
      }
  }
}
