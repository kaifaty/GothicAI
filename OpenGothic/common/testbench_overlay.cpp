#include "testbench_internal.h"

#include <Tempest/Pixmap>
#include <Tempest/SoundDevice>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "camera.h"
#include "game/bodyarena.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/mesh/animblend.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/flinch.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/lean.h"
#include "graphics/mesh/lipsync.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/talkgestures.h"
#include "world/objects/npc.h"
#include "world/world.h"

#ifdef _WIN32
// The MSVC runtime names the POSIX pipe functions with an underscore (bench video capture only).
#define popen _popen
#define pclose _pclose
#endif

using namespace Tempest;
using namespace testbench_detail;

namespace {

const Rgba White  = {255,255,255,170};
const Rgba LegL   = { 60,200,255,255};
const Rgba LegR   = {255,140, 40,255};
const Rgba LegFL  = {150,140,255,255};
const Rgba LegFR  = {255,110,190,255};
const Rgba LegColor[] = {LegL, LegR, LegFL, LegFR};
const char* const LegName[] = {"L", "R", "FL", "FR"};
const Rgba Plant  = { 60,230, 80,255};
const Rgba Swing  = {240,220, 60,255};
const Rgba Red    = {255, 50, 50,255};
const Rgba Shade  = {  0,  0,  0,150};

// 3x5 font, rows top to bottom
struct Glyph { char c; const char* rows; };
const Glyph glyphs[] = {
  {'0',"XXXX.XX.XX.XXXX"},{'1',".X.XX..X..X.XXX"},{'2',"XXX..XXXXX..XXX"},{'3',"XXX..XXXX..XXXX"},
  {'4',"X.XX.XXXX..X..X"},{'5',"XXXX..XXX..XXXX"},{'6',"XXXX..XXXX.XXXX"},{'7',"XXX..X..X..X..X"},
  {'8',"XXXX.XXXXX.XXXX"},{'9',"XXXX.XXXX..XXXX"},{'A',".X.X.XXXXX.XX.X"},{'B',"XX.X.XXX.X.XXX."},
  {'C',"XXXX..X..X..XXX"},{'D',"XX.X.XX.XX.XXX."},{'E',"XXXX..XX.X..XXX"},{'F',"XXXX..XX.X..X.."},
  {'G',"XXXX..X.XX.XXXX"},{'H',"X.XX.XXXXX.XX.X"},{'I',"XXX.X..X..X.XXX"},{'J',"..X..X..XX.XXXX"},
  {'K',"X.XX.XXX.X.XX.X"},{'L',"X..X..X..X..XXX"},{'M',"X.XXXXXXXX.XX.X"},{'N',"XX.X.XX.XX.XX.X"},
  {'O',"XXXX.XX.XX.XXXX"},{'P',"XXXX.XXXXX..X.."},{'Q',"XXXX.XX.XXXX..X"},{'R',"XX.X.XXX.X.XX.X"},
  {'S',"XXXX..XXX..XXXX"},{'T',"XXX.X..X..X..X."},{'U',"X.XX.XX.XX.XXXX"},{'V',"X.XX.XX.XX.X.X."},
  {'W',"X.XX.XXXXXXXX.X"},{'X',"X.XX.X.X.X.XX.X"},{'Y',"X.XX.X.X..X..X."},{'Z',"XXX..X.X.X..XXX"},
  {'-',"......XXX......"},{'.',".............X."},{':',"....X.....X...."},{'=',"...XXX...XXX..."},
  {'_',"............XXX"},{'/',"..X..X.X.X..X.."},{'+',"....X.XXX.X...."},{'%',"X.X..X.X.X..X.X"},
  };

}

