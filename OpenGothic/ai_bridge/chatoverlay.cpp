#include "chatoverlay.h"

#include <Tempest/Application>
#include <Tempest/Painter>
#include <Tempest/Pixmap>
#include <Tempest/Log>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "utils/gthfont.h"
#include "gothic.h"
#include "resources.h"

#include "textcodec.h"

using namespace Tempest;

namespace {

constexpr size_t   MaxInput       = 300;
constexpr size_t   MaxLog         = 32;
constexpr size_t   OpenRows       = 6;   // entries shown while the input line is open
constexpr size_t   SubtitleRows   = 3;   // entries shown as subtitles otherwise (the hero's line and replies)
constexpr uint64_t LingerMs       = 2500;
constexpr uint64_t PttHoldMs      = 300;    // T held longer than this is push-to-talk
constexpr uint64_t PttMaxMs       = 30000;  // the key-up may never come (focus lost)
constexpr auto     FontName       = "font_old_20_white.tga"; // the large game font, has cyrillic in RU
constexpr uint64_t DealSettleMs   = 1000;   // Y waits this long after the terms appear or change
constexpr uint64_t DealFreshMs    = 4000;   // what changed stays marked this long
constexpr auto     DealParchment  = "LETTERS.TGA";     // the game's letters are written on it
constexpr auto     DealFont       = "font_10_book.tga"; // and in this ink
constexpr auto     DealBigFont    = "font_20_book.tga";
constexpr uint64_t FeatFadeInMs   = 300;
constexpr uint64_t FeatFadeOutMs  = 700;
constexpr uint64_t FeatBaseMs     = 3500;   // a toast stays this long, plus a little per letter of its line
constexpr uint64_t FeatPerCharMs  = 35;
constexpr uint64_t FeatMaxMs      = 7000;
constexpr size_t   FeatQueue      = 4;      // more waiting than this: the oldest waiting ones are dropped

// GOTHIC_AI_CHAT_SCALE: extra size multiplier for the chat, on top of the game's interface scale
// the end of `text` that fits into `width` (live transcripts grow at the end)
std::string fitTail(const GthFont& font, std::string text, int width) {
  if(font.textSize(text).w<=width)
    return text;
  while(!text.empty() && font.textSize("..."+text).w>width) {
    const size_t sp = text.find(' ',1);
    text.erase(0,sp==std::string::npos ? 1 : sp);
    }
  return "..."+text;
  }

float chatScale() {
  static const float v = [] {
    const char* env = std::getenv("GOTHIC_AI_CHAT_SCALE");
    const float f   = env!=nullptr ? std::strtof(env,nullptr) : 0.f;
    return f>0.f ? f : 1.f;
    }();
  return v;
  }

}

ChatOverlay::ChatOverlay() {
  }

void ChatOverlay::openInput() {
  active = true;
  input.clear();
  cursor = 0;
  update();
  }

void ChatOverlay::closeInput() {
  active = false;
  input.clear();
  cursor = 0;
  update();
  }

void ChatOverlay::setPartial(std::string text) {
  partial = std::move(text);
  update();
  }

void ChatOverlay::pttPress(char pendingChar) {
  partial.clear();
  tDown        = true;
  tDownAt      = Application::tickCount();
  tPendingChar = pendingChar;
  if(onPttStart)
    onPttStart();
  }

void ChatOverlay::pttRelease() {
  if(!tDown)
    return;
  const bool spoke = recording;
  const char typed = tPendingChar;
  pttReset();
  if(spoke) {
    if(onPttStop)
      onPttStop();
    update();
    return;
    }
  if(onPttCancel)
    onPttCancel();
  if(!active) {
    openInput();  // a short tap: type instead of speaking
    return;
    }
  if(typed!='\0' && input.size()<MaxInput) {
    input.insert(cursor,1,typed);
    ++cursor;
    }
  update();
  }

void ChatOverlay::pttReset() {
  tDown        = false;
  recording    = false;
  tPendingChar = '\0';
  }

