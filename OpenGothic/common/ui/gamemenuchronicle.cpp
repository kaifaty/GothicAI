#include "gamemenu.h"

#include <Tempest/Dialog>
#include <Tempest/Log>
#include <Tempest/Painter>
#include <Tempest/Pixmap>

#include <algorithm>
#include <utility>

#include "ui/menuroot.h"
#include "utils/gthfont.h"
#include "gothic.h"
#include "resources.h"

using namespace Tempest;

// The «Летопись» tab of the diary (MENU_LOG): the hero's feats of this branch and his other fates by
// save fork (docs/achievements.md). The game scripts know nothing of it: the tab and its list are copies
// of the diary's own «Общее» items, added when the diary opens. Texts are in the game codepage (cp1251).

namespace {
constexpr float       scriptDiv     = 8192.0f;  // the scripts' menu units, as in gamemenu.cpp
constexpr const char* SelChronicle  = "MENU_ITEM_SEL_CHRONICLE";
constexpr const char* ListChronicle = "MENU_ITEM_LIST_CHRONICLE";
constexpr const char* Chronicle     = "CHRONICLE";

constexpr const char* TabTitle  = "\xcb\xe5\xf2\xee\xef\xe8\xf1\xfc";  // Летопись
constexpr const char* TabInfo   = "\xcf\xee\xf1\xf2\xf3\xef\xea\xe8 \xe3\xe5\xf0\xee\xff \xe8 \xe5\xe3\xee "
                                  "\xe4\xf0\xf3\xe3\xe8\xe5 \xf1\xf3\xe4\xfc\xe1\xfb";  // Поступки героя и его другие судьбы
constexpr const char* Others    = "\xc4\xf0\xf3\xe3\xe8\xe5 \xf1\xf3\xe4\xfc\xe1\xfb";  // Другие судьбы
constexpr const char* Chapter   = "\xc3\xeb\xe0\xe2\xe0 ";  // Глава
constexpr const char* Day       = ", \xe4\xe5\xed\xfc ";  // , день
constexpr const char* Empty     = "\xcb\xe5\xf2\xee\xef\xe8\xf1\xfc \xef\xee\xea\xe0 \xef\xf3\xf1\xf2\xe0.";  // Летопись пока пуста.
constexpr const char* OtherFate = "\xc4\xf0\xf3\xe3\xe0\xff \xf1\xf3\xe4\xfc\xe1\xe0: \xe3\xeb\xe0\xe2\xe0 ";  // Другая судьба: глава

std::string when(const char* prefix, int64_t chapter, int64_t day) {
  return std::string(prefix) + std::to_string(chapter) + Day + std::to_string(day);
  }
}

GameMenu::BenchDiary GameMenu::benchDiary;

struct GameMenu::ChronicleRow {
  const ChronicleFeat* feat  = nullptr;  // a feat, or a header line when null
  const ChronicleFork* fork  = nullptr;  // the fork of another fate's feat, or of its header
  std::string          title;
  };

struct GameMenu::ChronicleDialog : Dialog {
  ChronicleDialog(GameMenu& owner, Item& list):owner(owner), list(list) {
    setFocusPolicy(ClickFocus);
    setCursorShape(CursorShape::Hidden);
    setFocus(true);
    }

  void mouseDownEvent(MouseEvent& e) override {
    if(e.button==Event::ButtonLeft)
      toggle();
    if(e.button==Event::ButtonRight)
      back();
    }

  void mouseWheelEvent(MouseEvent& e) override { onMove(-e.delta); }
  void keyDownEvent   (KeyEvent& e)   override { e.accept(); }
  void keyRepeatEvent (KeyEvent& e)   override { keyUpEvent(e); }

  void keyUpEvent(KeyEvent& e) override {
    if(e.key==Event::K_Return)
      toggle();
    else if(e.key==Event::K_ESCAPE)
      back();
    else if(e.key==Event::K_W || e.key==Event::K_Up)
      onMove(-1);
    else if(e.key==Event::K_S || e.key==Event::K_Down)
      onMove(+1);
    }