namespace testbench_detail {

const Rgba Text   = {255,255,255,255};

void Canvas::blend(int x, int y, Rgba c) {
    if(x<0 || y<0 || x>=w || y>=h)
      return;
    uint8_t*  p = px + (size_t(y)*size_t(w) + size_t(x))*4;
    const int a = c.a;
    p[0] = uint8_t((c.r*a + p[0]*(255-a))/255);
    p[1] = uint8_t((c.g*a + p[1]*(255-a))/255);
    p[2] = uint8_t((c.b*a + p[2]*(255-a))/255);
    p[3] = 255;
    }

void Canvas::rect(int x, int y, int rw, int rh, Rgba c) {
    for(int j=y; j<y+rh; ++j)
      for(int i=x; i<x+rw; ++i)
        blend(i,j,c);
    }

void Canvas::dot(float x, float y, int r, Rgba c) {
    const int cx = int(std::lround(x)), cy = int(std::lround(y));
    for(int j=-r; j<=r; ++j)
      for(int i=-r; i<=r; ++i)
        if(i*i+j*j<=r*r+r)
          blend(cx+i,cy+j,c);
    }

void Canvas::line(float x0, float y0, float x1, float y1, int r, Rgba c) {
    const float len = std::max(std::abs(x1-x0),std::abs(y1-y0));
    if(len>4000)
      return;
    const int n = std::max(1,int(len));
    for(int i=0; i<=n; ++i) {
      const float t = float(i)/float(n);
      if(r<=0)
        blend(int(std::lround(x0+(x1-x0)*t)),int(std::lround(y0+(y1-y0)*t)),c); else
        dot(x0+(x1-x0)*t,y0+(y1-y0)*t,r,c);
      }
    }

void Canvas::stroke(const std::vector<Vec2>& pt, float alpha, const Crosshair::Pen& pen) {
    if(pt.size()<2)
      return;
    float x0 = pt[0].x, y0 = pt[0].y, x1 = x0, y1 = y0;
    for(auto& p:pt) {
      x0 = std::min(x0,p.x); x1 = std::max(x1,p.x);
      y0 = std::min(y0,p.y); y1 = std::max(y1,p.y);
      }
    const float m = pen.half+pen.edge+1.f;
    for(int y=int(std::floor(y0-m)); y<=int(std::ceil(y1+m)); ++y)
      for(int x=int(std::floor(x0-m)); x<=int(std::ceil(x1+m)); ++x) {
        const Vec2 q = Vec2(float(x),float(y));
        float      d = 1e9f;
        for(size_t i=0; i+1<pt.size(); ++i) {
          const Vec2  ab = pt[i+1]-pt[i];
          const float ll = ab.x*ab.x+ab.y*ab.y;
          const float t  = ll>0.f ? std::clamp(((q.x-pt[i].x)*ab.x+(q.y-pt[i].y)*ab.y)/ll,0.f,1.f) : 0.f;
          d = std::min(d,(pt[i]+ab*t-q).length());
          }
        const float edge = std::clamp(pen.half+pen.edge+0.5f-d,0.f,1.f);
        const float core = std::clamp(pen.half+0.5f-d,0.f,1.f);
        if(edge>0.f)
          blend(x,y,{0,0,0,uint8_t(255.f*alpha*pen.outline*edge)});
        if(core>0.f)
          blend(x,y,{255,255,255,uint8_t(255.f*alpha*core)});
        }
    }

int Canvas::text(int x, int y, std::string_view s, int sc, Rgba c) {
    rect(x-sc,y-sc,int(s.size())*4*sc+sc,7*sc,Shade);
    for(char ch:s) {
      ch = char(std::toupper(uint8_t(ch)));
      for(auto& g:glyphs) {
        if(g.c!=ch)
          continue;
        for(int j=0; j<5; ++j)
          for(int i=0; i<3; ++i)
            if(g.rows[j*3+i]=='X')
              rect(x+i*sc,y+j*sc,sc,sc,c);
        }
      x += 4*sc;
      }
    return x;
    }

VideoPipe::VideoPipe(std::FILE* f):f(f),th([this]{ loop(); }) {}

VideoPipe::~VideoPipe() {
      {
        std::lock_guard<std::mutex> g(sync);
        done = true;
      }
      cv.notify_all();
      th.join();
      pclose(f);
      }

void VideoPipe::push(std::vector<uint8_t>&& frame) {
      std::unique_lock<std::mutex> g(sync);
      cv.wait(g,[this]{ return queue.size()<MaxQueued; });
      queue.push_back(std::move(frame));
      cv.notify_all();
      }

void VideoPipe::loop() {
      for(;;) {
        std::vector<uint8_t> frame;
        {
          std::unique_lock<std::mutex> g(sync);
          cv.wait(g,[this]{ return done || !queue.empty(); });
          if(queue.empty())
            return;
          frame = std::move(queue.front());
          queue.pop_front();
        }
        cv.notify_all();
        std::fwrite(frame.data(),1,frame.size(),f);
        }
      }

void writeWavHeader(std::FILE* f, uint32_t rate, uint64_t frames, uint16_t channels) {
  const uint16_t pcm = 1, align = uint16_t(channels*2), bits = 16;
  const uint32_t data = uint32_t(frames*align), riff = 36+data, fmtSize = 16, byteRate = rate*align;
  std::fseek(f,0,SEEK_SET);
  std::fwrite("RIFF",1,4,f); std::fwrite(&riff,4,1,f); std::fwrite("WAVE",1,4,f);
  std::fwrite("fmt ",1,4,f); std::fwrite(&fmtSize,4,1,f); std::fwrite(&pcm,2,1,f); std::fwrite(&channels,2,1,f);
  std::fwrite(&rate,4,1,f); std::fwrite(&byteRate,4,1,f); std::fwrite(&align,2,1,f); std::fwrite(&bits,2,1,f);
  std::fwrite("data",1,4,f); std::fwrite(&data,4,1,f);
  }

void stopRecording(const CameraContext& ctx) {
  if(!ctx.camera.rec)
    return;
  for(auto& stem:ctx.camera.voiceStems) {
    std::lock_guard<std::mutex> guard(stem->sync);
    stem->stopped = true;
    // OpenAL may request resampler lookahead. Keep only the part within the segment.
    const uint64_t available = ctx.camera.audioFrames>=stem->sample ? ctx.camera.audioFrames-stem->sample : 0;
    const uint64_t limit = available*stem->rate/SoundDevice::loopbackRate();
    if(stem->pcm.size()>limit)
      stem->pcm.resize(size_t(limit));
    if(auto* file = std::fopen(stem->path.c_str(),"wb")) {
      writeWavHeader(file,stem->rate,stem->pcm.size(),1);
      std::fwrite(stem->pcm.data(),sizeof(int16_t),stem->pcm.size(),file);
      std::fclose(file);
      }
    stem->pcm.clear();
    }
  ctx.camera.voiceStems.clear();
  ctx.overlay.video.reset();  // waits for the frames still queued
  if(ctx.camera.audio!=nullptr) {
    writeWavHeader(ctx.camera.audio,SoundDevice::loopbackRate(),ctx.camera.audioFrames);
    std::fclose(ctx.camera.audio);
    }
  ctx.camera.audio = nullptr;
  // how fast the game ran against the wall clock while recording (the AI server keeps to the wall clock)
  const auto wall  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-ctx.camera.recWall).count();
  const int  speed = wall>0 ? int((ctx.time-ctx.camera.recStart)*100/uint64_t(wall)) : 100;
  event("rec_stop","\"frames\":" + std::to_string(ctx.camera.recFrame) + ",\"speed\":" + std::to_string(speed));
  say("rec stop " + ctx.camera.recName + " frames=" + std::to_string(ctx.camera.recFrame) +
      (ctx.realtime ? " speed=" + std::to_string(speed) + "%" : std::string()));
  ctx.camera.rec = false;
  }