void ChatOverlay::addLine(std::string speaker, std::string text, uint32_t showMs) {
  Entry e;
  e.speaker    = std::move(speaker);
  e.text       = std::move(text);
  e.shownUntil = Application::tickCount() + showMs + LingerMs;
  log.push_back(std::move(e));
  while(log.size()>MaxLog)
    log.pop_front();
  update();
  }

void ChatOverlay::upsertLine(std::string_view key, std::string speaker, std::string text, uint32_t showMs) {
  for(auto it=log.rbegin(); it!=log.rend(); ++it) {
    if(it->key!=key)
      continue;
    it->text       = std::move(text);
    it->shownUntil = std::max(it->shownUntil, Application::tickCount() + showMs + LingerMs);
    update();
    return;
    }
  addLine(std::move(speaker),std::move(text),showMs);
  log.back().key = std::string(key);
  }

void ChatOverlay::extend(std::string_view key, uint32_t showMs) {
  for(auto it=log.rbegin(); it!=log.rend(); ++it)
    if(it->key==key) {
      it->shownUntil = std::max(it->shownUntil, Application::tickCount() + showMs + LingerMs);
      update();
      return;
      }
  }

void ChatOverlay::cut(std::string_view key, std::string said) {
  const uint64_t now = Application::tickCount();
  for(auto it=log.rbegin(); it!=log.rend(); ++it) {
    if(it->key!=key)
      continue;
    if(said.empty()) {
      it->shownUntil = now;
      } else {
      it->text       = std::move(said) + " -";
      it->shownUntil = now + LingerMs;
      }
    update();
    return;
    }
  }

void ChatOverlay::setStatus(std::string text) {
  status = std::move(text);
  update();
  }

bool ChatOverlay::hasVisibleContent() const {
  return !log.empty() && log.back().shownUntil>Application::tickCount();
  }

void ChatOverlay::tick() {
  if(tDown) {
    const uint64_t held = Application::tickCount() - tDownAt;
    if(!recording && held>=PttHoldMs)
      recording = true;
    if(recording && held>=PttMaxMs)
      pttRelease();
    }
  // repaint while something is on screen (cursor blink, subtitles expiring), and once after it is gone
  const bool visible = active || recording || !partial.empty() || !status.empty() || hasVisibleContent() ||
                       !deal.id.empty() || featOnScreen();
  if(visible || wasVisible)
    update();
  wasVisible = visible;
  }

void ChatOverlay::showDeal(std::string id, int32_t version, std::string npc,
                           std::vector<DealLine> npcGives, std::vector<DealLine> heroGives) {
  if(deal.id==id && deal.version==version) {
    deal.sent = false;  // the server answered: Y may be pressed again
    update();
    return;
    }
  // what is not in the shown version is new
  auto mark = [&](std::vector<DealLine>& now, const std::vector<DealLine>& before) {
    for(auto& l:now)
      l.fresh = deal.id==id && std::none_of(before.begin(),before.end(),[&](const DealLine& b){
        return b.text==l.text;
        });
    };
  mark(npcGives, deal.npcGives);
  mark(heroGives,deal.heroGives);
  deal.id        = std::move(id);
  deal.version   = version;
  deal.npc       = std::move(npc);
  deal.npcGives  = std::move(npcGives);
  deal.heroGives = std::move(heroGives);
  deal.changedAt = Application::tickCount();
  deal.sent      = false;
  update();
  }

void ChatOverlay::hideDeal(std::string_view id) {
  if(deal.id.empty() || (!id.empty() && deal.id!=id))
    return;
  deal = Deal();
  update();
  }

void ChatOverlay::markMissing(const std::function<bool(const DealLine&)>& lacks) {
  for(auto& l:deal.heroGives)
    l.missing = lacks(l);
  }

std::string ChatOverlay::declineDeal() {
  if(deal.id.empty())
    return "none";
  const auto id = deal.id;
  const auto v  = deal.version;
  deal = Deal();  // the window goes at once; the npc may offer again
  if(onDealDecline)
    onDealDecline(id,v);
  update();
  return "declined";
  }