  void toggle() {
    owner.chronicleOpen = owner.chronicleOpen ? false : true;
    owner.update();
    }

  void back() {
    if(owner.chronicleOpen) {
      owner.chronicleOpen = false;
      owner.update();
      return;
      }
    close();
    }

  void onMove(int dy) {
    if(owner.chronicleOpen)
      return;
    const int32_t num = owner.chronicleFeatCount();
    list.value = std::clamp(list.value+dy, 0, std::max(num-1,0));
    owner.update();
    }

  void paintEvent (PaintEvent&) override {}
  void paintShadow(PaintEvent&) override {}

  GameMenu& owner;
  Item&     list;
  };

bool GameMenu::isChronicle(const Item& it) {
  return it.handle!=nullptr && it.handle->type==zenkit::MenuItemType::LISTBOX && it.handle->user_string[0]==Chronicle;
  }

bool GameMenu::isChronicleSlot(const Item& it) {
  return it.name==SelChronicle || it.name==ListChronicle;
  }

void GameMenu::addChronicleItems() {
  // the tab is a copy of «Общее» one step below it, the list a copy of the notes list
  auto find = [this](std::string_view name) -> Item* {
    for(auto& i:hItems)
      if(i.name==name && i.handle!=nullptr)
        return &i;
    return nullptr;
    };
  Item* selLog  = find("MENU_ITEM_SEL_LOG");
  Item* selPrev = find("MENU_ITEM_SEL_MISSIONS_FAILED");
  Item* listLog = find("MENU_ITEM_LIST_LOG");
  if(selLog==nullptr || selPrev==nullptr || listLog==nullptr) {
    Log::e("[chronicle] the diary has no «Общее» items: no chronicle tab");
    return;
    }
  // the tab goes into the first free slot after «Общее», so that the keys reach it right after it
  size_t selAt = size_t(selLog-hItems)+1, listAt = 0;
  while(selAt<zenkit::IMenu::item_count && !hItems[selAt].name.empty())
    ++selAt;
  for(listAt=selAt+1; listAt<zenkit::IMenu::item_count && !hItems[listAt].name.empty(); ++listAt)
    ;
  if(listAt>=zenkit::IMenu::item_count) {
    Log::e("[chronicle] the diary has no free slots: no chronicle tab");
    return;
    }

  // the date and time under the tabs move down by one tab to make room
  const int32_t step = selLog->handle->pos_y - selPrev->handle->pos_y;
  for(auto& i:hItems)
    if(i.handle!=nullptr && (i.name=="MENU_ITEM_DAY_TITLE" || i.name=="MENU_ITEM_DAY" ||
                             i.name=="MENU_ITEM_TIME_TITLE" || i.name=="MENU_ITEM_TIME"))
      i.handle->pos_y += step;

  auto sel = std::make_shared<zenkit::IMenuItem>(*selLog->handle);
  sel->text[0] = TabTitle;
  sel->text[1] = TabInfo;
  sel->pos_y  += step;
  for(auto& action:sel->on_sel_action_s)
    if(action.rfind("EFFECTS ",0)==0)
      action = std::string("EFFECTS ")+ListChronicle;
  for(auto& event:sel->on_event_action)
    event = 0;  // nothing of the scripts' own «Общее»
  hItems[selAt].name   = SelChronicle;
  hItems[selAt].handle = sel;
  hItems[selAt].img    = selLog->img;

  auto list = std::make_shared<zenkit::IMenuItem>(*listLog->handle);
  list->user_string[0] = Chronicle;
  hItems[listAt].name    = ListChronicle;
  hItems[listAt].handle  = list;
  hItems[listAt].img     = listLog->img;
  hItems[listAt].visible = false;
  }

std::vector<GameMenu::ChronicleRow> GameMenu::chronicleRows() const {
  const auto& book = Gothic::inst().chronicle();
  std::vector<ChronicleRow> rows;
  for(auto& f:book.fate)
    rows.push_back({&f,nullptr,f.title});
  bool others = false;
  for(auto& fork:book.others) {
    if(fork.feats.empty())
      continue;
    if(!others)
      rows.push_back({nullptr,nullptr,Others});
    others = true;
    rows.push_back({nullptr,&fork,when(Chapter,fork.chapter,fork.day)});
    for(auto& f:fork.feats)
      rows.push_back({&f,&fork,f.title});
    }
  return rows;
  }