void drawLeg(Canvas& cv, const Matrix4x4& vp, const FootIk::Probe::Leg& l, Rgba color) {
  auto proj = [&](const Vec3& p, float& x, float& y) {
    float px = p.x, py = p.y, pz = p.z, pw = 1;
    vp.project(px,py,pz,pw);
    if(pw<=1e-3f)
      return false;
    x = (0.5f*px/pw+0.5f)*float(cv.w);
    y = (0.5f*py/pw+0.5f)*float(cv.h);
    return true;
    };
  auto seg = [&](const Vec3& a, const Vec3& b, int r, Rgba c) {
    float x0,y0,x1,y1;
    if(proj(a,x0,y0) && proj(b,x1,y1))
      cv.line(x0,y0,x1,y1,r,c);
    };
  auto pt = [&](const Vec3& a, int r, Rgba c) {
    float x,y;
    if(proj(a,x,y))
      cv.dot(x,y,r,c);
    };

  // the animation before IK: thin white
  for(size_t k=0; k<3; ++k)
    seg(l.anim[k],l.anim[k+1],0,White);
  // after IK: thick, in the leg color
  for(size_t k=0; k<3; ++k)
    seg(l.final[k],l.final[k+1],1,color);
  for(size_t k=1; k<4; ++k)
    pt(l.final[k],2,color);

  // ground under the ankle and the toes: green under a planted foot, yellow under a swinging one
  const Rgba g = l.plant>0.5f ? Plant : Swing;
  if(l.hasCol) {
    const Vec3 at = {l.anim[2].x, l.groundY, l.anim[2].z};
    seg(at-Vec3(8,0,0),at+Vec3(8,0,0),0,g);
    seg(at-Vec3(0,0,8),at+Vec3(0,0,8),0,g);
    seg(at,at+l.n*15.f,0,g);
    }
  if(l.toeCol)
    pt({l.anim[3].x, l.toeY, l.anim[3].z},1,g);
  }

// stretches the brightness so that the brightest 2% of the image are near white
void autoExposure(Canvas& cv) {
  uint32_t hist[256] = {};
  uint32_t n = 0;
  for(int y=0; y<cv.h; y+=4)
    for(int x=0; x<cv.w; x+=4) {
      const uint8_t* p = cv.px + (size_t(y)*size_t(cv.w) + size_t(x))*4;
      hist[(p[0]*54 + p[1]*183 + p[2]*19) >> 8]++;
      ++n;
      }
  uint32_t acc = 0;
  int      top = 255;
  for(; top>0; --top) {
    acc += hist[top];
    if(acc>n/50)
      break;
    }
  const float gain = std::clamp(230.f/float(std::max(top,1)), 1.f, 6.f);
  if(gain<1.05f)
    return;
  uint8_t lut[256];
  for(int i=0; i<256; ++i)
    lut[i] = uint8_t(std::min(255.f, 255.f*std::pow(std::min(1.f, float(i)*gain/255.f), 0.85f)));
  const size_t size = size_t(cv.w)*size_t(cv.h)*4;
  for(size_t i=0; i<size; i+=4) {
    cv.px[i+0] = lut[cv.px[i+0]];
    cv.px[i+1] = lut[cv.px[i+1]];
    cv.px[i+2] = lut[cv.px[i+2]];
    }
  }