std::string ChatOverlay::pressDeal() {
  if(deal.id.empty())
    return "none";
  if(deal.sent)
    return "waiting";
  for(auto& l:deal.heroGives)
    if(l.missing)
      return "missing";
  if(Application::tickCount() < deal.changedAt + DealSettleMs)
    return "changed";
  deal.sent = true;
  if(onDealAccept)
    onDealAccept(deal.id,deal.version);
  update();
  return "sent";
  }

void ChatOverlay::setHidden(bool h) {
  if(hidden==h)
    return;
  hidden = h;
  if(hidden)
    closeInput();
  update();
  }

void ChatOverlay::paintEvent(PaintEvent& e) {
  if(hidden)
    return;
  Painter p(e);
  if(!deal.id.empty())
    paintDeal(p);
  paintFeat(p);
  paintChat(p);
  }

static uint64_t featShowMs(const std::string& text) {
  return std::min(FeatMaxMs, FeatBaseMs + FeatPerCharMs*text.size()) + FeatFadeInMs + FeatFadeOutMs;
  }

void ChatOverlay::showFeat(std::string title, std::string text, std::string icon) {
  while(feats.size()>FeatQueue)
    feats.erase(feats.begin()+1);
  feats.push_back({std::move(title),std::move(text),std::move(icon)});
  if(feats.size()==1)
    featShownAt = 0;  // the next frame starts it
  update();
  }

bool ChatOverlay::featOnScreen() {
  // the front toast is over: the next one waits for a frame (its picture loads when it starts)
  const uint64_t now = Application::tickCount();
  if(!feats.empty() && featShownAt!=0 && now>=featShownAt+featShowMs(feats.front().text)) {
    feats.pop_front();
    featShownAt = 0;
    featIcon.reset();
    }
  return !feats.empty();
  }

void ChatOverlay::paintFeat(Painter& p) {
  // a strip of the letters' parchment in the top-left corner: the picture in its own frame on the left,
  // the title in the book ink, the line under it; it fades in and out, nothing else asks for attention
  if(!featOnScreen())
    return;
  const uint64_t now = Application::tickCount();
  auto&          f   = feats.front();
  if(featShownAt==0) {
    featShownAt = now;
    featIcon.reset();
    if(!f.icon.empty()) {
      try {
        featIcon = std::make_unique<Texture2d>(Resources::loadTexturePm(Pixmap(f.icon.c_str())));
        }
      catch(...) {
        Log::e("[feat] can't load the picture ",f.icon);
        }
      }
    }
  const uint64_t age   = now - featShownAt;
  const uint64_t total = featShowMs(f.text);
  const float    alpha = age<FeatFadeInMs ? float(age)/float(FeatFadeInMs) :
                         age+FeatFadeOutMs>total ? float(total-std::min(total,age))/float(FeatFadeOutMs) : 1.f;

  const float scale = Gothic::interfaceScale(this)*chatScale();
  auto&       title = Resources::font(DealBigFont,Resources::FontType::Normal,  scale);
  auto&       line  = Resources::font(DealFont,   Resources::FontType::Normal,  scale);
  const int   pad   = std::max(10,title.pixelSize()*2/3);
  const int   iconS = featIcon!=nullptr ? int(96*scale) : 0;
  const int   boxW  = std::min(w()-16, int(480*scale) + iconS);
  const int   textX = pad + (iconS>0 ? iconS + pad : 0);
  const int   textW = boxW - textX - pad;
  // a long title wraps: everything below it moves down by its real height
  const int   titleH = title.textSize(textW,f.title).h;
  const int   textH  = titleH + line.textSize(textW,f.text).h + pad/3;
  const int   boxH  = std::max(iconS,textH) + 2*pad;
  const int   x0    = std::max(8,w()/40);
  const int   y0    = std::max(8,h()/14) - int(float(pad)*(1.f-alpha));  // it settles in from a little above
  const Color shade = Color(1.f,1.f,1.f,alpha);

  if(auto back = Resources::loadTexture(DealParchment)) {
    p.setBrush(Brush(*back,shade,PaintDevice::Alpha,ClampMode::ClampToEdge));
    p.drawRect(x0,y0,boxW,boxH,0,0,back->w(),back->h());
    } else {
    p.setBrush(Color(0.82f,0.74f,0.55f,0.95f*alpha));
    p.drawRect(x0,y0,boxW,boxH);
    }
  if(featIcon!=nullptr) {
    p.setBrush(Brush(*featIcon,shade,PaintDevice::Alpha,ClampMode::ClampToEdge));
    p.drawRect(x0+pad,y0+(boxH-iconS)/2,iconS,iconS,0,0,featIcon->w(),featIcon->h());
    }
  // the fonts have no alpha of their own: the text comes in with the parchment, once it is mostly there
  if(alpha<0.5f)
    return;
  int y = y0 + pad + (boxH-2*pad-textH)/2;
  title.drawText(p,x0+textX,y+title.pixelSize(),textW,0,f.title,AlignLeft);
  y += titleH + pad/3;
  line.drawText(p,x0+textX,y+line.pixelSize(),textW,0,f.text,AlignLeft);
  }