int32_t GameMenu::chronicleFeatCount() const {
  const auto& book = Gothic::inst().chronicle();
  size_t num = book.fate.size();
  for(auto& fork:book.others)
    num += fork.feats.size();
  return int32_t(num);
  }

const Texture2d* GameMenu::chronicleImage(const std::string& path) {
  if(path.empty())
    return nullptr;
  auto found = chronicleImages.find(path);
  if(found!=chronicleImages.end())
    return found->second.get();
  std::unique_ptr<Texture2d> tex;
  try {
    tex = std::make_unique<Texture2d>(Resources::loadTexturePm(Pixmap(path.c_str())));
    }
  catch(...) {
    Log::e("[chronicle] can't load the picture ",path);
    }
  auto* ret = tex.get();
  chronicleImages[path] = std::move(tex);
  return ret;
  }

void GameMenu::drawChronicle(Painter& p, Item& it, int x, int y, int w, int h) {
  const float scale = Gothic::interfaceScale(this);
  auto& fntN = Resources::font(it.handle->fontname,Resources::FontType::Normal,  scale);
  auto& fntH = Resources::font(it.handle->fontname,Resources::FontType::Hi,      scale);
  auto& fntD = Resources::font(it.handle->fontname,Resources::FontType::Disabled,scale);
  const int line = fntN.pixelSize();

  const auto rows = chronicleRows();
  if(rows.empty()) {
    fntD.drawText(p,x,y+line,w,line,Empty,AlignLeft);
    return;
    }

  // the selected feat: its row and the fork it belongs to
  const ChronicleRow* selected = nullptr;
  int32_t selectedRow = 0;
  {
  int32_t id = -1;
  for(size_t i=0; i<rows.size(); ++i)
    if(rows[i].feat!=nullptr && ++id==it.value) {
      selected    = &rows[i];
      selectedRow = int32_t(i);
      break;
      }
  }
  const bool active = selectedItem()==&it;

  if(chronicleOpen && selected!=nullptr) {
    // one feat: the engraving, its title and line, the epilogue the hero has seen
    auto& f   = *selected->feat;
    const int side = std::min(h, w*2/5);
    if(auto* img = chronicleImage(f.engraving.empty() ? f.icon : f.engraving)) {
      p.setBrush(*img);
      p.drawRect(x,y,side,side,0,0,img->w(),img->h());
      }
    const int tx = x+side+line, tw = w-side-line;
    int ty = y;
    const auto titleH = fntH.textSize(tw,f.title).h;
    fntH.drawText(p,tx,ty+line,tw,titleH,f.title,AlignLeft);
    ty += titleH+line;
    if(selected->fork!=nullptr) {
      const auto fate  = when(OtherFate,selected->fork->chapter,selected->fork->day);
      const auto fateH = fntD.textSize(tw,fate).h;
      fntD.drawText(p,tx,ty+line,tw,fateH,fate,AlignLeft);
      ty += fateH+line;
      }
    const auto textH = fntN.textSize(tw,f.text).h;
    fntN.drawText(p,tx,ty+line,tw,textH,f.text,AlignLeft);
    ty += textH+line;
    if(!f.epilogue.empty()) {
      const auto epiH = fntD.textSize(tw,f.epilogue).h;
      fntD.drawText(p,tx,ty+line,tw,epiH,f.epilogue,AlignLeft);
      }
    return;
    }

  // the list: an icon and a title per feat, the other fates under their fork's chapter and day
  const int rowH = line*2;
  const int visible = std::max(h/rowH,1);
  if(selectedRow<it.scroll)
    it.scroll = selectedRow;
  if(selectedRow>=it.scroll+visible)
    it.scroll = selectedRow-visible+1;
  it.scroll = std::clamp(it.scroll,0,std::max(int32_t(rows.size())-visible,0));
  if(selectedRow>0 && rows[size_t(selectedRow-1)].feat==nullptr && it.scroll==selectedRow)
    it.scroll = std::max(it.scroll-1,0);  // keep a feat's fork header in view with it

  int itY = y;
  for(size_t i=size_t(it.scroll); i<rows.size() && itY+rowH<=y+h; ++i) {
    auto& r = rows[i];
    if(r.feat==nullptr) {
      auto& fnt = r.fork==nullptr ? fntN : fntD;
      fnt.drawText(p,x,itY+line+line/2,w,line,r.title,AlignLeft);
      itY += rowH;
      continue;
      }
    const int icon = rowH-line/4;
    int tx = x;
    if(auto* img = chronicleImage(r.feat->icon)) {
      p.setBrush(*img);
      p.drawRect(x,itY,icon,icon,0,0,img->w(),img->h());
      }
    tx += icon+line/2;
    auto& fnt = (&r==selected && active) ? fntH : fntN;
    fnt.drawText(p,tx,itY+line+line/2,w-(tx-x),line,r.title,AlignLeft);
    itY += rowH;
    }

  if(it.scroll+visible<int32_t(rows.size()) && down!=nullptr) {
    p.setBrush(*down);
    p.drawRect(x+w-down->w(),y+h-down->h(),down->w(),down->h());
    }
  if(it.scroll>0 && up!=nullptr) {
    p.setBrush(*up);
    p.drawRect(x+w-up->w(),y,up->w(),up->h());
    }
  }