// hit zones of an npc (showzones): capsule outlines, the head red, the trunk yellow, the limbs blue
void drawZones(const OverlayContext& ctx,Canvas& cv, Camera* cam) {
  if(cam!=nullptr && (!ctx.overlay.zonesNpc.empty() || ctx.living.livingDebug)) {
    auto* w = Gothic::inst().world();
    const auto actors=ctx.living.livingDebug ? ctx.crew : std::vector<Npc*>{ctx.overlay.zonesNpc=="player" ? Gothic::inst().player() : (w!=nullptr ? npcBySymbol(*w,ctx.overlay.zonesNpc) : nullptr)};
    for(auto* n:actors) if(n!=nullptr) {
      const Matrix4x4 vp = cam->viewProj();
      auto proj = [&](const Vec3& p, float& x, float& y) {
        float px = p.x, py = p.y, pz = p.z, pw = 1;
        vp.project(px,py,pz,pw);
        if(pw<=1e-3f)
          return false;
        x = (0.5f*px/pw+0.5f)*float(cv.w);
        y = (0.5f*py/pw+0.5f)*float(cv.h);
        return true;
        };
      std::vector<HitZones::Posed> caps;
      n->hitZonesPosed(caps);
      for(auto& c:caps) {
        const Rgba col = c.zone==HitZones::Head ? Rgba{255,60,60,230} : (c.zone==HitZones::Body ? Rgba{255,220,40,170} : Rgba{60,190,255,170});
        float ax,ay,bx,by,ux,uy;
        if(!proj(c.a,ax,ay) || !proj(c.b,bx,by) || !proj(c.a+Vec3(0,c.r,0),ux,uy))
          continue;
        const float r  = std::hypot(ux-ax,uy-ay);
        float       vx = bx-ax, vy = by-ay;
        const float l  = std::hypot(vx,vy);
        for(int i=0; i<32; ++i) {
          const float t0 = float(i)*float(M_PI)/16.f, t1 = float(i+1)*float(M_PI)/16.f;
          cv.line(ax+r*std::cos(t0),ay+r*std::sin(t0),ax+r*std::cos(t1),ay+r*std::sin(t1),0,col);
          if(l>0.5f)
            cv.line(bx+r*std::cos(t0),by+r*std::sin(t0),bx+r*std::cos(t1),by+r*std::sin(t1),0,col);
          }
        if(l>0.5f) {
          vx /= l;
          vy /= l;
          cv.line(ax-vy*r,ay+vx*r,bx-vy*r,by+vx*r,0,col);
          cv.line(ax+vy*r,ay-vx*r,bx+vy*r,by-vx*r,0,col);
          }
        }
      }
    }
  }

// `cam two arena`: a mark over the arena's primary, the body under test (the opponent is native)
void drawArenaMark(Pixmap& pm) {
  const auto actors = BodyArena::actors();
  auto       cam    = Gothic::inst().camera();
  if(actors.size()<2 || cam==nullptr)
    return;
  Canvas cv;
  cv.px = reinterpret_cast<uint8_t*>(pm.data());
  cv.w  = int(pm.w());
  cv.h  = int(pm.h());
  const Vec3 at = actors[0]->mapHeadBone() + Vec3(0,40,0);
  float x = at.x, y = at.y, z = at.z, ww = 1;
  cam->viewProj().project(x,y,z,ww);
  if(ww<=1e-3f)
    return;
  cv.dot((0.5f*x/ww+0.5f)*float(cv.w),(0.5f*y/ww+0.5f)*float(cv.h),6,{255,200,30,255});
  }

