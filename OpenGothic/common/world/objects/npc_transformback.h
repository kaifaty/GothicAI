#pragma once

#include "npc.h"

struct Npc::TransformBack {
  TransformBack(Npc& self);
  TransformBack(Npc& owner, daedalus::DaedalusVm& vm, Serialize& fin);
  void undo(Npc& self);
  void save(Serialize& fout);

  std::shared_ptr<daedalus::INpc>   hnpc={};
  Inventory                       invent;
  int32_t                         talentsSk[TALENT_MAX_G2]={};
  int32_t                         talentsVl[TALENT_MAX_G2]={};

  std::string                     body,head;
  int32_t                         vHead=0, vTeeth=0, vColor=0;
  int32_t                         bdColor=0;

  const Skeleton*                 skeleton = nullptr;
  };
