#include "fateend.h"

#include <Tempest/Log>
#include <Tempest/Painter>
#include <Tempest/Pixmap>
#include <Tempest/UiOverlay>

#include <algorithm>
#include <cmath>

#include "game/gamesession.h"
#include "utils/gthfont.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"
#include "resources.h"
#include "testbench.h"
#include "textcodec.h"

using namespace Tempest;

namespace {

constexpr uint64_t DeadMs     = 4000;   // the hero lies dead before the screen goes black
constexpr uint64_t FadeMs     = 1500;   // to black
constexpr uint64_t AnswerMs   = 30000;  // the chronicler has not written by then: no card
constexpr uint64_t CardMs     = 1500;   // the card comes up
constexpr uint64_t KeyAfterMs = 1500;   // after the card is up, a key ends the game
constexpr size_t   MaxFeats   = 3;
constexpr auto     FontName   = "font_old_20_white.tga";

const GthFont& font(Resources::FontType type, float px) {
  const float base = float(std::max(1,Resources::font(FontName,Resources::FontType::Normal,1.f).pixelSize()));
  return Resources::font(FontName,type,px/base);
  }

std::string sinceDay(const FateCard& c) {
  auto s = TextCodec::fromUtf8("Глава ") + std::to_string(c.chapter) +
           TextCodec::fromUtf8(", день ") + std::to_string(c.day);
  if(c.count>0)
    s += TextCodec::fromUtf8(". В летописи: ") + std::to_string(c.count);
  return s;
  }

}

// black, the chronicler's card over the whole screen; it takes the keys while it is up
class FateEnd::Screen final : public Tempest::UiOverlay {
  public:
    void setCard(const FateCard& c) {
      card = c;
      if(card.feats.size()>MaxFeats)
        card.feats.resize(MaxFeats);
      pictures.clear();
      for(auto& f:card.feats) {
        std::unique_ptr<Texture2d> tex;
        for(auto* path:{&f.engraving,&f.icon}) {
          if(path->empty() || tex!=nullptr)
            continue;
          try {
            tex = std::make_unique<Texture2d>(Resources::loadTexturePm(Pixmap(path->c_str())));
            }
          catch(...) {
            Log::e("[fate] can't load the picture ",*path);
            }
          }
        pictures.push_back(std::move(tex));
        }
      footer = sinceDay(card);
      waiting.clear();
      update();
      }
    void setWaiting() {
      waiting = TextCodec::fromUtf8("Летописец подводит итог...");
      update();
      }
    void set(float b, float c, float p, float k) {
      if(b==black && c==shownCard && p==pulse && k==keyHint)
        return;
      black     = b;
      shownCard = c;
      pulse     = p;
      keyHint   = k;
      update();
      }
    bool shown() const { return black>0.f; }
    std::function<void()> onKey;