void GameMenu::execChronicle(Item& list) {
  if(chronicleFeatCount()==0)
    return;
  const uint32_t prev = curItem;
  for(uint32_t i=0; i<zenkit::IMenu::item_count; ++i)
    if(&hItems[i]==&list)
      curItem = i;
  ChronicleDialog dlg(*this,list);
  dlg.resize(owner.size());
  dlg.exec();
  chronicleOpen = false;
  curItem = prev;
  }

void GameMenu::applyBenchDiary() {
  const auto preset = std::exchange(benchDiary,BenchDiary());
  for(uint32_t i=0; i<zenkit::IMenu::item_count; ++i)
    if(hItems[i].name==SelChronicle)
      setSelection(int(i));
  for(uint32_t i=0; i<zenkit::IMenu::item_count; ++i)
    if(hItems[i].name==ListChronicle && preset.feat>=0) {
      hItems[i].value = preset.feat;
      curItem         = i;
      chronicleOpen   = preset.open;
      }
  }

void GameMenu::drawChronicleFrame(Painter& p) {
  // The tabs' frames are drawn in the diary's background picture, four of them. The fifth gets the line
  // under «Общая информация» copied one tab lower: the same band of the picture, so it looks the same.
  if(back==nullptr)
    return;
  const Item* sel  = nullptr;
  const Item* prev = nullptr;
  const Item* list = nullptr;
  for(auto& i:hItems) {
    if(i.name==SelChronicle)
      sel = &i;
    if(i.name=="MENU_ITEM_SEL_LOG")
      prev = &i;
    if(i.name==ListChronicle)
      list = &i;
    }
  if(sel==nullptr || prev==nullptr || list==nullptr || sel->handle==nullptr || prev->handle==nullptr || list->handle==nullptr)
    return;
  // only the tabs' column: the content panel to the right has no line there
  const int right = int(float(w()*list->handle->pos_x)/scriptDiv);
  const int top  = int(float(h()*sel->handle->pos_y)/scriptDiv);
  const int step = top - int(float(h()*prev->handle->pos_y)/scriptDiv);
  // the line sits a little above the tab's top in the picture: take a band around it, the rest of the
  // band is the same plain background
  const int above = step/4, below = step/10;
  const int v0    = int(float(back->h())*float(top-above)/float(h()));
  const int v1    = int(float(back->h())*float(top+below)/float(h()));
  p.setBrush(*back);
  const int u1 = int(float(back->w())*float(right)/float(w()));
  p.drawRect(0,top+step-above,right,above+below, 0,v0,u1,v1);
  }