// a key on the keyboard, drawn by strokes: the game's fonts draw a Latin Y like a Cyrillic У
static void drawKey(Painter& p, int x, int y, int size, char key, const Color& cl, int thin) {
  p.setBrush(cl);
  p.drawRect(x,y,size,thin);
  p.drawRect(x,y+size-thin,size,thin);
  p.drawRect(x,y,thin,size);
  p.drawRect(x+size-thin,y,thin,size);

  auto at = [&](float fx, float fy) { return Point(x+int(fx*float(size)),y+int(fy*float(size))); };
  std::vector<std::pair<Point,Point>> strokes;
  if(key=='Y')
    strokes = {{at(0.28f,0.25f),at(0.5f,0.52f)},{at(0.72f,0.25f),at(0.5f,0.52f)},{at(0.5f,0.52f),at(0.5f,0.78f)}};
  if(key=='N')
    strokes = {{at(0.3f,0.78f),at(0.3f,0.24f)},{at(0.3f,0.24f),at(0.7f,0.78f)},{at(0.7f,0.78f),at(0.7f,0.24f)}};
  p.setPen(Pen(cl,PaintDevice::Alpha,float(thin)));
  for(auto& [a,b]:strokes)
    for(int d=0; d<=thin; ++d) {
      p.drawLine(a.x+d,a.y,b.x+d,b.y);
      p.drawLine(a.x+d,a.y+1,b.x+d,b.y+1);
      }
  }