// the arrows' ways (overlay trail): gold lines from the bow to where each flew, fading out after it stopped
void drawTrails(const OverlayContext& ctx,Canvas& cv, Camera* cam) {
  if(cam==nullptr)
    return;
  const Matrix4x4 vp = cam->viewProj();
  auto proj = [&](const Vec3& p, float& x, float& y) {
    float px = p.x, py = p.y, pz = p.z, pw = 1;
    vp.project(px,py,pz,pw);
    if(pw<=1e-3f)
      return false;
    x = (0.5f*px/pw+0.5f)*float(cv.w);
    y = (0.5f*py/pw+0.5f)*float(cv.h);
    return true;
    };
  const int thick = cv.h>1200 ? 2 : 1;
  for(auto& tr:ctx.overlay.trails) {
    const uint64_t age = ctx.time>tr.last ? ctx.time-tr.last : 0;
    if(age>ctx.overlay.trailKeep || tr.pts.size()<2)
      continue;
    const float  k   = 1.f - float(age)/float(std::max<uint64_t>(ctx.overlay.trailKeep,1));
    const Rgba   col = {255,214,120,uint8_t(235.f*k)};
    for(size_t i=1; i<tr.pts.size(); ++i) {
      float x0,y0,x1,y1;
      if(proj(tr.pts[i-1],x0,y0) && proj(tr.pts[i],x1,y1))
        cv.line(x0,y0,x1,y1,thick,col);
      }
    float x,y;
    if(age==0 && proj(tr.pts.back(),x,y))
      cv.dot(x,y,thick+1,{255,255,255,235});
    if(!ctx.overlay.trailLine)
      continue;
    // the bow's line: the first step of the flight, drawn straight on as far (across the ground) as the arrow
    // has flown; under it the arrow's way bends down
    Vec3 d = tr.pts[1]-tr.pts[0];
    const float dh = std::hypot(d.x,d.z);
    const float fh = std::hypot(tr.pts.back().x-tr.pts[0].x,tr.pts.back().z-tr.pts[0].z);
    if(dh<1e-3f || fh<1.f)
      continue;
    const Vec3   end = tr.pts[0] + d*(fh/dh);
    const Rgba   wc  = {255,255,255,uint8_t(200.f*k)};
    const int    n   = 40;
    for(int i=0; i<n; i+=2) {
      float x0,y0,x1,y1;
      const Vec3 p0 = tr.pts[0] + (end-tr.pts[0])*(float(i)/float(n));
      const Vec3 p1 = tr.pts[0] + (end-tr.pts[0])*(float(i+1)/float(n));
      if(proj(p0,x0,y0) && proj(p1,x1,y1))
        cv.line(x0,y0,x1,y1,thick,wc);
      }
    }
  }

