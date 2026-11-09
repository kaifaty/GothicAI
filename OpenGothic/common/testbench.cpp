#include "appcadence.h"
// resources.cpp: texture residency of a deterministic bench (`deterministic on`)
extern bool benchTexturesSettle;
extern bool benchTexturesFrozen;

#include "testbench.h"
#include "testbench_internal.h"
#include "ui/gamemenu.h"
#include "navbench.h"
#include "navdemos.h"
#include "npcdayjournal.h"
#include "frameprofile.h"
#include "loadprobe.h"

#include <Tempest/Pixmap>
#include <Tempest/SoundDevice>
#include <Tempest/SystemApi>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
// The MSVC runtime names the POSIX pipe functions with an underscore (bench video capture only).
#define popen _popen
#define pclose _pclose
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "game/gamescript.h"
#include "game/damagecalculator.h"
#include "game/herodeed.h"
#include "game/playercontrol.h"
#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/bodyarena.h"
#include "game/bodyreplay.h"
#include "game/navigationeyebench.h"
#include "game/navigationmemorybench.h"
#include "game/knownroadsbench.h"
#include "game/npckeysbench.h"
#include "game/bodytracesbench.h"
#include "game/bloodstainsbench.h"
#include "game/serialize.h"
#include "game/ragdollsave.h"
#include "game/definitions/sounddefinitions.h"
#include "resources.h"
#include "graphics/drawcommands.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/gaze.h"
#include "graphics/mesh/lipsync.h"
#include "graphics/mesh/talkgestures.h"
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/lean.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/animblend.h"
#include "graphics/mesh/aim.h"
#include "graphics/mesh/animationsolver.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/mesh/hitzones.h"
#include "game/archery.h"
#include "game/archerai.h"
#include "graphics/mesh/flinch.h"
#include "graphics/mesh/skeleton.h"
#include "physics/dynamicworld.h"
#include "physics/cloth.h"
#include "physics/physicmesh.h"
#include "physics/physicmeshshape.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "world/triggers/movetrigger.h"
#include "physics/ragdoll.h"
#include "physics/physicsnoise.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/objects/sound.h"
#include "graphics/objvisual.h"
#include "graphics/worldview.h"
#include "graphics/mesh/protomesh.h"
#include "world/movablevobs.h"
#include "world/world.h"
#include "world/chunkresidency.h"
#include "world/npcsimtier.h"
#include "world/bullet.h"
#include "world/waypoint.h"
#include "world/aiqueue.h"
#include "ui/crosshair.h"
#include "utils/keycodec.h"
#include "camera.h"
#include "gothic.h"
#include "marvin.h"

using namespace Tempest;
using namespace testbench_detail;

namespace testbench_detail {

const char* envStr(const char* name) {
  const char* v = std::getenv(name);
  return (v!=nullptr && v[0]!='\0') ? v : nullptr;
  }

}

namespace {

const char*       scenarioPath = envStr("OPENGOTHIC_BENCH");
}

namespace testbench_detail {

std::string       outDir       = envStr("OPENGOTHIC_BENCH_OUT") ? envStr("OPENGOTHIC_BENCH_OUT") : ".";

}

namespace {
const bool        keepRunning  = envStr("OPENGOTHIC_BENCH_KEEP")!=nullptr;
constexpr int     Fps          = 30;
// `deterministic on`: steps between the end of `streaming wait` and the next command; the TAA history of the shot
// holds all its jitter phases twice
constexpr uint32_t DeterministicSettleSteps = 16;
// the fixed steps keep to the wall clock (never faster): the AI server and its voices live in real time
const bool        realtime     = envStr("OPENGOTHIC_BENCH_REALTIME")!=nullptr;
bool              bodyTrace    = envStr("OPENGOTHIC_BODY_TRACE")!=nullptr &&
                                std::string_view(envStr("OPENGOTHIC_BODY_TRACE"))=="1";
// Trivial process-lifetime guard: NPCs may be destroyed after Bench's nontrivial members at shutdown.
bool              bodyTraceAlive = true;
// Keep the established regression CSV unchanged unless the stage 2 trace is requested.
const bool        ragdollTrace = envStr("OPENGOTHIC_RAGDOLL_TRACE")!=nullptr &&
                                std::string_view(envStr("OPENGOTHIC_RAGDOLL_TRACE"))=="1";
const bool        ragdollPerf = envStr("OPENGOTHIC_RAGDOLL_PERF")!=nullptr &&
                               std::string_view(envStr("OPENGOTHIC_RAGDOLL_PERF"))=="1";
}

namespace testbench_detail {

const bool        npcCharacterTrace = envStr("OPENGOTHIC_NPC_CHARACTER_TRACE")!=nullptr &&
                                     std::string_view(envStr("OPENGOTHIC_NPC_CHARACTER_TRACE"))=="1";
}

namespace {

constexpr uint64_t SpeakTailMs = 400;  // the talk key is held this long after the recording ends

std::FILE* logFile = nullptr;

}

namespace testbench_detail {

void say(const std::string& s) {
  std::printf("[bench] %s\n",s.c_str());
  std::fflush(stdout);
  if(logFile==nullptr)
    logFile = std::fopen((outDir+"/bench.log").c_str(),"w");
  if(logFile!=nullptr) {
    std::fprintf(logFile,"%s\n",s.c_str());
    std::fflush(logFile);
    }
  }

}

namespace testbench_detail {

std::vector<std::string> split(std::string_view s) {
  std::vector<std::string> ret;
  size_t i = 0;
  while(i<s.size()) {
    while(i<s.size() && std::isspace(uint8_t(s[i])))
      ++i;
    size_t e = i;
    while(e<s.size() && !std::isspace(uint8_t(s[e])))
      ++e;
    if(e>i)
      ret.emplace_back(s.substr(i,e-i));
    i = e;
    }
  return ret;
  }

}

namespace testbench_detail {

float num(const std::vector<std::string>& a, size_t i, float def) {
  return i<a.size() ? std::strtof(a[i].c_str(),nullptr) : def;
  }

bool isNumber(const std::string& s) {
  char* end = nullptr;
  std::strtof(s.c_str(),&end);
  return end!=s.c_str() && *end=='\0';
  }

}

namespace {

std::string rest(const std::string& line) {
  const size_t sp = line.find(' ');
  return sp==std::string::npos ? "" : line.substr(sp+1);
  }

void replaceAll(std::string& s, std::string_view from, std::string_view to) {
  for(size_t p=s.find(from); p!=std::string::npos; p=s.find(from,p+to.size()))
    s.replace(p,from.size(),to);
  }

}

namespace testbench_detail {

std::string jsonStr(std::string_view s) {
  std::string r = "\"";
  for(char ch:s) {
    switch(ch) {
      case '"':  r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\r': r += "\\r"; break;
      case '\t': r += "\\t"; break;
      default:
        if(uint8_t(ch)<0x20) {
          char buf[8];
          std::snprintf(buf,sizeof(buf),"\\u%04x",unsigned(uint8_t(ch)));
          r += buf;
          } else {
          r += ch;
          }
      }
    }
  return r + "\"";
  }

}

namespace {

// length of a 16-bit PCM wav in ms, 0 if it is not one
uint64_t wavMs(const std::string& path) {
  std::ifstream f(path,std::ios::binary);
  char riff[12];
  if(!f.read(riff,12) || std::memcmp(riff,"RIFF",4)!=0 || std::memcmp(riff+8,"WAVE",4)!=0)
    return 0;
  uint32_t byteRate = 0;
  char     hdr[8];
  while(f.read(hdr,8)) {
    uint32_t size = 0;
    std::memcpy(&size,hdr+4,4);
    if(std::memcmp(hdr,"fmt ",4)==0) {
      std::vector<char> fmt(size);
      if(!f.read(fmt.data(),size) || size<16)
        return 0;
      std::memcpy(&byteRate,fmt.data()+8,4);
      }
    else if(std::memcmp(hdr,"data",4)==0) {
      return byteRate>0 ? uint64_t(size)*1000/byteRate : 0;
      }
    else {
      f.seekg(size + (size&1),std::ios::cur);
      }
    }
  return 0;
  }

void writeWavHeader(std::FILE* f, uint32_t rate, uint64_t frames, uint16_t channels = 2) {
  const uint16_t pcm = 1, align = uint16_t(channels*2), bits = 16;
  const uint32_t data = uint32_t(frames*align), riff = 36+data, fmtSize = 16, byteRate = rate*align;
  std::fseek(f,0,SEEK_SET);
  std::fwrite("RIFF",1,4,f); std::fwrite(&riff,4,1,f); std::fwrite("WAVE",1,4,f);
  std::fwrite("fmt ",1,4,f); std::fwrite(&fmtSize,4,1,f); std::fwrite(&pcm,2,1,f); std::fwrite(&channels,2,1,f);
  std::fwrite(&rate,4,1,f); std::fwrite(&byteRate,4,1,f); std::fwrite(&align,2,1,f); std::fwrite(&bits,2,1,f);
  std::fwrite("data",1,4,f); std::fwrite(&data,4,1,f);
  }

}

namespace testbench_detail {

float groundY(World& w, float x, float z, float y) {
  auto r = w.physic()->ray({x,y+150,z},{x,y-300,z});
  return r.hasCol ? r.v.y : NAN;
  }

// slope at a point: angle in degrees, horizontal direction down the slope, roughness (steps)
bool slopeAt(World& w, const Vec3& p, float& angle, Vec3& down, float& rough) {
  const float d  = 40;
  const float h0 = groundY(w,p.x,p.z,p.y);
  const float x0 = groundY(w,p.x-d,p.z,p.y), x1 = groundY(w,p.x+d,p.z,p.y);
  const float z0 = groundY(w,p.x,p.z-d,p.y), z1 = groundY(w,p.x,p.z+d,p.y);
  if(std::isnan(h0) || std::isnan(x0) || std::isnan(x1) || std::isnan(z0) || std::isnan(z1))
    return false;
  const float gx = (x1-x0)/(2*d), gz = (z1-z0)/(2*d);
  const float g  = std::sqrt(gx*gx+gz*gz);
  angle = std::atan(g)*180.f/float(M_PI);
  down  = g>0 ? Vec3(-gx/g,0,-gz/g) : Vec3(1,0,0);
  rough = std::max(std::abs((x0+x1)*0.5f-h0), std::abs((z0+z1)*0.5f-h0));
  return true;
  }

}

namespace testbench_detail {

Vec3 facing(const Npc& pl) {
  const float a = pl.rotation()*float(M_PI)/180.f;
  return {std::cos(a),0,std::sin(a)};
  }

}

namespace {


// Reuse the hero's skeleton prediction at a candidate perch without teleporting him during a search.
Vec3 perchHead(const Npc& pl, const Vec3& origin, const Vec3& forward, const Vec3& target, float angle) {
  const Vec3 f = facing(pl), s(-f.z,0,f.x), side(-forward.z,0,forward.x);
  const Vec3 d = target-origin;
  const Vec3 mapped = pl.position() + f*Vec3::dotProduct(d,forward) + s*Vec3::dotProduct(d,side) + Vec3(0,d.y,0);
  const Vec3 head = pl.aimHead(mapped,angle)-pl.position();
  return origin + forward*Vec3::dotProduct(head,f) + side*Vec3::dotProduct(head,s) + Vec3(0,head.y,0);
  }

bool perchLean(World& w, const Npc& pl, const Vec3& origin, const Vec3& forward, const Vec3& target,
               float minDown, float maxDown, float& down, float& bend) {
  const Vec3 head = perchHead(pl,origin,forward,target,0.f);
  const Vec3 d = target-head;
  down = -std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z))*180.f/float(M_PI);
  const auto blocked = w.physic()->ray(head,target);
  if(down<minDown || down>maxDown || !blocked.hasCol || (blocked.v-head).length()>200.f)
    return false;
  Vec3 previous = head;
  for(float angle=1.f; angle<=Aim::LeanMax; angle+=1.f) {
    const Vec3 next = perchHead(pl,origin,forward,target,angle*float(M_PI)/180.f);
    if(w.physic()->ray(previous,next).hasCol)
      return false;
    if(!w.physic()->ray(next,target).hasCol) {
      bend = angle;
      return true;
      }
    previous = next;
    }
  return false;
  }

struct Spot {
  std::string name;
  float       angle = 0;
  float       rough = 0;
  float       dist  = 0;
  Vec3        pos;
  };

enum SpotKind { K_Slope, K_Stairs, K_Name };

// waypoints of a kind, `count` of them spread from near to far, not closer than 5 m to each other
std::vector<Spot> findSpots(World& w, const Npc& pl, SpotKind kind, float a0, float a1, std::string_view part, size_t count) {
  std::vector<Spot> all;
  const Vec3 at = pl.position();
  w.findWayPoint(at,[&](const WayPoint& wp) {
    if(kind==K_Name && wp.name.find(part)==std::string::npos)
      return false;
    Spot s;
    Vec3 d;
    s.name = wp.name;
    s.pos  = wp.pos;
    s.dist = (wp.pos-at).length();
    if(!slopeAt(w,wp.pos,s.angle,d,s.rough))
      return false;
    if(kind==K_Slope  && !(s.angle>=a0 && s.angle<=a1 && s.rough<4))
      return false;
    if(kind==K_Stairs && !(s.angle>=18 && s.angle<=40 && s.rough>=6 && s.rough<=25))
      return false;
    all.push_back(s);
    return false;
    });
  std::sort(all.begin(),all.end(),[](const Spot& a, const Spot& b){ return a.dist<b.dist; });

  std::vector<Spot> far;
  for(auto& s:all) {
    bool close = false;
    for(auto& f:far)
      close |= (f.pos-s.pos).length()<500;
    if(!close)
      far.push_back(s);
    }
  if(far.size()<=count)
    return far;
  std::vector<Spot> ret;
  for(size_t i=0; i<count; ++i)
    ret.push_back(far[(i*far.size())/count]);
  return ret;
  }

// --- overlay ---------------------------------------------------------------------------------------

struct Rgba { uint8_t r,g,b,a; };

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
const Rgba Text   = {255,255,255,255};

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

struct Canvas {
  uint8_t* px = nullptr;
  int      w  = 0;
  int      h  = 0;

  void blend(int x, int y, Rgba c) {
    if(x<0 || y<0 || x>=w || y>=h)
      return;
    uint8_t*  p = px + (size_t(y)*size_t(w) + size_t(x))*4;
    const int a = c.a;
    p[0] = uint8_t((c.r*a + p[0]*(255-a))/255);
    p[1] = uint8_t((c.g*a + p[1]*(255-a))/255);
    p[2] = uint8_t((c.b*a + p[2]*(255-a))/255);
    p[3] = 255;
    }

  void rect(int x, int y, int rw, int rh, Rgba c) {
    for(int j=y; j<y+rh; ++j)
      for(int i=x; i<x+rw; ++i)
        blend(i,j,c);
    }

  void dot(float x, float y, int r, Rgba c) {
    const int cx = int(std::lround(x)), cy = int(std::lround(y));
    for(int j=-r; j<=r; ++j)
      for(int i=-r; i<=r; ++i)
        if(i*i+j*j<=r*r+r)
          blend(cx+i,cy+j,c);
    }

  void line(float x0, float y0, float x1, float y1, int r, Rgba c) {
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

  // a polyline stroked as the game strokes the crosshair: white over a dark edge, smooth; each pixel once
  void stroke(const std::vector<Vec2>& pt, float alpha, const Crosshair::Pen& pen) {
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

  int text(int x, int y, std::string_view s, int sc, Rgba c) {
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
  };

// frames go to ffmpeg from a thread of their own: the game goes on while a frame is written
class VideoPipe final {
  public:
    explicit VideoPipe(std::FILE* f):f(f),th([this]{ loop(); }) {}
    ~VideoPipe() {
      {
        std::lock_guard<std::mutex> g(sync);
        done = true;
      }
      cv.notify_all();
      th.join();
      pclose(f);
      }

    void push(std::vector<uint8_t>&& frame) {
      std::unique_lock<std::mutex> g(sync);
      cv.wait(g,[this]{ return queue.size()<MaxQueued; });
      queue.push_back(std::move(frame));
      cv.notify_all();
      }

  private:
    static constexpr size_t MaxQueued = 4;

    void loop() {
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

    std::FILE*                       f;
    std::mutex                       sync;
    std::condition_variable          cv;
    std::deque<std::vector<uint8_t>> queue;
    bool                             done = false;
    std::thread                      th;
  };

// --- the bench ---------------------------------------------------------------------------------------

struct Bench {
  bool navigationOnly = false; // navigation trials use the independent stuck journal
  ~Bench() { bodyTraceAlive = false; }
  std::vector<std::string> prog;
  size_t      pc        = 0;
  bool        loaded    = false;
  bool        started   = false;
  bool        done      = false;
  std::string holdCommand;      // --fork-prefix: stop before this command
  bool        heldNow   = false;
  uint64_t    time      = 0;    // game time since the start, ms
  uint64_t    step      = 0;    // fixed steps made
  std::chrono::steady_clock::time_point heartbeatStart, heartbeatNext;

  using NpcTierTest = testbench_detail::NpcTierTest;
  NpcTierTest npcTierTest;

  using NpcSpeedTest = testbench_detail::NpcSpeedTest;
  NpcSpeedTest npcSpeedTest;

  testbench_detail::Healing healing;

  uint64_t    waitUntil = 0;
  std::chrono::steady_clock::time_point realWaitUntil;
  bool        waitArrive = false;
  uint64_t    arriveEnd = 0;

  std::vector<Hold>             holds;
  std::vector<KeyCodec::Action> taps;
  uint64_t                    inputTraceUntil = 0;
  std::string                 inputAnimation;

  Melee melee;

  CamMode     cam       = C_Game;
  float       camDist   = 220;
  float       camHeight = 40;
  float       camAngle  = 90;   // C_Follow: degrees from the facing (0 in front, 90 on the left); C_World: world yaw
  float       camLook   = 45;   // look at this height above the player origin
  bool        camLift   = true; // C_Follow, C_World: kept above the ground under it (off: where it is put,
                                // a floor above it is no ground: under a gallery, in a hall)
  bool        camAuto   = false;
  int         occlTime  = 0;
  Vec3        camPos;
  PointF      camHeldAngles;
  PointF      camTurnAngles;
  float       camTurnDegrees = 0;
  uint64_t    camTurnStart = 0, camTurnDuration = 0;

  bool        rec       = false;
  bool        recVideo  = false;
  std::string recName;
  uint64_t    recFrame  = 0;
  uint64_t    recStart  = 0;
  std::string perfSegment;
  uint64_t    perfFrame = 0;
  bool        perfArmed = false;
  bool        waitStreaming = false;
  std::chrono::steady_clock::time_point streamingDeadline;
  // `deterministic on`: a `shot` does not depend on how many frames the loading took
  // (reports/engine/2026-11-03-streaming-optimisation/deterministic-shots.md)
  bool        deterministic = false;
  bool        frozen        = false; // the world stands still: `streaming wait` waits in deterministic mode
  bool        renderReset   = false; // the renderer drops its temporal state before this frame's picture
  uint32_t    settleSteps   = 0;     // fixed steps left between the end of `streaming wait` and the next command
  bool        ordered       = false; // visibility appends clusters in order (slow): from the end of the wait to the shot
  struct StreamingSample {
    std::string segment;
    uint64_t frame = 0;
    double updateMs = 0, workMs = 0;
    size_t near = 0, far = 0, outside = 0;
    uint64_t nearTransitions = 0, farTransitions = 0;
    std::vector<double> publicationTimesMs;
    uint64_t published = 0, released = 0;
    size_t pageUsedBytes = 0, pageFreeBytes = 0, pageRetiredBytes = 0, pageCount = 0;
    Resources::TextureStats textures;
    size_t vobNear = 0, vobFar = 0, vobOutside = 0, vobPending = 0, vobNearMissing = 0;
    uint64_t vobCreated = 0, vobReleased = 0;
    double vobPublicationMs = 0, vobNearMissingSeconds = 0;
    size_t npcFull = 0, npcSimplified = 0;
    uint64_t npcTierTransitions = 0;
    double npcFullTickMs = 0, npcSimplifiedTickMs = 0;
    };
  std::vector<StreamingSample> streamingSamples;
  std::vector<double> streamingWarmupFarMs;
  size_t streamingWarmupFarMaxPerFrame = 0;
  double streamingWarmupQueueMaxMs = 0;
  uint64_t streamingNearBefore = 0, streamingFarBefore = 0;
  uint64_t streamingPublishedBefore = 0, streamingReleasedBefore = 0;
  Resources::TextureStats streamingTexturesBefore;
  uint64_t streamingVobCreatedBefore = 0, streamingVobReleasedBefore = 0;
  double streamingVobMissingBefore = 0;
  uint64_t streamingNpcTransitionsBefore = 0;
  std::vector<Vec3> routePoints;
  size_t      routeNext = 0;
  Vec3        routePosition;
  float       routeSpeed = 0;
  double      routeLength = 0;
  uint64_t    routeStarted = 0;
  bool        routeActive = false, routeWasExternal = false;

  std::unique_ptr<VideoPipe> video;
  uint32_t    videoW    = 0;
  uint32_t    videoH    = 0;
  std::FILE*  csv       = nullptr;
  std::string shot;
  bool        overlay   = true;
  bool        ui        = false;  // the interface is in the captured frame
  bool        zoom      = true;
  bool        bright    = true;  // auto exposure: forests and interiors are too dark to see the legs
  float       clip      = 0;     // > 0: everything closer to the camera than the subject minus this is clipped away

  Vec3        prevPos;
  bool        hasPrev   = false;
  bool        inDialog  = false;
  Npc*        pinned    = nullptr;  // pin: held there every frame
  Npc*        hearNpc   = nullptr;  // hear: it listens to the hero (words every hearEvery ms) until hearUntil
  uint64_t    hearUntil = 0, hearEvery = 600, hearNext = 0;
  Npc*        thinkNpc  = nullptr;  // think: it thinks of an answer until thinkUntil
  uint64_t    thinkUntil = 0;
  Vec3        pinAt     = {};
  float       speed     = 0;
  uint8_t     occl      = 0;    // bit per leg: the ankle is hidden from the camera
  std::string label;            // free text on the overlay
  Routine routine;
  NpcCommands npcCommands;
  Npc*        subject   = nullptr; // the camera watches this npc instead of the player (gaze)
  PlayerControl* ctrl     = nullptr; // the player's input: the crosshair of the manual aim
  uint32_t    lcg          = 1;  // the bench's own random numbers (seed)
  uint32_t    scenarioSeed = 0;  // the last `seed <n>`
  std::vector<Npc*> crew;        // arc: the npcs set out, `all` of fight
  Living living;
  using LivingPhysics = testbench_detail::LivingPhysics;
  Movables movables;
  using FurnitureDrop = testbench_detail::FurnitureDrop;
  using FurnitureParts = testbench_detail::FurnitureParts;
  FurnitureParts furnitureParts;
  OwnMesh ownMesh;
  Water water;
  ItemHulls itemHulls;
  FracturePieces fracturePieces;
  uint32_t clothZen=uint32_t(-1);
  std::string clothCase,clothObserverSupport="native-floor";
  Vec3 clothCenter,clothDirection=Vec3(1,0,0);
  bool clothTrace=false,clothMeasuring=false,clothOverflow=false,clothHold=false;
  Vec3 clothHeldPosition;
  uint64_t clothSample=uint64_t(-1);
  std::vector<LivingPhysics> clothPhysics;
  std::unordered_map<uintptr_t,std::vector<Vec3>> clothPrevious;
  std::unordered_map<uintptr_t,bool> clothPreviousActive;
  const void* clothArrow=nullptr;
  Vec3 clothShotTarget,clothShotNormal;
  float clothArrowDownstream=0;
  uint64_t clothShotAt=0,clothShotHitsBefore=0;
  std::string clothShotKind;
  Npc* clothArrowNpc=nullptr;


  ItemWake itemWake;

  std::string waterFilm;
  Vec3 waterFilmLip;
  bool waterFilmKill=false;
  uint64_t waterFilmStart=0;
  Destruction destruction;
  Character character;
  MonsterCharacter monster;
  using PlacedItems = testbench_detail::PlacedItems;
  PlacedItems placedItems;
  Npc* traceHost=nullptr;
  uint32_t traceTable=uint32_t(-1);
  Matrix4x4 traceOrigin;
  Vec3 traceCenter,traceVisible,traceBlocked,traceFar,traceWall,traceShotDirection;
  bool traceReady=false;
  ArcheryBench archery;
  using BodyImpact = testbench_detail::BodyImpact;
  BodyImpact bodyImpact;
  struct SavedItemProbe {
    Item* item=nullptr;
    Matrix4x4 saved;
    float maximumPositionDelta=0,maximumRotationDelta=0;
    bool alwaysSleeping=true;
  };
  std::vector<SavedItemProbe> savedItemProbes;
  uint64_t savedItemStart=0,savedItemSample=uint64_t(-1);
  bool savedItemTracing=false;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  bool        cadenceVerified=false,cadenceWait=false;
  uint32_t    cadenceDown=0;
#endif
  RagdollBench ragdoll;
  using BirthLive = RagdollBench::BirthLive;
  Npc*        duelA        = nullptr; // bodyduel: bench-owned NPCs, never resolved by their script symbols
  Npc*        duelB        = nullptr;
  uint64_t    duelFrom = 0, duelEnd = 0;
  bool        duelWaiting  = false;
  using NpcDuel = testbench_detail::NpcDuel;
  NpcDuel npcDuel;
  std::vector<Npc*> observeActors;
  uint64_t    observeFrom = 0, observeEnd = 0;
  bool        observeActive = false;
  Vec3        observeVisibleA, observeVisibleB;
  bool        observeVisibleSaved = false;
  std::string observeProbeCase;
  std::unique_ptr<::Sound> probeSound;
  uint64_t    probeSoundStart = 0, probeSoundDuration = 0;
  bool        probeOccluded = false;
  bool           crossValid = false;
  Vec3           crossPrev  = {}; // on the screen, -1..1
  float          aspect     = 4.f/3.f; // of the captured frame
  float          uiScale    = 1;       // of the game's interface
  // joints of the watched one's animated pose in the last two frames: a pop is a jump of the second difference
  std::vector<Vec3> anim1, anim2;
  // the hero placed around the subject: distance, angle from its facing (0 in front), orbiting
  float       placeDist  = 0;
  float       placeAngle = 0;
  float       orbitSpeed = 0;     // degrees per second
  uint64_t    orbitUntil = 0;

  // talking (gothic-shorts)
  std::string camNpc;           // C_Shoulder, C_NpcFace, C_Two
  std::string zonesNpc;         // showzones: the npc whose hit zones the overlay draws
  bool        zonesOnly = false; // the overlay draws the hit zones only
  // overlay trail: the way each arrow flew, kept trailKeep ms after its last move; nothing else drawn
  using Trail = testbench_detail::Trail;
  bool        trailOn   = false;
  bool        trailLine = false;  // ...and a dashed straight line along the way it left: the bow's line
  uint64_t    trailKeep = 1500;
  std::vector<Trail> trails;
  // slowmo: the game runs `slow` times slower than the video (its sound is not recorded meanwhile)
  float       slow      = 1;
  uint64_t    slowAcc   = 0;      // microseconds of game time owed to the next frame
  uint64_t    frameMs   = 0;      // the video's time of this frame, ms
  // track: where an npc's head (or trunk) is in each recorded frame, for labels that follow it
  std::vector<std::string> trackNpcs;
  bool        trackHead = true;
  // Opt-in live policy telemetry. The controller has one exclusive sink; arenas/replays own theirs.
  bool        bodyLive = false;
  std::string bodyLiveWait;
  std::chrono::steady_clock::time_point bodyLiveDeadline;
  // sight: the hero's line of sight to an npc in each recorded frame and shot (the focus' rays).
  std::string sightNpc;
  float       sightFan  = 700;
  float       camSmooth = 0.12f; // share of the way to the wanted camera per frame; 1: no smoothing
  bool        smValid   = false;
  bool        cutPending = false; // `cam cut`: the next placement of the camera is a cut (Camera::markCut)
  Vec3        smOrg, smTgt;
  bool        speaking  = false;  // the talk key is held while a recording is said
  uint64_t    speakUntil = 0;
  uint64_t    heroEndAt = 0;      // the hero's last phrase ended
  uint64_t    lastLineAt = 0;     // an npc line of the AI started
  uint64_t    busyAt    = 0;      // an npc of the AI spoke (or had a line waiting)
  bool        waitReply = false;
  std::string waitAiKey, waitAiStage;  // waitai: until the bridge says it got that far
  std::unordered_set<std::string> brokenSignals; // explicit server barriers, independent of NPC lines
  uint64_t    waitAiEnd = 0;
  bool        replyNeed = false;  // a line has to start after replyFrom first
  uint64_t    replyFrom = 0;
  uint64_t    replyEnd  = 0;
  uint64_t    replyQuiet = 0;
  std::FILE*  events    = nullptr;
  std::unordered_map<const Npc*,uint64_t> bodyIds;
  uint64_t    nextBodyId = 0;
  uint64_t    bodySeq = 0;
  uint64_t    nativeChoices = 0;
  BodyAnimationControl bodyAnimation;

  // sound: the mix of the game (Tempest loopback), recorded with the video
  std::FILE*  audio     = nullptr;
  uint64_t    audioFrames = 0;
  uint64_t    audioAcc  = 0;
  std::vector<int16_t> mix;
  struct VoiceStem {
    std::mutex           sync;
    std::string          path;
    uint32_t             rate = 0;
    uint64_t             sample = 0;
    std::vector<int16_t> pcm;
    bool                 stopped = false;
    };
  std::vector<std::shared_ptr<VoiceStem>> voiceStems;

  std::chrono::steady_clock::time_point wallStart;
  std::chrono::steady_clock::time_point recWall;  // when the recording started
  int64_t     lagMs     = 0;      // how far the game fell behind the wall clock (realtime)
  };

Bench bench;

bool frameRagdollArena(size_t expected);
Npc& subjectOf(Npc& pl);
void stunDriftSample(World& world,uint64_t elapsed,uint64_t requested);

DestructionContext destructionContext() {
  return {bench.cam,bench.movables.movableCenter,bench.movables.movableIds,bench.movables.movableMeasuring,bench.movables.movableReady,
          bench.movables.movableWalkStart,bench.movables.noiseHost,bench.movables.noiseSource,bench.movables.noiseTrace,bench.ragdoll.ragdollArenaTarget,
          bench.ragdoll.ragdollArenaWaypoint,bench.movables.spellActors,bench.time,bench.waitUntil};
  }

}

namespace testbench_detail {

NpcCommandsContext npcCommandsContext() {
  return {bench.npcCommands,bench.healing,bench.holds,bench.taps,bench.pinned,bench.pinAt,
          bench.scenarioSeed,bench.lcg,bench.monster.monsterCharacterSeed,bench.crew,bench.time,
          bench.melee,bench.archery,bench.subject,stunDriftSample};
  }

ArcheryContext archeryContext() {
  return {bench.archery,bench.living,bench.crew,bench.subject,bench.ragdoll.ragdollArenaTarget,bench.cam,
          bench.trails,bench.camAngle,bench.camDist,bench.camHeight,bench.camLook};
  }

MeleeContext meleeContext() {
  return {bench.melee,bench.crew,bench.living.livingWalkers,bench.ragdoll.ragdollArenaPlaces,bench.ragdoll.ragdollArenaTarget};
  }

RoutineContext routineContext() {
  return {bench.routine,bench.pinned,bench.pinAt,bench.csv,bench.subject,bench.time,subjectOf};
  }

void meleePhysicsCamera() {
    // The same camera and world geometry are used for zero, one and ten actors.
    bench.camPos=bench.ragdoll.ragdollArenaTarget+Vec3(0,2600,1100);
    bench.cam=C_RagdollArena;bench.ragdoll.ragdollArenaCamera=true;bench.camAuto=false;bench.smValid=false;
    lookFrom(bench.camPos,bench.ragdoll.ragdollArenaTarget);
  }

OwnMeshContext ownMeshContext() {
  return {bench.ownMesh,bench.cam,bench.camAuto};
  }

RagdollContext ragdollContext() {
  return {bench.ragdoll,bench.crew,bench.camPos,bench.csv,bench.subject,bench.bodyIds,bench.nextBodyId,
          bench.step,bench.routine.questNpc,bench.recFrame,bench.recName,ragdollTrace,ragdollPerf,subjectOf};
  }

ItemWakeContext itemWakeContext() {
  return {bench.itemWake,bench.destruction,bench.cam,bench.ragdoll.ragdollArenaTarget};
  }

LivingContext livingContext() {
  return {bench.living,bench.crew,bench.subject,bench.csv,bench.ragdoll.ragdollArenaPlaces,frameRagdollArena};
  }

MovablesContext movablesContext() {
  return {bench.movables,bench.cam,bench.crew,bench.routine.questCliff,bench.routine.questWater,
          bench.ragdoll.ragdollArenaTarget,bench.shot,bench.subject};
  }

BodyImpactContext bodyImpactContext() {
  return {bench.bodyImpact,bench.subject,bench.movables.spellActors,bench.crew,bench.movables.spellTrace,bodyTrace,
          bench.time,bench.waitUntil};
  }

BodyContext bodyContext() {
  return {bench.bodyLive,bench.bodyLiveWait,bench.bodyLiveDeadline,bench.bodyAnimation,bench.duelA,
          bench.duelB,bench.duelFrom,bench.duelEnd,bench.duelWaiting,bench.observeActors,
          bench.observeFrom,bench.observeEnd,bench.observeActive,bench.observeVisibleA,bench.observeVisibleB,
          bench.observeVisibleSaved,bench.observeProbeCase,bench.probeSound,bench.probeSoundStart,bench.probeSoundDuration,
          bench.probeOccluded,bench.subject,bench.lcg,bench.pc,bench.prog,
          bench.time,bench.waitUntil};
  }

uintptr_t destructionAdd(World& world,std::string_view name,const Vec3& support) {
  return destructionAdd(bench.destruction,destructionContext(),world,name,support);
  }

void destructionCheckpoint(World& world) {
  destructionCheckpoint(bench.destruction,destructionContext(),world);
  }

}

namespace {

CharacterContext characterContext() {
  return {bench.cam,bench.camAngle,bench.camAuto,bench.camDist,bench.camHeight,bench.frameMs,
          bench.hasPrev,bench.holds,bench.movables.movableFixtures,bench.movables.movableReady,bench.movables.movableWalkEnd,
          bench.movables.movableWalkStart,bench.movables.noiseHost,bench.rec,bench.recVideo,bench.step,bench.subject,bench.taps};
  }

MonsterCharacterContext monsterCharacterContext() {
  return {bench.character,characterContext(),bench.cam,bench.camAuto,bench.subject,
          bench.ragdoll.ragdollArenaTarget,bench.ragdoll.ragdollArenaPlaces,bench.ragdoll.ragdollArenaWaypoint,bench.rec,bench.recVideo};
  }

WaterContext waterContext() {
  return {bench.cam,bench.camPos,bench.movables.movableMeasuring,bench.movables.movableIds,bench.movables.movableMapIds,
          bench.movables.movableReady,bench.movables.movableCenter,bench.routine.questWater,bench.destruction.destructionFixtures,
          bench.holds,bench.taps,bench.ctrl,bench.crew,bench.movables.spellActors,bench.movables.spellGuardTrace,
          bench.movables.spellRunes,bench.movables.spellCastingId,bench.movables.spellEquipPending,bench.movables.spellEquipUntil,
          bench.ragdoll.expectedRagdollLoot,bench.ragdoll.ragdollLootDidOpen};
  }

void writeHeartbeat(const World& world) {
  if(envStr("OPENGOTHIC_BENCH_OUT")==nullptr)
    return;
  const auto now = std::chrono::steady_clock::now();
  if(now<bench.heartbeatNext)
    return;
  bench.heartbeatNext = now+std::chrono::seconds(10);
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now-bench.heartbeatStart).count();
  const auto command = std::min(std::max(bench.pc,size_t(1)),bench.prog.size());
  char gameTime[80];
  std::snprintf(gameTime,sizeof(gameTime),"day %lld %02lld:%02lld",(long long)world.time().day(),
                (long long)world.time().hour(),(long long)world.time().minute());
  const auto path = outDir+"/heartbeat.json";
  std::ofstream file(path+".tmp");
  file << "{\"elapsed_seconds\":" << elapsed << ",\"frame\":" << bench.step
       << ",\"command_index\":" << command << ",\"command_count\":" << bench.prog.size()
       << ",\"command\":" << jsonStr(command>0 ? bench.prog[command-1] : "")
       << ",\"game_time\":" << jsonStr(gameTime);
  if(NpcDayJournal::running())
    file << ",\"npcday_elapsed_minutes\":" << double(world.time().toInt()-bench.npcCommands.npcDayStart)/60000.0
         << ",\"npcday_target_minutes\":" << bench.npcCommands.npcDayMinutes;
  file << "}\n";
  file.close();
  if(file) {
    std::error_code error;
    std::filesystem::rename(path+".tmp",path,error);
    }
  }

}

namespace testbench_detail {

// a line of events.jsonl: what happened when, in the recording's frames (the cut of a short lines up its
// subtitles and sound by them); `fields` are more json members, without braces
void event(std::string_view kind, const std::string& fields) {
  if(bench.events==nullptr)
    bench.events = std::fopen((outDir+"/events.jsonl").c_str(),"w");
  if(bench.events==nullptr)
    return;
  std::fprintf(bench.events,"{\"t\":%llu,\"seg\":%s,\"frame\":%lld,\"kind\":%s%s%s}\n",
               (unsigned long long)bench.time,jsonStr(bench.rec ? bench.recName : "").c_str(),
               bench.rec ? (long long)bench.recFrame : -1LL,jsonStr(kind).c_str(),fields.empty() ? "" : ",",fields.c_str());
  std::fflush(bench.events);
  }

}

namespace {

Npc* npcBySymbol(World& w, std::string_view symbol) {
  const size_t id = w.script().findSymbolIndex(symbol);
  return id==size_t(-1) ? nullptr : w.findNpcByInstance(id);
  }

bool aiCommand(const std::string& cmd, std::string& reply) {
  auto& fn = Gothic::inst().consoleCommandFn;
  return fn && fn(cmd,reply);
  }

void loadScenario() {
  bench.loaded = true;
  std::ifstream fin(scenarioPath);
  if(!fin) {
    say(std::string("can not read scenario: ") + scenarioPath);
    return;
    }
  const char*       tv   = std::getenv("OPENGOTHIC_BENCH_TAKE");
  const std::string take = tv!=nullptr && tv[0]!='\0' ? tv : "on";
  std::string line;
  while(std::getline(fin,line)) {
    const size_t hash = line.find('#');
    if(hash!=std::string::npos)
      line.resize(hash);
    while(!line.empty() && std::isspace(uint8_t(line.back())))
      line.pop_back();
    size_t b = 0;
    while(b<line.size() && std::isspace(uint8_t(line[b])))
      ++b;
    // `off: <command>` / `on: <command>`: in that take of a before/after short only (gothic-shorts compare/film.sh
    // sets OPENGOTHIC_BENCH_TAKE; without it the take is `on`)
    for(const std::string_view tk:{std::string_view("off:"),std::string_view("on:")}) {
      if(line.compare(b,tk.size(),tk)!=0)
        continue;
      if(tk.substr(0,tk.size()-1)!=take)
        b = line.size(); else
        b += tk.size();
      while(b<line.size() && std::isspace(uint8_t(line[b])))
        ++b;
      break;
      }
    if(b<line.size())
      bench.prog.push_back(line.substr(b));
    }
  say("scenario " + std::string(scenarioPath) + ", " + std::to_string(bench.prog.size()) + " lines");
  }

Marvin& marvin() {
  static Marvin m;
  return m;
  }

bool keyAction(std::string_view k, KeyCodec::Action& a) {
  static const struct { std::string_view name; KeyCodec::Action a; } keys[] = {
    {"forward",KeyCodec::Forward}, {"back",KeyCodec::Back},       {"left",KeyCodec::Left},  {"right",KeyCodec::Right},
    {"turnl",  KeyCodec::RotateL}, {"turnr",KeyCodec::RotateR},   {"jump",KeyCodec::Jump},  {"walk", KeyCodec::Walk},
    {"sneak",  KeyCodec::Sneak},   {"weapon",KeyCodec::Weapon},   {"action",KeyCodec::ActionGeneric},
    {"attack", KeyCodec::ActionGeneric},
    {"melee",  KeyCodec::WeaponMele},{"bow",KeyCodec::WeaponBow}, {"spell",KeyCodec::WeaponMage3},
    {"parade", KeyCodec::Parade},  {"breath",KeyCodec::Walk},     {"dodge",KeyCodec::Dodge},
    {"heal", KeyCodec::Heal},      {"potion",KeyCodec::Potion},
    };
  for(auto& i:keys)
    if(i.name==k) {
      a = i.a;
      return true;
      }
  return false;
  }

void press(PlayerControl& ctrl, KeyCodec::Action a) {
  if(a==KeyCodec::ActionGeneric || a==KeyCodec::Forward) {
    bench.inputTraceUntil = bench.time+2000;
    const auto* world = Gothic::inst().world();
    say("input press " + std::string(a==KeyCodec::Forward ? "forward" : "attack") + " at " +
        std::to_string(world ? world->tickCount() : 0) + " ms");
    }
  ctrl.onKeyPressed(a,KeyEvent::K_NoKey,KeyCodec::Mapping::Primary);
  }

}

namespace testbench_detail {

void release(PlayerControl& ctrl, KeyCodec::Action a) {
  if(a==KeyCodec::ActionGeneric || a==KeyCodec::Forward) {
    bench.inputTraceUntil = bench.time+2000;
    const auto* world = Gothic::inst().world();
    say("input release " + std::string(a==KeyCodec::Forward ? "forward" : "attack") + " at " +
        std::to_string(world ? world->tickCount() : 0) + " ms");
    }
  ctrl.onKeyReleased(a,KeyCodec::Mapping::Primary);
  }

}

namespace {

void stopRecording() {
  if(!bench.rec)
    return;
  for(auto& stem:bench.voiceStems) {
    std::lock_guard<std::mutex> guard(stem->sync);
    stem->stopped = true;
    // OpenAL may request resampler lookahead. Keep only the part within the segment.
    const uint64_t available = bench.audioFrames>=stem->sample ? bench.audioFrames-stem->sample : 0;
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
  bench.voiceStems.clear();
  bench.video.reset();  // waits for the frames still queued
  if(bench.audio!=nullptr) {
    writeWavHeader(bench.audio,SoundDevice::loopbackRate(),bench.audioFrames);
    std::fclose(bench.audio);
    }
  bench.audio = nullptr;
  // how fast the game ran against the wall clock while recording (the AI server keeps to the wall clock)
  const auto wall  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-bench.recWall).count();
  const int  speed = wall>0 ? int((bench.time-bench.recStart)*100/uint64_t(wall)) : 100;
  event("rec_stop","\"frames\":" + std::to_string(bench.recFrame) + ",\"speed\":" + std::to_string(speed));
  say("rec stop " + bench.recName + " frames=" + std::to_string(bench.recFrame) +
      (realtime ? " speed=" + std::to_string(speed) + "%" : std::string()));
  bench.rec = false;
  }

void finish() {
  if(auto* world=Gothic::inst().world())
    NpcDayJournal::stop(*world,"bench_end");
  stopBodyLive();
  npcCharacterStopMotionTrace(bench.character,Gothic::inst().world());
  endAnimationControl();
  animationControlTotals();
  if(bench.speaking) {
    std::string reply;
    aiCommand("ai ptt cancel",reply);
    bench.speaking = false;
    }
  stopRecording();
  if(!bench.streamingSamples.empty()) {
    if(auto* file=std::fopen((outDir+"/streaming.csv").c_str(),"w")) {
      std::fprintf(file,"segment,frame,update_ms,work_ms,near,far,outside,near_transitions,far_transitions,publication_ms,published,released,page_used_bytes,page_free_bytes,page_retired_bytes,page_count,tex_live_bytes,tex_retired_bytes,tex_peak_bytes,tex_live_n,tex_evicted,tex_reloaded,tex_landscape_bytes,tex_landscape_n,tex_static_bytes,tex_static_n,tex_dynamic_bytes,tex_dynamic_n,tex_other_bytes,tex_other_n,tex_full_bytes,tex_tail_bytes,tex_promoted,tex_demoted,tex_promotion_queue,tex_near_tail_n,tex_near_tail_seconds,vob_near,vob_far,vob_outside,vob_created,vob_released,vob_pending,vob_publication_ms,vob_near_missing,vob_near_missing_seconds,npc_full,npc_simplified,npc_tier_transitions,npc_full_tick_ms,npc_simplified_tick_ms\n");
      for(const auto& row:bench.streamingSamples) {
        std::fprintf(file,"%s,%llu,%.9g,%.9g,%zu,%zu,%zu,%llu,%llu,",row.segment.c_str(),
          (unsigned long long)row.frame,row.updateMs,row.workMs,row.near,row.far,row.outside,
          (unsigned long long)row.nearTransitions,(unsigned long long)row.farTransitions);
        for(size_t i=0;i<row.publicationTimesMs.size();++i)
          std::fprintf(file,"%s%.9g",i==0 ? "" : ";",row.publicationTimesMs[i]);
        std::fprintf(file,",%llu,%llu,%zu,%zu,%zu,%zu",(unsigned long long)row.published,(unsigned long long)row.released,
          row.pageUsedBytes,row.pageFreeBytes,row.pageRetiredBytes,row.pageCount);
        const auto& textures = row.textures;
        std::fprintf(file,",%llu,%llu,%llu,%llu,%llu,%llu",
          (unsigned long long)textures.liveBytes,(unsigned long long)textures.retiredBytes,
          (unsigned long long)textures.peakBytes,(unsigned long long)textures.count,
          (unsigned long long)textures.evicted,(unsigned long long)textures.reloaded);
        for(size_t i=0;i<textures.categoryBytes.size();++i)
          std::fprintf(file,",%llu,%llu",(unsigned long long)textures.categoryBytes[i],
            (unsigned long long)textures.categoryCount[i]);
        std::fprintf(file,",%llu,%llu,%llu,%llu,%llu,%llu,%.9g",
          (unsigned long long)textures.fullBytes,(unsigned long long)textures.tailBytes,
          (unsigned long long)textures.promoted,(unsigned long long)textures.demoted,
          (unsigned long long)textures.promotionQueue,(unsigned long long)textures.nearTailCount,textures.nearTailSeconds);
        std::fprintf(file,",%zu,%zu,%zu,%llu,%llu,%zu,%.9g,%zu,%.9g",
          row.vobNear,row.vobFar,row.vobOutside,(unsigned long long)row.vobCreated,
          (unsigned long long)row.vobReleased,row.vobPending,row.vobPublicationMs,
          row.vobNearMissing,row.vobNearMissingSeconds);
        std::fprintf(file,",%zu,%zu,%llu,%.9g,%.9g\n",row.npcFull,row.npcSimplified,
          (unsigned long long)row.npcTierTransitions,row.npcFullTickMs,row.npcSimplifiedTickMs);
        }
      std::fclose(file);
      } else event("streaming_error","\"reason\":\"cannot write residency samples\"");
    }
  if(!bench.character.characterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"route,frame,t_game_ms,dt_ms,x_cm,y_cm,z_cm,move_held,in_air,falling,swimming,cv_enabled,cv_active,ground_vx_cm_s,ground_vy_cm_s,ground_vz_cm_s,step_ms,wall_ms,captured,normal_clock,visual_x_cm,visual_y_cm,visual_z_cm\n");
      for(const auto& row:bench.character.characterFrames)
        std::fprintf(file,"%s,%llu,%llu,%llu,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%.9g,%.9g,%.9g\n",
          row.route.c_str(),(unsigned long long)row.frame,(unsigned long long)row.time,(unsigned long long)row.dt,
          double(row.root.x),double(row.root.y),double(row.root.z),int(row.moving),int(row.air),int(row.fall),int(row.swim),
          int(row.enabled),int(row.active),double(row.groundVelocity.x),double(row.groundVelocity.y),double(row.groundVelocity.z),
          double(row.physics),double(row.wall),int(row.captured),int(row.normal),
          double(row.visualRoot.x),double(row.visualRoot.y),double(row.visualRoot.z));
      std::fclose(file);
      } else event("character_error","\"reason\":\"cannot write native character frames\"");
    }
  if(!bench.character.npcCharacterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/npc-character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"phase,frame,t_game_ms,dt_ms,npc_id,x_cm,y_cm,z_cm,visual_x_cm,visual_y_cm,visual_z_cm,cv_enabled,cv_active,moving,in_air,interactive,step_ms,wall_ms,nearby_humans,nearby_cv,moving_humans,moving_cv,captured,normal_clock%s\n",characterMoveFrameColumns);
      for(const auto& row:bench.character.npcCharacterFrames) {
        std::fprintf(file,"%s,%llu,%llu,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%.9g,%.9g,%u,%u,%u,%u,%d,%d",
          row.phase.c_str(),(unsigned long long)row.frame,(unsigned long long)row.time,(unsigned long long)row.dt,
          (unsigned long long)row.npc,double(row.root.x),double(row.root.y),double(row.root.z),
          double(row.visual.x),double(row.visual.y),double(row.visual.z),int(row.enabled),int(row.active),int(row.moving),
          int(row.air),int(row.interactive),double(row.physics),double(row.wall),row.nearby,row.virtualNearby,row.movingNearby,row.movingVirtualNearby,int(row.captured),int(row.normal));
        writeCharacterMoveFrame(file,row.move);
        }
      std::fclose(file);
      } else event("npc_character_error","\"reason\":\"cannot write native NPC character frames\"");
    }
  if(!bench.monster.monsterCharacterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/monster-character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"route,t_game_ms,dt_ms,npc_id,x_cm,y_cm,z_cm,visual_x_cm,visual_y_cm,visual_z_cm,cv_enabled,cv_active,moving,in_air,falling,swimming,animation,body_yaw_deg%s\n",characterMoveFrameColumns);
      for(const auto& row:bench.monster.monsterCharacterFrames) {
        std::fprintf(file,"%s,%llu,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%d,\"%s\",%.9g",
          row.route.c_str(),(unsigned long long)row.time,(unsigned long long)row.dt,(unsigned long long)row.npc,
          double(row.root.x),double(row.root.y),double(row.root.z),double(row.visual.x),double(row.visual.y),double(row.visual.z),
          int(row.enabled),int(row.active),int(row.moving),int(row.air),int(row.fall),int(row.swim),row.animation.c_str(),double(row.bodyYaw));
        writeCharacterMoveFrame(file,row.move);
        }
      std::fclose(file);
      } else event("monster_character_error","\"reason\":\"cannot write monster frames\"");
    }
  if(!bench.monster.monsterCharacterPerformance.empty()) {
    if(auto* file=std::fopen((outDir+"/monster-character-performance.csv").c_str(),"w")) {
      std::fprintf(file,"t_game_ms,dt_ms,step_ms,wall_ms,nearby_monsters,nearby_cv,moving,maximum_distance_cm,captured,normal_clock\n");
      for(const auto& row:bench.monster.monsterCharacterPerformance)
        std::fprintf(file,"%llu,%llu,%.9g,%.9g,%u,%u,%u,%.9g,%d,%d\n",
          (unsigned long long)row.time,(unsigned long long)row.dt,double(row.physics),double(row.wall),row.nearby,
          row.virtualNearby,row.moving,double(row.maximumDistance),int(row.captured),int(row.normal));
      std::fclose(file);
      } else event("monster_character_error","\"reason\":\"cannot write monster performance samples\"");
    }
  bench.monster.monsterCharacterProps.clear();
  bench.monster.monsterCharacterLedgeBody=DynamicWorld::Item();bench.monster.monsterCharacterLedgeShape.reset();
  bench.character.characterProp.reset();bench.character.characterPlatform.reset();bench.character.characterPlatformVisual.reset();
  bench.destruction.destructionFixtures.clear();
  bench.furnitureParts.loadBodies.clear();
  if(bench.npcDuel.active) {
    Gothic::inst().onCombatHit.ubind(&bench.npcDuel,&Bench::NpcDuel::hit);
    bench.npcDuel.active=false;
    }
  if(bench.bodyImpact.observing) {
    Gothic::inst().onHeroDeed.ubind(&bench.bodyImpact,&Bench::BodyImpact::deed);
    Gothic::inst().onObservedPerception.ubind(&bench.bodyImpact,&Bench::BodyImpact::perception);
    Gothic::inst().onExternalPerception.ubind(&bench.bodyImpact,&Bench::BodyImpact::perception);
    bench.bodyImpact.observing=false;
    }
  if(bench.placedItems.watchingTheft) {
    Gothic::inst().onHeroDeed.ubind(&bench.placedItems,&Bench::PlacedItems::deed);
    Gothic::inst().onObservedPerception.ubind(&bench.placedItems,&Bench::PlacedItems::perception);
    bench.placedItems.watchingTheft=false;
  }
  bench.movables.movableFixtures.clear();
  if(ragdollPerf) {
    std::FILE* perf=std::fopen((outDir+"/ragdoll-performance.csv").c_str(),"w");
    if(perf!=nullptr) {
      std::fprintf(perf,"seg,frame,wall_ms,step_ms,world_bodies,rays,actors,corpses,physical,sleeping,parts,centers_in_view,physical_before_impulse,sleeping_before_impulse,visible_mesh_proxies\n");
      for(const auto& sample:bench.ragdoll.ragdollPerformance)
        std::fprintf(perf,"%s,%llu,%.9g,%.9g,%zu,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",sample.segment.c_str(),
                     (unsigned long long)sample.frame,double(sample.wallMs),double(sample.stepMs),sample.worldBodies,
                     (unsigned long long)sample.rays,sample.actors,sample.corpses,sample.physical,sample.sleeping,sample.parts,sample.centersInView,
                     sample.physicalBeforeImpulse,sample.sleepingBeforeImpulse,sample.visibleMeshes);
      std::fclose(perf);
      } else {
      event("ragdoll_error","\"reason\":\"cannot write deferred performance samples\"");
      }
    }
  if(!bench.ragdoll.ragdollCoverage.empty()) {
    if(auto* file=std::fopen((outDir+"/ragdoll-coverage.csv").c_str(),"w")) {
      std::fprintf(file,"mode,frame,actors,corpses,physical,sleeping,parts,centers_in_view,physical_before_impulse,sleeping_before_impulse,visible_meshes\n");
      for(const auto& sample:bench.ragdoll.ragdollCoverage)
        std::fprintf(file,"%s,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",sample.segment.c_str(),
                     (unsigned long long)sample.frame,sample.actors,sample.corpses,sample.physical,sample.sleeping,
                     sample.parts,sample.centersInView,sample.physicalBeforeImpulse,sample.sleepingBeforeImpulse,sample.visibleMeshes);
      std::fclose(file);
      }
    }
  if(bench.csv!=nullptr)
    std::fclose(bench.csv);
  bench.csv  = nullptr;
  if(bench.events!=nullptr)
    std::fclose(bench.events);
  bench.events = nullptr;
  if(realtime)
    say("realtime: " + std::string(bench.lagMs>200 ? "fell behind the wall clock by " + std::to_string(bench.lagMs) + " ms"
                                                   : "kept to the wall clock"));
  bench.done = true;
  DrawCommands::hiddenTypes = 0;
  if(auto c = Gothic::inst().camera()) {
    c->setMarvinMode(Camera::M_Normal);
    c->setNearClip(10);
    }
  say("done");
  if(!keepRunning && !Resources::isHeadless())
    SystemApi::exit();
  }

Vec3 ankle(const Npc& pl, size_t leg) {
  auto& p = FootIk::playerProbe;
  if(p.valid && pl.isPlayer())
    return p.leg[leg].final[2];
  return pl.position() + Vec3(0,10,0);
  }

Npc& subjectOf(Npc& pl) {
  return bench.subject!=nullptr ? *bench.subject : pl;
  }

}

namespace testbench_detail {

// "nearest": the nearest living npc but the player; else a script symbol (NONE_100_XARDAS)
Npc* findNpc(World& w, Npc& pl, const std::string& name) {
  if(name=="player")
    return &pl;
  if(name=="watched")
    return &subjectOf(pl);
  if(name.starts_with("id:")) {
    // Archive fixtures address the persisted world slot, including dead NPCs.
    // Reject signs, whitespace, trailing text and overflow rather than falling
    // back to a script symbol that might select another actor.
    if(name.size()==3)
      return nullptr;
    uint64_t id=0;
    for(size_t i=3; i<name.size(); ++i) {
      if(name[i]<'0' || name[i]>'9')
        return nullptr;
      id=id*10+uint64_t(name[i]-'0');
      if(id>uint64_t(UINT32_MAX))
        return nullptr;
      }
    return w.npcById(uint32_t(id));
    }
  if(name=="nearest") {
    Npc*  best = nullptr;
    float dist = 0;
    for(uint32_t i=0; i<w.npcCount(); ++i) {
      Npc* n = w.npcById(i);
      if(n==nullptr || n==&pl || n->isDead())
        continue;
      const float d = (n->position()-pl.position()).quadLength();
      if(best==nullptr || d<dist) {
        best = n;
        dist = d;
        }
      }
    return best;
    }
  // by its script symbol; of several (two sheep inserted) a living one, the nearest
  const size_t id = w.script().findSymbolIndex(name);
  if(id==size_t(-1))
    return nullptr;
  Npc*  best = nullptr;
  float dist = 0;
  for(uint32_t i=0; i<w.npcCount(); ++i) {
    Npc* n = w.npcById(i);
    if(n==nullptr || n->instanceSymbol()!=id)
      continue;
    const float d = (n->position()-pl.position()).quadLength() + (n->isDead() ? 1e12f : 0.f);
    if(best==nullptr || d<dist) {
      best = n;
      dist = d;
      }
    }
  return best!=nullptr ? best : w.findNpcByInstance(id);
  }

}

namespace {

// an npc's name as the player sees it, for the log and events.jsonl (UTF-8): the game's strings are cp1251
// (the russian localization). The engine has no codec (ai_bridge's TextCodec is above it); a name only
// needs the letters, other bytes above ASCII become '?'
std::string nameUtf8(const Npc& n) {
  std::string r;
  for(char ch:n.displayName()) {
    const auto b = uint8_t(ch);
    if(b<0x80) {
      r += ch;
      continue;
      }
    uint32_t cp = '?';
    if(b>=0xC0)
      cp = 0x410 + (uint32_t(b) - 0xC0);  // А..я
    else if(b==0xA8)
      cp = 0x401;                         // Ё
    else if(b==0xB8)
      cp = 0x451;                         // ё
    if(cp<0x80) {
      r += char(cp);
      continue;
      }
    r += char(0xC0 | (cp>>6));
    r += char(0x80 | (cp & 0x3F));
    }
  return r;
  }

}

namespace testbench_detail {

std::string symName(World& w, const Npc* n) {
  if(n==nullptr)
    return "-";
  if(auto sym = w.script().findSymbol(size_t(n->instanceSymbol())))
    return sym->name();
  return nameUtf8(*n);
  }

}

namespace {

const char* gazeSource(Gaze::Source s) {
  switch(s) {
    case Gaze::Source::None:      return "none";
    case Gaze::Source::Talk:      return "talk";
    case Gaze::Source::Script:    return "script";
    case Gaze::Source::Focus:     return "focus";
    case Gaze::Source::Attention: return "attention";
    case Gaze::Source::Input:     return "input";
    }
  return "?";
  }

// whom the gaze of that source looks at, "-" for none or a point
std::string gazeWho(World& w, const Npc& n, Gaze::Source s) {
  return symName(w,s==Gaze::Source::Talk ? n.talkPartner() : s==Gaze::Source::Script ? n.lookAtTarget() :
                   s==Gaze::Source::Attention ? n.attentionTarget() : nullptr);
  }

}

namespace testbench_detail {

// how much of its AI runs, by its distance to the hero (worldobjects.cpp): far ones say nothing, far2 ones
// barely move
const char* policyName(NpcProcessPolicy p) {
  switch(p) {
    case NpcProcessPolicy::Player:   return "player";
    case NpcProcessPolicy::AiNormal: return "normal";
    case NpcProcessPolicy::AiFar:    return "far";
    case NpcProcessPolicy::AiFar2:   return "far2";
    }
  return "?";
  }

}

namespace testbench_detail {

// the script state it is in (ZS_SMALLTALK), "-" for none
std::string stateName(World& w, const Npc& n) {
  const ScriptFn fn  = n.currentState();
  auto           sym = fn.isValid() ? w.script().findSymbol(fn.ptr) : nullptr;
  return sym!=nullptr ? sym->name() : "-";
  }

// a point is hidden if something is between it and the camera (not counting the ground right at the point)
bool hidden(World& w, const Vec3& from, const Vec3& to) {
  auto r = w.physic()->ray(from,to);
  return r.hasCol && (r.v-to).length()>15.f;
  }

}

namespace {

Vec3 camOrigin(World& w, const Npc& pl, float angle) {
  const Vec3  p = pl.position();
  Vec3        dir;
  if(bench.cam==C_World) {
    const float a = angle*float(M_PI)/180.f;
    dir = {std::cos(a),0,std::sin(a)};
    } else {
    const Vec3  f    = facing(pl);
    const Vec3  side = {-f.z,0,f.x};
    const float b    = angle*float(M_PI)/180.f;
    dir = f*std::cos(b) + side*std::sin(b);
    }
  Vec3 org = p + dir*bench.camDist + Vec3(0,bench.camHeight,0);
  // keep the camera above the ground
  const float g = bench.camLift ? groundY(w,org.x,org.z,org.y+200) : NAN;
  if(!std::isnan(g) && org.y<g+25)
    org.y = g+25;
  return org;
  }

int hiddenCount(World& w, const Npc& pl, const Vec3& org) {
  const Vec3 p = pl.position();
  int n = 0;
  n += hidden(w,org,ankle(pl,0)+Vec3(0,5,0)) ? 1 : 0;
  n += hidden(w,org,ankle(pl,1)+Vec3(0,5,0)) ? 1 : 0;
  n += hidden(w,org,p+Vec3(0,bench.camLook,0)) ? 1 : 0;
  return n;
  }

// side views first
void pickCamAngle(World& w, const Npc& pl) {
  static const float angles[] = {90,270,60,120,240,300,30,150,210,330,0,180};
  float best = bench.camAngle;
  int   bn   = 100;
  for(float a:angles) {
    const int n = hiddenCount(w,pl,camOrigin(w,pl,a));
    if(n<bn) {
      bn   = n;
      best = a;
      }
    if(n==0)
      break;
    }
  bench.camAngle = best;
  }

}

namespace testbench_detail {

void lookFrom(const Vec3& org, const Vec3& tgt) {
  auto c = Gothic::inst().camera();
  if(c==nullptr)
    return;
  const Vec3  d   = tgt-org;
  const float lxz = std::sqrt(d.x*d.x+d.z*d.z);
  const float yaw = std::atan2(d.z,d.x)*180.f/float(M_PI);
  const float pit = std::atan2(d.y,lxz)*180.f/float(M_PI);
  c->setMarvinMode(Camera::M_Freeze);
  c->setPosition(org);
  c->setSpin(PointF(-pit,yaw));
  c->setAngles(PointF(-pit,yaw));
  if(bench.cutPending) {
    c->markCut();
    bench.cutPending = false;
    }
  bench.camPos = org;
  bench.camHeldAngles = PointF(-pit,yaw);
  }

Vec3 rotY(const Vec3& v, float deg) {
  const float a = deg*float(M_PI)/180.f, c = std::cos(a), s = std::sin(a);
  return {v.x*c - v.z*s, v.y, v.x*s + v.z*c};
  }

}

namespace {

// a shot of the hero and camNpc; false if the npc is not there. camNpc `arena`: the arena's primary
// in place of the hero and its opponent
bool npcShot(World& w, Npc& player, Vec3& org, Vec3& tgt) {
  Npc* npc = nullptr;
  Npc* own = &player;
  if(bench.camNpc=="arena") {
    const auto actors = BodyArena::actors();
    if(actors.size()<2)
      return false;
    own = actors[0];
    npc = actors[1];
    } else {
    npc = findNpc(w,player,bench.camNpc);  // of several (wolves) the nearest living one
    if(npc==&player)
      return false;
    }
  if(npc==nullptr)
    return false;
  Npc& pl = *own;
  const Vec3 hero  = pl.mapHeadBone();
  const Vec3 other = npc->mapHeadBone();
  Vec3 d = other-hero;
  d.y = 0;
  const float len = d.length();
  d = len>1 ? d/len : facing(pl);
  const Vec3 side = {-d.z,0,d.x};
  const Vec3 up   = {0,1,0};
  if(bench.cam==C_Shoulder) {
    // behind the hero, off his shoulder (camAngle: +1 or -1 picks the shoulder), at the npc's face
    org = hero - d*bench.camDist + side*(40.f*bench.camAngle) + up*bench.camHeight;
    tgt = other - up*8.f;
    }
  else if(bench.cam==C_NpcFace) {
    // in front of the npc's face (where it looks), turned camAngle degrees towards the hero's side
    const Vec3 f  = facing(*npc);
    const Vec3 c0 = rotY(f, bench.camAngle), c1 = rotY(f,-bench.camAngle);
    const Vec3 c  = (c0.x*-d.x + c0.z*-d.z) >= (c1.x*-d.x + c1.z*-d.z) ? c0 : c1;  // the turn towards the hero
    org = other + c*bench.camDist + up*bench.camHeight;
    tgt = other - up*8.f;
    }
  else {
    const Vec3  mid  = (hero+other)*0.5f;
    const float dist = std::max(bench.camDist,len*1.2f);
    org = mid + side*(dist*(bench.camAngle<0 ? -1.f : 1.f)) + up*bench.camHeight;
    tgt = mid - up*15.f;
    }
  const float g = groundY(w,org.x,org.z,org.y+200);
  if(!std::isnan(g) && org.y<g+25)
    org.y = g+25;
  return true;
  }


void updateCamera(World& w, Npc& pl) {
  bench.occl = 0;
  if(bench.cam==C_Turn) {
    if(auto* camera=Gothic::inst().camera()) {
      const float progress=std::min(1.f,float(bench.time-bench.camTurnStart)/float(bench.camTurnDuration));
      const float eased=progress*progress*(3.f-2.f*progress);
      const PointF angles(bench.camTurnAngles.x,bench.camTurnAngles.y+bench.camTurnDegrees*eased);
      camera->setPosition(bench.camPos);
      camera->setSpin(angles);camera->setAngles(angles);
      bench.camHeldAngles=angles;
      if(progress>=1.f)bench.cam=C_Hold;
      }
    return;
    }
  if(bench.cam==C_Water) {
    lookFrom(bench.camPos,bench.water.cameraTarget);
    return;
    }
  if(bench.cam==C_RagdollArena) {
    lookFrom(bench.camPos,bench.ragdoll.ragdollArenaTarget);
    return;
    }
  if(bench.cam==C_Game || bench.cam==C_Hold)
    return;
  if(bench.cam==C_Arrow) {
    Vec3 org, tgt;
    arrowShot(w,pl,org,tgt);
    if(!bench.smValid) {
      bench.smOrg   = org;
      bench.smTgt   = tgt;
      bench.smValid = true;
      } else {
      bench.smOrg += (org-bench.smOrg)*bench.camSmooth;
      bench.smTgt += (tgt-bench.smTgt)*bench.camSmooth;
      }
    lookFrom(bench.smOrg,bench.smTgt);
    return;
    }
  if(bench.cam==C_Shoulder || bench.cam==C_NpcFace || bench.cam==C_Two) {
    // shots of the hero himself, also while the camera watches a subject (gaze)
    Npc* hero = Gothic::inst().player();
    Vec3 org, tgt;
    if(hero==nullptr || !npcShot(w,*hero,org,tgt))
      return;
    if(!bench.smValid) {
      bench.smOrg   = org;
      bench.smTgt   = tgt;
      bench.smValid = true;
      } else {
      bench.smOrg += (org-bench.smOrg)*bench.camSmooth;
      bench.smTgt += (tgt-bench.smTgt)*bench.camSmooth;
      }
    lookFrom(bench.smOrg,bench.smTgt);
    if(bench.clip>0)
      if(auto cam = Gothic::inst().camera())
        cam->setNearClip(std::max(10.f, (bench.smOrg-bench.smTgt).length() - bench.clip));
    return;
    }
  Vec3 tgt = pl.position() + Vec3(0,bench.camLook,0);
  Vec3 org = bench.camPos;
  if(bench.cam==C_Follow || bench.cam==C_World) {
    org = camOrigin(w,pl,bench.camAngle);
    // a body down sinks its position under the ground (85 cm for the hero knocked down): look at the ground over it
    const Vec3  at = pl.position();
    const float g  = groundY(w,at.x,at.z,at.y+100.f);
    if(!std::isnan(g) && g>at.y) {
      tgt.y += g-at.y;
      org.y  = std::max(org.y,g+bench.camHeight);
      }
    }
  if(bench.cam==C_Face) {
    // the mouth (a bit above the head bone), seen from the front or at an angle
    const Vec3  f    = facing(pl);
    const Vec3  side = {-f.z,0,f.x};
    const float b    = bench.camAngle*float(M_PI)/180.f;
    tgt = pl.mapHeadBone() + Vec3(0,bench.camLook,0);
    org = tgt + (f*std::cos(b) + side*std::sin(b))*bench.camDist + Vec3(0,bench.camHeight,0);
    }
  lookFrom(org,tgt);
  if(bench.clip>0)
    if(auto cam = Gothic::inst().camera())
      cam->setNearClip(std::max(10.f, (org-tgt).length() - bench.clip));

  for(size_t i=0; i<2; ++i)
    if(hidden(w,org,ankle(pl,i)+Vec3(0,5,0)))
      bench.occl |= uint8_t(1u<<i);
  bench.occlTime = bench.occl!=0 ? bench.occlTime+1 : 0;
  if(bench.camAuto && bench.cam==C_Follow && bench.occlTime>Fps/3) {
    pickCamAngle(w,pl);
    bench.occlTime = 0;
    }
  }


// the faces a mood shows (the bridge's say.mood)
struct FaceColumn { const char* column; const char* anim; };
const FaceColumn FaceColumns[] = {
  {"face_neutral","S_NEUTRAL"}, {"face_friendly","S_FRIENDLY"}, {"face_angry","S_ANGRY"},
  {"face_hostile","S_HOSTILE"}, {"face_afraid","S_FRIGHTENED"}, {"face_pain","T_HURT"},
  };

void writeHeader() {
  std::fprintf(bench.csv,"seg,frame,t,x,y,z,rot,speed,bs,walk,occl,ik_valid,ik_on,ik_hardoff,ik_w,nlegs,bad,pelvis,chest,pelvis_target,pelvis_limit,floor");
  for(const char* s:{"l","r","fl","fr"}) {
    std::fprintf(bench.csv,",%s_plant,%s_has,%s_gy,%s_nx,%s_ny,%s_nz,%s_toehas,%s_toey,%s_smooth,%s_err,%s_knee,%s_tilt,%s_roll,%s_yaw,%s_out,%s_slack",
                 s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s);
    for(const char* j:{"hip","kneea","anka","toea","kneef","ankf","toef"})
      std::fprintf(bench.csv,",%s_%s_x,%s_%s_y,%s_%s_z",s,j,s,j,s,j);
    }
  std::fprintf(bench.csv,",g_on,g_src,g_w,g_err,g_yaw,g_pitch,g_yawgoal,g_pitchgoal,g_head,g_chest,g_waist,g_animyaw,g_animpitch,g_hipsyaw,g_fyaw,g_fpitch,pg_on,pg_src,pg_w,pg_err,pg_yaw");
  std::fprintf(bench.csv,",ln_on,ln_roll,ln_pitch,ln_speed,ln_turn,ln_slope");
  std::fprintf(bench.csv,",an_pop,an_bone,an_speed,an_layers,bl_on,bl_active,bl_started,bl_angle,bl_time");
  std::fprintf(bench.csv,",a_on,a_w,a_yaw,a_pitch,a_animyaw,a_animpitch,a_turn,a_bend,a_err,a_steady,a_lean,a_side,ik_crouch,m_crouch,m_peek,m_cant,m_walk,aw_w,aw_speed,aw_twist,aw_chest,aw_drop");
  std::fprintf(bench.csv,",m_stage,m_draw,m_hold,m_fatigue,m_breath,m_holding,m_short,m_disturb,m_amp,m_sway_yaw,m_sway_pitch,m_off,m_spread,m_cross");
  std::fprintf(bench.csv,",bd_w,bd_draw,bd_span,bd_travel,bd_miss,bd_follow,bd_fit,bd_offset,bd_angle,hd_hand,hd_fore,hd_arrow,bd_hold,bd_nock,rh_x,rh_y,rh_z,bd_held,bd_overgrip,bd_atmiddle,bd_nockgap,bd_square,bd_cant,bd_bowmove,bd_bowmiss,bd_back,bd_kick,bd_turn");
  std::fprintf(bench.csv,",lip_valid,lip_on,lip_t,lip_level,lip_mouth,lip_open,lip_f0,lip_f1,lip_alpha,lip_int,lip_picks");
  std::fprintf(bench.csv,",ge_on,ge_talk,ge_id,ge_mood,ge_starts,ge_cuts,ge_stops,ge_nods,ge_pitch,ge_yaw,ge_roll");
  std::fprintf(bench.csv,",ge_listen,ge_think,ge_lnods,ge_aside");
  std::fprintf(bench.csv,",bk_on,bk_w,bk_n,bk_pause,bk_shift,bk_double");
  for(auto& f:FaceColumns)
    std::fprintf(bench.csv,",%s",f.column);
  std::fprintf(bench.csv,",rig_on,rig_anims,rig_targets,rig_us,rig_n,rig_up_us");
  std::fprintf(bench.csv,",f_on,f_bend,f_yaw,f_twist,f_head,f_hits,f_str,f_ht");
  std::fprintf(bench.csv,",hp_player,hp_subject");
  std::fprintf(bench.csv,",stamina_player,stamina_subject");
  std::fprintf(bench.csv,",ai_on,ai_want,ai_sigma,ai_errx,ai_erry,ai_lead,ai_pv,ai_ps");
  std::fprintf(bench.csv,",mr_w,mr_dev,mr_hits,physics_ms");
  // the watched one's quiver (graphics/mesh/quiver): cm the armour and the legs go into it, the arrows shown,
  // the mouth in the pelvis bone's frame, how far out it was pushed, the arrows over the mouth and the bottom
  std::fprintf(bench.csv,",qv_on,qv_arrows,qv_armor,qv_leg,qv_arm,qv_mx,qv_my,qv_mz,qv_out,qv_top,qv_bottom,qv_swing,qv_back,qv_outward,qv_us,qv_twist,qv_bow,qv_2h,qv_blade,qv_yield,qv_legt,qv_legl,qv_gnd,qv_gndm,qv_gmode,qv_gon,qv_botup,qv_gndt,qv_need,qv_needb,qv_needo,qv_needbody,qv_trunk,qv_clear,qv_rays,qv_rayus,qv_cpu,qv_ax,qv_ay,qv_az");
  if(ragdollTrace)
    std::fprintf(bench.csv,",ph_ms,ph_bodies,ph_rays,rd_mode,rd_weight,rd_sleep");
  if(bench.routine.questTrace)
    std::fprintf(bench.csv,",ig_immortal,ig_enabled,ig_anchor,ig_armed,ig_hazards,ig_returns,ig_reason,ig_anchor_x,ig_anchor_y,ig_anchor_z,ig_x,ig_y,ig_z,ig_drop_cm,ig_in_water,ig_ko,ig_wall_ms");
  if(bench.living.livingFixture)
    std::fprintf(bench.csv,",lb_models,lb_alive,lb_kinematic,lb_parts,lb_dynamic,lb_dynamic_parts,lb_duplicates,lb_world_bodies,lb_ph_ms");
  std::fprintf(bench.csv,"\n");
  }

}

namespace testbench_detail {

std::string ragdollNumber(float value) {
  if(!std::isfinite(value))
    return "null"; // Valid JSON, with an explicit invalid measurement for the strict reader.
  char buffer[40];
  std::snprintf(buffer,sizeof(buffer),"%.9g",double(value));
  return buffer;
  }

std::string ragdollVector(const Vec3& value) {
  return "["+ragdollNumber(value.x)+","+ragdollNumber(value.y)+","+ragdollNumber(value.z)+"]";
  }

}

namespace {

void stunDriftSample(World& world, uint64_t elapsed, uint64_t requested) {
  const auto& drift=bench.npcCommands.stunDrift;
  const auto& target=*drift.target;
  const auto delta=target.position()-drift.origin;
  const auto separation=drift.attacker->position()-target.position();
  event("stun_drift","\"actor\":"+jsonStr(symName(world,&target))+
    ",\"attacker\":"+jsonStr(symName(world,drift.attacker))+",\"elapsed_ms\":"+std::to_string(elapsed)+
    ",\"requested_ms\":"+std::to_string(requested)+",\"position_cm\":"+ragdollVector(target.position())+
    ",\"attacker_position_cm\":"+ragdollVector(drift.attacker->position())+
    ",\"axis\":"+ragdollVector(drift.axis)+",\"toward_cm\":"+ragdollNumber(Vec3::dotProduct(delta,drift.axis))+
    ",\"horizontal_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+",\"height_cm\":"+ragdollNumber(delta.y)+
    ",\"separation_cm\":"+ragdollNumber(std::hypot(separation.x,separation.z))+
    ",\"stunned\":"+(target.isMeleeStunned() ? "true" : "false")+
    ",\"slide\":"+(target.isSlide() ? "true" : "false")+
    ",\"in_air\":"+(target.isInAir() ? "true" : "false")+
    ",\"melee_stun_beat\":"+(target.meleeStunSecondBeat() ? "true" : "false")+
    ",\"external\":"+(target.isExternalControl() ? "true" : "false")+
    ",\"animation\":"+jsonStr(target.animNames()));
  }

}

namespace testbench_detail {

std::string ragdollMatrix(const Matrix4x4& value) {
  std::string result="[";
  for(size_t i=0;i<16;++i) {
    if(i>0) result+=',';
    result+=ragdollNumber(value.data()[i]);
    }
  return result+"]";
  }


}


namespace {


// the animated pose of the watched one (or the hero): the largest second difference of a joint over
// three frames (cm; a pop), which joint, the fastest joint (cm/s) and the playing animations
void writeAnim() {
  const auto&  a    = bench.subject!=nullptr ? Pose::subjectProbe : Pose::playerProbe;
  const size_t n    = a.valid ? a.numBones : 0;
  float        pop  = 0, speed = 0;
  size_t       bone = size_t(-1);
  if(bench.anim2.size()==n && bench.anim1.size()==n) {
    for(size_t i=0; i<n; ++i) {
      const float d2 = (a.joint[i] - bench.anim1[i]*2.f + bench.anim2[i]).length();
      if(d2>pop) {
        pop  = d2;
        bone = i;
        }
      speed = std::max(speed,(a.joint[i] - bench.anim1[i]).length()*float(Fps));
      }
    }
  bench.anim2 = std::move(bench.anim1);
  bench.anim1.assign(a.joint,a.joint+n);

  std::string name = "-";
  if(bone!=size_t(-1) && a.skeleton!=nullptr && bone<a.skeleton->nodes.size())
    name = a.skeleton->nodes[bone].name;
  for(auto& c:name)
    if(c==' ' || c==',')
      c = '_';
  std::fprintf(bench.csv,",%.2f,%s,%.0f,%s",pop,name.c_str(),speed,a.layers.empty() ? "-" : a.layers.c_str());
  std::fprintf(bench.csv,",%d,%u,%u,%.1f,%.3f",int(AnimBlend::isEnabled()),a.blend.active,a.blend.started,a.blend.angle,a.blend.time);
  }


}

namespace {


void spellFields(World& world) {
  if(!bench.movables.spellTrace || bench.movables.spellSampleTick==world.tickCount()) return;
  bench.movables.spellSampleTick=world.tickCount();
  std::string actors="[",items="[";
  for(size_t i=0;i<bench.movables.spellActors.size();++i) {
    auto& actor=bench.movables.spellActors[i];auto& npc=*actor.npc;
    const auto* living=npc.livingBody();const auto* corpse=npc.ragdollBody();
    const auto* body=living?living:corpse;
    std::vector<Matrix4x4> native;
    const bool nativePose=body && body->getPose(native);
    if(i==0 && bench.movables.spellCase.starts_with("ice")) {
      if(npc.spellFrozen()) {npc.setExternalControl(true);bench.movables.spellWasFrozen=true;}
      else if(bench.movables.spellWasFrozen) {npc.setExternalControl(false);bench.movables.spellWasFrozen=false;}
      }
    if(i) actors+=',';
    actors+="{\"index\":"+std::to_string(i)+",\"role\":"+jsonStr(actor.role)+",\"mass_kg\":"+ragdollNumber(npc.physicsMass())+
      ",\"position_cm\":"+ragdollVector(npc.position())+",\"spell_velocity_cm_s\":"+ragdollVector(npc.spellVelocity())+
      ",\"state\":"+jsonStr(stateName(world,npc))+",\"state_time\":"+std::to_string(npc.stateTime())+
      ",\"frozen\":"+(npc.spellFrozen()?"true":"false")+",\"whirling\":"+(npc.spellWhirling()?"true":"false")+
      ",\"in_air\":"+(npc.isInAir()?"true":"false")+
      ",\"dead\":"+(npc.isDead()?"true":"false")+",\"living_parts\":"+std::to_string(living?living->partCount():0)+
      ",\"corpse_parts\":"+std::to_string(corpse?corpse->partCount():0)+
      ",\"kinematic\":"+(body && body->isKinematic()?"true":"false")+
      ",\"sleeping\":"+(body && body->isSleeping()?"true":"false")+",\"bones\":[";
    const auto* sk=npc.visualSkeleton();
    if(sk)
      for(std::string_view name:{"BIP01 PELVIS","BIP01 SPINE2","BIP01 L UPPERARM","BIP01 L CALF"}) {
        const auto bone=arrowBone(*sk,name);if(bone==size_t(-1)) continue;
        if(actors.back()!='[') actors+=',';
        actors+="{\"id\":"+std::to_string(bone)+",\"render\":"+ragdollMatrix(npc.renderedBone(bone));
        if(nativePose && bone<native.size()) actors+=",\"native\":"+ragdollMatrix(native[bone]);
        actors+='}';
        }
    actors+="]}";
    }
  for(size_t i=0;i<bench.movables.spellItems.size();++i) {
    const auto& item=*bench.movables.spellItems[i];if(i) items+=',';
    items+="{\"index\":"+std::to_string(i)+",\"position_cm\":"+ragdollVector(item.position())+
      ",\"mass_kg\":"+ragdollNumber(item.physicsMass())+",\"velocity_cm_s\":"+ragdollVector(item.physicsVelocity())+
      ",\"sleeping\":"+(item.physicsSleeping()?"true":"false")+",\"dynamic\":"+(item.isDynamic()?"true":"false")+"}";
    }
  if(bench.movables.spellGuardTrace && !bench.movables.spellActors.empty()) {
    const auto& npc=*bench.movables.spellActors[0].npc;
    const auto& q=npc.ragdollQuestReturnState();
    const auto returns=npc.ragdollQuestReturnCount();
    event("spell_guard_frame","\"position_cm\":"+ragdollVector(npc.position())+
      ",\"case\":"+jsonStr(bench.movables.spellGuardCase)+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"return_from_cm\":"+ragdollVector(npc.ragdollQuestReturnFrom())+
      ",\"hp\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
      ",\"human\":"+(npc.isHuman()?"true":"false")+",\"hero\":"+(npc.isPlayer()?"true":"false")+
      ",\"immortal\":"+(npc.isImmortal()?"true":"false")+
      ",\"in_air\":"+(npc.isInAir()?"true":"false")+",\"falling\":"+(npc.isFalling()?"true":"false")+
      ",\"standing\":"+(npc.isStanding()?"true":"false")+
      ",\"unconscious\":"+(npc.isUnconscious()?"true":"false")+",\"dead\":"+(npc.isDead()?"true":"false")+
      ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+
      ",\"hazards\":"+std::to_string(q.hazards)+",\"returns\":"+std::to_string(returns));
    if(returns>bench.movables.spellGuardObservedReturns) {
      const auto from=npc.ragdollQuestReturnFrom();
      const auto floor=world.physic()->landRay(from+Vec3(0,200,0),6000);
      const auto water=world.physic()->waterRay(from,200.f);
      event("spell_guard_landing","\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(npc.position())+
        ",\"case\":"+jsonStr(bench.movables.spellGuardCase)+",\"hazards\":"+std::to_string(q.hazards)+
        ",\"has_water\":"+(water.hasCol?"true":"false")+
        ",\"water_y_cm\":"+ragdollNumber(water.wdepth)+
        ",\"water_depth_cm\":"+ragdollNumber(water.wdepth-from.y)+
        ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"returns\":"+std::to_string(returns)+
        ",\"has_floor\":"+(floor.hasCol?"true":"false")+
        ",\"floor_cm\":"+(floor.hasCol?ragdollVector(floor.v):"null")+
        ",\"floor_normal\":"+(floor.hasCol?ragdollVector(floor.n):"null"));
      bench.movables.spellGuardObservedReturns=returns;
      }
    }
  const auto stats=world.physic()->physicsStats();
  event("spell_physics_frame","\"case\":"+jsonStr(bench.movables.spellCase)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"feature\":"+(Gothic::settingsGetI("GAME","spell_physics")?"true":"false")+
    ",\"fields\":"+std::to_string(world.spellFieldCount())+",\"world_bodies\":"+std::to_string(stats.bodies)+
    ",\"physics_ms\":"+ragdollNumber(stats.stepMilliseconds)+",\"actors\":"+actors+"],\"items\":"+items+"]");
  }

}


namespace testbench_detail {

std::optional<DynamicWorld::MovableVobSnapshot> furnitureBody(World& world,uint32_t zenId) {
  for(const auto& body:world.physic()->movableVobSnapshot())if(body.zenId==zenId)return body;
  return {};
  }

}

namespace testbench_detail {

void destructionCamera(World& world,const Vec3& source,const Vec3& direction) {
  const Vec3 side(-direction.z,0,direction.x);
  // Look back from the open end of the tavern. The caster-side view has a
  // counter post between the lens and both objects. Check the real mesh rays
  // to every prop before selecting a camera for the before/after pair.
  for(float forward:{250.f,200.f,300.f,150.f,100.f})
    for(float lateral:{-220.f,220.f,0.f,-300.f,300.f}) {
      const auto camera=source+direction*forward+side*lateral+Vec3(0,190,0);
      bool clear=true;
      for(const auto& body:world.physic()->movableVobSnapshot()) {
        if(!destructionOwns(bench.destruction,body.id))continue;
        auto center=(body.localBounds[0]+body.localBounds[1])*.5f;body.transform.project(center);
        const auto hit=world.physic()->ray(camera,center);
        if(hit.hasCol && (hit.v-center).length()>85.f)clear=false;
        }
      if(!clear)continue;
      bench.cam=C_Hold;lookFrom(camera,source+Vec3(0,55,0));
      event("destruction_camera","\"case\":"+jsonStr(bench.destruction.destructionCase)+",\"camera_cm\":"+
        ragdollVector(camera)+",\"target_cm\":"+ragdollVector(source+Vec3(0,55,0)));
      return;
      }
  arenaAbort("destruction before/after camera has no clear view of both native props");
  }

}

namespace {

void destructionSkinSample(const Npc& npc,const ProtoMesh& proto,const MdlVisual& mdl,const Matrix4x4& object,const Pose& pose) {
  auto [cache,inserted]=bench.destruction.destructionSkinGeometry.try_emplace(&proto);
  if(inserted)
    for(const auto& mesh:proto.skined) {
      std::vector<Resources::VertexA> vertices(mesh.vbo.size());
      std::vector<uint32_t> indices(mesh.ibo.size());
      if(vertices.empty() || indices.empty())continue;
      Resources::device().readBytes(mesh.vbo,vertices.data(),vertices.size()*sizeof(vertices[0]));
      Resources::device().readBytes(mesh.ibo,indices.data(),indices.size()*sizeof(indices[0]));
      std::vector<bool> used(vertices.size(),false);
      for(const auto index:indices)
        if(index<vertices.size() && !used[index]) {
          used[index]=true;
          cache->second.push_back(vertices[index]);
          }
      }
  const auto* skeleton=npc.visualSkeleton();const auto* skinSkeleton=mdl.skinSkeleton();size_t skinCount=0;const Matrix4x4* skin=mdl.skinPalette(skinCount); // as drawn: a render rig's
  if(cache->second.empty() || !skeleton || !skinSkeleton)return;
  // Probe a copy because AnimBlend::probe consumes its diagnostic counters.
  // The real animation and its telemetry remain untouched.
  Pose rawPose=pose;
  Pose::Probe raw;
  rawPose.probe(raw);
  std::string rawBones="[";
  if(raw.valid)
    for(std::string_view name:{"BIP01 PELVIS","BIP01 SPINE2","BIP01 L THIGH","BIP01 L CALF","BIP01 L FOOT",
                              "BIP01 R THIGH","BIP01 R CALF","BIP01 R FOOT"}) {
      const auto bone=arrowBone(*skeleton,name);
      if(bone>=raw.numBones)continue;
      if(rawBones.size()>1)rawBones+=',';
      rawBones+="{\"name\":"+jsonStr(name)+",\"bone\":"+std::to_string(bone)+
        ",\"raw_model_cm\":"+ragdollVector(raw.joint[bone])+"}";
      }
  rawBones+=']';
  Vec3 lowest,highest;
  lowest.y=std::numeric_limits<float>::infinity();
  highest.y=-std::numeric_limits<float>::infinity();
  size_t lowestBone=size_t(-1);
  size_t count=0;
  for(const auto& vertex:cache->second) {
    Vec3 point;
    float weight=0;
    for(size_t r=0;r<4;++r) {
      if(vertex.weights[r]<=0.f || vertex.boneId[r]>=skinCount || vertex.boneId[r]>=skinSkeleton->nodes.size())continue;
      Vec3 p(vertex.pos[r][0],vertex.pos[r][1],vertex.pos[r][2]);
      skin[vertex.boneId[r]].project(p);
      point+=p*vertex.weights[r];
      weight+=vertex.weights[r];
      }
    if(weight<=0.f)continue;
    // This is the shader's full weighted skinning, including the instance's
    // normal displacement. Dominant-bone clouds and capsule radii are unused.
    Vec3 normal;
    normal.x=object.at(0,0)*vertex.norm[0]+object.at(1,0)*vertex.norm[1]+object.at(2,0)*vertex.norm[2];
    normal.y=object.at(0,1)*vertex.norm[0]+object.at(1,1)*vertex.norm[1]+object.at(2,1)*vertex.norm[2];
    normal.z=object.at(0,2)*vertex.norm[0]+object.at(1,2)*vertex.norm[1]+object.at(2,2)*vertex.norm[2];
    point+=normal*(npc.fatness()*.5f);
    if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))continue;
    ++count;
    if(point.y<lowest.y) {
      lowest=point;
      lowestBone=vertex.boneId[size_t(std::max_element(vertex.weights,vertex.weights+4)-vertex.weights)];
      }
    if(point.y>highest.y)highest=point;
    }
  if(!count)return;
  auto& physics=*npc.world().physic();
  // The ordinary ray uses exact authored query triangles, excluding the NPC's
  // own body. Begin above both the walking root and the lowest skin vertex:
  // the latter can already be below the visible floor in the failing pose.
  const Vec3 rayFrom(lowest.x,std::max(npc.position().y+20.f,lowest.y+20.f),lowest.z);
  const Vec3 rayTo(lowest.x,std::min(npc.position().y,lowest.y)-600.f,lowest.z);
  const auto floor=physics.landRay(rayFrom,rayFrom.y-rayTo.y);
  bool found=floor.hasCol;
  Vec3 support=floor.v,supportNormal=floor.n;
  std::string kind="source_mesh",visual;
  uintptr_t parent=0;
  size_t piece=size_t(-1);
  std::unordered_map<uintptr_t,std::string> visuals;
  for(const auto& body:physics.movableVobSnapshot())visuals[body.id]=body.visual;
  // Fragments have no intact query mesh. Intersect the visible cut triangles
  // themselves, so a visible piece above the floor can be the real support.
  for(const auto& fragment:physics.fragmentSnapshot()) {
    const auto entry=visuals.find(fragment.parentId);
    if(entry==visuals.end())continue;
    const auto fracture=FractureMesh::load(entry->second);
    if(!fracture || fragment.piece>=fracture->pieces.size())continue;
    for(const auto& face:fracture->pieces[fragment.piece].triangles) {
      Vec3 vertices[3];
      for(size_t i=0;i<3;++i) {
        vertices[i]=face.vertices[i].position;
        fragment.transform.project(vertices[i]);
        }
      const auto a=vertices[1]-vertices[0],b=vertices[2]-vertices[0],p=lowest-vertices[0];
      const float determinant=a.x*b.z-a.z*b.x;
      if(std::abs(determinant)<1.e-5f)continue;
      const float u=(p.x*b.z-p.z*b.x)/determinant,v=(a.x*p.z-a.z*p.x)/determinant;
      if(u<0.f || v<0.f || u+v>1.f)continue;
      const float y=vertices[0].y+u*a.y+v*b.y;
      if(y>rayFrom.y || y<rayTo.y || (found && y<=support.y))continue;
      found=true;support=Vec3(lowest.x,y,lowest.z);
      supportNormal=Vec3::crossProduct(a,b);
      if(supportNormal.quadLength()>1.e-8f)supportNormal=Vec3::normalize(supportNormal);
      if(supportNormal.y<0.f)supportNormal*=-1.f;
      kind="fragment";visual=entry->second;parent=fragment.parentId;piece=fragment.piece;
      }
    }
  std::string probes="[";
  const auto probe=[&](std::string_view label,const Vec3& point) {
    if(probes.size()>1)probes+=',';
    probes+=destructionFloorProbe(npc,label,point);
    };
  probe("root",npc.position());
  probe("skin_lowest",lowest);probe("skin_highest",highest);
  for(std::string_view name:{"BIP01 PELVIS","BIP01 L CALF","BIP01 L FOOT","BIP01 R CALF","BIP01 R FOOT"}) {
    const auto bone=arrowBone(*skeleton,name);
    if(bone==size_t(-1))continue;
    Vec3 point;
    npc.renderedBone(bone).project(point);
    probe(name,point);
    }
  for(const auto& center:{std::pair("root",npc.position()),std::pair("skin_lowest",lowest)})
    for(const auto& offset:{std::pair("+x",Vec3(30,0,0)),std::pair("-x",Vec3(-30,0,0)),
                           std::pair("+z",Vec3(0,0,30)),std::pair("-z",Vec3(0,0,-30))})
      probe(std::string(center.first)+offset.first,center.second+offset.second);
  probes+=']';
  bench.destruction.destructionSkinSample="{\"mesh\":"+jsonStr(proto.fname)+",\"indexed_vertices\":"+std::to_string(count)+
    ",\"object_matrix\":"+ragdollMatrix(object)+
    ",\"object_up\":"+ragdollVector(Vec3(object.at(1,0),object.at(1,1),object.at(1,2)))+
    ",\"raw_model_bones\":"+rawBones+
    ",\"lowest_world_cm\":"+ragdollVector(lowest)+",\"highest_world_cm\":"+ragdollVector(highest)+
    ",\"lowest_dominant_bone\":"+(lowestBone<skinSkeleton->nodes.size()?jsonStr(skinSkeleton->nodes[lowestBone].name):"null")+
    ",\"ray_from_cm\":"+ragdollVector(rayFrom)+",\"ray_to_cm\":"+ragdollVector(rayTo)+
    ",\"support_found\":"+(found?"true":"false")+
    ",\"support_kind\":"+jsonStr(kind)+",\"support_visual\":"+jsonStr(visual)+
    ",\"support_parent_id\":"+std::to_string(parent)+",\"support_piece\":"+
    (piece==size_t(-1)?"null":std::to_string(piece))+
    ",\"support_world_cm\":"+(found?ragdollVector(support):"null")+
    ",\"support_normal\":"+(found?ragdollVector(supportNormal):"null")+
    ",\"gap_cm\":"+(found?ragdollNumber(lowest.y-support.y):"null")+",\"floor_probes\":"+probes+"}";
  }

// These fixtures only observe authored cloth and launch ordinary Bullet objects. No
// synthetic impulse, replacement cloth or NPC target participates in acceptance.
std::string clothRows(World& world,bool geometry=false) {
  std::string rows="[";
  for(const auto& body:world.physic()->cloth().snapshots()) {
    if(rows.size()>1)rows+=",";
    rows+="{\"id\":"+std::to_string(body.id)+",\"zen_id\":"+std::to_string(body.zenId)+
      ",\"visual\":"+jsonStr(body.visual)+",\"center_cm\":"+ragdollVector(body.center)+
      ",\"active\":"+(body.active?"true":"false")+",\"exposed\":"+(body.exposed?"true":"false")+
      ",\"visible\":"+(body.visible?"true":"false")+",\"simulation_steps\":"+std::to_string(body.simulationSteps)+
      ",\"vertices\":"+std::to_string(body.vertices)+
      ",\"rendered_vertices\":"+std::to_string(body.worldPositionsCm.size())+
      ",\"pinned\":"+std::to_string(body.pinned)+",\"weight\":"+ragdollNumber(body.simulationWeight)+
      ",\"anchor_error_cm\":"+ragdollNumber(body.maxAnchorErrorCm)+
      ",\"max_speed_cm_s\":"+ragdollNumber(body.maxSpeedCmPerSec)+
      ",\"max_speed_ever_cm_s\":"+ragdollNumber(body.maxSpeedEverCmPerSec)+
      ",\"contact_vertices\":"+std::to_string(body.contactVertices)+
      ",\"max_speed_vertex\":"+std::to_string(body.maxSpeedVertex)+
      ",\"max_speed_source_cm\":"+ragdollVector(body.maxSpeedSourceCm)+
      ",\"max_speed_world_cm\":"+ragdollVector(body.maxSpeedWorldCm)+
      ",\"max_speed_contact\":"+(body.maxSpeedContact?"true":"false")+
      ",\"max_speed_contact_body\":"+jsonStr(body.maxSpeedContactBody)+
      ",\"max_speed_contact_normal\":"+ragdollVector(body.maxSpeedContactNormal)+
      ",\"max_speed_ever_vertex\":"+std::to_string(body.maxSpeedEverVertex)+
      ",\"max_speed_ever_source_cm\":"+ragdollVector(body.maxSpeedEverSourceCm)+
      ",\"max_speed_ever_world_cm\":"+ragdollVector(body.maxSpeedEverWorldCm)+
      ",\"max_speed_ever_contact\":"+(body.maxSpeedEverContact?"true":"false")+
      ",\"max_speed_ever_contact_body\":"+jsonStr(body.maxSpeedEverContactBody)+
      ",\"max_speed_ever_contact_normal\":"+ragdollVector(body.maxSpeedEverContactNormal)+
      ",\"transition_blend_delta_cm\":"+ragdollNumber(body.transitionMaxDeltaCm)+
      ",\"spell_hits\":"+std::to_string(body.projectileHits)+",\"arrow_passes\":"+std::to_string(body.arrowPasses)+
      ",\"recoveries\":"+std::to_string(body.recoveryCount);
    rows+=",\"contact_bodies\":[";
    for(size_t i=0;i<body.contactBodies.size();++i) {
      if(i) rows+=",";
      rows+=jsonStr(body.contactBodies[i]);
      }
    rows+="]";
    rows+=",\"wind_occluders\":[";
    for(size_t i=0;i<body.windOccluders.size();++i) {
      if(i) rows+=",";
      const auto& roof=body.windOccluders[i];
      rows+="{\"body\":"+jsonStr(roof.body)+",\"point_cm\":"+ragdollVector(roof.pointCm)+
            ",\"distance_cm\":"+ragdollNumber(roof.distanceCm)+"}";
      }
    rows+="]";
    if(geometry && body.zenId==bench.clothZen) {
      rows+=",\"rendered_world_vertices_cm\":[";
      for(size_t i=0;i<body.worldPositionsCm.size();++i) {
        if(i)rows+=",";
        rows+=ragdollVector(body.worldPositionsCm[i]);
        }
      rows+="]";
      }
    rows+="}";
    }
  return rows+"]";
  }

void clothFields(World& world) {
  if(!bench.clothTrace || bench.clothMeasuring || bench.clothSample==world.tickCount())return;
  bench.clothSample=world.tickCount();
  float frameDelta=0;
  bool changed=false;
  for(const auto& body:world.physic()->cloth().snapshots()) {
    auto& old=bench.clothPrevious[body.id];
    if(body.zenId==bench.clothZen && old.size()==body.worldPositionsCm.size())
      for(size_t i=0;i<old.size();++i)frameDelta=std::max(frameDelta,(old[i]-body.worldPositionsCm[i]).length());
    if(const auto i=bench.clothPreviousActive.find(body.id);i!=bench.clothPreviousActive.end())
      changed|=body.zenId==bench.clothZen && i->second!=body.active;
    bench.clothPreviousActive[body.id]=body.active;old=body.worldPositionsCm;
    }
  event("cloth_sample","\"case\":"+jsonStr(bench.clothCase)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"hero_cm\":"+ragdollVector(world.player()->position())+
    ",\"hero_target_distance_cm\":"+ragdollNumber((world.player()->position()-bench.clothCenter).length())+
    ",\"target_max_frame_delta_cm\":"+ragdollNumber(frameDelta)+
    ",\"target_active_changed\":"+(changed?"true":"false")+",\"cloth\":"+clothRows(world,changed));
  }

void clothCheckpoint(World& world,std::string_view label) {
  event("cloth_checkpoint","\"case\":"+jsonStr(bench.clothCase)+",\"label\":"+jsonStr(label)+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+",\"enabled\":"+
    (world.physic()->clothEnabled()?"true":"false")+",\"wind_scale\":"+
    ragdollNumber(world.physic()->cloth().windScale())+",\"cloth\":"+clothRows(world,true));
  }

bool clothStandAt(World& world,Npc& player,const Vec3& target,float distance) {
  for(float angle:{0.f,.4f,-.4f,.8f,-.8f,1.6f,-1.6f,3.14f}) {
    const float ca=std::cos(angle),sa=std::sin(angle);
    const auto base=bench.clothDirection;
    const Vec3 direction(base.x*ca-base.z*sa,0,base.x*sa+base.z*ca);
    float horizontal=distance;
    Vec3 point;
    bool supported=true;
    // Resolve the horizontal distance against the native floor, retaining the
    // requested 3D observer radius when a banner is well above street level.
    for(int pass=0;pass<4;++pass) {
      const auto floor=world.physic()->landRay(target+direction*horizontal+Vec3(0,6000,0),12000);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-target.y)>=distance ||
         world.physic()->waterRay(floor.v+Vec3(0,5,0),1).hasCol) {supported=false;break;}
      point=floor.v+Vec3(0,1,0);
      horizontal=std::sqrt(std::max(0.f,distance*distance-(point.y-target.y)*(point.y-target.y)));
      }
    if(!supported || std::abs((point-target).length()-distance)>1.f)continue;
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(point);
    player.setDirection(target-point);player.updateTransform();
    bench.clothDirection=direction;
    bench.clothHeldPosition=point;bench.clothHold=true;
    return true;
    }
  return false;
  }

bool clothObserverFloor(World& world,const Vec3& position,Vec3& supported) {
  const auto floor=world.physic()->landRay(position+Vec3(0,10000,0),20000);
  if(!floor.hasCol || floor.n.y<.8f || world.physic()->waterRay(floor.v+Vec3(0,5,0),1).hasCol)
    return false;
  supported=floor.v+Vec3(0,1,0);
  // A native roof or yard is a valid fixture support, provided the hero fits.
  return !world.physic()->ray(supported+Vec3(0,5,0),supported+Vec3(0,180,0)).hasCol;
  }

bool clothNearbyObserver(World& world,const Vec3& target,Vec3& supported) {
  float score=std::numeric_limits<float>::max();
  // High ship flags cannot be observed from street level within the production
  // 40 m sphere. Search native yards and decks as well as ordinary ground.
  for(float radius:{0.f,100.f,200.f,350.f,500.f,750.f,1000.f,1500.f,2000.f,2500.f,3000.f}) {
    const int steps=radius==0.f?1:48;
    for(int step=0;step<steps;++step) {
      const float angle=float(step)*2.f*float(M_PI)/float(steps);
      Vec3 candidate;
      if(!clothObserverFloor(world,target+Vec3(std::cos(angle)*radius,0,std::sin(angle)*radius),candidate))continue;
      const auto distance=(candidate-target).length();
      if(distance>3400.f)continue;
      // Prefer a little room around a native support over balancing on a mast.
      unsigned room=0;
      for(const auto offset:{Vec3(25,0,0),Vec3(-25,0,0),Vec3(0,0,25),Vec3(0,0,-25)}) {
        const auto nearby=world.physic()->landRay(candidate+offset+Vec3(0,30,0),60);
        room+=nearby.hasCol && nearby.n.y>.8f && std::abs(nearby.v.y-candidate.y)<10.f;
        }
      const float cost=std::abs(distance-1800.f)+float(4-room)*500.f;
      if(cost<score) {score=cost;supported=candidate;}
      }
    }
  return std::isfinite(score) && score<std::numeric_limits<float>::max();
  }

std::string clothUpper(std::string text) {
  for(auto& c:text)c=char(std::toupper(static_cast<unsigned char>(c)));
  return text;
  }

bool clothCamera(World& world,std::string_view group,bool required=true,Vec3* targetOut=nullptr);

float clothCameraDistance(const std::vector<Vec3>& points,const Vec3& target,const Vec3& direction) {
  const auto* camera=Gothic::inst().camera();
  const float halfY=(camera?camera->fovY():67.5f)*float(M_PI)/360.f;
  const auto projection=camera?camera->projective():Matrix4x4();
  const float aspect=camera?std::abs(projection.at(1,1)/projection.at(0,0)):4.f/3.f;
  const float tanY=std::tan(halfY),tanX=tanY*aspect;
  auto right=Vec3::crossProduct(direction,Vec3(0,1,0));
  if(right.length()<.01f)right=Vec3(1,0,0);
  else right=Vec3::normalize(right);
  const auto up=Vec3::normalize(Vec3::crossProduct(right,direction));
  float distance=650.f;
  for(const auto& point:points) {
    const auto delta=point-target;
    const float depth=Vec3::dotProduct(delta,direction);
    // Fit the actual projected geometry, with room for flutter at all edges.
    distance=std::max(distance,depth+1.2f*(std::abs(Vec3::dotProduct(delta,right))+25.f)/tanX);
    distance=std::max(distance,depth+1.2f*(std::abs(Vec3::dotProduct(delta,up))+25.f)/tanY);
    }
  return distance;
  }

bool clothFrame(World& world,const Cloth::Snapshot& body,Vec3& cameraEye,Vec3& cameraTarget,
                Vec3& cameraNormal,float& span) {
  const auto& reference=body.referenceWorldPositionsCm.empty()?body.worldPositionsCm:body.referenceWorldPositionsCm;
  if(reference.empty())return false;
  float referenceLow=reference.front().y,referenceHigh=reference.front().y;
  for(const auto& point:reference) {referenceLow=std::min(referenceLow,point.y);referenceHigh=std::max(referenceHigh,point.y);}
  // Only a short original bottom strip can be buried. An EARTH material on an
  // upper storey or awning must not erase the top of a city banner.
  const float burialCeiling=referenceLow+std::min(75.f,(referenceHigh-referenceLow)*.2f);
  const auto earthFloor=[&](const Vec3& point)->std::optional<float> {
    auto from=point+Vec3(0,200,0);
    const float bottom=point.y-20.f;
    for(int pass=0;pass<8 && from.y>bottom;++pass) {
      const auto hit=world.physic()->landRay(from,from.y-bottom);
      if(!hit.hasCol)return {};
      if(hit.mat==asset::MaterialGroup::EARTH && hit.n.y>.7f)return hit.v.y;
      from.y=hit.v.y-.5f;
      }
    return {};
    };
  std::vector<bool> sourceBuried(reference.size(),false),currentBuried(body.worldPositionsCm.size(),false);
  size_t buriedSource=0,buriedCurrent=0;
  float maximumBurial=0;
  Vec3 buriedLow,buriedHigh;
  for(size_t i=0;i<reference.size();++i) {
    if(reference[i].y>burialCeiling)continue;
    const auto floor=earthFloor(reference[i]);
    if(!floor || *floor>burialCeiling || reference[i].y>=*floor-2.f)continue;
    sourceBuried[i]=true;
    if(!buriedSource)buriedLow=buriedHigh=reference[i];
    ++buriedSource;maximumBurial=std::max(maximumBurial,*floor-reference[i].y);
    buriedLow.x=std::min(buriedLow.x,reference[i].x);buriedLow.y=std::min(buriedLow.y,reference[i].y);buriedLow.z=std::min(buriedLow.z,reference[i].z);
    buriedHigh.x=std::max(buriedHigh.x,reference[i].x);buriedHigh.y=std::max(buriedHigh.y,reference[i].y);buriedHigh.z=std::max(buriedHigh.z,reference[i].z);
    if(i<body.worldPositionsCm.size()) {
      const auto currentFloor=earthFloor(body.worldPositionsCm[i]);
      if(currentFloor && *currentFloor<=burialCeiling && body.worldPositionsCm[i].y<*currentFloor-2.f) {currentBuried[i]=true;++buriedCurrent;}
      }
    }
  std::vector<Vec3> fit;
  for(size_t i=0;i<reference.size();++i)if(!sourceBuried[i])fit.push_back(reference[i]);
  for(size_t i=0;i<body.worldPositionsCm.size();++i)if(!currentBuried[i])fit.push_back(body.worldPositionsCm[i]);
  if(fit.empty())return false;
  Vec3 bounds[2]={fit.front(),fit.front()};
  for(const auto& point:fit) {
    bounds[0].x=std::min(bounds[0].x,point.x);bounds[0].y=std::min(bounds[0].y,point.y);bounds[0].z=std::min(bounds[0].z,point.z);
    bounds[1].x=std::max(bounds[1].x,point.x);bounds[1].y=std::max(bounds[1].y,point.y);bounds[1].z=std::max(bounds[1].z,point.z);
    }
  float area=0;
  for(size_t i=0;i+2<body.indices.size();i+=3) {
    const auto a=body.indices[i],b=body.indices[i+1],c=body.indices[i+2];
    if(a>=reference.size() || b>=reference.size() || c>=reference.size())continue;
    const auto normal=Vec3::crossProduct(reference[b]-reference[a],reference[c]-reference[a]);
    const auto next=normal.length();
    if(next>area && (std::abs(normal.y)<next*.9f || area==0.f)) {area=next;cameraNormal=Vec3::normalize(normal);}
    }
  if(area<.01f)return false;
  cameraTarget=(bounds[0]+bounds[1])*.5f;
  span=(bounds[1]-bounds[0]).length();
  const auto hiddenByCloth=[&](const Vec3& from,const Vec3& to,const std::vector<Vec3>& pose) {
    const auto direction=to-from;
    for(size_t i=0;i+2<body.indices.size();i+=3) {
      const auto a=body.indices[i],b=body.indices[i+1],c=body.indices[i+2];
      if(a>=pose.size() || b>=pose.size() || c>=pose.size())continue;
      const auto edge1=pose[b]-pose[a],edge2=pose[c]-pose[a];
      const auto cross=Vec3::crossProduct(direction,edge2);
      const float determinant=Vec3::dotProduct(edge1,cross);
      if(std::abs(determinant)<1.e-6f)continue;
      const auto offset=from-pose[a];
      const float u=Vec3::dotProduct(offset,cross)/determinant;
      if(u<0.f || u>1.f)continue;
      const auto q=Vec3::crossProduct(offset,edge1);
      const float v=Vec3::dotProduct(direction,q)/determinant;
      if(v<0.f || u+v>1.f)continue;
      const float distance=Vec3::dotProduct(edge2,q)/determinant;
      if(distance>=0.f && distance<.9999f)return true;
      }
    return false;
    };
  bool cameraClear=false;
  size_t candidates=0,tested=0,hidden=0,attachments=0,selfRigidHits=0,bestBlocked=size_t(-1);
  std::string firstBlockedLabel;
  Vec3 firstBlockedPosition={},firstBlockedTarget={};
  float selectedAngle=0,selectedElevation=0;
  // A cloth's rear face can be hidden by its own front face. The authored
  // below-ground section also cannot be photographed; neither masks world walls.
  for(float elevation:{.55f,.8f,1.1f,0.f,-.3f,-.55f}) {
    // High ship cloth hangs underneath its yard/stay. A view from below can
    // show the fabric without putting the attachment in front of it.
    if(elevation<=0.f && bounds[0].y<1000.f)continue;
    for(float angle:{0.f,15.f,-15.f,30.f,-30.f,60.f,-60.f,180.f,195.f,165.f,210.f,150.f}) {
      ++candidates;
      const auto normal=rotY(cameraNormal,angle);
      const auto direction=Vec3::normalize(normal+Vec3(0,elevation,0));
      const float cameraDistance=clothCameraDistance(fit,cameraTarget,direction);
      const auto eye=cameraTarget+direction*cameraDistance;
      if(bounds[1].y<1000.f && eye.y<bounds[1].y+100.f)continue;
      size_t blocked=0,candidateTested=0,candidateHidden=0,candidateAttachments=0,candidateSelfHits=0;
      std::string candidateBlockedLabel;
      Vec3 candidateBlockedPosition={},candidateBlockedTarget={};
      const auto checkPose=[&](const std::vector<Vec3>& pose,const std::vector<bool>& buried) {
        for(size_t vertex=0;vertex<pose.size();++vertex) {
          if(buried[vertex])continue;
          // Authored pins can lie inside their beam or stone mount. They
          // remain in the frame bounds; only the free cloth needs a clear ray.
          if(std::any_of(body.anchorsCm.begin(),body.anchorsCm.end(),[&](const Vec3& anchor) {
               return (pose[vertex]-anchor).quadLength()<.0625f;
               })) {++candidateAttachments;continue;}
          const auto inward=cameraTarget-pose[vertex];
          const float length=inward.length();
          const auto target=pose[vertex]+(length>2.f?inward*(2.f/length):inward);
          const auto end=target-Vec3::normalize(target-eye)*2.f;
          if(hiddenByCloth(eye,end,pose)) {++candidateHidden;continue;}
          ++candidateTested;
          std::string label;
          const auto hit=world.physic()->rayClothCamera(eye,end,body.visual,bounds,candidateSelfHits,&label);
          if(!hit.hasCol)continue;
          bool attachment=false;
          if(hit.mat!=asset::MaterialGroup::EARTH && !hit.vob && (hit.v-target).length()<=20.f) {
            for(const auto& anchor:body.anchorsCm)
              if((target-anchor).length()<=25.f && (hit.v-anchor).length()<=25.f) {attachment=true;break;}
            }
          if(attachment)++candidateAttachments;
          else {
            if(!blocked) {candidateBlockedLabel=label;candidateBlockedPosition=hit.v;candidateBlockedTarget=target;}
            ++blocked;
            }
          }
        };
      checkPose(reference,sourceBuried);checkPose(body.worldPositionsCm,currentBuried);
      const auto ground=world.physic()->landRay(eye+Vec3(0,500,0),1000);
      const bool groundAccepted=!ground.hasCol || eye.y-ground.v.y>=180.f;
      if(blocked<bestBlocked) {
        bestBlocked=blocked;tested=candidateTested;hidden=candidateHidden;attachments=candidateAttachments;selfRigidHits=candidateSelfHits;
        firstBlockedLabel=candidateBlockedLabel;firstBlockedPosition=candidateBlockedPosition;firstBlockedTarget=candidateBlockedTarget;
        }
      if(blocked || !groundAccepted || !candidateTested)continue;
      cameraEye=eye;cameraNormal=normal;cameraClear=true;
      tested=candidateTested;hidden=candidateHidden;attachments=candidateAttachments;selfRigidHits=candidateSelfHits;
      selectedAngle=angle;selectedElevation=elevation;break;
      }
    if(cameraClear)break;
    }
  event("cloth_camera_frame","\"zen_id\":"+std::to_string(body.zenId)+",\"visual\":"+jsonStr(body.visual)+
    ",\"reference_vertices\":"+std::to_string(reference.size())+",\"current_vertices\":"+std::to_string(body.worldPositionsCm.size())+
    ",\"buried_reference_vertices\":"+std::to_string(buriedSource)+",\"buried_current_vertices\":"+std::to_string(buriedCurrent)+
    ",\"buried_bounds_min_cm\":"+(buriedSource?ragdollVector(buriedLow):"null")+
    ",\"buried_bounds_max_cm\":"+(buriedSource?ragdollVector(buriedHigh):"null")+
    ",\"maximum_original_burial_cm\":"+ragdollNumber(maximumBurial)+",\"fit_vertices\":"+std::to_string(fit.size())+
    ",\"burial_strip_ceiling_cm\":"+ragdollNumber(burialCeiling)+",\"self_rigid_hits\":"+std::to_string(selfRigidHits)+
    ",\"tested_visible_vertices\":"+std::to_string(tested)+",\"self_hidden_vertices\":"+std::to_string(hidden)+
    ",\"attachment_allowances\":"+std::to_string(attachments)+",\"minimum_blocked_vertices\":"+std::to_string(bestBlocked)+
    ",\"candidates\":"+std::to_string(candidates)+",\"clear\":"+(cameraClear?"true":"false")+
    ",\"azimuth_deg\":"+ragdollNumber(selectedAngle)+",\"elevation\":"+ragdollNumber(selectedElevation)+
    ",\"first_blocked_label\":"+jsonStr(firstBlockedLabel)+
    ",\"first_blocked_position_cm\":"+(bestBlocked?ragdollVector(firstBlockedPosition):"null")+
    ",\"first_blocked_target_cm\":"+(bestBlocked?ragdollVector(firstBlockedTarget):"null")+
    ",\"first_blocked_distance_cm\":"+(bestBlocked?ragdollNumber((firstBlockedPosition-firstBlockedTarget).length()):"null")+
    ",\"bounds_min_cm\":"+ragdollVector(bounds[0])+",\"bounds_max_cm\":"+ragdollVector(bounds[1]));
  return cameraClear;
  }

void clothSetup(World& world,Npc& player,std::string selector,std::string label) {
  const auto all=world.physic()->cloth().snapshots();
  if(all.empty()) {arenaAbort("cloth fixture world has no authored cloth");return;}
  auto found=all.end();
  Vec3 denseObserver;
  bool dense=false,denseSupported=false;
  bench.clothObserverSupport="native-floor";
  if(selector=="densest") {
    size_t maximum=0;float nearest=std::numeric_limits<float>::max();
    // Search observer positions and count the unchanged real 3D radius.
    // Cluster centroids include the gaps between cloth objects, where a larger
    // number may be active than when standing directly beside one flag.
    std::vector<Vec3> centers;
    for(const auto& body:all) {
      centers.push_back(body.center);
      Vec3 center;size_t count=0;
      for(const auto& other:all)if((other.center-body.center).length()<7000.f) {center+=other.center;++count;}
      if(count)centers.push_back(center/float(count));
      }
    for(const auto& center:centers) {
      Vec3 floor;
      const bool hasFloor=clothNearbyObserver(world,center,floor);
      for(bool onFloor:{true,false}) {
        if(onFloor && !hasFloor)continue;
        // Static flight is a test-bench observer only, equivalent to Marvin
        // flying up the mast. Production always keeps its unchanged 3D radius.
        const auto observer=onFloor?floor:center-Vec3(0,100,0);
        size_t count=0;float minimum=std::numeric_limits<float>::max();auto target=all.end();
        for(auto i=all.begin();i!=all.end();++i) {
          const auto distance=(i->center-observer).length();
          count+=distance<Cloth::radiusCm;
          if(distance<minimum) {minimum=distance;target=i;}
          }
        if(count>maximum || (count==maximum && onFloor && !denseSupported) ||
           (count==maximum && onFloor==denseSupported && minimum<nearest)) {
          maximum=count;nearest=minimum;found=target;denseObserver=observer;dense=true;denseSupported=onFloor;
          }
        }
      }
    }
  else if(selector.find_first_not_of("0123456789")==std::string::npos) {
    const auto zen=uint32_t(std::strtoull(selector.c_str(),nullptr,10));
    found=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==zen;});
    }
  else {
    selector=clothUpper(std::move(selector));
    found=std::find_if(all.begin(),all.end(),[&](const auto& body){return clothUpper(body.visual)==selector;});
    if(found==all.end())
      found=std::find_if(all.begin(),all.end(),[&](const auto& body){return clothUpper(body.visual).find(selector)!=std::string::npos;});
    }
  if(found==all.end()) {arenaAbort("cloth fixture requires an authored cloth id or visual selector");return;}
  Vec3 cameraEye,cameraTarget,cameraNormal;float span=0;
  bool cameraClear=false;
  if(dense && clothUpper(found->visual).find("NW_SHIP_")!=std::string::npos) {
    // Density selection is independent of the camera. A nearest furled sail can
    // sit against its yard, whereas the complete ship has a clear broadside.
    cameraClear=clothCamera(world,"harbour",false,&cameraTarget);
    if(cameraClear) {
      cameraEye=bench.camPos;span=(cameraEye-cameraTarget).length()/1.6f;
      cameraNormal=Vec3::normalize(cameraEye-cameraTarget);
      }
    }
  else cameraClear=clothFrame(world,*found,cameraEye,cameraTarget,cameraNormal,span);
  if(!cameraClear && dense) {
    // Retain the exact observer and active body count. Only the photographed
    // cloth target changes if another member of the cluster is unobstructed.
    for(auto candidate=all.begin();candidate!=all.end();++candidate) {
      if((candidate->center-denseObserver).length()>=Cloth::radiusCm)continue;
      if(!clothFrame(world,*candidate,cameraEye,cameraTarget,cameraNormal,span))continue;
      found=candidate;cameraClear=true;break;
      }
    }
  if(!cameraClear) {arenaAbort("cloth fixture has no unobstructed fully framed camera side");return;}
  bench.clothZen=found->zenId;bench.clothCase=std::move(label);bench.clothCenter=found->center;
  bench.clothDirection=Vec3(cameraNormal.x,0,cameraNormal.z);
  if(bench.clothDirection.length()<.01f)bench.clothDirection=Vec3(1,0,0);
  else bench.clothDirection=Vec3::normalize(bench.clothDirection);
  bool supported=false;
  if(dense) {
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(denseObserver);
    player.setDirection(found->center-denseObserver);player.updateTransform();
    bench.clothHeldPosition=denseObserver;bench.clothHold=true;supported=true;
    bench.clothObserverSupport=denseSupported?"native-floor":"fixture-static-flight";
    }
  else supported=clothStandAt(world,player,found->center,1800.f) || clothStandAt(world,player,found->center,3200.f);
  if(!supported) {
    Vec3 observer;
    if(clothNearbyObserver(world,found->center,observer)) {
      player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(observer);
      player.setDirection(found->center-observer);player.updateTransform();
      bench.clothHeldPosition=observer;bench.clothHold=true;supported=true;
      }
    }
  if(!supported) {
    const auto observer=found->center+bench.clothDirection*1800.f;
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(observer);
    player.setDirection(found->center-observer);player.updateTransform();
    bench.clothHeldPosition=observer;bench.clothHold=true;
    bench.clothObserverSupport="fixture-static-flight";
    }
  bench.subject=nullptr;bench.cam=C_Hold;lookFrom(cameraEye,cameraTarget);
  bench.clothPrevious.clear();bench.clothPreviousActive.clear();bench.clothTrace=true;
  world.physic()->cloth().resetStats();
  size_t nearby=0;for(const auto& body:all)nearby+=(body.center-player.position()).length()<Cloth::radiusCm;
  event("cloth_setup","\"case\":"+jsonStr(bench.clothCase)+",\"world\":"+jsonStr(world.name())+
    ",\"selector\":"+jsonStr(selector)+",\"zen_id\":"+std::to_string(found->zenId)+",\"visual\":"+jsonStr(found->visual)+
    ",\"center_cm\":"+ragdollVector(found->center)+",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"observer_support\":"+jsonStr(bench.clothObserverSupport)+
    ",\"nearby_cloth_count\":"+std::to_string(nearby)+",\"camera_cm\":"+ragdollVector(cameraEye)+
    ",\"camera_target_cm\":"+ragdollVector(cameraTarget)+",\"camera_face_normal\":"+ragdollVector(cameraNormal)+
    ",\"camera_frame_span_cm\":"+ragdollNumber(span));
  }

bool clothCamera(World& world,std::string_view group,bool required,Vec3* targetOut) {
  if(group!="harbour") {if(required)arenaAbort("cloth camera group expects harbour");return false;}
  const auto all=world.physic()->cloth().snapshots();
  std::vector<Vec3> points;
  size_t objects=0;
  for(const auto& body:all)if(clothUpper(body.visual).find("NW_SHIP_")!=std::string::npos) {
    points.insert(points.end(),body.worldPositionsCm.begin(),body.worldPositionsCm.end());++objects;
    }
  if(points.empty()) {if(required)arenaAbort("harbour camera requires the authored ship cloth");return false;}
  Vec3 low=points.front(),high=low;
  for(const auto& point:points) {
    low.x=std::min(low.x,point.x);low.y=std::min(low.y,point.y);low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x);high.y=std::max(high.y,point.y);high.z=std::max(high.z,point.z);
    }
  const auto target=(low+high)*.5f;
  Vec3 eye;bool clear=false;
  for(float angle:{3.14f,0.f,2.74f,.4f,3.54f,-.4f,2.34f,.8f}) {
    const auto direction=Vec3::normalize(Vec3(std::cos(angle),.3f,std::sin(angle)));
    const auto distance=clothCameraDistance(points,target,direction);
    eye=target+direction*distance;
    bool visible=true;
    for(const auto& body:all)if(clothUpper(body.visual).find("NW_SHIP_")!=std::string::npos) {
      const auto delta=body.center-eye;
      visible&=!world.physic()->ray(eye,eye+delta*.97f).hasCol;
      }
    if(visible) {clear=true;break;}
    }
  if(!clear) {if(required)arenaAbort("harbour camera has no unobstructed view of every authored cloth object");return false;}
  bench.subject=nullptr;bench.cam=C_Hold;lookFrom(eye,target);
  if(targetOut)*targetOut=target;
  event("cloth_camera","\"group\":"+jsonStr(group)+",\"objects\":"+std::to_string(objects)+
    ",\"camera_cm\":"+ragdollVector(eye)+",\"target_cm\":"+ragdollVector(target)+
    ",\"bounds_min_cm\":"+ragdollVector(low)+",\"bounds_max_cm\":"+ragdollVector(high));
  return true;
  }

void clothCameraRange(World& world,float range) {
  if(bench.cam!=C_Hold || Gothic::inst().camera()==nullptr || bench.clothZen==uint32_t(-1) ||
     !std::isfinite(range) || range<=0.f) {
    arenaAbort("cloth camera-range requires held selected cloth and a positive finite range");return;
    }
  const auto all=world.physic()->cloth().snapshots();
  const auto body=std::find_if(all.begin(),all.end(),[](const auto& value){return value.zenId==bench.clothZen;});
  if(body==all.end() || body->worldPositionsCm.empty()) {arenaAbort("cloth camera-range selected cloth is absent");return;}
  auto side=bench.camPos-bench.clothCenter;side.y=0.f;
  const float previousRange=side.length();
  if(previousRange<.01f || range<previousRange) {arenaAbort("cloth camera-range must increase its existing horizontal range");return;}
  const auto eye=bench.clothCenter+side*(range/previousRange)+Vec3(0,bench.camPos.y-bench.clothCenter.y,0);
  const auto& reference=body->referenceWorldPositionsCm.empty()?body->worldPositionsCm:body->referenceWorldPositionsCm;
  Vec3 bounds[2]={reference.front(),reference.front()};
  const auto addBounds=[&](const auto& pose) {
    for(const auto& point:pose) {
      bounds[0].x=std::min(bounds[0].x,point.x);bounds[0].y=std::min(bounds[0].y,point.y);bounds[0].z=std::min(bounds[0].z,point.z);
      bounds[1].x=std::max(bounds[1].x,point.x);bounds[1].y=std::max(bounds[1].y,point.y);bounds[1].z=std::max(bounds[1].z,point.z);
      }
    };
  addBounds(reference);addBounds(body->worldPositionsCm);
  lookFrom(eye,bench.clothCenter);
  size_t tested=0,blocked=0,attachments=0,selfHits=0;bool framed=true;
  const auto checkPose=[&](const auto& pose) {
    for(const auto& point:pose) {
      framed&=arenaProjected(point);
      if(std::any_of(body->anchorsCm.begin(),body->anchorsCm.end(),[&](const Vec3& anchor) {
           return (point-anchor).quadLength()<.0625f;
           })) {++attachments;continue;}
      const auto inward=bench.clothCenter-point;
      const float length=inward.length();
      const auto target=point+(length>2.f?inward*(2.f/length):inward);
      const auto end=target-Vec3::normalize(target-eye)*2.f;
      ++tested;
      const auto hit=world.physic()->rayClothCamera(eye,end,body->visual,bounds,selfHits);
      if(!hit.hasCol)continue;
      bool attachment=false;
      if(hit.mat!=asset::MaterialGroup::EARTH && !hit.vob && (hit.v-target).length()<=20.f)
        attachment=std::any_of(body->anchorsCm.begin(),body->anchorsCm.end(),[&](const Vec3& anchor) {
          return (target-anchor).length()<=25.f && (hit.v-anchor).length()<=25.f;
          });
      if(attachment)++attachments;else ++blocked;
      }
    };
  checkPose(reference);checkPose(body->worldPositionsCm);
  const bool clear=framed && tested>0 && blocked==0;
  event("cloth_camera_range","\"horizontal_range_cm\":"+ragdollNumber(range)+
    ",\"previous_horizontal_range_cm\":"+ragdollNumber(previousRange)+",\"camera_cm\":"+ragdollVector(eye)+
    ",\"target_cm\":"+ragdollVector(bench.clothCenter)+",\"pitch_degrees\":"+ragdollNumber(bench.camHeldAngles.x)+
    ",\"yaw_degrees\":"+ragdollNumber(bench.camHeldAngles.y)+",\"tested_vertices\":"+std::to_string(tested)+
    ",\"blocked_vertices\":"+std::to_string(blocked)+",\"attachment_allowances\":"+std::to_string(attachments)+
    ",\"self_rigid_hits\":"+std::to_string(selfHits)+",\"fully_framed\":"+(framed?"true":"false")+
    ",\"clear\":"+(clear?"true":"false"));
  if(!clear)arenaAbort("cloth camera-range has no unobstructed fully framed view");
  }

void clothSaveSession(World& world,const std::string& label) {
  if(label.empty() || label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
    throw std::runtime_error("invalid cloth save label");
  std::vector<uint8_t> bytes;
  {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
  Gothic::inst().gameSession()->save(out,"cloth "+label,preview);}
  const auto path="session-cloth-"+label+".sav";
  std::ofstream file(outDir+"/"+path,std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file)throw std::runtime_error("cannot write complete cloth GameSession archive");
  event("cloth_session_save","\"archive\":"+jsonStr(path)+",\"bytes\":"+std::to_string(bytes.size())+
    ",\"world\":"+jsonStr(world.name())+",\"cloth\":"+clothRows(world));
  }

void clothInitial(World& world,std::string_view label) {
  bool rest=true;size_t count=0;
  for(const auto& body:world.physic()->cloth().snapshots()) {
    ++count;
    rest&=!body.active && body.maxSpeedCmPerSec==0.f && body.maxSpeedEverCmPerSec==0.f &&
      body.projectileHits==0 && body.arrowPasses==0 && body.recoveryCount==0 && body.maxAnchorErrorCm<=.5f;
    }
  event("cloth_session_initial","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"cloth_count\":"+std::to_string(count)+",\"rest\":"+(rest?"true":"false")+",\"cloth\":"+clothRows(world));
  if(!count || !rest)arenaAbort("cloth cold load did not start from transient rest");
  }

void clothShoot(World& world,Npc& player,std::string_view kind) {
  const auto all=world.physic()->cloth().snapshots();
  const auto found=std::find_if(all.begin(),all.end(),[](const auto& body){return body.zenId==bench.clothZen;});
  if(found==all.end() || !found->active) {arenaAbort("cloth projectile requires its active authored target");return;}
  float bestArea=0;Vec3 target,normal;
  for(size_t i=0;i+2<found->indices.size();i+=3) {
    const auto a=found->indices[i],b=found->indices[i+1],c=found->indices[i+2];
    if(a>=found->worldPositionsCm.size() || b>=found->worldPositionsCm.size() || c>=found->worldPositionsCm.size())continue;
    const auto p=found->worldPositionsCm[a],q=found->worldPositionsCm[b],r=found->worldPositionsCm[c];
    const auto cross=Vec3::crossProduct(q-p,r-p);
    const auto area=cross.length();
    const auto center=(p+q+r)/3.f;
    bool pinned=false;
    for(const auto& anchor:found->anchorsCm)if((center-anchor).length()<15.f)pinned=true;
    if(pinned || area<=bestArea)continue;
    const auto direction=Vec3::normalize(cross);
    for(float side:{1.f,-1.f}) {
      const auto from=center+direction*(250.f*side);
      const auto hit=world.physic()->ray(from,center-direction*(100.f*side));
      if(hit.hasCol)continue;
      bestArea=area;target=center;normal=direction*side;break;
      }
    }
  if(bestArea<1.f) {arenaAbort("cloth projectile has no exposed native cloth face");return;}
  const bool arrow=kind=="arrow";
  size_t symbol=size_t(-1);
  if(arrow)symbol=world.script().findSymbolIndex("ITRW_ARROW");
  else if(kind=="fireball" || kind=="wind") {
    for(size_t i=0;i<world.script().symbolsCount();++i) {
      const auto* entry=world.script().findSymbol(i);
      if(!entry || entry->type()!=daedalus::DaedalusDataType::INSTANCE)continue;
      auto name=entry->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_"))continue;
      Item candidate(world,i,Item::T_Inventory);
      if(world.script().physicalSpell(candidate.spellId())==
         (kind=="wind"?GameScript::PhysicalSpell::WindFist:GameScript::PhysicalSpell::Fireball)) {symbol=i;break;}
      }
    }
  if(symbol==size_t(-1)) {arenaAbort("cloth fixture native projectile instance is unavailable");return;}
  Item item(world,symbol,Item::T_Inventory);
  const auto from=target+normal*250.f;
  // shootArrow is the public free-direction entry to WorldObjects::shootBullet;
  // the Item still creates the native spell Bullet, spell VFX and collision path.
  auto& bullet=world.shootArrow(item,from,-normal,arrow?DynamicWorld::bulletSpeed:DynamicWorld::spellSpeed);
  bench.clothShotKind=std::string(kind);bench.clothShotAt=bench.time;
  bench.clothShotHitsBefore=arrow?found->arrowPasses:found->projectileHits;
  bench.clothArrow=arrow?&bullet:nullptr;bench.clothShotTarget=target;bench.clothShotNormal=normal;
  bench.clothArrowDownstream=0;
  bullet.setOrigin(&player);if(arrow)bullet.setPrecise(1.f);
  event("cloth_projectile","\"kind\":"+jsonStr(kind)+",\"zen_id\":"+std::to_string(bench.clothZen)+
    ",\"native_spell\":"+(bullet.isSpell()?"true":"false")+",\"spell_id\":"+std::to_string(bullet.spellId())+
    ",\"from_cm\":"+ragdollVector(from)+",\"face_target_cm\":"+ragdollVector(target)+
    ",\"direction\":"+ragdollVector(-normal));
  }

void clothArrowNpc(World& world,Npc& player,std::string_view order) {
  if(order!="behind" && order!="ahead") {arenaAbort("cloth arrow-npc expects behind or ahead");return;}
  const auto all=world.physic()->cloth().snapshots();
  const auto found=std::find_if(all.begin(),all.end(),[](const auto& body){return body.zenId==bench.clothZen;});
  if(found==all.end() || !found->active) {arenaAbort("cloth arrow-npc requires an active authored banner");return;}
  float bestArea=0;Vec3 target,normal;
  for(size_t i=0;i+2<found->indices.size();i+=3) {
    const auto p=found->worldPositionsCm[found->indices[i]],q=found->worldPositionsCm[found->indices[i+1]],r=found->worldPositionsCm[found->indices[i+2]];
    const auto cross=Vec3::crossProduct(q-p,r-p);
    if(cross.length()<=bestArea)continue;
    const auto center=(p+q+r)/3.f;
    for(float side:{1.f,-1.f}) {
      const auto direction=Vec3::normalize(cross)*side;
      if(std::abs(direction.y)>.25f || world.physic()->ray(center+direction*200.f,center-direction*150.f).hasCol)continue;
      target=center;normal=direction;bestArea=cross.length();break;
      }
    }
  if(bestArea<1.f) {arenaAbort("cloth arrow-npc has no unobstructed face and NPC segment");return;}
  if(bench.clothArrowNpc==nullptr) {
    const auto symbol=world.script().findSymbolIndex("BAU_904_BAUER");
    if(symbol==size_t(-1)) {arenaAbort("cloth arrow-npc fixture instance unavailable");return;}
    bench.clothArrowNpc=world.addNpc(symbol,target);
    if(!bench.clothArrowNpc) {arenaAbort("cloth arrow-npc fixture insertion failed");return;}
    bench.crew.push_back(bench.clothArrowNpc);
    }
  auto& npc=*bench.clothArrowNpc;
  npc.clearAiQueue();npc.clearState(false);npc.stopWalking();npc.clearSpeed();npc.setExternalControl(true);
  npc.handle().attribute[ATR_HITPOINTS]=npc.handle().attribute[ATR_HITPOINTSMAX]=50000;
  const auto from=target+normal*200.f,to=target-normal*150.f;
  Vec3 expectedImpact;bool prepared=false;
  const float npcSide=order=="behind"?-70.f:70.f;
  for(float height:{100.f,120.f,80.f,140.f,160.f}) {
    npc.setPosition(target+normal*npcSide-Vec3(0,height,0));npc.setDirection(normal);npc.updateTransform();
    float distance=0;
    if(npc.rayHitZone(from,to,distance)==HitZones::None)continue;
    expectedImpact=from+Vec3::normalize(to-from)*distance;
    const float side=Vec3::dotProduct(expectedImpact-target,normal);
    if((order=="behind" && side< -5.f) || (order=="ahead" && side>5.f)) {prepared=true;break;}
    }
  if(!prepared) {arenaAbort("cloth arrow-npc fixture bones do not lie strictly before or after its banner");return;}
  const auto arrowSymbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(arrowSymbol==size_t(-1)) {arenaAbort("cloth arrow-npc native arrow item unavailable");return;}
  Item item(world,arrowSymbol,Item::T_Inventory);
  constexpr uint64_t dt=100;
  // One synchronous native moveBullet call, with gravity compensated in its
  // initial velocity. The native Bullet callback still resolves bones, damage,
  // attachments and stop. Its registered body cannot tick during this call.
  Bullet arrow(world,item,from);
  arrow.setOrigin(&player);arrow.setPrecise(1.f);
  const auto direction=(to-from)/float(dt)+Vec3(0,.5f*DynamicWorld::gravity*float(dt),0);
  arrow.setDirection(direction);
  DynamicWorld::BulletBody step(world.physic(),&arrow);
  step.setPosition(from);step.setDirection(direction);
  const auto before=found->arrowPasses;
  ClothBulletBenchDriver::step(*world.physic(),step,direction,dt);
  const auto afterRows=world.physic()->cloth().snapshots();
  const auto after=std::find_if(afterRows.begin(),afterRows.end(),[](const auto& body){return body.zenId==bench.clothZen;});
  const auto passes=after==afterRows.end()?before:after->arrowPasses;
  const bool nativeHit=arrow.isFinished() && arrow.hitZone()!=HitZones::None && (arrow.impactPoint()-expectedImpact).length()<.1f;
  const bool pass=nativeHit && passes-before==(order=="behind"?1u:0u);
  event("cloth_arrow_npc_verify","\"order\":"+jsonStr(order)+",\"pass\":"+(pass?"true":"false")+
    ",\"move_bullet_calls\":1,\"dt_ms\":"+std::to_string(dt)+",\"native_precise_hit\":"+(nativeHit?"true":"false")+
    ",\"arrow_passes_before\":"+std::to_string(before)+",\"arrow_passes_after\":"+std::to_string(passes)+
    ",\"from_cm\":"+ragdollVector(from)+",\"step_end_cm\":"+ragdollVector(to)+",\"cloth_target_cm\":"+ragdollVector(target)+
    ",\"npc_impact_cm\":"+ragdollVector(arrow.impactPoint())+",\"npc_impact_side_cm\":"+
    ragdollNumber(Vec3::dotProduct(arrow.impactPoint()-target,normal)));
  if(!pass)arenaAbort("cloth single-step arrow/NPC ordering check failed");
  }

}

namespace {

float savedItemRotationDelta(const Matrix4x4& a,const Matrix4x4& b) {
  double trace=0;
  for(int column=0;column<3;++column) {
    double dot=0,lengthA=0,lengthB=0;
    for(int row=0;row<3;++row) {
      const double x=a.at(column,row),y=b.at(column,row);
      dot+=x*y;lengthA+=x*x;lengthB+=y*y;
    }
    trace+=dot/std::sqrt(lengthA*lengthB);
  }
  return float(std::acos(std::clamp((trace-1.)*.5,-1.,1.))*180./M_PI);
}

void savedItemSample(World& world) {
  if(!bench.savedItemTracing || bench.savedItemSample==world.tickCount())return;
  bench.savedItemSample=world.tickCount();
  for(auto& probe:bench.savedItemProbes) {
    const auto expected=Vec3(probe.saved.at(3,0),probe.saved.at(3,1),probe.saved.at(3,2));
    probe.maximumPositionDelta=std::max(probe.maximumPositionDelta,(probe.item->position()-expected).length());
    probe.maximumRotationDelta=std::max(probe.maximumRotationDelta,savedItemRotationDelta(probe.saved,probe.item->transform()));
    probe.alwaysSleeping&=probe.item->physicsSleeping();
  }
}

void savedItemInitial(World& world,Npc& player,uint64_t initialDt) {
  if(initialDt!=0)throw std::runtime_error("item save observation must precede the first native step");
  const auto* path=envStr("SAVE");
  if(path==nullptr)throw std::runtime_error("item save observation requires the loaded SAVE archive");
  std::ifstream file(path,std::ios::binary);
  if(!file)throw std::runtime_error("cannot open loaded item save archive");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
  MemReader reader(bytes);Serialize in(reader);in.setContext(&world);
  if(!in.setEntry("worlds/",world.name(),"/item-physics"))
    throw std::runtime_error("loaded archive has no native item physics entry");
  uint32_t magic=0,total=0,count=0;
  uint16_t schema=0;
  std::string worldName;
  in.read(magic,schema,worldName,total,count);
  if(magic!=0x31505449 || schema!=1 || worldName!=world.name() || count>total || count>100000)
    throw std::runtime_error("loaded item physics entry has unexpected identity");
  const auto symbol=world.script().findSymbolIndex("ITFO_BEER");
  bench.savedItemProbes.clear();
  std::string items="[";
  for(uint32_t i=0;i<count;++i) {
    uint32_t index=0,instance=0;
    std::string visual;
    Matrix4x4 matrix;
    Vec3 velocity,angular;
    bool sleeping=false;
    in.read(index,instance,visual,matrix,velocity,angular,sleeping);
    const Vec3 point(matrix.at(3,0),matrix.at(3,1),matrix.at(3,2));
    if(instance!=symbol || (point-player.position()).length()>400.f)continue;
    if(!sleeping)throw std::runtime_error("item save fixture contains an awake dropped Item");
    Item* found=nullptr;
    world.detectItem(point,1.f,[&](Item& item) {
      if(item.clsId()!=instance || item.handle().visual!=visual)return;
      if(found!=nullptr)throw std::runtime_error("item save fixture has ambiguous loaded Item positions");
      found=&item;
    });
    if(found==nullptr || !found->isDynamic() || !found->physicsSleeping())
      throw std::runtime_error("saved sleeping dropped Item was not restored with its native body");
    if(std::any_of(bench.savedItemProbes.begin(),bench.savedItemProbes.end(),[&](const auto& probe){return probe.item==found;}))
      throw std::runtime_error("saved item records resolved to the same loaded Item");
    Bench::SavedItemProbe probe;probe.item=found;probe.saved=matrix;
    bench.savedItemProbes.push_back(probe);
    if(items.size()>1)items+=',';
    items+="{\"index\":"+std::to_string(index)+",\"symbol\":"+std::to_string(instance)+
      ",\"visual\":"+jsonStr(visual)+",\"saved_transform\":"+ragdollMatrix(matrix)+
      ",\"loaded_transform\":"+ragdollMatrix(found->transform())+
      ",\"saved_sleeping\":true,\"loaded_sleeping\":true}";
  }
  if(bench.savedItemProbes.size()!=10)
    throw std::runtime_error("item save fixture requires exactly ten saved sleeping dropped beer Items");
  bench.savedItemStart=world.tickCount();bench.savedItemSample=uint64_t(-1);bench.savedItemTracing=true;
  savedItemSample(world);
  float position=0,rotation=0;
  for(const auto& probe:bench.savedItemProbes) {
    position=std::max(position,probe.maximumPositionDelta);rotation=std::max(rotation,probe.maximumRotationDelta);
  }
  event("item_save_initial","\"items\":"+items+"],\"count\":10,\"saved_sleepers\":10,\"loaded_sleepers\":10"+
    std::string(",\"initial_dt\":0,\"maximum_position_delta_cm\":")+ragdollNumber(position)+
    ",\"maximum_rotation_delta_degrees\":"+ragdollNumber(rotation)+
    ",\"pass\":"+(position<=.1f && rotation<=.1f?"true":"false"));
}

void savedItemReport(World& world) {
  if(!bench.savedItemTracing)throw std::runtime_error("item save observation was not initialized");
  savedItemSample(world);bench.savedItemTracing=false;
  const auto elapsed=world.tickCount()-bench.savedItemStart;
  float position=0,rotation=0;size_t sleeping=0;
  for(const auto& probe:bench.savedItemProbes) {
    position=std::max(position,probe.maximumPositionDelta);rotation=std::max(rotation,probe.maximumRotationDelta);
    sleeping+=probe.alwaysSleeping;
  }
  event("item_save_stability","\"count\":"+std::to_string(bench.savedItemProbes.size())+
    ",\"elapsed_ms\":"+std::to_string(elapsed)+",\"always_sleeping\":"+std::to_string(sleeping)+
    ",\"maximum_position_delta_cm\":"+ragdollNumber(position)+
    ",\"maximum_rotation_delta_degrees\":"+ragdollNumber(rotation)+
    ",\"pass\":"+(elapsed>=3000 && position<=.1f && rotation<=.1f && sleeping==10?"true":"false"));
}

// Film fixtures are deliberately separate from the water regression fixtures:
// each shot owns its actors/props and accepts the stock, non-ragdoll outcome.
std::string waterFilmPosition(World& world,const Vec3& point) {
  const auto floor=world.physic()->landRay(point+Vec3(0,300,0),6000.f);
  const auto water=floor.hasCol?world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0):DynamicWorld::RayWaterResult{};
  return "\"position_cm\":"+ragdollVector(point)+
    ",\"water_y_cm\":"+(water.hasCol?ragdollNumber(water.wdepth):"null")+
    ",\"bottom_y_cm\":"+(floor.hasCol?ragdollNumber(floor.v.y):"null");
  }

void waterFilmCheckpoint(World& world) {
  std::string actors="[",items="[",fragments="[";
  for(const auto& actor:bench.water.actors) {
    if(actors.size()>1)actors+=',';
    actors+="{\"role\":"+jsonStr(actor.role)+",\"dead\":"+(actor.npc->isDead()?"true":"false")+
      ",\"swimming\":"+(actor.npc->isSwim()?"true":"false")+
      ",\"last_hit_spell\":"+std::to_string(actor.npc->lastHitSpellId())+
      ",\"ragdoll\":"+(actor.npc->ragdollBody()?"true":"false")+","+waterFilmPosition(world,actor.npc->position())+"}";
    }
  for(size_t i=0;i<bench.water.items.size();++i) {
    if(i)items+=',';
    items+="{\"label\":"+jsonStr(std::array{"sword","ore","lute","arrow"}[i])+","+
      waterFilmPosition(world,bench.water.items[i]->midPosition())+"}";
    }
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(body.category!=DynamicWorld::C_Fragment || !bench.water.ids.contains(body.parentId))continue;
    if(fragments.size()>1)fragments+=',';
    fragments+="{\"parent\":"+jsonStr(bench.water.labels[body.parentId])+",\"piece\":"+std::to_string(body.piece)+
      ",\"submerged_fraction\":"+ragdollNumber(body.submergedFraction)+","+
      waterFilmPosition(world,(body.worldBounds[0]+body.worldBounds[1])*.5f)+"}";
    }
  event("water_film_checkpoint","\"scene\":"+jsonStr(bench.waterFilm)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-bench.waterFilmStart)+",\"actors\":"+actors+"],\"items\":"+items+
    "],\"props\":"+destructionRows(bench.destruction,world)+",\"fragments\":"+fragments+"],\"bodies\":"+waterRows(bench.water,world));
  }

void waterFilmTick(World& world) {
  if(!bench.waterFilmKill || bench.water.actors.empty())return;
  auto& npc=*bench.water.actors.front().npc;
  const auto velocity=npc.spellVelocity();
  const auto displacement=(npc.position()-bench.waterFilmLip).length();
  if(npc.lastHitSpellId()!=26 || npc.lastHitBy()!=world.player() ||
     velocity.length()<=100.f || displacement<=50.f)return;
  bench.waterFilmKill=false;
  const auto at=npc.position();
  if(!npc.setRagdollDown(true)) {arenaAbort("water film real wind death refused");return;}
  npc.prepareRagdoll();
  event("water_film_death","\"scene\":"+jsonStr(bench.waterFilm)+","+waterFilmPosition(world,at)+
    ",\"observed_wind_velocity_cm_s\":"+ragdollVector(velocity)+
    ",\"displacement_cm\":"+ragdollNumber(displacement)+
    ",\"ragdoll\":"+(npc.ragdollBody()?"true":"false")+",\"path\":\"onNoHealth after real WindFist hit\"");
  }

// Measure the rendered pose/model in the actual capture viewport. A stock
// corpse may leave the close shot; that is evidence, not a fixture failure.
struct WaterFilmPixels {
  Matrix4x4 vp;
  float width,height;
  std::vector<Vec2> points;
  bool inFrame=true;

  WaterFilmPixels(const Matrix4x4& viewProjection,uint32_t w,uint32_t h)
    :vp(viewProjection),width(float(w)),height(float(h)) {}

  void point(const Vec3& p) {
    float x=p.x,y=p.y,z=p.z,w=1;vp.project(x,y,z,w);
    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w) || w<=0) {
      inFrame=false;return;
      }
    inFrame&=std::abs(x)<=w && std::abs(y)<=w && z>=0 && z<=w;
    // The Vulkan projection already points Y down, like the captured pixmap.
    points.emplace_back((x/w+1.f)*width*.5f,(y/w+1.f)*height*.5f);
    }

  void box(const Vec3* bounds,const Matrix4x4& object) {
    if(!bounds)return;
    for(unsigned i=0;i<8;++i) {
      Vec3 p(bounds[i&1].x,bounds[(i>>1)&1].y,bounds[(i>>2)&1].z);
      object.project(p);point(p);
      }
    }

  std::string json() const {
    if(points.empty())return "null";
    auto lo=points.front(),hi=lo;float span=0;
    for(const auto& p:points) {
      lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);
      hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);
      for(const auto& q:points)span=std::max(span,(p-q).length());
      }
    return "{\"width_px\":"+ragdollNumber(hi.x-lo.x)+",\"height_px\":"+ragdollNumber(hi.y-lo.y)+
      ",\"span_px\":"+ragdollNumber(span)+",\"rect_px\":["+ragdollNumber(lo.x)+","+ragdollNumber(lo.y)+","+
      ragdollNumber(hi.x)+","+ragdollNumber(hi.y)+"],\"in_frame\":"+(inFrame?"true":"false")+"}";
    }
  };

void waterFilmFraming(World& world,uint32_t width,uint32_t height) {
  if(!bench.rec || bench.waterFilm.empty())return;
  const auto* camera=Gothic::inst().camera();
  if(!camera)return;
  std::string objects="[";
  const auto row=[&](std::string_view label,std::string_view method,const WaterFilmPixels& pixels) {
    if(objects.size()>1)objects+=',';
    objects+="{\"label\":"+jsonStr(label)+",\"method\":"+jsonStr(method)+",\"pixels\":"+pixels.json()+"}";
    };
  if(bench.waterFilm=="body" || bench.waterFilm=="skeleton")
    for(const auto& actor:bench.water.actors) {
      WaterFilmPixels pixels(camera->viewProj(),width,height);
      const auto& npc=*actor.npc;
      if(const auto* skeleton=npc.visualSkeleton())
        for(size_t i=0;i<skeleton->nodes.size();++i) {
          if(!skeleton->nodes[i].name.starts_with("BIP01"))continue;
          Vec3 point{};npc.renderedBone(i).project(point);pixels.point(point);
          }
      row(actor.role,"rendered_bones",pixels);
      }
  for(size_t i=0;i<bench.water.items.size();++i) {
    WaterFilmPixels pixels(camera->viewProj(),width,height);
    pixels.box(bench.water.items[i]->bBox(),bench.water.items[i]->transform());
    row(std::array{"sword","ore","lute","arrow"}[i],"native_mesh_bbox",pixels);
    }
  if(bench.waterFilm=="fireball")
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(!bench.water.ids.contains(body.id) || body.broken)continue;
      WaterFilmPixels pixels(camera->viewProj(),width,height);
      if(const auto* mesh=Resources::loadMesh(body.visual))pixels.box(mesh->bbox(),body.transform);
      row(bench.water.labels[body.id],"native_mesh_bbox",pixels);
      }
  event("water_film_framing","\"scene\":"+jsonStr(bench.waterFilm)+",\"elapsed_ms\":"+
    std::to_string(bench.time-bench.recStart)+",\"viewport_px\":["+std::to_string(width)+","+
    std::to_string(height)+"],\"objects\":"+objects+"]");
  }

void waterFilmSetup(World& world,Npc& player,std::string_view scene) {
  if(scene!="body" && scene!="skeleton" && scene!="fireball" && scene!="things") {
    arenaAbort("water-film setup expects body, skeleton, fireball or things");return;
    }
  if(bench.movables.spellRunes.empty())prepareSpellFixture(world,player,"wind");
  if(bench.movables.spellRunes.empty())return;
  player.setTarget(nullptr);player.closeWeapon(true);player.clearAiQueue();player.clearGoTo();player.stopWalking();player.clearSpeed();
  bench.subject=nullptr;bench.crew.clear();bench.water.actors.clear();bench.waterFilmKill=false;
  bench.movables.spellTrace=false;bench.movables.spellGuardTrace=false;bench.movables.spellReleaseAt=0;bench.movables.spellEquipPending=0;
  for(const auto& actor:bench.movables.spellActors)world.removeNpc(*actor.npc);
  for(auto* item:bench.movables.spellItems)world.removeItem(*item);
  for(auto* item:bench.water.items)world.removeItem(*item);
  bench.movables.spellActors.clear();bench.movables.spellItems.clear();bench.water.items.clear();
  bench.destruction.destructionFixtures.clear();bench.destruction.destructionIds.clear();bench.movables.movableIds.clear();
  bench.water.ids.clear();bench.water.labels.clear();bench.water.reference.clear();bench.water.travel.clear();bench.water.sleepAt.clear();
  bench.destruction.destructionTrace=false;
  std::vector<Vec3> places;
  if(!waterFindWindSite(bench.water,world,places) || bench.water.waypoint!="NW_CITY_HABOUR_05") {
    arenaAbort("water film needs the low NW_CITY_HABOUR_05 pier");return;
    }
  const auto dryFloor=[&](const Vec3& at,Vec3& result) {
    const auto floor=world.physic()->landRay(at+Vec3(0,2000,0),4000.f);
    if(!floor.hasCol || floor.n.y<.99f || world.physic()->waterRay(floor.v,1.f).hasCol)return false;
    for(float dx:{-30.f,30.f})for(float dz:{-30.f,30.f}) {
      const auto foot=world.physic()->landRay(floor.v+Vec3(dx,100,dz),150.f);
      if(!foot.hasCol || foot.n.y<.99f || std::abs(foot.v.y-floor.v.y)>5.f ||
         world.physic()->waterRay(foot.v,1.f).hasCol)return false;
      }
    result=floor.v;return true;
    };
  auto lip=bench.water.dry;
  const auto direction=bench.water.direction;
  // The regression lip is the last perfectly flat point. Follow the remaining
  // dry slope to the actual waterline before measuring the 1-2 m drop distance.
  const auto flatLip=lip;
  for(float distance=5.f;distance<=200.f;distance+=5.f) {
    const auto floor=world.physic()->landRay(flatLip+direction*distance+Vec3(0,400,0),600.f);
    if(!floor.hasCol || floor.n.y<.75f || world.physic()->waterRay(floor.v,1.f).hasCol)break;
    lip=floor.v;
    }
  bool deep=false;
  for(float distance=100.f;distance<=200.f;distance+=5.f) {
    Vec3 surface;
    if(!waterProbe(world,lip+direction*distance,220.f,6000.f,surface))continue;
    bool slots=true;
    if(scene=="things")for(unsigned i=0;i<4;++i) {
      Vec3 drop;
      if(!waterProbe(world,surface+Vec3(-direction.z,0,direction.x)*(float(i)-1.5f)*65.f,220.f,6000.f,drop))slots=false;
      }
    if(slots) {bench.water.center=surface;deep=true;break;}
    }
  if(!deep) {arenaAbort("water film needs deep water 1-2m from the actual waterline");return;}
  bench.water.dry=lip;bench.water.direction=direction;
  const auto side=Vec3(-direction.z,0,direction.x);
  bench.waterFilmLip=lip;bench.waterFilm=scene;bench.water.caseName="film-"+std::string(scene);
  std::array<std::string,2> visuals={"NW_HARBOUR_CRATE_01.3DS",""};
  std::array<Vec3,2> supports;
  if(scene=="fireball") {
    // Some stock harbour barrels are authored as stone. Copy a real wooden,
    // cached barrel visual into a NEW owned vob; never move a map object.
    float footprint=std::numeric_limits<float>::infinity();
    const auto native=world.physic()->waterBodySnapshot();
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(!body.enabled || !body.destructible || body.broken || body.className!="barrel")continue;
      const bool wood=std::any_of(native.begin(),native.end(),[&](const auto& sample) {
        return sample.parentId==body.id && sample.targetFraction<1.f;
        });
      const auto size=body.localBounds[1]-body.localBounds[0];
      const float area=size.x*size.z;
      if(wood && (area<footprint || (area==footprint && body.visual<visuals[1]))) {
        visuals[1]=body.visual;footprint=area;
        }
      }
    if(visuals[1].empty()) {arenaAbort("water film needs a stock wooden destructible barrel visual");return;}
    for(unsigned i=0;i<2;++i) {
      Vec3 failed;bool found=false;
      for(float retreat:{140.f,165.f,190.f}) {
        const auto floor=world.physic()->landRay(lip-direction*retreat+side*(i?85.f:-85.f)+Vec3(0,400,0),600.f);
        if(floor.hasCol && !world.physic()->waterRay(floor.v,1.f).hasCol &&
           destructionSupported(world,visuals[i],floor.v,supports[i],failed)) {found=true;break;}
        }
      if(!found) {arenaAbort("water film crate/barrel needs dry full-footprint support");return;}
      }
    }
  Vec3 caster,spellTarget=lip;bool supported=false;
  if(scene=="fireball" && !dryFloor(lip-direction*220.f,spellTarget)) {
    arenaAbort("water film fireball target needs dry native support");return;
    }
  std::vector<float> distances={200,250,300,150};
  if(scene=="fireball") {
    distances.clear();for(float distance=600.f;distance<=900.f;distance+=10.f)distances.push_back(distance);
    }
  for(float distance:distances) {
    for(unsigned angle=0;angle<(scene=="fireball"?181u:1u);++angle) {
      const int step=angle==0?0:int((angle+1)/2)*(angle%2?1:-1);
      const float turn=float(step);
      if(!dryFloor(lip-rotY(direction,turn)*distance,caster) ||
         world.physic()->ray(caster+Vec3(0,140,0),spellTarget+Vec3(0,100,0)).hasCol)continue;
      if(scene=="fireball") {
        bool range=true;
        for(unsigned i=0;i<2;++i) {
          const auto* mesh=Resources::loadMesh(visuals[i]);
          const auto center=supports[i]+Vec3(0,(mesh->bbox()[1].y-mesh->bbox()[0].y)*.5f,0);
          for(const auto& at:{supports[i],center})
            if((caster-at).length()<600.f || (caster-at).length()>800.f)range=false;
          }
        if(!range)continue;
        }
      supported=true;break;
      }
    if(supported)break;
    }
  if(!supported) {
    Vec3 standard;
    const bool found=scene=="fireball" && waterFireballCaster(world,spellTarget+Vec3(0,100,0),0.f,standard);
    event("water_film_caster_search","\"scene\":"+jsonStr(scene)+",\"standard_found\":"+(found?"true":"false")+
      ",\"standard_caster_cm\":"+(found?ragdollVector(standard):"null")+
      ",\"standard_distance_cm\":"+(found?ragdollNumber((standard-lip).length()):"null"));
    arenaAbort("water film caster needs a clear supported dry lane");return;
    }
  player.setPosition(caster);player.setDirection(direction);player.updateTransform();
  player.handle().attribute[ATR_MANA]=player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && (npc->position()-lip).length()<2500.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  if(scene!="things") {
    if(!waterBodiesAdd(bench.water,waterContext(),world,player,scene=="skeleton"?"SKELETON":"BAU_904_BAUER",std::string(scene),lip,false))return;
    auto* target=bench.water.actors.back().npc;
    if(scene=="fireball") {target->setPosition(spellTarget);target->updateTransform();}
    target->handle().attribute[ATR_HITPOINTS]=target->handle().attribute[ATR_HITPOINTSMAX]=50000;
    bench.movables.spellActors.push_back({target,"water_target"});
    }
  if(scene=="fireball") {
    // Fresh owned vobs keep this dry authored pose when movable is disabled.
    // The real projectile hits the actor in the gap; only its ordinary blast
    // can break the two props or send their fragments over the edge.
    for(unsigned i=0;i<2;++i) {
      const auto id=destructionAdd(world,visuals[i],supports[i]);
      if(!id) {arenaAbort("water film fresh crate/barrel insertion failed");return;}
      bench.water.ids.insert(id);bench.water.labels[id]=i?"barrel":"crate";
      }
    }
  bench.water.ready=true;bench.water.trace=true;bench.water.start=world.tickCount();bench.water.sample=uint64_t(-1);
  bench.movables.noiseTrace=true;bench.movables.noisePhase="water-film-"+std::string(scene);
  const bool bodies=scene=="body" || scene=="skeleton";
  // Broadside close shots keep the surface legible. Do not widen the body
  // shot to chase the stock corpse ten metres away from its buoyant partner.
  Vec3 focus,eye;
  bool camera=false;
  if(bodies) {
    focus=lip+direction*450.f;focus.y=bench.water.center.y;
    for(float lateral:{420.f,-420.f}) {
      eye=focus+side*lateral+Vec3(0,160,0);
      if(world.physic()->ray(eye,bench.water.center+Vec3(0,20,0)).hasCol ||
         world.physic()->ray(eye,lip+Vec3(0,110,0)).hasCol)continue;
      camera=true;break;
      }
    }
  else if(scene=="fireball") {
    const float pierY=(supports[0].y+supports[1].y)*.5f;
    // Behind the target and above its head: it stays low in the frame and the
    // crate, the barrel and the water where their fragments land stay in view.
    focus=lip+direction*150.f;focus.y=bench.water.center.y;
    for(float lateral:{0.f,-40.f,40.f}) {
      eye=lip-direction*480.f+side*lateral;eye.y=pierY+330.f;
      bool clear=!world.physic()->ray(eye,bench.water.center+Vec3(0,20,0)).hasCol;
      for(unsigned i=0;i<2;++i) {
        const auto* mesh=Resources::loadMesh(visuals[i]);
        const auto center=supports[i]+Vec3(0,(mesh->bbox()[1].y-mesh->bbox()[0].y)*.5f,0);
        const auto hit=world.physic()->ray(eye,center);
        if(hit.hasCol && (hit.v-center).length()>85.f)clear=false;
        }
      if(clear) {camera=true;break;}
      }
    }
  else {
    focus=bench.water.center+Vec3(0,3,0);
    eye=bench.water.center-direction*210.f+Vec3(0,100,0);
    camera=true;
    for(unsigned i=0;i<4;++i)
      if(world.physic()->ray(eye,bench.water.center+side*(float(i)-1.5f)*65.f+Vec3(0,5,0)).hasCol)camera=false;
    }
  if(!camera) {arenaAbort("water film camera has no clear close side view");return;}
  bench.cam=C_Water;bench.water.cameraTarget=focus;lookFrom(eye,focus);
  event("water_film_setup","\"scene\":"+jsonStr(scene)+",\"waypoint\":"+jsonStr(bench.water.waypoint)+
    ",\"lip_cm\":"+ragdollVector(lip)+",\"water_cm\":"+ragdollVector(bench.water.center)+
    ",\"caster_cm\":"+ragdollVector(caster)+",\"camera_cm\":"+ragdollVector(bench.camPos)+
    ",\"camera_target_cm\":"+ragdollVector(focus));
  }

void waterFilmBegin(World& world,Npc& player) {
  bench.waterFilmStart=world.tickCount();
  bench.waterFilmKill=bench.waterFilm=="body" || bench.waterFilm=="skeleton";
  if(bench.waterFilm=="things") {
    const auto side=Vec3(-bench.water.direction.z,0,bench.water.direction.x);
    const char* symbols[]{"ITMW_1H_SLD_SWORD","ITMI_NUGGET","ITMI_LUTE","ITRW_ARROW"};
    for(unsigned i=0;i<4;++i) {
      auto symbol=world.script().findSymbolIndex(symbols[i]);
      if(symbol==size_t(-1) && i==0)
        for(size_t candidate=0;candidate<world.script().symbolsCount();++candidate) {
          const auto* entry=world.script().findSymbol(candidate);
          if(entry && entry->type()==daedalus::DaedalusDataType::INSTANCE && entry->name().starts_with("ITMW_") &&
             entry->name().find("SWORD")!=std::string::npos) {symbol=candidate;break;}
          }
      if(symbol==size_t(-1)) {arenaAbort("water film item instance missing");return;}
      Vec3 surface;
      if(!waterProbe(world,bench.water.center+side*(float(i)-1.5f)*65.f,220.f,6000.f,surface)) {
        arenaAbort("water film item drop needs four adjacent deep-water slots");return;
        }
      const auto before=world.physic()->waterBodySnapshot();
      auto matrix=Matrix4x4::mkIdentity();matrix.translate(surface+Vec3(0,100,0));
      auto* item=world.addItemDyn(symbol,matrix,world.npcId(&player));
      if(!item) {arenaAbort("water film native item drop failed");return;}
      bench.water.items.push_back(item);
      for(const auto& body:world.physic()->waterBodySnapshot())
        if(body.category==DynamicWorld::C_Item &&
           std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {
          bench.water.ids.insert(body.id);bench.water.labels[body.id]=std::array{"sword","ore","lute","arrow"}[i];
          }
      }
    }
  event("water_film_begin","\"scene\":"+jsonStr(bench.waterFilm));
  }


void writeRow(Npc& pl) {
  if(bench.csv==nullptr) {
    bench.csv = std::fopen((outDir+"/metrics.csv").c_str(),"w");
    if(bench.csv==nullptr)
      return;
    writeHeader();
    }
  auto&      p   = FootIk::playerProbe;
  const Vec3 pos = pl.position();
  std::fprintf(bench.csv,"%s,%llu,%llu,%.2f,%.2f,%.2f,%.1f,%.1f,%u,%u,%u,%d,%d,%d,%.3f,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f",
               bench.recName.c_str(),(unsigned long long)bench.recFrame,(unsigned long long)(bench.time-bench.recStart),
               pos.x,pos.y,pos.z,pl.rotation(),bench.speed,unsigned(pl.bodyStateMasked()),unsigned(pl.walkMode()),unsigned(bench.occl),
               int(p.valid),int(p.enable),int(p.hardOff),p.weight,unsigned(p.numLegs),unsigned(p.badBones),p.pelvis,p.chest,p.target,p.limit,p.floorY);
  for(auto& l:p.leg) {
    std::fprintf(bench.csv,",%.3f,%d,%.2f,%.3f,%.3f,%.3f,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f",
                 l.plant,int(l.hasCol),l.groundY,l.n.x,l.n.y,l.n.z,int(l.toeCol),l.toeY,l.smoothY,l.err,l.kneeDev,l.tilt,l.roll,l.yaw,l.outward,l.slack);
    const Vec3* pts[] = {&l.final[0],&l.anim[1],&l.anim[2],&l.anim[3],&l.final[1],&l.final[2],&l.final[3]};
    for(auto v:pts)
      std::fprintf(bench.csv,",%.2f,%.2f,%.2f",v->x,v->y,v->z);
    }
  // gaze of the watched npc (or the hero) and of the hero
  const auto& g  = bench.subject!=nullptr ? Gaze::subjectProbe : Gaze::playerProbe;
  const auto& pg = Gaze::playerProbe;
  // where the face looks in the world: a jerk is a jump of this, whatever the body does
  const float fyaw   = std::atan2(g.facing.z,g.facing.x)*180.f/float(M_PI);
  const float fpitch = std::asin(std::clamp(g.facing.y,-1.f,1.f))*180.f/float(M_PI);
  std::fprintf(bench.csv,",%d,%d,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%.3f,%.2f,%.2f",
               int(g.active),int(g.source),g.weight,g.error,g.yaw,g.pitch,g.yawGoal,g.pitchGoal,g.headYaw,g.chestYaw,g.waistYaw,
               g.animYaw,g.animPitch,g.hipsYaw,fyaw,fpitch,int(pg.active),int(pg.source),pg.weight,pg.error,pg.yaw);
  // lean of the watched npc (or the hero)
  const auto& ln = bench.subject!=nullptr ? Lean::subjectProbe : Lean::playerProbe;
  std::fprintf(bench.csv,",%d,%.2f,%.2f,%.1f,%.1f,%.1f",int(ln.active),ln.roll,ln.pitch,ln.speed,ln.turn,ln.slope);
  writeAnim();
  // aim of the watched npc (or the hero)
  const auto& am = bench.subject!=nullptr ? Aim::subjectProbe : Aim::playerProbe;
  std::fprintf(bench.csv,",%d,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",int(am.active),am.weight,am.yaw,am.pitch,
               am.animYaw,am.animPitch,am.turn,am.bend,am.error);
  std::fprintf(bench.csv,",%d,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f",int(am.steady),am.lean,am.side,FootIk::playerProbe.crouch,
               double(pl.aimCrouch()),double(pl.aimPeek()),double(pl.aimCant()));
  // steps with the bow drawn
  const auto& aw = AimWalk::playerProbe;
  std::fprintf(bench.csv,",%.2f,%.2f,%.1f,%.1f,%.1f,%.1f",double(pl.aimWalk()),aw.weight,aw.speed,aw.twist,aw.chest,aw.drop);
  // the manual aim: the bow's stage, the sway, how far the arrow points off the aim
  const auto& mp = Archer::playerProbe;
  // the crosshair: how far it is open, degrees; how far it moved on the screen since the last frame,
  // percent of the screen's height
  Vec3  cross;
  float spread = 0;
  auto  cam    = Gothic::inst().camera();
  bool  hasCross = cam!=nullptr && bench.ctrl!=nullptr && bench.ctrl->crosshair(cross,spread);
  float moved  = -1;
  if(hasCross) {
    // as the game draws it: the center of the screen, unless the bench aims elsewhere
    float x = cross.x, y = cross.y, z = cross.z, w = 1;
    cam->viewProj().project(x,y,z,w);
    if(bench.ctrl->isCrosshairCentered()) {
      x = 0;
      y = 0;
      w = 1;
      }
    hasCross = w>1e-3f;
    const Vec3 at = hasCross ? Vec3(x/w,y/w,0) : Vec3();
    if(hasCross && bench.crossValid)
      moved = std::hypot((at.x-bench.crossPrev.x)*bench.aspect,at.y-bench.crossPrev.y)*50.f;
    bench.crossPrev = at;
    }
  bench.crossValid = hasCross;
  std::fprintf(bench.csv,",%d,%.3f,%.2f,%.3f,%.2f,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",mp.valid ? int(mp.stage) : -1,mp.draw,mp.hold,
               mp.fatigue,mp.breath,int(mp.holding),int(mp.short_),mp.disturb,mp.amp,mp.yaw,mp.pitch,manualAimOff(pl),
               hasCross ? spread*180.f/float(M_PI) : -1.f,moved);
  // the hand drawing the string
  const auto& bd = BowDraw::playerProbe;
  std::fprintf(bench.csv,",%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",bd.valid ? bd.weight : -1.f,bd.draw,bd.span,bd.travel,bd.miss,
               bd.follow,bd.fit,bd.offset,bd.angle);
  std::fprintf(bench.csv,",%.1f,%.1f,%.1f,%d,%.2f",bd.head,bd.fore,bd.arrow,int(bd.holding),bd.nock);
  std::fprintf(bench.csv,",%.2f,%.2f,%.2f",bd.handAt.x,bd.handAt.y,bd.handAt.z);
  std::fprintf(bench.csv,",%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",bd.aligned,bd.overGrip,bd.atMiddle,bd.nockGap,bd.square,bd.cant,bd.bowMove,bd.bowMiss);
  // the release: the drawing hand off where it let go, the bow hand thrown back, the bow turned in it
  std::fprintf(bench.csv,",%.2f,%.2f,%.2f",bd.back,bd.kick,bd.turn);
  // lips of the watched one
  const auto& lp = LipSync::subjectProbe;
  std::fprintf(bench.csv,",%d,%d,%llu,%.3f,%.3f,%.3f,%u,%u,%.2f,%.3f,%u",
               int(lp.valid),int(lp.active),(unsigned long long)lp.time,lp.level,lp.mouth,lp.open,
               lp.shape.frame0,lp.shape.frame1,lp.shape.alpha,lp.shape.intensity,lp.picks);
  // its hands while it speaks
  const auto& ge = TalkGestures::subjectProbe;
  std::fprintf(bench.csv,",%d,%d,%u,%u,%u,%u,%u,%u,%.2f,%.2f,%.2f",int(ge.engaged),int(ge.speaking),unsigned(ge.gesture),
               unsigned(ge.mood),ge.starts,ge.cuts,ge.stops,ge.nods,double(ge.head.pitch*180.f/float(M_PI)),
               double(ge.head.yaw*180.f/float(M_PI)),double(ge.head.roll*180.f/float(M_PI)));
  std::fprintf(bench.csv,",%d,%d,%u,%.3f",int(ge.listening),int(ge.thinking),ge.listenNods,ge.aside.weight);
  // its blinks: how closed the eyes are now, counters
  const auto& bk = EyeBlink::subjectProbe;
  std::fprintf(bench.csv,",%d,%.3f,%u,%u,%u,%u",int(bk.valid && bk.enabled),subjectOf(pl).faceWeight("R_EYESBLINK"),
               bk.blinks,bk.pauses,bk.shifts,bk.doubles);
  // faces of the watched one, as shown (fading in and out)
  Npc& s = subjectOf(pl);
  for(auto& f:FaceColumns)
    std::fprintf(bench.csv,",%.3f",s.faceWeight(f.anim));
  // the own head's face (FaceRig): active anims, targets with a weight, the last blend and upload, blends so far
  const auto& fr = FaceRig::subjectProbe;
  std::fprintf(bench.csv,",%d,%u,%u,%.1f,%u,%.1f",int(fr.valid),fr.anims,fr.targets,double(fr.blendUs),fr.blends,
               double(fr.uploadUs));
  // flinch of the watched one (or the hero)
  const auto& fl = bench.subject!=nullptr ? Flinch::subjectProbe : Flinch::playerProbe;
  std::fprintf(bench.csv,",%d,%.2f,%.1f,%.2f,%.2f,%u,%.2f,%.2f",int(fl.active),fl.bend,fl.bendYaw,fl.twist,fl.head,
               fl.hits,fl.strength,fl.height);
  // hit points: the hero's, the watched one's (a fight's course)
  std::fprintf(bench.csv,",%d,%d",pl.attribute(ATR_HITPOINTS),bench.subject!=nullptr ? bench.subject->attribute(ATR_HITPOINTS) : -1);
  std::fprintf(bench.csv,",%.3f,%.3f",double(pl.stamina()),bench.subject!=nullptr ? double(bench.subject->stamina()) : -1.0);
  // the watched npc's archer by hand (game/archerai)
  const auto& ai = ArcherAi::subjectProbe;
  std::fprintf(bench.csv,",%d,%d,%.3f,%.3f,%.3f,%.0f,%.2f,%.2f",int(ai.valid && ai.active),int(ai.want),ai.sigma,ai.err.x,ai.err.y,
               ai.lead,ai.pv,ai.ps);
  {
    // the watched npc's motor reaction to a blow: its weight, how far the shown body leaves the animation (cm)
    const Npc&     s      = subjectOf(pl);
    const Ragdoll* living = s.livingBody();
    float          weight = 0, dev = 0;
    if(living!=nullptr && living->reactionWeight()>0.f) {
      weight = living->reactionWeight();
      for(const auto b:living->reactionBones()) {
        Vec3 shown, anim;
        s.renderedBone(b).project(shown);
        s.animationBone(b).project(anim);
        dev = std::max(dev,(shown-anim).length());
        }
      }
    std::fprintf(bench.csv,",%.3f,%.2f,%u,%.6f",weight,dev,unsigned(s.meleeReactionProbe().applied),
                 double(s.world().physic()->physicsStats().stepMilliseconds));
  }
  {
    const auto q = subjectOf(pl).quiverProbe();
    std::fprintf(bench.csv,",%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f",int(q.on),q.arrows,q.armor,q.leg,
                 q.arm,q.mouth.x,q.mouth.y,q.mouth.z,q.out,q.top,q.bottom,q.swing,q.back,q.outward,
                 double(subjectOf(pl).quiverCostUs()));
    std::fprintf(bench.csv,",%.2f,%.2f,%.2f,%.2f,%.2f,%.1f",q.twist,q.bow,q.longsword,q.blade,q.yield,q.legAt);
    std::fprintf(bench.csv,",%.2f",q.legLeft);
    // cm the tube and its mouth go under the world (its collision, a ray down over each point of the outer side)
    float gnd = 0, gndMouth = 0, gndTube = 0;
    Quiver::measureGround = true;
    if(q.on) {
      constexpr int Rings = 9, Around = 12;
      const auto pts = subjectOf(pl).quiverSurface(Rings,Around);
      auto* phys = Gothic::inst().world()->physic();
      for(size_t i=0; i<pts.size(); ++i) {
        const auto& v = pts[i];
        const auto  r = phys->rayStatic({v.x,v.y+150.f,v.z},{v.x,v.y,v.z});
        if(!r.hasCol || r.v.y<=v.y)
          continue;
        gnd = std::max(gnd,r.v.y-v.y);
        if(i>=size_t(2*Around))
          gndTube = std::max(gndTube,r.v.y-v.y); // from the third ring, 16.75 cm down, on
        if(i<size_t(Around))
          gndMouth = std::max(gndMouth,r.v.y-v.y);
        }
      }
    // cm the bottom's centre is over the ground under it (NAN: no ground under it)
    float botUp = NAN;
    if(q.on) {
      const auto pts = subjectOf(pl).quiverSurface(9,12);
      if(pts.size()==size_t(9*12)) {
        Vec3 c;
        for(size_t i=8*12; i<pts.size(); ++i)
          c += pts[i];
        c = c/12.f;
        const auto r = Gothic::inst().world()->physic()->rayStatic({c.x,c.y+150.f,c.z},{c.x,c.y-300.f,c.z});
        if(r.hasCol)
          botUp = c.y-r.v.y;
        }
      }
    std::fprintf(bench.csv,",%.2f,%.2f,%d,%d",gnd,gndMouth,q.ground,int(q.groundOn));
    std::fprintf(bench.csv,",%.2f",botUp);
    std::fprintf(bench.csv,",%.2f,%.1f,%.1f,%.1f,%.2f",gndTube,q.needAngle,q.needBack,q.needOut,q.needBody);
    const Npc& qs = subjectOf(pl);
    std::fprintf(bench.csv,",%.2f,%.1f,%d,%.1f,%.1f",q.trunk,double(qs.quiverClearance()),qs.quiverRays(),
                 double(qs.quiverRayUs()),double(qs.quiverCpuUs()));
    std::fprintf(bench.csv,",%.5f,%.5f,%.5f",q.axis.x,q.axis.y,q.axis.z);
  }
  if(ragdollTrace)
    writeRagdoll(pl);
  if(bench.routine.questTrace && bench.routine.questNpc!=nullptr)
    questFields(*Gothic::inst().world(),*bench.routine.questNpc,true);
  if(bench.living.livingFixture)
    livingFields(*Gothic::inst().world());
  livingSeatFields(*Gothic::inst().world());
  std::fprintf(bench.csv,"\n");
  }

// the hero stands around the watched npc, facing it
void placeHero(World& w, Npc& pl) {
  Npc& s = subjectOf(pl);
  if(&s==&pl || bench.placeDist<=0)
    return;
  const float a   = (s.rotation()+bench.placeAngle)*float(M_PI)/180.f;
  Vec3        pos = s.position() + Vec3(std::cos(a),0,std::sin(a))*bench.placeDist;
  const float g   = groundY(w,pos.x,pos.z,pos.y+100);
  if(std::isnan(g))
    return; // inside a rock: the hero would fall through the world
  pos.y = g;
  pl.setPosition(pos);
  const Vec3 d = s.position()-pos;
  pl.setDirection(Vec3(d.x,0,d.z));
  pl.updateTransform();
  }

// foreach slope <min deg> <max deg> <count> | foreach stairs <count> | foreach wp <name part> <count>
// ... end: the body runs at each spot, after a teleport; $NAME, $I, $ANGLE are replaced
void expandForeach(size_t at, World& w, Npc& pl) {
  const auto a     = split(bench.prog[at]);
  size_t     end   = at+1;
  int        depth = 1;
  for(; end<bench.prog.size(); ++end) {
    const auto c = split(bench.prog[end]);
    if(c[0]=="foreach")
      ++depth;
    if(c[0]=="end" && --depth==0)
      break;
    }
  std::vector<std::string> body(bench.prog.begin()+long(at)+1, bench.prog.begin()+long(std::min(end,bench.prog.size())));

  std::vector<Spot> spots;
  const std::string kind = a.size()>1 ? a[1] : "";
  if(kind=="slope")
    spots = findSpots(w,pl,K_Slope,num(a,2,10),num(a,3,35),"",size_t(num(a,4,5)));
  else if(kind=="stairs")
    spots = findSpots(w,pl,K_Stairs,0,0,"",size_t(num(a,2,5)));
  else if(kind=="wp" && a.size()>2)
    spots = findSpots(w,pl,K_Name,0,90,a[2],size_t(num(a,3,5)));
  else
    say("foreach: unknown kind '" + kind + "'");

  std::vector<std::string> out;
  for(size_t i=0; i<spots.size(); ++i) {
    char ang[32];
    std::snprintf(ang,sizeof(ang),"%.0f",spots[i].angle);
    char info[256];
    std::snprintf(info,sizeof(info),"say spot %zu %s angle=%.1f rough=%.1f dist=%.0f",i,spots[i].name.c_str(),spots[i].angle,spots[i].rough,spots[i].dist);
    out.push_back(info);
    out.push_back("goto " + spots[i].name);
    out.push_back("wait 300");
    for(auto s:body) {
      replaceAll(s,"$NAME",spots[i].name);
      replaceAll(s,"$ANGLE",ang);
      replaceAll(s,"$I",std::to_string(i));
      out.push_back(s);
      }
    }
  say("foreach " + kind + ": " + std::to_string(spots.size()) + " spots");
  const size_t last = std::min(end+1,bench.prog.size());
  bench.prog.erase(bench.prog.begin()+long(at),bench.prog.begin()+long(last));
  bench.prog.insert(bench.prog.begin()+long(at),out.begin(),out.end());
  bench.pc = at;
  }

void face(Npc& pl, const Vec3& dir) {
  if(dir.x==0 && dir.z==0)
    return;
  pl.setDirection(dir);
  pl.updateTransform();
  }

// The bench owns only the hero pose. Path selection and waypoint heights come from WayMatrix.
void beginRoute(const std::vector<std::string>& args, World& world, Npc& player) {
  const float speed=num(args,1,0);
  if(args.size()<4 || !isNumber(args[1]) || !std::isfinite(speed) || speed<=0) {
    arenaAbort("route expects a positive speed in cm/s and at least two waypoints");return;
    }
  std::vector<const WayPoint*> stops;
  for(size_t i=2;i<args.size();++i) {
    auto* point=world.findWayPoint(args[i]);
    if(point==nullptr) {arenaAbort("route waypoint is missing: "+args[i]);return;}
    stops.push_back(point);
    }
  std::vector<Vec3> points{stops.front()->position()};
  for(size_t i=1;i<stops.size();++i) {
    auto path=world.wayTo(*stops[i-1],*stops[i]);
    if(path.last()!=stops[i]) {arenaAbort("route waypoints are not connected: "+stops[i]->name);return;}
    while(auto* point=path.pop())
      if((points.back()-point->position()).quadLength()>0)
        points.push_back(point->position());
    }
  if(points.size()<2) {arenaAbort("route has no distance to travel");return;}
  bench.routeLength=0;
  for(size_t i=1;i<points.size();++i)
    bench.routeLength+=(points[i]-points[i-1]).length();
  bench.routePoints=std::move(points);bench.routeNext=1;bench.routeSpeed=speed;
  bench.routePosition=bench.routePoints.front();bench.routeStarted=bench.time;
  bench.routeWasExternal=player.isExternalControl();bench.routeActive=true;
  player.stopWalking();player.clearAiQueue();player.setExternalControl(true);
  player.setPosition(bench.routePosition);player.updateTransform();
  event("route_start","\"speed_cm_s\":"+std::to_string(speed)+",\"length_cm\":"+std::to_string(bench.routeLength)+
    ",\"points\":"+std::to_string(bench.routePoints.size()));
  }

void tickRoute(Npc& player, uint64_t dt) {
  if(!bench.routeActive)return;
  if(bench.routeNext==bench.routePoints.size()) {
    // Keep the endpoint for one complete update/render before perf stop advances the DSL.
    bench.routeActive=false;player.setExternalControl(bench.routeWasExternal);
    event("route_end","\"speed_cm_s\":"+std::to_string(bench.routeSpeed)+",\"length_cm\":"+std::to_string(bench.routeLength)+
      ",\"elapsed_ms\":"+std::to_string(bench.time-bench.routeStarted)+",\"pass\":true");
    return;
    }
  float remaining=bench.routeSpeed*float(dt)/1000.f;
  Vec3 direction;
  while(remaining>0 && bench.routeNext<bench.routePoints.size()) {
    const auto delta=bench.routePoints[bench.routeNext]-bench.routePosition;
    const float distance=delta.length();
    direction=delta;
    if(distance<=remaining) {
      bench.routePosition=bench.routePoints[bench.routeNext++];remaining-=distance;
      } else {
      bench.routePosition+=delta*(remaining/distance);remaining=0;
      }
    }
  player.setPosition(bench.routePosition);face(player,direction);player.updateTransform();
  }

// The migration oracle deliberately uses the public game API. A generated input file is replayed
// verbatim by the other backend: even waypoint ground adjustment must not change the queries.
void physicsDump(World& w, const Npc& pl, size_t count, uint32_t seed, const std::string& input) {
  struct Query { Vec3 from, to; float radius=0; };
  std::vector<Query> queries;
  if(!input.empty()) {
    std::ifstream file(input);
    Query q;
    while(file >> q.from.x >> q.from.y >> q.from.z >> q.to.x >> q.to.y >> q.to.z >> q.radius)
      queries.push_back(q);
    if(queries.empty() || !file.eof()) {
      say("physics dump failed: malformed or empty query file " + input);
      return;
      }
    } else {
    std::vector<Spot> points;
    w.findWayPoint(pl.position(),[&](const WayPoint& wp) {
      Spot point;
      point.name=wp.name;
      point.pos=wp.pos;
      points.push_back(point);
      return false;
      });
    std::sort(points.begin(),points.end(),[](const Spot& a, const Spot& b) { return a.name<b.name; });
    if(points.empty()) {
      say("physics dump failed: no waypoints");
      return;
      }
    auto next = [&]() {
      seed ^= seed<<13;
      seed ^= seed>>17;
      seed ^= seed<<5;
      return seed;
      };
    if(seed==0) seed=1;
    auto random = [&](float lo, float hi) { return lo+(hi-lo)*(float(next()>>8)/16777216.f); };
    queries.reserve(count);
    for(size_t i=0;i<count;++i) {
      const auto& point=points[next()%points.size()];
      Query q;
      q.from=point.pos+Vec3(random(-500,500),random(50,1500),random(-500,500));
      if(i%4<2) {
        // Vertical ground probes cover the directions used by Foot IK and locomotion.
        q.to=q.from-Vec3(0,random(500,4000),0);
        }
      else if(i%4==2) {
        q.to=q.from+Vec3(random(-5000,5000),random(-1000,1000),random(-5000,5000));
        }
      else {
        q.to=points[next()%points.size()].pos+Vec3(random(-500,500),random(50,1500),random(-500,500));
        }
      static constexpr float radii[]={0.1f,1.f,5.f,25.f,50.f,100.f};
      q.radius=radii[next()%6];
      queries.push_back(q);
      }
    say("physics dump waypoints " + std::to_string(points.size()));
    }
  std::FILE* queryFile=std::fopen((outDir+"/physics-queries.tsv").c_str(),"w");
  std::FILE* resultFile=std::fopen((outDir+"/physics-results.jsonl").c_str(),"w");
  if(queryFile==nullptr || resultFile==nullptr) {
    if(queryFile!=nullptr) std::fclose(queryFile);
    if(resultFile!=nullptr) std::fclose(resultFile);
    say("physics dump failed: cannot create output files");
    return;
    }
  for(const auto& q:queries)
    std::fprintf(queryFile,"%.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",double(q.from.x),double(q.from.y),double(q.from.z),
                 double(q.to.x),double(q.to.y),double(q.to.z),double(q.radius));
  std::fclose(queryFile);
  for(int kind=0;kind<2;++kind) {
    const char* name=kind==0 ? "ray" : "sphere";
    auto cast = [&](const Query& q) { return kind==0 ? w.physic()->ray(q.from,q.to) : w.physic()->sweep(q.from,q.to,q.radius); };
    for(size_t i=0;i<std::min<size_t>(4096,queries.size());++i)
      (void)cast(queries[i]);
    std::vector<double> batchTimes;
    // Timing batches avoids the clock overhead dominating cheap individual queries. JSON/file IO
    // happens outside the timed block, and every batch has the same mixture in both backends.
    constexpr size_t batchSize=256;
    for(size_t begin=0;begin<queries.size();begin+=batchSize) {
      const size_t end=std::min(begin+batchSize,queries.size());
      std::vector<DynamicWorld::RayLandResult> results(end-begin);
      const auto start=std::chrono::steady_clock::now();
      for(size_t i=begin;i<end;++i)
        results[i-begin]=cast(queries[i]);
      const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
      batchTimes.push_back(double(elapsed)/double(end-begin));
      for(size_t i=begin;i<end;++i) {
        const auto& r=results[i-begin];
        const std::string sector=jsonStr(r.sector!=nullptr ? r.sector : "");
        std::fprintf(resultFile,"{\"kind\":\"%s\",\"id\":%zu,\"hit\":%s,\"point\":[%.9g,%.9g,%.9g],\"normal\":[%.9g,%.9g,%.9g],\"material\":%u,\"sector\":%s,\"fraction\":%.9g}\n",
                     name,i,r.hasCol ? "true" : "false",r.hasCol ? double(r.v.x) : 0.0,r.hasCol ? double(r.v.y) : 0.0,r.hasCol ? double(r.v.z) : 0.0,
                     double(r.n.x),double(r.n.y),double(r.n.z),unsigned(r.mat),sector.c_str(),double(r.hitFraction));
        }
      }
    std::sort(batchTimes.begin(),batchTimes.end());
    if(!batchTimes.empty()) {
      const auto percentile=[&](double p) { return batchTimes[size_t(p*double(batchTimes.size()-1))]; };
      say(std::string("physics timing ")+name+" count "+std::to_string(queries.size())+
          " batch 256 ns/query p50 "+std::to_string(percentile(0.5))+" p95 "+std::to_string(percentile(0.95)));
      }
    }
  std::fclose(resultFile);
  say("physics dump complete " + std::to_string(queries.size()) + " rays and spheres");
  }

}

namespace testbench_detail {

void arenaAbort(std::string_view reason) {
  say("ragdoll arena failed: "+std::string(reason));
  event("ragdoll_error","\"reason\":"+jsonStr(reason));
  bench.pc=bench.prog.size(); bench.waitUntil=0;
  }


}

namespace {

bool frameRagdollArena(size_t expected=20) {
  auto* camera=Gothic::inst().camera();
  if(camera==nullptr || bench.crew.size()!=expected) return false;
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low{infinity,infinity,infinity},high{-infinity,-infinity,-infinity};
  auto add=[&](const Vec3& point) {
    low.x=std::min(low.x,point.x); low.y=std::min(low.y,point.y); low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x); high.y=std::max(high.y,point.y); high.z=std::max(high.z,point.z);
    };
  for(const auto* npc:bench.crew) {
    const auto* skeleton=npc->visualSkeleton(); const auto bounds=npc->bounds();
    if(skeleton==nullptr || bounds.rConservative<=0 || !std::isfinite(bounds.rConservative)) return false;
    // The full proof pads each axis by50cm; a radial proxy needs the diagonal.
    bench.ragdoll.ragdollArenaRadii.push_back(bounds.rConservative+std::sqrt(3.f)*50.f);
    const auto object=npc->transform();
    for(int c=0;c<8;++c) {
      Vec3 p{bounds.bbox[c&1].x,bounds.bbox[(c>>1)&1].y,bounds.bbox[(c>>2)&1].z}; object.project(p); add(p);
      }
    for(const auto& node:skeleton->nodes) {
      const auto p=npc->mapBone(node.name); add(p-Vec3(50,50,50)); add(p+Vec3(50,50,50));
      }
    }
  const float halfY=camera->fovY()*float(M_PI)/360.f;
  const auto projection=camera->projective();
  const float aspect=std::abs(projection.at(1,1)/projection.at(0,0));
  const float halfFov=std::min(halfY,std::atan(std::tan(halfY)*aspect));
  const float distance=((high-low)*.5f).length()*1.25f/std::sin(halfFov);
  if(!std::isfinite(distance) || distance<=0) return false;
  bench.ragdoll.ragdollArenaTarget=(high+low)*.5f;
  bench.camPos=bench.ragdoll.ragdollArenaTarget+Vec3(0,.9238795f*distance,.3826834f*distance);
  bench.cam=C_RagdollArena; bench.ragdoll.ragdollArenaCamera=true; bench.camAuto=false; bench.smValid=false;
  lookFrom(bench.camPos,bench.ragdoll.ragdollArenaTarget);
  event("ragdoll_arena_camera","\"origin\":"+ragdollVector(bench.camPos)+",\"target\":"+
        ragdollVector(bench.ragdoll.ragdollArenaTarget)+",\"bounds_min\":"+ragdollVector(low)+",\"bounds_max\":"+
        ragdollVector(high)+",\"fov_y\":"+std::to_string(camera->fovY())+",\"aspect\":"+std::to_string(aspect));
  return true;
  }


bool monsterCharacterRenderProof(World& world,Npc& npc,std::string_view role) {
  npc.updateTransform();
  const auto* camera=Gothic::inst().camera();const auto* skeleton=npc.visualSkeleton();
  const auto bounds=npc.bounds();const auto object=npc.transform();
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
  Vec3 ndcLow(infinity,infinity,infinity),ndcHigh(-infinity,-infinity,-infinity);
  bool projected=camera && skeleton;unsigned behind=0;std::string bones="[";
  auto add=[&](const Vec3& point) {
    low.x=std::min(low.x,point.x);low.y=std::min(low.y,point.y);low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x);high.y=std::max(high.y,point.y);high.z=std::max(high.z,point.z);
    if(!camera)return;
    float x=point.x,y=point.y,z=point.z,w=1;camera->viewProj().project(x,y,z,w);
    if(w<=0 || !std::isfinite(w)) {++behind;projected=false;return;}
    const Vec3 ndc(x/w,y/w,z/w);
    ndcLow.x=std::min(ndcLow.x,ndc.x);ndcLow.y=std::min(ndcLow.y,ndc.y);ndcLow.z=std::min(ndcLow.z,ndc.z);
    ndcHigh.x=std::max(ndcHigh.x,ndc.x);ndcHigh.y=std::max(ndcHigh.y,ndc.y);ndcHigh.z=std::max(ndcHigh.z,ndc.z);
    projected&=std::isfinite(ndc.x) && std::isfinite(ndc.y) && std::isfinite(ndc.z) &&
      std::abs(ndc.x)<=.98f && std::abs(ndc.y)<=.98f && ndc.z>=0 && ndc.z<=1;
    };
  for(unsigned i=0;i<8;++i) {
    Vec3 point(bounds.bbox[i&1].x,bounds.bbox[(i>>1)&1].y,bounds.bbox[(i>>2)&1].z);object.project(point);add(point);
    }
  if(skeleton)for(const auto& bone:skeleton->nodes) {
    const auto point=npc.mapBone(bone.name);add(point);
    if(bone.name.find("FOOT")==std::string::npos && bone.name.find("HEAD")==std::string::npos && bone.name.find("PELVIS")==std::string::npos)continue;
    if(bones.size()>1)bones+=',';
    bones+="{\"name\":"+jsonStr(bone.name)+",\"position_cm\":"+ragdollVector(point)+
      ",\"occluded\":"+(hidden(world,bench.camPos,point+Vec3(0,5,0))?"true":"false")+"}";
    }
  bench.monster.monsterCharacterRenderRequest=&npc;const bool drawable=TestBench::ragdollBodyDrawable(npc);bench.monster.monsterCharacterRenderRequest=nullptr;
  event("monster_character_render","\"route\":"+jsonStr(bench.monster.monsterCharacterRoute.name)+",\"role\":"+jsonStr(role)+
    ",\"npc_id\":"+std::to_string(world.npcId(&npc))+",\"instance\":"+jsonStr(symName(world,&npc))+
    ",\"root_cm\":"+ragdollVector(npc.position())+",\"visual_root_cm\":"+ragdollVector(npc.characterVisualPosition())+
    ",\"world_min_cm\":"+ragdollVector(low)+",\"world_max_cm\":"+ragdollVector(high)+
    ",\"ndc_min\":"+ragdollVector(ndcLow)+",\"ndc_max\":"+ragdollVector(ndcHigh)+
    ",\"behind_camera_points\":"+std::to_string(behind)+",\"full_pose_projected\":"+(projected?"true":"false")+
    ",\"body_drawable\":"+(drawable?"true":"false")+",\"hidden_draw_types\":"+std::to_string(DrawCommands::hiddenTypes)+
    ",\"near_clip_cm\":"+ragdollNumber(camera?camera->zNear():0)+",\"bones\":"+bones+"]");
  return drawable && projected;
  }

void monsterCharacterCamera(World& world,Npc& player,bool requested=false,float preferredYaw=0,float preferredElevation=.42f) {
  if(requested && (!std::isfinite(preferredYaw) || !std::isfinite(preferredElevation) || preferredElevation<=0 || preferredElevation>=1)) {
    monsterCharacterError("monster camera expects a finite yaw and elevation fraction strictly between 0 and 1");return;
    }
  const auto& route=bench.monster.monsterCharacterRoute;
  if(bench.monster.monsterCharacterCrowd) {
    if(requested) {monsterCharacterError("monster camera angle selection requires a prepared video fixture");return;}
    event("monster_character_camera","\"route\":\"performance\",\"eye_cm\":"+ragdollVector(bench.camPos)+",\"fixed\":true");return;
    }
  if(!route.npc) {monsterCharacterError("monster camera requires a prepared fixture");return;}
  std::vector<Vec3> points{route.start,route.goal,bench.monster.monsterCharacterHeroStart};
  for(const auto& actor:bench.monster.monsterCharacterExtras) {
    if(actor.npc)points.push_back(actor.start);
    for(const auto& point:actor.points)points.push_back(point.pos);
    }
  // Include the ground along the entire lane, not only the endpoints. The
  // resulting held camera is identical in OFF and ON and never tracks roots.
  const auto actorPoints=points.size();
  for(unsigned i=1;i<10;++i) {
    Vec3 floor;const auto at=route.start+(route.goal-route.start)*(float(i)/10.f);
    if(characterFloor(world,at,floor))points.push_back(floor);
    }
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
  float height=180,horizontalPadding=75;
  auto envelope=[&](const Npc& npc) {
    const auto bounds=npc.bounds();height=std::max(height,bounds.bbox[1].y-bounds.bbox[0].y);
    const float x=std::max(std::abs(bounds.bbox[0].x),std::abs(bounds.bbox[1].x));
    const float z=std::max(std::abs(bounds.bbox[0].z),std::abs(bounds.bbox[1].z));
    // A static local XZ circle covers any later yaw. Extra space preserves
    // flapping wings beyond the rest pose without following live animation.
    horizontalPadding=std::max(horizontalPadding,std::hypot(x,z)+50.f);
    };
  envelope(player);envelope(*route.npc);
  for(const auto& actor:bench.monster.monsterCharacterExtras)if(actor.npc)envelope(*actor.npc);
  for(const auto& point:points) {
    low.x=std::min(low.x,point.x-horizontalPadding);low.y=std::min(low.y,point.y-35);low.z=std::min(low.z,point.z-horizontalPadding);
    high.x=std::max(high.x,point.x+horizontalPadding);high.y=std::max(high.y,point.y+height+50);high.z=std::max(high.z,point.z+horizontalPadding);
    }
  const auto target=(low+high)*.5f;auto along=route.goal-route.start;along.y=0;
  along=along.length()>1?Vec3::normalize(along):Vec3(1,0,0);const Vec3 side(-along.z,0,along.x);
  auto* camera=Gothic::inst().camera();if(!camera) {monsterCharacterError("monster video camera unavailable");return;}
  const float halfY=camera->fovY()*float(M_PI)/360.f;const auto projection=camera->projective();
  const float aspect=std::abs(projection.at(1,1)/projection.at(0,0));
  const float halfFov=std::min(halfY,std::atan(std::tan(halfY)*aspect));
  const float distance=std::max(600.f,((high-low)*.5f).length()*1.1f/std::sin(halfFov));
  Vec3 eye;bool found=false;float selectedYaw=0,selectedElevation=0;bool fallback=false;
  std::vector<std::pair<float,float>> shots;
  if(requested)shots.emplace_back(preferredYaw,preferredElevation);
  for(float elevation:{.42f,.62f,.85f})for(float yaw:{0.f,180.f,30.f,-30.f,150.f,210.f,60.f,-60.f})
    if(!requested || std::abs(yaw-preferredYaw)>.001f || std::abs(elevation-preferredElevation)>.001f)shots.emplace_back(yaw,elevation);
  for(size_t shot=0;shot<shots.size();++shot) {
    const auto [yaw,elevation]=shots[shot];
    const auto direction=rotY(side,yaw);const auto candidate=target+direction*(distance*std::sqrt(1.f-elevation*elevation))+Vec3(0,distance*elevation,0);
    bool visible=true;
    for(size_t pointId=0;pointId<points.size();++pointId)for(float y:{15.f,height*.5f,height}) {
      const auto& point=points[pointId];
      // Theoretical lane samples inside a prop describe its occupied volume.
      // Keep that volume in the frame without demanding a view through it;
      // actual actors and authored endpoints retain every sight-line check.
      bool propFootprint=false;
      if(pointId>=actorPoints && bench.character.characterProp) {
        const auto delta=point-bench.character.characterPropInitial;propFootprint=std::hypot(delta.x,delta.z)<120;
        }
      if(pointId>=actorPoints)for(const auto& prop:bench.monster.monsterCharacterProps) {
        const auto delta=point-prop.start;propFootprint|=std::hypot(delta.x,delta.z)<120;
        }
      if(!propFootprint && hidden(world,candidate,point+Vec3(0,y,0))) {visible=false;break;}
      }
    if(!visible)continue;
    eye=candidate;found=true;selectedYaw=yaw;selectedElevation=elevation;fallback=requested && shot!=0;break;
    }
  if(!found) {monsterCharacterError("monster fixed camera cannot see the complete route and feet");return;}
  bench.cam=C_Water;bench.water.cameraTarget=target;bench.camAuto=false;bench.smValid=false;lookFrom(eye,target);
  std::string actors="[{\"npc_id\":"+std::to_string(world.npcId(route.npc))+",\"role\":\"primary\",\"start_cm\":"+ragdollVector(route.start)+"}";
  for(const auto& actor:bench.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    actors+=",{\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"role\":"+jsonStr(actor.role)+",\"start_cm\":"+ragdollVector(actor.start)+"}";
  std::string ready="[{\"npc_id\":"+std::to_string(world.npcId(route.npc))+",\"root_cm\":"+ragdollVector(route.npc->position())+"}";
  for(const auto& actor:bench.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    ready+=",{\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"root_cm\":"+ragdollVector(actor.npc->position())+"}";
  event("monster_character_camera","\"route\":"+jsonStr(route.name)+",\"camera_cm\":"+ragdollVector(eye)+
    ",\"requested_shot\":"+(requested?"true":"false")+",\"requested_yaw_deg\":"+ragdollNumber(preferredYaw)+
    ",\"requested_elevation_fraction\":"+ragdollNumber(preferredElevation)+",\"selected_yaw_deg\":"+ragdollNumber(selectedYaw)+
    ",\"selected_elevation_fraction\":"+ragdollNumber(selectedElevation)+",\"used_fallback\":"+(fallback?"true":"false")+
    ",\"seed\":"+std::to_string(bench.monster.monsterCharacterSeed)+",\"hour\":"+std::to_string(world.time().hour())+
    ",\"minute\":"+std::to_string(world.time().minute())+",\"hero_cm\":"+ragdollVector(bench.monster.monsterCharacterHeroStart)+",\"actors\":"+actors+"]"+
    ",\"ready_actors\":"+ready+"],\"hero_ready_cm\":"+ragdollVector(player.position())+
    ",\"target_cm\":"+ragdollVector(target)+",\"bounds_min_cm\":"+ragdollVector(low)+",\"bounds_max_cm\":"+ragdollVector(high)+
    ",\"horizontal_actor_padding_cm\":"+ragdollNumber(horizontalPadding)+",\"fov_y\":"+ragdollNumber(camera->fovY())+",\"aspect\":"+ragdollNumber(aspect)+",\"fixed\":true");
  bool rendered=monsterCharacterRenderProof(world,player,"hero");
  rendered=monsterCharacterRenderProof(world,*route.npc,"primary") && rendered;
  for(auto& actor:bench.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    rendered=monsterCharacterRenderProof(world,*actor.npc,actor.role) && rendered;
  if(!rendered)monsterCharacterError("monster fixed camera requires drawable actor meshes and their complete projected poses");
  }

// This fixture changes only preparation. After the native stand-up is queued,
// NPC samples and completion checks are passive: no path, state or movement command.
}

namespace testbench_detail {

void exec(const std::string& line, World& w, Npc& pl, PlayerControl& ctrl) {
  const auto a = split(line);
  if(a.empty())
    return;
  const std::string& c = a[0];

  if(c=="healing") {
    try {healingCommand(bench.healing,a,w,pl,ctrl,bench.melee.meleeShadowActors);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    return;
  }
  if(c=="roads") {
    try {
      event("roads_command","\"sample\":"+KnownRoadsBench::command(a,w,outDir));
    } catch(const std::exception& error) {
      event("roads_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="memory") {
    try {
      event("memory_command","\"sample\":"+NavigationMemoryBench::command(a,w,pl,ctrl,outDir));
    } catch(const std::exception& error) {
      event("memory_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="body_traces") {
    try {
      event("body_traces","\"sample\":"+BodyTracesBench::command(a,w,pl,outDir));
    } catch(const std::exception& error) {
      event("body_traces_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="blood_stains") {
    try {
      event("blood_stains","\"sample\":"+BloodStainsBench::command(a,w,pl,outDir));
    } catch(const std::exception& error) {
      event("blood_stains_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="keys") {
    try {
      event("keys_command","\"sample\":"+NpcKeysBench::command(a,w,pl,ctrl,outDir));
    } catch(const std::exception& error) {
      event("keys_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  // Navigation-eye commands are independent of navigation mesh and body rays.
  if(c=="eye") {
    try {
      if(a.size()>1 && a[1]=="setup") NavigationMemoryBench::reset(true);
      event("eye_command","\"sample\":"+NavigationEyeBench::command(a,w,pl,outDir));
    } catch(const std::exception& error) {
      event("eye_error","\"reason\":"+jsonStr(error.what()));
      arenaAbort(error.what());
    }
    return;
  }
  if(c=="navdemos") {
    const auto* live=std::getenv("OPENGOTHIC_NAV_LIVE");
    // Only the explicit live stand may record navigation in a rendered bench.
    if(!Resources::isHeadless() && (live==nullptr || std::string_view(live)!="1"))
      throw std::runtime_error("navdemos is runner-only");
    bench.navigationOnly = true;
    NavDemos::command(a,w,pl,outDir);
    return;
  }
  // MSVC allows at most 128 nested blocks, including else-if clauses (C1061).
  // Split the ladder into runs while preserving the first matching command.
  bool dispatched=true;
  if(c=="navmesh") {
    bench.navigationOnly = true;
    if(a.size()!=2 || a[1]!="wait") NavBench::command(a,w,pl,outDir);
    }
  else if(monsterCharacterCommand(bench.monster,monsterCharacterContext(),a,w,pl)) return;
  else if(c=="monstercharacter" && a.size()>=2 && a[1]=="camera") {
    if((a.size()!=2 && a.size()!=4) || (a.size()==4 && (!isNumber(a[2]) || !isNumber(a[3])))) {
      monsterCharacterError("monster camera expects no arguments or yaw_degrees elevation_fraction");return;
      }
    monsterCharacterCamera(w,pl,a.size()==4,a.size()==4?num(a,2,NAN):0,a.size()==4?num(a,3,NAN):.42f);
    }
  else if(c=="monstercharacter" && a.size()==5 && a[1]=="follow") {
    auto& route=bench.monster.monsterCharacterRoute;
    auto* watched=route.npc;
    if(bench.monster.monsterCharacterPack && bench.monster.monsterCharacterExtras.size()>=2)watched=bench.monster.monsterCharacterExtras[1].npc;
    if(!watched) {monsterCharacterError("follow requires prepared monster");return;}
    exec("watch id:"+std::to_string(w.npcId(watched)),w,pl,ctrl);
    exec("cam follow "+a[2]+" "+a[3]+" "+a[4],w,pl,ctrl);
    exec("cam look "+ragdollNumber(std::min(130.f,std::max(45.f,(watched->bounds().bbox[1].y-watched->bounds().bbox[0].y)*.45f))),w,pl,ctrl);
    exec(route.name=="troll_barrel"?"view clean 500":"view clean 300",w,pl,ctrl);
    updateCamera(w,*watched);
    std::string actors="[{\"npc_id\":"+std::to_string(w.npcId(route.npc))+",\"role\":\"primary\",\"start_cm\":"+ragdollVector(route.start)+"}";
    for(const auto& actor:bench.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
      actors+=",{\"npc_id\":"+std::to_string(w.npcId(actor.npc))+",\"role\":"+jsonStr(actor.role)+",\"start_cm\":"+ragdollVector(actor.start)+"}";
    event("monster_character_camera","\"route\":"+jsonStr(route.name)+",\"mode\":\"follow\",\"fixed\":false,\"watch_npc_id\":"+std::to_string(w.npcId(watched))+
      ",\"distance_cm\":"+ragdollNumber(bench.camDist)+",\"height_cm\":"+ragdollNumber(bench.camHeight)+",\"angle_deg\":"+ragdollNumber(bench.camAngle)+
      ",\"look_height_cm\":"+ragdollNumber(bench.camLook)+",\"seed\":"+std::to_string(bench.monster.monsterCharacterSeed)+",\"hour\":"+std::to_string(w.time().hour())+
      ",\"minute\":"+std::to_string(w.time().minute())+",\"hero_cm\":"+ragdollVector(bench.monster.monsterCharacterHeroStart)+",\"actors\":"+actors+"]");
    monsterCharacterRenderProof(w,*watched,"followed");
    }
  else if(characterCommand(bench.character,characterContext(),a,w,pl,ctrl)) return;
  else if(Resources::isHeadless() && c=="cheat" && a.size()==2 && a[1]=="god") {
    // Marvin also draws a notification with a GPU font. The CPU runner needs the same game flag.
    Gothic::inst().setGodMode(!Gothic::inst().isGodMode());
    say("cheat god");
    }
  else if(itemWakeCommand(a,w,pl)) return;
  else if(placedItemsCommand(bench.placedItems,a,w,pl,ctrl)) return;
  else if(itemHullsCommand(bench.itemHulls,a,w,pl)) return;
  else if(c=="physics" && a.size()>1 && a[1]=="dump") {
    physicsDump(w,pl,size_t(std::clamp(num(a,2,100000),1.f,1000000.f)),uint32_t(num(a,3,73013)),a.size()>4 ? a[4] : "");
    }
  else if(c=="physics" && a.size()==2 && a[1]=="flags-check") {
    struct Flag {const char* key;bool Gothic::PhysicsFlags::*member;};
    const Flag flags[]{
      {"ragdoll",&Gothic::PhysicsFlags::ragdoll},
      {"ragdoll_quest_guard",&Gothic::PhysicsFlags::questGuard},
      {"living_bodies",&Gothic::PhysicsFlags::livingBodies},
      {"arrow_physics",&Gothic::PhysicsFlags::arrowPhysics},
      {"arrow_sticking",&Gothic::PhysicsFlags::arrowSticking},
      {"spell_physics",&Gothic::PhysicsFlags::spellPhysics},
      {"body_impact_damage",&Gothic::PhysicsFlags::bodyImpactDamage},
      {"body_traces",&Gothic::PhysicsFlags::bodyTraces},
      {"blood",&Gothic::PhysicsFlags::blood},
      {"movable_vobs",&Gothic::PhysicsFlags::movableVobs},
      {"item_hulls",&Gothic::PhysicsFlags::itemHulls},
      {"item_deepest_contact",&Gothic::PhysicsFlags::itemDeepestContact},
      {"ragdoll_birth_guard",&Gothic::PhysicsFlags::ragdollBirthGuard},
      {"furniture_parts",&Gothic::PhysicsFlags::furnitureParts},
      {"placed_items",&Gothic::PhysicsFlags::placedItems},
      {"cloth",&Gothic::PhysicsFlags::cloth}
      };
    auto& npc=subjectOf(pl);
    const auto tick=w.tickCount();
    bool pass=true;
    for(const auto& flag:flags) {
      const int original=Gothic::settingsGetI("GAME",flag.key);
      pass&=(Gothic::physicsFlags().*flag.member)==(original!=0);
      for(const std::string_view phase:{"off","on","restore"}) {
        const int value=phase=="restore" ? original : int(phase=="on");
        Gothic::settingsSetI("GAME",flag.key,value);
        const bool immediate=Gothic::physicsFlags().*flag.member;
        npc.prepareRagdoll();npc.prepareLivingBody();
        const int configured=Gothic::settingsGetI("GAME",flag.key);
        const bool cached=Gothic::physicsFlags().*flag.member;
        const bool same=w.tickCount()==tick;
        const bool ok=immediate==(value!=0) && cached==(configured!=0) && configured==value && same;
        pass&=ok;
        event("physics_flags_check","\"flag\":"+jsonStr(flag.key)+",\"phase\":"+jsonStr(phase)+
          ",\"original\":"+std::to_string(original)+",\"configured\":"+std::to_string(configured)+
          ",\"cached\":"+(cached?"true":"false")+",\"prepared\":true,\"same_dispatch\":"+(same?"true":"false")+
          ",\"pass\":"+(ok?"true":"false"));
        }
      }
    if(!pass) arenaAbort("physics flag snapshot differs in same dispatch");
    }
  else if(c=="physics" && a.size()==2 && a[1]=="stats") {
    const auto stats=w.physic()->physicsStats();
    char text[200];
    std::snprintf(text,sizeof(text),"physics stats step_ms=%.6f bodies=%zu rays=%llu",
                  double(stats.stepMilliseconds),stats.bodies,(unsigned long long)stats.rays);
    say(text);
    }
  else if(c=="traces" && a.size()>=2) {
    std::string command=a[1],value,error;
    for(size_t i=2;i<a.size();++i)command+=' '+a[i];
    if(!TestBench::physicsTracesDebug(w,pl,command,value,error)) {arenaAbort(error);return;}
    event("physics_traces_debug","\"command\":"+jsonStr(command)+",\"sample\":"+value);
    }
  else if(destructionCommand(bench.destruction,destructionContext(),a,w,pl,ctrl)) return;
  else if(fracturePiecesCommand(bench.destruction.destructionIds,bench.destruction.destructionMeleeActor,bench.destruction.destructionMeleeStand,bench.destruction.destructionMeleeFacing,bench.cam,bench.cutPending,a,w)) return;
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if(c=="water-film" && a.size()==3 && a[1]=="setup")waterFilmSetup(w,pl,a[2]);
  else if(c=="water-film" && a.size()==2 && a[1]=="begin")waterFilmBegin(w,pl);
  else if(c=="water-film" && a.size()==2 && a[1]=="checkpoint")waterFilmCheckpoint(w);
  else if(c=="cloth" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","cloth",a[1]=="on");
    event("cloth_toggle","\"enabled\":"+std::string(w.physic()->clothEnabled()?"true":"false"));
    }
  else if(c=="cloth" && a.size()==4 && a[1]=="setup")clothSetup(w,pl,a[2],a[3]);
  else if(c=="cloth" && a.size()==6 && a[1]=="room-check") {
    if(!isNumber(a[2]) || !isNumber(a[3]) || !isNumber(a[4]) || (a[5]!="indoor" && a[5]!="outdoor")) {
      arenaAbort("cloth room-check expects finite x y z and indoor or outdoor");return;
      }
    const Vec3 point(num(a,2,0),num(a,3,0),num(a,4,0));
    if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      arenaAbort("cloth room-check expects finite coordinates");return;
      }
    const bool indoor=w.isIndoor(point),expected=a[5]=="indoor";
    const bool pass=indoor==expected;
    event("cloth_room_check","\"point_cm\":"+ragdollVector(point)+",\"bsp_room\":"+jsonStr(w.roomAt(point))+
      ",\"indoor\":"+(indoor?"true":"false")+",\"expected\":"+jsonStr(a[5])+",\"pass\":"+(pass?"true":"false"));
    if(!pass)arenaAbort("cloth room classification check failed");
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="wind") {
    if(a[2]!="calm" && a[2]!="gust" && a[2]!="natural") {arenaAbort("cloth wind expects calm, gust or natural");return;}
    w.physic()->cloth().setWindScale(a[2]=="calm"?0.f:(a[2]=="gust"?2.f:1.f));
    event("cloth_wind","\"mode\":"+jsonStr(a[2]));
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="checkpoint")clothCheckpoint(w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="shoot")clothShoot(w,pl,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="arrow-npc")clothArrowNpc(w,pl,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="save")clothSaveSession(w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="camera")clothCamera(w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="camera-range") {
    if(!isNumber(a[2])) {arenaAbort("cloth camera-range expects a numeric range in cm");return;}
    clothCameraRange(w,num(a,2,0));
    }
  else if(c=="cloth" && a.size()==4 && a[1]=="camera-turn") {
    const float degrees=num(a,2,0),duration=num(a,3,0);
    auto* camera=Gothic::inst().camera();
    if(bench.cam==C_Turn && bench.time>=bench.camTurnStart+bench.camTurnDuration)updateCamera(w,pl);
    if(!isNumber(a[2]) || !isNumber(a[3]) || !std::isfinite(degrees) || !std::isfinite(duration) ||
       duration<1.f || duration>60000.f || camera==nullptr || bench.cam!=C_Hold) {
      arenaAbort("cloth camera-turn requires a held camera, finite degrees and duration from 1 to 60000 ms");return;
      }
    bench.camTurnAngles=bench.camHeldAngles;bench.camTurnDegrees=degrees;
    bench.camTurnStart=bench.time;bench.camTurnDuration=uint64_t(duration);bench.cam=C_Turn;
    bench.waitUntil=bench.time+bench.camTurnDuration+1;
    event("cloth_camera_turn","\"degrees\":"+ragdollNumber(degrees)+",\"duration_ms\":"+
      std::to_string(bench.camTurnDuration)+",\"camera_cm\":"+ragdollVector(bench.camPos)+
      ",\"pitch_degrees\":"+ragdollNumber(bench.camTurnAngles.x)+",\"from_yaw_degrees\":"+
      ragdollNumber(bench.camTurnAngles.y));
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="initial")clothInitial(w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="verify") {
    bool pass=true;bool target=false;
    size_t sails=0,flags=0,exposedSails=0,exposedFlags=0;
    for(const auto& body:w.physic()->cloth().snapshots()) {
      if(a[2]=="harbour-wind") {
        if(!body.active)continue;
        if(body.visual.starts_with("NW_SHIP_SAIL_02:")) {
          ++sails;exposedSails+=body.exposed;
          }
        else if(body.visual.starts_with("NW_SHIP_FLAG_01:") || body.visual.starts_with("NW_SHIP_FLAG_02:")) {
          ++flags;exposedFlags+=body.exposed;
          }
        }
      else if(a[2]=="anchors") {
        target|=body.zenId==bench.clothZen && body.active && body.pinned>0;
        pass&=std::isfinite(body.maxAnchorErrorCm) && body.maxAnchorErrorCm<=.5f &&
          std::isfinite(body.maxSpeedEverCmPerSec) && body.maxSpeedEverCmPerSec<=Cloth::maxVelocityMS*100.f+.5f && body.recoveryCount==0;
        for(const auto& point:body.worldPositionsCm)
          pass&=std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        }
      else if(body.zenId==bench.clothZen) {
        target=true;
        if(a[2]=="arrow")pass&=bench.clothShotKind=="arrow" && body.arrowPasses>bench.clothShotHitsBefore && bench.clothArrowDownstream>=30.f;
        else if(a[2]=="fireball" || a[2]=="wind")pass&=bench.clothShotKind==a[2] && body.projectileHits>bench.clothShotHitsBefore;
        else pass=false;
        }
      }
    if(a[2]=="harbour-wind") {
      target=sails==5 && flags==4;
      pass&=exposedSails==sails && exposedFlags==flags;
      }
    pass&=target;
    event("cloth_verify","\"check\":"+jsonStr(a[2])+",\"pass\":"+(pass?"true":"false")+
      ",\"active_sails\":"+std::to_string(sails)+",\"exposed_sails\":"+std::to_string(exposedSails)+
      ",\"active_flags\":"+std::to_string(flags)+",\"exposed_flags\":"+std::to_string(exposedFlags)+
      ",\"arrow_downstream_cm\":"+ragdollNumber(bench.clothArrowDownstream)+",\"cloth\":"+clothRows(w));
    if(!pass)arenaAbort("cloth native "+a[2]+" check failed");
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="distance") {
    if(bench.clothZen==uint32_t(-1)) {arenaAbort("cloth boundary requires a selected target");return;}
    if(!clothStandAt(w,pl,bench.clothCenter,num(a,2,4000))) {
      if(bench.clothObserverSupport!="fixture-static-flight") {
        arenaAbort("cloth boundary requires a dry supported observer at the requested radius");return;
        }
      bench.clothHeldPosition=bench.clothCenter+bench.clothDirection*num(a,2,4000);
      pl.setPosition(bench.clothHeldPosition);pl.updateTransform();
      }
    event("cloth_observer","\"hero_cm\":"+ragdollVector(pl.position())+",\"distance_cm\":"+
      ragdollNumber((pl.position()-bench.clothCenter).length()));
    }
  else if(c=="cloth" && a.size()==2 && a[1]=="reset-stats")w.physic()->cloth().resetStats();
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="cloth" && a.size()==3 && a[1]=="measure") {
    if(!AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {arenaAbort("cloth normal cadence arm failed");return;}
    bench.clothMeasuring=true;bench.clothPhysics.clear();bench.clothPhysics.reserve(20000);bench.clothOverflow=false;
    const auto all=w.physic()->cloth().snapshots();
    const auto active=std::count_if(all.begin(),all.end(),[](const auto& body){return body.active;});
    AppCadence::counts(true,uint32_t(active),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("cloth_measure_before","\"mode\":"+jsonStr(a[2])+",\"active_soft_bodies\":"+
      std::to_string(active)+",\"cloth\":"+clothRows(w));
    }
  else if(c=="cloth" && a.size()==2 && a[1]=="report") {
    bench.clothMeasuring=false;
    const auto all=w.physic()->cloth().snapshots();
    const auto active=std::count_if(all.begin(),all.end(),[](const auto& body){return body.active;});
    AppCadence::counts(false,uint32_t(active),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    std::vector<float> costs;costs.reserve(bench.clothPhysics.size());
    if(auto* file=std::fopen((outDir+"/cloth-physics.csv").c_str(),"w")) {
      std::fprintf(file,"sample,t_game_ms,dt_ms,step_ms,native_bodies\n");
      for(size_t i=0;i<bench.clothPhysics.size();++i) {
        const auto& row=bench.clothPhysics[i];costs.push_back(row.milliseconds);
        std::fprintf(file,"%zu,%llu,%llu,%.9g,%zu\n",i,(unsigned long long)row.time,
          (unsigned long long)row.dt,double(row.milliseconds),row.bodies);
        }
      std::fclose(file);
      }
    else {arenaAbort("cannot write cloth native step timings");return;}
    std::sort(costs.begin(),costs.end());
    const auto p95=costs.empty()?0.f:costs[std::min(costs.size()-1,size_t(std::ceil(double(costs.size())*.95)-1))];
    event("cloth_measure_after","\"active_soft_bodies\":"+std::to_string(active)+
      ",\"samples\":"+std::to_string(costs.size())+",\"physics_p95_ms\":"+ragdollNumber(p95)+
      ",\"overflow\":"+(bench.clothOverflow?"true":"false")+",\"cloth\":"+clothRows(w));
    }
#else
  else if(c=="cloth" && a.size()>=2 && (a[1]=="measure" || a[1]=="report")) {
    arenaAbort("cloth timing requires a build with OPENGOTHIC_ENABLE_APP_CADENCE=ON");
    }
#endif
  else if(waterCommand(bench.water,waterContext(),a,w,pl,ctrl)) return;
  else if(c=="item-save" && a.size()==2 && a[1]=="initial") {
    arenaAbort("item save initial must be the first scenario command before simulation");
    }
  else if(c=="item-save" && a.size()==2 && a[1]=="report") {
    try {savedItemReport(w);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(noiseCommand(movablesContext(),a,w,pl,ctrl)) return;
  else if(c=="world-data" && a.size()==2 && (a[1]=="prepare" || a[1]=="check")) {
    Interactive* door=nullptr;
    for(uint32_t id=0;auto* mob=w.mobsiById(id);++id)
      if(mob->isDoor() && mob->stateCount()>0 && (!door || mob->zenId()<door->zenId()))
        door=mob;
    if(!door) {arenaAbort("world-data needs an authored door with an open state");return;}
    if(a[1]=="prepare") {
      door->setAsCracked(true);
      door->resetPositionToTA(1);
      }
    const bool pass=door->stateId()==1 && door->isCracked() && !door->isLocked();
    event("world_data_door","\"operation\":"+jsonStr(a[1])+",\"pass\":"+(pass?"true":"false")+
      ",\"zen_id\":"+std::to_string(door->zenId())+",\"mob_id\":"+std::to_string(w.mobsiId(door))+
      ",\"state\":"+std::to_string(door->stateId())+",\"cracked\":"+(door->isCracked()?"true":"false")+
      ",\"locked\":"+(door->isLocked()?"true":"false")+
      ",\"world_load_ms\":"+ragdollNumber(float(w.loadMilliseconds())));
    if(!pass)arenaAbort("world-data door lost its open or cracked state");
    }
  else if(ownMeshCommand(ownMeshContext(),a,w,pl,ctrl)) return;


  else if(furniturePartsCommand(bench.furnitureParts,a,w,pl)) return;
  else if(movableCommand(movablesContext(),a,w,pl,ctrl)) return;
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if(spellCommand(movablesContext(),a,w,pl,ctrl)) return;
  else if(bodyImpactCommand(bench.bodyImpact,bodyImpactContext(),a,w,pl)) return;
  else if((c=="sticking" || c=="arrow") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(livingCommand(livingContext(),a,w,pl,ctrl)) return;
  else if(meleeCommand(meleeContext(),a,w,pl,ctrl)) return;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="cadence" && a.size()==2 && a[1]=="normal") {
    AppCadence::enable(outDir+"/appcadence.csv");FootIk::setProbing(false);
    }
  else if(c=="cadence" && a.size()==2 && a[1]=="check") {
    uint32_t models=0,dead=0,parts=0;
    const auto* skeleton=bench.crew.empty() ? nullptr : bench.crew.front()->visualSkeleton();
    for(const auto* npc:bench.crew) {
      if(skeleton!=nullptr && npc->visualSkeleton()==skeleton && TestBench::ragdollBodyDrawable(*npc)) ++models;
      if(npc->isDead()) ++dead;
      if(const auto* body=npc->ragdollBody()) parts+=uint32_t(body->partCount());
      }
    if(bench.crew.size()!=20 || models!=20 || dead!=0 || parts!=0) { arenaAbort("cadence requires twenty loaded live humans");return; }
    bench.cadenceVerified=true;AppCadence::counts(true,models,dead,parts,0,0);
    FrameProfile::setActors(bench.crew);
    }
  else if(c=="cadence" && a.size()==3 && a[1]=="die") {
    if(!bench.cadenceVerified || !AppCadence::normalClock() || (a[2]!="on" && a[2]!="off") || !AppCadence::arm(a[2]=="on")) { arenaAbort("cadence death start failed");return; }
    for(auto* npc:bench.crew) { npc->setLastHitBy(&pl);if(npc->setRagdollDown(true)) ++bench.cadenceDown;npc->prepareRagdoll(); }
    }
  else if(c=="cadence" && a.size()==2 && a[1]=="wait") bench.cadenceWait=true;
  else if(c=="cadence" && a.size()==2 && a[1]=="report") {
    uint32_t models=0,dead=0,parts=0,asleep=0;
    for(const auto* npc:bench.crew) {
      if(TestBench::ragdollBodyDrawable(*npc)) ++models;
      if(npc->isDead()) ++dead;
      if(const auto* body=npc->ragdollBody()) { parts+=uint32_t(body->partCount());if(body->isSleeping())++asleep; }
      }
    AppCadence::counts(false,models,dead,parts,asleep,bench.cadenceDown);
    }
#else
  else if(c=="cadence") {
    arenaAbort("cadence requires a build with OPENGOTHIC_ENABLE_APP_CADENCE=ON");
    }
#endif
  else if(ragdollCommand(ragdollContext(),line,a,w,pl,ctrl)) return;


  else if(routineCommand(routineContext(),a,w,pl,ctrl)) return;
  else if(c=="waitragdoll") {
    Npc* npc=a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    const std::string state=a.size()>2 ? a[2] : "";
    const float timeout=num(a,3,30000);
    if(npc==nullptr || a.size()<3 || a.size()>4 || !std::isfinite(timeout) || timeout<=0 ||
       (state!="sleep" && state!="recovery" && state!="getup" && state!="knockdown")) {
      say("waitragdoll failed: expected <npc> sleep|recovery|getup|knockdown [timeout ms]");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    bench.ragdoll.waitRagdollNpc=npc; bench.ragdoll.waitRagdollState=state;
    bench.ragdoll.waitRagdollEnd=bench.time+uint64_t(std::min(timeout,300000.f));
    }
  else if(c=="standup" && a.size()==2) {
    Npc* npc=findNpc(w,pl,a[1]);
    if(npc==nullptr || npc->isDead() || !npc->isUnconscious()) {
      say("standup failed: expected an unconscious NPC");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    npc->clearAiQueue();
    npc->setExternalControl(false);
    npc->clearState(false); // The normal ZS_Unconscious End hook may enqueue AI_StandUp.
    if(npc->isAiQueueEmpty())
      npc->aiPush(AiQueue::aiStandup());
    event("ragdoll_standup","\"actor\":"+jsonStr(TestBench::who(npc))+
          ",\"t_game_ms\":"+std::to_string(w.tickCount())+
          ",\"control\":\"normal unconscious End hook and native AI_StandUp after sleep\"");
    say("standup "+symName(w,npc));
    }


  else if(c=="ragdoll" && a.size()==2 && a[1]=="frame20") {
    if(!frameRagdollArena()) arenaAbort("twenty loaded models required before framing arena");
    }


  else if(c=="crowd" && a.size()==3) {
    const size_t instance=w.script().findSymbolIndex(a[1]);
    const float count=num(a,2,0);
    if(instance==size_t(-1) || count!=20 || !bench.crew.empty()) {
      say("crowd failed: expected one fresh twenty-NPC fixture");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    const Vec3 center=pl.position()+facing(pl)*1000.f;
    for(int i=0;i<20;++i) {
      Vec3 at=bench.ragdoll.ragdollArenaPlaces.size()==20 ? bench.ragdoll.ragdollArenaPlaces[size_t(i)] :
              center+Vec3(float(i%5)*250.f-500.f,0,float(i/5)*250.f-375.f);
      const float ground=groundY(w,at.x,at.z,at.y+200);
      if(!std::isfinite(ground)) {
        event("ragdoll_error","\"reason\":\"twenty-NPC fixture has no ground\"");
        continue;
        }
      at.y=ground;
      Npc* npc=w.addNpc(instance,at);
      if(npc==nullptr) { event("ragdoll_error","\"reason\":\"twenty-NPC fixture insertion failed\""); continue; }
      npc->setExternalControl(true); npc->clearAiQueue(); npc->clearState(false);
      npc->setDirection(pl.position()-at); npc->updateTransform();
      bench.crew.push_back(npc);
      }
    if(bench.ragdoll.ragdollArenaPlaces.size()==20 && bench.crew.size()!=20) { arenaAbort("arena insertion must retain all twenty actors"); return; }
    if(bench.crew.size()==20) {
      bench.subject=bench.crew[7];
      exec("watch watched",w,pl,ctrl);
      }
    say("crowd "+std::to_string(bench.crew.size()));
    event("ragdoll_crowd","\"actors\":"+std::to_string(bench.crew.size()));
    }
  else if(c=="down" && a.size()==3 && a[1]=="all" && (a[2]=="dead" || a[2]=="ko")) {
    size_t down=0;
    std::string bodies="[";
    for(auto* npc:bench.crew) {
      npc->setLastHitBy(&pl);
      if(npc->setRagdollDown(a[2]=="dead")) ++down;
      npc->prepareRagdoll();
      if(bodies.size()>1) bodies+=",";
      const auto* body=npc->ragdollBody();
      const auto* skeleton=npc->visualSkeleton();
      bodies+="{\"actor_id\":"+std::to_string(w.npcId(npc))+",\"symbol\":"+jsonStr(symName(w,npc))+
              ",\"skeleton\":"+jsonStr(skeleton!=nullptr ? skeleton->name() : std::string_view())+
              ",\"part_count\":"+std::to_string(body!=nullptr ? body->partCount() : 0)+",\"bone_indices\":[";
      if(body!=nullptr)
        for(size_t i=0;i<body->boneIndices().size();++i)
          bodies+=(i>0 ? "," : "")+std::to_string(body->boneIndices()[i]);
      bodies+="]}";
      }
    event("ragdoll_down_bulk","\"actors\":"+std::to_string(bench.crew.size())+",\"down\":"+std::to_string(down)+",\"bodies\":"+bodies+"]");
    say("down all "+std::to_string(down));
    }
  else if(c=="down") {
    Npc* npc=a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    if(a.size()!=3 || npc==nullptr || (a[2]!="dead" && a[2]!="ko")) {
      say("down failed: expected down <npc> dead|ko: "+line);
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    const bool inAirBefore=npc->isInAir();
    if(npc!=&pl)
      npc->setLastHitBy(&pl);
    npc->setRagdollDown(a[2]=="dead");
    npc->prepareRagdoll();
    say("down "+symName(w,npc)+" "+a[2]+(npc->isDown() ? "" : " failed"));
    event("ragdoll_down","\"actor\":"+jsonStr(TestBench::who(npc))+",\"death\":"+
          (a[2]=="dead" ? "true" : "false")+",\"down\":"+(npc->isDown() ? "true" : "false")+
          ",\"in_air_before\":"+(inAirBefore ? "true" : "false")+
          ",\"parts\":"+std::to_string(npc->ragdollBody()!=nullptr ? npc->ragdollBody()->partCount() : 0));
    }
  else if(c=="push") {
    Npc* npc=a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    const Vec3 direction(num(a,2,0),num(a,3,0),num(a,4,0));
    const float magnitude=num(a,5,0), length=direction.length();
    const bool numbers=a.size()==6 && isNumber(a[2]) && isNumber(a[3]) && isNumber(a[4]) && isNumber(a[5]);
    if(!numbers || npc==nullptr || npc->ragdollBody()==nullptr || !std::isfinite(length) || length<1e-6f ||
       !std::isfinite(magnitude) || magnitude<0) {
      say("push failed: expected a physical body and finite direction/impulse: "+line);
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    const Vec3 impulse=direction*(magnitude/length);
    npc->addRagdollImpulse(impulse);
    say("push "+symName(w,npc)+" impulse="+ragdollVector(impulse));
    event("ragdoll_push","\"actor\":"+jsonStr(TestBench::who(npc))+",\"impulse_ns\":"+ragdollVector(impulse));
    }
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if(c=="world") {
    if(a.size()<2 || a.size()>3 || !Resources::hasWorld(a[1])) {
      arenaAbort("world expects an indexed ZEN and an optional waypoint");
      return;
      }
    say("world change requested: "+a[1]);
    w.triggerChangeWorld(a[1],a.size()==3 ? std::string_view(a[2]) : std::string_view{});
    // Yield until the next native tick schedules loading; tick() then waits for its completion.
    bench.waitUntil = bench.time+1;
    }
  else if(c=="world-check") {
    if(a.size()!=2 || !std::equal(w.name().begin(),w.name().end(),a[1].begin(),a[1].end(),[](char x,char y) {
        return std::toupper(static_cast<unsigned char>(x))==std::toupper(static_cast<unsigned char>(y));
        })) {
      arenaAbort("world-check did not reach the requested world");
      return;
      }
    say("world check passed: "+a[1]);
    event("world_check","\"world\":"+jsonStr(w.name())+",\"pass\":true");
    }
  else if(c=="perf") {
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE && defined(OPENGOTHIC_ENABLE_APP_CADENCE)
    if(a.size()==3 && a[1]=="sample" && a[2].size()<64 && AppCadence::normalClock()) {
      if(!bench.perfArmed) {
        if(!AppCadence::arm(false)) {arenaAbort("perf sample could not arm app cadence");return;}
        bench.perfArmed=true;
        }
      bench.perfSegment=a[2];bench.perfFrame=0;
      bench.streamingTexturesBefore=Resources::textureStats();
      if(auto* residency=w.chunkResidency()) {
        bench.streamingNearBefore=residency->stats().transitionsNear;
        bench.streamingFarBefore=residency->stats().transitionsFar;
        bench.streamingPublishedBefore=residency->stats().published;
        bench.streamingReleasedBefore=residency->stats().released;
        bench.streamingVobCreatedBefore=residency->stats().vobCreated;
        bench.streamingVobReleasedBefore=residency->stats().vobReleased;
        bench.streamingVobMissingBefore=residency->stats().vobNearMissingSeconds;
        bench.streamingNpcTransitionsBefore=w.npcSimTierStats().toFull+w.npcSimTierStats().toSimplified;
        }
      FrameProfile::setActors(bench.crew);
      event("perf_segment","\"segment\":"+jsonStr(bench.perfSegment)+
        ",\"camera_cm\":"+ragdollVector(bench.camPos)+",\"actors\":"+std::to_string(bench.crew.size())+
        ",\"residency_manager\":"+(w.chunkResidency()!=nullptr ? "true" : "false"));
      }
    else if(a.size()==2 && a[1]=="stop") {
      if(bench.perfSegment.starts_with("streaming-flythrough/")) {
        const auto textures=Resources::textureStats();
        char textureSeconds[48];
        std::snprintf(textureSeconds,sizeof(textureSeconds),"%.9g",textures.nearTailSeconds-bench.streamingTexturesBefore.nearTailSeconds);
        event("streaming_end","\"segment\":"+jsonStr(bench.perfSegment)+
          ",\"gpu_mb\":"+std::to_string(double(tempestProbeGpuBytes())/1048576.0)+
          ",\"tex_promoted\":"+std::to_string(textures.promoted-bench.streamingTexturesBefore.promoted)+
          ",\"tex_demoted\":"+std::to_string(textures.demoted-bench.streamingTexturesBefore.demoted)+
          ",\"tex_near_tail_seconds\":"+textureSeconds);
        }
      bench.perfSegment.clear();
      }
    else arenaAbort("perf expects sample <segment under 64 bytes> or stop, after cadence normal");
#else
    arenaAbort("perf requires OPENGOTHIC_ENABLE_FRAME_PROFILE=ON and OPENGOTHIC_ENABLE_APP_CADENCE=ON");
#endif
    }
  else if(c=="streaming") {
    if(a.size()<2 || a.size()>3 || a[1]!="wait" || (a.size()==3 && !isNumber(a[2]))) {
      arenaAbort("streaming expects wait [timeout_ms]");
      return;
      }
    const float timeout=num(a,2,60000);
    if(!std::isfinite(timeout) || timeout<=0 || timeout>300000) {
      arenaAbort("streaming wait timeout must be in (0, 300000] ms");
      return;
      }
    bench.waitStreaming=true;
    bench.streamingDeadline=std::chrono::steady_clock::now()+
      std::chrono::milliseconds(uint64_t(timeout));
    // Camera and desired residency are updated after this bench tick.
    bench.waitUntil=bench.time+1;
    }
  else if(c=="deterministic") {
    if(a.size()!=2 || (a[1]!="on" && a[1]!="off")) {
      arenaAbort("deterministic expects on or off");
      return;
      }
    bench.deterministic=a[1]=="on";
    benchTexturesSettle=bench.deterministic;
    benchTexturesFrozen=false;
    say("deterministic "+a[1]);
    }
  else if(c=="wait") {
    if(a.size()>1 && a[1]=="real")
      bench.realWaitUntil=std::chrono::steady_clock::now()+std::chrono::milliseconds(uint64_t(num(a,2,1000)));
    else
      bench.waitUntil = bench.time + uint64_t(num(a,1,1000));
    }
  else if(c=="waitarrive") {
    bench.waitArrive = true;
    bench.arriveEnd  = bench.time + uint64_t(num(a,1,10000));
    }
  else if(c=="say" || c=="log") {
    say(rest(line));
    }
  else if(c=="label") {
    bench.label = rest(line);
    }
  else if(c=="goto") {
    // goto <waypoint> | goto pos <x> <y> <z> (a roof, a rock: where no waypoint is)
    const bool ok = marvin().exec(a.size()>1 && a[1]=="pos" ? line : "goto waypoint " + rest(line));
    bench.hasPrev = false;
    say(std::string(ok ? "goto " : "goto failed: ") + rest(line));
    }
  else if(c=="route")beginRoute(a,w,pl);
  else if(c=="walkto") {
    auto wp = w.findPoint(rest(line));
    if(wp!=nullptr)
      pl.aiPush(AiQueue::aiGoToPoint(*wp));
    say(std::string(wp ? "walkto " : "walkto: no waypoint ") + rest(line));
    }
  else if(c=="near") {
    // near <npc> [dist cm 170] [degrees off the npc's facing 0]: the hero stands in front of the npc, facing it
    Npc* npc = a.size()>1 ? npcBySymbol(w,a[1]) : nullptr;
    if(npc==nullptr) {
      say("near: no npc " + (a.size()>1 ? a[1] : std::string()));
      return;
      }
    Vec3 at = npc->position() + rotY(facing(*npc),num(a,3,0))*num(a,2,170);
    const float g = groundY(w,at.x,at.z,at.y+100);
    if(!std::isnan(g))
      at.y = g;
    pl.setPosition(at);
    face(pl,npc->position()-at);
    bench.hasPrev = false;
    say("near " + a[1]);
    }
  else if(c=="approach") {
    // approach <npc>: walk up to the npc (AI path), then waitarrive
    if(Npc* npc = a.size()>1 ? npcBySymbol(w,a[1]) : nullptr)
      pl.aiPush(AiQueue::aiGoToNpc(npc)); else
      say("approach: no npc " + (a.size()>1 ? a[1] : std::string()));
    }
  else if(c=="walkmode") {
    const std::string m = a.size()>1 ? a[1] : "run";
    pl.setWalkMode(m=="walk" ? WalkBit::WM_Walk : m=="sneak" ? WalkBit::WM_Sneak : WalkBit::WM_Run);
    }
  else if(c=="face") {
    const std::string m = a.size()>1 ? a[1] : "";
    float an = 0, r = 0;
    Vec3  d;
    if(m=="uphill" || m=="downhill" || m=="across") {
      if(slopeAt(w,pl.position(),an,d,r))
        face(pl, m=="uphill" ? d*-1.f : m=="downhill" ? d : Vec3(d.z,0,-d.x));
      }
    else if(m=="wp" && a.size()>2) {
      if(auto wp = w.findPoint(a[2]))
        face(pl,wp->pos-pl.position());
      }
    else if(m=="npc" && a.size()>2) {
      if(auto npc = npcBySymbol(w,a[2]))
        face(pl,npc->position()-pl.position()); else
        say("face: no npc " + a[2]);
      }
    else if(m=="rel") {
      // face rel <deg>: along the facing of the watched npc turned by that much (the angles of place)
      const float y = (subjectOf(pl).rotation()+num(a,2,0))*float(M_PI)/180.f;
      face(pl,Vec3(std::cos(y),0,std::sin(y)));
      }
    else {
      const float y = num(a,1,0)*float(M_PI)/180.f;
      face(pl,Vec3(std::cos(y),0,std::sin(y)));
      }
    }
  else if(c=="turn") {
    const float y = (pl.rotation()+num(a,1,90))*float(M_PI)/180.f;
    face(pl,Vec3(std::cos(y),0,std::sin(y)));
    }
  else if(c=="wheel") {
    // the mouse wheel, as the window gets it: -1 down, 1 up
    ctrl.onMouseWheel(int(num(a,1,-1)));
    }
  else if(c=="press" || c=="release" || c=="tap" || c=="hold") {
    if(c=="release" && a.size()>1 && a[1]=="all") {
      for(auto& h:bench.holds)
        release(ctrl,h.a);
      bench.holds.clear();
      return;
      }
    KeyCodec::Action k = KeyCodec::Idle;
    if(a.size()<2 || !keyAction(a[1],k)) {
      say("unknown key: " + line);
      return;
      }
    if(c=="press") {
      press(ctrl,k);
      bench.holds.push_back({k,uint64_t(-1)});
      }
    else if(c=="release") {
      if((k==KeyCodec::Heal || k==KeyCodec::Potion) && !bench.healing.test.label.empty() && !bench.healing.test.hotkeyAt)
        bench.healing.test.hotkeyAt=w.tickCount();
      release(ctrl,k);
      bench.holds.erase(std::remove_if(bench.holds.begin(),bench.holds.end(),[k](const Hold& h){ return h.a==k; }),bench.holds.end());
      }
    else if(c=="tap") {
      press(ctrl,k);
      bench.taps.push_back(k);
      }
    else {
      press(ctrl,k);
      const auto until = bench.time+uint64_t(num(a,2,1000));
      bench.holds.push_back({k,until});
      if(k==KeyCodec::ActionGeneric || k==KeyCodec::Forward)
        bench.inputTraceUntil = until+2000;
      }
    healingObserve(bench.healing,w,pl);
    }
  else if(c=="cam") {
    const std::string m = a.size()>1 ? a[1] : "";
    bench.camAuto = false;
    if(m!="shoulder" && m!="face" && m!="two" && m!="smooth" && m!="arrow")
      bench.smValid = false;
    if(m=="game") {
      bench.cam = C_Game;
      // the game's camera as the player has it: without the near clip `view clean` gives the bench's shots
      // (130 cm after `cam follow 230`: the hero was clipped away, the aim camera's collision rays hit
      // the ground under him)
      if(auto cam = Gothic::inst().camera()) {
        cam->setMarvinMode(Camera::M_Normal);
        cam->setNearClip(10);
        }
      }
    else if(m=="follow" || m=="front" || m=="auto") {
      bench.cam       = C_Follow;
      bench.camDist   = num(a,2,220);
      bench.camHeight = num(a,3,40);
      bench.camAngle  = m=="front" ? 0 : num(a,4,90);
      if(m=="auto") {
        bench.camAuto = true;
        pickCamAngle(w,subjectOf(pl));
        }
      }
    else if(m=="world") {
      bench.cam       = C_World;
      bench.camDist   = num(a,2,220);
      bench.camHeight = num(a,3,40);
      bench.camAngle  = num(a,4,0);
      }
    else if(m=="fixed") {
      bench.cam = C_Fixed;
      }
    else if(m=="hold") {
      bench.cam = C_Hold;  // a shot of a scene: the subject may fall out of it, the frame stays
      }
    else if(m=="shoulder" || m=="two" || (m=="face" && a.size()>2 && !isNumber(a[2]))) {
      // cam shoulder <npc> [dist 120] [height 15] [shoulder 1|-1]
      // cam face <npc> [dist 140] [height 0] [degrees from the hero 30]
      // cam two <npc> [dist 250] [height 10] [side 1|-1]
      if(a.size()<3) {
        say("cam " + m + ": no npc");
        return;
        }
      bench.cam       = m=="shoulder" ? C_Shoulder : (m=="face" ? C_NpcFace : C_Two);
      bench.camNpc    = a[2];
      bench.camDist   = num(a,3,m=="shoulder" ? 120 : (m=="face" ? 140 : 250));
      bench.camHeight = num(a,4,m=="shoulder" ? 15 : (m=="face" ? 0 : 10));
      bench.camAngle  = num(a,5,m=="face" ? 30 : 1);
      if(bench.camNpc!="arena" && npcBySymbol(w,bench.camNpc)==nullptr)
        say("cam " + m + ": no npc " + bench.camNpc);
      }
    else if(m=="arrow") {
      // cam arrow [dist 600] [height 40] [side 1|-1] [ahead 80]: beside the newest arrow's way (overlay trail
      // records it), from the hero until one flies; glides by `cam smooth`
      bench.cam       = C_Arrow;
      bench.camDist   = num(a,2,600);
      bench.camHeight = num(a,3,40);
      bench.camAngle  = num(a,4,1);
      bench.camLook   = num(a,5,80);
      }
    else if(m=="cut") {
      bench.smValid  = false;  // the next shot starts where it is, without moving there
      bench.cutPending = true; // and TAA starts its history over there (lookFrom)
      }
    else if(m=="smooth") {
      bench.camSmooth = std::clamp(num(a,2,0.12f),0.01f,1.f);
      }
    else if(m=="face") {
      // cam face <dist> [angle] [height]: close to the face of the watched one, looking at the mouth
      bench.cam       = C_Face;
      bench.camDist   = num(a,2,60);
      bench.camAngle  = num(a,3,0);
      bench.camHeight = num(a,4,0);
      bench.camLook   = 5;
      }
    else if(m=="look") {
      bench.camLook = num(a,2,45);
      }
    else if(m=="lift") {
      bench.camLift = !(a.size()>2 && a[2]=="off");
      }
    else {
      say("unknown cam mode: " + line);
      }
    }
  else if(c=="view") {
    const std::string m = a.size()>1 ? a[1] : "full";
    // Static and Movable objects share draw commands (same shaders), hiding them by type hides
    // parts of creatures too: what stands between the camera and the subject is clipped instead
    bench.clip = 0;
    DrawCommands::hiddenTypes = 0;
    if(m=="clean") {
      bench.clip = num(a,2,100);
      DrawCommands::hiddenTypes = (1u<<DrawCommands::Pfx);
      }
    else if(m=="nopfx")
      DrawCommands::hiddenTypes = (1u<<DrawCommands::Pfx);
    else if(m=="hide") {
      // view hide static movable morph pfx ...
      DrawCommands::hiddenTypes = 0;
      for(size_t i=2; i<a.size(); ++i) {
        const std::string& t = a[i];
        if(t=="static")   DrawCommands::hiddenTypes |= (1u<<DrawCommands::Static);
        if(t=="movable")  DrawCommands::hiddenTypes |= (1u<<DrawCommands::Movable);
        if(t=="morph")    DrawCommands::hiddenTypes |= (1u<<DrawCommands::Morph);
        if(t=="pfx")      DrawCommands::hiddenTypes |= (1u<<DrawCommands::Pfx);
        if(t=="animated") DrawCommands::hiddenTypes |= (1u<<DrawCommands::Animated);
        }
      }
    if(bench.clip<=0)
      if(auto cam = Gothic::inst().camera())
        cam->setNearClip(10);
    }
  else if(c=="overlay") {
    const std::string m = a.size()>1 ? a[1] : "on";
    const bool on = !(a.size()>2 && a[2]=="off");
    if(m=="zoom")
      bench.zoom = on;
    else if(m=="bright")
      bench.bright = on;
    else if(m=="zones") {
      bench.zonesOnly = on; // the hit zones only, no legs, no text
      bench.overlay   = bench.overlay || on;
      }
    else if(m=="trail") {
      // overlay trail [off] [keep ms] [line]: the arrows' ways only (with the zones of showzones), no legs, no
      // text; `line`: also the straight line it left along (where the bow pointed), dashed
      bench.trailOn   = on;
      bench.trailKeep = a.size()>2 && a[2]!="off" && a[2]!="line" ? uint64_t(num(a,2,1500)) : bench.trailKeep;
      bench.trailLine = on && std::find(a.begin(),a.end(),"line")!=a.end();
      bench.overlay   = bench.overlay || on;
      if(!on)
        bench.trails.clear();
      }
    else
      bench.overlay = (m!="off");
    }
  else if(c=="diary") {
    // diary chronicle [feat] [open]: opens the diary on its «Летопись» tab, with that feat selected
    // (counting the other fates' ones after this fate's), shown in full with `open`. The game pauses in
    // the diary and the bench with it: shoot the window from outside (screencapture -l).
    GameMenu::benchDiary.chronicle = a.size()>1 && a[1]=="chronicle";
    GameMenu::benchDiary.feat      = int32_t(num(a,2,-1));
    GameMenu::benchDiary.open      = a.size()>3 && a[3]=="open";
    if(TestBench::openMenu)
      TestBench::openMenu("MENU_LOG"); else
      say("diary: no menu hook");
    }
  else if(c=="ui") {
    bench.ui = !(a.size()>1 && a[1]=="off");
    }
  else if(c=="slowmo") {
    // slowmo <k>: the game runs k times slower than the video (1: normal); the game's sound is not recorded
    // while slowed (silence keeps the sound in step with the picture)
    bench.slow    = std::clamp(num(a,1,1),1.f,20.f);
    bench.slowAcc = 0;
    say("slowmo " + std::to_string(bench.slow));
    }
  else if(c=="track") {
    // track <npc[,npc...]|off> [head|body]: one ordinary track row per actor and recorded frame.
    bench.trackNpcs.clear();
    if(a.size()>1 && a[1]!="off") {
      size_t begin=0;
      while(begin<a[1].size()) {
        const size_t end=a[1].find(',',begin);
        const auto symbol=a[1].substr(begin,end==std::string::npos ? end : end-begin);
        if(!symbol.empty() && std::find(bench.trackNpcs.begin(),bench.trackNpcs.end(),symbol)==bench.trackNpcs.end())
          bench.trackNpcs.push_back(symbol);
        if(end==std::string::npos) break;
        begin=end+1;
        }
      }
    bench.trackHead = !(a.size()>2 && a[2]=="body");
    say("track " + (bench.trackNpcs.empty() ? std::string("off") : a[1]));
    }
  else if(c=="sight") {
    // sight <npc|off> [fan cm]: each recorded frame and shot, the hero's line of sight to it on the picture
    // (events.jsonl "sight"): the shared standing head, the current head, and the minimum lean that reveals
    // the target; the fan of the bow's focus down (FOCUS_RANGED elevdo)
    bench.sightNpc = a.size()>1 && a[1]!="off" ? a[1] : "";
    bench.sightFan = num(a,2,700);
    say("sight " + (bench.sightNpc.empty() ? std::string("off") : bench.sightNpc));
    }
  else if(c=="rec") {
    if(ragdollPerf && a.size()>1 && a[1].starts_with("twenty_") &&
       !arenaFullSchema(w,std::string_view(a[1]).substr(7))) {
      arenaAbort("refusing timing without twenty complete visible bodies"); return;
      }
    stopRecording();
    bench.rec      = true;
    bench.recVideo = !(a.size()>2 && a[2]=="novideo");
    bench.recName  = a.size()>1 ? a[1] : "rec";
    bench.recFrame = 0;
    bench.recStart = bench.time;
    bench.anim1.clear();
    bench.anim2.clear();
    bench.recWall  = std::chrono::steady_clock::now();
    if(bench.recVideo && SoundDevice::loopbackRate()>0) {
      bench.audio       = std::fopen((outDir+"/"+bench.recName+".wav").c_str(),"wb");
      bench.audioFrames = 0;
      if(bench.audio!=nullptr)
        writeWavHeader(bench.audio,SoundDevice::loopbackRate(),0);
      }
    event("rec","\"fps\":" + std::to_string(Fps) + ",\"audio\":" + (bench.audio!=nullptr ? "true" : "false"));
    say("rec " + bench.recName + (bench.recVideo ? "" : " (metrics only)") + (bench.audio!=nullptr ? " with sound" : ""));
    }
  else if(c=="stop") {
    if(a.size()==2 && a[1]=="shadow") {
      // Preserve each duel before the ordinary 8 MiB shadow journal rotates again.
      for(const auto* suffix:{".jsonl",".previous.jsonl"}) {
        const std::string source="melee-blade-shadow"+std::string(suffix);
        if(std::filesystem::exists(source))
          std::filesystem::copy_file(source,outDir+"/"+bench.recName+"-"+source,
                                     std::filesystem::copy_options::overwrite_existing);
        }
      }
    stopRecording();
    }
  else if(c=="shot") {
    bench.shot = a.size()>1 ? a[1] : "shot";
    }
  else if(c=="anims") {
    // anims: what the watched one (or the hero) plays, its body state, weapon and target
    Npc& s = subjectOf(pl);
    char buf[160];
    std::snprintf(buf,sizeof(buf),"anims %s bs=%u ws=%d target=%s: ",symName(w,&s).c_str(),unsigned(s.bodyStateMasked() & BS_MAX),
                  int(s.weaponState()),symName(w,s.target()).c_str());
    say(buf + s.animNames());
    }
  else if(c=="cansee") {
    // cansee <npc|nearest> [edge yaw]: visibility and the head/arrow reach toward the edge, in cm from the feet
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("cansee: no npc " + line);
      return;
      }
    const Vec3  tg   = n->collosionCenter();
    const Vec3  head = pl.aimHead(tg,0.f);
    const auto  lean = pl.aimLean(pl.hasManualAim() ? pl.manualAimPoint() : tg,pl.hasManualAim());
    const Vec3  d    = tg-head;
    const float h    = std::sqrt(d.x*d.x+d.z*d.z);
    auto        r    = w.physic()->ray(head,tg);
    const auto& fr = w.script().focusRange();
    char buf[800];
    std::snprintf(buf,sizeof(buf),"cansee %s: head %d, over the edge %d; dist %.0f, down %.0f deg; the ray from the head hits at %.0f cm of %.0f; bow focus %.0f..%.0f",
                  symName(w,n).c_str(),int(!r.hasCol),int(pl.canSeeNpcOverEdge(*n)),double(d.length()),
                  double(-std::atan2(d.y,h)*180.f/float(M_PI)),r.hasCol ? double((r.v-head).length()) : -1.0,double(d.length()),
                  double(fr.npc_range1),double(fr.npc_range2));
    say(buf);
    const Vec3  dir  = Vec3::normalize(Vec3(d.x,0,d.z));
    const Vec3  eye  = pl.aimHead(tg,lean.angle);
    const Vec3  full = pl.aimHead(tg,Aim::LeanMax*float(M_PI)/180.f);
    auto        r2   = w.physic()->ray(eye,tg);
    const float degrees = lean.angle*180.f/float(M_PI);
    const float before = std::max(0.f,degrees-0.25f);
    const Vec3  prior = pl.aimHead(tg,before*float(M_PI)/180.f);
    const bool  priorSeen = !w.physic()->ray(prior,tg).hasCol;
    std::snprintf(buf,sizeof(buf),"  leaned head: required_deg %.2f current_deg %.2f line %s visible %d room %d edge %d hit_cm %.2f shot_clear %d; to it %d (hit at %.0f of %.0f), before %.2f deg seen %d; max_forward_cm %.1f from feet, head at %.0f above the feet",
                  double(degrees),double(Aim::playerProbe.lean),leanLineName(lean.line),int(lean.visible),int(lean.room),
                  int(lean.edge),double(lean.hitDistance),int(lean.shotClear),
                  int(!r2.hasCol),r2.hasCol ? double((r2.v-eye).length()) : -1.0,double((tg-eye).length()),double(before),int(priorSeen),
                  double(Vec3::dotProduct(full-pl.position(),dir)),double(eye.y-pl.position().y));
    say(buf);
    const Vec3 actual = pl.mapHeadBone();
    const Vec3 predicted = pl.aimCurrentHead();
    std::snprintf(buf,sizeof(buf),"  actual head: visible %d prediction_error_cm %.2f",
                  int(!w.physic()->ray(actual,tg).hasCol),double((actual-predicted).length()));
    say(buf);
    const float yaw = num(a,2,pl.rotation());
    const float rad = yaw*float(M_PI)/180.f;
    const Vec3 edgeDir(std::cos(rad),0,std::sin(rad));
    Vec3 arrow = {}, arrowDir = {};
    const bool hasArrow = pl.arrowRay(arrow,arrowDir);
    const Vec3 fullArrow = pl.aimArrow(tg,Aim::LeanMax*float(M_PI)/180.f);
    std::snprintf(buf,sizeof(buf),"  edge reach yaw %.1f: current head_forward_cm %.2f arrow_forward_cm %.2f arrow_valid %d head (%.2f %.2f %.2f) arrow (%.2f %.2f %.2f); full70 head_forward_cm %.2f arrow_forward_cm %.2f head (%.2f %.2f %.2f) arrow (%.2f %.2f %.2f) feet (%.2f %.2f %.2f)",
                  double(yaw),double(Vec3::dotProduct(actual-pl.position(),edgeDir)),
                  hasArrow ? double(Vec3::dotProduct(arrow-pl.position(),edgeDir)) : -1.0,int(hasArrow),
                  double(actual.x),double(actual.y),double(actual.z),double(arrow.x),double(arrow.y),double(arrow.z),
                  double(Vec3::dotProduct(full-pl.position(),edgeDir)),double(Vec3::dotProduct(fullArrow-pl.position(),edgeDir)),
                  double(full.x),double(full.y),double(full.z),double(fullArrow.x),double(fullArrow.y),double(fullArrow.z),
                  double(pl.position().x),double(pl.position().y),double(pl.position().z));
    say(buf);
    std::snprintf(buf,sizeof(buf),"  directional lean: edge_cm %.2f drop_cm %.2f support_y %.2f head_clear %d shot_clear %d head_hit_cm %.2f shot_hit_cm %.2f limited %d room_limited %d",
                  double(lean.edgeDistance),double(std::isfinite(lean.edgeDrop) ? lean.edgeDrop : -1.f),double(lean.edgeHeight),int(lean.headClear),int(lean.shotClear),
                  double(lean.headHitDistance),double(lean.shotHitDistance),int(lean.limited),int(lean.roomLimited));
    say(buf);
    if(hasArrow && pl.hasManualAim()) {
      const Vec3 look = Vec3::normalize(pl.aimLook());
      const Vec3 lookFlat = Vec3::normalize(Vec3(look.x,0,look.z));
      const float edgeProjection = lean.edgeDistance*Vec3::dotProduct(lookFlat,edgeDir);
      const auto headRay = w.physic()->ray(actual,actual+look*200.f);
      const auto shotRay = w.physic()->ray(arrow,arrow+Vec3::normalize(arrowDir)*200.f);
      std::snprintf(buf,sizeof(buf),"  actual edge rays: head_over_edge_cm %.2f arrow_over_edge_cm %.2f head_hit_cm %.2f at (%.2f %.2f %.2f) arrow_hit_cm %.2f at (%.2f %.2f %.2f)",
                    double(Vec3::dotProduct(actual-pl.position(),edgeDir)-edgeProjection),
                    double(Vec3::dotProduct(arrow-pl.position(),edgeDir)-edgeProjection),
                    headRay.hasCol ? double((headRay.v-actual).length()) : -1.0,
                    double(headRay.v.x),double(headRay.v.y),double(headRay.v.z),
                    shotRay.hasCol ? double((shotRay.v-arrow).length()) : -1.0,
                    double(shotRay.v.x),double(shotRay.v.y),double(shotRay.v.z));
      say(buf);
      }
    }
  else if(c=="views") {
    // views <npc> [count] [max dist] [from <min dist>] [shoot <prefix>]: where a camera sees the hero (the top of
    // the head, the chest: a wall's top may hide the legs) and the npc in one frame, from open air (nothing within
    // 40 cm, a clear line to the hero's head: the same room), as `cam lift off`, `cam follow <dist> <height>
    // <angle>`, `cam look <look>` (halfway between them) put it; the nearest of each side first, no nearer than
    // `from`. For a scene on a roof, in a hall or under a gallery, where a camera put by hand lands in a wall
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("views: no npc " + line);
      return;
      }
    const size_t count = size_t(num(a,2,10));
    const float  maxD  = num(a,3,900);
    float        minD  = 150;
    std::string  prefix;
    for(size_t i=4; i+1<a.size(); i+=2) {
      if(a[i]=="from")
        minD = num(a,i+1,150);
      else if(a[i]=="shoot")
        prefix = a[i+1];
      }
    const Vec3   p     = pl.position();
    const Vec3   top   = pl.mapHeadBone() + Vec3(0,20,0);
    const Vec3   chest = pl.mapHeadBone() - Vec3(0,50,0);
    const Vec3   tg    = n->centerPosition();
    const float  look  = std::round(((top.y+tg.y)*0.5f - p.y)/10.f)*10.f;
    const Vec3   at    = Vec3(p.x,p.y+look,p.z);
    auto         cam   = Gothic::inst().camera();
    const float  half  = (cam!=nullptr ? cam->fovY() : 67.5f)*0.5f*0.9f; // a margin at the frame's edge
    const Vec3   f     = facing(pl);
    const Vec3   sd    = {-f.z,0,f.x};
    auto         clear = [&](const Vec3& x, const Vec3& y) { return !w.physic()->ray(x,y).hasCol; };
    struct View { float dist, height, angle, spread; };
    std::vector<View> found;
    for(float dist=minD; dist<=maxD; dist+=50)
      for(float ang=-165; ang<=180; ang+=15)
        for(float hgt=-700; hgt<=700; hgt+=50) {
          const float b   = ang*float(M_PI)/180.f;
          const Vec3  org = p + (f*std::cos(b) + sd*std::sin(b))*dist + Vec3(0,hgt,0);
          if(!clear(top,org) || !clear(org,chest) || !clear(org,tg))
            continue;
          bool open = true;
          for(const Vec3 d:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,40,0),Vec3(0,-40,0),Vec3(0,0,40),Vec3(0,0,-40)})
            open = open && clear(org,org+d);
          if(!open)
            continue;
          // up and down from where it looks: both within the field of view
          const Vec3 v = Vec3::normalize(at-org);
          auto angTo = [&](const Vec3& q) {
            return std::acos(std::clamp(Vec3::dotProduct(v,Vec3::normalize(q-org)),-1.f,1.f))*180.f/float(M_PI);
            };
          const float s = std::max({angTo(top),angTo(chest),angTo(tg)});
          if(s>half)
            continue;
          found.push_back({dist,hgt,ang,s});
          }
    std::sort(found.begin(),found.end(),[](const View& x, const View& y) {
      return x.dist!=y.dist ? x.dist<y.dist : std::abs(x.height)<std::abs(y.height);
      });
    char buf[200];
    std::snprintf(buf,sizeof(buf),"views %s: %zu spots, cam look %.0f; field of view %.0f deg",symName(w,n).c_str(),
                  found.size(),double(look),double(half*2.f/0.9f));
    say(buf);
    // the nearest of each side (angle): different shots, not ten of the same; `shoot <prefix>`: a shot from each
    // goes into the scenario right after this line
    const bool         shoot  = !prefix.empty();
    std::vector<float> sides;
    std::vector<std::string> shots;
    for(auto& v:found) {
      if(sides.size()>=count || std::find(sides.begin(),sides.end(),v.angle)!=sides.end())
        continue;
      sides.push_back(v.angle);
      std::snprintf(buf,sizeof(buf),"  cam follow %.0f %.0f %.0f  (both within %.0f deg of the middle)",
                    double(v.dist),double(v.height),double(v.angle),double(v.spread));
      say(buf);
      if(!shoot)
        continue;
      char cmd[120];
      std::snprintf(cmd,sizeof(cmd),"cam follow %.0f %.0f %.0f",double(v.dist),double(v.height),double(v.angle));
      shots.push_back(cmd);
      shots.push_back("cam look " + std::to_string(int(look)));
      shots.push_back("wait 300");
      std::snprintf(cmd,sizeof(cmd),"shot %s%.0f_%.0f_%.0f",prefix.c_str(),double(v.dist),double(v.height),double(v.angle));
      shots.push_back(cmd);
      shots.push_back("wait 100");
      }
    if(!shots.empty()) {
      shots.insert(shots.begin(),"cam lift off");
      bench.prog.insert(bench.prog.begin()+std::ptrdiff_t(bench.pc),shots.begin(),shots.end());
      }
    }
  else if((c=="hp" || c=="give" || c=="equip" || c=="disarm" || c=="draw" || c=="put" || c=="put-at" || c=="pin" || c=="npcgo") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if(c=="bowstrength" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="sfx") {
    // sfx <name>...: the files a sound effect plays (its variants _A1.. too), whether they load
    for(size_t k=1; k<a.size(); ++k) {
      std::string out = "sfx " + a[k] + ":";
      for(int v=0; v<10; ++v) {
        const std::string name = v==0 ? a[k] : a[k]+"_A"+std::to_string(v);
        auto& d = Gothic::sfx()[name];
        if(d.file.empty())
          break;
        out += " " + d.file + (Resources::loadSoundBuffer(d.file).isEmpty() ? " (missing)" : "") +
               " vol " + std::to_string(d.vol);
        }
      say(out);
      }
    }
  else if(c=="animspeed") {
    // animspeed <anim> <k>: the hero's animation of that name plays k times as fast from its next start
    // (the reload's tempo is set by the manual aim from dexterity: this overrides it until then)
    if(a.size()<3) {
      say("animspeed: <anim> <k>");
      return;
      }
    pl.setAnimSpeed(a[1],num(a,2,1));
    say("animspeed " + a[1] + " " + a[2]);
    }
  else if((c=="bowdraw" || c=="bowrelease") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="showzones") {
    // showzones <npc symbol>|player|off: the overlay draws the npc's hit zones (capsules on its bones: the head
    // red, the trunk yellow, the limbs blue)
    bench.zonesNpc = a.size()>1 && a[1]!="off" ? a[1] : "";
    say("showzones " + (bench.zonesNpc.empty() ? std::string("off") : bench.zonesNpc));
    }
  else if(c=="animinfo") {
    // animinfo <name>...: the hero's animations of those names (frames, the nodes, how far they go, the
    // hips)
    for(size_t i=1; i<a.size(); ++i)
      say("animinfo " + pl.animInfo(a[i]));
    }
  else if(c=="bladepath") {
    // bladepath <name>|<overlay.MDS>...: the tip of the hero's weapon at each frame of those animations (model
    // frame); an overlay (HUMANS_1HST2.MDS) is put on for the names after it
    const auto* weapon = pl.currentMeleeWeapon();
    const auto* mesh = weapon!=nullptr ? Resources::loadMesh(weapon->handle().visual) : nullptr;
    const float length = mesh!=nullptr ? -mesh->bbox()[0].x : 0.f;
    for(size_t i=1; i<a.size(); ++i)
      if(a[i].size()>4 && a[i].substr(a[i].size()-4)==".MDS")
        pl.addOverlay(a[i],0); else
        say("bladepath " + pl.bladePath(a[i],length));
    }
  else if(c=="aimwalk" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="bowgeom") {
    // bowgeom: the bow and the bow hand in the arrow's frame, as posed now
    say(pl.bowDump());
    }
  else if((c=="stats" || c=="npcday" || c=="seed") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if(npcSimCommand(bench.npcDuel,bench.npcTierTest,bench.npcSpeedTest,bench.pc,bench.prog,a,w,pl)) return;
  else if(bodyCommand(bodyContext(),a,w,pl)) return;
  else if(c=="knockdown-launch-equip") {
    auto* npc=findNpc(w,pl,"SLD_826_SOELDNER");
    const auto weapon=w.script().findSymbolIndex("ITMW_2H_SLD_AXE");
    if(npc==nullptr || weapon==size_t(-1)) {arenaAbort("knockdown launch actors or weapon missing");return;}
    npc->setToFightMode(weapon);
    if(npc->weaponState()!=WeaponState::W2H || npc->currentMeleeWeapon()==nullptr ||
       npc->currentMeleeWeapon()->clsId()!=weapon) {arenaAbort("knockdown launch requires the exact drawn two-handed axe");return;}
    event("launch_weapon","\"actor\":"+jsonStr(symName(w,npc))+",\"actor_id\":"+std::to_string(w.npcId(npc))+
          ",\"weapon\":\"ITMW_2H_SLD_AXE\",\"state\":\"W2H\"");
    }
  else if(c=="mds") {
    // mds <npc|player> <OVERLAY.MDS> [off]: an overlay of the model script on it or off (a gait: HUMANS_MILITIA.MDS)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr || a.size()<3) {
      say("mds: no npc or overlay " + line);
      return;
      }
    if(a.size()>3 && a[3]=="off")
      n->delOverlay(a[2]); else
      n->addOverlay(a[2],0);
    say("mds " + symName(w,n) + " " + a[2] + (a.size()>3 ? " " + a[3] : ""));
    }
  else if(c=="quiver") {
    // quiver place <deg behind the right side> <min push> <tilt back> <tilt out> [tilt right] [height] [side] |
    // quiver place off | quiver still on|off: a probe of other places (research.md of the quiver report),
    // the hero and the npcs fitted anew; still: held at rest, no swing
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="still") {
      Quiver::still = a.size()>2 && a[2]=="on";
      say(std::string("quiver still ") + (Quiver::still ? "on" : "off"));
      return;
      }
    if(m!="place") {
      say("quiver: place|still " + line);
      return;
      }
    Quiver::Place p;
    if(a.size()>2 && a[2]!="off") {
      p.on        = true;
      p.around    = num(a,2,0);
      p.minPush   = num(a,3,Quiver::MinOut);
      p.tiltBack  = num(a,4,Quiver::TiltBack);
      p.tiltOut   = num(a,5,Quiver::TiltOut);
      p.tiltRight = num(a,6,0);
      p.height    = num(a,7,Quiver::MouthHeight);
      p.side      = num(a,8,0);
      p.back      = 0;
      }
    Quiver::place = p;
    w.detectNpc(pl.position(),100000.f,[](Npc& n) { n.refitQuiver(); });
    pl.refitQuiver();
    const auto q = pl.quiverProbe();
    char buf[160];
    std::snprintf(buf,sizeof(buf),"quiver place %s out %.1f mouth %.2f %.2f %.2f",p.on ? "on" : "off",double(q.out),
                  double(q.mouth.x),double(q.mouth.y),double(q.mouth.z));
    say(buf);
    }
  else if(c=="arrows" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if((c=="npcequip" || c=="wander" || c=="stuck" || c=="fight") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if(c=="arc" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else dispatched=false;
  if(dispatched) return;
  dispatched=true;
  if((c=="waitdown" || c=="npcaim" || c=="still" || c=="zones" || c=="open" || c=="volley" || c=="immortal" || c=="stun-drift" || c=="stun" || c=="dodge" || c=="clear" || c=="flinch" || c=="hit" || c=="strike") && npcCommandsCommand(npcCommandsContext(),line,a,w,pl,ctrl)) return;
  else if((c=="archers" || c=="archer") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="aimat" && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if((c=="aim" || c=="aimlean") && archeryCommand(archeryContext(),line,a,w,pl,ctrl)) return;
  else if(c=="lean") {
    // lean on|off: the lean into turns and on slopes (graphics/mesh/lean)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Lean::setEnabled(m=="on"); else
      say("unknown lean command: " + line);
    }
  else if(c=="blend") {
    // blend on|off, blend param <min|scale|max|turn|move|out|side|blow> <value>: transitions between animations
    // (AnimBlend)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      AnimBlend::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(AnimBlend::setParam(a[2],num(a,3,0)) ? "blend param " : "blend: unknown param ") + a[2] + " " + a[3]);
    else
      say("unknown blend command: " + line);
    }
  else if(c=="ik") {
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      FootIk::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(FootIk::setParam(a[2],num(a,3,0)) ? "ik param " : "ik: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("ik params: " + FootIk::paramList());
    else
      say("unknown ik command: " + line);
    }
  else if(c=="foreach") {
    expandForeach(bench.pc-1,w,pl);
    }
  else if(c=="end") {
    }
  else if(c=="slopes" || c=="stairs") {
    for(auto& s:findSpots(w,pl,c=="stairs" ? K_Stairs : K_Slope,num(a,1,10),num(a,2,35),"",30)) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"%s %s angle=%.1f rough=%.1f dist=%.0f",c.c_str(),s.name.c_str(),s.angle,s.rough,s.dist);
      say(buf);
      }
    }
  else if(c=="ledges") {
    // ledges [min drop] [max drop] [count] [steep]: waypoints at an edge, open ground that much lower
    // 1.5..14 m away and seen from there (a target below); prints the waypoint, the yaw to face
    // (`face <yaw>`), the distance, the drop and the angle down; near first, or the steepest first
    const float minDrop = num(a,1,300), maxDrop = num(a,2,1000);
    const size_t count  = size_t(num(a,3,15));
    struct Ledge { std::string name; float yaw, dist, drop, down, from; };
    std::vector<Ledge> all;
    const Vec3 at = pl.position();
    w.findWayPoint(at,[&](const WayPoint& wp) {
      const float g0 = groundY(w,wp.pos.x,wp.pos.z,wp.pos.y);
      if(std::isnan(g0))
        return false;
      float an = 0, rough = 0;
      Vec3  dn;
      if(!slopeAt(w,Vec3(wp.pos.x,g0,wp.pos.z),an,dn,rough) || an>15)
        return false;
      if(w.physic()->ray({wp.pos.x,g0+200,wp.pos.z},{wp.pos.x,g0+1500,wp.pos.z}).hasCol)
        return false; // a roof above: indoors
      Ledge best;
      best.down = 0;
      for(int k=0; k<16; ++k) {
        const float y = float(k)*22.5f*float(M_PI)/180.f;
        for(float d:{150.f,250.f,400.f,550.f,700.f,1000.f,1400.f}) {
          const Vec3 p = Vec3(wp.pos.x+std::cos(y)*d, g0, wp.pos.z+std::sin(y)*d);
          auto r = w.physic()->ray({p.x,g0+200,p.z},{p.x,g0-maxDrop-100,p.z});
          if(!r.hasCol)
            continue;
          const float drop = g0-r.v.y;
          if(drop<minDrop || drop>maxDrop)
            continue;
          const Vec3 eye = Vec3(wp.pos.x,g0+170,wp.pos.z), tg = r.v+Vec3(0,60,0);
          auto los = w.physic()->ray(eye,tg);
          if(los.hasCol && (los.v-tg).length()>40)
            continue;
          const float down = std::atan2(drop,d)*180.f/float(M_PI);
          if(down>best.down)
            best = Ledge{wp.name, float(k)*22.5f, d, drop, down, (wp.pos-at).length()};
          }
        }
      if(best.down>0)
        all.push_back(best);
      return false;
      });
    if(a.size()>4 && a[4]=="steep")
      std::sort(all.begin(),all.end(),[](const Ledge& x, const Ledge& y) { return x.down>y.down; }); else
      std::sort(all.begin(),all.end(),[](const Ledge& x, const Ledge& y) { return x.from<y.from; });
    say("ledges " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"ledge %s yaw=%.1f dist=%.0f drop=%.0f down=%.0f from=%.0f",all[i].name.c_str(),
                    double(all[i].yaw),double(all[i].dist),double(all[i].drop),double(all[i].down),double(all[i].from));
      say(buf);
      }
    }
  else if(c=="corner") {
    // corner <left|right> [count] | corner <left|right> go <i>: spots at a corner of a wall (near waypoints):
    // a wall right in front of the head, open far ahead leaning out to that side (Aim::SideReach): where
    // the manual aim leans out round it. Lists them (nearest first) or puts the hero at the i-th, facing
    // along it
    const bool  right = a.size()>1 && a[1]=="right";
    const bool  go    = a.size()>2 && a[2]=="go";
    const size_t pick = size_t(num(a,3,0));
    const size_t count = size_t(num(a,2,10));
    struct Spot { std::string name; Vec3 at; float yaw, front, from; };
    std::vector<Spot> all;
    const Vec3 at0 = pl.position();
    w.findWayPoint(at0,[&](const WayPoint& wp) {
      if((wp.pos-at0).length()>8000.f)
        return false;
      for(int k=0; k<16; ++k) {
        const float y    = float(k)*22.5f*float(M_PI)/180.f;
        const Vec3  f    = Vec3(std::cos(y),0,std::sin(y));
        Vec3        sd   = Vec3::crossProduct(Vec3(0,1,0),f);
        sd = Vec3::normalize(sd)*(right ? 1.f : -1.f);
        for(float shift=-90; shift<=90; shift+=15) {
          const Vec3  p0 = Vec3(wp.pos.x,wp.pos.y+50,wp.pos.z);
          const Vec3  p1 = p0 + sd*shift;
          if(w.physic()->ray(p0,p1).hasCol)
            continue;
          const float g  = groundY(w,p1.x,p1.z,p1.y+50);
          if(std::isnan(g) || std::abs(g-wp.pos.y)>40)
            continue;
          const Vec3 head = Vec3(p1.x,g+148,p1.z);
          auto       fr   = w.physic()->ray(head,head+f*400.f);
          if(!fr.hasCol || (fr.v-head).length()<50.f)
            continue; // no wall in front, or right at the face
          const Vec3 out = head + sd*Aim::SideReach;
          if(w.physic()->ray(head,out+sd*15.f).hasCol)
            continue; // no room to lean out
          if(w.physic()->ray(out,out+f*600.f).hasCol)
            continue; // walled leaning out too
          if(w.physic()->ray(Vec3(p1.x,g+60,p1.z),Vec3(p1.x,g+60,p1.z)+f*60.f).hasCol)
            continue; // a low step in front: not a corner
          all.push_back(Spot{wp.name,Vec3(p1.x,g,p1.z),float(k)*22.5f,(fr.v-head).length(),(p1-at0).length()});
          break;
          }
        }
      return false;
      });
    std::sort(all.begin(),all.end(),[](const Spot& x, const Spot& y) { return x.from<y.from; });
    if(go) {
      if(pick>=all.size()) {
        say("corner: no spot " + std::to_string(pick));
        return;
        }
      auto& sp = all[pick];
      pl.setPosition(sp.at);
      face(pl,Vec3(std::cos(sp.yaw*float(M_PI)/180.f),0,std::sin(sp.yaw*float(M_PI)/180.f)));
      bench.hasPrev = false;
      char buf[200];
      std::snprintf(buf,sizeof(buf),"corner %s %zu: %s yaw=%.1f wall in front %.0f cm",right ? "right" : "left",pick,sp.name.c_str(),
                    double(sp.yaw),double(sp.front));
      say(buf);
      return;
      }
    say("corners " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[200];
      std::snprintf(buf,sizeof(buf),"corner %zu %s yaw=%.1f front=%.0f from=%.0f",i,all[i].name.c_str(),double(all[i].yaw),
                    double(all[i].front),double(all[i].from));
      say(buf);
      }
    }
  else if(c=="walls") {
    // walls [count]: spots at a wall's edge (from a waypoint, walking to where the ground falls away) with
    // the foot of the wall 2.5..10 m below, 1 m out: hidden from the head, seen leaning over (the aim's
    // lean); prints the waypoint and the yaw for `goto`, `edge <yaw>`
    const size_t count = size_t(num(a,1,15));
    struct Wall { std::string name; float yaw, drop, from, down, bend; };
    std::vector<Wall> all;
    const Vec3 at = pl.position();
    auto ground = [&](const Vec3& p, float y0, float up, float down) {
      auto r = w.physic()->ray({p.x,y0+up,p.z},{p.x,y0-down,p.z});
      return r.hasCol ? r.v.y : NAN;
      };
    w.findWayPoint(at,[&](const WayPoint& wp) {
      const float g0 = groundY(w,wp.pos.x,wp.pos.z,wp.pos.y);
      if(std::isnan(g0))
        return false;
      for(int k=0; k<16; ++k) {
        const float y   = float(k)*22.5f*float(M_PI)/180.f;
        const Vec3  dir = Vec3(std::cos(y),0,std::sin(y));
        Vec3 e = Vec3(wp.pos.x,g0,wp.pos.z);
        bool edge = false;
        for(float d=25; d<=600; d+=25) {
          const Vec3  p = Vec3(wp.pos.x,g0,wp.pos.z) + dir*d;
          const float g = ground(p,e.y,100,150);
          if(std::isnan(g) || std::abs(g-e.y)>60) {
            edge = true;
            break;
            }
          e = Vec3(p.x,g,p.z);
          }
        if(!edge)
          continue;
        const Vec3  st   = e - dir*35.f;
        const Vec3  t    = st + dir*135.f;
        const float tg   = ground(t,st.y,-60,1000);
        if(std::isnan(tg) || st.y-tg<250 || st.y-tg>1000)
          continue;
        const Vec3 aimT = Vec3(t.x,tg+60,t.z);
        float down = 0, bend = 0;
        if(!perchLean(w,pl,st,dir,aimT,Aim::LeanStart,90.f,down,bend))
          continue;
        if(w.physic()->ray({t.x,tg+100,t.z},{t.x,tg+1500,t.z}).hasCol)
          continue; // under a roof, an arch
        all.push_back(Wall{wp.name, float(k)*22.5f, st.y-tg, (wp.pos-at).length(),down,bend});
        break;
        }
      return false;
      });
    std::sort(all.begin(),all.end(),[](const Wall& x, const Wall& y) { return x.from<y.from; });
    say("walls " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"wall %s yaw=%.1f drop=%.0f from=%.0f down=%.1f bend=%.0f",all[i].name.c_str(),double(all[i].yaw),
                    double(all[i].drop),double(all[i].from),double(all[i].down),double(all[i].bend));
      say(buf);
      }
    }
  else if(c=="perches") {
    // perches [radius 3000] [count 15] [min drop 250] [max drop 1000] [min down 30] [max down 90]
    //         [target dist 135] [edge margin 35]: like `walls`, but on any top around the
    // hero, waypoint or not (a roof, a rock, a wall's top): a grid of the highest surfaces, each one you can stand
    // on (slope up to 35 deg, 1.9 m free above) at an edge with the foot 1 m out hidden from the head, seen
    // leaning over, the sky above it; one a 2 m cell, the nearest first: `goto pos <x y z>`, `face <yaw>`.
    // A farther target and larger margin can find blocked 40..55 degree shots instead of steep wall-foot shots.
    const float  radius = num(a,1,3000);
    const size_t count  = size_t(num(a,2,15));
    const float  minD   = num(a,3,250), maxD = num(a,4,1000);
    const float  minDown = num(a,5,Aim::LeanStart), maxDown = num(a,6,90);
    const float  targetDist = num(a,7,135), margin = num(a,8,35);
    const Vec3   at     = pl.position();
    auto ground = [&](const Vec3& p, float y0, float up, float down) {
      auto r = w.physic()->ray({p.x,y0+up,p.z},{p.x,y0-down,p.z});
      return r.hasCol ? r.v.y : NAN;
      };
    struct Perch { Vec3 st; float yaw, drop, slope, from, down, bend; };
    std::map<std::pair<int,int>,Perch> cells;
    for(float x=-radius; x<=radius; x+=50)
      for(float z=-radius; z<=radius; z+=50) {
        if(x*x+z*z>radius*radius)
          continue;
        const Vec3 top = Vec3(at.x+x,at.y,at.z+z);
        const float s  = ground(top,at.y,3000,6000);
        if(std::isnan(s))
          continue;
        const Vec3 sp = Vec3(top.x,s,top.z);
        if(w.physic()->ray(sp+Vec3(0,5,0),sp+Vec3(0,190,0)).hasCol)
          continue;
        float slope = 0, rough = 0;
        Vec3  dn;
        if(!slopeAt(w,sp,slope,dn,rough) || slope>35 || rough>20)
          continue;
        for(int k=0; k<16; ++k) {
          const float y   = float(k)*22.5f*float(M_PI)/180.f;
          const Vec3  dir = Vec3(std::cos(y),0,std::sin(y));
          Vec3 e = sp;
          bool edge = false;
          for(float d=25; d<=300; d+=25) {
            const Vec3  p = sp + dir*d;
            const float g = ground(p,e.y,100,150);
            if(std::isnan(g) || std::abs(g-e.y)>60) {
              edge = true;
              break;
              }
            e = Vec3(p.x,g,p.z);
            }
          if(!edge)
            continue;
          const Vec3  st = e - dir*margin;
          const float sg = ground(st,e.y,60,100);
          if(std::isnan(sg))
            continue;
          const Vec3  stand = Vec3(st.x,sg,st.z);
          const Vec3  t     = stand + dir*targetDist;
          const float tg    = ground(t,stand.y,-60,maxD+100);
          if(std::isnan(tg) || stand.y-tg<minD || stand.y-tg>maxD)
            continue;
          const Vec3 aimT = Vec3(t.x,tg+60,t.z);
          float down = 0, bend = 0;
          if(!perchLean(w,pl,stand,dir,aimT,minDown,maxDown,down,bend))
            continue;
          if(w.physic()->ray({t.x,tg+100,t.z},{t.x,tg+1500,t.z}).hasCol)
            continue; // under a roof, an arch
          const auto key  = std::make_pair(int(std::floor(stand.x/200.f)),int(std::floor(stand.z/200.f)));
          const Perch pc  = {stand, float(k)*22.5f, stand.y-tg, slope, (stand-at).length(),down,bend};
          auto        it  = cells.find(key);
          if(it==cells.end() || pc.slope<it->second.slope)
            cells[key] = pc;
          break;
          }
        }
    std::vector<Perch> all;
    for(auto& [k,v]:cells)
      all.push_back(v);
    std::sort(all.begin(),all.end(),[](const Perch& x, const Perch& y) { return x.from<y.from; });
    say("perches " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[320];
      std::snprintf(buf,sizeof(buf),"perch goto pos %.0f %.0f %.0f, face %.1f: drop=%.0f slope=%.0f above the hero %.0f, from=%.0f down=%.1f bend=%.0f; pin WOLF %.0f 0 %.0f",
                    double(all[i].st.x),double(all[i].st.y+5),double(all[i].st.z),double(all[i].yaw),double(all[i].drop),
                    double(all[i].slope),double(all[i].st.y-at.y),double(all[i].from),double(all[i].down),double(all[i].bend),
                    double(targetDist),double(-all[i].drop));
      say(buf);
      }
    }
  else if(c=="edge") {
    // edge <yaw> [max cm] [margin cm] [exact]: the hero is put that way to where the ground falls away, and stands
    // that far from the edge (35: closer the body slips over it) facing it; prints how deep it goes down
    // right past the edge. Optional exact refines the usual 25 cm search before applying the margin.
    const float y   = num(a,1,0)*float(M_PI)/180.f;
    const Vec3  dir = Vec3(std::cos(y),0,std::sin(y));
    const float max = num(a,2,2000);
    Vec3        pos = pl.position();
    const float g0  = pos.y;
    bool        found = false;
    float       boundary = 0;
    for(float d=25; d<=max; d+=25) {
      const Vec3  p = pl.position() + dir*d;
      auto        r = w.physic()->ray({p.x,g0+100,p.z},{p.x,g0-150,p.z});
      if(!r.hasCol || std::abs(r.v.y-g0)>60) {
        found = true;
        boundary = d;
        break;
        }
      pos = Vec3(p.x,r.v.y,p.z);
      }
    if(!found) {
      say("edge: no edge that way");
      return;
      }
    if(a.size()>4 && a[4]=="exact") {
      float lo = boundary-25.f, hi = boundary;
      for(int i=0; i<8; ++i) {
        const float mid = (lo+hi)*0.5f;
        const Vec3  p   = pl.position()+dir*mid;
        const auto  r   = w.physic()->ray({p.x,g0+100,p.z},{p.x,g0-150,p.z});
        if(r.hasCol && std::abs(r.v.y-g0)<=60) {
          lo = mid;
          pos = Vec3(p.x,r.v.y,p.z);
          } else {
          hi = mid;
          }
        }
      }
    // a step back from the very edge, the feet on the ground
    pos -= dir*num(a,3,35);
    pl.setPosition(pos);
    face(pl,dir);
    pl.updateTransform();
    const Vec3 past = pos + dir*80.f;
    auto       r    = w.physic()->ray({past.x,pos.y-60,past.z},{past.x,pos.y-3000,past.z});
    char buf[200];
    std::snprintf(buf,sizeof(buf),"edge at %.0f %.0f %.0f, down past it %.0f",pos.x,pos.y,pos.z,r.hasCol ? pos.y-r.v.y : -1.f);
    say(buf);
    }
  else if(c=="edgedist") {
    // edgedist <yaw> [max cm] [min drop cm]: measure the first drop ahead without moving the hero.
    // Follow the floor in 5 cm steps so a continuous slope does not count as an edge, then refine it.
    const float yaw     = num(a,1,0);
    const float y       = yaw*float(M_PI)/180.f;
    const Vec3  dir     = Vec3(std::cos(y),0,std::sin(y));
    const Vec3  at      = pl.position();
    const float max     = num(a,2,2000);
    const float minDrop = num(a,3,100);
    auto floor = [&](float d, float height) {
      const Vec3 p = at+dir*d;
      return w.physic()->ray({p.x,height+100,p.z},{p.x,height-3000,p.z});
      };
    auto r = floor(0,at.y);
    if(!r.hasCol) {
      say("edgedist: no floor under the hero");
      return;
      }
    float lo = 0, height = r.v.y;
    for(float d=5; d<=max; d+=5) {
      r = floor(d,height);
      if(!r.hasCol || height-r.v.y>minDrop) {
        float hi = d;
        const float top = height;
        for(int i=0; i<8; ++i) {
          const float mid = (lo+hi)*0.5f;
          const auto  q   = floor(mid,top);
          if(q.hasCol && top-q.v.y<=minDrop)
            lo = mid; else
            hi = mid;
          }
        r = floor(hi,top);
        char buf[200];
        std::snprintf(buf,sizeof(buf),"edgedist yaw=%.1f distance=%.1f cm drop=%.1f cm (min=%.0f)",
                      double(yaw),double((lo+hi)*0.5f),double(r.hasCol ? top-r.v.y : -1.f),double(minDrop));
        say(buf);
        return;
        }
      lo     = d;
      height = r.v.y;
      }
    say("edgedist: no edge within " + std::to_string(max) + " cm");
    }
  else if(c=="findwp") {
    for(auto& s:findSpots(w,pl,K_Name,0,90,rest(line),40)) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"wp %s angle=%.1f rough=%.1f dist=%.0f",s.name.c_str(),s.angle,s.rough,s.dist);
      say(buf);
      }
    }
  else if(c=="pos") {
    const Vec3 p = pl.position();
    event("position","\"world_tick_ms\":"+std::to_string(w.tickCount())+
          ",\"world_time_ms\":"+std::to_string(w.time().toInt())+",\"position_cm\":"+ragdollVector(p));
    char buf[256];
    std::snprintf(buf,sizeof(buf),"pos %.0f %.0f %.0f rot=%.1f bs=%u air=%d swim=%d slide=%d mob=%d guild=%u aligned=%d",p.x,p.y,p.z,pl.rotation(),
                  unsigned(pl.bodyStateMasked()),int(pl.isInAir()),int(pl.isSwim()),int(pl.isSlide()),int(pl.interactive()!=nullptr),
                  unsigned(pl.guild()),int(w.script().guildVal().surface_align[pl.guild()]));
    say(buf);
    }
  else if(c=="talk") {
    // talk <npc|nearest> <ms>: the npc and the hero talk to each other (no words): they look at each other
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("talk: no npc " + line);
      return;
      }
    const uint64_t ms = uint64_t(num(a,2,3000));
    n->setTalkPartner(&pl,ms);
    pl.setTalkPartner(n,ms);
    say("talk " + symName(w,n) + " " + std::to_string(ms));
    }
  else if(c=="speak") {
    // speak <npc|nearest|player> <output name|$SVM> [to]: it says a voiced line of the game, like AI_Output in a
    // script, or an SVM ($SMALLTALK01, like AI_OutputSvm). To another npc it turns to it first, as a line of
    // the AI said to an npc does (v15 say.to)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3) {
      say("speak: no npc or line " + line);
      return;
      }
    Npc* to = (n==&pl) ? &subjectOf(pl) : &pl;
    if(a.size()>3) {
      to = findNpc(w,pl,a[3]);
      if(to==nullptr || to==n) {
        say("speak: no one to say it to " + line);
        return;
        }
      if(to!=&pl)
        n->aiPush(AiQueue::aiTurnToNpc(to));
      }
    if(a[2].starts_with("$")) {
      std::string svm = a[2];
      for(auto& ch:svm)
        ch = char(std::toupper(uint8_t(ch)));
      n->aiPush(AiQueue::aiOutputSvm(*to,svm,0));
      } else {
      n->aiPush(AiQueue::aiOutput(*to,a[2],0));
      }
    say("speak " + nameUtf8(*n) + " " + a[2] + (a.size()>3 ? " -> " + symName(w,to) : ""));
    }
  else if(c=="faceanim") {
    // faceanim <npc|nearest|player> <anim> [ms] [intensity]: a face animation of the head (S_ANGRY, S_FRIENDLY...)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3) {
      say("faceanim: no npc or anim " + line);
      return;
      }
    n->startFaceAnim(a[2],num(a,4,1),uint64_t(num(a,3,3000)));
    say("faceanim " + nameUtf8(*n) + " " + a[2]);
    }
  else if(c=="ani") {
    // ani <npc|nearest|player> <anim> [force]: it plays an animation of its model script (T_YES,
    // T_DIALOGGESTURE_03); force cuts what plays now, as a hit reaction or a stun's stumble that a stand would wait for
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3) {
      say("ani: no npc or anim " + line);
      return;
      }
    auto sq = n->playAnimByName(a[2],BS_STAND,a.size()>3 && a[3]=="force");
    say("ani " + symName(w,n) + " " + a[2] + (sq==nullptr ? " failed" : ""));
    }
  else if(c=="dialog") {
    // dialog <npc|nearest>: the hero talks to it, as with the action key (important infos start by themselves)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("dialog: no npc " + line);
      return;
      }
    n->startDialog(pl);
    say("dialog " + symName(w,n));
    }
  else if(c=="choose") {
    // choose <n>: runs the n-th dialog choice (from 1, as listed) of the watched npc, as if the player picked it
    Npc&       n   = subjectOf(pl);
    auto       chs = w.script().dialogChoices(pl.handlePtr(),n.handlePtr(),{},false);
    std::sort(chs.begin(),chs.end(),[](const GameScript::DlgChoice& l, const GameScript::DlgChoice& r){ return l.sort<r.sort; });
    const size_t i = size_t(num(a,1,1));
    if(i<1 || i>chs.size()) {
      say("choose: no choice " + std::to_string(i) + " of " + std::to_string(chs.size()));
      return;
      }
    say("choose " + std::to_string(i) + " of " + std::to_string(chs.size()));
    Gothic::inst().dialogExec(chs[i-1],pl,n);
    }
  else if(c=="lips") {
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      LipSync::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(LipSync::setParam(a[2],num(a,3,0)) ? "lips param " : "lips: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("lips params: " + LipSync::paramList());
    else
      say("unknown lips command: " + line);
    }
  else if(c=="gestures") {
    // gestures on|off|param <name> <value>|params: the hands while one speaks (off: a random gesture per line)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      TalkGestures::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(TalkGestures::setParam(a[2],num(a,3,0)) ? "gestures param " : "gestures: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("gestures params: " + TalkGestures::paramList());
    else
      say("unknown gestures command: " + line);
    }
  else if(c=="hear") {
    // hear <npc|nearest> <ms> [every ms]: the hero speaks to it that long, his words come every so often
    // (as the bridge does with his voice: it listens, nods now and then)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("hear: no npc " + line);
      return;
      }
    bench.hearNpc   = n;
    bench.hearUntil = bench.time + uint64_t(num(a,2,4000));
    bench.hearEvery = uint64_t(std::max(100.f,num(a,3,600)));
    bench.hearNext  = bench.time;
    say("hear " + symName(w,n));
    }
  else if(c=="think") {
    // think <npc|nearest> <ms>: it thinks of an answer that long (as on the AI server's `thinking`)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("think: no npc " + line);
      return;
      }
    bench.thinkNpc   = n;
    bench.thinkUntil = bench.time + uint64_t(num(a,2,2500));
    say("think " + symName(w,n));
    }
  else if(c=="blink") {
    // blink on|off|param <name> <value>|params: blinking (off: nobody blinks, as OpenGothic was)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      EyeBlink::setEnabled(m=="on");
    else if(m=="param" && a.size()>3)
      say(std::string(EyeBlink::setParam(a[2],num(a,3,0)) ? "blink param " : "blink: unknown param ") + a[2] + " " + a[3]);
    else if(m=="params")
      say("blink params: " + EyeBlink::paramList());
    else
      say("unknown blink command: " + line);
    }
  else if(c=="watch") {
    // watch <npc|nearest|player>: the camera and the gaze metrics follow this one
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    bench.subject  = (n==&pl) ? nullptr : n;
    Gaze::probeNpc = bench.subject;
    LipSync::probeNpc     = n;
    LipSync::subjectProbe = LipSync::Probe();
    TalkGestures::probeNpc     = n;
    TalkGestures::subjectProbe = TalkGestures::Probe();
    EyeBlink::probeNpc         = n;
    EyeBlink::subjectProbe     = EyeBlink::Probe();
    FaceRig::probeNpc          = n;
    FaceRig::subjectProbe      = FaceRig::Probe();
    Lean::probeNpc = bench.subject;
    Pose::probeNpc = bench.subject;
    bench.anim1.clear();
    bench.anim2.clear();
    Aim::probeNpc  = bench.subject;
    ArcherAi::probeNpc     = bench.subject;
    ArcherAi::subjectProbe = ArcherAi::Probe();
    Flinch::probeNpc = bench.subject;
    char buf[160] = "watch nobody";
    if(n!=nullptr)
      if(auto sym = w.script().findSymbol(size_t(n->instanceSymbol())))
        std::snprintf(buf,sizeof(buf),"watch %s dist=%.0f",sym->name().c_str(),double((n->position()-pl.position()).length()));
    say(buf);
    }
  else if(c=="gazeinfo") {
    Npc&         s = subjectOf(pl);
    const auto&  g = &s==&pl ? Gaze::playerProbe : Gaze::subjectProbe;
    Vec3         at;
    const auto   src = s.gazeTarget(at);
    char buf[300];
    std::snprintf(buf,sizeof(buf),"gazeinfo %s source=%s partner=%s script=%s attention=%s probe valid=%d active=%d w=%.2f err=%.1f hero focus=%s",
                  symName(w,&s).c_str(),gazeSource(src),symName(w,s.talkPartner()).c_str(),symName(w,s.lookAtTarget()).c_str(),
                  symName(w,s.attentionTarget()).c_str(),int(g.valid),int(g.active),double(g.weight),double(g.error),
                  symName(w,pl.target()).c_str());
    say(buf);
    }
  else if(c=="nearby") {
    // nearby [radius]: the npcs around the hero, where they look and why
    const float r = num(a,1,1500);
    std::vector<std::pair<float,Npc*>> list;
    w.detectNpc(pl.position(),r,[&](Npc& n) {
      if(&n!=&pl)
        list.emplace_back((n.position()-pl.position()).length(),&n);
      });
    std::sort(list.begin(),list.end(),[](auto& x, auto& y) { return x.first<y.first; });
    say("nearby " + std::to_string(list.size()));
    for(auto& [d,n]:list) {
      const Vec3  dp  = pl.position()-n->position();
      const float ang = std::remainder(std::atan2(dp.z,dp.x)*180.f/float(M_PI) - n->rotation(), 360.f);
      Vec3 at;
      const auto src = n->gazeTarget(at);
      char buf[300];
      std::snprintf(buf,sizeof(buf),"  %-28s dist=%5.0f hero_at=%5.0f bs=%2u mob=%d look=%s at=%s",
                    symName(w,n).c_str(),double(d),double(ang),unsigned(n->bodyStateMasked() & BS_MAX),
                    int(n->interactive()!=nullptr),gazeSource(src),
                    symName(w,src==Gaze::Source::Talk ? n->talkPartner() : src==Gaze::Source::Script ? n->lookAtTarget() :
                                src==Gaze::Source::Attention ? n->attentionTarget() : nullptr).c_str());
      say(buf);
      }
    }
  else if(c=="state") {
    // state <npc> <ZS_*> <waypoint> [minutes 60]: it does that now at that place, as a routine entry would
    // (TA_*), for that many game minutes; its routine goes on afterwards (npc_act start_activity)
    Npc* n = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    std::string name = a.size()>2 ? a[2] : "";
    for(auto& ch:name)
      ch = char(std::toupper(uint8_t(ch)));
    const size_t fn = name.starts_with("ZS_") ? w.script().findSymbolIndex(name) : size_t(-1);
    auto         wp = a.size()>3 ? w.findPoint(a[3],false) : nullptr;
    if(n==nullptr || n==&pl || fn==size_t(-1) || wp==nullptr) {
      say("state: no npc, state or waypoint " + line);
      return;
      }
    auto until = w.time();
    until.addMilis(uint64_t(std::clamp(num(a,4,60),1.f,24*60.f))*60*1000);
    const bool ok = n->startState(ScriptFn(fn),wp->name,until,false);
    say("state " + symName(w,n) + " " + name + " " + wp->name + (ok ? "" : " failed"));
    }
  else if(c=="come") {
    // come <npc>: it walks up to the hero and turns to him (npc_act go_to_npc): a free one goes back to its
    // routine afterwards
    Npc* n = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    if(n==nullptr || n==&pl) {
      say("come: no npc " + line);
      return;
      }
    n->aiPush(AiQueue::aiGoToNpc(&pl));
    n->aiPush(AiQueue::aiTurnToNpc(&pl));
    char buf[160];
    std::snprintf(buf,sizeof(buf),"come %s dist=%.0f",symName(w,n).c_str(),double((n->position()-pl.position()).length()));
    say(buf);
    }
  else if(c=="pair") {
    // pair <a> <b>: how two npcs stand to each other: the distance; for each, where it faces (0: right at the
    // other), where it looks and why, its state and for how long (ms), its body state, a mob it uses, how much
    // of its AI runs
    Npc* x = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    Npc* y = a.size()>2 ? findNpc(w,pl,a[2]) : nullptr;
    if(x==nullptr || y==nullptr || x==y) {
      say("pair: no npcs " + line);
      return;
      }
    auto one = [&](Npc& n, Npc& o) {
      const Vec3  dp  = o.position()-n.position();
      const float ang = std::remainder(std::atan2(dp.z,dp.x)*180.f/float(M_PI) - n.rotation(), 360.f);
      Vec3        at;
      const auto  src = n.gazeTarget(at);
      char buf[400];
      std::snprintf(buf,sizeof(buf),"%s at_other=%.0f look=%s at=%s state=%s t=%llu bs=%u mob=%d ai=%s",
                    symName(w,&n).c_str(),double(ang),gazeSource(src),gazeWho(w,n,src).c_str(),stateName(w,n).c_str(),
                    (unsigned long long)n.stateTime(),unsigned(n.bodyStateMasked() & BS_MAX),
                    int(n.interactive()!=nullptr),policyName(n.processPolicy()));
      return std::string(buf);
      };
    char head[64];
    std::snprintf(head,sizeof(head),"pair dist=%.0f | ",double((x->position()-y->position()).length()));
    say(head + one(*x,*y) + " | " + one(*y,*x));
    }
  else if(c=="where") {
    // where <npc> [waypoint]: where it is, how far from the hero (and from the waypoint), its state, whether
    // it is busy with its queue (walking there), how much of its AI runs
    Npc* n = a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    if(n==nullptr) {
      say("where: no npc " + line);
      return;
      }
    const Vec3 p = n->position();
    char buf[400];
    int  len = std::snprintf(buf,sizeof(buf),"where %s at %.0f %.0f %.0f hero=%.0f",symName(w,n).c_str(),
                             double(p.x),double(p.y),double(p.z),double((p-pl.position()).length()));
    if(a.size()>2 && len>0 && size_t(len)<sizeof(buf)) {
      if(auto wp = w.findPoint(a[2],false))
        len += std::snprintf(buf+len,sizeof(buf)-size_t(len)," %s=%.0f",wp->name.c_str(),double((p-wp->position()).length())); else
        len += std::snprintf(buf+len,sizeof(buf)-size_t(len)," %s=?",a[2].c_str());
      }
    if(len>0 && size_t(len)<sizeof(buf))
      std::snprintf(buf+len,sizeof(buf)-size_t(len)," state=%s t=%llu queue=%d bs=%u ai=%s",stateName(w,*n).c_str(),
                    (unsigned long long)n->stateTime(),int(!n->isAiQueueEmpty()),unsigned(n->bodyStateMasked() & BS_MAX),
                    policyName(n->processPolicy()));
    say(buf);
    }
  else if(c=="lookat") {
    // lookat <npc|nearest> <npc|player|off>: as the script AI_LookAtNpc, AI_StopLookAt
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("lookat: no npc " + line);
      return;
      }
    const std::string t = a.size()>2 ? a[2] : "player";
    if(t=="off") {
      n->aiPush(AiQueue::aiStopLookAt());
      say("lookat " + symName(w,n) + " off");
      return;
      }
    Npc* o = findNpc(w,pl,t);
    if(o==nullptr || o==n) {
      say("lookat: no target " + line);
      return;
      }
    n->aiPush(AiQueue::aiLookAtNpc(o));
    say("lookat " + symName(w,n) + " " + symName(w,o));
    }
  else if(c=="freeze") {
    // freeze <npc|nearest> [off]: it leaves what it does and stands still, like a body the AI server attaches
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("freeze: no npc " + line);
      return;
      }
    if(a.size()>2 && a[2]=="off") {
      n->setExternalControl(false);
      say("freeze off");
      return;
      }
    n->setInteraction(nullptr,true);
    n->clearAiQueue();
    n->clearState(false);
    n->setExternalControl(true);
    say("freeze " + symName(w,n));
    }
  else if(c=="ani") {
    // ani <npc|nearest|player> <name>: it plays that animation, as play_ani does (T_STAND_2_PEE goes on to
    // its loop S_PEE till something else plays)
    Npc* n = a.size()>1 && a[1]=="player" ? &pl : findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3 || !n->hasAnim(a[2])) {
      say("ani: no npc or animation " + line);
      return;
      }
    n->aiPush(AiQueue::aiPlayAnim(a[2]));
    say("ani " + symName(w,n) + " " + a[2]);
    }
  else if(c=="heropose") {
    // heropose kneel|none|check: set the surrender pose, or exercise its real input paths.
    if(a.size()>1 && a[1]=="check") {
      if(!pl.isPlayerKneeling())
        throw std::runtime_error("heropose check requires a kneeling hero");
      const float rotation = pl.rotation();
      ctrl.onRotateMouse(15.f,5.f);
      ctrl.tickMove(16);
      if(!pl.isPlayerKneeling())
        throw std::runtime_error("heropose: mouse rotation ended kneel");
      if(std::abs(std::remainder(pl.rotation()-rotation-15.f,360.f))>0.01f)
        throw std::runtime_error("heropose: mouse rotation did not turn the hero");
      std::string reply;
      if(!aiCommand("ai ptt start",reply))
        throw std::runtime_error("heropose: no AI bridge for ptt_start");
      const bool speakingKneeling = pl.isPlayerKneeling();
      aiCommand("ai ptt cancel",reply);
      if(!speakingKneeling)
        throw std::runtime_error("heropose: ptt_start ended kneel");
      press(ctrl,KeyCodec::Forward);
      release(ctrl,KeyCodec::Forward);
      if(pl.isPlayerKneeling())
        throw std::runtime_error("heropose: Forward did not end kneel");
      say("heropose check passed: mouse and ptt_start preserve kneel; Forward ends kneel");
      return;
      }
    const bool kneel = a.size()>1 && a[1]=="kneel";
    say(std::string("heropose ") + (pl.setPlayerPose(kneel,"testbench none") ? (kneel ? "kneel" : "none") : "refused"));
    }
  else if(c=="place") {
    // place <dist> <angle>: the hero stands that far from the watched npc, at that angle from its facing
    bench.placeDist  = num(a,1,150);
    bench.placeAngle = num(a,2,0);
    placeHero(w,pl);
    bench.hasPrev = false;
    }
  else if(c=="orbit") {
    // orbit <deg> <ms>: the hero goes around the watched npc
    const float ms = std::max(1.f,num(a,2,3000));
    bench.orbitSpeed = num(a,1,90)*1000.f/ms;
    bench.orbitUntil = bench.time + uint64_t(ms);
    }
  else if(c=="transform") {
    // the player becomes a creature, like the shape change spell; "transform back"
    const std::string name = a.size()>1 ? a[1] : "";
    if(name=="back") {
      pl.transformBack();
      say("transform back");
      return;
      }
    const size_t id = w.script().findSymbolIndex(name);
    const bool   ok = id!=size_t(-1) && pl.transformInto(id);
    bench.hasPrev = false;
    say(std::string(ok ? "transform " : "transform failed: ") + name);
    }
  else if(c=="bones") {
    // skeleton of the player: index, parent, name
    auto sk = FootIk::playerProbe.skeleton;
    if(sk==nullptr) {
      say("bones: no skeleton");
      return;
      }
    say("bones: " + std::to_string(sk->nodes.size()) + " nodes, foot ik " + (FootIk::playerProbe.valid ? "valid" : "not valid"));
    for(size_t i=0; i<sk->nodes.size(); ++i) {
      char buf[256];
      const auto& t = sk->tr[i];
      std::snprintf(buf,sizeof(buf),"bone %zu parent %d %s at %.0f %.0f %.0f",i,int(sk->nodes[i].parent==size_t(-1) ? -1 : int(sk->nodes[i].parent)),
                    sk->nodes[i].name.c_str(),t.at(3,0),t.at(3,1),t.at(3,2));
      say(buf);
      }
    }
  else if(c=="ptt") {
    // ptt <wav> [text]: the hero holds the talk key while the recording is said; the AI server takes the
    // file instead of the microphone (--voice-files), the npc's hear it through speech recognition.
    // The text only goes into events.jsonl (subtitles)
    const std::string wav = a.size()>1 ? a[1] : "";
    const uint64_t    ms  = wavMs(wav);
    if(ms==0) {
      say("ptt: not a 16-bit wav: " + wav);
      return;
      }
    std::string reply;
    if(!aiCommand("ai ptt start " + wav,reply)) {
      say("ptt: no AI bridge");
      return;
      }
    const std::string text = rest(rest(line));
    bench.speaking   = true;
    bench.speakUntil = bench.time + ms + SpeakTailMs;
    event("hero","\"wav\":" + jsonStr(wav) + ",\"ms\":" + std::to_string(ms) + ",\"text\":" + jsonStr(text));
    say("ptt " + std::to_string(ms) + " ms: " + text + " (" + reply + ")");
    }
  else if(c=="waitreply" || c=="waitquiet") {
    // waitreply [timeout ms 30000] [quiet ms 1200]: until an npc of the AI has answered the hero's last
    // phrase and has been quiet that long; waitquiet [quiet ms 1200] [timeout ms 30000]: only the quiet
    const bool reply = c=="waitreply";
    bench.waitReply  = true;
    bench.replyNeed  = reply;
    bench.replyFrom  = bench.heroEndAt>0 ? bench.heroEndAt : bench.time;
    bench.replyEnd   = bench.time + uint64_t(num(a,reply ? 1 : 2,30000));
    bench.replyQuiet = uint64_t(num(a,reply ? 2 : 1,1200));
    }
  else if(c=="waitai") {
    // waitai <key> <stage> [timeout ms 20000]: until a thing of the AI bridge has got that far (waitai necro risen)
    if(a.size()<3) {
      say("waitai: <key> <stage> [timeout]");
      return;
      }
    bench.waitAiKey   = a[1];
    bench.waitAiStage = a[2];
    bench.waitAiEnd   = bench.time + uint64_t(num(a,3,20000));
    }
  else if(c=="caption") {
    // text on the screen of a short from here (gothic-shorts cut): caption <text>, caption off
    event("caption","\"text\":" + jsonStr(rest(line)=="off" ? "" : rest(line)));
    }
  else if(c=="mark") {
    const auto text=rest(line);
    event("mark","\"text\":" + jsonStr(text));
    if(LoadProbe::enabled() && ((text=="stream_warm" && w.chunkResidency()!=nullptr) || text=="stream_warm_5s")) {
      std::string pages;
      if(auto* chunks=w.chunkResidency()) {
        const auto& s=chunks->stats();
        pages="landscape_used_bytes="+std::to_string(s.landscapePageUsedBytes)+
              " landscape_free_bytes="+std::to_string(s.landscapePageFreeBytes)+
              " landscape_retired_bytes="+std::to_string(s.landscapePageRetiredBytes);
        pages+=" "+s.vobProbeFields();
        }
      pages+=(pages.empty() ? "" : " ")+w.npcSimTierStats().probeFields();
      pages+=" "+NpcLogicAudit::stats().probeFields();
      w.probeMemory(text);
      LoadProbe::mark(text,-1,pages);
      }
    }
  else if(c=="ai") {
    std::string reply;
    say(std::string(aiCommand(line,reply) ? "" : "no AI bridge: ") + line + " -> " + reply);
    }
  else if(c=="quit") {
    bench.pc = bench.prog.size();
    }
  else {
    const bool ok = marvin().exec(line);
    say(std::string(ok ? "marvin: " : "marvin failed: ") + line);
    }
  }

}

namespace {

void tickVolley(PlayerControl& ctrl) {
  const auto& mp = Archer::playerProbe;
  const auto  t  = bench.time - bench.npcCommands.volleyT;
  auto stop = [&](const char* why) {
    say(std::string("volley: ") + why);
    release(ctrl,KeyCodec::ActionGeneric);
    bench.npcCommands.volleyLeft = 0;
    };
  switch(bench.npcCommands.volleyPhase) {
    case 0:
      press(ctrl,KeyCodec::ActionGeneric);
      bench.npcCommands.volleyPhase = 1;
      bench.npcCommands.volleyT     = bench.time;
      break;
    case 1: {
      if(mp.valid && mp.stage==Archer::Held && mp.draw<Archery::MinDraw) {
        stop("the draw limit is below the release threshold");
        break;
        }
      // Held means the archer reached its own limit, which can be below the requested draw.
      const bool drawn = mp.valid && (mp.stage==Archer::Held || (mp.stage==Archer::Drawing && mp.draw>=bench.npcCommands.volleyDraw));
      if(drawn) {
        bench.npcCommands.volleyPhase   = 2;
        bench.npcCommands.volleyT       = bench.time;
        // held at its limit it waits; drawn as far as asked short of it, it goes at once: the button held
        // draws on (the hero has no partial hold)
        bench.npcCommands.volleyHoldNow = mp.stage==Archer::Held ? bench.npcCommands.volleyHold : 0;
        if(bench.npcCommands.volleyJitter>0 && mp.stage==Archer::Held) {
          bench.lcg = bench.lcg*1664525u + 1013904223u;
          bench.npcCommands.volleyHoldNow += (bench.lcg>>8)%(bench.npcCommands.volleyJitter+1);
          }
        }
      else if(t>10000)
        stop("the string is not drawn");
      break;
      }
    case 2:
      if(t>=bench.npcCommands.volleyHoldNow) {
        if(bench.archery.aimNpc!=nullptr) {
          // where it aims, the middle of the zone: the log compares the arrow's flight with it
          Vec3 c = bench.archery.aimNpc->collosionCenter();
          aimPoint(*bench.archery.aimNpc,bench.archery.aimZone,bench.archery.aimLeg,c);
          char buf[160];
          std::snprintf(buf,sizeof(buf),"aim zone %.1f %.1f %.1f",c.x,c.y,c.z);
          TestBench::note(buf);
          }
        release(ctrl,KeyCodec::ActionGeneric);
        bench.npcCommands.volleyPhase = 3;
        bench.npcCommands.volleyT     = bench.time;
        }
      break;
    case 3:
      if(mp.valid && mp.stage==Archer::Nocked && t>200) {
        --bench.npcCommands.volleyLeft;
        bench.npcCommands.volleyPhase = 0;
        }
      else if(t>8000)
        stop("no next arrow");
      break;
    }
  }

// the hero's phrase is said: the talk key goes up
void endSpeaking() {
  std::string reply;
  aiCommand("ai ptt stop",reply);
  bench.speaking  = false;
  bench.heroEndAt = bench.time;
  event("hero_end");
  }

bool waitingForReply() {
  if(!bench.waitReply)
    return false;
  const bool replied = !bench.replyNeed || bench.lastLineAt>bench.replyFrom;
  const bool quiet   = bench.time>=bench.busyAt+bench.replyQuiet;
  if(replied && quiet) {
    bench.waitReply = false;
    return false;
    }
  if(bench.time<bench.replyEnd)
    return true;
  say(std::string(bench.replyNeed ? "waitreply" : "waitquiet") + ": timeout" + (replied ? "" : ", nobody answered"));
  event("timeout");
  bench.waitReply = false;
  return false;
  }

bool waitingForAi() {
  if(bench.waitAiKey.empty())
    return false;
  const bool signalled=bench.waitAiKey=="broken" && bench.brokenSignals.count(bench.waitAiStage)>0;
  if(signalled || (TestBench::aiReached && TestBench::aiReached(bench.waitAiKey,bench.waitAiStage))) {
    say("waitai " + bench.waitAiKey + " " + bench.waitAiStage);
    bench.waitAiKey.clear();
    return false;
    }
  if(bench.time<bench.waitAiEnd)
    return true;
  say("waitai " + bench.waitAiKey + " " + bench.waitAiStage + ": timeout");
  bench.waitAiKey.clear();
  return false;
  }

// One frame of `streaming wait`; true while the scenario stays in it. In deterministic mode the world stands
// still from the first frame that finds the landscape unready (MainWindow::tick takes no step, this runs with
// dt 0); readiness then also means no texture demotion or reload is pending. Once it is ready, the renderer drops
// its temporal state on this very frame, texture residency stands still, and the scenario takes a fixed number of
// steps with visibility in cluster order; both last until the next shot.
// Neither the frame count of the wait nor its wall time then reach the picture.
bool streamingWaitTick(World& w) {
  if(bench.time<bench.waitUntil)
    return true;
  if(!w.landscapeReady()) {
    if(std::chrono::steady_clock::now()<bench.streamingDeadline) {
      bench.frozen=bench.deterministic;
      return true;
      }
    bench.waitStreaming=false;
    bench.frozen=false;
    arenaAbort("streaming wait timed out before all Near/Far landscape and vob visuals were published");
    return true;
    }
  bench.waitStreaming=false;
  bench.frozen=false;
  std::string publications="[";
  for(size_t i=0;i<bench.streamingWarmupFarMs.size();++i) {
    if(i!=0)publications+=',';
    publications+=std::to_string(bench.streamingWarmupFarMs[i]);
    }
  publications+=']';
  event("streaming_ready","\"world\":"+jsonStr(w.name())+",\"pass\":true,\"far_publication_ms\":"+publications+
    ",\"far_max_per_frame\":"+std::to_string(bench.streamingWarmupFarMaxPerFrame)+
    ",\"queue_max_ms\":"+std::to_string(bench.streamingWarmupQueueMaxMs)+
    ",\"vob_pending\":"+std::to_string(w.chunkResidency() ? w.chunkResidency()->stats().vobPending : 0)+
    ",\"vob_near_missing\":"+std::to_string(w.chunkResidency() ? w.chunkResidency()->stats().vobNearMissing : 0));
  bench.streamingWarmupFarMs.clear();
  bench.streamingWarmupFarMaxPerFrame=0;
  bench.streamingWarmupQueueMaxMs=0;
  if(bench.deterministic) {
    bench.renderReset=true;
    bench.ordered=true;
    benchTexturesFrozen=true; // the levels stay as they are now until the shot
    bench.settleSteps=DeterministicSettleSteps;
    }
  return false;
  }

void run(World& w, Npc& pl, PlayerControl& ctrl) {
  if(npcTierTestTick(bench.npcTierTest,w,pl,bench.frameMs))return;
  if(npcSpeedTick(bench.npcSpeedTest,w))return;
  if(bench.routeActive || bench.placedItems.probing)return;
  if(bench.routine.routinePhase) {routineReturnTick(w,pl,ctrl);if(bench.routine.routinePhase)return;}
  if(NavBench::running() || NavDemos::running() || NpcDayJournal::running()) return;
  if(bench.movables.spellEquipPending) {
    if(pl.drawSpell(bench.movables.spellEquipPending)) {bench.movables.spellCastingId=bench.movables.spellEquipPending;bench.movables.spellEquipPending=0;}
    else if(w.tickCount()<bench.movables.spellEquipUntil) return;
    else {bench.movables.spellEquipPending=0;arenaAbort("real spell draw animation did not finish");return;}
    }
  if(bench.movables.spellWaitFrozen>=0) {
    const bool frozen=!bench.movables.spellActors.empty() && bench.movables.spellActors[0].npc->spellFrozen();
    const bool ready=bench.movables.spellWaitFrozen==2 ? !bench.movables.spellActors.empty() && bench.movables.spellActors[0].npc->isInAir() :
                     bench.movables.spellWaitFrozen==3 ? w.spellFieldCount()>0 :
                     bench.movables.spellWaitFrozen==4 ? !bench.movables.spellActors.empty() &&
                       bench.movables.spellActors[0].npc->ragdollQuestReturnCount()==1 &&
                       bench.movables.spellActors[0].npc->isStanding() && !bench.movables.spellActors[0].npc->isInAir() &&
                       !bench.movables.spellActors[0].npc->isDown() && !bench.movables.spellActors[0].npc->ragdollQuestReturnState().armed :
                       int(frozen)==bench.movables.spellWaitFrozen;
    if(!ready && w.tickCount()<bench.movables.spellWaitUntil) return;
    event("spell_physics_freeze_wait","\"frozen\":"+std::string(frozen?"true":"false")+",\"pass\":"+(ready?"true":"false"));
    bench.movables.spellWaitFrozen=-1;
    if(!ready) {arenaAbort(bench.movables.spellGuardTrace?"real wind throw did not return standing":"real freeze state did not reach requested transition");return;}
    }
  if(!bench.npcCommands.wanderPoints.empty())
    for(auto* n:bench.npcCommands.wanderers) {
      if(n->isDown() || n->isAiBusy())
        continue;
      bench.lcg = bench.lcg*1664525u + 1013904223u;
      n->aiPush(AiQueue::aiGoToPoint(*bench.npcCommands.wanderPoints[(bench.lcg>>8) % bench.npcCommands.wanderPoints.size()]));
      ++bench.npcCommands.wanderLegs;
      }
  if(bench.melee.meleeFixture && !bench.melee.meleeBrawl) {
    size_t actor=0;
    for(auto& walker:bench.living.livingWalkers) {
      auto& npc=*walker.npc;
      const auto index=actor++;
      const int phase=int(bench.melee.meleeSteady ? index%3 : ((w.tickCount()-bench.melee.meleeMotionFrom)/4000+index)%3);
      if(!npc.drawWeaponMelee()) continue;
      if(phase!=walker.meleePhase) {
        npc.clearAiQueue();npc.clearGoTo();npc.stopWalking();npc.setAnim(Npc::Anim::Idle);
        walker.queued=false;walker.meleePhase=phase;walker.meleeAttackAt=0;
        }
      if(phase==0) {
        const auto& target=walker.ends[walker.next?1:0];
        if(!walker.queued || ((npc.position()-target.pos).length()<35 && npc.isAiQueueEmpty())) {
          if(walker.queued) walker.next=!walker.next;
          npc.aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
          }
        }
      else if(phase==2 && w.tickCount()>=walker.meleeAttackAt) {
        const bool started=npc.setAnim(Npc::Anim::Attack);
        if(bench.melee.meleeMeasuring && started) ++bench.melee.meleeAttackStarts;
        walker.meleeAttackAt=w.tickCount()+1500;
        }
      }
    }
  if(bench.living.livingMotion)
    for(auto& walker:bench.living.livingWalkers) {
      if(walker.npc->isDown()) continue;
      const auto& target=walker.ends[walker.next?1:0];
      if(!walker.queued || ((walker.npc->position()-target.pos).length()<120 && walker.npc->isAiQueueEmpty())) {
        if(walker.queued) walker.next=!walker.next;
        walker.npc->aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
        }
      }
  for(auto& walker:bench.character.npcCharacterCrowd) {
    if(walker.npc->isDown())continue;
    const auto& target=walker.ends[walker.next?1:0];
    if(!walker.queued || ((walker.npc->position()-target.pos).length()<90 && walker.npc->isAiQueueEmpty())) {
      if(walker.queued)walker.next=!walker.next;
      walker.npc->aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
      }
    }
  if(bench.routine.questNpc!=nullptr && !bench.routine.questCase.empty()) questFields(w,*bench.routine.questNpc,false);
  if(bench.routine.questStandingWait) {
    const auto* npc=bench.routine.questNpc;
    const bool ready=npc && npc->isStanding() && !npc->isDown() && !npc->ragdollBody() &&
                     !npc->isRagdollBlending() && !npc->ragdollQuestReturnState().armed;
    if(!ready && bench.time<bench.routine.questDeadline) return;
    bench.routine.questStandingWait=false;
    event("immortal_guard_standing","\"case\":"+jsonStr(bench.routine.questCase)+",\"standing\":"+(ready?"true":"false"));
    if(!ready) {arenaAbort("real get-up did not finish standing");return;}
    }
  if(bench.routine.questWait) {
    const bool observed=bench.routine.questWaterCase?bench.routine.questSawWater:bench.routine.questSawDrop;
    const uint8_t wanted=bench.routine.questWaterCase ? RagdollSave::QuestReturn::water : RagdollSave::QuestReturn::fell;
    const bool guarded=bench.routine.questNpc->isHuman() && !bench.routine.questNpc->isPlayer() && Gothic::settingsGetI("GAME","ragdoll_quest_guard");
    const bool reached=observed && (!guarded || (bench.routine.questNpc->ragdollQuestReturnState().hazards & wanted));
    if(!reached && bench.time<bench.routine.questDeadline) return;
    bench.routine.questWait=false;
    if(!reached) {arenaAbort("quest guard physical hazard was not reached");return;}
    const auto* npc=bench.routine.questNpc;
    const auto* body=npc->ragdollBody();
    auto pelvis=npc->position();
    if(body) {const auto tr=body->pelvisTransform();pelvis={tr.at(3,0),tr.at(3,1),tr.at(3,2)};}
    const auto water=w.physic()->waterRay(pelvis,0);
    event("immortal_guard_hazard","\"case\":"+jsonStr(bench.routine.questCase)+",\"pelvis_cm\":"+ragdollVector(pelvis)+
      ",\"water_y_cm\":"+(water.hasCol?ragdollNumber(water.wdepth):"null")+
      ",\"unconscious\":"+(npc->isUnconscious()?"true":"false")+
      ",\"parts\":"+std::to_string(body?body->partCount():0)+",\"hazards\":"+std::to_string(npc->ragdollQuestReturnState().hazards));
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  if(bench.cadenceWait) { if(!AppCadence::done()) return;bench.cadenceWait=false; }
#endif
  if(bench.observeActive) {
    const auto now = w.tickCount();
    bool invalid = false;
    for(const auto* actor:bench.observeActors)
      invalid = invalid || actor->isDown() || actor->isImmortal() || actor->isExternalControl() ||
                actor->processPolicy()!=NpcProcessPolicy::AiNormal || actor->qDistTo(pl)>3000.f*3000.f;
    if(invalid) {
      event("body_observe_error","\"reason\":\"native actor escaped, died or lost active native processing\"");
      bench.observeActive = false;
      bench.pc = bench.prog.size();
      }
    else if(now>=bench.observeEnd) {
      event("body_observe_end","\"t_game_ms\":"+std::to_string(now)+",\"elapsed_ms\":"+
            std::to_string(now-bench.observeFrom)+",\"actors\":4");
      bench.observeActive = false;
      }
    }
  if(bench.npcCommands.volleyLeft>0) {
    tickVolley(ctrl);
    if(bench.npcCommands.volleyLeft>0)
      return;
    }
  while(bench.pc<bench.prog.size() || bench.speaking || bench.waitReply || !bench.waitAiKey.empty() || !bench.bodyLiveWait.empty() ||
        bench.npcCommands.downNpc!=nullptr || bench.ragdoll.waitRagdollNpc!=nullptr || bench.duelWaiting || bench.npcDuel.active || bench.waitStreaming ||
        std::chrono::steady_clock::now()<bench.realWaitUntil) {
    if(bench.speaking) {
      if(bench.time<bench.speakUntil)
        return;
      endSpeaking();
      }
    if(waitingForReply() || waitingForAi() || waitingForBodyLive())
      return;
    if(bench.waitStreaming && streamingWaitTick(w))
      return;
    if(bench.ragdoll.waitRagdollNpc!=nullptr) {
      const auto* npc=bench.ragdoll.waitRagdollNpc;
      const auto* body=npc->ragdollBody();
      const auto& quest=npc->ragdollQuestReturnState();
      const bool guardedReturn=quest.armed && quest.hazards!=0;
      const bool ready=bench.ragdoll.waitRagdollState=="knockdown" ? npc->isMeleeKnockedDown() :
                       bench.ragdoll.waitRagdollState=="sleep" ? body!=nullptr && body->isSleeping() :
                       bench.ragdoll.waitRagdollState=="recovery" ? npc->isRagdollBlending() :
                       body==nullptr && !npc->isDown() && !npc->isMeleeKnockedDown() && !npc->isRagdollBlending() &&
                       (!guardedReturn || (npc->isStanding() && !quest.armed));
      if(!ready && bench.time<bench.ragdoll.waitRagdollEnd)
        return;
      say("waitragdoll "+bench.ragdoll.waitRagdollState+(ready ? " reached" : " timeout"));
      event(ready ? "ragdoll_wait" : "ragdoll_error","\"state\":"+jsonStr(bench.ragdoll.waitRagdollState)+
            ",\"reached\":"+(ready ? "true" : "false"));
      bench.ragdoll.waitRagdollNpc=nullptr;
      }
    if(tickNpcDuel(bench.npcDuel,w))return;
    if(bench.duelWaiting) {
      Npc& first = *bench.duelA;
      Npc& second = *bench.duelB;
      const uint64_t now = w.tickCount();
      if(!first.isDown() && !second.isDown() && now<bench.duelEnd)
        return;
      const std::string outcome = first.isDown() ? (second.isDown() ? "both_down" : "actor_down") :
                                  (second.isDown() ? "target_down" : "timeout");
      endAnimationControl();
      TestBench::bodyEvent(first,&second,"episode_end",now,
                           "\"outcome\":"+jsonStr(outcome)+",\"elapsed_ms\":"+std::to_string(now-bench.duelFrom)+
                           ",\"actor_hp\":"+std::to_string(first.attribute(ATR_HITPOINTS))+
                           ",\"target_hp\":"+std::to_string(second.attribute(ATR_HITPOINTS))+
                           ",\"actor_dead\":"+(first.isDead() ? "true" : "false")+
                           ",\"target_dead\":"+(second.isDead() ? "true" : "false"));
      say("bodyduel outcome "+outcome+" after "+std::to_string(now-bench.duelFrom));
      for(Npc* n:{bench.duelA,bench.duelB}) {
        n->clearAiQueue();
        n->clearState(true);
        n->setExternalControl(true);
        }
      bench.duelWaiting = false;
      }
    if(bench.npcCommands.downNpc!=nullptr) {
      Npc* down = bench.npcCommands.downNpc->isDown() ? bench.npcCommands.downNpc :
                  bench.npcCommands.downOther!=nullptr && bench.npcCommands.downOther->isDown() ? bench.npcCommands.downOther : nullptr;
      if(down==nullptr && bench.time<bench.npcCommands.downEnd)
        return;
      event("waitdown_end","\"t_game_ms\":"+std::to_string(w.tickCount())+
            ",\"elapsed_ms\":"+std::to_string(bench.time-bench.npcCommands.downFrom)+
            ",\"outcome\":"+jsonStr(down!=nullptr ? "knockout" : "timeout")+
            ",\"down\":"+jsonStr(symName(w,down))+
            ",\"actor_hp\":"+std::to_string(bench.npcCommands.downNpc->attribute(ATR_HITPOINTS))+
            ",\"other_hp\":"+(bench.npcCommands.downOther!=nullptr ? std::to_string(bench.npcCommands.downOther->attribute(ATR_HITPOINTS)) : "null"));
      char buf[160];
      if(down!=nullptr)
        std::snprintf(buf,sizeof(buf),"fight over: %s down after %llu",symName(w,down).c_str(),
                      (unsigned long long)(bench.time-bench.npcCommands.downFrom)); else
        std::snprintf(buf,sizeof(buf),"waitdown: timeout, %s hp %d",symName(w,bench.npcCommands.downNpc).c_str(),
                      bench.npcCommands.downNpc->attribute(ATR_HITPOINTS));
      TestBench::note(buf);
      bench.npcCommands.downNpc = bench.npcCommands.downOther = nullptr;
      }
    if(bench.pc>=bench.prog.size())
      break;
    if(bench.time<bench.waitUntil || std::chrono::steady_clock::now()<bench.realWaitUntil || bench.npcCommands.volleyLeft>0 ||
       bench.settleSteps>0)
      return;
    if(bench.waitArrive) {
      if(!pl.isAiQueueEmpty() && bench.time<bench.arriveEnd)
        return;
      if(!pl.isAiQueueEmpty())
        say("waitarrive: timeout");
      bench.waitArrive = false;
      }
    if(!bench.holdCommand.empty() && bench.prog[bench.pc].rfind(bench.holdCommand,0)==0) {
      bench.heldNow = true;
      return;
      }
    const std::string line = bench.prog[bench.pc++];
    exec(line,w,pl,ctrl);
    if(bench.routeActive || bench.placedItems.probing || bench.waitStreaming || NavBench::running() || NavDemos::running() ||
       NpcDayJournal::running() || bench.npcTierTest.npc || bench.npcSpeedTest.npc) return;
    if(bench.movables.spellWaitFrozen>=0 || bench.movables.spellEquipPending) return;
    if(bench.routine.questWait || bench.routine.questStandingWait || bench.routine.routinePhase) return;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
    if(bench.cadenceWait) return;
#endif
    }
  if(bench.time>=bench.waitUntil && std::chrono::steady_clock::now()>=bench.realWaitUntil && !bench.waitArrive && bench.npcCommands.volleyLeft==0 && bench.npcCommands.downNpc==nullptr && bench.ragdoll.waitRagdollNpc==nullptr && !bench.duelWaiting && !bench.npcDuel.active && !bench.waitStreaming && bench.settleSteps==0 && bench.bodyLiveWait.empty())
    finish();
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
void drawZones(Canvas& cv, Camera* cam) {
  if(cam!=nullptr && (!bench.zonesNpc.empty() || bench.living.livingDebug)) {
    auto* w = Gothic::inst().world();
    const auto actors=bench.living.livingDebug ? bench.crew : std::vector<Npc*>{bench.zonesNpc=="player" ? Gothic::inst().player() : (w!=nullptr ? npcBySymbol(*w,bench.zonesNpc) : nullptr)};
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
void drawTrails(Canvas& cv, Camera* cam) {
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
  for(auto& tr:bench.trails) {
    const uint64_t age = bench.time>tr.last ? bench.time-tr.last : 0;
    if(age>bench.trailKeep || tr.pts.size()<2)
      continue;
    const float  k   = 1.f - float(age)/float(std::max<uint64_t>(bench.trailKeep,1));
    const Rgba   col = {255,214,120,uint8_t(235.f*k)};
    for(size_t i=1; i<tr.pts.size(); ++i) {
      float x0,y0,x1,y1;
      if(proj(tr.pts[i-1],x0,y0) && proj(tr.pts[i],x1,y1))
        cv.line(x0,y0,x1,y1,thick,col);
      }
    float x,y;
    if(age==0 && proj(tr.pts.back(),x,y))
      cv.dot(x,y,thick+1,{255,255,255,235});
    if(!bench.trailLine)
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

void drawOverlay(Pixmap& pm) {
  Canvas cv;
  cv.px = reinterpret_cast<uint8_t*>(pm.data());
  cv.w  = int(pm.w());
  cv.h  = int(pm.h());
  if(cv.h>0)
    bench.aspect = float(cv.w)/float(cv.h);
  auto  cam = Gothic::inst().camera();
  auto& p   = FootIk::playerProbe;
  if(bench.bright)
    autoExposure(cv);
  if(bench.zonesOnly || bench.trailOn) {
    drawZones(cv,cam);
    drawTrails(cv,cam);
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
  const auto& g = bench.subject!=nullptr ? Gaze::subjectProbe : Gaze::playerProbe;
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

  drawZones(cv,cam);

  // the manual aim: the crosshair as the game draws it in its interface (not captured): a chevron at where
  // it aims, brackets as far apart as the arrow may go off; a dot where the arrow points now (debugging)
  auto  hero = Gothic::inst().player();
  Vec3  cross;
  float spread = 0;
  if(cam!=nullptr && hero!=nullptr && bench.ctrl!=nullptr && bench.ctrl->crosshair(cross,spread)) {
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
    const bool centered = bench.ctrl->isCrosshairCentered();
    if(centered) {
      x = 0.5f*float(cv.w);
      y = 0.5f*float(cv.h);
      }
    if(centered || proj(cross,x,y)) {
      const float r = Crosshair::radius(spread,cam->fovY(),float(cv.h),bench.uiScale);
      for(auto& l:Crosshair::shape(x,y,r,bench.uiScale))
        cv.stroke(l.pt,l.alpha,Crosshair::pen(bench.uiScale));
      }
    }

  // zoomed copy of the feet in the bottom right corner
  if(bench.zoom && zx>=0 && zy>=0 && zx<float(cv.w) && zy<float(cv.h)) {
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
  std::snprintf(buf,sizeof(buf),"F%llu T%.2f",(unsigned long long)bench.recFrame,double(bench.time-bench.recStart)/1000.0);
  cv.text(6,6,buf,3,Text);
  cv.text(6,30,bench.recName,2,Text);
  if(!bench.label.empty())
    cv.text(6,46,bench.label,2,Text);

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
  if(g.valid && (g.active || bench.subject!=nullptr)) {
    std::snprintf(buf,sizeof(buf),"GAZE W%.2f E%.0f Y%.0f P%.0f",double(g.weight),double(g.error),double(g.yaw),double(g.pitch));
    cv.text(x,y,buf,2,{255,230,40,255});
    y += 16;
    std::snprintf(buf,sizeof(buf),"H%.0f C%.0f W%.0f",double(g.headYaw),double(g.chestYaw),double(g.waistYaw));
    cv.text(x,y,buf,2,{255,230,40,255});
    y += 16;
    }
  const auto& am = bench.subject!=nullptr ? Aim::subjectProbe : Aim::playerProbe;
  if(am.valid && am.active) {
    std::snprintf(buf,sizeof(buf),"AIM W%.2f E%.1f Y%.0f P%.0f T%.0f B%.0f L%.0f S%.0f",double(am.weight),double(am.error),double(am.yaw),
                  double(am.pitch),double(am.turn),double(am.bend),double(am.lean),double(am.side));
    cv.text(x,y,buf,2,{255,140,60,255});
    y += 16;
    }
  const auto& mp = Archer::playerProbe;
  if(mp.valid && bench.subject==nullptr) {
    std::snprintf(buf,sizeof(buf),"BOW %s D%.2f H%.1f F%.2f B%.1f%s%s A%.2f",archerStage(mp.stage),double(mp.draw),double(mp.hold),
                  double(mp.fatigue),double(mp.breath),mp.holding ? " HOLD" : "",mp.short_ ? " SHORT" : "",double(mp.amp));
    cv.text(x,y,buf,2,{140,200,255,255});
    y += 16;
    }
  const auto& bd = BowDraw::playerProbe;
  if(bd.valid && bd.weight>0.f && bench.subject==nullptr) {
    std::snprintf(buf,sizeof(buf),"DRAW%s W%.2f D%.2f S%.0f T%.0f M%.1f",BowDraw::isEnabled() ? "" : " OFF",double(bd.weight),
                  double(bd.draw),double(bd.span),double(bd.travel),double(bd.miss));
    cv.text(x,y,buf,2,{140,200,255,255});
    y += 16;
    }
  const auto& fl = bench.subject!=nullptr ? Flinch::subjectProbe : Flinch::playerProbe;
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
  const auto& ln = bench.subject!=nullptr ? Lean::subjectProbe : Lean::playerProbe;
  if(ln.valid) {
    std::snprintf(buf,sizeof(buf),"LEAN%s R%.1f P%.1f V%.0f T%.0f S%.0f",Lean::isEnabled() ? "" : " OFF",double(ln.roll),
                  double(ln.pitch),double(ln.speed),double(ln.turn),double(ln.slope));
    cv.text(x,y,buf,2,{120,255,160,255});
    y += 16;
    }
  const auto& an = bench.subject!=nullptr ? Pose::subjectProbe : Pose::playerProbe;
  if(an.valid) {
    std::snprintf(buf,sizeof(buf),"BLEND%s N%u A%.0f",AnimBlend::isEnabled() ? "" : " OFF",an.blend.active,double(an.blend.angle));
    cv.text(x,y,buf,2,{200,160,255,255});
    y += 16;
    }
  if(bench.occl!=0) {
    std::snprintf(buf,sizeof(buf),"HIDDEN %s%s",(bench.occl&1) ? "L" : "",(bench.occl&2) ? "R" : "");
    cv.text(x,y,buf,2,Red);
    }
  }

void writeVideo(const Pixmap& pm) {
  if(bench.video==nullptr) {
    bench.videoW = pm.w() & ~1u;
    bench.videoH = pm.h() & ~1u;
    const char* ff = envStr("OPENGOTHIC_BENCH_FFMPEG") ? envStr("OPENGOTHIC_BENCH_FFMPEG") : "ffmpeg";
    char cmd[2048];
    // OPENGOTHIC_BENCH_ENCODER: ffmpeg's video encoder options instead of x264 (the hardware encoder keeps
    // a large frame in real time: "-c:v h264_videotoolbox -b:v 24M")
    const char* enc = envStr("OPENGOTHIC_BENCH_ENCODER") ? envStr("OPENGOTHIC_BENCH_ENCODER") : "-c:v libx264 -preset veryfast -crf 18";
    std::snprintf(cmd,sizeof(cmd),
                  "\"%s\" -loglevel error -y -f rawvideo -pix_fmt rgba -s %ux%u -framerate %d -i - "
                  "%s -pix_fmt yuv420p \"%s/%s.mp4\"",
                  ff,bench.videoW,bench.videoH,Fps,enc,outDir.c_str(),bench.recName.c_str());
    std::FILE* f = popen(cmd,"w");
    if(f==nullptr) {
      say("can not start ffmpeg");
      bench.recVideo = false;
      return;
      }
    bench.video = std::make_unique<VideoPipe>(f);
    }
  const size_t row = size_t(bench.videoW)*4;
  std::vector<uint8_t> frame(row*bench.videoH,0);
  if(pm.w()>=bench.videoW && pm.h()>=bench.videoH && pm.data()!=nullptr) {
    auto px = reinterpret_cast<const uint8_t*>(pm.data());
    for(uint32_t j=0; j<bench.videoH; ++j)
      std::memcpy(frame.data() + j*row, px + size_t(j)*size_t(pm.w())*4, row);
    }
  bench.video->push(std::move(frame));
  }

}

bool TestBench::npcDuelActor(const Npc& npc) {
  return bench.npcDuel.active && (bench.npcDuel.actors[0]==&npc || bench.npcDuel.actors[1]==&npc);
  }

void TestBench::npcDuelSwingStart(const Npc& npc) {
  if(!npcDuelActor(npc))return;
  auto& duel=bench.npcDuel;
  const size_t i=duel.actors[0]==&npc ? 0 : 1;
  ++duel.swings[i];
  if(i==1 && duel.firstResponseMs<0)
    duel.firstResponseMs=int64_t(npc.world().tickCount()-duel.from);
  }

void TestBench::npcDuelSwingEnd(const Npc& npc, std::string_view outcome) {
  if(!npcDuelActor(npc))return;
  auto& duel=bench.npcDuel;
  const size_t i=duel.actors[0]==&npc ? 0 : 1;
  if(outcome=="blocked")++duel.blocked[i];
  else if(outcome!="hit" && outcome!="applied")++duel.misses[i];
  }

void TestBench::npcDuelHit(const Npc& victim, const Npc& by, int32_t hpBefore, int32_t hpAfter,
                           int32_t baseDamage, bool critical, float meleeScale, float stunBonus, float backstabMult) {
  if(!npcDuelActor(by) || !npcDuelActor(victim) || &by==&victim || hpBefore<=hpAfter)return;
  auto& duel=bench.npcDuel;
  auto& log=duel.hitLog;
  if(log.size()>1)log+=',';
  log+="{\"time_ms\":"+std::to_string(by.world().tickCount()-duel.from)+
    ",\"attacker\":"+jsonStr(who(&by))+",\"victim\":"+jsonStr(who(&victim))+
    ",\"hp_before\":"+std::to_string(hpBefore)+",\"hp_after\":"+std::to_string(hpAfter)+
    ",\"base_damage\":"+std::to_string(baseDamage)+",\"critical\":"+(critical ? "true" : "false")+
    ",\"melee_scale\":"+std::to_string(meleeScale)+",\"stun_bonus\":"+std::to_string(stunBonus)+
    ",\"backstab_mult\":"+std::to_string(backstabMult)+",\"hp_loss\":"+std::to_string(hpBefore-hpAfter)+"}";
  }

bool TestBench::isActive() {
  return scenarioPath!=nullptr;
  }

bool TestBench::strictGraphMovement(const Npc& npc) {
  return isActive() && (bench.npcTierTest.npc==&npc || bench.npcSpeedTest.npc==&npc);
  }

bool TestBench::isPhysicsNoiseDebug() {
  return bodyTraceAlive && bench.movables.noiseBridgeDebug;
  }

void TestBench::physicsNoiseBridgeEvent(const Npc& listener,uint64_t tick) {
  if(isPhysicsNoiseDebug() && &listener==bench.movables.noiseHost)bench.movables.noiseBridgeEvents.push_back(tick);
  }

bool TestBench::physicsNoiseDebug(World& world,Npc& player,std::string_view command,
                                  std::string& value,std::string& error) {
  const auto fail=[&](std::string_view message) {error=message;return false;};
  const auto resetCounts=[] {
    bench.movables.noiseBridgeImpacts=0;bench.movables.noiseBridgeLoudImpacts=0;bench.movables.noiseBridgeCulpritImpacts=0;
    bench.movables.noiseBridgeUnseenCulpritImpacts=0;bench.movables.noiseBridgeCupImpacts=0;bench.movables.noiseBridgeCupMaxRadius=0;
    bench.movables.noiseBridgeDialogueImpacts=0;bench.movables.noiseBridgeCombatImpacts=0;
    bench.movables.noiseBridgeHeard.clear();
    bench.movables.noiseBridgeEvents.clear();
    };
  if(command=="setup") {
    world.physic()->setMovableVobsEnabled(true);world.physic()->setPhysicsNoiseEnabled(true);
    prepareNoiseFixture(world,player);
    if(!bench.movables.movableReady || !bench.movables.noiseHost)return fail("Coragon tavern fixture unavailable in this save");
    world.script().skipImportantInfos(*bench.movables.noiseHost,player);
    player.handle().attribute[ATR_HITPOINTS]=100000;
    player.handle().attribute[ATR_HITPOINTSMAX]=100000;
    bench.movables.noiseBridgeDebug=true;
    }
  if(!bench.movables.noiseBridgeDebug || !bench.movables.noiseHost)return fail("physics_noise setup required");
  auto& npc=*bench.movables.noiseHost;
  if(command.ends_with("-ready")) {
    noiseRetireListeners();
    const bool quiet=command=="quiet-ready";
    if(command!="loud-ready" && command!="quiet-ready" && command!="dialogue-ready" &&
       command!="combat-ready" && command!="throttle-ready")return fail("unknown physics_noise ready mode");
    const auto source=quiet ? (bench.movables.movableWalkStart+bench.movables.movableWalkEnd)*.5f : bench.movables.movableCenter;
    noiseBegin(world,command,source);
    Vec3 point;
    if(!noiseFloor(world,source,quiet?600.f:500.f,false,point))return fail("supported clear listener floor unavailable");
    if(!noiseAddListener(world,"bridge",point,&npc))return fail("Coragon listener unavailable");
    // The real instigator is behind the listener together with the impact.
    // Its identity must be absent from the NPC's event even though native physics knows it.
    auto hero=source+Vec3(0,0,100);
    if(command=="dialogue-ready" || command=="combat-ready")hero=point+Vec3(0,0,80);
    const auto floor=world.physic()->landRay(hero+Vec3(0,250,0),600);
    if(floor.hasCol)hero.y=floor.v.y;
    player.setPosition(hero);player.setDirection(point-hero);player.updateTransform();
    if(command=="dialogue-ready") {
      npc.clearAiQueue();npc.aiPush(AiQueue::aiProcessInfo(player));
      }
    if(command=="combat-ready") {
      const auto state=world.script().findSymbolIndex("ZS_ATTACK");
      if(state==size_t(-1))return fail("native ZS_ATTACK unavailable");
      npc.clearAiQueue();npc.setTarget(&player);
      npc.aiPush(AiQueue::aiStartState(ScriptFn(state),0,&player,nullptr,""));
      }
    resetCounts();
    }
  else if(command=="loud-hit" || command=="dialogue-hit" || command=="combat-hit" || command=="throttle-hit") {
    if(command=="dialogue-hit" && !Gothic::inst().isNpcInDialog(npc))return fail("actual native dialogue has not opened");
    if(command=="combat-hit" && npc.weaponState()==WeaponState::NoWeapon)return fail("actual native combat has not drawn a weapon");
    if(command!="throttle-hit")resetCounts();
    noisePushTable(world,&player);
    }
  else if(command=="cup-hit") {resetCounts();noiseDropItems(world,1);}
  else if(command!="setup" && command!="sample")return fail("unknown physics_noise command");
  const bool routine=npc.currentState().isValid() && npc.isInRoutine(npc.currentState());
  std::string heard="[";
  for(auto tick:bench.movables.noiseBridgeHeard) {if(heard.size()>1)heard+=',';heard+=std::to_string(tick);}
  std::string bridgeEvents="[";
  for(auto tick:bench.movables.noiseBridgeEvents) {if(bridgeEvents.size()>1)bridgeEvents+=',';bridgeEvents+=std::to_string(tick);}
  value="{\"npc\":"+jsonStr(symName(world,&npc))+",\"position_cm\":"+ragdollVector(npc.position())+
    ",\"source_cm\":"+ragdollVector(bench.movables.noiseSource)+",\"distance_cm\":"+
    ragdollNumber((npc.position()-bench.movables.noiseSource).length())+",\"facing_error_deg\":"+
    ragdollNumber(noiseFacingError(npc,bench.movables.noiseSource))+",\"can_see_hero\":"+(npc.canSeeNpc(player,false)?"true":"false")+
    ",\"eligible\":"+(npc.canHearPhysicsNoise()?"true":"false")+
    ",\"external\":"+(npc.isExternalControl()?"true":"false")+
    ",\"investigating\":"+(npc.isPhysicsInvestigating()?"true":"false")+
    ",\"player_distance_cm\":"+ragdollNumber((npc.position()-player.position()).length())+
    ",\"dialogue\":"+(Gothic::inst().isNpcInDialog(npc)?"true":"false")+
    ",\"combat\":"+(npc.weaponState()!=WeaponState::NoWeapon?"true":"false")+
    ",\"ai_queue_empty\":"+(npc.isAiQueueEmpty()?"true":"false")+
    ",\"routine_state\":"+(routine?"true":"false")+",\"state\":"+jsonStr(stateName(world,npc))+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"native_impacts\":"+std::to_string(bench.movables.noiseBridgeImpacts)+
    ",\"native_loud_impacts\":"+std::to_string(bench.movables.noiseBridgeLoudImpacts)+
    ",\"native_culprit_impacts\":"+std::to_string(bench.movables.noiseBridgeCulpritImpacts)+
    ",\"native_unseen_culprit_loud_impacts\":"+std::to_string(bench.movables.noiseBridgeUnseenCulpritImpacts)+
    ",\"native_cup_impacts\":"+std::to_string(bench.movables.noiseBridgeCupImpacts)+
    ",\"native_cup_max_radius_cm\":"+ragdollNumber(bench.movables.noiseBridgeCupMaxRadius)+
    ",\"audible_dialogue_loud_impacts\":"+std::to_string(bench.movables.noiseBridgeDialogueImpacts)+
    ",\"audible_combat_loud_impacts\":"+std::to_string(bench.movables.noiseBridgeCombatImpacts)+
    ",\"native_heard_ms\":"+heard+"],\"bridge_event_ms\":"+bridgeEvents+"]}";
  return true;
  }

bool TestBench::physicsTracesDebug(World& world,Npc& player,std::string_view command,
                                  std::string& value,std::string& error) {
  const auto fail=[&](std::string_view message) {error=message;return false;};
  const auto park=[&](Npc& npc,const Vec3& point) {
    npc.clearAiQueue();npc.clearState(false);npc.stopWalking();npc.setInteraction(nullptr,true);
    npc.closeWeapon(true);npc.setPosition(point);npc.setExternalControl(false);
    npc.setDirection(bench.traceCenter-point);npc.updateTransform();
    npc.aiPush(AiQueue::aiWait(120000));
    };
  const auto floorAt=[&](Vec3& point) {
    const auto floor=world.physic()->landRay(point+Vec3(0,180,0),400);
    if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-bench.traceCenter.y)>180)return false;
    point.y=floor.v.y;
    for(const auto offset:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,0,40),Vec3(0,0,-40)})
      if(world.physic()->ray(point+Vec3(0,85,0),point+offset+Vec3(0,85,0)).hasCol)return false;
    return true;
    };
  if(command=="setup" || command.starts_with("reload ")) {
    world.physic()->setMovableVobsEnabled(true);
    Gothic::settingsSetI("GAME","arrow_sticking",1);
    bench.traceHost=nullptr;
    for(uint32_t i=0;i<world.npcCount();++i) {
      auto* npc=world.npcById(i);if(!npc)continue;
      auto name=symName(world,npc);
      for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.find("CORAGON")!=std::string::npos || name.find("KORAGON")!=std::string::npos) {bench.traceHost=npc;break;}
      }
    if(!bench.traceHost)return fail("trace fixture requires Coragon in Khorinis");
    const auto all=world.physic()->movableVobSnapshot();
    const DynamicWorld::MovableVobSnapshot* table=nullptr;
    if(command.starts_with("reload ")) {
      const auto zenId=uint32_t(std::stoul(std::string(command.substr(7))));
      for(const auto& body:all)if(body.zenId==zenId) {table=&body;break;}
      }
    else {
      float nearest=1400.f*1400.f;
      for(const auto& body:all) {
        if(body.className!="table" || body.zenId==uint32_t(-1))continue;
        const auto distance=(movablePosition(body.transform)-bench.traceHost->position()).quadLength();
        if(distance<nearest) {nearest=distance;table=&body;}
        }
      }
    if(!table)return fail("Coragon trace fixture lacks a native map table");
    bench.traceTable=table->zenId;bench.traceOrigin=table->origin;
    bench.traceCenter=(table->localBounds[0]+table->localBounds[1])*.5f;
    table->origin.project(bench.traceCenter);
    if(command=="setup" && !world.physic()->setMovableVobTransform(bench.traceTable,bench.traceOrigin))
      return fail("trace table baseline restoration failed");
    world.script().skipImportantInfos(*bench.traceHost,player);
    player.handle().attribute[ATR_HITPOINTS]=100000;player.handle().attribute[ATR_HITPOINTSMAX]=100000;
    bool wallFound=false;
    const auto from=bench.traceCenter+Vec3(0,120,0);
    for(unsigned i=0;i<96;++i) {
      const float angle=float(i)*float(M_PI)/48.f;
      const Vec3 direction(std::cos(angle),0,std::sin(angle));
      const auto hit=world.physic()->ray(from,from+direction*1000.f);
      if(!hit.hasCol || std::abs(hit.n.y)>.25f || (hit.v-from).length()<180)continue;
      bench.traceWall=hit.v;bench.traceShotDirection=direction;wallFound=true;break;
      }
    if(!wallFound)return fail("trace fixture lacks a real vertical tavern wall");
    bool supported=false;
    for(unsigned i=0;i<48;++i) {
      const float angle=float(i)*float(M_PI)/24.f;
      auto point=bench.traceCenter+Vec3(std::cos(angle)*400,0,std::sin(angle)*400);
      if(!floorAt(point))continue;
      park(*bench.traceHost,point);bench.traceVisible=point;supported=true;break;
      }
    if(!supported)return fail("trace fixture lacks supported observer floor");
    auto hero=bench.traceVisible+Vec3(100,0,0);
    if(floorAt(hero)) {player.setPosition(hero);player.updateTransform();}
    bench.traceReady=true;
    }
  if(!bench.traceReady || !bench.traceHost)return fail("physics_traces setup required");
  auto& npc=*bench.traceHost;
  if(command=="small" || command=="moved" || command=="tilted") {
    auto matrix=bench.traceOrigin;
    if(command=="tilted")matrix.rotate(35,0,0,1);
    else matrix.set(3,0,matrix.at(3,0)+(command=="small"?10.f:80.f));
    if(!world.physic()->setMovableVobTransform(bench.traceTable,matrix,&player))
      return fail("native trace table transform failed");
    }
  else if(command=="arrow" || command=="arrows") {
    const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
    if(symbol==size_t(-1))return fail("native trace arrow instance missing");
    Item arrow(world,symbol,Item::T_Inventory);
    const unsigned count=command=="arrow"?1:7;
    for(unsigned i=0;i<count;++i) {
      const auto to=bench.traceWall+Vec3(0,float(i)*8,0);
      const auto from=to-bench.traceShotDirection*80.f;
      auto& bullet=world.shootArrow(arrow,from,bench.traceShotDirection,DynamicWorld::bulletSpeed);
      bullet.setOrigin(&player);bullet.setPrecise(1.f);
      }
    }
  else if(command=="visible" || command=="blocked" || command=="far") {
    const auto traces=world.physicsTraces();
    std::vector<const World::PhysicsTrace*> scene;
    for(const auto& trace:traces)
      if((trace.position-bench.traceCenter).length()<1300.f)scene.push_back(&trace);
    if(scene.size()<2)return fail("trace observer placement requires table and wall arrow");
    bool found=false;
    // named arrays: a ?: over two initializer_lists copies the lists, and their arrays die with the expression
    static constexpr float farDistances[]={1700,1900,2100,2400};
    static constexpr float nearDistances[]={300,450,600,800,1000};
    const std::span<const float> distances=command=="far"?std::span<const float>(farDistances):
                                                          std::span<const float>(nearDistances);
    for(float distance:distances) {
      const unsigned steps=command=="far"?360:96;
      for(unsigned i=0;i<steps;++i) {
        const float angle=float(i)*float(M_PI)*2.f/float(steps);
        auto point=bench.traceCenter+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
        if(!floorAt(point))continue;
        park(npc,point);
        bool all=true,anyVisible=false;
        for(const auto* trace:scene) {
          bool visible=false;
          for(const auto& sight:trace->sightPoints)
            visible|=command=="visible"?npc.canRayHitPoint(sight,false):npc.canRayHitPoint(sight,true,2000.f);
          const float cm=(trace->position-npc.position()).length();
          anyVisible|=visible;
          if(command=="blocked")all&=!visible && cm<1500;
          else if(command=="far")all&=cm>1500;
          else all&=visible && cm<1500;
          }
        if(command=="far")all&=anyVisible;
        if(all) {found=true;break;}
        }
      if(found)break;
      }
    if(!found)return fail(command=="blocked"?"no supported observer behind a real wall":
                         command=="far"?"no supported visible observer beyond 15m":
                                        "no supported observer seeing both native traces");
    if(command=="visible")bench.traceVisible=npc.position();
    if(command=="blocked")bench.traceBlocked=npc.position();
    if(command=="far")bench.traceFar=npc.position();
    }
  else if(command!="setup" && !command.starts_with("reload ") && command!="sample")
    return fail("unknown physics_traces command");
  std::string rows="[";
  for(const auto& trace:world.physicsTraces()) {
    if((trace.position-bench.traceCenter).length()>2500)continue;
    bool visible=false;
    for(const auto& point:trace.sightPoints)visible|=npc.canRayHitPoint(point,true,2000.f);
    if(rows.size()>1)rows+=',';
    const auto* wp=world.findWayPoint(trace.position);
    rows+="{\"id\":"+jsonStr(trace.id)+",\"kind\":"+jsonStr(trace.kind)+",\"name\":"+jsonStr(trace.name)+
      ",\"waypoint\":"+jsonStr(wp?wp->name:"")+",\"position_cm\":"+ragdollVector(trace.position)+
      ",\"distance_cm\":"+ragdollNumber((trace.position-npc.position()).length())+
      ",\"appeared_at\":"+std::to_string(trace.appearedAt)+",\"actor_truth\":"+jsonStr(trace.culprit)+
      ",\"can_ray_hit_point\":"+(visible?"true":"false")+"}";
    }
  value="{\"npc\":"+jsonStr(symName(world,&npc))+",\"hero\":"+jsonStr(symName(world,&player))+
    ",\"table_zen_id\":"+std::to_string(bench.traceTable)+
    ",\"table_trace_id\":"+jsonStr(std::string(world.name())+":vob:"+std::to_string(bench.traceTable))+
    ",\"npc_position_cm\":"+ragdollVector(npc.position())+",\"table_center_cm\":"+ragdollVector(bench.traceCenter)+
    ",\"wall_cm\":"+ragdollVector(bench.traceWall)+",\"game_minutes\":"+std::to_string(world.time().toInt()/60000)+
    ",\"truth\":"+rows+"]}";
  return true;
  }

bool TestBench::destructionTracesDebug(World& world,Npc& player,std::string_view command,
                                      std::string& value,std::string& error) {
  if(command.starts_with("signal ")) {
    bench.brokenSignals.insert(std::string(command.substr(7)));
    value="{}";
    return true;
    }
  const auto fail=[&](std::string_view message) {error=message;return false;};
  const auto park=[&](Npc& npc,const Vec3& point) {
    npc.clearAiQueue();npc.clearState(false);npc.clearGoTo();npc.stopWalking();npc.setInteraction(nullptr,true);
    npc.closeWeapon(true);npc.setPosition(point);npc.setExternalControl(false);
    npc.setDirection(bench.destruction.destructionTraceSource-point);npc.updateTransform();
    npc.aiPush(AiQueue::aiWait(120000));
    };
  const auto floorAt=[&](Vec3& point) {
    const auto floor=world.physic()->landRay(point+Vec3(0,220,0),600);
    if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-bench.destruction.destructionTraceSource.y)>350)return false;
    point.y=floor.v.y;
    for(const auto offset:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,0,40),Vec3(0,0,-40)})
      if(world.physic()->ray(point+Vec3(0,85,0),point+offset+Vec3(0,85,0)).hasCol)return false;
    return true;
    };
  if(command=="setup" || command.starts_with("reload ")) {
    bench.destruction.destructionTraceDebug=false;bench.destruction.destructionMeleeRunning=false;
    bench.destruction.destructionTraceHost=nullptr;bench.destruction.destructionTraceWorld=nullptr;
    uint32_t zenId=19355;
    if(command.starts_with("reload ")) {
      try {zenId=uint32_t(std::stoul(std::string(command.substr(7))));}
      catch(const std::exception&) {return fail("destruction trace reload requires a map-vob zen id");}
      }
    Gothic::settingsSetI("GAME","destruction",1);
    world.physic()->setDestructionEnabled(true);
    const auto bodies=world.physic()->movableVobSnapshot();
    const auto found=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body){return body.zenId==zenId;});
    if(found==bodies.end() || found->className!="fence" || !found->destructible)
      return fail("destruction trace fixture requires the authored Lobart farm fence");
    if(command=="setup" && found->broken)return fail("destruction trace setup requires an intact farm fence");
    for(uint32_t i=0;i<world.npcCount();++i) {
      auto* npc=world.npcById(i);if(!npc)continue;
      auto name=symName(world,npc);
      for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.find("LOBART")!=std::string::npos) {bench.destruction.destructionTraceHost=npc;break;}
      }
    if(!bench.destruction.destructionTraceHost)return fail("destruction trace fixture requires Lobart");
    bench.destruction.destructionTraceZen=zenId;
    bench.destruction.destructionTraceSource=(found->localBounds[0]+found->localBounds[1])*.5f;
    found->transform.project(bench.destruction.destructionTraceSource);
    if(command=="setup") {
      destructionFarmSetup(bench.destruction,destructionContext(),world,player,zenId);
      if(bench.destruction.destructionFarmZen!=zenId || bench.destruction.destructionMeleeActor!=&player)
        return fail("destruction trace native sword stance preparation failed");
      }
    world.script().skipImportantInfos(*bench.destruction.destructionTraceHost,player);
    player.handle().attribute[ATR_HITPOINTS]=100000;player.handle().attribute[ATR_HITPOINTSMAX]=100000;
    bool supported=false;
    for(unsigned i=0;i<96;++i) {
      const float angle=float(i)*float(M_PI)/48.f;
      auto point=bench.destruction.destructionTraceSource+Vec3(std::cos(angle)*500,0,std::sin(angle)*500);
      if(!floorAt(point))continue;
      park(*bench.destruction.destructionTraceHost,point);supported=true;break;
      }
    if(!supported)return fail("destruction trace fixture lacks supported observer floor");
    bench.destruction.destructionTraceWorld=&world;bench.destruction.destructionTraceDebug=true;
    }
  if(!bench.destruction.destructionTraceDebug || bench.destruction.destructionTraceWorld!=&world || !bench.destruction.destructionTraceHost)
    return fail("destruction_traces setup or reload required");
  auto& npc=*bench.destruction.destructionTraceHost;
  const auto traceId=std::string(world.name())+":vob:"+std::to_string(bench.destruction.destructionTraceZen);
  if(command=="break") {
    if(bench.destruction.destructionFarmZen!=bench.destruction.destructionTraceZen || bench.destruction.destructionMeleeActor!=&player)
      return fail("destruction trace break requires the original intact setup");
    if(bench.destruction.destructionMeleeRunning)return fail("destruction trace native sword attack is already running");
    const auto bodies=world.physic()->movableVobSnapshot();
    const auto found=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body){return body.zenId==bench.destruction.destructionTraceZen;});
    if(found==bodies.end() || found->broken)return fail("destruction trace break requires its intact farm fence");
    bench.destruction.destructionMeleeRunning=true;bench.destruction.destructionMeleeNext=world.tickCount();
    bench.destruction.destructionMeleeDeadline=world.tickCount()+90000;
    if(scenarioPath!=nullptr)bench.waitUntil=bench.time+90034;
    }
  else if(command=="visible" || command=="blocked") {
    const auto traces=world.physicsTraces();
    const auto source=std::find_if(traces.begin(),traces.end(),[&](const auto& trace){return trace.id==traceId && trace.kind=="broken";});
    if(source==traces.end())return fail("destruction trace observer placement requires broken native debris");
    bool found=false;
    for(float distance:{300.f,450.f,600.f,800.f,1000.f,1250.f,1400.f}) {
      for(unsigned i=0;i<192;++i) {
        const float angle=float(i)*float(M_PI)/96.f;
        auto point=bench.destruction.destructionTraceSource+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
        if(!floorAt(point))continue;
        park(npc,point);
        if((source->position-point).length()>1500.f)continue;
        bool visible=false;
        for(const auto& sight:source->sightPoints)
          visible|=npc.canRayHitPoint(sight,command=="blocked",2000.f);
        if((command=="visible" && visible) || (command=="blocked" && !visible)) {found=true;break;}
        }
      if(found)break;
      }
    if(!found)return fail(command=="blocked"?"no supported Lobart position behind a real farm wall":
                                             "no supported Lobart position seeing the native debris");
    }
  else if(command!="setup" && !command.starts_with("reload ") && command!="sample")
    return fail("unknown destruction_traces command");
  std::string rows="[";
  for(const auto& trace:world.physicsTraces()) {
    if(trace.id!=traceId || trace.kind!="broken")continue;
    bool visible=false;
    for(const auto& point:trace.sightPoints)visible|=npc.canRayHitPoint(point,true,2000.f);
    const auto* wp=world.findWayPoint(trace.position);
    if(rows.size()>1)rows+=',';
    rows+="{\"id\":"+jsonStr(trace.id)+",\"kind\":"+jsonStr(trace.kind)+",\"name\":"+jsonStr(trace.name)+
      ",\"waypoint\":"+jsonStr(wp?wp->name:"")+",\"position_cm\":"+ragdollVector(trace.position)+
      ",\"distance_cm\":"+ragdollNumber((trace.position-npc.position()).length())+
      ",\"appeared_at\":"+std::to_string(trace.appearedAt)+",\"actor_truth\":"+jsonStr(trace.culprit)+
      ",\"can_ray_hit_point\":"+(visible?"true":"false")+"}";
    }
  value="{\"npc\":"+jsonStr(symName(world,&npc))+",\"hero\":"+jsonStr(symName(world,&player))+
    ",\"zen_id\":"+std::to_string(bench.destruction.destructionTraceZen)+",\"trace_id\":"+jsonStr(traceId)+
    ",\"source_position_cm\":"+ragdollVector(bench.destruction.destructionTraceSource)+
    ",\"npc_position_cm\":"+ragdollVector(npc.position())+
    ",\"game_minutes\":"+std::to_string(world.time().toInt()/60000)+
    ",\"melee_running\":"+(bench.destruction.destructionMeleeRunning?"true":"false")+
    ",\"melee_attempts\":"+std::to_string(bench.destruction.destructionMeleeAttempts)+",\"truth\":"+rows+"]}";
  return true;
  }

void TestBench::destructionBreak(uintptr_t body,size_t pieces,float milliseconds) {
  if(scenarioPath==nullptr || !bench.destruction.destructionTrace || !destructionOwns(bench.destruction,body))return;
  auto* world=Gothic::inst().world();
  if(world)destructionFields(bench.destruction,destructionContext(),*world);
  bench.destruction.destructionLastBreak=world?world->tickCount():0;bench.destruction.destructionSleepSampled=false;
  event("destruction_break","\"case\":"+jsonStr(bench.destruction.destructionCase)+",\"body\":"+std::to_string(body)+
    ",\"pieces\":"+std::to_string(pieces)+",\"break_ms\":"+ragdollNumber(milliseconds)+
    ",\"wall_ns\":"+std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count())+
    ",\"t_game_ms\":"+std::to_string(world?world->tickCount():0)+",\"arrows\":"+std::to_string(bench.destruction.destructionArrows));
  // Not inside a timed window: loading the pieces would land in the measured break frame.
  if(world && !bench.movables.movableMeasuring)fracturePiecesBreak(bench.fracturePieces,*world,body,bench.destruction.destructionCase);
  }

void TestBench::destructionRetire(uintptr_t body,size_t piece,bool expired,float observerDistanceCm) {
  if(scenarioPath==nullptr || !bench.destruction.destructionTrace || !destructionOwns(bench.destruction,body))return;
  auto* world=Gothic::inst().world();
  event("destruction_fragment_retired","\"case\":"+jsonStr(bench.destruction.destructionCase)+",\"parent_id\":"+std::to_string(body)+
    ",\"piece\":"+std::to_string(piece)+",\"sleeping\":true,\"reason\":"+jsonStr(expired?"expired":"distant")+
    ",\"observer_distance_cm\":"+ragdollNumber(observerDistanceCm)+",\"t_game_ms\":"+std::to_string(world?world->tickCount():0));
  }

void TestBench::destructionMeleeHit(const Npc& actor,size_t affected,float damage) {
  if(scenarioPath==nullptr || !bench.destruction.destructionMeleeRunning || bench.destruction.destructionMeleeActor!=&actor)return;
  if(affected)++bench.destruction.destructionMeleeHits;
  auto* world=Gothic::inst().world();
  event("destruction2_melee_hit","\"case\":"+jsonStr(bench.destruction.destructionCase)+",\"affected\":"+
    std::to_string(affected)+",\"hits\":"+std::to_string(bench.destruction.destructionMeleeHits)+
    ",\"raw_hit_power\":"+ragdollNumber(damage)+",\"bodies\":"+(world?destructionRows(bench.destruction,*world):"[]"));
  }

void TestBench::movementImpulseHandoff(const Npc& actor,std::string_view backend,uint32_t fromState,
                                      uint32_t toState,const Vec3& velocity) {
  if(scenarioPath==nullptr || !bench.started || bench.done)return;
  auto* world=Gothic::inst().world();
  if(!world)return;
  event("movement_impulse_handoff","\"actor\":"+jsonStr(symName(*world,&actor))+
    ",\"actor_id\":"+std::to_string(world->npcId(&actor))+",\"backend\":"+jsonStr(backend)+
    ",\"from_state\":"+std::to_string(fromState)+",\"to_state\":"+std::to_string(toState)+
    ",\"impulse_velocity_cm_s\":"+ragdollVector(velocity));
  }

void TestBench::physicsNoise(const PhysicsNoise& noise,std::string_view sound,bool played) {
  if(bench.movables.noiseBridgeDebug && (noise.position-bench.movables.noiseSource).quadLength()<250.f*250.f) {
    ++bench.movables.noiseBridgeImpacts;
    if(noise.volume>=.35f)++bench.movables.noiseBridgeLoudImpacts;
    if(noise.culprit)++bench.movables.noiseBridgeCulpritImpacts;
    if(noise.what.find("BEER")!=std::string::npos) {
      ++bench.movables.noiseBridgeCupImpacts;
      bench.movables.noiseBridgeCupMaxRadius=std::max(bench.movables.noiseBridgeCupMaxRadius,noise.radius);
      }
    if(auto* world=Gothic::inst().world();world && bench.movables.noiseHost && noise.volume>=.35f) {
      auto& npc=*bench.movables.noiseHost;
      const float radius=noise.radius*std::max(0.f,1.f-world->physic()->soundOclusion(npc.mapHeadBone(),noise.position));
      if(npc.qDistTo(noise.position)<=radius*radius) {
        if(noise.culprit && !npc.canSeeNpc(*noise.culprit,false))++bench.movables.noiseBridgeUnseenCulpritImpacts;
        if(Gothic::inst().isNpcInDialog(npc))++bench.movables.noiseBridgeDialogueImpacts;
        if(npc.weaponState()!=WeaponState::NoWeapon && stateName(*world,npc)=="ZS_ATTACK")++bench.movables.noiseBridgeCombatImpacts;
        }
      }
    }
  if(scenarioPath==nullptr || !bench.movables.noiseTrace)return;
  if(bench.movables.movableMeasuring) {++bench.movables.noiseMeasuredImpacts;return;}
  const char* material="unknown";
  switch(noise.material) {
    case MAT_WOOD:material="wood";break;
    case MAT_METAL:material="metal";break;
    case MAT_STONE:material="stone";break;
    case MAT_CLAY:material="clay";break;
    case MAT_GLAS:material="glass";break;
    case MAT_LEATHER:material="leather";break;
    default:break;
    }
  auto* world=Gothic::inst().world();
  std::string listeners="[";
  if(world)for(const auto& listener:bench.movables.noiseListeners) {
    if(listeners.size()>1)listeners+=',';
    const auto head=listener.npc->mapHeadBone();
    const auto occlusion=world->physic()->soundOclusion(head,noise.position);
    listeners+="{\"role\":"+jsonStr(listener.role)+",\"npc_id\":"+std::to_string(noiseNpcId(listener.npc))+
      ",\"distance_cm\":"+ragdollNumber((head-noise.position).length())+
      ",\"occlusion\":"+ragdollNumber(occlusion)+",\"effective_radius_cm\":"+
      ragdollNumber(noise.radius*(1.f-occlusion))+"}";
    }
  event("physics_noise","\"phase\":"+jsonStr(bench.movables.noisePhase)+",\"t_game_ms\":"+
    std::to_string(world?world->tickCount():0)+",\"body\":"+std::to_string(noise.body)+
    ",\"position_cm\":"+ragdollVector(noise.position)+",\"volume\":"+ragdollNumber(noise.volume)+
    ",\"radius_cm\":"+ragdollNumber(noise.radius)+",\"impulse_ns\":"+ragdollNumber(noise.impulse)+
    ",\"material\":"+jsonStr(material)+",\"surface_material\":"+std::to_string(int(noise.surface))+
    ",\"what\":"+jsonStr(noise.what)+",\"culprit\":"+
    (world && noise.culprit?std::to_string(noiseNpcId(noise.culprit)):"null")+
    ",\"sound\":"+jsonStr(sound)+",\"played\":"+(played?"true":"false")+
    ",\"listeners\":"+listeners+"]");
  }

void TestBench::physicsNoiseHeard(const Npc& npc,const PhysicsNoise& noise,bool callbackDispatched) {
  if(bench.movables.noiseBridgeDebug && &npc==bench.movables.noiseHost)
    if(auto* world=Gothic::inst().world())bench.movables.noiseBridgeHeard.push_back(world->tickCount());
  if(scenarioPath==nullptr || !bench.movables.noiseTrace)return;
  if(bench.movables.movableMeasuring) {
    ++bench.movables.noiseMeasuredHeard;
    for(auto& listener:bench.movables.noiseListeners)
      if(listener.npc==&npc)++listener.heard;
    return;
    }
  auto* world=Gothic::inst().world();if(!world)return;
  const auto id=noiseNpcId(&npc);
  ++bench.movables.noisePerceptions[id];
  std::string_view role;
  for(auto& listener:bench.movables.noiseListeners)
    if(listener.npc==&npc) {++listener.heard;role=listener.role;}
  event("physics_noise_heard","\"phase\":"+jsonStr(bench.movables.noisePhase)+",\"t_game_ms\":"+
    std::to_string(world->tickCount())+",\"npc_id\":"+std::to_string(id)+",\"role\":"+jsonStr(role)+
    ",\"body\":"+std::to_string(noise.body)+",\"position_cm\":"+ragdollVector(noise.position)+
    ",\"perception\":"+std::to_string(int(PERC_ASSESSQUIETSOUND))+
    ",\"callback_dispatched\":"+(callbackDispatched?"true":"false")+
    ",\"culprit\":"+(noise.culprit?std::to_string(noiseNpcId(noise.culprit)):"null"));
  }

void TestBench::configure(std::string scenario, std::string output, int64_t seedOverride) {
  stopBodyLive();
  benchTexturesSettle=false;
  benchTexturesFrozen=false;
  npcCharacterStopMotionTrace(bench.character,Gothic::inst().world());
  KnownRoadsBench::reset();
  NavigationMemoryBench::reset();
  NavigationEyeBench::reset();
  NpcKeysBench::reset();
  BodyTracesBench::reset();
  BloodStainsBench::reset();
  NavBench::reset();
  NavDemos::reset();
  stopRecording();
  if(bench.csv!=nullptr)
    std::fclose(bench.csv);
  if(bench.events!=nullptr)
    std::fclose(bench.events);
  if(logFile!=nullptr)
    std::fclose(logFile);
  logFile = nullptr;
  // Reconstruct all scenario-owned buffers/counters/pointers, including terminal and RNG state.
  bench.~Bench();
  new(&bench) Bench();
  bodyTraceAlive = true;
  static std::string configuredScenario;
  configuredScenario = std::move(scenario);
  scenarioPath = configuredScenario.c_str();
  outDir = std::move(output);
  const auto traceSetting = envStr("OPENGOTHIC_BODY_TRACE");
  bodyTrace = traceSetting!=nullptr ? std::string_view(traceSetting)=="1" : bodyTrace || Resources::isHeadless();
  Pose::probeNpc = nullptr;
  loadScenario();
  if(seedOverride>=0) {
    for(auto& line:bench.prog) {
      if(line.rfind("bodyduel ",0)!=0)
        continue;
      auto args = split(line);
      line = "bodyduel " + std::to_string(seedOverride) + " " + (args.size()>2 ? args[2] : "60000");
      }
    }
  }

void TestBench::holdBefore(std::string command) {
  bench.holdCommand = std::move(command);
  bench.heldNow = false;
  }

bool TestBench::held() { return bench.heldNow; }

void TestBench::flushOutput() {
  for(std::FILE* file:{bench.events,bench.csv,logFile})
    if(file!=nullptr)
      std::fflush(file);
  }

void TestBench::retarget(std::string output) {
  // The template flushed these before forking: closing the fork's copies writes nothing twice.
  for(std::FILE** file:{&bench.events,&bench.csv,&logFile})
    if(*file!=nullptr) {
      std::fclose(*file);
      *file = nullptr;
      }
  outDir = std::move(output);
  }

void TestBench::resumeHeld() {
  auto world  = Gothic::inst().world();
  auto player = Gothic::inst().player();
  if(!bench.heldNow || world==nullptr || player==nullptr || bench.ctrl==nullptr)
    throw std::runtime_error("no held scenario step to resume");
  bench.holdCommand.clear();
  bench.heldNow = false;
  run(*world,*player,*bench.ctrl);
  }

bool TestBench::done() { return bench.done; }

void TestBench::endWait(std::string_view reason) {
  if(scenarioPath==nullptr || bench.done)
    return;
  bench.waitUntil = bench.time;
  bench.realWaitUntil={};
  say("wait ended: " + std::string(reason));
  }

uint64_t TestBench::stepCount() { return bench.step; }
uint64_t TestBench::nativeChoices() { return bench.nativeChoices; }

std::vector<Npc*> TestBench::bodyObservationActors() {
  if(BodyArena::enabled())
    return BodyArena::actors();
  if(!bench.observeActors.empty())
    return bench.observeActors;
  if(bench.duelA!=nullptr && bench.duelB!=nullptr)
    return {bench.duelA,bench.duelB};
  return {};
  }

bool TestBench::bodyTracing() {
  return bodyTrace && !bench.navigationOnly && bodyTraceAlive && scenarioPath!=nullptr && bench.started && !bench.done;
  }

void TestBench::bodyEvent(const Npc& actor, const Npc* target, std::string_view phase, uint64_t gameTime,
                          const std::string& fields, std::string_view animation) {
  if(!bodyTracing())
    return;
  if(phase=="native_choice")
    ++bench.nativeChoices;
  if(phase=="body_impact" && (bench.bodyImpact.duel || (bench.bodyImpact.tracing && bench.bodyImpact.npc==&actor))) {
    rapidjson::Document sample;
    const std::string row="{"+fields+"}";sample.Parse(row.c_str());
    if(!sample.HasParseError() && sample.IsObject() && sample.HasMember("damage") && sample["damage"].IsInt() &&
       sample.HasMember("speed_cm_s") && sample["speed_cm_s"].IsNumber()) {
      if(bench.bodyImpact.tracing && bench.bodyImpact.npc==&actor) {
        bench.bodyImpact.damage+=sample["damage"].GetInt();++bench.bodyImpact.impacts;
        if(sample.HasMember("source") && sample["source"].IsString() &&
           std::string_view(sample["source"].GetString())=="spell_capsule")bench.bodyImpact.capsuleDamage+=sample["damage"].GetInt();
        bench.bodyImpact.maximumSpeed=std::max(bench.bodyImpact.maximumSpeed,sample["speed_cm_s"].GetFloat());
        }
      for(auto& body:bench.bodyImpact.duelBodies)if(body.npc==&actor && body.knocked) {
        body.damage+=sample["damage"].GetInt();body.speed=std::max(body.speed,sample["speed_cm_s"].GetFloat());
        }
      }
    }
  const auto id = [](const Npc* npc) -> uint64_t {
    if(npc==nullptr)
      return 0;
    auto it = bench.bodyIds.find(npc);
    if(it==bench.bodyIds.end())
      it = bench.bodyIds.emplace(npc,++bench.nextBodyId).first;
    return it->second;
    };
  const auto actorId = id(&actor);
  const auto targetId = id(target);
  event("body_combat","\"schema_version\":1,\"seq\":" + std::to_string(++bench.bodySeq) +
                     ",\"t_game\":" + std::to_string(gameTime) + ",\"step\":" + std::to_string(bench.step) +
                     ",\"actor_id\":" + std::to_string(actorId) + ",\"actor\":" + jsonStr(who(&actor)) +
                     ",\"target_id\":" + std::to_string(targetId) + ",\"target\":" + jsonStr(who(target)) +
                     ",\"phase\":" + jsonStr(phase) + ",\"animation\":" + jsonStr(animation) +
                     (fields.empty() ? "" : ",") + fields);
  if(phase=="native_attempt")
    observeAnimationAttempt(actor,fields,gameTime);
  BodyArena::noteCombat(actor,target,phase,fields);
  }

void TestBench::bodyForget(const Npc* npc) {
  if(bodyTraceAlive && bench.melee.meleeAiResponse &&
     (bench.melee.meleeAiResponse->npc==npc || bench.melee.meleeAiResponse->attacker==npc)) bench.melee.meleeAiResponse.reset();
  if(bodyTraceAlive && (bench.npcCommands.stunDrift.target==npc || bench.npcCommands.stunDrift.attacker==npc)) bench.npcCommands.stunDrift={};
  if(bodyTraceAlive) {
    if(bench.monster.monsterCharacterRoute.npc==npc)bench.monster.monsterCharacterRoute.npc=nullptr;
    for(auto& actor:bench.monster.monsterCharacterExtras)if(actor.npc==npc)actor.npc=nullptr;
    if(bench.character.characterIdle.actor==npc)bench.character.characterIdle.actor=nullptr;
    if(bench.character.characterIdle.fixtureNpc==npc)bench.character.characterIdle.fixtureNpc=nullptr;
    bench.character.npcCharacterGoals.erase(npc);bench.character.npcCharacterWalkers.erase(npc);
    std::erase_if(bench.character.npcCharacterTraceActors,[npc](const auto& actor){return actor.npc==npc;});
    }
  if(bodyTraceAlive && bench.movables.noiseTrace)bench.movables.noiseNpcIds.erase(npc);
  if(bodyTracing() || (ragdollTrace && bodyTraceAlive && bench.started && !bench.done)) {
    bench.bodyIds.erase(npc);
    for(auto it=bench.ragdoll.savedRagdollNpcs.begin();it!=bench.ragdoll.savedRagdollNpcs.end();) {
      if(it->second.npc==npc) it=bench.ragdoll.savedRagdollNpcs.erase(it); else ++it;
      }
    for(auto& actor:bench.bodyAnimation.actors)
      if(actor.npc==npc)
        actor.npc = nullptr;
    }
  }

std::string TestBench::bodyJson(std::string_view text) {
  return jsonStr(text);
  }

int TestBench::bodyBlockVariant(const Npc& npc, int fallback) {
  auto& control = bench.bodyAnimation;
  if(!bodyTracing() || !control.active || control.mode==BodyAnimationControl::Default)
    return fallback;
  const unsigned side = animationActorIndex(npc);
  if(side>=control.actorCount)
    return fallback;
  auto& actor = control.actors[side];
  ++actor.solverCalls;
  const uint64_t now = Gothic::inst().gameSession()->tickCount();
  const uint64_t time = now-bench.duelFrom;
  int variant = fallback;
  BodyAnimationEntry* entry = nullptr;
  if(control.mode==BodyAnimationControl::Isolated) {
    actor.rng ^= actor.rng<<13;
    actor.rng ^= actor.rng>>17;
    actor.rng ^= actor.rng<<5;
    variant = int(actor.rng%3);
    ++actor.controlledCalls;
    }
  else {
    entry = animationFixtureEntry(side,time);
    if(entry!=nullptr) {
      variant = entry->variant; // repeated continuation/start solves in one tick use the same entry
      ++actor.controlledCalls;
      }
    else
      ++actor.fallbackCalls;
    }
  actor.lastTime = time;
  actor.lastVariant = variant;
  bodyEvent(npc,npc.target(),"animation_branch_draw",now,
            "\"mode\":"+jsonStr(animationModeName())+",\"seed\":"+std::to_string(control.seed)+
            ",\"role\":"+jsonStr(side%2==0 ? "actor" : "target")+",\"relative_time_ms\":"+std::to_string(time)+
            ",\"fallback_variant\":"+std::to_string(fallback)+",\"selected_variant\":"+std::to_string(variant)+
            ",\"fixture_matched\":"+(entry==nullptr ? "false" : "true")+
            ",\"source_seq\":"+std::to_string(entry==nullptr ? 0 : entry->sourceSeq)+animationActorFields(side));
  return variant;
  }

void TestBench::ragdollBirthPeak(const Npc& npc, const Npc* culprit, const std::string& fields) {
  if(!isActive() || !bench.started || bench.done) return;
  event("ragdoll_birth_peak","\"actor\":"+jsonStr(who(&npc))+",\"culprit\":"+(culprit ? jsonStr(who(culprit)) : "null")+
    ",\"guard_enabled\":"+(Gothic::physicsFlags().ragdollBirthGuard ? "true" : "false")+","+fields);
  }

void TestBench::note(const char* text) {
  if(scenarioPath!=nullptr && bench.started && !bench.done) {
    say(text);
    event("note","\"text\":"+jsonStr(text));
    // releaseArrow reports this note immediately after releasing the real projectile.
    // Sample the feet and the applied aim pose here, rather than at the script's
    // earlier `release action` (which can precede the shot by an animation frame).
    auto* player = Gothic::inst().player();
    if(player!=nullptr && player->hasManualAim() &&
       std::string_view(text).starts_with("arrow shot: ") &&
       std::string_view(text).ends_with(", by "+std::string(who(player)))) {
      const float yaw = player->rotationRad();
      const auto edge = player->aimEdge(Vec3(std::cos(yaw),0,std::sin(yaw)),2000.f);
      char buf[256];
      std::snprintf(buf,sizeof(buf),"shot pose %s: edge_facing_cm %.2f edge_found %d facing_yaw_deg %.2f a_lean_deg %.2f",
                    bench.recName.c_str(),edge.found ? double(edge.distance) : -1.0,int(edge.found),
                    double(player->rotation()),double(Aim::playerProbe.lean));
      say(buf);
      event("note","\"text\":"+jsonStr(buf));
      }
    if(bench.archery.arrowKoOnHit && std::string_view(text).starts_with("arrow hit ") && bench.archery.arrowNpc!=nullptr &&
       bench.archery.arrowNpc->arrowPhysicsProbe().hits>bench.archery.arrowSeenHits && !bench.archery.arrowNpc->isDown()) {
      // This note runs inside real projectile damage, after queueArrowPhysics
      // and before the next sequential native prepare. Ordinary arrows kill;
      // only the benchmark forces KO to exercise the same pending-hit handoff.
      bench.archery.arrowKoOnHit=false;
      bench.archery.arrowNpc->setRagdollDown(false);
      event("arrow_reaction_forced_ko","\"case\":"+jsonStr(bench.archery.arrowCase)+
        ",\"hits\":"+std::to_string(bench.archery.arrowNpc->arrowPhysicsProbe().hits));
      }
    }
  }

void TestBench::ownMeshArrowHit(uint32_t zenId,const Vec3& point,const Vec3& normal) {
  if(!isActive() || !bench.started || bench.done || bench.ownMesh.ownMeshZen==uint32_t(-1))return;
  const auto delta=point-bench.ownMesh.ownMeshAxis;
  const bool matched=zenId==bench.ownMesh.ownMeshZen;
  if(matched)++bench.ownMesh.ownMeshArrowHits;
  event("own_mesh_arrow_hit","\"zen_id\":"+std::to_string(zenId)+
    ",\"expected_zen_id\":"+std::to_string(bench.ownMesh.ownMeshZen)+",\"matched\":"+(matched?"true":"false")+
    ",\"point_cm\":"+ragdollVector(point)+",\"normal\":"+ragdollVector(normal)+
    ",\"axis_distance_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+
    ",\"half_height_error_cm\":"+ragdollNumber(delta.y));
  }

std::string_view TestBench::who(const Npc* npc) {
  auto w = Gothic::inst().world();
  if(npc==nullptr || w==nullptr)
    return "none";
  auto sym = w->script().findSymbol(size_t(npc->instanceSymbol()));
  return sym!=nullptr ? std::string_view(sym->name()) : std::string_view("npc");
  }

bool TestBench::filming() {
  return scenarioPath!=nullptr && realtime;
  }

bool TestBench::deterministic() { return bench.started && !bench.done && bench.deterministic; }

bool TestBench::simulationFrozen() { return bench.started && !bench.done && bench.frozen; }

bool TestBench::orderedVisibility() { return bench.ordered; }

bool TestBench::takeRenderReset() {
  const bool r=bench.renderReset;
  bench.renderReset=false;
  return r;
  }

uint64_t TestBench::frameDt(uint64_t dt) {
  if(!bench.started || bench.done)
    return dt;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  if(AppCadence::normalClock()) { ++bench.step;bench.frameMs=dt;return dt; }
#endif
  ++bench.step;
  if(realtime) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if(bench.step==1)
      bench.wallStart = now;
    const auto due = bench.wallStart + microseconds(int64_t(bench.step)*1000000/Fps);
    if(now<due)
      std::this_thread::sleep_until(due); else
      bench.lagMs = std::max<int64_t>(bench.lagMs,duration_cast<milliseconds>(now-due).count());
    }
  bench.frameMs = (bench.step*1000)/Fps - ((bench.step-1)*1000)/Fps;
  const auto healingStep=[&](uint64_t step) {
    const auto* world=Gothic::inst().world();
    if(world && bench.healing.test.strikeAt>world->tickCount())
      return std::min(step,bench.healing.test.strikeAt-world->tickCount());
    return step;
    };
  if(bench.slow<=1.f)
    return healingStep(bench.frameMs);
  bench.slowAcc += uint64_t(float(bench.frameMs*1000)/bench.slow);
  const uint64_t dtGame = bench.slowAcc/1000;
  bench.slowAcc -= dtGame*1000;
  return healingStep(dtGame);
  }

void TestBench::beforeNpcTick(Npc& npc) {
  if(scenarioPath==nullptr || bench.done || !bench.healing.test.strikeAt || !bench.ctrl || !npc.isPlayer())
    return;
  auto& world=npc.world();
  if(world.tickCount()<bench.healing.test.strikeAt)
    return;
  bench.healing.test.strikeAt=0;
  exec("strike player BDT_1013_BANDIT_L",world,npc,*bench.ctrl);
  healingObserve(bench.healing,world,npc);
  }

void TestBench::beforeWorldTick(uint64_t dt) {
  // Keep the native observer at the fixture's supported point. Nearby enemies
  // retain their ordinary behaviour and cannot push this measurement out of range.
  if(scenarioPath!=nullptr && bench.started && !bench.done && bench.clothTrace && bench.clothHold) {
    if(auto* player=Gothic::inst().player()) {
      player->stopWalking();player->clearAiQueue();
      player->setPosition(bench.clothHeldPosition);player->updateTransform();
      }
    }
  // A reload observer accepts no live keyboard or mouse movement. Its actor
  // remains subject to ordinary native support and gravity, with no pose pin.
  if(scenarioPath!=nullptr && bench.started && !bench.done && bench.water.observer && bench.ctrl)
    bench.ctrl->clearInput();
  // The live bridge stand has no bench program, but uses the same real sword
  // animation and its normal hit frame as the scripted destruction fixture.
  if(scenarioPath==nullptr && bench.destruction.destructionTraceDebug && bench.destruction.destructionMeleeRunning && dt!=0) {
    auto* world=Gothic::inst().world();
    if(world && world==bench.destruction.destructionTraceWorld)destructionMeleeTick(bench.destruction,destructionContext(),*world);
    else {bench.destruction.destructionTraceDebug=false;bench.destruction.destructionMeleeRunning=false;}
    }
  const bool destructionCast=bench.destruction.destructionTrace &&
                             (bench.destruction.destructionCase=="fireball" || bench.destruction.destructionCase=="five");
  const bool movableCast=bench.movables.movableTrace && bench.movables.movablePhase=="blast";
  const bool waterCast=bench.water.ready && !bench.movables.spellActors.empty() && bench.movables.spellActors[0].role=="water_target";
  if(scenarioPath!=nullptr && bench.started && !bench.done && (destructionCast || movableCast || waterCast) && !bench.movables.spellActors.empty()) {
    // PlayerControl refreshes auto-focus after each world step. Keep this
    // fixture's selected native target until commitSpell has emitted the real
    // projectile, so a nearby tavern host cannot steal the cast mid-animation.
    if(auto* world=Gothic::inst().world();world && world->player())world->player()->setTarget(bench.movables.spellActors[0].npc);
    }
  if(scenarioPath==nullptr || !bench.started || bench.done || !bench.character.characterRideMoving || !bench.character.characterPlatform || dt==0)return;
  auto matrix=bench.character.characterRideMatrixStart;
  if(bench.character.characterRideRotating) {
    bench.character.characterRideAngle+=25.f*float(dt)*.001f;
    matrix.translate(bench.character.characterRidePivot);matrix.rotate(bench.character.characterRideAngle,0,1,0);matrix.translate(-bench.character.characterRidePivot);
    }
  else {
    bench.character.characterRideTravel+=80.f*float(dt)*.001f;
    matrix.translate(bench.character.characterRideDirection*bench.character.characterRideTravel);
    }
  bench.character.characterPlatformMatrix=matrix;
  bench.character.characterPlatform->setObjMatrix(matrix);bench.character.characterPlatformVisual->setObjMatrix(matrix);
  }

namespace {
// the game's sound for `dt` ms of game time, into the recording when there is one
void renderSound(uint64_t dt) {
  if(Resources::isHeadless())
    return;
  const uint32_t rate = SoundDevice::loopbackRate();
  if(rate==0 || dt==0)
    return;
  const bool slowed = bench.slow>1.f && bench.frameMs>0;
  bench.audioAcc += uint64_t(rate)*dt;
  const size_t n = size_t(bench.audioAcc/1000);
  bench.audioAcc -= uint64_t(n)*1000;
  bench.mix.resize(n*2);
  SoundDevice::renderLoopback(bench.mix.data(),n);
  if(bench.audio==nullptr)
    return;
  if(!slowed) {
    std::fwrite(bench.mix.data(),sizeof(int16_t),bench.mix.size(),bench.audio);
    bench.audioFrames += n;
    return;
    }
  // slowed: the mixer has played dt of game time (the sounds go on), the recording gets the frame's
  // length of silence, so that the sound stays in step with the picture
  const size_t quiet = size_t(uint64_t(rate)*bench.frameMs/1000);
  bench.mix.assign(quiet*2,0);
  std::fwrite(bench.mix.data(),sizeof(int16_t),bench.mix.size(),bench.audio);
  bench.audioFrames += quiet;
  }
}

std::function<bool()> TestBench::aiBusy;
std::function<void(std::string_view)> TestBench::openMenu;
std::function<bool(std::string_view,std::string_view)> TestBench::aiReached;
std::function<bool(World&,std::string_view,float)> TestBench::stuckBegin;
std::function<std::string(World&,std::string_view)> TestBench::npcView;

namespace {
bool benchBodySweep = false;
}

void TestBench::allowBodySweep(bool allow) {
  benchBodySweep = allow;
  }

bool TestBench::bodySweepAllowed() {
  return benchBodySweep;
  }
std::function<bool(World&)> TestBench::stuckEnd;

bool TestBench::voiceStems() {
  static const bool enabled = envStr("OPENGOTHIC_BENCH_VOICE_STEM")!=nullptr &&
                             std::string_view(envStr("OPENGOTHIC_BENCH_VOICE_STEM"))=="1";
  return enabled;
  }

std::function<void(const int16_t*,size_t)> TestBench::aiVoiceStem(std::string_view npc, std::string_view line, uint32_t rate) {
  if(!voiceStems() || !bench.rec || bench.audio==nullptr || SoundDevice::loopbackRate()==0)
    return {};
  auto stem = std::make_shared<Bench::VoiceStem>();
  char suffix[48];
  std::snprintf(suffix,sizeof(suffix),".voice-%02zu.wav",bench.voiceStems.size()+1);
  stem->path = std::filesystem::absolute(outDir+"/"+bench.recName+suffix).string();
  stem->rate = rate;
  // Sources start in the next loopback render, immediately after the recorded mix so far.
  stem->sample = bench.audioFrames;
  bench.voiceStems.push_back(stem);
  event("npc_voice","\"npc\":"+jsonStr(npc)+",\"line\":"+jsonStr(line)+
                    ",\"wav\":"+jsonStr(stem->path)+",\"sample\":"+std::to_string(stem->sample));
  return [stem](const int16_t* pcm, size_t n) {
    std::lock_guard<std::mutex> guard(stem->sync);
    if(!stem->stopped)
      stem->pcm.insert(stem->pcm.end(),pcm,pcm+n);
    };
  }

bool TestBench::ragdollBodyDrawable(const Npc& npc) {
  const auto& mesh=npc.visual.view;
  if(bench.monster.monsterCharacterRenderRequest==&npc) {
    std::string parts="[";
    for(size_t i=0;i<mesh.subCount;++i) {
      const auto& item=mesh.sub[i];if(item.isEmpty())continue;
      const auto matrix=item.position();const auto& bounds=item.bounds();const auto& material=item.material();
      auto center=(bounds.bbox[0]+bounds.bbox[1])*.5f;matrix.project(center);
      const auto root=Vec3(matrix.at(3,0),matrix.at(3,1),matrix.at(3,2));
      float x=center.x,y=center.y,z=center.z,w=1;
      if(auto* camera=Gothic::inst().camera())camera->viewProj().project(x,y,z,w);
      if(parts.size()>1)parts+=',';
      parts+="{\"part\":"+std::to_string(i)+",\"indices\":"+std::to_string(item.meshSlice().second)+
        ",\"root_cm\":"+ragdollVector(root)+",\"center_cm\":"+ragdollVector(center)+
        ",\"clip_cm\":["+ragdollNumber(x)+","+ragdollNumber(y)+","+ragdollNumber(z)+","+ragdollNumber(w)+"]"+
        ",\"radius_cm\":"+ragdollNumber(bounds.rConservative)+",\"alpha\":"+ragdollNumber(material.alphaWeight)+
        ",\"ghost\":"+(material.isGhost?"true":"false")+",\"texture\":"+
        (material.tex && !material.tex->isEmpty()?"true":"false")+"}";
      }
    event("monster_character_render_mesh","\"npc_id\":"+std::to_string(npc.world().npcId(&npc))+
      ",\"cpu_only\":"+(mesh.cpuOnly?"true":"false")+",\"submeshes\":"+std::to_string(mesh.subCount)+",\"parts\":"+parts+"]");
    }
  if(mesh.cpuOnly) return false;
  if(bench.destruction.destructionSkinRequest==&npc && mesh.protoMesh()!=nullptr)
    destructionSkinSample(npc,*mesh.protoMesh(),npc.visual,npc.visual.transform(),npc.visual.pose());
  for(size_t i=0;i<mesh.subCount;++i) {
    const auto& item=mesh.sub[i];
    if(item.isEmpty()) continue;
    const auto& bounds=item.bounds(); const auto& material=item.material();
    if(item.meshSlice().second>0 && std::isfinite(bounds.rConservative) && bounds.rConservative>0 && material.tex!=nullptr &&
       !material.tex->isEmpty() && std::isfinite(material.alphaWeight) && material.alphaWeight>0 && !material.isGhost)
      return true;
    }
  return false;
  }

void TestBench::aiLine(std::string_view npc, std::string_view name, std::string_view text, uint64_t ms,
                       std::string_view to, bool filler) {
  if(scenarioPath==nullptr)
    return;
  bench.lastLineAt = std::max<uint64_t>(bench.time,1);
  bench.busyAt     = bench.time;
  event("npc_line","\"npc\":" + jsonStr(npc) + ",\"name\":" + jsonStr(name) + ",\"text\":" + jsonStr(text) +
                   ",\"ms\":" + std::to_string(ms) + ",\"to\":" + jsonStr(to) + (filler ? ",\"filler\":true" : ""));
  say("npc line " + std::string(npc) + (to.empty() ? "" : " -> " + std::string(to)) + ": " + std::string(text));
  }

void TestBench::npcOutput(const Npc& npc, std::string_view name, const Npc* target, uint64_t ms) {
  World* w = Gothic::inst().world();
  if(scenarioPath==nullptr || !bench.started || bench.done || w==nullptr)
    return;
  const auto who    = symName(*w,&npc);
  const auto whom   = symName(*w,target);
  const auto policy = npc.processPolicy();
  say("output " + who + " -> " + whom + " " + std::string(name) + " " + std::to_string(ms) +
      (policy==NpcProcessPolicy::AiNormal || policy==NpcProcessPolicy::Player ? "" : std::string(" ") + policyName(policy)));
  event("output","\"npc\":" + jsonStr(who) + ",\"target\":" + jsonStr(whom) + ",\"name\":" + jsonStr(name) +
                 ",\"ms\":" + std::to_string(ms) + ",\"ai\":" + jsonStr(policyName(policy)));
  }

void TestBench::aiVoiceEnd(std::string_view npc, uint64_t ms) {
  if(scenarioPath==nullptr)
    return;
  bench.busyAt = bench.time;
  event("npc_voice_end","\"npc\":" + jsonStr(npc) + ",\"ms\":" + std::to_string(ms));
  }

void TestBench::aiHeard(std::string_view text, std::string_view to) {
  if(scenarioPath==nullptr)
    return;
  event("heard","\"text\":" + jsonStr(text) + ",\"to\":" + jsonStr(to));
  say("heard: " + std::string(text) + " -> " + std::string(to));
  }

bool TestBench::tick(uint64_t dt, PlayerControl& ctrl) {
  FrameProfile::Scope timing(FrameProfile::Zone::Bench);
  if(scenarioPath==nullptr || bench.done)
    return false;
  if(!bench.loaded)
    loadScenario();

  if(!bench.perfSegment.empty())FrameProfile::sample(bench.perfSegment,bench.perfFrame);

  auto world  = Gothic::inst().world();
  auto player = Gothic::inst().player();
  KnownRoadsBench::tick(Gothic::inst().checkLoading()==Gothic::LoadState::Idle?world:nullptr);
  if(world==nullptr || player==nullptr || Gothic::inst().checkLoading()!=Gothic::LoadState::Idle) {
    renderSound(dt);
    return false;
    }
  if(!bench.started) {
    bench.started = true;
    bench.heartbeatStart = std::chrono::steady_clock::now();
    FootIk::setProbing(true);
    say("start");
    if(std::string_view(scenarioPath).find("placed-items")!=std::string_view::npos)
      event("placed_items_loaded","\"world\":"+jsonStr(world->name())+",\"world_load_ms\":"+
        ragdollNumber(float(world->loadMilliseconds()))+",\"classification_ms\":"+
        ragdollNumber(float(world->placedItemValidationMilliseconds()))+",\"enabled\":"+
        (Gothic::physicsFlags().placedItems?"true":"false"));
    if(std::string_view(scenarioPath).find("furniture-parts")!=std::string_view::npos)
      event("furniture_parts_loaded","\"world\":"+jsonStr(world->name())+",\"world_load_ms\":"+
        ragdollNumber(float(world->loadMilliseconds()))+",\"enabled\":"+
        (Gothic::settingsGetI("GAME","furniture_parts")?"true":"false")+
        ",\"peak_rss_bytes\":"+std::to_string(furniturePeakMemory()));
    if(std::string_view(scenarioPath).find("movable-vobs")!=std::string_view::npos)
      event("movable_loaded","\"world\":"+jsonStr(world->name())+",\"world_load_ms\":"+
        ragdollNumber(float(world->loadMilliseconds()))+",\"enabled\":"+
        (Gothic::settingsGetI("GAME","movable_vobs")?"true":"false"));
    // A reload observer can enable god mode before the untouched initial
    // snapshot without allowing a native world step between those commands.
    if(bench.pc+1<bench.prog.size()) {
      const auto prelude=split(bench.prog[bench.pc]);
      const auto initial=split(bench.prog[bench.pc+1]);
      if(prelude.size()==2 && prelude[0]=="cheat" && prelude[1]=="god" &&
         initial.size()==3 && (initial[0]=="water" || initial[0]=="water-bodies") && initial[1]=="initial")
        exec(bench.prog[bench.pc++],*world,*player,ctrl);
      }
    if(bench.pc<bench.prog.size()) {
      const auto args=split(bench.prog[bench.pc]);
      if(args.size()>1 && args[0]=="ragdoll" && args[1]=="session-initial") {
        const auto line=bench.prog[bench.pc++];
        try {
          ragdollSessionArchive(args,*world,*player,dt);
          } catch(const std::exception& error) {
          say("initial session snapshot failed: "+std::string(error.what()));
          event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
          }
        }
      else if(args.size()==2 && args[0]=="placed-items" && args[1]=="initial") {
        ++bench.pc;placedItemsSnapshot(*world,"initial",dt);
        }
      else if(args.size()==2 && args[0]=="item-save" && args[1]=="initial") {
        ++bench.pc;
        try {savedItemInitial(*world,*player,dt);}
        catch(const std::exception& error) {arenaAbort(error.what());}
        }
      else if(args.size()==2 && args[0]=="monstercharacter" && args[1]=="initial") {
        ++bench.pc;monsterCharacterSnapshot(*world,*player,"initial");
        characterIdleGuards(*world,*player);
        }
      else if(args.size()==2 && args[0]=="npccharacter" && args[1]=="initial") {
        ++bench.pc;npcCharacterSnapshot(*world,*player,"initial");
        characterIdleGuards(*world,*player);
        // The first native step recreates the transient controller. Observe it
        // before normal command dispatch can advance past that initial contact.
        std::string trace="npccharacter trace";
        for(uint32_t id=0;id<world->npcCount();++id) {
          const auto* npc=world->npcById(id);
          if(npc && npc!=player && npc->isHuman() && !npc->isDown() &&
             (npc->position()-player->position()).length()<=2300.f)
            trace+=' '+std::to_string(id);
          }
        if(trace!="npccharacter trace")exec(trace,*world,*player,ctrl);
        }
      else if(args.size()==2 && args[0]=="character" && args[1]=="initial") {
        ++bench.pc;characterSnapshot(*world,*player,"initial");
        characterIdleGuards(*world,*player);
        exec("npccharacter trace player",*world,*player,ctrl);
        }
      else if(args.size()==3 && args[0]=="movable" && args[1]=="initial") {
        ++bench.pc;
        movableSaveSnapshot(*world,args[2]);
        // External control is a transient fixture setting. Restore it before
        // the first native step, after observing the unmodified loaded state.
        if(args[2]=="restored")
          exec("movable observe",*world,*player,ctrl);
        }
      else if(args.size()==3 && args[0]=="cloth" && args[1]=="initial") {
        ++bench.pc;clothInitial(*world,args[2]);
        }
      else if(args.size()==3 && args[0]=="destruction2" && args[1]=="initial") {
        ++bench.pc;destructionSessionSnapshot(*world,args[2]);
        }
      else if(args.size()==3 && args[0]=="water" && args[1]=="initial") {
        ++bench.pc;waterInitial(bench.water,waterContext(),*world,*player,ctrl,args[2]);
        }
      else if(args.size()==3 && args[0]=="water-bodies" && args[1]=="initial") {
        ++bench.pc;waterBodiesInitial(bench.water,waterContext(),*world,*player,args[2]);
        }
      }
    renderSound(dt);
    return false;
    }
  writeHeartbeat(*world);
  if(dt==0) {
    // the world stands still (`deterministic on`): only the streaming wait runs
    if(bench.frozen)
      streamingWaitTick(*world);
    return false;
    }
  if(bench.settleSteps>0)
    --bench.settleSteps;
  NavBench::tick(*world);
  NavDemos::tick(*world,dt);
  NavigationEyeBench::tick(world->tickCount());
  NavigationMemoryBench::tick(*world);
  NpcKeysBench::tick(*world,*player,ctrl,dt);
  arrowFields(*world);
  stickingFields(*world);
  spellFields(*world);
  bodyImpactTick(*world);
  healingObserve(bench.healing,*world,*player);
  if(auto& drift=bench.npcCommands.stunDrift;drift.target!=nullptr) {
    const uint64_t elapsed=world->tickCount()-drift.start;
    const uint64_t requested=drift.next*100;
    if(elapsed>=requested) {
      stunDriftSample(*world,elapsed,requested);
      if(++drift.next==17) drift={};
      }
    }
  destructionFields(bench.destruction,destructionContext(),*world);
  if(!bench.movables.movableMeasuring)fracturePiecesTick(bench.fracturePieces,*world);
  if(bench.destruction.destructionSupportTrace && bench.destruction.destructionTrace &&
     world->tickCount()-bench.destruction.destructionStart>=bench.destruction.destructionSupportNext) {
    bench.destruction.destructionSupportNext=world->tickCount()-bench.destruction.destructionStart+1000;
    destructionSupportSample(bench.destruction,destructionContext(),*world);
    }
  destructionShoot(bench.destruction,*world,*player);
  destructionMeleeTick(bench.destruction,destructionContext(),*world);
  if(bench.destruction.destructionFarmNpc) {
    bench.destruction.destructionFarmNpcTravel=std::max(bench.destruction.destructionFarmNpcTravel,
      (bench.destruction.destructionFarmNpc->position()-bench.destruction.destructionFarmNpcStart).length());
    if(bench.destruction.destructionFarmNpcGoal)
      bench.destruction.destructionFarmNpcBest=std::min(bench.destruction.destructionFarmNpcBest,
        (bench.destruction.destructionFarmNpc->position()-bench.destruction.destructionFarmNpcGoal->pos).length());
    }
  if(bench.furnitureParts.walking) {
    auto& fixture=bench.furnitureParts;
    auto& actor=fixture.walker?*fixture.walker:*player;
    const auto position=actor.position(),delta=position-fixture.walkStart;
    fixture.walkMaxStep=std::max(fixture.walkMaxStep,std::abs(position.y-fixture.walkPrevious.y));
    fixture.walkPrevious=position;
    fixture.walkMaxCrossTrack=std::max(fixture.walkMaxCrossTrack,
      std::abs(delta.x*fixture.walkDirection.z-delta.z*fixture.walkDirection.x));
    const float progress=Vec3::dotProduct(delta,fixture.walkDirection);
    const float length=Vec3::dotProduct(fixture.walkGoal-fixture.walkStart,fixture.walkDirection);
    if(!fixture.walkArrived && progress>=length-25.f) {
      fixture.walkArrived=world->tickCount();
      if(!fixture.walker) {
        release(ctrl,KeyCodec::Forward);
        bench.holds.erase(std::remove_if(bench.holds.begin(),bench.holds.end(),[](const Hold& hold){return hold.a==KeyCodec::Forward;}),bench.holds.end());
        player->stopWalking();player->clearSpeed();
        }
      }
    if(!fixture.walker && !fixture.walkArrived)player->setDirection(fixture.walkDirection);
    }

  meleeDodgeFields(*world);
  meleeAiResponseFields(*world);
  placedItemsFields(bench.placedItems,*world,*player,ctrl,bench.holds);
  movableFields(*world);
  ownMeshFields(*world,*player);
  ownMeshChestFields(*world,*player);
  waterFields(bench.water,waterContext(),*world);
  itemWakeFields(*world);
  savedItemSample(*world);
  waterFilmTick(*world);
  clothFields(*world);
  movableCandleFields(*world);
  noiseFields(*world);
  if(bench.movables.spellReleaseAt && world->tickCount()>=bench.movables.spellReleaseAt) {
    player->endCastSpell(false);bench.movables.spellReleaseAt=0;
    event("spell_physics_release","\"id\":"+std::to_string(bench.movables.spellCastingId));
    }
  if(ragdollPerf) {
    const auto now=std::chrono::steady_clock::now();
    FrameProfile::observer(dt,now);
    FrameProfile::setActors(bench.crew);
    bench.ragdoll.perfWallMs=bench.ragdoll.hasPerfFrame ? std::chrono::duration<float,std::milli>(now-bench.ragdoll.lastPerfFrame).count() : 0;
    bench.ragdoll.lastPerfFrame=now; bench.ragdoll.hasPerfFrame=true;
    }
  // Independent privileged oracle at the same post-world/animation phase as the observer.
  // Setup commands run below and may leave real root motion for the next native substep; comparing
  // their pre-substep coordinates to a later observation would incorrectly reject correct data.
  if(!bench.observeProbeCase.empty()) {
    const auto actors = bodyObservationActors();
    if(actors.size()>=2) {
      const auto& first = *actors[0]; const auto& second = *actors[1];
      const auto self = first.position(), target = second.position();
      const auto center = world->physic()->ray(first.mapHeadBone(),second.collosionCenter());
      const auto head = world->physic()->ray(first.mapHeadBone(),second.mapHeadBone());
      event("body_observe_probe_sample","\"case\":"+jsonStr(bench.observeProbeCase)+
            ",\"t_game_ms\":"+std::to_string(world->tickCount())+
            ",\"sim_step\":"+std::to_string(BodyObservations::currentStep())+
            ",\"actor_id\":"+std::to_string(BodyObservations::entityId(first))+
            ",\"target_id\":"+std::to_string(BodyObservations::entityId(second))+
            ",\"visible\":"+(first.canSeeNpc(second,false) ? "true" : "false")+
            ",\"center_ray_hit\":"+(center.hasCol ? "true" : "false")+
            ",\"head_ray_hit\":"+(head.hasCol ? "true" : "false")+
            ",\"truth_self_hp\":"+std::to_string(first.attribute(ATR_HITPOINTS))+
            ",\"truth_self_yaw_rad\":"+std::to_string(first.rotationRad())+
            ",\"truth_self_position_cm\":["+std::to_string(self.x)+","+std::to_string(self.y)+","+std::to_string(self.z)+"]"+
            ",\"truth_target_hp\":"+std::to_string(second.attribute(ATR_HITPOINTS))+
            ",\"truth_target_position_cm\":["+std::to_string(target.x)+","+std::to_string(target.y)+","+std::to_string(target.z)+"]");
      }
    }
  characterFields(bench.character,characterContext(),*world,*player,dt,ctrl);
  monsterCharacterFields(bench.monster,monsterCharacterContext(),*world,dt);
  npcCharacterFields(bench.character,characterContext(),*world,*player,dt);
  npcCharacterMotionTrace(bench.character,characterContext(),*world,dt);
  NpcDayJournal::tick(*world);
  if(bench.inputTraceUntil!=0 && bench.time<=bench.inputTraceUntil) {
    const auto animation = player->animNames();
    if(animation!=bench.inputAnimation) {
      bench.inputAnimation = animation;
      say("input animation PC_HERO at " + std::to_string(world->tickCount()) + " ms: " + animation);
      }
    }
  bench.time += dt;
  if(aiBusy && aiBusy())
    bench.busyAt = bench.time;

  for(auto k:bench.taps)
    release(ctrl,k);
  bench.taps.clear();
  for(size_t i=0; i<bench.holds.size();) {
    if(bench.holds[i].until<=bench.time) {
      release(ctrl,bench.holds[i].a);
      bench.holds.erase(bench.holds.begin()+long(i));
      } else {
      ++i;
      }
    }

  bench.ctrl = &ctrl;
  if(bench.ragdoll.birthActor!=nullptr && !bench.ragdoll.birthActor->isDown()) {
    const Bench::BirthLive live{bench.ragdoll.birthActor->position(),world->tickCount(),bench.ragdoll.birthActor->isInAir()};
    if(bench.ragdoll.birthHistory.empty() || live.tick>bench.ragdoll.birthHistory.back().tick) {
      bench.ragdoll.birthHistory.push_back(live); if(bench.ragdoll.birthHistory.size()>4) bench.ragdoll.birthHistory.pop_front();
      event("ragdoll_birth_live","\"label\":"+jsonStr(bench.ragdoll.birthLabel)+",\"actor_id\":"+std::to_string(world->npcId(bench.ragdoll.birthActor))+
            ",\"t_game_ms\":"+std::to_string(live.tick)+",\"in_air\":"+(live.inAir ? "true" : "false")+
            ",\"position_cm\":"+ragdollVector(live.position));
      }
    }
  if(bench.ragdoll.birthContinuation!=nullptr && world->tickCount()>bench.ragdoll.birthTick) {
    const auto* body=bench.ragdoll.birthContinuation->ragdollBody();
    if(body!=nullptr)
      event("ragdoll_birth_continuation","\"label\":"+jsonStr(bench.ragdoll.birthContinuationLabel)+",\"actor_id\":"+
            std::to_string(world->npcId(bench.ragdoll.birthContinuation))+",\"t_game_ms\":"+std::to_string(world->tickCount())+
            ",\"pelvis_y_cm\":"+std::to_string(body->pelvisTransform().data()[13])+",\"native_parts\":"+std::to_string(body->partCount()));
    else event("ragdoll_error","\"reason\":\"birth body missing on the next native tick\"");
    bench.ragdoll.birthContinuation=nullptr;
    }
  if((ragdollPerf || bench.ragdoll.ragdollArenaCamera)
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
     && !AppCadence::normalClock()
#endif
     ) {
    // Coverage is sampled after the actual world step and BEFORE fixture commands
    // can wake bodies. A post-impulse "awake" flag alone could hide a sleeping tick.
    bench.ragdoll.perfPhysicalBeforeImpulse=0; bench.ragdoll.perfSleepingBeforeImpulse=0;
    for(const auto* npc:bench.crew)
      if(const auto* body=npc->ragdollBody()) {
        ++bench.ragdoll.perfPhysicalBeforeImpulse;
        if(body->isSleeping()) ++bench.ragdoll.perfSleepingBeforeImpulse;
        }
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  if(bench.clothMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    if(bench.clothPhysics.size()<20000)
      bench.clothPhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.clothOverflow=true;
    }
  if(bench.movables.movableMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    bench.movables.noiseMeasuredRays+=stats.rays;
    if(bench.movables.movablePhysics.size()<20000)
      bench.movables.movablePhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.movables.movableOverflow=true;
    }
  if(bench.living.livingMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    if(bench.living.livingPhysics.size()<20000)
      bench.living.livingPhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.living.livingOverflow=true;
    }
#endif
  itemHullSample(bench.itemHulls,*world);
  furniturePerformanceSample(bench.furnitureParts,*world,dt);
  tickRoute(*player,dt);
  run(*world,*player,ctrl);
  renderSound(dt);
  if(bench.done)
    return false;
  if(bench.ragdoll.ragdollChurn) {
    for(size_t i=0;i<bench.crew.size();++i)
      if(bench.crew[i]->ragdollBody()!=nullptr) {
        const float sign=((bench.ragdoll.ragdollPulseFrame+2)/5+i)%2==0 ? 1.f : -1.f;
        bench.crew[i]->addRagdollImpulse({50.f*sign,5.f,25.f*sign});
        }
    ++bench.ragdoll.ragdollPulseFrame;
    }
  // a dialog (an npc with an important one comes to the hero) holds the player: say so
  if(Gothic::inst().isInDialog()!=bench.inDialog) {
    bench.inDialog = !bench.inDialog;
    std::string who;
    if(bench.inDialog)
      world->detectNpc(player->position(),2000,[&](Npc& n) {
        if(&n!=player && Gothic::inst().isNpcInDialog(n))
          who += " " + symName(*world,&n);
        });
    say(std::string(bench.inDialog ? "dialog started:" : "dialog ended") + who);
    }
  for(auto& [n,at]:bench.archery.archers) {
    // the archer harness: where it was set, at the target, the bow drawn, arrows; an arrow every time it
    // can (no fight AI, no perception's wait)
    if(n==nullptr || bench.archery.archerAt==nullptr || n->isDown())
      continue;
    const Vec3 d = bench.archery.archerAt->position()-at;
    n->setPosition(at);
    n->setDirection(Vec3(d.x,0,d.z));
    n->updateTransform();
    n->setTarget(bench.archery.archerAt);
    const auto ws = n->weaponState();
    if(auto rw = n->currentRangedWeapon(); rw!=nullptr && n->itemCount(size_t(rw->handle().munition))<5)
      n->addItem(size_t(rw->handle().munition),20);
    if(ws!=WeaponState::Bow && ws!=WeaponState::CBow) {
      n->drawWeaponBow();
      continue;
      }
    if(!bench.archery.archerDice && n->archerAi().isDriving(*n))
      n->archerAi().attack(*n);
    else if(!n->shootBow())
      n->aimBow();
    }
  if(bench.pinned!=nullptr) {
    const Vec3 d = player->position()-bench.pinAt;
    bench.pinned->setPosition(bench.pinAt);
    bench.pinned->setDirection(Vec3(d.x,0,d.z));
    bench.pinned->updateTransform();
    }
  if(bench.archery.aimNpc!=nullptr || (bench.archery.aimMouse && bench.archery.aimFixed)) {
    Vec3 at = bench.archery.aimAt;
    if(bench.archery.aimNpc!=nullptr && !aimPoint(*bench.archery.aimNpc,bench.archery.aimZone,bench.archery.aimLeg,at) &&
       !bench.archery.aimNpc->hitZoneCenter(HitZones::Body,at))
      at = bench.archery.aimNpc->collosionCenter();
    if(bench.archery.aimNpc!=nullptr && !bench.archery.aimRaw) {
      // Aim for the release draw throughout drawing, so the changing trajectory does not shake the aim.
      const auto st = Archery::stats(*player,player->currentRangedWeapon());
      Vec3       nock, dir;
      if(!player->arrowRay(nock,dir))
        nock = player->position() + Vec3(0,145,0);
      const auto& ar = player->archer();
      const float releaseDraw = bench.npcCommands.volleyLeft>0 ? std::min(st.burstDraw,bench.npcCommands.volleyDraw) : st.burstDraw;
      const float draw = st.bowStrength ? (ar.stage()==Archer::Held ? ar.draw() : releaseDraw) : 1.f;
      at = Archery::launch(nock,at,Archery::speedAt(st,draw));
      }
    auto cam = Gothic::inst().camera();
    if(!bench.archery.aimMouse) {
      ctrl.setAimOverride(true,at);
      }
    else if(cam!=nullptr && cam->isAim()) {
      // as the mouse: the center of the screen onto it, the hero turned with the camera. The camera looks
      // through a point over the shoulder that turns about the hero with it: the turn that puts the target in
      // the camera's plane through it (steeply down a turn toward the target from there would swing round)
      Vec3 from, dir;
      cam->aimRay(from,dir);
      Vec3 fw = Vec3(dir.x,0,dir.z);
      if(fw.length()<1e-3f)
        fw = facing(*player);
      fw = Vec3::normalize(fw);
      const Vec3  o    = from - player->position();
      const float ahd  = Vec3::dotProduct(o,fw);
      const float side = Vec3::dotProduct(o,Vec3(fw.z,0,-fw.x));
      const Vec3  t    = at - player->position();
      const float lt   = std::sqrt(t.x*t.x+t.z*t.z);
      const float yaw  = std::atan2(t.z,t.x) + (lt>1.f ? std::asin(std::clamp(side/lt,-1.f,1.f)) : 0.f);
      const Vec3  f    = Vec3(std::cos(yaw),0,std::sin(yaw));
      const Vec3  d    = t - (f*ahd + Vec3(f.z,0,-f.x)*side + Vec3(0,o.y,0));
      const float pit  = std::atan2(d.y,std::max(Vec3::dotProduct(d,f),1.f))*180.f/float(M_PI);
      cam->setSpin(PointF(-pit,yaw*180.f/float(M_PI)));
      player->setDirection(yaw*180.f/float(M_PI));
      }
    }
  if(bench.archery.arrowNpc!=nullptr && !bench.archery.arrowAim.empty()) {
    Vec3 at;
    if(arrowAimPoint(*bench.archery.arrowNpc,at)) {
      Vec3 nock,direction;
      if(!player->arrowRay(nock,direction)) nock=player->position()+Vec3(0,145,0);
      const auto stats=Archery::stats(*player,player->currentRangedWeapon());
      ctrl.setAimOverride(true,Archery::launch(nock,at,Archery::speedAt(stats,bench.archery.arrowDraw)));
      }
    }
  // the hero speaks to an npc (as the bridge does with his voice), it thinks of an answer
  if(bench.hearNpc!=nullptr) {
    if(bench.time<bench.hearUntil) {
      bench.hearNpc->listenTo(300);
      bench.hearNpc->setTalkPartner(player,1000);
      player->setTalkPartner(bench.hearNpc,1000);
      if(bench.time>=bench.hearNext) {
        bench.hearNpc->heardWords();
        bench.hearNext = bench.time + bench.hearEvery;
        }
      } else {
      bench.hearNpc->stopListening();
      bench.hearNpc = nullptr;
      }
    }
  if(bench.thinkNpc!=nullptr) {
    if(bench.time<bench.thinkUntil) {
      bench.thinkNpc->thinkOfAnswer(300);
      bench.thinkNpc->setTalkPartner(player,1000);
      } else {
      bench.thinkNpc->thinkOfAnswer(0);
      bench.thinkNpc = nullptr;
      }
    }
  if(bench.orbitUntil>bench.time) {
    bench.placeAngle += bench.orbitSpeed*float(dt)/1000.f;
    placeHero(*world,*player);
    }

  const Vec3 pos = player->position();
  if(bench.hasPrev) {
    const Vec3 d = pos-bench.prevPos;
    bench.speed  = std::sqrt(d.x*d.x+d.z*d.z)*1000.f/float(dt);
    }
  bench.prevPos = pos;
  bench.hasPrev = true;

  updateCamera(*world,subjectOf(*player));

  if(bench.perfArmed)FrameProfile::sample(bench.perfSegment,bench.perfFrame++);

  if(bench.ragdoll.ragdollCoverageActive) {
    auto sample=ragdollCoverageSample(*world);
    sample.segment=bench.ragdoll.ragdollCoverageMode; sample.frame=bench.ragdoll.ragdollCoverageFrames++;
    for(const auto* npc:bench.crew) arenaModelProof(*world,*npc,size_t(sample.frame));
    if(!arenaModeMatches(sample,bench.ragdoll.ragdollCoverageMode) && !bench.ragdoll.ragdollCoverageFailed) {
      bench.ragdoll.ragdollCoverageFailed=true;
      event("ragdoll_error","\"reason\":\"untimed twenty-body coverage failed\",\"coverage_frame\":"+std::to_string(sample.frame));
      }
    bench.ragdoll.ragdollCoverage.push_back(std::move(sample));
    if(bench.ragdoll.ragdollCoverageFrames==1020) {
      bench.ragdoll.ragdollCoverageActive=false;
      event("ragdoll_coverage_complete","\"mode\":"+jsonStr(bench.ragdoll.ragdollCoverageMode)+",\"frames\":1020,\"pass\":"+
            (bench.ragdoll.ragdollCoverageFailed ? "false" : "true"));
      }
    }

  bool capture = !bench.shot.empty();
  if(bench.rec) {
    if(ragdollPerf)
      sampleRagdollPerformance(*world);
    else
      writeRow(*player);
    capture |= bench.recVideo;
    if(!bench.recVideo)
      ++bench.recFrame;
    }
  return capture;
  }

void TestBench::sampleStreaming(const World& world) {
  if(scenarioPath==nullptr || bench.done)return;
  const bool measuring=bench.perfSegment.starts_with("streaming-flythrough/");
  const bool shots=std::filesystem::path(scenarioPath).filename()=="streaming-shots.bench";
  if(!measuring && !shots && std::filesystem::path(scenarioPath).filename()!="streaming-flythrough.bench")return;
  const auto* residency=world.chunkResidency();
  if(residency==nullptr)return;
  const auto& stats=residency->stats();
  if(shots && !bench.shot.empty())
    event("streaming_shot","\"shot\":"+jsonStr(bench.shot)+
      ",\"vob_near\":"+std::to_string(stats.vobNear)+
      ",\"vob_near_missing\":"+std::to_string(stats.vobNearMissing)+
      ",\"vob_pending\":"+std::to_string(stats.vobPending));
  if(!measuring && !shots) {
    bench.streamingWarmupFarMs.insert(bench.streamingWarmupFarMs.end(),stats.farPublicationTimesMs.begin(),stats.farPublicationTimesMs.end());
    bench.streamingWarmupFarMaxPerFrame=std::max(bench.streamingWarmupFarMaxPerFrame,stats.farPublicationTimesMs.size());
    bench.streamingWarmupQueueMaxMs=std::max(bench.streamingWarmupQueueMaxMs,stats.workMs);
    return;
    }
  bench.streamingSamples.push_back({bench.perfSegment,bench.perfFrame,stats.updateMs,stats.workMs,
    stats.near,stats.far,stats.outside,stats.transitionsNear-bench.streamingNearBefore,
    stats.transitionsFar-bench.streamingFarBefore,stats.publicationTimesMs,
    stats.published-bench.streamingPublishedBefore,stats.released-bench.streamingReleasedBefore,
    stats.landscapePageUsedBytes,stats.landscapePageFreeBytes,stats.landscapePageRetiredBytes,stats.landscapePageCount,
    Resources::textureStats(),stats.vobNear,stats.vobFar,stats.vobOutside,stats.vobPending,stats.vobNearMissing,
    stats.vobCreated-bench.streamingVobCreatedBefore,stats.vobReleased-bench.streamingVobReleasedBefore,
    stats.vobPublicationMs,stats.vobNearMissingSeconds-bench.streamingVobMissingBefore});
  const auto& tiers=world.npcSimTierStats();
  auto& sample=bench.streamingSamples.back();
  sample.npcFull=tiers.full;
  sample.npcSimplified=tiers.simplified;
  // Transitions since the previous sample, like near_transitions and far_transitions.
  sample.npcTierTransitions=tiers.toFull+tiers.toSimplified-bench.streamingNpcTransitionsBefore;
  bench.streamingNpcTransitionsBefore=tiers.toFull+tiers.toSimplified;
  sample.npcFullTickMs=tiers.fullTickMs;
  sample.npcSimplifiedTickMs=tiers.simplifiedTickMs;
  bench.streamingNearBefore=stats.transitionsNear;bench.streamingFarBefore=stats.transitionsFar;
  bench.streamingPublishedBefore=stats.published;bench.streamingReleasedBefore=stats.released;
  bench.streamingVobCreatedBefore=stats.vobCreated;bench.streamingVobReleasedBefore=stats.vobReleased;
  }

bool TestBench::withUi() {
  return isActive() && bench.ui;
  }

bool TestBench::ragdollLootOpened(const Npc& player, const Npc& target) {
  if((!ragdollTrace && !bench.water.bodiesReady) || !bodyTraceAlive || scenarioPath==nullptr || !bench.started || bench.done || bench.ragdoll.expectedRagdollLoot!=&target)
    return false;
  auto* world=Gothic::inst().world();
  if(world==nullptr)
    return false;
  bench.ragdoll.ragdollLootDidOpen=true;
  event("ragdoll_loot_open","\"player_actor_id\":"+std::to_string(world->npcId(&player))+
        ",\"target_actor_id\":"+std::to_string(world->npcId(&target))+",\"state\":\"Ransack\",\"close\":\"synchronous fixture after actual open\"");
  return true;
  }

// track: where a tracked npc is on this frame, into events.jsonl: x, y the head (or the trunk: track ... body),
// and every zone: hx hy (head), bx by (trunk), lx ly (the legs, below the knees); pixels of the frame
void trackActorFrame(World& world, const std::string& symbol, int w, int h) {
  auto   cam  = Gothic::inst().camera();
  Npc*   n    = npcBySymbol(world,symbol);
  if(n==nullptr)
    return;
  // HP/identity do not depend on visibility. Exporters still get one sample if a head is offscreen.
  if(bench.bodyLive) {
    const Npc* target=n->target()!=nullptr ? n->target() : world.player();
    event("body_live_sample","\"npc\":"+jsonStr(symbol)+",\"entity_id\":"+
          std::to_string(BodyObservations::entityId(*n))+",\"t_game_ms\":"+std::to_string(world.tickCount())+
          ",\"self_hp\":"+std::to_string(n->attribute(ATR_HITPOINTS))+",\"target_hp\":"+
          (target!=nullptr ? std::to_string(target->attribute(ATR_HITPOINTS)) : "null"));
    }
  if(cam==nullptr)
    return;
  std::vector<HitZones::Posed> caps;
  n->hitZonesPosed(caps);
  const float knees = n->position().y + 80.f;
  Vec3  sum[3] = {};
  float cnt[3] = {};
  for(auto& c:caps) {
    const Vec3 m = (c.a+c.b)*0.5f;
    const int  k = c.zone==HitZones::Head ? 0 : (c.zone==HitZones::Body ? 1 : (m.y<knees ? 2 : -1));
    if(k>=0) {
      sum[k] += m;
      cnt[k] += 1;
      }
    }
  const Matrix4x4 vp = cam->viewProj();
  float sx[3] = {-1,-1,-1}, sy[3] = {-1,-1,-1};
  for(int k=0; k<3; ++k) {
    if(cnt[k]==0)
      continue;
    Vec3  p  = sum[k]*(1.f/cnt[k]);
    float pw = 1;
    vp.project(p.x,p.y,p.z,pw);
    if(pw<=1e-3f)
      continue;
    sx[k] = (0.5f*p.x/pw+0.5f)*float(w);
    sy[k] = (0.5f*p.y/pw+0.5f)*float(h);
    }
  const int main = bench.trackHead ? 0 : 1;
  if(sx[main]<0)
    return;
  char buf[320];
  std::snprintf(buf,sizeof(buf),"\"npc\":%s,\"x\":%.1f,\"y\":%.1f,\"hx\":%.1f,\"hy\":%.1f,\"bx\":%.1f,\"by\":%.1f,"
                "\"lx\":%.1f,\"ly\":%.1f,\"w\":%d,\"h\":%d",
                jsonStr(symbol).c_str(),double(sx[main]),double(sy[main]),double(sx[0]),double(sy[0]),
                double(sx[1]),double(sy[1]),double(sx[2]),double(sy[2]),w,h);
  event("track",buf);
  }

void trackFrame(int w, int h) {
  if(auto* world=Gothic::inst().world())
    for(const auto& symbol:bench.trackNpcs)
      trackActorFrame(*world,symbol,w,h);
  }

// sight: the hero's line of sight to the npc on this frame, into events.jsonl; points are [x, y] in pixels of the
// frame (null behind the camera): h0 the shared standing head, c0 where its ray to the npc's
// centre hits (null: nothing; c0d cm of d0), e the minimum lean as Npc::canSeeNpcOverEdge puts it (room
// for it), ec where its ray hits; h the head now, c where its ray hits (cd of d, down degrees); t the npc's
// centre; the bow's focus fan from h0: f level, g at FOCUS_RANGED's elevdo down (elev), fan cm long
void sightFrame(int w, int h, const std::string& shot) {
  World* wrld = Gothic::inst().world();
  auto   cam  = Gothic::inst().camera();
  Npc*   pl   = Gothic::inst().player();
  Npc*   n    = wrld!=nullptr && pl!=nullptr ? findNpc(*wrld,*pl,bench.sightNpc) : nullptr;
  if(n==nullptr || n==pl || cam==nullptr)
    return;
  const Matrix4x4 vp = cam->viewProj();
  auto pt = [&](Vec3 p, bool has = true) -> std::string {
    float pw = 1;
    vp.project(p.x,p.y,p.z,pw);
    if(!has || pw<=1e-3f)
      return "null";
    char b[64];
    std::snprintf(b,sizeof(b),"[%.1f,%.1f]",double((0.5f*p.x/pw+0.5f)*float(w)),double((0.5f*p.y/pw+0.5f)*float(h)));
    return b;
    };
  auto*       phys = wrld->physic();
  const Vec3  tg   = n->collosionCenter();
  const bool  manual = pl->hasManualAim();
  const Vec3  aim  = manual ? pl->manualAimPoint() : tg;
  const Vec3  h0   = pl->aimHead(aim,0.f);
  const Vec3  hd   = pl->mapHeadBone();
  const Vec3  predicted = pl->aimCurrentHead();
  const float predictionError = (predicted-hd).length();
  const auto  lean = pl->aimLean(aim,manual);
  auto        r0   = phys->ray(h0,tg);
  auto        r1   = phys->ray(hd,tg);
  // The manual pose's directional solver is independent of this observed NPC.
  const Vec3  d    = tg-h0;
  const Vec3  eye  = pl->aimHead(aim,lean.angle);
  const Vec3  full = pl->aimHead(aim,Aim::LeanMax*float(M_PI)/180.f);
  auto        rt   = phys->ray(eye,tg);
  // the focus fan in the vertical plane toward the npc
  const float elev = -float(wrld->script().focusRange().npc_elevdo)*float(M_PI)/180.f;
  const Vec3  lvl  = Vec3::normalize(Vec3(d.x,0,d.z));
  const Vec3  fl   = h0 + lvl*bench.sightFan;
  const Vec3  fg   = h0 + (lvl*std::cos(elev) - Vec3(0,std::sin(elev),0))*bench.sightFan;
  const float down = -std::atan2(tg.y-hd.y,std::sqrt((tg.x-hd.x)*(tg.x-hd.x)+(tg.z-hd.z)*(tg.z-hd.z)))*180.f/float(M_PI);

  char buf[2300];
  std::snprintf(buf,sizeof(buf),"\"npc\":%s,\"shot\":%s,\"w\":%d,\"h\":%d,"
                "\"h0\":%s,\"c0\":%s,\"c0d\":%.0f,\"d0\":%.0f,\"e\":%s,\"room\":%d,\"ec\":%s,"
                "\"h\":%s,\"c\":%s,\"cd\":%.0f,\"d\":%.0f,\"down\":%.1f,\"t\":%s,\"f\":%s,\"g\":%s,\"elev\":%.0f,"
                "\"seen\":%d,\"over\":%d,\"required_deg\":%.2f,\"current_deg\":%.2f,\"max_forward_cm\":%.1f,"
                "\"error_cm\":%.2f,\"actual_seen\":%d,\"edge\":%d,\"hit_cm\":%.2f,\"visible\":%d,\"line\":%s,\"shot_clear\":%d,\"manual\":%d",
                jsonStr(bench.sightNpc).c_str(),jsonStr(shot).c_str(),w,h,
                pt(h0).c_str(),pt(r0.v,r0.hasCol).c_str(),r0.hasCol ? double((r0.v-h0).length()) : -1.0,double((tg-h0).length()),
                pt(eye).c_str(),int(lean.room),pt(rt.v,rt.hasCol).c_str(),
                pt(hd).c_str(),pt(r1.v,r1.hasCol).c_str(),r1.hasCol ? double((r1.v-hd).length()) : -1.0,double((tg-hd).length()),
                double(down),pt(tg).c_str(),pt(fl).c_str(),pt(fg).c_str(),double(elev*180.f/float(M_PI)),
                int(!r0.hasCol),int(pl->canSeeNpcOverEdge(*n)),double(lean.angle*180.f/float(M_PI)),
                double(Aim::playerProbe.lean),double(Vec3::dotProduct(full-pl->position(),lvl)),
                double(predictionError),int(!r1.hasCol),int(lean.edge),double(lean.hitDistance),int(lean.visible),
                jsonStr(leanLineName(lean.line)).c_str(),int(lean.shotClear),int(manual));
  event("sight",buf);
  // Captured after all pose passes. A shot captured during a recording also calls sightFrame;
  // only its ordinary frame writes the diagnostic, keeping exactly one sample per recorded frame.
  if(bench.rec && shot.empty()) {
    const std::string focus = symName(*wrld,pl->target());
    Vec3 arrow, arrowDir;
    const bool hasArrow = pl->arrowRay(arrow,arrowDir);
    int actualShotClear = -1;
    if(hasArrow) {
      const auto shotRay = phys->ray(arrow,arrow+Vec3::normalize(arrowDir)*200.f);
      actualShotClear = int(!shotRay.hasCol || (lean.edge && shotRay.v.y<lean.edgeHeight-100.f));
      }
    std::snprintf(buf,sizeof(buf),"\"npc\":%s,\"error_cm\":%.2f,\"current_deg\":%.2f,\"required_deg\":%.2f,"
                  "\"edge\":%d,\"hit_cm\":%.2f,\"room\":%d,\"visible\":%d,\"line\":%s,\"shot_clear\":%d,\"actual_seen\":%d,"
                  "\"focus\":%s,\"manual\":%d,\"actual_shot_clear\":%d,\"actual\":[%.2f,%.2f,%.2f],\"predicted\":[%.2f,%.2f,%.2f],"
                  "\"aim\":[%.2f,%.2f,%.2f],\"head_clear\":%d,\"edge_cm\":%.2f,\"drop_cm\":%.2f,\"support_y\":%.2f,"
                  "\"head_hit_cm\":%.2f,\"shot_hit_cm\":%.2f,\"limited\":%d,\"room_limited\":%d",
                  jsonStr(bench.sightNpc).c_str(),double(predictionError),double(Aim::playerProbe.lean),
                  double(lean.angle*180.f/float(M_PI)),
                  int(lean.edge),double(lean.hitDistance),int(lean.room),int(lean.visible),jsonStr(leanLineName(lean.line)).c_str(),int(lean.shotClear),
                  int(!r1.hasCol),jsonStr(focus).c_str(),int(manual),actualShotClear,
                  double(hd.x),double(hd.y),double(hd.z),double(predicted.x),double(predicted.y),double(predicted.z),
                  double(aim.x),double(aim.y),double(aim.z),int(lean.headClear),double(lean.edgeDistance),
                  double(std::isfinite(lean.edgeDrop) ? lean.edgeDrop : -1.f),double(lean.edgeHeight),double(lean.headHitDistance),double(lean.shotHitDistance),
                  int(lean.limited),int(lean.roomLimited));
    event("lean-frame",buf);
    std::snprintf(buf,sizeof(buf),"lean-frame %s frame %lld: error_cm %.2f current_deg %.2f required_deg %.2f "
                  "edge %d hit_cm %.2f room %d visible %d line %s shot_clear %d actual_shot_clear %d actual_seen %d focus %s manual %d "
                  "actual (%.2f %.2f %.2f) predicted (%.2f %.2f %.2f)",
                  bench.recName.c_str(),(long long)bench.recFrame,double(predictionError),double(Aim::playerProbe.lean),
                  double(lean.angle*180.f/float(M_PI)),
                  int(lean.edge),double(lean.hitDistance),int(lean.room),int(lean.visible),leanLineName(lean.line),int(lean.shotClear),actualShotClear,
                  int(!r1.hasCol),focus.c_str(),int(manual),
                  double(hd.x),double(hd.y),double(hd.z),double(predicted.x),double(predicted.y),double(predicted.z));
    say(buf);
    }
  }


void aimCameraFrame(const std::string& shot) {
  auto* pl=Gothic::inst().player();
  auto* camera=Gothic::inst().camera();
  if(pl==nullptr || camera==nullptr || !pl->hasManualAim())
    return;
  const Vec3 origin=camera->originLwc();
  const Vec3 front=camera->listenerPosition().front;
  Vec3 crosshair=pl->manualAimPoint();
  float spread=0;
  if(bench.ctrl!=nullptr)
    bench.ctrl->crosshair(crosshair,spread);
  const bool centered=bench.ctrl!=nullptr && bench.ctrl->isCrosshairCentered();
  // Input/pose precedes the camera tick. Use the rendered center ray at the target's depth,
  // retaining both input points so a one-frame tracking error cannot look like clear sight.
  const Vec3 aim=centered ? origin+front*std::max(0.f,Vec3::dotProduct(crosshair-origin,front)) : crosshair;
  const float distance=(aim-origin).length();
  const float pitch=-std::atan2(front.y,std::hypot(front.x,front.z))*180.f/float(M_PI);
  std::vector<HitZones::Posed> capsules;
  pl->hitZonesPosed(capsules);
  float clearance=std::numeric_limits<float>::infinity(), closestAlong=0;
  size_t closest=0, hits=0;
  std::string intersections="[";
  for(size_t i=0;i<capsules.size();++i) {
    const auto& capsule=capsules[i];
    float along=0;
    const float gap=aimSegmentDistance(origin,aim,capsule.a,capsule.b,along)-capsule.r;
    if(gap<clearance) { clearance=gap; closest=i; closestAlong=along; }
    if(gap<=0) {
      if(hits++>0) intersections+=',';
      intersections+="{\"index\":"+std::to_string(i)+",\"zone\":"+jsonStr(HitZones::name(capsule.zone))+
                     ",\"clearance_cm\":"+ragdollNumber(gap)+"}";
      }
    }
  intersections+=']';
  std::string equipment;
  for(bool ammo:{false,true}) {
    Matrix4x4 matrix;
    Vec3 box[2];
    const bool available=pl->aimAttachmentBounds(ammo,matrix,box);
    equipment+=std::string(",\"")+(ammo ? "arrow" : "bow")+"\":{\"available\":"+(available ? "true" : "false");
    if(available) {
      auto inverse=matrix;
      inverse.inverse();
      Vec3 a=origin, b=aim;
      inverse.project(a); inverse.project(b);
      // NPC attachment scales are uniform. The smallest basis length is conservative if
      // a model supplies a nonuniform scale (rotation and scale have no shear here).
      float scale=std::numeric_limits<float>::infinity();
      for(int i=0;i<3;++i)
        scale=std::min(scale,Vec3(matrix.at(i,0),matrix.at(i,1),matrix.at(i,2)).length());
      const float gap=aimBoxDistance(a,b,box)*scale;
      equipment+=",\"model\":\"morph-expanded-attachment-obb\",\"conservative\":true,\"clearance_cm\":"+ragdollNumber(gap)+
                 ",\"intersects\":"+(gap<=1e-4f ? "true" : "false");
      }
    equipment+='}';
    }
  // Check the rendered near plane and a 10 cm shell around the optical origin.
  // Cast both ways so a wall's triangle winding cannot hide an embedded camera.
  int wallHits = 0;
  auto* world = Gothic::inst().world();
  auto nearView = camera->viewProj();
  nearView.inverse();
  if(world!=nullptr) {
    auto probe = [&](const Vec3& point) {
      if(world->physic()->ray(origin,point).hasCol || world->physic()->ray(point,origin).hasCol)
        ++wallHits;
      };
    for(float x:{-1.f,1.f})
      for(float y:{-1.f,1.f}) {
        Vec3 corner(x,y,0);
        nearView.project(corner);
        probe(corner);
        }
    for(const Vec3 axis:{Vec3(1,0,0),Vec3(0,1,0),Vec3(0,0,1)}) {
      probe(origin+axis*10.f);
      probe(origin-axis*10.f);
      }
    }
  const Vec3 forward = facing(*pl);
  const Vec3 shoulder = Camera::aimShoulder(pl->position(),forward);
  Vec3 pivot, direction;
  camera->aimRay(pivot,direction);
  const bool wallAhead = world!=nullptr && world->physic()->ray(shoulder,shoulder+forward*85.f).hasCol;
  const Vec3 look = pl->aimLook();
  const Vec3 head = pl->mapHeadBone();
  const auto edge = pl->aimEdge(look);
  const auto headRay = pl->world().physic()->ray(head,head+look*200.f);
  const bool headClear = !headRay.hasCol || (edge.found && headRay.v.y<edge.supportY-100.f);
  event("aim-camera","\"shot\":"+jsonStr(shot)+",\"origin\":"+ragdollVector(origin)+",\"front\":"+ragdollVector(front)+
        ",\"camera_wall_hits\":"+std::to_string(wallHits)+",\"wall_ahead\":"+(wallAhead ? "true" : "false")+
        ",\"pivot_reach_cm\":"+ragdollNumber(Vec3::dotProduct(pivot-shoulder,forward))+
        ",\"feet\":"+ragdollVector(pl->position())+",\"head\":"+ragdollVector(head)+",\"stance_look\":"+ragdollVector(look)+
        ",\"lean_deg\":"+ragdollNumber(Aim::playerProbe.lean)+",\"stance_head_clear\":"+(headClear ? "true" : "false")+
        ",\"edge_distance_cm\":"+ragdollNumber(edge.found ? edge.distance : -1.f)+
        ",\"aim_point\":"+ragdollVector(aim)+",\"manual_aim_point\":"+ragdollVector(pl->manualAimPoint())+
        ",\"crosshair_point\":"+ragdollVector(crosshair)+",\"centered\":"+(centered ? "true" : "false")+
        ",\"pitch_deg\":"+ragdollNumber(pitch)+",\"draw\":"+ragdollNumber(Archer::playerProbe.draw)+",\"distance_cm\":"+ragdollNumber(distance)+
        ",\"body_clearance_cm\":"+ragdollNumber(clearance)+",\"body_closest_zone\":"+
        (capsules.empty() ? "null" : jsonStr(HitZones::name(capsules[closest].zone)))+
        ",\"body_closest_index\":"+(capsules.empty() ? "null" : std::to_string(closest))+
        ",\"body_closest_along_cm\":"+ragdollNumber(closestAlong*distance)+",\"body_hit_count\":"+std::to_string(hits)+
        ",\"body_hits\":"+intersections+equipment);
  }

void TestBench::capture(Pixmap& frame, float uiScale) {
  bench.uiScale = uiScale;
  Pixmap pm;
  if(frame.w()>0 && frame.format()!=TextureFormat::RGBA8)
    pm = Pixmap(frame,TextureFormat::RGBA8); else
    pm = std::move(frame);

  if(pm.w()>0)
    if(auto* world=Gothic::inst().world())waterFilmFraming(*world,pm.w(),pm.h());

  if(pm.w()>0 && bench.overlay)
    drawOverlay(pm);
  if(pm.w()>0 && bench.cam==C_Two && bench.camNpc=="arena")
    drawArenaMark(pm);
  if(pm.w()>0 && bench.living.livingDebug) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    if(!bench.overlay) drawZones(cv,Gothic::inst().camera());
    const auto counts=livingCounts();char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"LIVING BODIES: "+bench.living.livingPhase,2,Text);
    std::snprintf(label,sizeof(label),"LIVE %zu  NATIVE KIN %zu / %zu PARTS  DYNAMIC %zu",counts.alive,counts.kinematic,counts.parts,counts.dynamic);
    cv.text(6,26,label,2,Text);cv.text(6,46,"OVERLAY = GAME HIT ZONES (NOT NATIVE SHAPES)",2,Text);
    }
  if(pm.w()>0 && bench.archery.arrowTrace && bench.archery.arrowNpc!=nullptr) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    const auto& npc=*bench.archery.arrowNpc;
    const auto* body=npc.livingBody()!=nullptr ? npc.livingBody() : npc.ragdollBody();
    const auto& hit=npc.arrowPhysicsProbe();
    const auto* skeleton=npc.visualSkeleton();
    const auto bone=skeleton!=nullptr && hit.bone<skeleton->nodes.size() ?
                    std::string_view(skeleton->nodes[hit.bone].name) : std::string_view("-");
    const char* mode=body==nullptr ? "NONE" : (body->isKinematic() ? "KINEMATIC" :
                     (body->reactionWeight()>0.f ? "ACTIVE MOTORS" : "LIMP"));
    char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"ARROW PHYSICS: "+bench.archery.arrowCase,2,Text);
    std::snprintf(label,sizeof(label),"NATIVE %s  WEIGHT %.3f  PARTS %zu",mode,
                  double(body!=nullptr ? body->reactionWeight() : 0.f),body!=nullptr ? body->partCount() : 0);
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"HIT %.*s  IMPULSE %.2f NS",int(std::min<size_t>(bone.size(),38)),bone.data(),double(hit.impulseNs.length()));
    cv.text(6,46,label,2,Text);
    }
  if(pm.w()>0 && bench.archery.stickingTrace) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    const auto* npc=bench.archery.arrowNpc;auto* world=Gothic::inst().world();
    float error=0;
    if(npc)
      for(size_t i=0;i<npc->stuckArrowCount();++i) {
        Npc::ArrowPinProbe pin;
        if(!npc->stuckArrowProbe(i,pin)) continue;
        const auto expected=npc->renderedBone(pin.bone)*pin.local;
        error=std::max(error,Vec3(pin.world.at(3,0)-expected.at(3,0),pin.world.at(3,1)-expected.at(3,1),pin.world.at(3,2)-expected.at(3,2)).length());
        }
    char label[200];cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"ARROW STICKING: "+bench.archery.stickingCase,2,Text);
    std::snprintf(label,sizeof(label),"ACTUAL DRAW PINS  NPC %zu  WORLD %zu  ERROR %.4f CM",npc?npc->stuckArrowCount():0,
      world?world->stuckArrowCount():0,double(error));
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"BONE LOCAL -> RENDER POSE  %s  NATIVE BODIES %zu",npc && npc->isDown()?"LIMP":"LIVING",
      world?world->physic()->physicsStats().bodies:0);
    cv.text(6,46,label,2,Text);
    }
  if(pm.w()>0 && bench.movables.spellTrace) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    auto* world=Gothic::inst().world();char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});cv.text(6,6,"SPELL PHYSICS: "+bench.movables.spellCase,2,Text);
    std::snprintf(label,sizeof(label),"REAL CAST %d  FIELDS %zu  NATIVE BODIES %zu",bench.movables.spellCastingId,
      world?world->spellFieldCount():0,world?world->physic()->physicsStats().bodies:0);cv.text(6,26,label,2,Text);
    if(!bench.movables.spellActors.empty()) {
      const auto& npc=*bench.movables.spellActors[0].npc;
      std::snprintf(label,sizeof(label),"HUMAN %.0f KG  SPEED %.0f CM/S  FROZEN %d  PROPS %zu",double(npc.physicsMass()),
        double(npc.spellVelocity().length()),int(npc.spellFrozen()),bench.movables.spellItems.size());cv.text(6,46,label,2,Text);
      }
    }
  if(pm.w()>0 && bench.routine.questTrace && bench.routine.questNpc!=nullptr) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    cv.rect(0,0,std::min(cv.w,640),66,{0,0,0,190});
    cv.text(6,6,bench.routine.questCase,2,Text);
    const auto& npc=*bench.routine.questNpc;const auto& q=npc.ragdollQuestReturnState();char label[160];
    std::snprintf(label,sizeof(label),"IMMORTAL %d GUARD %d KO %d RETURN %u",int(npc.isImmortal()),
      Gothic::settingsGetI("GAME","ragdoll_quest_guard"),int(npc.isUnconscious()),npc.ragdollQuestReturnCount());
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"REASON %s DROP %.0f CM",questReason(q.hazards),double(bench.routine.questStable.y-npc.position().y));
    cv.text(6,46,label,2,Text);
    }
  // This runs after the final pose and camera, independently of sight/track overlays.
  // A simultaneous shot and video frame share one sample with the current recording index.
  if(pm.w()>0 && (bench.rec || !bench.shot.empty()))
    aimCameraFrame(bench.shot);
  if(!bench.shot.empty()) {
    if(pm.w()>0 && !bench.sightNpc.empty())
      sightFrame(int(pm.w()),int(pm.h()),bench.shot);
    if(pm.w()>0)
      pm.save((outDir+"/"+bench.shot+".png").c_str());
    say("shot " + bench.shot);
    bench.shot.clear();
    bench.ordered=false;
    benchTexturesFrozen=false;
    }
  if(bench.rec && bench.recVideo) {
    if(!bench.trackNpcs.empty() && pm.w()>0)
      trackFrame(int(pm.w()),int(pm.h()));
    if(!bench.sightNpc.empty() && pm.w()>0)
      sightFrame(int(pm.w()),int(pm.h()),"");
    writeVideo(pm);
    ++bench.recFrame;
    }
  }

void TestBench::monsterCharacterCollision(const Npc& actor, const Npc* target) {
  auto& route=bench.monster.monsterCharacterRoute;
  if(scenarioPath==nullptr || !route.active || route.npc!=&actor)
    return;
  ++route.callbacks;
  event("monster_character_collision","\"actor\":"+std::to_string(actor.world().npcId(&actor))+
    ",\"target\":"+(target!=nullptr ? std::to_string(actor.world().npcId(target)) : "null"));
  }

void TestBench::arrowAt(const void* arrow, float x, float y, float z) {
  if(scenarioPath!=nullptr && bench.started && !bench.done && bench.clothTrace && arrow==bench.clothArrow && bench.time<=bench.clothShotAt+2000) {
    const Vec3 point(x,y,z);
    const auto downstream=Vec3::dotProduct(point-bench.clothShotTarget,-bench.clothShotNormal);
    bench.clothArrowDownstream=std::max(bench.clothArrowDownstream,downstream);
    event("cloth_arrow_trajectory","\"position_cm\":"+ragdollVector(point)+
      ",\"downstream_cm\":"+ragdollNumber(downstream));
    }
  if(scenarioPath==nullptr || !bench.trailOn)
    return;
  const Vec3 p = {x,y,z};
  // the same arrow if it moved within 200 ms (a new arrow may get the place of an old one in memory)
  for(auto& tr:bench.trails) {
    if(tr.key!=arrow || bench.time>tr.last+200)
      continue;
    if(tr.pts.empty() || (tr.pts.back()-p).length()>2.f) {
      if(tr.pts.size()<4000)
        tr.pts.push_back(p);
      }
    tr.last = bench.time;
    return;
    }
  // drop the ones long gone, start this one
  std::erase_if(bench.trails,[](const Bench::Trail& t){ return bench.time>t.last+bench.trailKeep; });
  bench.trails.push_back({arrow,{p},bench.time});
  }