void ChatOverlay::paintDeal(Painter& p) {
  // a contract on the parchment of the game's letters, in their ink: what the npc gives on the left, what
  // the hero gives on the right; what changed is marked, what he lacks is red; the keys in frames
  const uint64_t now   = Application::tickCount();
  const float    scale = Gothic::interfaceScale(this)*chatScale();
  auto&          big   = Resources::font(DealBigFont,Resources::FontType::Normal,  scale);
  auto&          ink   = Resources::font(DealBigFont,Resources::FontType::Normal,  scale);
  auto&          small = Resources::font(DealFont,   Resources::FontType::Disabled,scale);
  auto&          dim   = Resources::font(DealBigFont,Resources::FontType::Disabled,scale);
  auto&          red   = Resources::font(FontName,   Resources::FontType::Red,     scale*0.5f);
  const int      lineH = ink.pixelSize();
  const int      gap   = std::max(2,lineH/5);
  const int      pad   = std::max(12,lineH);
  const int      boxW  = std::min(w()-2*pad, std::max(int(600*scale), w()*3/8));
  const int      colW  = (boxW-3*pad)/2;
  const int      x0    = w()-boxW-std::max(8,lineH/3);
  const int      y0    = std::max(8,h()/6);
  const int      thin  = std::max(1,int(scale+0.5f));
  const Color    inkCl = Color(0.22f,0.13f,0.05f,0.9f);

  static const std::string title   = TextCodec::fromUtf8("Сделка");
  static const std::string gives   = TextCodec::fromUtf8(" дает");
  static const std::string youGive = TextCodec::fromUtf8("Ты даешь");
  static const std::string nothing = TextCodec::fromUtf8("ничего");
  static const std::string accept  = TextCodec::fromUtf8("Принять");
  static const std::string decline = TextCodec::fromUtf8("Отказаться");
  static const std::string lacking = TextCodec::fromUtf8("не хватает");
  static const std::string waiting = TextCodec::fromUtf8("Ждем ответа...");

  const bool  missing = std::any_of(deal.heroGives.begin(),deal.heroGives.end(),[](const DealLine& l){
    return l.missing;
    });
  const bool  settled = now >= deal.changedAt + DealSettleMs;
  const float fresh   = now < deal.changedAt + DealFreshMs ?
                        1.f - float(now-deal.changedAt)/float(DealFreshMs) : 0.f;
  const int   noteH   = red.pixelSize();

  auto lineH_ = [&](const DealLine& l) { return ink.textSize(colW,l.text).h + (l.missing ? noteH : 0); };
  auto columnH = [&](const std::vector<DealLine>& lines) {
    int sum = 0;
    if(lines.empty())
      sum += ink.textSize(colW,nothing).h + gap;
    for(auto& l:lines)
      sum += lineH_(l) + gap;
    return sum;
    };
  const std::string npcHead = deal.npc + gives;
  const int titleH = big.pixelSize();
  const int headH  = std::max(small.textSize(colW,npcHead).h,small.textSize(colW,youGive).h);
  const int bodyH  = std::max(columnH(deal.npcGives),columnH(deal.heroGives));
  const int keyH   = lineH + gap;
  const int total  = pad + titleH + pad/2 + headH + gap + bodyH + pad/2 + keyH + pad;

  if(auto back = Resources::loadTexture(DealParchment)) {
    p.setBrush(*back);
    p.drawRect(x0,y0,boxW,total,0,0,back->w(),back->h());
    } else {
    p.setBrush(Color(0.82f,0.74f,0.55f,0.95f));
    p.drawRect(x0,y0,boxW,total);
    }

  int y = y0 + pad;
  big.drawText(p,x0,y+titleH,boxW,0,title,AlignHCenter);
  // rules on both sides of the title, and between the columns
  const int tw = big.textSize(title).w;
  p.setBrush(Color(0.25f,0.15f,0.05f,0.45f));
  p.drawRect(x0+pad,y+titleH*2/3,(boxW-tw)/2-pad-pad/2,thin);
  p.drawRect(x0+(boxW+tw)/2+pad/2,y+titleH*2/3,(boxW-tw)/2-pad-pad/2,thin);
  y += titleH + pad/2;

  const int xl = x0 + pad, xr = x0 + 2*pad + colW;
  p.drawRect(x0+pad+colW+pad/2,y,thin,headH+gap+bodyH);
  small.drawText(p,xl,y+small.pixelSize(),colW,0,npcHead,AlignLeft);
  small.drawText(p,xr,y+small.pixelSize(),colW,0,youGive,AlignLeft);
  y += headH + gap;

  auto column = [&](int x, const std::vector<DealLine>& lines) {
    int yy = y;
    if(lines.empty())
      small.drawText(p,x,yy+lineH,colW,0,nothing,AlignLeft);
    for(auto& l:lines) {
      const int hh = lineH_(l);
      if(l.missing) {
        p.setBrush(Color(0.75f,0.1f,0.05f,0.22f));
        p.drawRect(x-gap,yy,colW+2*gap,hh);
        } else if(l.fresh && fresh>0.f) {
        // a marker over what changed, fading
        p.setBrush(Color(1.f,0.75f,0.15f,0.45f*fresh));
        p.drawRect(x-gap,yy,colW+2*gap,hh);
        }
      ink.drawText(p,x,yy+lineH,colW,0,l.text,AlignLeft);
      if(l.missing)
        red.drawText(p,x,yy+hh,colW,0,lacking,AlignLeft);
      yy += hh + gap;
      }
    };
  column(xl,deal.npcGives);
  column(xr,deal.heroGives);
  y += bodyH + pad/2;

  if(deal.sent) {
    small.drawText(p,x0,y+lineH,boxW,0,waiting,AlignHCenter);
    return;
    }
  // the keys, then what they do
  auto key = [&](int x, char k, const std::string& what, bool on) {
    const Color cl = on ? inkCl : Color(inkCl.r(),inkCl.g(),inkCl.b(),0.35f);
    drawKey(p,x,y,keyH,k,cl,thin);
    (on ? ink : dim).drawText(p,x+keyH+lineH/2,y+lineH-gap/2,what);
    };
  key(xl,'Y',accept,settled && !missing);
  key(xr,'N',decline,true);
  }