void drawOverlay(const OverlayContext& ctx,Pixmap& pm) {
  Canvas cv;
  cv.px = reinterpret_cast<uint8_t*>(pm.data());
  cv.w  = int(pm.w());
  cv.h  = int(pm.h());
  if(cv.h>0)
    ctx.overlay.aspect = float(cv.w)/float(cv.h);
  auto  cam = Gothic::inst().camera();
  auto& p   = FootIk::playerProbe;
  if(ctx.overlay.bright)
    autoExposure(cv);
  if(ctx.overlay.zonesOnly || ctx.overlay.trailOn) {
    drawZones(ctx,cv,cam);
    drawTrails(ctx,cv,cam);
    return;
    }

  float zx = -1, zy = -1;
  if(cam!=nullptr && p.valid) {
    const Matrix4x4 vp = cam->viewProj();
    for(size_t i=0; i<p.numLegs; ++i)
      drawLeg(cv,vp,p.leg[i],LegColor[i]);
    // zoom center: between the feet
    Vec3 m = {};
    for(size_t i=0; i<p.numLegs; ++i)
      m += p.leg[i].final[2]*(1.f/float(p.numLegs));
    float x = m.x, y = m.y, z = m.z, ww = 1;
    vp.project(x,y,z,ww);
    if(ww>1e-3f) {
      zx = (0.5f*x/ww+0.5f)*float(cv.w);
      zy = (0.5f*y/ww+0.5f)*float(cv.h);
      }
    }

  // gaze: where the face looks (yellow) and where the partner is (red)
  const auto& g = ctx.subject!=nullptr ? Gaze::subjectProbe : Gaze::playerProbe;
  if(cam!=nullptr && g.valid && g.active) {
    const Matrix4x4 vp = cam->viewProj();
    auto proj = [&](const Vec3& p, float& x, float& y) {
      float px = p.x, py = p.y, pz = p.z, pw = 1;
      vp.project(px,py,pz,pw);
      if(pw<=1e-3f)
        return false;
      x = (0.5f*px/pw+0.5f)*float(cv.w);
      y = (0.5f*py/pw+0.5f)*float(cv.h);
      return true;
      };
    float x0,y0,x1,y1,x2,y2;
    const bool h = proj(g.head,x0,y0);
    if(h && proj(g.head+g.facing*60.f,x1,y1))
      cv.line(x0,y0,x1,y1,1,{255,230,40,255});
    if(h && proj(g.target,x2,y2)) {
      cv.line(x0,y0,x2,y2,0,{255,80,80,140});
      cv.dot(x2,y2,3,Red);
      }
    }

  drawZones(ctx,cv,cam);

  // the manual aim: the crosshair as the game draws it in its interface (not captured): a chevron at where
  // it aims, brackets as far apart as the arrow may go off; a dot where the arrow points now (debugging)
  auto  hero = Gothic::inst().player();
  Vec3  cross;
  float spread = 0;
  if(cam!=nullptr && hero!=nullptr && ctx.ctrl!=nullptr && ctx.ctrl->crosshair(cross,spread)) {
    const Matrix4x4 vp = cam->viewProj();
    auto proj = [&](const Vec3& p, float& x, float& y) {
      float px = p.x, py = p.y, pz = p.z, pw = 1;
      vp.project(px,py,pz,pw);
      if(pw<=1e-3f)
        return false;
      x = (0.5f*px/pw+0.5f)*float(cv.w);
      y = (0.5f*py/pw+0.5f)*float(cv.h);
      return true;
      };
    Vec3  pos, dir;
    float x, y;
    if(hero->arrowRay(pos,dir)) {
      auto r = hero->world().physic()->rayArrow(pos,pos+dir*10000.f,[hero](Npc& n, const Vec3& a, const Vec3& b, float& t) {
        return &n!=hero && n.rayHitZone(a,b,t)!=HitZones::None;
        });
      if(proj(r.hasCol ? r.v : pos+dir*10000.f,x,y))
        cv.dot(x,y,1,{255,120,120,200});
      }
    const bool centered = ctx.ctrl->isCrosshairCentered();
    if(centered) {
      x = 0.5f*float(cv.w);
      y = 0.5f*float(cv.h);
      }
    if(centered || proj(cross,x,y)) {
      const float r = Crosshair::radius(spread,cam->fovY(),float(cv.h),ctx.overlay.uiScale);
      for(auto& l:Crosshair::shape(x,y,r,ctx.overlay.uiScale))
        cv.stroke(l.pt,l.alpha,Crosshair::pen(ctx.overlay.uiScale));
      }
    }

  // zoomed copy of the feet in the bottom right corner
  if(ctx.overlay.zoom && zx>=0 && zy>=0 && zx<float(cv.w) && zy<float(cv.h)) {
    const int sc = 2;
    const int sw = cv.w/5, sh = cv.h/5;
    const int sx = std::clamp(int(zx)-sw/2, 0, cv.w-sw);
    const int sy = std::clamp(int(zy)-sh/2, 0, cv.h-sh);
    std::vector<uint8_t> src(size_t(sw*sh*4));
    for(int j=0; j<sh; ++j)
      std::memcpy(&src[size_t(j*sw*4)], cv.px + (size_t(sy+j)*size_t(cv.w) + size_t(sx))*4, size_t(sw*4));
    const int dx = cv.w - sw*sc - 4, dy = cv.h - sh*sc - 4;
    cv.rect(dx-2,dy-2,sw*sc+4,sh*sc+4,{255,255,255,255});
    for(int j=0; j<sh*sc; ++j) {
      uint8_t*       row = cv.px + (size_t(dy+j)*size_t(cv.w) + size_t(dx))*4;
      const uint8_t* s   = &src[size_t((j/sc)*sw*4)];
      for(int i=0; i<sw*sc; ++i)
        std::memcpy(row+i*4, s+(i/sc)*4, 4);
      }
    }

  // text
  char buf[160];
  std::snprintf(buf,sizeof(buf),"F%llu T%.2f",(unsigned long long)ctx.camera.recFrame,double(ctx.time-ctx.camera.recStart)/1000.0);
  cv.text(6,6,buf,3,Text);
  cv.text(6,30,ctx.camera.recName,2,Text);
  if(!ctx.label.empty())
    cv.text(6,46,ctx.label,2,Text);

  int y = 6;
  const int x = cv.w - 4*2*24 - 6;
  if(p.numLegs>2)
    std::snprintf(buf,sizeof(buf),"IK %s W%.2f P%.1f C%.1f",p.enable ? "ON" : "OFF",double(p.weight),double(p.pelvis),double(p.chest)); else
    std::snprintf(buf,sizeof(buf),"IK %s W%.2f P%.1f",p.enable ? "ON" : "OFF",double(p.weight),double(p.pelvis));
  cv.text(x,y,buf,2,Text);
  y += 16;
  for(size_t i=0; i<p.numLegs; ++i) {
    auto& l = p.leg[i];
    std::snprintf(buf,sizeof(buf),"%s %s E%.1f K%.0f",LegName[i],l.plant>0.5f ? "PLANT" : "SWING",double(l.err),double(l.kneeDev));
    cv.text(x,y,buf,2,LegColor[i]);
    y += 16;
    }
  if(g.valid && (g.active || ctx.subject!=nullptr)) {
    std::snprintf(buf,sizeof(buf),"GAZE W%.2f E%.0f Y%.0f P%.0f",double(g.weight),double(g.error),double(g.yaw),double(g.pitch));
    cv.text(x,y,buf,2,{255,230,40,255});
    y += 16;
    std::snprintf(buf,sizeof(buf),"H%.0f C%.0f W%.0f",double(g.headYaw),double(g.chestYaw),double(g.waistYaw));
    cv.text(x,y,buf,2,{255,230,40,255});
    y += 16;
    }
  const auto& am = ctx.subject!=nullptr ? Aim::subjectProbe : Aim::playerProbe;
  if(am.valid && am.active) {
    std::snprintf(buf,sizeof(buf),"AIM W%.2f E%.1f Y%.0f P%.0f T%.0f B%.0f L%.0f S%.0f",double(am.weight),double(am.error),double(am.yaw),
                  double(am.pitch),double(am.turn),double(am.bend),double(am.lean),double(am.side));
    cv.text(x,y,buf,2,{255,140,60,255});
    y += 16;
    }
  const auto& mp = Archer::playerProbe;
  if(mp.valid && ctx.subject==nullptr) {
    std::snprintf(buf,sizeof(buf),"BOW %s D%.2f H%.1f F%.2f B%.1f%s%s A%.2f",archerStage(mp.stage),double(mp.draw),double(mp.hold),
                  double(mp.fatigue),double(mp.breath),mp.holding ? " HOLD" : "",mp.short_ ? " SHORT" : "",double(mp.amp));
    cv.text(x,y,buf,2,{140,200,255,255});
    y += 16;
    }
  const auto& bd = BowDraw::playerProbe;
  if(bd.valid && bd.weight>0.f && ctx.subject==nullptr) {
    std::snprintf(buf,sizeof(buf),"DRAW%s W%.2f D%.2f S%.0f T%.0f M%.1f",BowDraw::isEnabled() ? "" : " OFF",double(bd.weight),
                  double(bd.draw),double(bd.span),double(bd.travel),double(bd.miss));
    cv.text(x,y,buf,2,{140,200,255,255});
    y += 16;
    }
  const auto& fl = ctx.subject!=nullptr ? Flinch::subjectProbe : Flinch::playerProbe;
  if(fl.valid && (fl.active || fl.hits>0)) {
    std::snprintf(buf,sizeof(buf),"HIT%s #%u S%.2f H%.2f B%.1f Y%.0f T%.1f N%.1f",Flinch::isEnabled() ? "" : " OFF",fl.hits,
                  double(fl.strength),double(fl.height),double(fl.bend),double(fl.bendYaw),double(fl.twist),double(fl.head));
    cv.text(x,y,buf,2,{255,90,90,255});
    y += 16;
    }
  // lips: shapes and intensity, then the voice (thin bar) and the mouth as shown (thick bar)
  const auto& lp = LipSync::subjectProbe;
  if(lp.valid && LipSync::probeNpc!=nullptr) {
    const Rgba c = {120,230,255,255};
    std::snprintf(buf,sizeof(buf),"LIPS %s F%u F%u A%.1f I%.2f",lp.active ? "ON" : "OFF",lp.shape.frame0,lp.shape.frame1,
                  double(lp.shape.alpha),double(lp.shape.intensity));
    cv.text(x,y,buf,2,c);
    y += 16;
    const int bw = 160;
    cv.rect(x,y,bw,14,Shade);
    cv.rect(x,y+1,int(float(bw)*std::clamp(lp.level,0.f,1.f)),3,{255,255,255,255});
    cv.rect(x,y+6,int(float(bw)*std::clamp(lp.open,0.f,1.f)),7,c);
    y += 20;
    }
  const auto& bk = EyeBlink::subjectProbe;
  if(bk.valid && EyeBlink::probeNpc!=nullptr) {
    std::snprintf(buf,sizeof(buf),"BLINK%s N%u P%u S%u D%u",bk.enabled ? "" : " OFF",bk.blinks,bk.pauses,bk.shifts,bk.doubles);
    cv.text(x,y,buf,2,{200,200,255,255});
    y += 16;
    }
  const auto& fr = FaceRig::subjectProbe;
  if(fr.valid && FaceRig::probeNpc!=nullptr) {
    std::snprintf(buf,sizeof(buf),"FACE T%u %.0fus %s",fr.targets,double(fr.blendUs),fr.shown.empty() ? "-" : fr.shown.c_str());
    cv.text(x,y,buf,2,{255,170,200,255});
    y += 16;
    }
  const auto& ge = TalkGestures::subjectProbe;
  if(ge.valid && TalkGestures::probeNpc!=nullptr) {
    const auto mood = TalkGestures::moodName(ge.mood);
    std::snprintf(buf,sizeof(buf),"GEST%s %s G%02u %.*s S%u C%u X%u N%u P%.1f",ge.enabled ? "" : " OFF",
                  ge.speaking ? "TALK" : (ge.thinking ? "THINK" : (ge.listening ? "LISTEN" : "-")),
                  unsigned(ge.gesture),int(mood.size()),mood.data(),ge.starts,ge.cuts,ge.stops,ge.nods,
                  double(ge.head.pitch*180.f/float(M_PI)));
    cv.text(x,y,buf,2,{255,200,120,255});
    y += 16;
    }
  const auto& ln = ctx.subject!=nullptr ? Lean::subjectProbe : Lean::playerProbe;
  if(ln.valid) {
    std::snprintf(buf,sizeof(buf),"LEAN%s R%.1f P%.1f V%.0f T%.0f S%.0f",Lean::isEnabled() ? "" : " OFF",double(ln.roll),
                  double(ln.pitch),double(ln.speed),double(ln.turn),double(ln.slope));
    cv.text(x,y,buf,2,{120,255,160,255});
    y += 16;
    }
  const auto& an = ctx.subject!=nullptr ? Pose::subjectProbe : Pose::playerProbe;
  if(an.valid) {
    std::snprintf(buf,sizeof(buf),"BLEND%s N%u A%.0f",AnimBlend::isEnabled() ? "" : " OFF",an.blend.active,double(an.blend.angle));
    cv.text(x,y,buf,2,{200,160,255,255});
    y += 16;
    }
  if(ctx.camera.occl!=0) {
    std::snprintf(buf,sizeof(buf),"HIDDEN %s%s",(ctx.camera.occl&1) ? "L" : "",(ctx.camera.occl&2) ? "R" : "");
    cv.text(x,y,buf,2,Red);
    }
  }