  protected:
    void paintEvent(PaintEvent& e) override {
      if(black<=0.f)
        return;
      Painter pt(e);
      pt.setBrush(Color(0.f,0.f,0.f,black));
      pt.drawRect(0,0,w(),h());
      if(black<1.f)
        return;

      // the words by the screen, not by the interface: as large on a phone-shaped window as on a monitor
      const float side = float(std::min(w(),h()));
      if(shownCard<=0.f) {
        if(!waiting.empty()) {
          auto& f = font(Resources::FontType::Disabled,side/34.f);
          f.drawText(pt,0,h()/2,w(),0,waiting,AlignHCenter);
          veil(pt,0,h()/2-f.pixelSize(),w(),2*f.pixelSize(),pulse);
          }
        return;
        }

      auto& big     = font(Resources::FontType::Normal,  side/13.f);
      auto& caption = font(Resources::FontType::Hi,      side/38.f);
      auto& line    = font(Resources::FontType::Normal,  side/26.f);
      auto& dim     = font(Resources::FontType::Disabled,side/40.f);
      const int pad = big.pixelSize()/2;

      // the engravings in a row, as large as three of them take across the screen
      const int n       = int(pictures.size());
      const int gap     = pad;
      const int picSide = n==0 ? 0 : std::min(int(side*0.27f), (w()*4/5-gap*(n-1))/n);
      int capH = 0;
      for(auto& f:card.feats)
        capH = std::max(capH,caption.textSize(picSide,f.title).h);
      const int lineW   = std::min(w()-4*pad,int(side*1.15f));
      const int titleH  = card.title.empty() ? 0 : big.textSize(w()-4*pad,card.title).h;
      const int textH   = card.text.empty()  ? 0 : line.textSize(lineW,card.text).h;
      const int rowH    = n==0 ? 0 : picSide+pad/2+capH;
      const int total   = titleH + (titleH>0 ? pad*2 : 0) + rowH + (rowH>0 ? pad*2 : 0) + textH + pad*2 + dim.pixelSize();

      int y = std::max(pad,(h()-total)/2);
      if(titleH>0) {
        big.drawText(pt,2*pad,y+big.pixelSize(),w()-4*pad,titleH,card.title,AlignHCenter);
        y += titleH+pad*2;
        }
      if(n>0) {
        int x = (w()-(n*picSide+(n-1)*gap))/2;
        for(int i=0; i<n; ++i) {
          if(auto& tex = pictures[size_t(i)]) {
            pt.setBrush(Brush(*tex,Color(1.f),PaintDevice::Alpha,ClampMode::ClampToEdge));
            pt.drawRect(x,y,picSide,picSide,0,0,tex->w(),tex->h());
            } else {
            pt.setBrush(Color(0.85f,0.78f,0.62f,1.f));  // the parchment of a picture not drawn
            pt.drawRect(x,y,picSide,picSide);
            }
          caption.drawText(pt,x,y+picSide+pad/2+caption.pixelSize(),picSide,capH,card.feats[size_t(i)].title,AlignHCenter);
          x += picSide+gap;
          }
        y += rowH+pad*2;
        }
      if(textH>0) {
        line.drawText(pt,(w()-lineW)/2,y+line.pixelSize(),lineW,textH,card.text,AlignHCenter);
        y += textH+pad*2;
        }
      dim.drawText(pt,0,y+dim.pixelSize(),w(),0,footer,AlignHCenter);

      // the card comes up out of the black: a veil over it thins
      veil(pt,0,0,w(),h(),1.f-shownCard);
      if(keyHint>0.f) {
        auto& hint = font(Resources::FontType::Disabled,side/50.f);
        const auto txt = TextCodec::fromUtf8("Любая клавиша - главное меню");
        hint.drawText(pt,0,h()-hint.pixelSize(),w(),0,txt,AlignHCenter);
        veil(pt,0,h()-2*hint.pixelSize(),w(),2*hint.pixelSize(),1.f-keyHint);
        }
      }
    void keyDownEvent(KeyEvent& e) override {
      if(!shown()) {
        e.ignore();
        return;
        }
      if(onKey)
        onKey();
      }
    void keyUpEvent(KeyEvent& e) override {
      if(!shown())
        e.ignore();
      }
    void mouseDownEvent(MouseEvent& e) override {
      if(!shown()) {
        e.ignore();
        return;
        }
      if(onKey)
        onKey();
      }

  private:
    static void veil(Painter& pt, int x, int y, int vw, int vh, float a) {
      if(a<=0.f)
        return;
      pt.setBrush(Color(0.f,0.f,0.f,std::min(1.f,a)));
      pt.drawRect(x,y,vw,vh);
      }

    float                                   black     = 0.f;
    float                                   shownCard = 0.f;
    float                                   pulse     = 0.f;
    float                                   keyHint   = 0.f;
    FateCard                                card;
    std::vector<std::unique_ptr<Texture2d>> pictures;
    std::string                             footer, waiting;
  };

FateEnd::FateEnd() = default;

FateEnd::~FateEnd() = default;  // the overlay leaves the window by itself