void ChatOverlay::paintChat(Painter& p) {
  const uint64_t now = Application::tickCount();
  std::vector<const Entry*> rows;
  for(auto it=log.rbegin(); it!=log.rend(); ++it) {
    if(active ? rows.size()>=OpenRows : (rows.size()>=SubtitleRows || it->shownUntil<=now))
      break;
    rows.insert(rows.begin(),&*it);
    }
  // one line under the subtitles: the player speaking, his words being recognized, or a status
  const bool extra = recording || !partial.empty() || !status.empty();
  if(!active && !extra && rows.empty())
    return;

  const float scale = Gothic::interfaceScale(this)*chatScale();
  auto&       text  = Resources::font(FontName,Resources::FontType::Normal,  scale);
  auto&       name  = Resources::font(FontName,Resources::FontType::Yellow,  scale);
  auto&       hint  = Resources::font(FontName,Resources::FontType::Disabled,scale);
  auto&       rec   = Resources::font(FontName,Resources::FontType::Red,     scale);
  const int   lineH = text.pixelSize();
  const int   pad   = std::max(8,lineH/2);
  // ~75 characters per line of the large font, at least 60% of the window
  const int   boxW  = std::min(w()-2*pad, std::max(int(1000*scale), w()*3/5));
  const int   textW = boxW-2*pad;
  const int   x0    = (w()-boxW)/2;

  int total = pad;
  for(auto r:rows)
    total += lineH + text.textSize(textW,r->text).h + pad/2;
  if(extra)
    total += lineH;
  if(active)
    total += lineH;
  total += pad/2;

  int y = h() - 2*lineH - total;
  p.setBrush(Color(0.f,0.f,0.f,0.6f));
  p.drawRect(x0,y,boxW,total);

  y += pad;
  for(auto r:rows) {
    name.drawText(p,x0+pad,y+lineH,r->speaker);
    y += lineH;
    text.drawText(p,x0+pad,y+lineH,textW,0,r->text,AlignLeft);
    y += text.textSize(textW,r->text).h + pad/2;
    }

  if(recording) {
    static const std::string label = TextCodec::fromUtf8("Запись: ");
    static const std::string speak = TextCodec::fromUtf8("говорите... отпустите T, чтобы сказать");
    rec.drawText(p,x0+pad,y+lineH,label+fitTail(rec,partial.empty() ? speak : partial,textW-rec.textSize(label).w));
    y += lineH;
    }
  else if(!partial.empty()) {
    static const std::string label = TextCodec::fromUtf8("Распознаю: ");
    hint.drawText(p,x0+pad,y+lineH,label+fitTail(hint,partial,textW-hint.textSize(label).w));
    y += lineH;
    }
  else if(!status.empty()) {
    hint.drawText(p,x0+pad,y+lineH,fitTail(hint,status,textW));
    y += lineH;
    }

  if(!active)
    return;
  const std::string prompt = "> ";
  text.drawText(p,x0+pad,y+lineH,prompt+input);
  // blinking cursor
  const int   cx = x0+pad + text.textSize(prompt+input.substr(0,cursor)).w;
  const float a  = float(now%1000)/1000.f;
  p.setBrush(Color(1.f,1.f,1.f,a<0.5f ? 1.f : 0.f));
  p.drawRect(cx,y+2,std::max(1,int(scale)),lineH-4);
  }