void writeVideo(const OverlayContext& ctx,const Pixmap& pm) {
  if(ctx.overlay.video==nullptr) {
    ctx.overlay.videoW = pm.w() & ~1u;
    ctx.overlay.videoH = pm.h() & ~1u;
    const char* ff = envStr("OPENGOTHIC_BENCH_FFMPEG") ? envStr("OPENGOTHIC_BENCH_FFMPEG") : "ffmpeg";
    char cmd[2048];
    // OPENGOTHIC_BENCH_ENCODER: ffmpeg's video encoder options instead of x264 (the hardware encoder keeps
    // a large frame in real time: "-c:v h264_videotoolbox -b:v 24M")
    const char* enc = envStr("OPENGOTHIC_BENCH_ENCODER") ? envStr("OPENGOTHIC_BENCH_ENCODER") : "-c:v libx264 -preset veryfast -crf 18";
    std::snprintf(cmd,sizeof(cmd),
                  "\"%s\" -loglevel error -y -f rawvideo -pix_fmt rgba -s %ux%u -framerate %d -i - "
                  "%s -pix_fmt yuv420p \"%s/%s.mp4\"",
                  ff,ctx.overlay.videoW,ctx.overlay.videoH,Fps,enc,outDir.c_str(),ctx.camera.recName.c_str());
    std::FILE* f = popen(cmd,"w");
    if(f==nullptr) {
      say("can not start ffmpeg");
      ctx.camera.recVideo = false;
      return;
      }
    ctx.overlay.video = std::make_unique<VideoPipe>(f);
    }
  const size_t row = size_t(ctx.overlay.videoW)*4;
  std::vector<uint8_t> frame(row*ctx.overlay.videoH,0);
  if(pm.w()>=ctx.overlay.videoW && pm.h()>=ctx.overlay.videoH && pm.data()!=nullptr) {
    auto px = reinterpret_cast<const uint8_t*>(pm.data());
    for(uint32_t j=0; j<ctx.overlay.videoH; ++j)
      std::memcpy(frame.data() + j*row, px + size_t(j)*size_t(pm.w())*4, row);
    }
  ctx.overlay.video->push(std::move(frame));
  }

