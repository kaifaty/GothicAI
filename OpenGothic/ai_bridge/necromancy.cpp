#include "necromancy.h"

#include <Tempest/Log>
#include <Tempest/Painter>
#include <Tempest/Pixmap>

#include <algorithm>
#include <cstdlib>

#include "game/constants.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
#include "graphics/effect.h"
#include "graphics/mesh/skeleton.h"
#include "utils/gthfont.h"
#include "world/aiqueue.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"
#include "resources.h"
#include "testbench.h"

using namespace Tempest;

namespace {

constexpr uint64_t CastEvery   = 400;    // ms: the cast is tried again until the spell goes
constexpr uint64_t CastAgain   = 3000;   // ms after a cast that went: it missed, another one
constexpr uint64_t CastMax     = 7000;   // ms: the spell has not reached the hero by then: he dies by its effect
constexpr uint64_t WordsMax    = 30000;  // ms: the npc's last words (its queue) end by then, or they are cut
constexpr uint64_t FallenMs    = 2200;   // the hero lies dead
constexpr uint64_t RaiseMs     = 1600;   // the summoning before he gets up
constexpr uint64_t RisenMs     = 3200;   // the skeleton gets up and stands
constexpr uint64_t FadeMs      = 1500;   // to black
constexpr uint64_t PictureMs   = 1200;   // the picture comes up
constexpr uint64_t KeyAfterMs  = 1500;   // after the picture is up, a key ends the game
constexpr auto     FontName    = "font_old_20_white.tga";
constexpr auto     SkeletonNpc    = "SKELETON";
constexpr auto     SummonSpell = "SPL_SUMMONSKELETON";
constexpr auto     SummonRune  = "ITRU_SUMSKEL";
constexpr auto     SummonFx    = "SPELLFX_SUMMONCREATURE_ORIGIN";

// the rune of a spell: SPL_DEATHBALL -> ITRU_DEATHBALL
std::string runeOf(std::string_view spell) {
  if(spell==SummonSpell)
    return SummonRune;
  if(spell.substr(0,4)=="SPL_")
    return "ITRU_" + std::string(spell.substr(4));
  return {};
  }

int32_t spellId(World& world, std::string_view spell) {
  auto* s = world.script().findSymbol(spell);
  return s!=nullptr ? s->get_int() : -1;
  }

void playFx(World& world, std::string_view name, const Tempest::Vec3& at) {
  const VisualFx* vfx = Gothic::inst().loadVisualFx(name);
  if(vfx==nullptr) {
    Log::i("[necro] no effect ",name);
    return;
    }
  Effect e(*vfx,world,at);
  e.setActive(true);
  world.runEffect(std::move(e));
  }

}