void ChatOverlay::keyDownEvent(KeyEvent& e) {
  const bool plainT = e.key==Event::K_T && e.modifier==Event::M_NoModifier;
  if(!active) {
    // the game keeps every key but T: the hero moves and fights while the player speaks
    if(plainT && (tDown || (canSpeak && canSpeak()))) {
      e.accept();
      if(!tDown)
        pttPress('\0');  // held, it is push-to-talk right away; a short tap opens the input line
      return;
      }
    // Y accepts the deal in the window, N declines it; while the input line is open they are letters
    const bool dealKey = (e.key==Event::K_Y || e.key==Event::K_N) && e.modifier==Event::M_NoModifier;
    if(dealKey && !deal.id.empty() && (!canDeal || canDeal())) {
      e.accept();
      if(e.key==Event::K_Y) {
        yDown = true;
        pressDeal();
        } else {
        nDown = true;
        declineDeal();
        }
      return;
      }
    e.ignore();
    return;
    }
  if(Event::K_F1<=e.key && e.key<=Event::K_F12) {
    e.ignore();
    return;
    }
  e.accept();
  if(tDown && e.key!=Event::K_ESCAPE)
    return;
  if(plainT && input.empty()) {
    // a word may start with this letter: decided on release (short tap) or after the hold delay
    pttPress(TextCodec::encode(e.code));
    return;
    }
  edit(e);
  }

void ChatOverlay::keyRepeatEvent(KeyEvent& e) {
  if(!active) {
    if((e.key==Event::K_T && tDown) || (e.key==Event::K_Y && yDown) || (e.key==Event::K_N && nDown))
      e.accept(); else
      e.ignore();
    return;
    }
  e.accept();
  if(!tDown)
    edit(e);
  }

void ChatOverlay::keyUpEvent(KeyEvent& e) {
  const bool ours = active || (e.key==Event::K_T && tDown) || (e.key==Event::K_Y && yDown) ||
                    (e.key==Event::K_N && nDown);
  if(e.key==Event::K_Y)
    yDown = false;
  if(e.key==Event::K_N)
    nDown = false;
  if(e.key==Event::K_T)
    pttRelease();
  if(ours)
    e.accept(); else
    e.ignore();
  }

void ChatOverlay::edit(KeyEvent& e) {
  switch(e.key) {
    case Event::K_ESCAPE:
      if(tDown) {
        pttReset();
        if(onPttCancel)
          onPttCancel();
        }
      closeInput();
      return;
    case Event::K_Return: {
      const std::string txt = input;
      closeInput();
      if(!txt.empty() && onSubmit)
        onSubmit(txt);
      return;
      }
    case Event::K_Back:
      if(cursor>0)
        input.erase(--cursor,1);
      break;
    case Event::K_Delete:
      if(cursor<input.size())
        input.erase(cursor,1);
      break;
    case Event::K_Left:
      if(cursor>0)
        --cursor;
      break;
    case Event::K_Right:
      if(cursor<input.size())
        ++cursor;
      break;
    case Event::K_Home:
      cursor = 0;
      break;
    case Event::K_End:
      cursor = input.size();
      break;
    default: {
      if(e.code<32 || (e.modifier & (Event::M_Ctrl|Event::M_Command))!=0)
        break;
      const char c = TextCodec::encode(e.code);
      if(c!='\0' && input.size()<MaxInput) {
        input.insert(cursor,1,c);
        ++cursor;
        }
      break;
      }
    }
  update();
  }

// while the line is open, clicks and the wheel stay here (no swings at the air); the mouse still turns the hero
void ChatOverlay::mouseDownEvent(MouseEvent& e) {
  if(active) e.accept(); else e.ignore();
  }

void ChatOverlay::mouseUpEvent(MouseEvent& e) {
  if(active) e.accept(); else e.ignore();
  }

void ChatOverlay::mouseWheelEvent(MouseEvent& e) {
  if(active) e.accept(); else e.ignore();
  }
