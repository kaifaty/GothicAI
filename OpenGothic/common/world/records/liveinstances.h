#pragma once

#include "recordinstance.h"
#include "npcrecord.h"
#include "itemrecord.h"

#include <string>
#include <vector>

struct NpcInstance : RecordInstance, NpcRecord {
  static void register_(daedalus::DaedalusScript& script);
  };

struct ItemInstance : RecordInstance, ItemRecord {
  static void register_(daedalus::DaedalusScript& script);
  };

struct DialogChoiceRecord {
  std::string text;
  int32_t function = 0;
  };

struct DialogInstance : RecordInstance {
  int32_t npc;
  int32_t nr;
  int32_t important;
  int32_t condition;
  int32_t information;
  std::string description;
  int32_t trade;
  int32_t permanent;

  std::vector<DialogChoiceRecord> choices{};

  void add_choice(const DialogChoiceRecord& choice) { choices.insert(choices.begin(),choice); }
  void remove_choice(size_t index) { choices.erase(choices.begin()+static_cast<decltype(choices)::difference_type>(index)); }

  static void register_(daedalus::DaedalusScript& script);
  };