// black, then the picture over the whole screen with its title; it takes the keys while it is up
class Necromancy::Screen final : public Tempest::UiOverlay {
  public:
    void setCard(const Card& c) {
      title = c.title;
      text  = c.text;
      image.reset();
      if(c.image.empty())
        return;
      try {
        image = std::make_unique<Texture2d>(Resources::loadTexturePm(Pixmap(c.image.c_str())));
        }
      catch(...) {
        Log::e("[necro] can't load the picture ",c.image);
        }
      }
    void set(float b, float p) {
      if(b==black && p==picture)
        return;
      black   = b;
      picture = p;
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
      if(picture<=0.f)
        return;
      // the words by the screen, not by the interface: as large on a phone-shaped window as on a monitor
      const float side  = float(std::min(w(),h()));
      const float base  = float(std::max(1,Resources::font(FontName,Resources::FontType::Normal,1.f).pixelSize()));
      auto&       big   = Resources::font(FontName,Resources::FontType::Normal, side/11.f/base);
      auto&       small = Resources::font(FontName,Resources::FontType::Normal, side/28.f/base);
      const int   pad   = big.pixelSize()/2;
      const int   textW = w()-4*pad;
      // the words go under the picture: the title, then the line (it may wrap)
      const int   textH = (title.empty() ? 0 : big.pixelSize()+pad) +
                          (text.empty()  ? 0 : small.textSize(textW,text).h+pad);
      int y = 2*pad;
      if(image!=nullptr) {
        // the whole picture, as large as the screen takes it above the words: nothing of it is cut
        const int   room = std::max(1,h()-textH-4*pad);
        const float s    = std::min(float(w()-2*pad)/float(image->w()), float(room)/float(image->h()));
        const int   iw   = int(float(image->w())*s), ih = int(float(image->h())*s);
        y = std::max(2*pad,(h()-ih-textH)/2);
        pt.setBrush(Brush(*image,Color(1.f,1.f,1.f,picture),PaintDevice::Alpha,ClampMode::ClampToEdge));
        pt.drawRect((w()-iw)/2,y,iw,ih,0,0,image->w(),image->h());
        y += ih+pad;
        } else {
        y = (h()-textH)/2;
        }
      if(!title.empty()) {
        big.drawText(pt,0,y+big.pixelSize(),w(),0,title,AlignHCenter);
        y += big.pixelSize()+pad;
        }
      if(!text.empty())
        small.drawText(pt,2*pad,y+small.pixelSize(),textW,0,text,AlignHCenter);
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
    float                      black   = 0.f;
    float                      picture = 0.f;
    std::string                title, text;
    std::unique_ptr<Texture2d> image;
  };

Necromancy::Necromancy() = default;

Necromancy::~Necromancy() = default;  // the overlay leaves the window by itself

bool Necromancy::start(World& world, Npc& npc, Card c, std::string spl, std::string& error) {
  Npc* hero = world.player();
  if(hero==nullptr || hero->isDead()) {
    error = "no living hero";
    return false;
    }
  if(&npc==hero) {
    error = "the hero can't raise himself";
    return false;
    }
  if(spellId(world,spl)<0 || world.script().findSymbolIndex(runeOf(spl))==size_t(-1)) {
    error = "no spell " + spl;
    return false;
    }
  if(world.script().findSymbolIndex(SkeletonNpc)==size_t(-1)) {
    error = "no skeleton in the scripts";
    return false;
    }
  caster = &npc;
  card   = std::move(c);
  spell  = std::move(spl);
  // the hero can die now (the bench and the scripts may keep him alive)
  auto& h = hero->handle();
  h.flags = zenkit::NpcFlag(uint32_t(h.flags) & ~uint32_t(zenkit::NpcFlag::IMMORTAL));
  heroHp  = hero->attribute(ATR_HITPOINTS);
  Gothic::inst().setHudHidden(true);  // a scene of its own from here on
  // after its last words (they are in its queue), off the bookstand or the bench: a spell is cast standing
  npc.aiPush(AiQueue::aiStandup());
  enter(world,Stage::Cast);
  Log::i("[necro] ",spell," at the hero, he has ",heroHp," hp");
  return true;
  }

void Necromancy::enter(World& world, Stage s) {
  stage    = s;
  since    = world.tickCount();
  nextCast = since;
  castFrom = 0;
  }

bool Necromancy::cast(World& world, Npc& npc, Npc& hero) {
  const int32_t id   = spellId(world,spell);
  const size_t  rune = world.script().findSymbolIndex(runeOf(spell));
  npc.setTarget(&hero);
  npc.changeAttribute(ATR_MANA,npc.attribute(ATR_MANAMAX),false);
  if(npc.weaponState()!=WeaponState::Mage) {
    if(!npc.inventory().hasSpell(id))
      npc.addItem(rune,1);
    npc.aiPush(AiQueue::aiTurnToNpc(&hero));
    npc.aiPush(AiQueue::aiReadySpell(id,0));
    return false;
    }
  // the spell is in his hands: cast where he stands (the fight AI would step about for it); the cast turns
  // him to the target as it goes
  const auto r = npc.beginCastSpell();
  return r==Npc::BeginCastResult::BC_Cast || r==Npc::BeginCastResult::BC_Invest;
  }

void Necromancy::raise(World& world, Npc& npc, Npc& hero) {
  const int32_t id   = spellId(world,SummonSpell);
  const size_t  rune = world.script().findSymbolIndex(SummonRune);
  if(id>=0 && rune!=size_t(-1)) {
    // the summoning readied, not cast: the creature comes out of the body, not beside the caster
    if(!npc.inventory().hasSpell(id))
      npc.addItem(rune,1);
    npc.clearAiQueue();
    npc.aiPush(AiQueue::aiTurnToNpc(&hero));
    npc.aiPush(AiQueue::aiReadySpell(id,0));
    }
  playFx(world,SummonFx,hero.position());
  }

void Necromancy::rise(World& world, Npc& hero) {
  // back from ZS_Dead, then the shape of a skeleton: the body that lay there gets up
  hero.clearState(true);
  hero.changeAttribute(ATR_HITPOINTS,hero.attribute(ATR_HITPOINTSMAX),false);
  const size_t id = world.script().findSymbolIndex(SkeletonNpc);
  const bool transformed=hero.transformInto(id);
  if(!transformed)
    Log::e("[necro] the hero did not become a skeleton");
  if(const auto* trace=std::getenv("OPENGOTHIC_RAGDOLL_TRACE");trace!=nullptr && trace[0]=='1')
    Log::i("[necro] raised hero transformed=",transformed," ragdoll=",hero.hasRagdoll(),
           " skeleton=",hero.visualSkeleton()!=nullptr ? hero.visualSkeleton()->name() : std::string_view());
  if(hero.playAnimByName("T_SPAWN",BS_STAND)==nullptr)
    Log::i("[necro] no T_SPAWN for the skeleton");
  playFx(world,SummonFx,hero.position());
  }

void Necromancy::tick(World& world) {
  if(stage==Stage::Idle)
    return;
  Npc* hero = world.player();
  if(hero==nullptr || caster==nullptr) {
    clear();
    return;
    }
  const uint64_t now = world.tickCount();
  const uint64_t in  = now-since;

  switch(stage) {
    case Stage::Idle:
      break;
    case Stage::Cast: {
      if(hero->isDead()) {
        caster->clearAiQueue();
        enter(world,Stage::Fallen);
        break;
        }
      const bool late = castFrom!=0 && now-castFrom>=CastMax;
      if(hero->attribute(ATR_HITPOINTS)<heroHp || late) {
        // hit and still standing, or never reached: the spell's effect, and he falls
        if(late)
          if(auto vfx = Gothic::inst().loadVisualFx("SPELLFX_" + spell.substr(4)))
            caster->startEffect(*hero,*vfx);
        hero->changeAttribute(ATR_HITPOINTS,-hero->attribute(ATR_HITPOINTSMAX)-1,false);
        Log::i("[necro] the hero is finished off ",now-castFrom," ms after the cast");
        break;
        }
      if(castFrom==0 && in>=WordsMax)
        caster->clearAiQueue();
      if(now>=nextCast && caster->isAiQueueEmpty()) {
        nextCast = now + (cast(world,*caster,*hero) ? CastAgain : CastEvery);
        if(castFrom==0)
          castFrom = now;
        }
      break;
      }
    case Stage::Fallen:
      if(in>=FallenMs) {
        raise(world,*caster,*hero);
        enter(world,Stage::Raise);
        }
      break;
    case Stage::Raise:
      if(in>=RaiseMs) {
        rise(world,*hero);
        caster->aiPush(AiQueue::aiRemoveWeapon());
        enter(world,Stage::Risen);
        }
      break;
    case Stage::Risen:
      if(in>=RisenMs) {
        if(screen==nullptr) {
          screen = std::make_unique<Screen>();
          screen->onKey = [this]() {
            if(stage!=Stage::Card || TestBench::isActive())
              return;
            auto* w = Gothic::inst().world();
            if(w==nullptr || w->tickCount()-since < FadeMs+PictureMs+KeyAfterMs)
              return;
            clear();
            if(onEnd) {
              onEnd();
              return;
              }
            Log::i("[necro] the end: back to the main menu");
            if(auto s = Gothic::inst().gameSession())
              s->exitSession();
            };
          SystemApi::addOverlay(screen.get());
          }
        screen->setCard(card);
        enter(world,Stage::Card);
        }
      break;
    case Stage::Card: {
      const float b = std::min(1.f,float(in)/float(FadeMs));
      const float p = in<FadeMs ? 0.f : std::min(1.f,float(in-FadeMs)/float(PictureMs));
      screen->set(b,p);
      break;
      }
    }
  }

void Necromancy::clear() {
  if(stage!=Stage::Idle)
    Gothic::inst().setHudHidden(false);
  stage  = Stage::Idle;
  caster = nullptr;
  if(screen!=nullptr)
    screen->set(0.f,0.f);
  }

bool Necromancy::reached(std::string_view name) const {
  static constexpr std::pair<std::string_view,Stage> names[] = {
    {"cast",Stage::Cast}, {"fallen",Stage::Fallen}, {"raise",Stage::Raise}, {"risen",Stage::Risen}, {"card",Stage::Card},
    };
  for(auto& [n,s]:names)
    if(n==name)
      return stage!=Stage::Idle && stage>=s;
  return false;
  }

std::string Necromancy::status() const {
  switch(stage) {
    case Stage::Idle:   return "idle";
    case Stage::Cast:   return "cast";
    case Stage::Fallen: return "fallen";
    case Stage::Raise:  return "raise";
    case Stage::Risen:  return "risen";
    case Stage::Card:   return "card";
    }
  return "?";
  }
