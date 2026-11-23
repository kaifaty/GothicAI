#pragma once

#include <cstdint>
#include <string>

namespace MenuDefinitions {
enum class MenuFlag : std::uint32_t {
  OVERTOP = 1 << 0,
  EXCLUSIVE = 1 << 1,
  NO_ANIMATION = 1 << 2,
  DONT_SCALE_DIMENSION = 1 << 3,
  DONT_SCALE_POSITION = 1 << 4,
  ALIGN_CENTER = 1 << 5,
  SHOW_INFO = 1 << 6,

  overtop = OVERTOP,
  exclusive = EXCLUSIVE,
  no_animation = NO_ANIMATION,
  dont_scale_dimension = DONT_SCALE_DIMENSION,
  dont_scale_position = DONT_SCALE_POSITION,
  align_center = ALIGN_CENTER,
  show_info = SHOW_INFO,
};
inline MenuFlag operator|(MenuFlag a, MenuFlag b) {
  return MenuFlag(uint32_t(a)|uint32_t(b));
  }
inline bool operator&(MenuFlag a, MenuFlag b) {
  return (uint32_t(a)&uint32_t(b))!=0;
  }

struct Menu {
  static constexpr std::uint8_t item_count = 150;

  std::string back_pic;
  std::string back_world;
  int32_t pos_x;
  int32_t pos_y;
  int32_t dim_x;
  int32_t dim_y;
  int32_t alpha;
  std::string music_theme;
  int32_t event_timer_msec;
  std::string items[item_count];
  MenuFlag flags;
  int32_t default_outgame;
  int32_t default_ingame;

  bool operator==(const Menu&) const = default;
};

enum class MenuItemFlag : std::uint32_t {
  CHROMAKEYED = 1 << 0,
  TRANSPARENT = 1 << 1,
  SELECTABLE = 1 << 2,
  MOVABLE = 1 << 3,
  CENTERED = 1 << 4,
  DISABLED = 1 << 5,
  FADE = 1 << 6,
  EFFECTS = 1 << 7,
  ONLY_OUTGAME = 1 << 8,
  ONLY_INGAME = 1 << 9,
  PERF_OPTION = 1 << 10,
  MULTILINE = 1 << 11,
  NEEDS_APPLY = 1 << 12,
  NEEDS_RESTART = 1 << 13,
  EXTENDED_MENU = 1 << 14,
  HOR_SELECTABLE = 1 << 15,

  chromakeyed = CHROMAKEYED,
  transparent = TRANSPARENT,
  selectable = SELECTABLE,
  movable = MOVABLE,
  centered = CENTERED,
  disabled = DISABLED,
  fade = FADE,
  effects = EFFECTS,
  only_outgame = ONLY_OUTGAME,
  only_ingame = ONLY_INGAME,
  perf_option = PERF_OPTION,
  multiline = MULTILINE,
  needs_apply = NEEDS_APPLY,
  needs_restart = NEEDS_RESTART,
  extended_menu = EXTENDED_MENU,
  hor_selectable = HOR_SELECTABLE,
};
inline MenuItemFlag operator|(MenuItemFlag a, MenuItemFlag b) {
  return MenuItemFlag(uint32_t(a)|uint32_t(b));
  }
inline bool operator&(MenuItemFlag a, MenuItemFlag b) {
  return (uint32_t(a)&uint32_t(b))!=0;
  }

enum class MenuItemType : std::uint32_t {
  UNKNOWN = 0,
  TEXT = 1,
  SLIDER = 2,
  INPUT = 3,
  CURSOR = 4,
  CHOICEBOX = 5,
  BUTTON = 6,
  LISTBOX = 7,

  unknown = UNKNOWN,
  text = TEXT,
  slider = SLIDER,
  input = INPUT,
  cursor = CURSOR,
  choicebox = CHOICEBOX,
  button = BUTTON,
  listbox = LISTBOX,
};

enum class MenuItemSelectEvent : uint8_t {
  EXECUTE = 1,
  CHANGED = 2,
  LEAVE = 3,
  TIMER = 4,
  CLOSE = 5,
  INIT = 6,
  SELECT_PREVIOUS = 7,
  SELECT_NEXT = 8,

  execute = EXECUTE,
  changed = CHANGED,
  leave = LEAVE,
  timer = TIMER,
  close = CLOSE,
  init = INIT,
  select_previous = SELECT_PREVIOUS,
  select_next = SELECT_NEXT,
};

enum class MenuItemSelectAction : uint8_t {
  UNKNOWN = 0,
  BACK = 1,
  START_MENU = 2,
  START_ITEM = 3,
  CLOSE = 4,
  CON_COMMANDS = 5,
  PLAY_SOUND = 6,
  EXECUTE_COMMANDS = 7,

  unknown = UNKNOWN,
  back = BACK,
  start_menu = START_MENU,
  start_item = START_ITEM,
  close = CLOSE,
  con_commands = CON_COMMANDS,
  play_sound = PLAY_SOUND,
  execute_commands = EXECUTE_COMMANDS,
};

struct MenuItem {
  static constexpr std::uint32_t text_count = 10;
  static constexpr std::uint32_t select_action_count = 5;
  static constexpr std::uint32_t event_action_count = 10;
  static constexpr std::uint32_t user_item_count = 4;

  std::string fontname;
  std::string text[text_count];
  std::string backpic;
  std::string alphamode;
  int32_t alpha;
  MenuItemType type;
  int32_t on_sel_action[select_action_count];
  std::string on_sel_action_s[select_action_count];
  std::string on_chg_set_option;
  std::string on_chg_set_option_section;
  int32_t on_event_action[event_action_count];
  int32_t pos_x;
  int32_t pos_y;
  int32_t dim_x;
  int32_t dim_y;
  float size_start_scale;
  MenuItemFlag flags;
  float open_delay_time;
  float open_duration;
  float user_float[user_item_count];
  std::string user_string[user_item_count];
  int32_t frame_posx;
  int32_t frame_posy;
  int32_t frame_sizex;
  int32_t frame_sizey;
  std::string hide_if_option_section_set;
  std::string hide_if_option_set;
  int32_t hide_on_value;

  bool operator==(const MenuItem&) const = default;
};
}