bool overlayCommand(const OverlayContext& ctx,const std::vector<std::string>& a) {
  const auto& c=a[0];
  if(c=="overlay") {
    const std::string m = a.size()>1 ? a[1] : "on";
    const bool on = !(a.size()>2 && a[2]=="off");
    if(m=="zoom")
      ctx.overlay.zoom = on;
    else if(m=="bright")
      ctx.overlay.bright = on;
    else if(m=="zones") {
      ctx.overlay.zonesOnly = on; // the hit zones only, no legs, no text
      ctx.overlay.overlay   = ctx.overlay.overlay || on;
      }
    else if(m=="trail") {
      // overlay trail [off] [keep ms] [line]: the arrows' ways only (with the zones of showzones), no legs, no
      // text; `line`: also the straight line it left along (where the bow pointed), dashed
      ctx.overlay.trailOn   = on;
      ctx.overlay.trailKeep = a.size()>2 && a[2]!="off" && a[2]!="line" ? uint64_t(num(a,2,1500)) : ctx.overlay.trailKeep;
      ctx.overlay.trailLine = on && std::find(a.begin(),a.end(),"line")!=a.end();
      ctx.overlay.overlay   = ctx.overlay.overlay || on;
      if(!on)
        ctx.overlay.trails.clear();
      }
    else
      ctx.overlay.overlay = (m!="off");
    }
  else if(c=="showzones") {
    // showzones <npc symbol>|player|off: the overlay draws the npc's hit zones (capsules on its bones: the head
    // red, the trunk yellow, the limbs blue)
    ctx.overlay.zonesNpc = a.size()>1 && a[1]!="off" ? a[1] : "";
    say("showzones " + (ctx.overlay.zonesNpc.empty() ? std::string("off") : ctx.overlay.zonesNpc));
    }
  else {
    return false;
    }
  return true;
  }

}