void FateEnd::tick(World& world, bool busy) {
  const uint64_t now = world.tickCount();
  const uint64_t in  = now-since;
  switch(stage) {
    case Stage::Idle: {
      Npc* hero = world.player();
      if(busy || hero==nullptr || !hero->isDead())
        return;
      stage = Stage::Dead;
      since = now;
      return;
      }
    case Stage::Dead: {
      Npc* hero = world.player();
      if(busy || hero==nullptr || !hero->isDead()) {
        stage = Stage::Idle;  // up again (a script, the console), or another ending took him
        return;
        }
      if(in<DeadMs)
        return;
      std::string killer, name;
      if(Npc* by = hero->lastHitBy(); by!=nullptr && by!=hero) {
        if(auto sym = world.script().findSymbol(by->instanceSymbol()))
          killer = sym->name();
        name = std::string(by->displayName());
        }
      exits = false;
      request(world,"death",std::move(killer),std::move(name));
      return;
      }
    case Stage::Asked: {
      const float b = std::min(1.f,float(in)/float(FadeMs));
      const float p = 0.35f+0.35f*std::sin(float(in)/400.f);
      screen->set(b,0.f,p,0.f);
      if(in>=AnswerMs) {
        Log::i("[fate] the chronicler has not answered: no card");
        off = true;
        end();
        }
      return;
      }
    case Stage::Card: {
      const float c = std::min(1.f,float(in)/float(CardMs));
      const float k = in<CardMs+KeyAfterMs ? 0.f : std::min(1.f,float(in-CardMs-KeyAfterMs)/float(CardMs));
      screen->set(1.f,c,0.f,k);
      return;
      }
    case Stage::Done:
      return;
    }
  }

void FateEnd::finish(World& world, std::string_view k, bool black) {
  exits = true;
  request(world,std::string(k),{},{});
  if(black && screen!=nullptr) {
    since -= FadeMs;  // no fade: it is black already
    screen->set(1.f,0.f,0.f,0.f);
    }
  }

void FateEnd::request(World& world, std::string k, std::string killer, std::string name) {
  kind  = std::move(k);
  stage = Stage::Asked;
  since = world.tickCount();
  off   = false;
  card  = FateCard();
  if(screen==nullptr) {
    screen = std::make_unique<Screen>();
    screen->onKey = [this]() {
      if(stage!=Stage::Card || TestBench::isActive())
        return;
      auto* w = Gothic::inst().world();
      if(w==nullptr || w->tickCount()-since < CardMs+KeyAfterMs)
        return;
      Log::i("[fate] the end: back to the main menu");
      clear();
      if(auto s = Gothic::inst().gameSession())
        s->exitSession();
      };
    SystemApi::addOverlay(screen.get());
    }
  screen->setWaiting();
  Gothic::inst().setHudHidden(true);
  Log::i("[fate] ",kind,": asking the chronicler");
  const uint64_t g = ++gen;
  if(!ask) {
    answer(g,FateCard());
    return;
    }
  ask(kind,std::move(killer),std::move(name),[this,g](const FateCard& c) { answer(g,c); });
  }

void FateEnd::answer(uint64_t g, const FateCard& c) {
  if(g!=gen || stage!=Stage::Asked)
    return;
  if(c.off) {
    Log::i("[fate] ",kind,": no card");
    off = true;
    end();
    return;
    }
  card  = c;
  stage = Stage::Card;
  if(auto* w = Gothic::inst().world())
    since = w->tickCount();
  screen->setCard(card);
  Log::i("[fate] ",kind,": the card, ",card.feats.size()," deeds");
  }

void FateEnd::end() {
  // no card: a death goes on as the game has it (the menu, a load); the other endings end the game
  stage = Stage::Done;
  if(screen!=nullptr && !exits) {
    screen->set(0.f,0.f,0.f,0.f);
    Gothic::inst().setHudHidden(false);
    }
  if(exits) {
    clear();
    if(auto s = Gothic::inst().gameSession())
      s->exitSession();
    }
  }

void FateEnd::clear() {
  if(stage!=Stage::Idle)
    Gothic::inst().setHudHidden(false);
  stage = Stage::Idle;
  ++gen;
  if(screen!=nullptr)
    screen->set(0.f,0.f,0.f,0.f);
  }

bool FateEnd::reached(std::string_view name) const {
  if(name=="off")
    return off;
  static constexpr std::pair<std::string_view,Stage> names[] = {
    {"dead",Stage::Dead}, {"asked",Stage::Asked}, {"card",Stage::Card},
    };
  for(auto& [n,s]:names)
    if(n==name)
      return stage!=Stage::Idle && stage!=Stage::Done && stage>=s;
  return false;
  }

std::string FateEnd::status() const {
  static constexpr const char* names[] = {"idle","dead","asked","card","done"};
  auto s = std::string(names[size_t(stage)]);
  if(!kind.empty())
    s += " (" + kind + ")";
  if(off)
    s += ", off";
  return s;
  }
