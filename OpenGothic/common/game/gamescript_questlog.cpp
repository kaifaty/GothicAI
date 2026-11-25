#include "gamescript.h"

#include <stdexcept>

void GameScript::givePlayerXp(int32_t amount) {
  const auto [self,other] = storyActors();
  const auto result = callStoryFunction("B_GIVEPLAYERXP",self,other,{amount});
  if(!result.ok)
    throw std::runtime_error("B_GIVEPLAYERXP: "+result.error);
  }
