#include "appcadence.h"
#include "testbench.h"
#include "navbench.h"
#include "navdemos.h"
#include "frameprofile.h"

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
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/bodyarena.h"
#include "game/bodyreplay.h"
#include "game/navigationeyebench.h"
#include "game/navigationmemorybench.h"
#include "game/knownroadsbench.h"
#include "game/npckeysbench.h"
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
#include "graphics/mesh/protomesh.h"
#include "world/movablevobs.h"
#include "world/world.h"
#include "world/bullet.h"
#include "world/waypoint.h"
#include "world/aiqueue.h"
#include "ui/crosshair.h"
#include "utils/keycodec.h"
#include "camera.h"
#include "gothic.h"
#include "marvin.h"

using namespace Tempest;

class ClothBulletBenchDriver final {
  public:
    static void step(DynamicWorld& world,DynamicWorld::BulletBody& bullet,const Vec3& direction,uint64_t dt) {
      world.moveBullet(bullet,direction,dt);
      }
  };

namespace {

const char* envStr(const char* name) {
  const char* v = std::getenv(name);
  return (v!=nullptr && v[0]!='\0') ? v : nullptr;
  }

const char*       scenarioPath = envStr("OPENGOTHIC_BENCH");
std::string       outDir       = envStr("OPENGOTHIC_BENCH_OUT") ? envStr("OPENGOTHIC_BENCH_OUT") : ".";
const bool        keepRunning  = envStr("OPENGOTHIC_BENCH_KEEP")!=nullptr;
constexpr int     Fps          = 30;
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
const bool        npcCharacterTrace = envStr("OPENGOTHIC_NPC_CHARACTER_TRACE")!=nullptr &&
                                     std::string_view(envStr("OPENGOTHIC_NPC_CHARACTER_TRACE"))=="1";
constexpr uint64_t SpeakTailMs = 400;  // the talk key is held this long after the recording ends

std::FILE* logFile = nullptr;

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

float num(const std::vector<std::string>& a, size_t i, float def) {
  return i<a.size() ? std::strtof(a[i].c_str(),nullptr) : def;
  }

bool isNumber(const std::string& s) {
  char* end = nullptr;
  std::strtof(s.c_str(),&end);
  return end!=s.c_str() && *end=='\0';
  }

std::string rest(const std::string& line) {
  const size_t sp = line.find(' ');
  return sp==std::string::npos ? "" : line.substr(sp+1);
  }

void replaceAll(std::string& s, std::string_view from, std::string_view to) {
  for(size_t p=s.find(from); p!=std::string::npos; p=s.find(from,p+to.size()))
    s.replace(p,from.size(),to);
  }

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

Vec3 facing(const Npc& pl) {
  const float a = pl.rotation()*float(M_PI)/180.f;
  return {std::cos(a),0,std::sin(a)};
  }

const char* leanLineName(Aim::LeanLine line) {
  switch(line) {
    case Aim::LeanLine::Shot: return "shot";
    case Aim::LeanLine::Head: return "head";
    default: return "none";
    }
  }

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

enum CamMode : uint8_t {
  C_Game,
  C_Follow, // around the player, relative to its facing
  C_World,  // around the player, fixed world direction
  C_Fixed,  // fixed position, looking at the player
  C_Hold,   // the camera stays as it is: the position and where it looks
  C_Turn,   // yaw at the held camera position, retaining its pitch
  C_Water,  // retain the water overview after deferred player teleports
  C_RagdollArena, // benchmark-only fixed view derived from the complete twenty-model group
  C_Face,   // close to the face, relative to the facing of the body
  C_Shoulder, // over the hero's shoulder at an npc (camNpc)
  C_NpcFace,  // the npc's face, from the hero's side
  C_Two,      // the hero and the npc from the side
  C_Arrow,    // from the side of the newest arrow's way, going along with it (overlay trail records it)
  };

struct Hold {
  KeyCodec::Action a;
  uint64_t         until;
  };

struct BodyAnimationEntry {
  uint32_t seed = 0;
  unsigned side = 0;
  uint64_t time = 0;
  std::string animation;
  int variant = 0;
  uint64_t sourceSeq = 0;
  unsigned consumed = 0;
  };

struct BodyAnimationActor {
  const Npc* npc = nullptr;
  uint32_t rng = 0;
  uint64_t solverCalls = 0, controlledCalls = 0, fallbackCalls = 0, accepted = 0;
  uint64_t lastTime = uint64_t(-1);
  int lastVariant = -1;
  };

struct BodyAnimationControl {
  enum Mode { Default, Isolated, Replay };
  Mode mode = Default;
  bool initialized = false;
  bool valid = true;
  bool active = false;
  bool totalsWritten = false;
  uint32_t seed = 0;
  uint64_t errors = 0, errorsAtStart = 0;
  BodyAnimationActor actors[4];
  unsigned actorCount = 2;
  std::vector<BodyAnimationEntry> fixture;
  std::string sourceEventsHash;
  };

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

  uint64_t    waitUntil = 0;
  bool        waitArrive = false;
  uint64_t    arriveEnd = 0;

  std::vector<Hold>             holds;
  std::vector<KeyCodec::Action> taps;

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
  struct QuestSite { bool ready=false; Vec3 dry={}, direction={}; float waterY=0; std::string waypoint; };
  QuestSite   questCliff,questWater;
  Npc*        questNpc = nullptr;
  std::string questCase;
  Vec3        questStable = {};
  bool        questTrace=false,questWaterCase=false,questSawDrop=false,questSawWater=false,questWait=false,questLaunched=false,questStandingWait=false;
  uint64_t    questDeadline=0,questWallNs=0;
  uint32_t    questLastReturns=0;
  Npc*        subject   = nullptr; // the camera watches this npc instead of the player (gaze)
  PlayerControl* ctrl     = nullptr; // the player's input: the crosshair of the manual aim
  // volley: shots by hand in a row - press, draw to volleyDraw, hold volleyHold, let go, wait for the
  // next arrow
  int         volleyLeft  = 0;
  int         volleyPhase = 0;
  uint64_t    volleyHold  = 0;
  float       volleyDraw  = 1;
  uint64_t    volleyJitter = 0;  // ms more to hold, at random up to that (the sway is a function of time)
  uint64_t    volleyHoldNow = 0;
  uint32_t    lcg          = 1;  // the bench's own random numbers (seed)
  std::vector<Npc*> crew;        // arc: the npcs set out, `all` of fight
  // wander: the crew walks the waynet from waypoint to waypoint near the hero (docs/navigation.md, stage 0)
  std::vector<Npc*>            wanderers;
  std::vector<const WayPoint*> wanderPoints;
  size_t                       wanderLegs = 0;
  struct LivingWalker { Npc* npc=nullptr; WayPoint ends[2]; bool next=false,queued=false; };
  std::deque<LivingWalker> livingWalkers;
  bool livingFixture=false,livingDebug=false,livingMotion=false,livingMeasuring=false,livingMode=false;
  std::string livingPhase;
  Vec3 livingHero;
  struct LivingPhysics { uint64_t time=0;uint64_t dt=0;float milliseconds=0;size_t bodies=0; };
  std::vector<LivingPhysics> livingPhysics;
  bool livingOverflow=false;
  // Test-only props use the body API directly; the map allowlist contains only
  // visuals actually authored in the stock worlds, not these Item mesh copies.
  struct MovableFixture { ObjVisual visual; DynamicWorld::Item body; };
  std::vector<std::unique_ptr<MovableFixture>> movableFixtures;
  std::unordered_set<uintptr_t> movableMapIds;
  std::vector<uintptr_t> movableIds;
  std::unordered_map<uintptr_t,Vec3> movableReference;
  std::unordered_map<uintptr_t,float> movableTravel;
  std::unordered_set<uintptr_t> movableNpcTouched;
  uintptr_t movableTable=0,movableDish=0;
  Vec3 movableCenter,movableWalkStart,movableWalkEnd;
  Npc* movableWalker=nullptr;
  WayPoint movableDestination;
  bool movableReady=false,movableTrace=false,movableMeasuring=false,movableOverflow=false;
  std::string movablePhase;
  uint64_t movablePhaseStart=0,movableSampleTick=0;
  std::vector<LivingPhysics> movablePhysics;
  uint32_t ownMeshZen=uint32_t(-1);
  Vec3 ownMeshAxis,ownMeshApproach,ownMeshHeroStart,ownMeshBodyStart;
  float ownMeshMaxCrossTrack=0;
  std::deque<std::pair<uint64_t,Vec3>> ownMeshHeroHistory;
  bool ownMeshWalking=false;
  unsigned ownMeshArrowHits=0;
  std::unordered_set<uintptr_t> waterIds;
  std::unordered_map<uintptr_t,std::string> waterLabels;
  std::unordered_map<uintptr_t,uint64_t> waterSleepAt;
  std::unordered_map<uintptr_t,Vec3> waterReference;
  std::unordered_map<uintptr_t,float> waterTravel;
  std::vector<Item*> waterItems;
  std::string waterCase,waterWaypoint;
  Vec3 waterDry,waterCenter,waterDirection,waterHeroStart,waterHeroLast,waterObserverStart,waterCameraTarget;
  uint32_t waterBarrelZen=uint32_t(-1),waterCrateZen=uint32_t(-1);
  uint64_t waterStart=0,waterSample=0,waterObserverSample=0;
  float waterHeroMaxStep=0,waterRayTop=0;
  bool waterReady=false,waterTrace=false,waterSwim=false,waterSawSwim=false,waterObserver=false;
  struct WaterActor { Npc* npc=nullptr; std::string role; uint64_t sleepAt=uint64_t(-1); };
  std::vector<WaterActor> waterActors;
  bool waterBodiesReady=false,waterLootReady=false,waterKillOnThrow=false;
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


  std::string waterLootMode;
  std::string waterFilm;
  Vec3 waterFilmLip;
  bool waterFilmKill=false;
  uint64_t waterFilmStart=0;
  std::vector<std::unique_ptr<MovableFixture>> destructionFixtures;
  std::vector<uintptr_t> destructionIds;
  std::map<std::pair<uintptr_t,size_t>,Vec3> destructionFragmentOrigins;
  std::map<std::pair<uintptr_t,size_t>,float> destructionFragmentTravel;
  std::string destructionCase;
  Vec3 destructionCaster,destructionTarget,destructionDirection,destructionActorStart;
  Npc* destructionActor=nullptr;
  WayPoint destructionDestination;
  uint64_t destructionStart=0,destructionNextArrow=0;
  uint64_t destructionLastBreak=0;
  uint64_t destructionFallSample=0;
  unsigned destructionArrows=0;
  bool destructionReady=false,destructionTrace=false,destructionShooting=false;
  bool destructionSleepSampled=false;
  bool destructionSupportTrace=false;
  uint64_t destructionSupportNext=0;
  const Npc* destructionSkinRequest=nullptr;
  std::unordered_map<const ProtoMesh*,std::vector<Resources::VertexA>> destructionSkinGeometry;
  std::string destructionSkinSample="null";
  Npc* destructionMeleeActor=nullptr;
  Npc* destructionMeleeNpc=nullptr;
  Vec3 destructionMeleeFacing,destructionMeleeStand;
  bool destructionMeleeRestance=false;
  uint64_t destructionMeleeNext=0,destructionMeleeDeadline=0;
  unsigned destructionMeleeAttempts=0,destructionMeleeHits=0,destructionMeleeBlows=0;
  bool destructionMeleeRunning=false,destructionMeleeArena=false;
  Npc* destructionFarmNpc=nullptr;
  Vec3 destructionFarmNpcStart,destructionFarmStart,destructionFarmEnd;
  const WayPoint* destructionFarmNpcGoal=nullptr;
  uint32_t destructionFarmZen=uint32_t(-1);
  float destructionFarmNpcTravel=0;
  float destructionFarmNpcBest=std::numeric_limits<float>::infinity();
  struct CharacterRoute {
    std::string name,waypoint;
    Vec3 start,goal,direction;
    float tolerance=80;
    bool prepared=false,active=false,reached=false,sawSwim=false,sawSlide=false,falling=false;
    unsigned fallCount=0;
    Vec3 lastSupported;
    uint64_t began=0,arrived=0;
  } characterRoute;
  struct CharacterIdleSlope {
    bool prepared=false,npc=false;
    float angle=0;
    uint64_t began=0,previousTick=0;
    Vec3 start,previous;
  } characterIdleSlope;
  struct CharacterFrame {
    std::string route;
    uint64_t frame=0,time=0,dt=0;
    Vec3 root,groundVelocity,visualRoot;
    bool moving=false,air=false,fall=false,swim=false,enabled=false,active=false,captured=false,normal=false;
    float physics=0,wall=0;
  };
  std::vector<CharacterFrame> characterFrames;
  struct NpcCharacterRoute {
    Npc* npc=nullptr;
    WayPoint destination;
    Interactive* seat=nullptr;
    std::string name;
    Vec3 start,goal,lastPosition,lastVisual,progressPosition;
    uint64_t began=0,arrived=0,progressTime=0;
    unsigned stalls=0;
    float maxVisualJump=0,maxPhysicalJump=0,maxAboveFloor=0,mugMassKg=0;
    bool mugContact=false;
    uint64_t supportSince=0;Vec3 preparationSample;
    bool sawPreparationSample=false;
    std::deque<std::string> supportHistory;
    bool active=false,reached=false,stalled=false,sat=false,standing=false;
  } npcCharacterRoute;
  struct OwnMeshChest {
    Interactive* chest=nullptr;
    size_t lid=size_t(-1);
    bool opened=false,sawTransition=false,taken=false;
    std::string lastAnimation;
    std::map<size_t,size_t> contents,heroBefore;
  } ownMeshChest;
  struct CharacterMoveFrame {
    Vec3 requested,acceptedDelta,steepNormal;
    uint64_t sequence=0,dt=0;
    float advancementRatio=0;
    bool valid=false,steepContact=false,blocked=false;
  };
  struct MonsterCharacterRoute {
    Npc* npc=nullptr;
    WayPoint destination,heroDestination;
    bool heroRun=false,isolatedLedge=false;
    std::string name,animation;
    Vec3 start,goal,lastPosition,lastVisual,progressPosition,lastSupported,attackStart;
    uint64_t began=0,arrived=0,progressTime=0,attackEnded=0;
    unsigned stalls=0,falls=0,cvFrames=0,swimFrames=0,callbacks=0;
    float maxPhysicalJump=0,maxVisualJump=0,attackDistance=0,attackRise=0,tolerance=60;
    bool active=false,reached=false,stalled=false,falling=false,wasAirborne=false,sawSwim=false,leftWater=false,chase=false,attack=false;
    Vec3 savedPosition;
    unsigned restoreSteps=0;
  } monsterCharacterRoute;
  // Source shape must outlive the collision handle that borrows its mesh.
  std::unique_ptr<PhysicMeshShape> monsterCharacterLedgeShape;
  DynamicWorld::Item monsterCharacterLedgeBody;
  struct MonsterCharacterFrame {
    std::string route,animation;
    uint64_t time=0,dt=0,npc=0;
    Vec3 root,visual;
    bool enabled=false,active=false,moving=false,air=false,fall=false,swim=false;
    float bodyYaw=0;
    CharacterMoveFrame move;
  };
  std::vector<MonsterCharacterFrame> monsterCharacterFrames;
  struct MonsterCharacterExtra {
    Npc* npc=nullptr;
    std::string role;
    std::vector<WayPoint> points;
    size_t next=0;
    bool loop=false,queued=false,stalled=false,falling=false,wasAirborne=false;
    Vec3 start,lastPosition,lastVisual,progressPosition,lastSupported;
    uint64_t progressTime=0,arrived=0;
    unsigned arrivals=0,stalls=0,falls=0,cvFrames=0;
    float maxPhysicalJump=0,maxVisualJump=0,travel=0;
  };
  std::deque<MonsterCharacterExtra> monsterCharacterExtras;
  struct MonsterCharacterProp {
    std::unique_ptr<MovableFixture> fixture;
    std::string role;
    Vec3 start,lastIntact;
    uintptr_t id=0;
    float travel=0;
    unsigned fragments=0;
    bool broken=false;
  };
  std::vector<MonsterCharacterProp> monsterCharacterProps;
  MonsterCharacterProp monsterCharacterBarrel;
  struct MonsterCharacterPerformance {
    uint64_t time=0,dt=0;
    float physics=0,wall=0,maximumDistance=0;
    unsigned nearby=0,virtualNearby=0,moving=0;
    bool captured=false,normal=false;
  };
  std::vector<MonsterCharacterPerformance> monsterCharacterPerformance;
  bool monsterCharacterMeasuring=false,monsterCharacterHasWall=false,monsterCharacterCrowd=false,monsterCharacterPack=false;
  std::chrono::steady_clock::time_point monsterCharacterLastWall;
  const Npc* monsterCharacterRenderRequest=nullptr;
  Vec3 monsterCharacterHeroStart;
  uint32_t monsterCharacterSeed=1;
  unsigned monsterCharacterMinNearby=20,monsterCharacterMaxNearby=0;
  float monsterCharacterMaximumDistance=0;
  struct NpcCharacterFrame {
    std::string phase;
    uint64_t frame=0,time=0,dt=0,npc=0;
    Vec3 root,visual;
    bool enabled=false,active=false,moving=false,air=false,interactive=false,captured=false,normal=false;
    float physics=0,wall=0;
    unsigned nearby=0,virtualNearby=0,movingNearby=0,movingVirtualNearby=0;
    CharacterMoveFrame move;
  };
  std::deque<LivingWalker> npcCharacterCrowd;
  std::vector<NpcCharacterFrame> npcCharacterFrames;
  struct NpcCharacterTraceActor { Npc* npc=nullptr;Vec3 lastRoot,lastVisual;bool hasPrevious=false; };
  std::vector<NpcCharacterTraceActor> npcCharacterTraceActors;
  struct CharacterIdle {
    Npc* actor=nullptr;
    Npc* fixtureNpc=nullptr;
    std::string site,role;
    Vec3 initial;
    uint64_t began=0;
  } characterIdle;
  bool npcCharacterMeasuring=false,npcCharacterHasWall=false,npcCharacterObserving=false;
  std::chrono::steady_clock::time_point npcCharacterLastWall;
  std::string npcCharacterWindow;
  struct NpcCharacterGoal { Vec3 target;const Npc* npc=nullptr;std::string name,kind;bool active=false; };
  std::unordered_map<const Npc*,NpcCharacterGoal> npcCharacterGoals;
  std::unordered_set<const Npc*> npcCharacterWalkers;
  uint64_t npcCharacterObservedFrames=0,npcCharacterCvFrames=0,npcCharacterArrivals=0,npcCharacterObserveStart=0;
  unsigned npcCharacterMaxNearby=0,npcCharacterCrowdMinNearby=20,npcCharacterCrowdMaxNearby=0;
  float npcCharacterCrowdMaximumDistance=0;
  std::chrono::steady_clock::time_point characterLastWall;
  bool characterHasWall=false,characterMeasuring=false;
  uint64_t characterFrame=0;
  Item* characterItem=nullptr;
  std::unique_ptr<MovableFixture> characterProp;
  uintptr_t characterPropId=0;
  std::string characterPropRole;
  Vec3 characterPropInitial,characterPropHeroInitial;
  float characterPropTravel=0;
  uint64_t characterPropBegan=0;
  Vec3 characterPropDirection;
  uint64_t characterMeasureStart=0;
  std::unique_ptr<ObjVisual> characterPlatformVisual;
  std::unique_ptr<PhysicMesh> characterPlatform;
  Matrix4x4 characterPlatformMatrix;
  MoveTrigger* characterMover=nullptr;
  Npc* characterRideNpc=nullptr;
  Vec3 characterRideStart,characterRideHeroStart,characterRideLocalStart,characterRideDirection,characterRidePivot;
  Matrix4x4 characterRideMatrixStart;
  float characterRideAngle=0,characterRideTravel=0,characterRideMaximumDrift=0;
  float characterRideMaximumHorizontalDrift=0,characterRideMaximumLocalDrift=0;
  uint64_t characterRideBegan=0;
  bool characterRiding=false,characterRideMoving=false,characterRideRotating=false;
  uint64_t movableWakeStart=0;
  bool movableWakeTrace=false;
  std::unordered_map<uintptr_t,float> movableWakeTravel;
  struct CandleAttachment {
    uint32_t zenId=uint32_t(-1);
    std::string visual;
    bool light=false;
    Vec3 local,initial;
    float maximumError=0,maximumTravel=0;
  };
  std::vector<CandleAttachment> candleAttachments;
  uintptr_t candleBody=0;
  uint32_t candleZenId=uint32_t(-1),candleTableZenId=uint32_t(-1);
  Matrix4x4 candleTableInitial;
  Vec3 candleInitial,candleSupport;
  Vec3 candleTableBounds[2];
  float candleMaximumTilt=0,candleMaximumTravel=0;
  bool candleTrace=false,candleLeftTable=false,candleSnapshotMissing=false,candleCaptured=false;
  uint64_t candleStart=0,candleSamples=0;
  struct NoiseListener { Npc* npc=nullptr; std::string role; Vec3 initial; unsigned heard=0; };
  std::vector<NoiseListener> noiseListeners;
  bool noiseTrace=false,noiseHasPushed=false,noiseTipNegative=false;
  std::string noisePhase;
  uint64_t noiseStart=0;
  Vec3 noiseSource;
  Matrix4x4 noiseTableInitial;
  float noiseTableMaximumTilt=0;
  std::unordered_map<uint32_t,unsigned> noisePerceptions;
  std::unordered_map<const Npc*,uint32_t> noiseNpcIds;
  uint32_t noiseNextNpcId=0;
  Npc* noiseHost=nullptr;
  uint64_t noiseMeasuredImpacts=0,noiseMeasuredHeard=0,noiseMeasuredRays=0;
  bool noiseBridgeDebug=false;
  uint64_t noiseBridgeImpacts=0,noiseBridgeLoudImpacts=0,noiseBridgeCulpritImpacts=0;
  uint64_t noiseBridgeUnseenCulpritImpacts=0,noiseBridgeCupImpacts=0;
  uint64_t noiseBridgeDialogueImpacts=0,noiseBridgeCombatImpacts=0;
  float noiseBridgeCupMaxRadius=0;
  std::vector<uint64_t> noiseBridgeHeard;
  std::vector<uint64_t> noiseBridgeEvents;
  Npc* traceHost=nullptr;
  uint32_t traceTable=uint32_t(-1);
  Matrix4x4 traceOrigin;
  Vec3 traceCenter,traceVisible,traceBlocked,traceFar,traceWall,traceShotDirection;
  bool traceReady=false;
  Npc* destructionTraceHost=nullptr;
  World* destructionTraceWorld=nullptr;
  uint32_t destructionTraceZen=uint32_t(-1);
  Vec3 destructionTraceSource;
  bool destructionTraceDebug=false;
  Npc* arrowNpc=nullptr;
  std::string arrowCase,arrowAim;
  float arrowDraw=1.f;
  uint64_t arrowSampleTick=0;
  bool arrowTrace=false,arrowKoOnHit=false;
  uint32_t arrowSeenHits=0;
  bool stickingTrace=false;
  std::string stickingCase;
  uint64_t stickingSampleTick=0;
  Vec3 stickingWorldPoint;
  struct SpellActor {Npc* npc=nullptr;std::string role;};
  std::vector<SpellActor> spellActors;
  std::vector<Item*> spellItems;
  std::map<int32_t,size_t> spellRunes;
  bool spellArenaReady=false,spellTrace=false,spellWasFrozen=false;
  std::string spellCase;
  uint64_t spellSampleTick=0,spellReleaseAt=0,spellWaitUntil=0;
  int32_t spellCastingId=0;
  int32_t spellEquipPending=0;
  uint64_t spellEquipUntil=0;
  int spellWaitFrozen=-1;
  bool spellGuardTrace=false;
  std::string spellGuardCase;
  uint32_t spellGuardObservedReturns=0;
  Npc* seatNpc=nullptr;
  Npc* seatCorpse=nullptr;
  Interactive* seatMob=nullptr;
  std::vector<Item*> seatItems;
  std::vector<Vec3> seatPreviousParts,seatPreviousItems;
  uint64_t seatSampleTick=0;
  std::string seatPhase;
  Npc*        downNpc      = nullptr; // waitdown: till it falls
  uint64_t    downFrom = 0, downEnd = 0;
  Npc*        waitRagdollNpc = nullptr;
  std::string waitRagdollState;
  uint64_t    waitRagdollEnd = 0;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  bool        cadenceVerified=false,cadenceWait=false;
  uint32_t    cadenceDown=0;
#endif
  bool        ragdollChurn = false;
  uint64_t    ragdollPulseFrame = 0;
  std::vector<Vec3> ragdollArenaPlaces;
  std::vector<float> ragdollArenaRadii;
  Vec3        ragdollArenaTarget;
  std::string ragdollArenaWaypoint;
  bool        ragdollArenaCamera = false;
  std::string ragdollCoverageMode;
  size_t      ragdollCoverageFrames = 0;
  bool        ragdollCoverageActive = false;
  bool        ragdollCoverageFailed = false;
  struct BirthLive { Vec3 position; uint64_t tick; bool inAir; };
  Npc*        birthActor = nullptr;
  std::string birthLabel;
  std::deque<BirthLive> birthHistory;
  Npc*        birthContinuation = nullptr;
  std::string birthContinuationLabel;
  uint64_t    birthTick = 0;
  const Npc*  expectedRagdollLoot = nullptr;
  bool        ragdollLootDidOpen = false;
  struct PerfSample {
    std::string segment;
    uint64_t frame = 0;
    float wallMs = 0, stepMs = 0;
    size_t worldBodies = 0, actors = 0, corpses = 0, physical = 0, sleeping = 0, parts = 0, centersInView = 0, visibleMeshes = 0;
    size_t physicalBeforeImpulse = 0, sleepingBeforeImpulse = 0;
    uint64_t rays = 0;
    };
  std::vector<PerfSample> ragdollPerformance;
  std::vector<PerfSample> ragdollCoverage;
  std::chrono::steady_clock::time_point lastPerfFrame;
  bool        hasPerfFrame = false;
  float       perfWallMs = 0;
  size_t      perfPhysicalBeforeImpulse = 0, perfSleepingBeforeImpulse = 0;
  Npc*        duelA        = nullptr; // bodyduel: bench-owned NPCs, never resolved by their script symbols
  Npc*        duelB        = nullptr;
  uint64_t    duelFrom = 0, duelEnd = 0;
  bool        duelWaiting  = false;
  std::vector<Npc*> observeActors;
  uint64_t    observeFrom = 0, observeEnd = 0;
  bool        observeActive = false;
  Vec3        observeVisibleA, observeVisibleB;
  bool        observeVisibleSaved = false;
  std::string observeProbeCase;
  std::unique_ptr<::Sound> probeSound;
  uint64_t    probeSoundStart = 0, probeSoundDuration = 0;
  bool        probeOccluded = false;
  std::vector<std::pair<Npc*,Vec3>> archers; // archer: they stand there and shoot at archerAt...
  Npc*        archerAt     = nullptr;
  bool        archerDice   = false;       // ...by the dice
  uint64_t    volleyT     = 0;
  // aimat: the manual aim aims there instead of the mouse, every frame
  Npc*           aimNpc   = nullptr;
  HitZones::Zone aimZone  = HitZones::Body;
  bool        aimLeg    = false;  // aimat ... leg: the middle of the legs' capsules (limb takes the arms too)
  bool           aimFixed = false;
  bool           aimRaw   = false; // no holding over for the drop of the arrow
  bool           aimMouse = false; // the game's camera turns onto it, as the mouse would: the shot from the
                                   // center of the screen
  bool           crossValid = false;
  Vec3           crossPrev  = {}; // on the screen, -1..1
  float          aspect     = 4.f/3.f; // of the captured frame
  float          uiScale    = 1;       // of the game's interface
  Vec3           aimAt    = {};
  // joints of the watched one's animated pose in the last two frames: a pop is a jump of the second difference
  std::vector<Vec3> anim1, anim2;
  // the hero placed around the subject: distance, angle from its facing (0 in front), orbiting
  float       placeDist  = 0;
  float       placeAngle = 0;
  float       orbitSpeed = 0;     // degrees per second
  uint64_t    orbitUntil = 0;

  // talking (scripts/shorts)
  std::string camNpc;           // C_Shoulder, C_NpcFace, C_Two
  std::string zonesNpc;         // showzones: the npc whose hit zones the overlay draws
  bool        zonesOnly = false; // the overlay draws the hit zones only
  // overlay trail: the way each arrow flew, kept trailKeep ms after its last move; nothing else drawn
  struct Trail { const void* key = nullptr; std::vector<Vec3> pts; uint64_t last = 0; };
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
  struct SavedNpc {
    Npc* npc = nullptr;
    std::vector<uint8_t> bytes;
    std::optional<Ragdoll::State> body;
    std::vector<std::pair<std::string,Vec3>> bones;
    Vec3 position;
    int hp = 0;
    bool dead = false, unconscious = false, enabled = false;
    };
  std::unordered_map<std::string,SavedNpc> savedRagdollNpcs;
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

// a line of events.jsonl: what happened when, in the recording's frames (the cut of a short lines up its
// subtitles and sound by them); `fields` are more json members, without braces
void event(std::string_view kind, const std::string& fields = {}) {
  if(bench.events==nullptr)
    bench.events = std::fopen((outDir+"/events.jsonl").c_str(),"w");
  if(bench.events==nullptr)
    return;
  std::fprintf(bench.events,"{\"t\":%llu,\"seg\":%s,\"frame\":%lld,\"kind\":%s%s%s}\n",
               (unsigned long long)bench.time,jsonStr(bench.rec ? bench.recName : "").c_str(),
               bench.rec ? (long long)bench.recFrame : -1LL,jsonStr(kind).c_str(),fields.empty() ? "" : ",",fields.c_str());
  std::fflush(bench.events);
  }

void stopBodyLive() {
  if(!bench.bodyLive)
    return;
  BodyController::setSink({});
  bench.bodyLive = false;
  }

const char* animationModeName() {
  switch(bench.bodyAnimation.mode) {
    case BodyAnimationControl::Isolated: return "isolated";
    case BodyAnimationControl::Replay: return "replay";
    default: return "default";
    }
  }

void animationControlError(const std::string& reason) {
  ++bench.bodyAnimation.errors;
  say("body animation control: " + reason);
  event("body_animation_control_error","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
         ",\"seed\":"+std::to_string(bench.bodyAnimation.seed)+",\"reason\":"+jsonStr(reason));
  // The existing strict baseline reader must reject a failed replay, even if the duel finishes.
  event("body_duel_error","\"reason\":"+jsonStr("body animation control: "+reason));
  }

bool uniqueJsonKeys(const rapidjson::Value& value) {
  if(value.IsObject()) {
    std::unordered_set<std::string> names;
    for(auto i=value.MemberBegin(); i!=value.MemberEnd(); ++i)
      if(!names.emplace(i->name.GetString(),i->name.GetStringLength()).second || !uniqueJsonKeys(i->value))
        return false;
    }
  else if(value.IsArray()) {
    for(const auto& i:value.GetArray())
      if(!uniqueJsonKeys(i))
        return false;
    }
  return true;
  }

int blockVariant(std::string_view name) {
  for(std::string_view prefix:{"T_1HPARADE_0","T_2HPARADE_0"}) {
    if(name==prefix) return 0;
    if(name==std::string(prefix)+"_A2") return 1;
    if(name==std::string(prefix)+"_A3") return 2;
    }
  return -1;
  }

bool initializeAnimationControl() {
  auto& control = bench.bodyAnimation;
  if(control.initialized)
    return control.valid;
  control.initialized = true;
  const char* mode = envStr("OPENGOTHIC_BODY_ANIMATION_RNG");
  if(mode==nullptr)
    return true;
  if(std::string_view(mode)=="isolated") {
    control.mode = BodyAnimationControl::Isolated;
    return true;
    }
  control.mode = BodyAnimationControl::Replay;
  const auto fail = [&](const std::string& reason) {
    control.valid = false;
    animationControlError(reason);
    return false;
    };
  if(std::string_view(mode)!="replay")
    return fail("unknown OPENGOTHIC_BODY_ANIMATION_RNG mode");
  const char* path = envStr("OPENGOTHIC_BODY_ANIMATION_FIXTURE");
  if(path==nullptr)
    return fail("replay requires OPENGOTHIC_BODY_ANIMATION_FIXTURE");
  std::ifstream input(path,std::ios::binary);
  if(!input)
    return fail("cannot read block-variant fixture");
  input.seekg(0,std::ios::end);
  const auto size = input.tellg();
  if(size<0 || size>4*1024*1024)
    return fail("invalid block-variant fixture size");
  input.seekg(0,std::ios::beg);
  const std::string text((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
  rapidjson::Document fixture;
  fixture.Parse<rapidjson::kParseValidateEncodingFlag>(text.c_str(),text.size());
  if(fixture.HasParseError() || !fixture.IsObject() || !uniqueJsonKeys(fixture))
    return fail("invalid/duplicate-key block-variant fixture JSON");
  if(!fixture.HasMember("schema_version") || !fixture["schema_version"].IsInt() || fixture["schema_version"].GetInt()!=1 ||
     !fixture.HasMember("kind") || !fixture["kind"].IsString() || std::string_view(fixture["kind"].GetString())!="body_animation_branch_fixture" ||
     !fixture.HasMember("source_events_sha256") || !fixture["source_events_sha256"].IsString() ||
     !fixture.HasMember("entries") || !fixture["entries"].IsArray())
    return fail("unsupported/incomplete block-variant fixture");
  control.sourceEventsHash = fixture["source_events_sha256"].GetString();
  if(control.sourceEventsHash.size()!=64 || control.sourceEventsHash.find_first_not_of("0123456789abcdef")!=std::string::npos)
    return fail("invalid fixture source_events_sha256");
  std::unordered_set<std::string> keys;
  std::unordered_set<uint64_t> sourceSequences;
  for(const auto& item:fixture["entries"].GetArray()) {
    if(!item.IsObject() || !item.HasMember("seed") || !item["seed"].IsUint() ||
       !item.HasMember("role") || !item["role"].IsString() ||
       !item.HasMember("relative_time_ms") || !item["relative_time_ms"].IsUint64() ||
       !item.HasMember("animation") || !item["animation"].IsString() ||
       !item.HasMember("variant") || !item["variant"].IsInt() ||
       !item.HasMember("source_seq") || !item["source_seq"].IsUint64() || item["source_seq"].GetUint64()==0)
      return fail("incomplete block-variant fixture entry");
    BodyAnimationEntry entry;
    entry.seed = item["seed"].GetUint();
    const std::string_view role = item["role"].GetString();
    if(role!="actor" && role!="target")
      return fail("unknown fixture actor role");
    entry.side = role=="actor" ? 0 : 1;
    entry.time = item["relative_time_ms"].GetUint64();
    entry.animation = item["animation"].GetString();
    entry.variant = item["variant"].GetInt();
    entry.sourceSeq = item["source_seq"].GetUint64();
    if(entry.variant<0 || entry.variant>2 || blockVariant(entry.animation)!=entry.variant)
      return fail("fixture animation/variant mismatch");
    const auto key = std::to_string(entry.seed)+":"+std::to_string(entry.side)+":"+std::to_string(entry.time);
    if(!keys.insert(key).second)
      return fail("duplicate seed/role/time fixture entry");
    if(!sourceSequences.insert(entry.sourceSeq).second)
      return fail("duplicate source_seq fixture entry");
    control.fixture.push_back(std::move(entry));
    }
  return true;
  }

bool beginAnimationControl(uint32_t seed, const std::vector<Npc*>& actors) {
  auto& control = bench.bodyAnimation;
  control.seed = seed;
  if((actors.size()!=2 && actors.size()!=4) || std::any_of(actors.begin(),actors.end(),[](Npc* npc) { return npc==nullptr; }))
    return false;
  if(!initializeAnimationControl())
    return false;
  if(control.mode==BodyAnimationControl::Default)
    return true;
  if(actors.size()==4 && control.mode==BodyAnimationControl::Replay) {
    animationControlError("four-actor animation replay requires a separate fixture contract");
    return false;
    }
  control.active = true;
  control.errorsAtStart = control.errors;
  control.actorCount = unsigned(actors.size());
  for(auto& actor:control.actors) actor = {};
  for(unsigned side=0; side<control.actorCount; ++side) {
    control.actors[side].npc = actors[side];
    // Defined uint32 overflow; slots 0/1 preserve the archived two-actor streams.
    // The four-actor load fixture gets two further independent streams, never std-RNG fallback.
    control.actors[side].rng = seed ^ (0x9e3779b9u*(side+1u));
    if(control.actors[side].rng==0)
      control.actors[side].rng = 0x6d2b79f5u;
    }
  event("body_animation_control_start","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
        ",\"seed\":"+std::to_string(seed)+",\"source_events_sha256\":"+jsonStr(control.sourceEventsHash)+
        (control.actorCount==4 ? ",\"actor_count\":4" : ""));
  return true;
  }

bool beginAnimationControl(uint32_t seed, Npc& first, Npc& second) {
  return beginAnimationControl(seed,std::vector<Npc*>{&first,&second});
  }

unsigned animationActorIndex(const Npc& npc) {
  const auto& control=bench.bodyAnimation;
  for(unsigned index=0;index<control.actorCount;++index)
    if(control.actors[index].npc==&npc) return index;
  return control.actorCount;
  }

std::string animationActorFields(unsigned index) {
  return bench.bodyAnimation.actorCount==4 ? ",\"pair_index\":"+std::to_string(index/2)+
         ",\"animation_actor_index\":"+std::to_string(index) : "";
  }

BodyAnimationEntry* animationFixtureEntry(unsigned side, uint64_t time) {
  for(auto& entry:bench.bodyAnimation.fixture)
    if(entry.seed==bench.bodyAnimation.seed && entry.side==side && entry.time==time)
      return &entry;
  return nullptr;
  }

void observeAnimationAttempt(const Npc& npc, const std::string& fields, uint64_t gameTime) {
  auto& control = bench.bodyAnimation;
  if(!control.active || control.mode==BodyAnimationControl::Default)
    return;
  if((npc.weaponState()!=WeaponState::W1H && npc.weaponState()!=WeaponState::W2H) || Gothic::inst().version().game!=2)
    return;
  const unsigned side = animationActorIndex(npc);
  if(side>=control.actorCount)
    return;
  rapidjson::Document row;
  const auto json = "{"+fields+"}";
  row.Parse(json.c_str(),json.size());
  if(row.HasParseError() || !row.IsObject() || !row.HasMember("requested_anim") || !row["requested_anim"].IsInt() ||
     row["requested_anim"].GetInt()!=int(AnimationSolver::AttackBlock))
    return;
  if(!row.HasMember("accepted") || !row["accepted"].IsBool() || !row["accepted"].GetBool() ||
     !row.HasMember("pose_changed") || !row["pose_changed"].IsBool() || !row["pose_changed"].GetBool())
    return;
  auto& actor = control.actors[side];
  ++actor.accepted;
  const uint64_t time = gameTime-bench.duelFrom;
  std::string animation;
  if(row.HasMember("layers_after") && row["layers_after"].IsArray())
    for(const auto& layer:row["layers_after"].GetArray())
      if(layer.IsObject() && layer.HasMember("name") && layer["name"].IsString() && blockVariant(layer["name"].GetString())>=0) {
        if(!animation.empty()) {
          animationControlError("multiple eligible block layers after accepted attempt");
          return;
          }
        animation = layer["name"].GetString();
        }
  const int variant = blockVariant(animation);
  auto* entry = control.mode==BodyAnimationControl::Replay ? animationFixtureEntry(side,time) : nullptr;
  if(variant<0 || actor.lastTime!=time || actor.lastVariant!=variant)
    animationControlError("accepted block has no matching controlled solver branch");
  if(control.mode==BodyAnimationControl::Replay) {
    if(entry==nullptr)
      animationControlError("unexpected accepted block: no exact seed/role/time fixture entry");
    else if(entry->consumed!=0)
      animationControlError("duplicate accepted block fixture consumption");
    else if(entry->animation!=animation || entry->variant!=variant)
      animationControlError("accepted block differs from fixture animation/variant");
    else if(actor.lastTime==time && actor.lastVariant==variant)
      ++entry->consumed;
    }
  TestBench::bodyEvent(npc,npc.target(),"animation_branch_accept",gameTime,
                      "\"mode\":"+jsonStr(animationModeName())+",\"seed\":"+std::to_string(control.seed)+
                      ",\"role\":"+jsonStr(side%2==0 ? "actor" : "target")+",\"relative_time_ms\":"+std::to_string(time)+
                      ",\"selected_variant\":"+std::to_string(variant)+",\"animation_name\":"+jsonStr(animation)+
                      ",\"source_seq\":"+std::to_string(entry==nullptr ? 0 : entry->sourceSeq)+animationActorFields(side));
  }

void endAnimationControl() {
  auto& control = bench.bodyAnimation;
  if(!control.active)
    return;
  uint64_t expected = 0, consumed = 0, solver = 0, controlled = 0, fallback = 0, accepted = 0;
  for(const auto& entry:control.fixture)
    if(entry.seed==control.seed) {
      ++expected;
      consumed += entry.consumed;
      }
  for(const auto& actor:control.actors) {
    solver += actor.solverCalls;
    controlled += actor.controlledCalls;
    fallback += actor.fallbackCalls;
    accepted += actor.accepted;
    }
  if(control.mode==BodyAnimationControl::Replay && expected!=consumed)
    animationControlError("unconsumed archived block fixture entries at episode terminal");
  const auto errors = control.errors-control.errorsAtStart;
  event("body_animation_control_summary","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
        ",\"seed\":"+std::to_string(control.seed)+",\"expected_entries\":"+std::to_string(expected)+
        ",\"consumed_entries\":"+std::to_string(consumed)+",\"remaining_entries\":"+std::to_string(expected-consumed)+
        ",\"solver_calls\":"+std::to_string(solver)+",\"controlled_calls\":"+std::to_string(controlled)+
        ",\"fallback_calls\":"+std::to_string(fallback)+",\"accepted_blocks\":"+std::to_string(accepted)+
        ",\"errors\":"+std::to_string(errors)+",\"status\":"+jsonStr(errors==0 ? "complete" : "incomplete"));
  control.active = false;
  for(auto& actor:control.actors)
    actor.npc = nullptr;
  }

void animationControlTotals() {
  auto& control = bench.bodyAnimation;
  if(control.mode==BodyAnimationControl::Default || control.totalsWritten)
    return;
  control.totalsWritten = true;
  uint64_t consumed = 0;
  for(const auto& entry:control.fixture)
    consumed += entry.consumed;
  if(control.mode==BodyAnimationControl::Replay && consumed!=control.fixture.size())
    animationControlError("unconsumed archived block fixture entries at scenario end");
  event("body_animation_control_totals","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
        ",\"expected_entries\":"+std::to_string(control.fixture.size())+",\"consumed_entries\":"+std::to_string(consumed)+
        ",\"remaining_entries\":"+std::to_string(control.fixture.size()-consumed)+
        ",\"errors\":"+std::to_string(control.errors)+",\"status\":"+jsonStr(control.errors==0 ? "complete" : "incomplete"));
  }

Npc* npcBySymbol(World& w, std::string_view symbol) {
  const size_t id = w.script().findSymbolIndex(symbol);
  return id==size_t(-1) ? nullptr : w.findNpcByInstance(id);
  }

// `bodyarena`, `bodyreplay`: the configured arena's participants, with isolated animation RNG
void setupArena(World& w, Npc& pl) {
  try {
    if(bench.bodyLive)
      throw std::runtime_error("bodylive must be off before bodyarena/bodyreplay");
    BodyArena::setup(w,pl);
    const auto actors=BodyArena::actors();
    bench.duelFrom=w.tickCount();
    if(!beginAnimationControl(BodyArena::seed(),actors))
      throw std::runtime_error("arena animation RNG setup failed");
    event("body_arena_setup","\"arena\":"+BodyArena::statusJson());
    }
  catch(const std::exception& error) {
    event("body_arena_error","\"reason\":"+jsonStr(error.what()));
    bench.pc=bench.prog.size();
    throw;
    }
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
    // `off: <command>` / `on: <command>`: in that take of a before/after short only (shorts/compare/film.sh
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
    {"melee",  KeyCodec::WeaponMele},{"bow",KeyCodec::WeaponBow}, {"spell",KeyCodec::WeaponMage3},
    {"parade", KeyCodec::Parade},  {"breath",KeyCodec::Walk},
    };
  for(auto& i:keys)
    if(i.name==k) {
      a = i.a;
      return true;
      }
  return false;
  }

void press(PlayerControl& ctrl, KeyCodec::Action a) {
  ctrl.onKeyPressed(a,KeyEvent::K_NoKey,KeyCodec::Mapping::Primary);
  }

void release(PlayerControl& ctrl, KeyCodec::Action a) {
  ctrl.onKeyReleased(a,KeyCodec::Mapping::Primary);
  }

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

void npcCharacterStopMotionTrace(World* world) {
  if(world)
    for(const auto& actor:bench.npcCharacterTraceActors)
      if(world->npcId(actor.npc)!=uint32_t(-1))actor.npc->setCharacterVirtualDiagnosticsEnabled(false);
  bench.npcCharacterTraceActors.clear();
  }

const char* characterMoveFrameColumns = ",diagnostics_valid,move_sequence,move_dt_ms,requested_x_cm,requested_y_cm,requested_z_cm,accepted_dx_cm,accepted_dy_cm,accepted_dz_cm,advancement_ratio,steep_contact,steep_normal_x,steep_normal_y,steep_normal_z,move_blocked";

void writeCharacterMoveFrame(std::FILE* file,const Bench::CharacterMoveFrame& move) {
  std::fprintf(file,",%d,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%d\n",
    int(move.valid),(unsigned long long)move.sequence,(unsigned long long)move.dt,
    double(move.requested.x),double(move.requested.y),double(move.requested.z),
    double(move.acceptedDelta.x),double(move.acceptedDelta.y),double(move.acceptedDelta.z),
    double(move.advancementRatio),int(move.steepContact),double(move.steepNormal.x),
    double(move.steepNormal.y),double(move.steepNormal.z),int(move.blocked));
  }

void finish() {
  stopBodyLive();
  npcCharacterStopMotionTrace(Gothic::inst().world());
  endAnimationControl();
  animationControlTotals();
  if(bench.speaking) {
    std::string reply;
    aiCommand("ai ptt cancel",reply);
    bench.speaking = false;
    }
  stopRecording();
  if(!bench.characterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"route,frame,t_game_ms,dt_ms,x_cm,y_cm,z_cm,move_held,in_air,falling,swimming,cv_enabled,cv_active,ground_vx_cm_s,ground_vy_cm_s,ground_vz_cm_s,step_ms,wall_ms,captured,normal_clock,visual_x_cm,visual_y_cm,visual_z_cm\n");
      for(const auto& row:bench.characterFrames)
        std::fprintf(file,"%s,%llu,%llu,%llu,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%.9g,%.9g,%.9g\n",
          row.route.c_str(),(unsigned long long)row.frame,(unsigned long long)row.time,(unsigned long long)row.dt,
          double(row.root.x),double(row.root.y),double(row.root.z),int(row.moving),int(row.air),int(row.fall),int(row.swim),
          int(row.enabled),int(row.active),double(row.groundVelocity.x),double(row.groundVelocity.y),double(row.groundVelocity.z),
          double(row.physics),double(row.wall),int(row.captured),int(row.normal),
          double(row.visualRoot.x),double(row.visualRoot.y),double(row.visualRoot.z));
      std::fclose(file);
      } else event("character_error","\"reason\":\"cannot write native character frames\"");
    }
  if(!bench.npcCharacterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/npc-character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"phase,frame,t_game_ms,dt_ms,npc_id,x_cm,y_cm,z_cm,visual_x_cm,visual_y_cm,visual_z_cm,cv_enabled,cv_active,moving,in_air,interactive,step_ms,wall_ms,nearby_humans,nearby_cv,moving_humans,moving_cv,captured,normal_clock%s\n",characterMoveFrameColumns);
      for(const auto& row:bench.npcCharacterFrames) {
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
  if(!bench.monsterCharacterFrames.empty()) {
    if(auto* file=std::fopen((outDir+"/monster-character-frames.csv").c_str(),"w")) {
      std::fprintf(file,"route,t_game_ms,dt_ms,npc_id,x_cm,y_cm,z_cm,visual_x_cm,visual_y_cm,visual_z_cm,cv_enabled,cv_active,moving,in_air,falling,swimming,animation,body_yaw_deg%s\n",characterMoveFrameColumns);
      for(const auto& row:bench.monsterCharacterFrames) {
        std::fprintf(file,"%s,%llu,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%d,\"%s\",%.9g",
          row.route.c_str(),(unsigned long long)row.time,(unsigned long long)row.dt,(unsigned long long)row.npc,
          double(row.root.x),double(row.root.y),double(row.root.z),double(row.visual.x),double(row.visual.y),double(row.visual.z),
          int(row.enabled),int(row.active),int(row.moving),int(row.air),int(row.fall),int(row.swim),row.animation.c_str(),double(row.bodyYaw));
        writeCharacterMoveFrame(file,row.move);
        }
      std::fclose(file);
      } else event("monster_character_error","\"reason\":\"cannot write monster frames\"");
    }
  if(!bench.monsterCharacterPerformance.empty()) {
    if(auto* file=std::fopen((outDir+"/monster-character-performance.csv").c_str(),"w")) {
      std::fprintf(file,"t_game_ms,dt_ms,step_ms,wall_ms,nearby_monsters,nearby_cv,moving,maximum_distance_cm,captured,normal_clock\n");
      for(const auto& row:bench.monsterCharacterPerformance)
        std::fprintf(file,"%llu,%llu,%.9g,%.9g,%u,%u,%u,%.9g,%d,%d\n",
          (unsigned long long)row.time,(unsigned long long)row.dt,double(row.physics),double(row.wall),row.nearby,
          row.virtualNearby,row.moving,double(row.maximumDistance),int(row.captured),int(row.normal));
      std::fclose(file);
      } else event("monster_character_error","\"reason\":\"cannot write monster performance samples\"");
    }
  bench.monsterCharacterProps.clear();
  bench.monsterCharacterLedgeBody=DynamicWorld::Item();bench.monsterCharacterLedgeShape.reset();
  bench.characterProp.reset();bench.characterPlatform.reset();bench.characterPlatformVisual.reset();
  bench.destructionFixtures.clear();
  bench.movableFixtures.clear();
  if(ragdollPerf) {
    std::FILE* perf=std::fopen((outDir+"/ragdoll-performance.csv").c_str(),"w");
    if(perf!=nullptr) {
      std::fprintf(perf,"seg,frame,wall_ms,step_ms,world_bodies,rays,actors,corpses,physical,sleeping,parts,centers_in_view,physical_before_impulse,sleeping_before_impulse,visible_mesh_proxies\n");
      for(const auto& sample:bench.ragdollPerformance)
        std::fprintf(perf,"%s,%llu,%.9g,%.9g,%zu,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",sample.segment.c_str(),
                     (unsigned long long)sample.frame,double(sample.wallMs),double(sample.stepMs),sample.worldBodies,
                     (unsigned long long)sample.rays,sample.actors,sample.corpses,sample.physical,sample.sleeping,sample.parts,sample.centersInView,
                     sample.physicalBeforeImpulse,sample.sleepingBeforeImpulse,sample.visibleMeshes);
      std::fclose(perf);
      } else {
      event("ragdoll_error","\"reason\":\"cannot write deferred performance samples\"");
      }
    }
  if(!bench.ragdollCoverage.empty()) {
    if(auto* file=std::fopen((outDir+"/ragdoll-coverage.csv").c_str(),"w")) {
      std::fprintf(file,"mode,frame,actors,corpses,physical,sleeping,parts,centers_in_view,physical_before_impulse,sleeping_before_impulse,visible_meshes\n");
      for(const auto& sample:bench.ragdollCoverage)
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

std::string symName(World& w, const Npc* n) {
  if(n==nullptr)
    return "-";
  if(auto sym = w.script().findSymbol(size_t(n->instanceSymbol())))
    return sym->name();
  return nameUtf8(*n);
  }

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
  bench.camPos = org;
  bench.camHeldAngles = PointF(-pit,yaw);
  }

Vec3 rotY(const Vec3& v, float deg) {
  const float a = deg*float(M_PI)/180.f, c = std::cos(a), s = std::sin(a);
  return {v.x*c - v.z*s, v.y, v.x*s + v.z*c};
  }

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

// the point aimed at: the middle of a zone of the npc; `leg`: the limb capsules below the knees' height (limb
// alone is the middle of arms and legs, at the hips)
bool aimPoint(Npc& n, HitZones::Zone z, bool leg, Vec3& out) {
  if(leg) {
    std::vector<HitZones::Posed> caps;
    n.hitZonesPosed(caps);
    const float knees = n.position().y + 80.f;
    Vec3  sum   = {};
    float count = 0;
    for(auto& c:caps) {
      const Vec3 m = (c.a+c.b)*0.5f;
      if(c.zone==HitZones::Limb && m.y<knees) {
        sum   += m;
        count += 1;
        }
      }
    if(count>0) {
      out = sum*(1.f/count);
      return true;
      }
    }
  return n.hitZoneCenter(z,out);
  }

// cam arrow: beside the newest arrow (from the hero until one flies), looking a little ahead of it
void arrowShot(World& w, Npc& pl, Vec3& org, Vec3& tgt) {
  Vec3 head = pl.position() + Vec3(0,150,0);
  Vec3 dir  = facing(pl);
  const Bench::Trail* tr = nullptr;
  for(auto& t:bench.trails)
    if(t.pts.size()>=2 && (tr==nullptr || t.last>=tr->last))
      tr = &t;
  if(tr!=nullptr) {
    head = tr->pts.back();
    Vec3 d = tr->pts[1]-tr->pts[0];
    d.y = 0;
    if(d.length()>1e-3f)
      dir = d/d.length();
    }
  const Vec3 side = Vec3(-dir.z,0,dir.x)*(bench.camAngle<0 ? -1.f : 1.f);
  org = head + side*bench.camDist + Vec3(0,bench.camHeight,0);
  tgt = head + dir*bench.camLook;
  // the ground may rise beside the way: keep the camera a man's height over it
  const float g = groundY(w,org.x,org.z,org.y+400);
  if(!std::isnan(g) && org.y<g+120)
    org.y = g+120;
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
    lookFrom(bench.camPos,bench.waterCameraTarget);
    return;
    }
  if(bench.cam==C_RagdollArena) {
    lookFrom(bench.camPos,bench.ragdollArenaTarget);
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
  if(bench.cam==C_Follow || bench.cam==C_World)
    org = camOrigin(w,pl,bench.camAngle);
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

// degrees between where the arrow points and the aim (sway, the turn lagging); -1 not aiming by hand
float manualAimOff(Npc& pl) {
  Vec3 pos, dir;
  if(!pl.hasManualAim() || !pl.arrowRay(pos,dir))
    return -1;
  const Vec3 want = Vec3::normalize(pl.manualAimPoint()-pos);
  return std::acos(std::clamp(Vec3::dotProduct(dir,want),-1.f,1.f))*180.f/float(M_PI);
  }

const char* archerStage(Archer::Stage s) {
  switch(s) {
    case Archer::Lowered:   return "lowered";
    case Archer::Raising:   return "raising";
    case Archer::Nocked:    return "nocked";
    case Archer::Drawing:   return "drawing";
    case Archer::Held:      return "held";
    case Archer::Reloading: return "reloading";
    }
  return "?";
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
  std::fprintf(bench.csv,",bd_w,bd_draw,bd_span,bd_travel,bd_miss,bd_follow,bd_fit,bd_offset,bd_angle,hd_hand,hd_fore,hd_arrow,bd_hold,bd_nock,rh_x,rh_y,rh_z,bd_held,bd_overgrip,bd_atmiddle,bd_nockgap,bd_square,bd_cant,bd_bowmove,bd_bowmiss");
  std::fprintf(bench.csv,",lip_valid,lip_on,lip_t,lip_level,lip_mouth,lip_open,lip_f0,lip_f1,lip_alpha,lip_int,lip_picks");
  std::fprintf(bench.csv,",ge_on,ge_talk,ge_id,ge_mood,ge_starts,ge_cuts,ge_stops,ge_nods,ge_pitch,ge_yaw,ge_roll");
  std::fprintf(bench.csv,",ge_listen,ge_think,ge_lnods,ge_aside");
  std::fprintf(bench.csv,",bk_on,bk_w,bk_n,bk_pause,bk_shift,bk_double");
  for(auto& f:FaceColumns)
    std::fprintf(bench.csv,",%s",f.column);
  std::fprintf(bench.csv,",f_on,f_bend,f_yaw,f_twist,f_head,f_hits,f_str,f_ht");
  std::fprintf(bench.csv,",hp_player,hp_subject");
  std::fprintf(bench.csv,",ai_on,ai_want,ai_sigma,ai_errx,ai_erry,ai_lead,ai_pv,ai_ps");
  if(ragdollTrace)
    std::fprintf(bench.csv,",ph_ms,ph_bodies,ph_rays,rd_mode,rd_weight,rd_sleep");
  if(bench.questTrace)
    std::fprintf(bench.csv,",ig_immortal,ig_enabled,ig_anchor,ig_armed,ig_hazards,ig_returns,ig_reason,ig_anchor_x,ig_anchor_y,ig_anchor_z,ig_x,ig_y,ig_z,ig_drop_cm,ig_in_water,ig_ko,ig_wall_ms");
  if(bench.livingFixture)
    std::fprintf(bench.csv,",lb_models,lb_alive,lb_kinematic,lb_parts,lb_dynamic,lb_dynamic_parts,lb_duplicates,lb_world_bodies,lb_ph_ms");
  std::fprintf(bench.csv,"\n");
  }

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

std::string ragdollMatrix(const Matrix4x4& value) {
  std::string result="[";
  for(size_t i=0;i<16;++i) {
    if(i>0) result+=',';
    result+=ragdollNumber(value.data()[i]);
    }
  return result+"]";
  }

std::string ragdollNativePrecision(const Ragdoll::PartState& part) {
  return ",\"center_of_mass_meters\":"+ragdollVector(part.centerOfMassMeters)+
    ",\"rotationxyzw\":["+ragdollNumber(part.rotation.x)+","+ragdollNumber(part.rotation.y)+","+
      ragdollNumber(part.rotation.z)+","+ragdollNumber(part.rotation.w)+"]"+
    ",\"linear_velocity_meters\":"+ragdollVector(part.linearVelocityMeters);
  }

std::string ragdollAncestorFields(const Ragdoll::State& state) {
  std::string result=",\"legacy_ancestors\":"+std::string(state.legacyAncestors ? "true" : "false")+",\"ancestors\":[";
  for(size_t i=0;i<state.ancestors.size();++i) {
    if(i>0) result+=',';
    const auto& ancestor=state.ancestors[i];
    result+="{\"bone\":"+std::to_string(ancestor.bone)+",\"anchor_bone\":"+std::to_string(ancestor.anchorBone)+
      ",\"reference_transform\":"+ragdollMatrix(ancestor.referenceTransform)+"}";
    }
  return result+"]";
  }

std::string ragdollInventory(const Npc& npc, World& world, Inventory::IteratorType type=Inventory::T_Inventory) {
  std::map<size_t,size_t> counts;
  for(auto item=npc.inventory().iterator(type);item.isValid();++item)
    counts[item->clsId()]+=item.count();
  std::string result="[";
  for(const auto& [id,count]:counts) {
    if(result.size()>1) result+=',';
    const auto* symbol=world.script().findSymbol(id);
    result+="{\"class_id\":"+std::to_string(id)+",\"class_name\":"+
      jsonStr(symbol!=nullptr ? symbol->name() : std::string_view{})+",\"count\":"+std::to_string(count)+"}";
    }
  return result+"]";
  }

std::vector<uint8_t> oldNpcArchive(const std::vector<uint8_t>& bytes) {
  mz_zip_archive reader={}, writer={};
  if(!mz_zip_reader_init_mem(&reader,bytes.data(),bytes.size(),0))
    throw std::runtime_error("NPC archive reader initialization failed");
  if(!mz_zip_writer_init_heap(&writer,0,0)) {
    mz_zip_reader_end(&reader);
    throw std::runtime_error("NPC archive writer initialization failed");
    }
  size_t removed=0;
  for(mz_uint i=0;i<mz_zip_reader_get_num_files(&reader);++i) {
    mz_zip_archive_file_stat stat;
    if(!mz_zip_reader_file_stat(&reader,i,&stat)) {
      mz_zip_writer_end(&writer); mz_zip_reader_end(&reader);
      throw std::runtime_error("NPC archive entry read failed");
      }
    const std::string_view name(stat.m_filename);
    if(name.ends_with("/ragdoll") || name.ends_with("/ragdoll-recovery")) {
      ++removed;
      continue;
      }
    if(!mz_zip_writer_add_from_zip_reader(&writer,&reader,i)) {
      mz_zip_writer_end(&writer); mz_zip_reader_end(&reader);
      throw std::runtime_error("NPC archive entry copy failed");
      }
    }
  void* data=nullptr;
  size_t size=0;
  const bool finalized=mz_zip_writer_finalize_heap_archive(&writer,&data,&size)!=0;
  mz_zip_writer_end(&writer); mz_zip_reader_end(&reader);
  if(!finalized || removed!=2) {
    mz_free(data);
    throw std::runtime_error("old NPC fixture must remove exactly the two optional entries");
    }
  const auto* begin=static_cast<const uint8_t*>(data);
  std::vector<uint8_t> result(begin,begin+size);
  mz_free(data);
  return result;
  }

void ragdollNpcArchive(const std::vector<std::string>& args, World& world, Npc& player) {
  const bool saving=args[1]=="save";
  Npc* npc=findNpc(world,player,args.size()>2 ? args[2] : "watched");
  const bool old=!saving && args.size()==5 && args[4]=="old";
  if(npc==nullptr || args.size()<4 || args.size()>5 || (saving && args.size()!=4) ||
     (args.size()==5 && !old) || args[3].empty() || args[3].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
    throw std::runtime_error("expected ragdoll save|restore <npc> <label> [old]");
    }
  const auto id=world.npcId(npc);
  if(id==uint32_t(-1))
    throw std::runtime_error("NPC archive actor does not belong to current world");
  const auto& label=args[3];
  if(saving) {
    if(bench.savedRagdollNpcs.contains(label))
      throw std::runtime_error("NPC archive label already exists");
    Bench::SavedNpc saved;
    saved.npc=npc; saved.position=npc->position(); saved.hp=npc->attribute(ATR_HITPOINTS);
    saved.dead=npc->isDead(); saved.unconscious=npc->isUnconscious();
    saved.enabled=Gothic::settingsGetI("GAME","ragdoll")!=0;
    const auto& probe=npc==&player ? Pose::playerProbe : Pose::subjectProbe;
    if(!probe.valid || probe.skeleton==nullptr || npc!=&subjectOf(player))
      throw std::runtime_error("NPC archive fixture needs the watched actor's complete bone probe");
    for(size_t i=0;i<probe.numBones;++i) {
      const auto& name=probe.skeleton->nodes[i].name;
      saved.bones.emplace_back(name,npc->mapBone(name));
      }
    {
    MemWriter writer(saved.bytes);
    Serialize out(writer); out.setContext(&world); out.setVersion(Serialize::Current);
    npc->save(out,id,"/npc/");
    }
    // Inspect what the real NPC saver wrote, including a dormant body while disabled.
    {
    MemReader reader(saved.bytes); Serialize in(reader); in.setContext(&world); in.setVersion(Serialize::Current);
    RagdollSave::Snapshot snapshot;
    if(!in.setEntry("worlds/",world.name(),"/npc/",id,"/ragdoll") || !RagdollSave::load(in,snapshot))
      throw std::runtime_error("actual NPC save did not write a valid optional body entry");
    saved.body=std::move(snapshot.body);
    }
    std::ofstream file(outDir+"/npc-"+label+".zip",std::ios::binary);
    file.write(reinterpret_cast<const char*>(saved.bytes.data()),std::streamsize(saved.bytes.size()));
    if(!file)
      throw std::runtime_error("unable to write the actual NPC archive fixture");
    event("ragdoll_npc_archive","\"operation\":\"save\",\"label\":"+jsonStr(label)+
          ",\"bytes\":"+std::to_string(saved.bytes.size())+",\"bone_count\":"+std::to_string(saved.bones.size())+
          ",\"saved_body\":"+(saved.body ? "true" : "false")+
          ",\"feature_enabled\":"+(saved.enabled ? "true" : "false")+
          ",\"t_game_ms\":"+std::to_string(world.tickCount()));
    bench.savedRagdollNpcs.emplace(label,std::move(saved));
    say("ragdoll save "+label);
    return;
    }
  auto found=bench.savedRagdollNpcs.find(label);
  if(found==bench.savedRagdollNpcs.end() || found->second.npc!=npc)
    throw std::runtime_error("NPC archive must restore the same saved actor lifetime");
  const auto& saved=found->second;
  const auto bytes=old ? oldNpcArchive(saved.bytes) : saved.bytes;
  {
  MemReader reader(bytes); Serialize in(reader); in.setContext(&world); in.setVersion(Serialize::Current);
  npc->load(in,id,"/npc/"); npc->postValidate();
  }
  npc->prepareRagdoll(); npc->updateAnimation(0,true);
  const bool enabled=Gothic::settingsGetI("GAME","ragdoll")!=0;
  const auto* body=npc->ragdollBody();
  bool matches=npc->attribute(ATR_HITPOINTS)==saved.hp && npc->isDead()==saved.dead && npc->isUnconscious()==saved.unconscious;
  const bool expectedBody=!old && enabled && saved.body.has_value();
  matches=matches && (body!=nullptr)==expectedBody;
  float bodyPosition=0, bodyBasis=0, linear=0, angular=0;
  if(expectedBody && body!=nullptr) {
    const auto actual=body->state(); const auto& expected=*saved.body;
    matches=matches && actual.version==expected.version && actual.skeleton==expected.skeleton && actual.parts.size()==expected.parts.size();
    for(size_t i=0;i<16;++i) {
      const float delta=std::abs(actual.rootTransform.data()[i]-expected.rootTransform.data()[i]);
      matches=matches && std::isfinite(delta);
      if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,delta); else bodyBasis=std::max(bodyBasis,delta);
      }
    if(actual.parts.size()==expected.parts.size())
      for(size_t p=0;p<actual.parts.size();++p) {
        const auto& a=actual.parts[p]; const auto& e=expected.parts[p];
        matches=matches && a.bone==e.bone && a.sleeping==e.sleeping;
        if(actual.version>=3) {
          matches=matches && a.centerOfMassMeters==e.centerOfMassMeters && a.rotation==e.rotation &&
            a.linearVelocityMeters==e.linearVelocityMeters;
          }
        for(size_t i=0;i<16;++i) {
          const float delta=std::abs(a.transform.data()[i]-e.transform.data()[i]);
          matches=matches && std::isfinite(delta);
          if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,delta); else bodyBasis=std::max(bodyBasis,delta);
          if(actual.version>=2) {
            const float referenceDelta=std::abs(a.referenceTransform.data()[i]-e.referenceTransform.data()[i]);
            matches=matches && std::isfinite(referenceDelta);
            if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,referenceDelta); else bodyBasis=std::max(bodyBasis,referenceDelta);
            }
          }
        linear=std::max(linear,(a.linearVelocity-e.linearVelocity).length());
        angular=std::max(angular,(a.angularVelocity-e.angularVelocity).length());
        matches=matches && std::isfinite((a.linearVelocity-e.linearVelocity).length()) && std::isfinite((a.angularVelocity-e.angularVelocity).length());
        }
    if(actual.version>=3) {
      matches=matches && actual.legacyAncestors==expected.legacyAncestors && actual.ancestors.size()==expected.ancestors.size();
      if(actual.ancestors.size()==expected.ancestors.size())
        for(size_t a=0;a<actual.ancestors.size();++a) {
          const auto& got=actual.ancestors[a]; const auto& saved=expected.ancestors[a];
          matches=matches && got.bone==saved.bone && got.anchorBone==saved.anchorBone;
          for(size_t i=0;i<16;++i) {
            const float delta=std::abs(got.referenceTransform.data()[i]-saved.referenceTransform.data()[i]);
            matches=matches && std::isfinite(delta);
            if(i>=12 && i<=14) bodyPosition=std::max(bodyPosition,delta); else bodyBasis=std::max(bodyBasis,delta);
            }
          }
      }
    matches=matches && bodyPosition<=.001f && bodyBasis<=.00001f && linear<=.001f && angular<=.00001f;
    }
  const bool sameMode=!old && saved.enabled==enabled;
  float boneDelta=0;
  for(const auto& [name,before]:saved.bones) {
    const float delta=(npc->mapBone(name)-before).length();
    matches=matches && std::isfinite(delta);
    boneDelta=std::max(boneDelta,delta);
    }
  if(sameMode) matches=matches && boneDelta<=.001f && (npc->position()-saved.position).length()<=.001f;
  event("ragdoll_npc_archive","\"operation\":\"restore\",\"label\":"+jsonStr(label)+
        ",\"old_without_optional_entries\":"+(old ? "true" : "false")+
        ",\"matched\":"+(matches ? "true" : "false")+",\"body_expected\":"+(expectedBody ? "true" : "false")+
        ",\"body_present\":"+(body!=nullptr ? "true" : "false")+
        ",\"same_feature_mode\":"+(sameMode ? "true" : "false")+
        ",\"max_world_bone_delta_cm\":"+ragdollNumber(boneDelta)+
        ",\"max_part_position_delta_cm\":"+ragdollNumber(bodyPosition)+
        ",\"max_part_basis_delta\":"+ragdollNumber(bodyBasis)+
        ",\"max_linear_velocity_delta_cm_s\":"+ragdollNumber(linear)+
        ",\"max_angular_velocity_delta_rad_s\":"+ragdollNumber(angular)+
        ",\"t_game_ms\":"+std::to_string(world.tickCount()));
  if(!matches)
    throw std::runtime_error("actual NPC archive restore differs from saved pose, velocities, sleep or state");
  say("ragdoll restore "+label+(old ? " old" : ""));
  }

void ragdollSessionArchive(const std::vector<std::string>& args, World& world, Npc& player, uint64_t initialDt=0) {
  if(args.size()!=4 || args[3].empty() ||
     args[3].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
    throw std::runtime_error("expected ragdoll session-save|session-snapshot|session-initial <npc> <label>");
  Npc* npc=findNpc(world,player,args[2]);
  const auto id=world.npcId(npc);
  const auto* skeleton=npc!=nullptr ? npc->visualSkeleton() : nullptr;
  if(npc==nullptr || id==uint32_t(-1) || skeleton==nullptr || skeleton->nodes.empty())
    throw std::runtime_error("session archive needs an exact actor with a loaded skeleton");
  const bool saving=args[1]=="session-save";
  const bool initial=args[1]=="session-initial";
  if(initial && initialDt!=0)
    throw std::runtime_error("initial session snapshot must precede the first positive simulation step");
  const auto* body=npc->ragdollBody();
  const std::string prefix=outDir+"/session-"+args[3];
  std::string fields="\"schema_version\":1,\"operation\":"+jsonStr(saving ? "save" : initial ? "initial" : "snapshot")+
    ",\"world\":"+jsonStr(world.name())+",\"actor_id\":"+std::to_string(id)+
    ",\"actor\":"+jsonStr(TestBench::who(npc))+",\"symbol\":"+jsonStr(symName(world,npc))+
    ",\"handle_id\":"+std::to_string(npc->handle().id)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"initial_dt\":"+(initial ? std::to_string(initialDt) : "null")+
    ",\"feature_enabled\":"+(Gothic::settingsGetI("GAME","ragdoll")!=0 ? "true" : "false")+
    ",\"dead\":"+(npc->isDead() ? "true" : "false")+
    ",\"unconscious\":"+(npc->isUnconscious() ? "true" : "false")+
    ",\"recovering\":"+(npc->isRagdollBlending() ? "true" : "false")+
    ",\"hp\":"+std::to_string(npc->attribute(ATR_HITPOINTS))+
    ",\"inventory\":"+ragdollInventory(*npc,world)+
    ",\"position_cm\":"+ragdollVector(npc->position())+
    ",\"object_transform\":"+ragdollMatrix(npc->transform())+
    ",\"sleeping\":"+(body!=nullptr && body->isSleeping() ? "true" : "false")+
    ",\"part_count\":"+std::to_string(body!=nullptr ? body->partCount() : 0)+",\"world_bones\":[";
  for(size_t i=0;i<skeleton->nodes.size();++i) {
    if(i>0) fields+=',';
    const auto& name=skeleton->nodes[i].name;
    fields+="{\"bone\":"+jsonStr(name)+",\"position_cm\":"+ragdollVector(npc->mapBone(name))+"}";
    }
  fields+="],\"snapshot\":";
  if(body!=nullptr) {
    const auto state=body->state();
    fields+="{\"version\":"+std::to_string(state.version)+",\"skeleton\":"+jsonStr(state.skeleton)+
      ",\"root\":"+ragdollMatrix(state.rootTransform)+",\"parts\":[";
    for(size_t i=0;i<state.parts.size();++i) {
      if(i>0) fields+=',';
      const auto& part=state.parts[i];
      fields+="{\"bone\":"+std::to_string(part.bone)+",\"transform\":"+ragdollMatrix(part.transform)+
        ",\"reference_transform\":"+ragdollMatrix(part.referenceTransform)+
        (state.version>=3 ? ragdollNativePrecision(part) : "")+
        ",\"linear_velocity_cm_s\":"+ragdollVector(part.linearVelocity)+
        ",\"angular_velocity_rad_s\":"+ragdollVector(part.angularVelocity)+
        ",\"sleeping\":"+(part.sleeping ? "true" : "false")+"}";
      }
    fields+="]"+(state.version>=3 ? ragdollAncestorFields(state) : "")+"}";
    } else {
    fields+="null";
    }
  if(saving) {
    std::vector<uint8_t> bytes;
    {
    MemWriter writer(bytes);
    Serialize out(writer);
    Pixmap preview(1,1,TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    Gothic::inst().gameSession()->save(out,"ragdoll "+args[3],preview);
    }
    std::ofstream file(prefix+".sav",std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    if(!file)
      throw std::runtime_error("cannot write full GameSession archive");
    fields+=",\"archive\":"+jsonStr("session-"+args[3]+".sav")+",\"archive_bytes\":"+std::to_string(bytes.size());
    }
  std::ofstream json(prefix+".json",std::ios::binary);
  json << '{' << fields << "}\n";
  if(!json)
    throw std::runtime_error("cannot write full session snapshot");
  event("ragdoll_session_archive",fields);
  say("ragdoll "+args[1]+" "+args[3]);
  }

void writeRagdoll(Npc& player) {
  Npc& subject=subjectOf(player);
  const Ragdoll* body=subject.ragdollBody();
  const auto stats=subject.world().physic()->physicsStats();
  const int mode=body!=nullptr ? 3 : 0; // Documented modes: off, kinematic, active, limp.
  std::fprintf(bench.csv,",%.6f,%zu,%llu,%d,%d,%d",double(stats.stepMilliseconds),stats.bodies,
               (unsigned long long)stats.rays,mode,body!=nullptr ? 1 : 0,
               int(body!=nullptr && body->isSleeping()));

  auto id=bench.bodyIds.find(&subject);
  if(id==bench.bodyIds.end())
    id=bench.bodyIds.emplace(&subject,++bench.nextBodyId).first;
  const auto& probe=bench.subject!=nullptr ? Pose::subjectProbe : Pose::playerProbe;
  const auto observed=subject.bodyObservationState();
  std::string fields="\"schema_version\":1,\"actor_id\":"+std::to_string(id->second)+
    ",\"actor\":"+jsonStr(TestBench::who(&subject))+",\"step\":"+std::to_string(bench.step)+
    ",\"t_game_ms\":"+std::to_string(subject.world().tickCount())+
    ",\"dead\":"+(subject.isDead() ? "true" : "false")+
    ",\"unconscious\":"+(subject.isUnconscious() ? "true" : "false")+
    ",\"recovering\":"+(subject.isRagdollBlending() ? "true" : "false")+
    ",\"in_air\":"+(subject.isInAir() ? "true" : "false")+
    ",\"ai_wait_ms\":"+std::to_string(observed.aiWaitMs)+
    ",\"ani_wait_ms\":"+std::to_string(observed.aniWaitMs)+
    ",\"output_wait_ms\":"+std::to_string(observed.outputWaitMs)+
    ",\"animations\":"+jsonStr(subject.animNames())+
    ",\"hp\":"+std::to_string(subject.attribute(ATR_HITPOINTS))+
    ",\"position_cm\":"+ragdollVector(subject.position())+
    ",\"mode\":"+std::to_string(mode)+",\"weight\":"+(body!=nullptr ? "1" : "0")+
    ",\"sleeping\":"+(body!=nullptr && body->isSleeping() ? "true" : "false")+
    ",\"part_count\":"+std::to_string(body!=nullptr ? body->partCount() : 0)+
    ",\"world_bones\":[";
  // Pose::probe is the animation before the procedural passes. mapBone reads the final
  // visual world transform, so this measurement includes NPC movement at recovery.
  if(probe.valid && probe.skeleton!=nullptr)
    for(size_t i=0;i<probe.numBones;++i) {
      if(i>0) fields+=',';
      const auto& name=probe.skeleton->nodes[i].name;
      fields+="{\"bone\":"+jsonStr(name)+",\"position_cm\":"+ragdollVector(subject.mapBone(name))+"}";
      }
  fields+="],\"snapshot\":";
  if(body!=nullptr) {
    const auto snapshot=body->state();
    fields+="{\"version\":"+std::to_string(snapshot.version)+",\"skeleton\":"+jsonStr(snapshot.skeleton)+
      ",\"root\":"+ragdollMatrix(snapshot.rootTransform)+",\"parts\":[";
    for(size_t i=0;i<snapshot.parts.size();++i) {
      if(i>0) fields+=',';
      const auto& part=snapshot.parts[i];
      fields+="{\"bone\":"+std::to_string(part.bone)+",\"transform\":"+ragdollMatrix(part.transform)+
        ",\"reference_transform\":"+ragdollMatrix(part.referenceTransform)+
        (snapshot.version>=3 ? ragdollNativePrecision(part) : "")+
        ",\"linear_velocity_cm_s\":"+ragdollVector(part.linearVelocity)+
        ",\"angular_velocity_rad_s\":"+ragdollVector(part.angularVelocity)+
        ",\"sleeping\":"+(part.sleeping ? "true" : "false")+"}";
      }
    fields+="]"+(snapshot.version>=3 ? ragdollAncestorFields(snapshot) : "")+"}";
    } else {
    fields+="null";
    }
  event("ragdoll_frame",fields);
  }

bool arenaProjected(const Vec3& point) {
  const auto* camera=Gothic::inst().camera();
  if(camera==nullptr) return false;
  float x=point.x,y=point.y,z=point.z,w=1;
  camera->viewProj().project(x,y,z,w);
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w) &&
         w>0 && std::abs(x)<=.90f*w && std::abs(y)<=.90f*w && z>=0 && z<=w;
  }

// A conservative final-pose envelope, actual loaded mesh bounds, and anatomical
// landscape/object sight lines. The untimed PNG independently verifies rendering.
bool arenaMeshVisible(World& world, const Npc& npc) {
  const auto* skeleton=npc.visualSkeleton();
  const auto bounds=npc.bounds();
  if(!TestBench::ragdollBodyDrawable(npc) || skeleton==nullptr || skeleton->nodes.empty() || !std::isfinite(bounds.rConservative) || bounds.rConservative<=0)
    return false;
  const auto object=npc.transform();
  for(int c=0;c<8;++c) {
    Vec3 p{bounds.bbox[c&1].x,bounds.bbox[(c>>1)&1].y,bounds.bbox[(c>>2)&1].z};
    object.project(p);
    if(!arenaProjected(p)) return false;
    }
  for(const auto& bone:skeleton->nodes) {
    const auto p=npc.mapBone(bone.name);
    for(int c=0;c<8;++c)
      if(!arenaProjected(p+Vec3((c&1) ? 50.f : -50.f,(c&2) ? 50.f : -50.f,(c&4) ? 50.f : -50.f)))
        return false;
    }
  for(const auto name:{"BIP01 PELVIS","BIP01 HEAD","BIP01 L FOOT","BIP01 R FOOT"})
    if(skeleton->findNode(name)==size_t(-1) || hidden(world,bench.camPos,npc.mapBone(name)+Vec3(0,5,0)))
      return false;
  return true;
  }

bool arenaCheapVisible(const Npc& npc, size_t index) {
  if(!TestBench::ragdollBodyDrawable(npc) || index>=bench.ragdollArenaRadii.size()) return false;
  const double radius=bench.ragdollArenaRadii[index];
  const auto center=npc.position();
  const auto* camera=Gothic::inst().camera();
  if(camera==nullptr || !std::isfinite(radius) || radius<=0) return false;
  const auto vp=camera->viewProj();
  float x=center.x,y=center.y,z=center.z,w=1;
  vp.project(x,y,z,w);
  if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w) || w<=0 ||
     std::abs(x)>w || std::abs(y)>w || z<0 || z>w) return false;
  // Conservative sphere NONREJECTION; full vertex/occlusion visibility is proven
  // separately by all untimed posed envelopes. Never test the larger cube corners.
  for(int plane=0;plane<6;++plane) {
    double coefficients[4]={};
    for(int column=0;column<4;++column) {
      if(plane<4) coefficients[column]=.90*vp.at(column,3)+(plane%2==0 ? 1. : -1.)*vp.at(column,plane/2);
      else coefficients[column]=plane==4 ? vp.at(column,2) : double(vp.at(column,3))-vp.at(column,2);
      if(!std::isfinite(coefficients[column])) return false;
      }
    const double normal=std::sqrt(coefficients[0]*coefficients[0]+coefficients[1]*coefficients[1]+coefficients[2]*coefficients[2]);
    const double distance=coefficients[0]*center.x+coefficients[1]*center.y+coefficients[2]*center.z+coefficients[3];
    if(!std::isfinite(distance) || distance< -radius*normal) return false;
    }
  return true;
  }

Bench::PerfSample ragdollCoverageSample(World& world) {
  Bench::PerfSample sample;
  sample.physicalBeforeImpulse=bench.perfPhysicalBeforeImpulse;
  sample.sleepingBeforeImpulse=bench.perfSleepingBeforeImpulse;
  sample.actors=bench.crew.size();
  const auto* camera=Gothic::inst().camera();
  const auto vp=camera!=nullptr ? camera->viewProj() : Matrix4x4::mkIdentity();
  for(size_t actor=0;actor<bench.crew.size();++actor) {
    const auto* npc=bench.crew[actor];
    if(npc->isDead()) ++sample.corpses;
    const auto* body=npc->ragdollBody();
    if(body!=nullptr) {
      ++sample.physical; sample.parts+=body->partCount();
      if(body->isSleeping()) ++sample.sleeping;
      }
    const auto center=npc->centerPosition();
    float x=center.x,y=center.y,z=center.z,w=1;
    vp.project(x,y,z,w);
    if(camera!=nullptr && w>0 && std::abs(x)<=w && std::abs(y)<=w && z>=0 && z<=w)
      ++sample.centersInView;
    if(bench.ragdollArenaCamera && (ragdollPerf ? arenaCheapVisible(*npc,actor) : arenaMeshVisible(world,*npc))) ++sample.visibleMeshes;
    }
  return sample;
  }

void sampleRagdollPerformance(World& world) {
  FrameProfile::sample(bench.recName,bench.recFrame);
  auto sample=ragdollCoverageSample(world);
  sample.segment=bench.recName; sample.frame=bench.recFrame; sample.wallMs=bench.perfWallMs;
  const auto stats=world.physic()->physicsStats();
  sample.stepMs=stats.stepMilliseconds; sample.worldBodies=stats.bodies; sample.rays=stats.rays;
  bench.ragdollPerformance.push_back(std::move(sample));
  }

bool arenaModeMatches(const Bench::PerfSample& sample, std::string_view mode) {
  const size_t physical=mode=="disabled" ? 0 : 20;
  const size_t sleeping=mode=="sleeping" ? 20 : 0;
  return sample.actors==20 && sample.corpses==20 && sample.centersInView==20 && sample.visibleMeshes==20 &&
         sample.physical==physical && sample.physicalBeforeImpulse==physical && sample.sleeping==sleeping &&
         sample.sleepingBeforeImpulse==sleeping && sample.parts==(physical==0 ? 0 : 480);
  }

void arenaModelProof(World& world, const Npc& npc, size_t frame) {
  const auto* skeleton=npc.visualSkeleton();
  if(skeleton==nullptr) return;
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low{infinity,infinity,infinity},high{-infinity,-infinity,-infinity};
  auto add=[&](const Vec3& point) {
    low.x=std::min(low.x,point.x); low.y=std::min(low.y,point.y); low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x); high.y=std::max(high.y,point.y); high.z=std::max(high.z,point.z);
    };
  const auto bounds=npc.bounds(); const auto object=npc.transform();
  for(int corner=0;corner<8;++corner) {
    Vec3 p{bounds.bbox[corner&1].x,bounds.bbox[(corner>>1)&1].y,bounds.bbox[(corner>>2)&1].z};
    object.project(p); add(p);
    }
  for(const auto& bone:skeleton->nodes) {
    const auto p=npc.mapBone(bone.name); add(p-Vec3(50,50,50)); add(p+Vec3(50,50,50));
    }
  Vec3 projectedLow{infinity,infinity,infinity},projectedHigh{-infinity,-infinity,-infinity};
  bool projected=true;
  const auto viewProjection=Gothic::inst().camera()->viewProj();
  for(int corner=0;corner<8;++corner) {
    float x=(corner&1) ? high.x : low.x,y=(corner&2) ? high.y : low.y,z=(corner&4) ? high.z : low.z,w=1;
    viewProjection.project(x,y,z,w);
    if(w<=0) { projected=false; continue; }
    x/=w; y/=w; z/=w;
    projected=projected && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
              std::abs(x)<=.90f && std::abs(y)<=.90f && z>=0 && z<=1;
    projectedLow.x=std::min(projectedLow.x,x); projectedLow.y=std::min(projectedLow.y,y); projectedLow.z=std::min(projectedLow.z,z);
    projectedHigh.x=std::max(projectedHigh.x,x); projectedHigh.y=std::max(projectedHigh.y,y); projectedHigh.z=std::max(projectedHigh.z,z);
    }
  if(!projected) bench.ragdollCoverageFailed=true;
  const auto iterator=std::find(bench.crew.begin(),bench.crew.end(),&npc);
  const bool cheap=iterator!=bench.crew.end() && arenaCheapVisible(npc,size_t(iterator-bench.crew.begin()));
  const float radius=iterator!=bench.crew.end() ? bench.ragdollArenaRadii[size_t(iterator-bench.crew.begin())] : 0;
  bool contains=true;
  for(int corner=0;corner<8;++corner) {
    const Vec3 point{(corner&1) ? high.x : low.x,(corner&2) ? high.y : low.y,(corner&4) ? high.z : low.z};
    contains=contains && (point-npc.position()).length()<=radius;
    }
  if(!cheap || !contains) bench.ragdollCoverageFailed=true;
  event("ragdoll_coverage_model","\"coverage_frame\":"+std::to_string(frame)+",\"actor_id\":"+
        std::to_string(world.npcId(&npc))+",\"model_bound_radius_cm\":"+std::to_string(bounds.rConservative)+
        ",\"bone_count\":"+std::to_string(skeleton->nodes.size())+",\"world_min_cm\":"+ragdollVector(low)+
        ",\"world_max_cm\":"+ragdollVector(high)+",\"ndc_min\":"+ragdollVector(projectedLow)+
        ",\"ndc_max\":"+ragdollVector(projectedHigh)+",\"projected_with_margin\":"+(projected ? "true" : "false")+
        ",\"body_drawable\":"+(TestBench::ragdollBodyDrawable(npc) ? "true" : "false")+
        ",\"proxy_radius_cm\":"+std::to_string(radius)+",\"actor_origin_cm\":"+ragdollVector(npc.position())+
        ",\"proxy_contains_envelope\":"+(contains ? "true" : "false")+
        ",\"cheap_proxy_visible\":"+(cheap ? "true" : "false")+
        ",\"visible\":"+(arenaMeshVisible(world,npc) ? "true" : "false"));
  }

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


const char* questReason(uint8_t hazards) {
  return hazards==3 ? "drop+water" : hazards==2 ? "water" : hazards==1 ? "drop" : "none";
  }

bool chooseQuestSite(World& world, bool water, Bench::QuestSite& site,
                     const std::function<bool(Bench::QuestSite&)>& accept={}) {
  if(site.ready && (!accept || accept(site))) return true;
  site.ready=false;
  std::vector<const WayPoint*> points;
  if(!water)
    if(const auto* point=world.findPoint("NW_XARDAS_TOWER_VALLEY_02",false)) points.push_back(point);
  world.forEachWayPoint([&](const WayPoint& point) {
    const auto& n=point.name;
    const bool wanted=water ? n.find("HABOUR")!=std::string::npos || n.find("HARBOR")!=std::string::npos ||
      n.find("HARBOUR")!=std::string::npos || n.find("WATER")!=std::string::npos || n.find("LAKE")!=std::string::npos :
      n.find("XARDAS_TOWER")!=std::string::npos;
    if(wanted && !point.underWater) points.push_back(&point);
    });
  if(points.size()>1) std::sort(points.begin()+(!water ? 1 : 0),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  if(points.size()>100) points.resize(100);
  auto* physics=world.physic();
  for(const auto* point:points) {
    const auto floor=physics->landRay(point->position()+Vec3(0,150,0),500);
    if(!floor.hasCol || floor.n.y<.9f) continue;
    const auto dry=floor.v;
    const auto wet=physics->waterRay(dry,15);
    if(wet.hasCol) continue;
    bool support=true;
    for(float x:{-25.f,25.f}) for(float z:{-25.f,25.f}) {
      const auto corner=physics->landRay(dry+Vec3(x,40,z),100);
      support=support && corner.hasCol && corner.n.y>.85f && std::abs(corner.v.y-dry.y)<15;
      }
    if(!support) continue;
    for(int a=0;a<24;++a) {
      const float angle=float(a)*float(M_PI)/12.f;
      const Vec3 direction(std::cos(angle),0,std::sin(angle));
      for(float distance:{200.f,350.f,500.f,700.f}) {
        const auto edge=dry+direction*distance;
        const auto bottom=physics->landRay(edge+Vec3(0,100,0),6000);
        if(!bottom.hasCol) continue;
        const auto liquid=physics->waterRay(bottom.v+Vec3(0,5,0),0);
        if(water ? !liquid.hasCol || liquid.wdepth-bottom.v.y<150 || liquid.wdepth>dry.y+80 : dry.y-bottom.v.y<1200)
          continue;
        if(physics->ray(dry+Vec3(0,110,0),edge+Vec3(0,110,0)).hasCol) continue;
        site={true,dry,direction,water ? liquid.wdepth : 0,point->name};
        if(accept && !accept(site)) {site.ready=false;continue;}
        return true;
        }
      }
    }
  return false;
  }

struct LivingCounts {
  size_t models=0,alive=0,kinematic=0,parts=0,dynamic=0,dynamicParts=0,duplicates=0;
  };

LivingCounts livingCounts() {
  LivingCounts result;
  for(const auto* npc:bench.crew) {
    if(TestBench::ragdollBodyDrawable(*npc)) ++result.models;
    if(!npc->isDown()) ++result.alive;
    const auto* living=npc->livingBody();const auto* dynamic=npc->ragdollBody();
    if(living) {if(living->isKinematic()) ++result.kinematic;result.parts+=living->partCount();}
    if(dynamic) {++result.dynamic;result.dynamicParts+=dynamic->partCount();}
    if(living && dynamic) ++result.duplicates;
    }
  return result;
  }

std::string livingCountFields(const LivingCounts& s, World& world) {
  return "\"models\":"+std::to_string(s.models)+",\"alive\":"+std::to_string(s.alive)+
    ",\"kinematic\":"+std::to_string(s.kinematic)+",\"parts\":"+std::to_string(s.parts)+
    ",\"dynamic\":"+std::to_string(s.dynamic)+",\"dynamic_parts\":"+std::to_string(s.dynamicParts)+
    ",\"duplicates\":"+std::to_string(s.duplicates)+",\"world_bodies\":"+std::to_string(world.physic()->physicsStats().bodies);
  }

void livingFields(World& world) {
  const auto s=livingCounts();const auto stats=world.physic()->physicsStats();
  std::fprintf(bench.csv,",%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.6f",s.models,s.alive,s.kinematic,s.parts,
    s.dynamic,s.dynamicParts,s.duplicates,stats.bodies,double(stats.stepMilliseconds));
  std::string actors="[";
  for(size_t i=0;i<bench.crew.size();++i) {
    const auto* npc=bench.crew[i];if(i) actors+=',';
    actors+="{\"index\":"+std::to_string(i)+",\"position_cm\":"+ragdollVector(npc->position())+
      ",\"animations\":"+jsonStr(npc->animNames());
    if(const auto* body=npc->livingBody()) {
      const auto pelvis=body->pelvisTransform();
      actors+=",\"native_pelvis_cm\":"+ragdollVector({pelvis.at(3,0),pelvis.at(3,1),pelvis.at(3,2)});
      }
    actors+='}';
    }
  event("living_frame",livingCountFields(s,world)+",\"phase\":"+jsonStr(bench.livingPhase)+",\"actors\":"+actors+"]");
  }

size_t arrowBone(const Skeleton& skeleton, std::string_view token) {
  const auto compact=[](std::string_view text) {
    std::string result;
    for(const char value:text) {
      const auto c=static_cast<unsigned char>(value);
      if(std::isalnum(c)) result+=char(std::toupper(c));
      }
    return result;
    };
  const auto wanted=compact(token);
  for(size_t i=0;i<skeleton.nodes.size();++i)
    if(compact(skeleton.nodes[i].name).find(wanted)!=std::string::npos) return i;
  return size_t(-1);
  }

bool arrowAimPoint(Npc& npc, Vec3& point) {
  const auto* skeleton=npc.visualSkeleton();
  if(skeleton==nullptr || bench.arrowAim.empty()) return false;
  if(bench.arrowAim=="world") {point=bench.stickingWorldPoint;return true;}
  if(bench.arrowAim=="corpse") {
    if(const auto* body=npc.ragdollBody()) {
      const auto state=body->state();
      const auto wanted=arrowBone(*skeleton,"SPINE2");
      for(const auto& part:state.parts)
        if(part.bone==wanted) {point=part.centerOfMassMeters*100.f;return true;}
      }
    return false;
    }
  const bool leg=bench.arrowAim=="leg";
  const bool chest=bench.arrowAim=="chest";
  const auto first=arrowBone(*skeleton,chest ? "SPINE1" : leg ? "L CALF" : "L UPPERARM");
  auto second=arrowBone(*skeleton,chest ? "SPINE2" : leg ? "L FOOT" : "L FOREARM");
  if(chest && second==size_t(-1)) second=arrowBone(*skeleton,"NECK");
  if(first==size_t(-1) || second==size_t(-1)) return false;
  const auto& a=npc.animationBone(first);const auto& b=npc.animationBone(second);
  point=Vec3(a.at(3,0)+b.at(3,0),a.at(3,1)+b.at(3,1),a.at(3,2)+b.at(3,2))*.5f;
  return true;
  }

size_t npcArrowPins(World& world) {
  size_t count=0;
  for(uint32_t i=0;i<world.npcCount();++i)
    if(const auto* npc=world.npcById(i)) count+=npc->stuckArrowCount();
  return count;
  }

void arenaAbort(std::string_view reason);
bool chooseRagdollArena(World& world,Npc& player);

void stickingFields(World& world) {
  if(!bench.stickingTrace || world.tickCount()==bench.stickingSampleTick) return;
  bench.stickingSampleTick=world.tickCount();
  const auto* npc=bench.arrowNpc;
  const auto* living=npc ? npc->livingBody() : nullptr;
  const auto* corpse=npc ? npc->ragdollBody() : nullptr;
  const auto* body=living ? living : corpse;
  const auto physics=world.physic()->physicsStats();
  std::string pins="[",worldPins="[";
  if(npc) {
    bench.arrowSeenHits=npc->arrowPhysicsProbe().hits;
    for(size_t i=0;i<npc->stuckArrowCount();++i) {
      Npc::ArrowPinProbe pin;
      if(!npc->stuckArrowProbe(i,pin)) {arenaAbort("stuck NPC pin has no actual draw node");return;}
      if(i) pins+=',';
      pins+="{\"index\":"+std::to_string(i)+",\"bone\":"+std::to_string(pin.bone)+
        ",\"local\":"+ragdollMatrix(pin.local)+",\"world\":"+ragdollMatrix(pin.world)+
        ",\"expected\":"+ragdollMatrix(npc->renderedBone(pin.bone)*pin.local)+
        ",\"render_bone\":"+ragdollMatrix(npc->renderedBone(pin.bone))+
        ",\"animation_bone\":"+ragdollMatrix(npc->animationBone(pin.bone))+"}";
      }
    }
  for(size_t i=0;i<world.stuckArrowCount();++i) {
    Matrix4x4 actual;
    if(!world.stuckArrowTransform(i,actual)) {arenaAbort("stuck world pin has no actual draw node");return;}
    if(i) worldPins+=',';
    worldPins+="{\"index\":"+std::to_string(i)+",\"world\":"+ragdollMatrix(actual)+"}";
    }
  event("arrow_sticking_frame","\"case\":"+jsonStr(bench.stickingCase)+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"feature\":"+(Gothic::settingsGetI("GAME","arrow_sticking")?"true":"false")+
    ",\"npc_present\":"+(npc?"true":"false")+
    ",\"root_cm\":"+ragdollVector(npc?npc->position():Vec3())+
    ",\"yaw_deg\":"+ragdollNumber(npc?npc->rotation():0.f)+
    ",\"ko\":"+(npc && npc->isUnconscious()?"true":"false")+
    ",\"dead\":"+(npc && npc->isDead()?"true":"false")+
    ",\"recovering\":"+(npc && npc->isRagdollBlending()?"true":"false")+
    ",\"collision_overlap\":"+(npc && npc->hasCollision()?"true":"false")+
    ",\"living_parts\":"+std::to_string(living?living->partCount():0)+
    ",\"corpse_parts\":"+std::to_string(corpse?corpse->partCount():0)+
    ",\"weight\":"+ragdollNumber(body?body->reactionWeight():0.f)+
    ",\"world_bodies\":"+std::to_string(physics.bodies)+",\"physics_ms\":"+ragdollNumber(physics.stepMilliseconds)+
    ",\"npc_pin_count\":"+std::to_string(npc?npc->stuckArrowCount():0)+
    ",\"all_npc_pin_count\":"+std::to_string(npcArrowPins(world))+
    ",\"world_pin_count\":"+std::to_string(world.stuckArrowCount())+
    ",\"npc_pins\":"+pins+"],\"world_pins\":"+worldPins+"]");
  }

void spellFields(World& world) {
  if(!bench.spellTrace || bench.spellSampleTick==world.tickCount()) return;
  bench.spellSampleTick=world.tickCount();
  std::string actors="[",items="[";
  for(size_t i=0;i<bench.spellActors.size();++i) {
    auto& actor=bench.spellActors[i];auto& npc=*actor.npc;
    const auto* living=npc.livingBody();const auto* corpse=npc.ragdollBody();
    const auto* body=living?living:corpse;
    std::vector<Matrix4x4> native;
    const bool nativePose=body && body->getPose(native);
    if(i==0 && bench.spellCase.starts_with("ice")) {
      if(npc.spellFrozen()) {npc.setExternalControl(true);bench.spellWasFrozen=true;}
      else if(bench.spellWasFrozen) {npc.setExternalControl(false);bench.spellWasFrozen=false;}
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
  for(size_t i=0;i<bench.spellItems.size();++i) {
    const auto& item=*bench.spellItems[i];if(i) items+=',';
    items+="{\"index\":"+std::to_string(i)+",\"position_cm\":"+ragdollVector(item.position())+
      ",\"mass_kg\":"+ragdollNumber(item.physicsMass())+",\"velocity_cm_s\":"+ragdollVector(item.physicsVelocity())+
      ",\"sleeping\":"+(item.physicsSleeping()?"true":"false")+",\"dynamic\":"+(item.isDynamic()?"true":"false")+"}";
    }
  if(bench.spellGuardTrace && !bench.spellActors.empty()) {
    const auto& npc=*bench.spellActors[0].npc;
    const auto& q=npc.ragdollQuestReturnState();
    const auto returns=npc.ragdollQuestReturnCount();
    event("spell_guard_frame","\"position_cm\":"+ragdollVector(npc.position())+
      ",\"case\":"+jsonStr(bench.spellGuardCase)+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"return_from_cm\":"+ragdollVector(npc.ragdollQuestReturnFrom())+
      ",\"hp\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
      ",\"human\":"+(npc.isHuman()?"true":"false")+",\"hero\":"+(npc.isPlayer()?"true":"false")+
      ",\"immortal\":"+(npc.isImmortal()?"true":"false")+
      ",\"in_air\":"+(npc.isInAir()?"true":"false")+",\"falling\":"+(npc.isFalling()?"true":"false")+
      ",\"standing\":"+(npc.isStanding()?"true":"false")+
      ",\"unconscious\":"+(npc.isUnconscious()?"true":"false")+",\"dead\":"+(npc.isDead()?"true":"false")+
      ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+
      ",\"hazards\":"+std::to_string(q.hazards)+",\"returns\":"+std::to_string(returns));
    if(returns>bench.spellGuardObservedReturns) {
      const auto from=npc.ragdollQuestReturnFrom();
      const auto floor=world.physic()->landRay(from+Vec3(0,200,0),6000);
      const auto water=world.physic()->waterRay(from,200.f);
      event("spell_guard_landing","\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(npc.position())+
        ",\"case\":"+jsonStr(bench.spellGuardCase)+",\"hazards\":"+std::to_string(q.hazards)+
        ",\"has_water\":"+(water.hasCol?"true":"false")+
        ",\"water_y_cm\":"+ragdollNumber(water.wdepth)+
        ",\"water_depth_cm\":"+ragdollNumber(water.wdepth-from.y)+
        ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"returns\":"+std::to_string(returns)+
        ",\"has_floor\":"+(floor.hasCol?"true":"false")+
        ",\"floor_cm\":"+(floor.hasCol?ragdollVector(floor.v):"null")+
        ",\"floor_normal\":"+(floor.hasCol?ragdollVector(floor.n):"null"));
      bench.spellGuardObservedReturns=returns;
      }
    }
  const auto stats=world.physic()->physicsStats();
  event("spell_physics_frame","\"case\":"+jsonStr(bench.spellCase)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"feature\":"+(Gothic::settingsGetI("GAME","spell_physics")?"true":"false")+
    ",\"fields\":"+std::to_string(world.spellFieldCount())+",\"world_bodies\":"+std::to_string(stats.bodies)+
    ",\"physics_ms\":"+ragdollNumber(stats.stepMilliseconds)+",\"actors\":"+actors+"],\"items\":"+items+"]");
  }

void prepareSpellFixture(World& world,Npc& player,std::string_view fixture) {
  bench.spellTrace=false;bench.spellReleaseAt=0;bench.spellWaitFrozen=-1;bench.spellWasFrozen=false;bench.spellEquipPending=0;
  player.setTarget(nullptr);player.closeWeapon(true);
  const auto before=world.physic()->physicsStats().bodies;
  size_t expected=0;
  for(const auto& actor:bench.spellActors) expected+=(actor.npc->livingBody()?actor.npc->livingBody()->partCount():0)+(actor.npc->ragdollBody()?actor.npc->ragdollBody()->partCount():0);
  for(const auto* item:bench.spellItems) expected+=size_t(item->isDynamic());
  for(auto& actor:bench.spellActors) world.removeNpc(*actor.npc);
  for(auto* item:bench.spellItems) world.removeItem(*item);
  if(expected) event("spell_physics_cleanup","\"before\":"+std::to_string(before)+",\"after\":"+
    std::to_string(world.physic()->physicsStats().bodies)+",\"expected_parts\":"+std::to_string(expected));
  bench.spellActors.clear();bench.spellItems.clear();bench.crew.clear();bench.subject=nullptr;
  if(!bench.spellArenaReady) {
    if(!chooseRagdollArena(world,player)) {arenaAbort("spell fixture needs supported open arena");return;}
    bench.spellArenaReady=true;
    std::vector<Npc*> nearby;
    for(uint32_t i=0;i<world.npcCount();++i)
      if(auto* npc=world.npcById(i);npc && npc!=&player && (npc->position()-player.position()).length()<2500) nearby.push_back(npc);
    for(auto* npc:nearby) world.removeNpc(*npc);
    }
  const auto center=bench.ragdollArenaTarget-Vec3(0,80,300);
  auto hero=center-Vec3(0,0,700);
  if(const auto floor=world.physic()->landRay(hero+Vec3(0,300,0),1200);floor.hasCol) hero.y=floor.v.y;
  player.setPosition(hero);player.setDirection({0,0,1});player.updateTransform();
  const auto add=[&](std::string_view symbol,std::string role,Vec3 point,bool dead=false) {
    if(const auto floor=world.physic()->landRay(point+Vec3(0,300,0),1200);floor.hasCol) point.y=floor.v.y;
    auto* npc=world.addNpc(world.script().findSymbolIndex(symbol),point);
    if(!npc) {arenaAbort("spell actor insertion failed");return;}
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);npc->setDirection(hero-point);npc->updateTransform();
    npc->handle().attribute[ATR_HITPOINTS]=50000;npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
    if(dead) {npc->setRagdollDown(true);npc->prepareRagdoll();}
    bench.spellActors.push_back({npc,std::move(role)});bench.crew.push_back(npc);
    };
  add("BAU_904_BAUER","human",center);
  if(fixture=="wind" || fixture=="field")
    add("BDT_1013_BANDIT_L","companion",center+Vec3(-180,0,0));
  if(fixture=="wind") {
    add("TROLL","troll",center+Vec3(450,0,0));
    add("BAU_904_BAUER","behind",hero-Vec3(0,0,300));
    add("BAU_904_BAUER","outside_cone",hero+Vec3(550,0,0));
    }
  else if(fixture!="ice") {
    add("BAU_904_BAUER","corpse",center+Vec3(120,0,40),true);
    for(size_t i=0;i<3;++i) {
      auto at=center+Vec3(float(i)*110.f-110.f,0,-100);
      if(const auto floor=world.physic()->landRay(at+Vec3(0,300,0),900);floor.hasCol) at.y=floor.v.y;
      const auto symbol=world.script().findSymbolIndex(i==0?"ITMI_LUTE":(i==1?"ITMI_PAN":"ITMW_1H_MACE_L_01"));
      if(symbol==size_t(-1)) {arenaAbort("spell loose prop instance missing");return;}
      auto* item=world.addItem(symbol,at);
      if(item) {
        if(const auto* bounds=item->bBox()) item->setPosition(at.x,at.y-bounds[0].y+1.f,at.z);
        bench.spellItems.push_back(item);
        }
      }
    }
  if(fixture=="ice" || fixture=="airborne") bench.spellActors[0].npc->setExternalControl(false);
  bench.subject=bench.spellActors[0].npc;
  if(bench.spellRunes.empty()) {
    for(size_t i=0;i<world.script().symbolsCount();++i) {
      const auto* symbol=world.script().findSymbol(i);
      if(!symbol || symbol->type()!=zenkit::DaedalusDataType::INSTANCE) continue;
      auto name=symbol->name();for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_")) continue;
      auto candidate=std::make_unique<Item>(world,i,Item::T_Inventory);
      const auto id=candidate->spellId();
      if((id!=23 && id!=26 && id!=30 && id!=33 && id!=71) || bench.spellRunes.contains(id)) continue;
      bench.spellRunes[id]=i;player.addItem(i,candidate->isRune()?1:5);
      event("spell_physics_rune","\"id\":"+std::to_string(id)+",\"symbol\":"+jsonStr(name)+
        ",\"class_id\":"+std::to_string(i)+",\"projectile\":"+(candidate->isSpellShoot()?"true":"false"));
      }
    }
  player.handle().attribute[ATR_MANA]=50000;player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  const auto target=center+Vec3(0,90,0);
  bench.cam=C_Hold;lookFrom(target+Vec3(600,280,-300),target);
  event("spell_physics_setup","\"fixture\":"+jsonStr(fixture)+",\"default_enabled\":"+
    std::string(Gothic::settingsGetI("GAME","spell_physics")?"true":"false"));
  }

Vec3 movablePosition(const Matrix4x4& matrix) {
  return {matrix.at(3,0),matrix.at(3,1),matrix.at(3,2)};
  }

std::string movableRows(World& world,bool allMap=false) {
  std::string result="[";
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(allMap ? body.zenId==uint32_t(-1) :
       std::find(bench.movableIds.begin(),bench.movableIds.end(),body.id)==bench.movableIds.end()) continue;
    if(result.size()>1) result+=',';
    result+="{\"id\":"+std::to_string(body.id)+",\"visual\":"+jsonStr(body.visual)+
      ",\"zen_id\":"+std::to_string(body.zenId)+",\"origin\":"+ragdollMatrix(body.origin)+
      ",\"class\":"+jsonStr(body.className)+",\"blocks_walking\":"+(body.blocksWalking?"true":"false")+
      ",\"legacy_collision\":"+(body.legacyCollision?"true":"false")+
      ",\"from_map\":"+((allMap || bench.movableMapIds.contains(body.id))?"true":"false")+
      ",\"mass_kg\":"+ragdollNumber(body.mass)+",\"enabled\":"+(body.enabled?"true":"false")+
      ",\"sleeping\":"+(body.sleeping?"true":"false")+",\"npc_contact\":"+(body.touchedByNpc?"true":"false")+
      ",\"position_cm\":"+ragdollVector(movablePosition(body.transform))+
      ",\"transform\":"+ragdollMatrix(body.transform)+",\"velocity_cm_s\":"+ragdollVector(body.velocity)+
      ",\"max_displacement_cm\":"+ragdollNumber(bench.movableTravel[body.id])+"}";
    }
  return result+"]";
  }

void movableBegin(World& world,std::string phase) {
  bench.movableReference.clear();bench.movableTravel.clear();bench.movableNpcTouched.clear();
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(std::find(bench.movableIds.begin(),bench.movableIds.end(),body.id)!=bench.movableIds.end()) {
      bench.movableReference[body.id]=movablePosition(body.transform);
      bench.movableTravel[body.id]=0;
      }
  bench.movablePhase=std::move(phase);bench.movablePhaseStart=world.tickCount();bench.movableTrace=true;
  event("movable_begin","\"phase\":"+jsonStr(bench.movablePhase)+",\"t_game_ms\":"+
    std::to_string(world.tickCount())+",\"bodies\":"+movableRows(world));
  }

void movableFields(World& world) {
  if(bench.movableWakeTrace)
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(body.enabled)
        bench.movableWakeTravel[body.id]=std::max(bench.movableWakeTravel[body.id],
          (movablePosition(body.transform)-movablePosition(body.origin)).length());
  if(!bench.movableTrace || bench.movableMeasuring || bench.movableSampleTick==world.tickCount()) return;
  bench.movableSampleTick=world.tickCount();
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    const auto initial=bench.movableReference.find(body.id);
    if(initial==bench.movableReference.end()) continue;
    bench.movableTravel[body.id]=std::max(bench.movableTravel[body.id],(movablePosition(body.transform)-initial->second).length());
    if(body.touchedByNpc) bench.movableNpcTouched.insert(body.id);
    }
  // Every native step is observed, but large JSON is emitted only at checkpoints.
  }

void movableCheckpoint(World& world) {
  movableFields(world);
  std::string touched="[";
  for(const auto id:bench.movableNpcTouched) {if(touched.size()>1)touched+=',';touched+=std::to_string(id);}
  event("movable_checkpoint","\"phase\":"+jsonStr(bench.movablePhase)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-bench.movablePhaseStart)+",\"npc_touched_ids\":"+touched+
    "],\"bodies\":"+movableRows(world)+",\"walker_cm\":"+
    (bench.movableWalker?ragdollVector(bench.movableWalker->position()):"null"));
  bench.movableTrace=false;
  }

void ownMeshWalk(World& world,Npc& player) {
  if(bench.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh requires setup");return;}
  auto start=bench.ownMeshAxis+bench.ownMeshApproach*270.f;
  const auto floor=world.physic()->landRay(start+Vec3(0,200,0),700.f);
  if(!floor.hasCol || floor.n.y<.8f) {arenaAbort("own-mesh approach has no supported ground");return;}
  start.y=floor.v.y;
  player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.clearSpeed();
  player.setPosition(start);player.setDirection(-bench.ownMeshApproach);player.updateTransform();
  bench.ownMeshHeroStart=start;bench.ownMeshHeroHistory.clear();bench.ownMeshWalking=true;bench.ownMeshMaxCrossTrack=0;
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(body.zenId==bench.ownMeshZen)bench.ownMeshBodyStart=movablePosition(body.transform);
  }

void ownMeshSetup(World& world,Npc& player,uint32_t zenId,const Vec3& approach) {
  const auto all=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(all.begin(),all.end(),[zenId](const auto& body){return body.zenId==zenId;});
  if(found==all.end() || !found->visual.starts_with("NW_HARBOUR_BARREL_01") || approach.length()<.9f) {
    arenaAbort("own-mesh requires the authored NW_HARBOUR_BARREL_01 and a horizontal approach");return;
    }
  bench.ownMeshZen=zenId;bench.ownMeshApproach=Vec3::normalize(approach);
  bench.ownMeshAxis=(found->localBounds[0]+found->localBounds[1])*.5f;
  found->origin.project(bench.ownMeshAxis);
  std::string corridor="[";
  for(float distance=270.f;distance>=90.f;distance-=30.f) {
    auto point=bench.ownMeshAxis+bench.ownMeshApproach*distance;
    const auto floor=world.physic()->landRay(point+Vec3(0,200,0),700.f);
    if(!floor.hasCol || floor.n.y<.8f) {arenaAbort("own-mesh straight corridor has no supported ground");return;}
    if(corridor.size()>1)corridor+=',';
    corridor+=ragdollVector(floor.v);
    }
  ownMeshWalk(world,player);bench.ownMeshWalking=false;
  bench.cam=C_Hold;
  const auto camera=bench.ownMeshAxis+Vec3(-.25879f,0,.96593f)*430.f+Vec3(0,160,0);
  lookFrom(camera,bench.ownMeshAxis);
  event("own_mesh_setup","\"zen_id\":"+std::to_string(zenId)+",\"visual\":"+jsonStr(found->visual)+
    ",\"origin\":"+ragdollMatrix(found->origin)+",\"axis_half_height_cm\":"+ragdollVector(bench.ownMeshAxis)+
    ",\"approach_direction\":"+ragdollVector(bench.ownMeshApproach)+
    ",\"approach_floor_cm\":"+corridor+"]"+
    ",\"hero_start_cm\":"+ragdollVector(bench.ownMeshHeroStart)+",\"camera_cm\":"+ragdollVector(camera));
  }

void ownMeshArrow(World& world,Npc& player) {
  if(bench.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh arrow requires setup");return;}
  const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(symbol==size_t(-1)) {arenaAbort("own-mesh arrow instance unavailable");return;}
  const auto target=bench.ownMeshAxis,from=target+bench.ownMeshApproach*270.f;
  const float speed=DynamicWorld::bulletSpeed;
  const auto direction=Vec3::normalize(Archery::launch(from,target,speed)-from);
  Item arrow(world,symbol,Item::T_Inventory);
  auto& bullet=world.shootArrow(arrow,from,direction,speed);
  bullet.setOrigin(&player);bullet.setPrecise(1.f);
  bench.ownMeshArrowHits=0;
  event("own_mesh_arrow","\"zen_id\":"+std::to_string(bench.ownMeshZen)+",\"from_cm\":"+
    ragdollVector(from)+",\"target_cm\":"+ragdollVector(target)+",\"speed_cm_ms\":"+ragdollNumber(speed));
  }

void ownMeshClearance(World& world,Npc& player) {
  if(bench.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh clearance requires setup");return;}
  auto bodies=world.physic()->movableVobContacts(100.f);
  // With MOVABLE_VOBS=0 there is no native dynamic hull to overlap. Keep the
  // same line/camera guards against the original source collision mesh.
  if(!world.physic()->movableVobsEnabled()) {
    for(const auto& source:world.physic()->movableVobSnapshot())if(source.zenId==bench.ownMeshZen) {
      DynamicWorld::MovableVobContacts body;
      body.zenId=source.zenId;body.visual=source.visual;body.disposition=source.disposition;
      body.hullMin=body.hullMax=bench.ownMeshAxis;
      for(unsigned corner=0;corner<8;++corner) {
        Vec3 point(source.localBounds[corner&1].x,source.localBounds[(corner>>1)&1].y,source.localBounds[(corner>>2)&1].z);
        source.origin.project(point);
        body.hullMin=Vec3(std::min(body.hullMin.x,point.x),std::min(body.hullMin.y,point.y),std::min(body.hullMin.z,point.z));
        body.hullMax=Vec3(std::max(body.hullMax.x,point.x),std::max(body.hullMax.y,point.y),std::max(body.hullMax.z,point.z));
        }
      bodies.push_back(std::move(body));break;
      }
    }
  for(const auto& body:bodies) {
    if(body.zenId!=bench.ownMeshZen)continue;
    std::string contacts="[";float gap=100.f;
    for(const auto& hit:body.contacts) {
      const bool floor=hit.category==DynamicWorld::C_Landscape && hit.below;
      if(!floor)gap=std::min(gap,-hit.depth);
      if(contacts.size()>1)contacts+=',';
      contacts+="{\"other\":"+jsonStr(hit.other)+",\"zen_id\":"+std::to_string(hit.otherZenId)+
        ",\"category\":"+std::to_string(hit.category)+",\"floor_support\":"+(floor?"true":"false")+
        ",\"depth_cm\":"+ragdollNumber(hit.depth)+",\"point_cm\":"+ragdollVector(hit.point)+"}";
      }
    event("own_mesh_clearance","\"zen_id\":"+std::to_string(body.zenId)+
      ",\"enabled\":"+(body.enabled?"true":"false")+",\"disposition\":"+jsonStr(body.disposition)+
      ",\"margin_cm\":100,\"minimum_non_floor_gap_cm\":"+(body.enabled?ragdollNumber(gap):"null")+",\"contacts\":"+contacts+"]");
    if(gap<=0.f || (world.physic()->movableVobsEnabled() && !body.enabled)) {
      arenaAbort("own-mesh barrel must retain a positive gap and remain movable when enabled");return;
      }
    const auto extent=(body.hullMax-body.hullMin)*.5f;
    const auto axis=bench.ownMeshAxis;
    const auto* camera=Gothic::inst().camera();bool framed=camera!=nullptr,visible=true;
    Vec3 ndcMin(1e9f,1e9f,1e9f),ndcMax(-1e9f,-1e9f,-1e9f);
    const auto eye=axis+Vec3(-.25879f,0,.96593f)*430.f+Vec3(0,160,0);
    for(unsigned corner=0;corner<8;++corner) {
      const Vec3 point((corner&1)?body.hullMax.x:body.hullMin.x,(corner&2)?body.hullMax.y:body.hullMin.y,
                       (corner&4)?body.hullMax.z:body.hullMin.z);
      float x=point.x,y=point.y,z=point.z,w=1;
      if(camera)camera->viewProj().project(x,y,z,w);
      if(w<=0)framed=false;
      else {
        x/=w;y/=w;z/=w;
        framed=framed && std::abs(x)<.95f && std::abs(y)<.95f && z>=0.f && z<=1.f;
        ndcMin=Vec3(std::min(ndcMin.x,x),std::min(ndcMin.y,y),std::min(ndcMin.z,z));
        ndcMax=Vec3(std::max(ndcMax.x,x),std::max(ndcMax.y,y),std::max(ndcMax.z,z));
        }
      const auto hit=world.physic()->ray(eye,point);
      const auto own=world.physic()->rayMovableVob(eye,point);
      visible=visible && (!hit.hasCol || (own && own->zenId==bench.ownMeshZen));
      }
    event("own_mesh_camera","\"zen_id\":"+std::to_string(body.zenId)+",\"eye_cm\":"+ragdollVector(eye)+
      ",\"framed\":"+(framed?"true":"false")+",\"visible\":"+(visible?"true":"false")+
      ",\"ndc_min\":"+ragdollVector(ndcMin)+",\"ndc_max\":"+ragdollVector(ndcMax));
    if(!framed || !visible) {arenaAbort("own-mesh camera must see the complete barrel from the free side");return;}
    for(unsigned line=0;line<3;++line) {
      const auto direction=line==2?-bench.ownMeshApproach:bench.ownMeshApproach;
      float edge=std::numeric_limits<float>::infinity();
      if(std::abs(direction.x)>.001f)edge=std::min(edge,extent.x/std::abs(direction.x));
      if(std::abs(direction.z)>.001f)edge=std::min(edge,extent.z/std::abs(direction.z));
      for(float y:{axis.y,body.hullMin.y+40.f,body.hullMin.y+110.f,body.hullMin.y+170.f}) {
        if(line==1 && y!=axis.y)continue;
        auto from=axis+direction*(edge+2.f);from.y=y;
        const auto to=from+direction*200.f;
        const auto hit=world.physic()->navigationRay(from,to,&player);
        const auto visual=world.physic()->ray(from,to);
        event("own_mesh_line","\"zen_id\":"+std::to_string(body.zenId)+",\"line\":"+
          jsonStr(line==0?"hero_approach":line==1?"arrow":"push")+",\"from_cm\":"+ragdollVector(from)+
          ",\"to_cm\":"+ragdollVector(to)+",\"length_cm\":200,\"physical_hit\":"+(hit.hasCol?"true":"false")+
          ",\"physical_hit_distance_cm\":"+ragdollNumber(hit.hasCol?(hit.point-from).length():200.f)+
          ",\"physical_kind\":"+std::to_string(unsigned(hit.kind))+",\"visual_hit\":"+(visual.hasCol?"true":"false")+
          ",\"visual_hit_distance_cm\":"+ragdollNumber(visual.hasCol?(visual.v-from).length():200.f));
        if(hit.hasCol || visual.hasCol) {arenaAbort("own-mesh approach, arrow and push require clear 200 cm lines");return;}
        }
      }
    return;
    }
  arenaAbort("own-mesh clearance body disappeared");
  }

void ownMeshFields(World& world,Npc& player) {
  if(!bench.ownMeshWalking)return;
  const Vec3 side(-bench.ownMeshApproach.z,0,bench.ownMeshApproach.x);
  const float crossTrack=Vec3::dotProduct(player.position()-bench.ownMeshHeroStart,side);
  bench.ownMeshMaxCrossTrack=std::max(bench.ownMeshMaxCrossTrack,std::abs(crossTrack));
  // Steer the held forward input back to the straight approach line. This
  // prevents a slanted barrel facet from turning the test into a walk around
  // it; all positions and velocities still come from the native controller.
  player.setDirection(Vec3::normalize(-bench.ownMeshApproach-side*(crossTrack*.1f)));
  bench.ownMeshHeroHistory.emplace_back(world.tickCount(),player.position());
  while(bench.ownMeshHeroHistory.size()>1 &&
        bench.ownMeshHeroHistory[1].first+500<world.tickCount())bench.ownMeshHeroHistory.pop_front();
  }

void ownMeshCheckpoint(World& world,Npc& player) {
  if(bench.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh checkpoint requires setup");return;}
  ownMeshFields(world,player);
  const auto hero=player.position(),delta=hero-bench.ownMeshAxis;
  const auto& history=bench.ownMeshHeroHistory;
  const uint64_t elapsed=history.empty()?0:world.tickCount()-history.front().first;
  const auto movement=history.empty()?Vec3():hero-history.front().second;
  const float speed=elapsed?Vec3(movement.x,0,movement.z).length()*1000.f/float(elapsed):0.f;
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.zenId!=bench.ownMeshZen)continue;
    const auto position=movablePosition(body.transform);
    event("own_mesh_checkpoint","\"zen_id\":"+std::to_string(body.zenId)+
      ",\"enabled\":"+(body.enabled?"true":"false")+",\"disposition\":"+jsonStr(body.disposition)+
      ",\"hero_start_cm\":"+ragdollVector(bench.ownMeshHeroStart)+",\"hero_position_cm\":"+ragdollVector(hero)+
      ",\"hero_axis_distance_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+
      ",\"max_cross_track_cm\":"+ragdollNumber(bench.ownMeshMaxCrossTrack)+
      ",\"final_sample_ms\":"+std::to_string(elapsed)+",\"final_speed_cm_s\":"+ragdollNumber(speed)+
      ",\"body_start_cm\":"+ragdollVector(bench.ownMeshBodyStart)+",\"body_position_cm\":"+ragdollVector(position)+
      ",\"body_displacement_cm\":"+ragdollNumber((position-bench.ownMeshBodyStart).length())+
      ",\"body_transform\":"+ragdollMatrix(body.transform)+",\"npc_contact\":"+(body.touchedByNpc?"true":"false")+
      ",\"mass_kg\":"+ragdollNumber(body.mass)+",\"hull_volume_m3\":"+ragdollNumber(body.hullVolume)+
      ",\"material_group\":"+std::to_string(unsigned(body.material))+",\"density_kg_m3\":"+ragdollNumber(body.density)+
      ",\"mass_multiplier\":"+ragdollNumber(body.massMultiplier)+",\"arrow_hits\":"+std::to_string(bench.ownMeshArrowHits));
    bench.ownMeshWalking=false;return;
    }
  arenaAbort("own-mesh authored body disappeared");
  }

void prepareMovableFixture(World& world,Npc& player) {
  if(bench.movableReady) {arenaAbort("movable tavern fixture already exists");return;}
  Npc* host=nullptr;
  for(uint32_t i=0;i<world.npcCount();++i) {
    auto* npc=world.npcById(i);if(!npc)continue;
    auto name=symName(world,npc);
    for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(name.find("CORAGON")!=std::string::npos || name.find("KORAGON")!=std::string::npos) {host=npc;break;}
    }
  if(!host) {arenaAbort("movable fixture requires Coragon in the loaded Khorinis world");return;}
  bench.noiseHost=host;
  const auto all=world.physic()->movableVobSnapshot();
  // Use the same authored geometry subset in ON and OFF fixtures. Rejected
  // map props remain in the real world, outside the dynamic test catalog.
  std::unordered_set<uint32_t> allowedMapVobs;
  for(const auto& body:world.physic()->movableVobContacts(1.f))
    if(body.disposition=="ok" || body.disposition=="item")allowedMapVobs.insert(body.zenId);
  std::map<std::string,size_t> noncollidingClasses,mapClasses;
  for(const auto& body:all)
    if(!body.legacyCollision)++noncollidingClasses[body.className];
  const auto classCounts=[](const std::map<std::string,size_t>& counts) {
    std::string result="{";
    for(const auto& [name,count]:counts) {
      if(result.size()>1)result+=',';
      result+=jsonStr(name)+":"+std::to_string(count);
      }
    return result+"}";
    };
  uintptr_t tableId=0;std::string tableVisual,utensilVisual="ITFO_BEER.3DS",barrelVisual="NW_HARBOUR_BARREL_01.3DS";
  if(!Resources::loadMesh(utensilVisual))utensilVisual.clear();
  if(!Resources::loadMesh(barrelVisual))barrelVisual.clear();
  Matrix4x4 tableTransform;float nearest=1400.f*1400.f;
  for(const auto& body:all) {
    if(!allowedMapVobs.contains(body.zenId))continue;
    if(body.className=="utensil" && utensilVisual.empty()) utensilVisual=body.visual;
    if(body.className=="barrel" && barrelVisual.empty()) barrelVisual=body.visual;
    if(body.className!="table")continue;
    const auto distance=(movablePosition(body.transform)-host->position()).quadLength();
    if(distance<nearest) {nearest=distance;tableId=body.id;tableVisual=body.visual;tableTransform=body.transform;}
    }
  if(!tableId || utensilVisual.empty()) {arenaAbort("Coragon tavern needs a real table and an available utensil mesh");return;}
  const auto* tableMesh=Resources::loadMesh(tableVisual);
  const auto* dishMesh=Resources::loadMesh(utensilVisual);
  if(!tableMesh || !dishMesh) {arenaAbort("movable fixture real mesh unavailable");return;}
  const auto* tableBounds=tableMesh->bbox();const auto* dishBounds=dishMesh->bbox();
  auto tableCenter=(tableBounds[0]+tableBounds[1])*.5f;tableTransform.project(tableCenter);
  auto tableTop=tableCenter;tableTop.y=-std::numeric_limits<float>::infinity();
  for(unsigned i=0;i<8;++i) {
    Vec3 point((i&1)?tableBounds[1].x:tableBounds[0].x,(i&2)?tableBounds[1].y:tableBounds[0].y,(i&4)?tableBounds[1].z:tableBounds[0].z);
    tableTransform.project(point);tableTop.y=std::max(tableTop.y,point.y);
    }
  bench.movableTable=tableId;bench.movableCenter=tableCenter;
  for(const auto& body:all) {
    if(!allowedMapVobs.contains(body.zenId))continue;
    const auto delta=movablePosition(body.transform)-tableCenter;
    if(delta.x*delta.x+delta.z*delta.z<900.f*900.f && std::abs(delta.y)<350) {
      bench.movableIds.push_back(body.id);bench.movableMapIds.insert(body.id);++mapClasses[body.className];
      }
    }
  const auto add=[&](const std::string& visual,const Vec3& support) {
    const auto* mesh=Resources::loadMesh(visual);if(!mesh)return uintptr_t(0);
    const auto* bounds=mesh->bbox();
    zenkit::VirtualObject data;data.type=zenkit::VirtualObjectType::zCVob;data.id=uint32_t(-1);
    data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
    data.position={support.x-(bounds[0].x+bounds[1].x)*.5f,support.y-bounds[0].y+.2f,support.z-(bounds[0].z+bounds[1].z)*.5f};
    data.visual=std::make_shared<zenkit::VisualMesh>();data.visual->type=zenkit::VisualType::MESH;data.visual->name=visual;
    const auto before=world.physic()->movableVobSnapshot();
    auto fixture=std::make_unique<Bench::MovableFixture>();
    fixture->visual.setVisual(data,world,false,false);
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(data.position.x,data.position.y,data.position.z);
    fixture->visual.setObjMatrix(matrix);
    const auto* rule=MovableVobs::classifyVisual(visual);
    const bool testMug=visual==utensilVisual;
    if(!testMug && rule==nullptr)return uintptr_t(0);
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,testMug?.12f:rule->massMultiplier,
                                              testMug?false:rule->blocksWalking,testMug?"utensil":rule->className,true);
    if(fixture->body.isEmpty())return uintptr_t(0);
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& matrix) {view->setObjMatrix(matrix);});
    bench.movableFixtures.emplace_back(std::move(fixture));
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& previous){return previous.id==body.id;})) {
        bench.movableIds.push_back(body.id);return body.id;
        }
    return uintptr_t(0);
    };
  // Same real mesh instances in both modes, supported by the actual tavern tabletop.
  const float spacing=std::max(22.f,std::max(dishBounds[1].x-dishBounds[0].x,dishBounds[1].z-dishBounds[0].z)+3.f);
  for(unsigned i=0;i<6;++i)
    if(!add(utensilVisual,tableTop+Vec3((float(i%3)-1)*spacing,0,(float(i/3)-.5f)*spacing))) {
      arenaAbort("movable tabletop utensil insertion failed");return;
      }
  Vec3 laneStart,laneEnd;bool laneFound=false;
  for(unsigned i=0;i<16 && !laneFound;++i) {
    const float angle=float(i)*float(M_PI)/8.f;
    const Vec3 direction(std::cos(angle),0,std::sin(angle)),side(-direction.z,0,direction.x);
    const auto center=Vec3(tableCenter.x,host->position().y,tableCenter.z)+direction*300.f;
    laneStart=center-side*150.f;laneEnd=center+side*150.f;
    bool supported=true;
    for(auto* point:{&laneStart,&laneEnd}) {
      const auto floor=world.physic()->landRay(*point+Vec3(0,80,0),180);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-host->position().y)>60) {supported=false;break;}
      point->y=floor.v.y;
      }
    if(supported && !world.physic()->ray(laneStart+Vec3(0,50,0),laneEnd+Vec3(0,50,0)).hasCol &&
       !world.physic()->ray(laneStart+Vec3(0,120,0),laneEnd+Vec3(0,120,0)).hasCol) laneFound=true;
    }
  if(!laneFound) {arenaAbort("Coragon tavern lacks a supported 3m walking lane");return;}
  bench.movableWalkStart=laneStart;bench.movableWalkEnd=laneEnd;
  bench.movableDish=add(utensilVisual,(laneStart+laneEnd)*.5f);
  if(!bench.movableDish) {arenaAbort("movable floor utensil insertion failed");return;}
  bool hasBarrel=false;
  for(const auto& body:all)
    if(body.className=="barrel" && allowedMapVobs.contains(body.zenId) && bench.movableMapIds.contains(body.id))hasBarrel=true;
  if(!hasBarrel && !barrelVisual.empty()) {
    const auto side=Vec3::normalize(laneEnd-laneStart);
    const auto point=(laneStart+laneEnd)*.5f+side*110.f;
    add(barrelVisual,point);
    }
  // Keep the original tavern population active during the untouched interval.
  player.setPosition(laneStart);player.setDirection(tableCenter-laneStart);player.updateTransform();
  bench.cam=C_Hold;lookFrom(laneStart+Vec3(0,180,0),tableTop);
  bench.movableReady=true;
  std::string massExamples="[";std::unordered_set<std::string> massClasses;
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(!body.enabled || !massClasses.insert(body.className).second)continue;
    if(massExamples.size()>1)massExamples+=',';
    massExamples+="{\"class\":"+jsonStr(body.className)+",\"visual\":"+jsonStr(body.visual)+
      ",\"kg\":"+ragdollNumber(body.mass)+"}";
    }
  event("movable_setup","\"world\":"+jsonStr(world.name())+",\"host\":"+jsonStr(symName(world,host))+
    ",\"host_cm\":"+ragdollVector(host->position())+",\"table_id\":"+std::to_string(tableId)+
    ",\"floor_dish_id\":"+std::to_string(bench.movableDish)+",\"tabletop_utensils\":6,\"temporary_vobs\":"+
    std::to_string(bench.movableFixtures.size())+",\"world_load_ms\":"+ragdollNumber(float(world.loadMilliseconds()))+
    ",\"map_vobs\":"+std::to_string(bench.movableMapIds.size())+",\"map_classes\":"+classCounts(mapClasses)+
    ",\"noncolliding_map_classes\":"+classCounts(noncollidingClasses)+
    ",\"enabled\":"+(world.physic()->movableVobsEnabled()?"true":"false")+
    ",\"walk_start_cm\":"+ragdollVector(laneStart)+",\"walk_end_cm\":"+ragdollVector(laneEnd)+
    ",\"mass_examples\":"+massExamples+"],\"bodies\":"+movableRows(world));
  }

float noiseFacingError(const Npc& npc,const Vec3& point) {
  const auto direction=point-npc.position();
  const float desired=std::atan2(direction.z,direction.x)*180.f/float(M_PI);
  return std::abs(std::remainder(desired-npc.rotation(),360.f));
  }

uint32_t noiseNpcId(const Npc* npc) {
  if(npc==nullptr)return 0;
  const auto found=bench.noiseNpcIds.find(npc);
  if(found!=bench.noiseNpcIds.end())return found->second;
  const auto id=++bench.noiseNextNpcId;
  bench.noiseNpcIds.emplace(npc,id);
  return id;
  }

std::string noiseListenerRows(World& world) {
  std::string rows="[";
  for(const auto& listener:bench.noiseListeners) {
    if(rows.size()>1)rows+=',';
    const auto& npc=*listener.npc;
    rows+="{\"role\":"+jsonStr(listener.role)+",\"npc_id\":"+std::to_string(noiseNpcId(&npc))+
      ",\"npc\":"+jsonStr(symName(world,&npc))+
      ",\"position_cm\":"+ragdollVector(npc.position())+",\"head_cm\":"+ragdollVector(npc.mapHeadBone())+
      ",\"yaw_deg\":"+ragdollNumber(npc.rotation())+
      ",\"distance_cm\":"+ragdollNumber((npc.position()-bench.noiseSource).length())+
      ",\"occlusion\":"+ragdollNumber(world.physic()->soundOclusion(bench.noiseSource,npc.mapHeadBone()))+
      ",\"facing_error_deg\":"+ragdollNumber(noiseFacingError(npc,bench.noiseSource))+
      ",\"hero_error_deg\":"+ragdollNumber(noiseFacingError(npc,world.player()->position()))+
      ",\"heard\":"+std::to_string(listener.heard)+",\"external\":"+(npc.isExternalControl()?"true":"false")+
      ",\"eligible\":"+(npc.canHearPhysicsNoise()?"true":"false")+
      ",\"process_policy\":"+std::to_string(int(npc.processPolicy()))+
      ",\"dialogue\":"+(Gothic::inst().isNpcInDialog(npc)?"true":"false")+"}";
    }
  return rows+"]";
  }

void noiseBegin(World& world,std::string_view phase,const Vec3& source) {
  bench.noiseTrace=true;bench.noisePhase=phase;bench.noiseStart=world.tickCount();bench.noiseSource=source;
  bench.noiseHasPushed=false;
  bench.noisePerceptions.clear();
  for(auto& listener:bench.noiseListeners)listener.heard=0;
  }

// Select actual supported tavern/world floor, using the same acoustic query as
// hearing. No synthetic wall, injected noise, spell sound or direct NPC turn.
bool noiseFloor(World& world,const Vec3& source,float distance,bool wall,Vec3& result,bool distant=false) {
  float best=-1;
  for(unsigned step=0;step<96;++step) {
    const float angle=float(step)*float(M_PI)/48.f;
    auto point=source+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
    const auto floor=world.physic()->landRay(point+Vec3(0,180,0),400);
    if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-bench.movableWalkStart.y)>70)continue;
    point.y=floor.v.y;
    bool free=true;
    for(const auto offset:{Vec3(35,0,0),Vec3(-35,0,0),Vec3(0,0,35),Vec3(0,0,-35)})
      if(world.physic()->ray(point+Vec3(0,85,0),point+offset+Vec3(0,85,0)).hasCol)free=false;
    if(!free)continue;
    const float occlusion=world.physic()->soundOclusion(source,point+Vec3(0,160,0));
    if(wall) {
      // 1200 cm is the engine's maximum physical-impact radius. This
      // endpoint is inside that radius but inaudible even at maximum volume.
      if(distance<=1200.f*(1.f-occlusion)+100.f || occlusion<.12f)continue;
      } else if(!distant && occlusion>.05f)continue;
    const float score=wall?occlusion:float(step==0 ? 2 : 1);
    if(score>best) {best=score;result=point;}
    }
  return best>=0;
  }

Npc* noiseAddListener(World& world,std::string role,const Vec3& point,Npc* npc=nullptr) {
  if(npc==nullptr)npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),point);
  if(!npc) {arenaAbort("physics noise listener insertion failed");return nullptr;}
  npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setInteraction(nullptr,true);
  npc->closeWeapon(true);npc->setPosition(point);npc->setExternalControl(false);
  npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
  npc->setDirection(point-bench.noiseSource);npc->updateTransform();
  // A native wait keeps the normal actor in this fixture without teleporting
  // or rotating it during observation. Hearing executes before this wait gate.
  npc->aiPush(AiQueue::aiWait(60000));
  const auto callback=world.script().findSymbolIndex("B_ASSESSQUIETSOUND");
  if(callback!=size_t(-1))npc->setPerceptionEnable(PERC_ASSESSQUIETSOUND,callback);
  bench.noiseListeners.push_back({npc,std::move(role),point,0});
  return npc;
  }

void noiseRetireListeners() {
  for(const auto& listener:bench.noiseListeners) {
    listener.npc->clearAiQueue();listener.npc->setExternalControl(true);
    }
  bench.noiseListeners.clear();
  }

void prepareNoiseFixture(World& world,Npc& player,bool tipping=false) {
  if(!bench.movableReady)prepareMovableFixture(world,player);
  if(!bench.movableReady)return;
  bench.noiseTipNegative=tipping;
  if(tipping) {
    // The nearest table is wedged between the counter and pub frame; the two
    // eastern tables have fixed candles. The southern map table has only the
    // frame beside its local +Z edge. Tip towards its free -Z side.
    uintptr_t tableId=0;
    Vec3 tableCenter;
    float southernmost=std::numeric_limits<float>::infinity();
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(body.className!="table" || !body.enabled || !bench.movableMapIds.contains(body.id))continue;
      const auto* mesh=Resources::loadMesh(body.visual);if(!mesh)continue;
      const auto* bounds=mesh->bbox();
      auto center=(bounds[0]+bounds[1])*.5f;body.transform.project(center);
      if(center.z<southernmost) {southernmost=center.z;tableId=body.id;tableCenter=center;}
      }
    if(!tableId) {arenaAbort("noise fixture lacks its southern tavern table");return;}
    bench.movableTable=tableId;bench.movableCenter=tableCenter;
    }
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* npc=world.npcById(i);npc && npc!=&player && (npc->position()-bench.movableCenter).length()<1400.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  bench.noiseTrace=true;
  event("physics_noise_setup","\"world\":"+jsonStr(world.name())+",\"table_id\":"+
    std::to_string(bench.movableTable)+",\"enabled\":"+(world.physic()->physicsNoiseEnabled()?"true":"false"));
  }

void noisePushTable(World& world,Npc* culprit=nullptr) {
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.id!=bench.movableTable)continue;
    const auto* mesh=Resources::loadMesh(body.visual);if(!mesh)break;
    const auto* bounds=mesh->bbox();auto center=(bounds[0]+bounds[1])*.5f;body.transform.project(center);
    auto point=(bounds[0]+bounds[1])*.5f;
    point.y=bounds[1].y;
    // Push across the narrower tabletop edge so the native hull rotates,
    // rather than receiving another centre-of-mass translation.
    const bool narrowX=bounds[1].x-bounds[0].x<bounds[1].z-bounds[0].z;
    if(narrowX)point.x=bounds[bench.noiseTipNegative?0:1].x;
    else point.z=bounds[bench.noiseTipNegative?0:1].z;
    body.transform.project(point);
    auto direction=point-center;direction.y=0;
    direction=Vec3::normalize(direction);
    // A real native impulse couple tips the table in place: the equal COM
    // reaction cancels linear momentum, preserving the 5 m hearing geometry.
    const Vec3 force=direction*(6.f*body.mass),reaction=force*-1.f;
    const auto pushed=world.physic()->applyMovableVobImpulse(center,140.f,
      [force,mass=body.mass](const Vec3&,float targetMass) {
        return std::abs(targetMass-mass)<.001f ? force : Vec3();
        },culprit,&point);
    const auto reacted=world.physic()->applyMovableVobImpulse(center,140.f,
      [reaction,mass=body.mass](const Vec3&,float targetMass) {
        return std::abs(targetMass-mass)<.001f ? reaction : Vec3();
        },culprit,&center);
    bench.noiseTableInitial=body.transform;bench.noiseTableMaximumTilt=0;bench.noiseHasPushed=true;
    event("physics_noise_push","\"phase\":"+jsonStr(bench.noisePhase)+",\"table_id\":"+
      std::to_string(body.id)+",\"affected\":"+std::to_string(pushed)+",\"mass_kg\":"+ragdollNumber(body.mass)+
      ",\"impulse_ns\":"+ragdollVector(force)+",\"application_point_cm\":"+ragdollVector(point)+
      ",\"reaction_impulse_ns\":"+ragdollVector(reaction)+",\"reaction_point_cm\":"+ragdollVector(center)+
      ",\"reaction_affected\":"+std::to_string(reacted)+
      ",\"transform\":"+ragdollMatrix(body.transform)+
      ",\"spell\":false,\"culprit\":"+(culprit?jsonStr(symName(world,culprit)):"null"));
    if(!pushed || !reacted)arenaAbort("physics noise silent table impulse couple was blocked");
    return;
    }
  arenaAbort("physics noise table absent");
  }

void movableCandleSetup(World& world,Npc& player) {
  const auto bodies=world.physic()->movableVobSnapshot();
  const DynamicWorld::MovableVobSnapshot* table=nullptr;
  const DynamicWorld::MovableVobSnapshot* candle=nullptr;
  float bestClearance=-1;
  bool negative=false;
  for(const auto& candidate:bodies) {
    if(!candidate.enabled || (candidate.zenId!=21133 && candidate.zenId!=21134))continue;
    const auto* mesh=Resources::loadMesh(candidate.visual);if(!mesh)continue;
    const auto* bounds=mesh->bbox();
    auto inverse=candidate.transform;inverse.inverse();
    const DynamicWorld::MovableVobSnapshot* supported=nullptr;
    for(const auto& item:bodies) {
      if(!item.enabled || item.visual.find("NW_CITY_CANDLE_01")==std::string::npos)continue;
      const auto* candleMesh=Resources::loadMesh(item.visual);if(!candleMesh)continue;
      const auto* cb=candleMesh->bbox();
      auto bottom=Vec3((cb[0].x+cb[1].x)*.5f,cb[0].y,(cb[0].z+cb[1].z)*.5f);
      item.transform.project(bottom);inverse.project(bottom);
      if(bottom.x>=bounds[0].x && bottom.x<=bounds[1].x && bottom.z>=bounds[0].z && bottom.z<=bounds[1].z &&
         std::abs(bottom.y-bounds[1].y)<5.f) {supported=&item;break;}
      }
    if(supported==nullptr)continue;
    const bool narrowX=bounds[1].x-bounds[0].x<bounds[1].z-bounds[0].z;
    auto center=(bounds[0]+bounds[1])*.5f;candidate.transform.project(center);
    for(const bool side:{false,true}) {
      auto edge=(bounds[0]+bounds[1])*.5f;
      if(narrowX)edge.x=bounds[side?0:1].x;
      else edge.z=bounds[side?0:1].z;
      candidate.transform.project(edge);
      auto direction=edge-center;direction.y=0;direction=Vec3::normalize(direction);
      float clearance=250.f;
      for(float height:{-20.f,0.f,30.f}) {
        const auto from=edge+direction*15.f+Vec3(0,height,0),to=from+direction*250.f;
        const auto hit=world.physic()->ray(from,to);
        if(hit.hasCol)clearance=std::min(clearance,(hit.v-from).length());
        }
      if(clearance>bestClearance) {bestClearance=clearance;table=&candidate;candle=supported;negative=side;}
      }
    }
  if(table==nullptr || candle==nullptr) {arenaAbort("candle fixture requires a movable candle on map table 21133 or 21134");return;}
  if(bestClearance<80.f) {arenaAbort("candle table has no clear narrow tipping edge");return;}
  const auto* tableMesh=Resources::loadMesh(table->visual);
  const auto* candleMesh=Resources::loadMesh(candle->visual);
  const auto* tb=tableMesh->bbox();const auto* cb=candleMesh->bbox();
  bench.candleTableBounds[0]=tb[0];bench.candleTableBounds[1]=tb[1];
  bench.candleSupport=Vec3((cb[0].x+cb[1].x)*.5f,cb[0].y,(cb[0].z+cb[1].z)*.5f);
  bench.candleTableInitial=table->transform;bench.candleInitial=movablePosition(candle->transform);
  bench.candleBody=candle->id;bench.candleZenId=candle->zenId;bench.candleTableZenId=table->zenId;
  bench.candleAttachments.clear();
  auto inverse=candle->transform;inverse.inverse();
  std::string attachments="[";unsigned flames=0,lights=0;
  for(const auto& child:world.vobAttachmentSnapshot(candle->zenId)) {
    const bool flame=child.visual.find("FIRE_COMPLETE_A0.TGA")!=std::string::npos;
    if(!flame && !child.isLight)continue;
    if(!child.hasRenderedPosition) {arenaAbort("candle child lacks a real rendered position");return;}
    Bench::CandleAttachment sample;
    sample.zenId=child.zenId;sample.visual=child.visual;sample.light=child.isLight;
    sample.initial=child.renderedPosition;sample.local=child.renderedPosition;inverse.project(sample.local);
    bench.candleAttachments.emplace_back(sample);
    flames+=flame;lights+=child.isLight;
    if(attachments.size()>1)attachments+=',';
    attachments+="{\"zen_id\":"+std::to_string(child.zenId)+",\"parent_zen_id\":"+std::to_string(child.parentZenId)+
      ",\"visual\":"+jsonStr(child.visual)+",\"light\":"+(child.isLight?"true":"false")+
      ",\"position_cm\":"+ragdollVector(child.renderedPosition)+",\"candle_local_cm\":"+ragdollVector(sample.local)+"}";
    }
  if(flames!=1 || lights<1 || lights>2) {arenaAbort("candle fixture requires its real fire sprite and one or two real lights");return;}
  bench.movableTable=table->id;bench.noiseTipNegative=negative;
  bench.movableCenter=(tb[0]+tb[1])*.5f;table->transform.project(bench.movableCenter);
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* npc=world.npcById(i);npc && npc!=&player && (npc->position()-bench.movableCenter).length()<1400.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  auto edge=(tb[0]+tb[1])*.5f;
  if(tb[1].x-tb[0].x<tb[1].z-tb[0].z)edge.x=tb[negative?0:1].x;
  else edge.z=tb[negative?0:1].z;
  table->transform.project(edge);
  auto outward=edge-bench.movableCenter;outward.y=0;outward=Vec3::normalize(outward);
  const auto camera=bench.movableCenter+outward*240.f+Vec3(-outward.z,0,outward.x)*100.f+Vec3(0,150,0);
  auto hero=bench.movableCenter-outward*300.f;
  if(const auto floor=world.physic()->landRay(hero+Vec3(0,180,0),400);floor.hasCol)hero.y=floor.v.y;
  player.setPosition(hero);player.setDirection(bench.movableCenter-hero);player.updateTransform();
  bench.cam=C_Hold;lookFrom(camera,bench.movableCenter+Vec3(0,15,0));
  bench.candleMaximumTilt=0;bench.candleMaximumTravel=0;bench.candleSamples=0;
  bench.candleLeftTable=false;bench.candleSnapshotMissing=false;bench.candleCaptured=false;
  event("movable_candle_setup","\"world\":"+jsonStr(world.name())+",\"table_zen_id\":"+
    std::to_string(table->zenId)+",\"candle_zen_id\":"+std::to_string(candle->zenId)+
    ",\"table_mass_kg\":"+ragdollNumber(table->mass)+",\"candle_mass_kg\":"+ragdollNumber(candle->mass)+
    ",\"clearance_cm\":"+ragdollNumber(bestClearance)+",\"negative_edge\":"+(negative?"true":"false")+
    ",\"table_transform\":"+ragdollMatrix(table->transform)+",\"candle_transform\":"+ragdollMatrix(candle->transform)+
    ",\"attachments\":"+attachments+"]");
  }

void movableCandleFields(World& world) {
  if(!bench.candleTrace)return;
  ++bench.candleSamples;
  const auto bodies=world.physic()->movableVobSnapshot();
  const DynamicWorld::MovableVobSnapshot* table=nullptr;
  const DynamicWorld::MovableVobSnapshot* candle=nullptr;
  for(const auto& body:bodies) {
    if(body.zenId==bench.candleTableZenId)table=&body;
    if(body.id==bench.candleBody)candle=&body;
    }
  if(table==nullptr || candle==nullptr || !table->enabled || !candle->enabled) {bench.candleSnapshotMissing=true;return;}
  const auto* before=bench.candleTableInitial.data();const auto* after=table->transform.data();
  const Vec3 a(before[4],before[5],before[6]),b(after[4],after[5],after[6]);
  const float tilt=std::acos(std::clamp((a.x*b.x+a.y*b.y+a.z*b.z)/(a.length()*b.length()),-1.f,1.f))*180.f/float(M_PI);
  bench.candleMaximumTilt=std::max(bench.candleMaximumTilt,tilt);
  bench.candleMaximumTravel=std::max(bench.candleMaximumTravel,(movablePosition(candle->transform)-bench.candleInitial).length());
  auto bottom=bench.candleSupport;candle->transform.project(bottom);
  auto inverse=table->transform;inverse.inverse();inverse.project(bottom);
  const auto* bounds=bench.candleTableBounds;
  bench.candleLeftTable|=bottom.x<bounds[0].x-3.f || bottom.x>bounds[1].x+3.f ||
    bottom.z<bounds[0].z-3.f || bottom.z>bounds[1].z+3.f || std::abs(bottom.y-bounds[1].y)>5.f;
  const auto children=world.vobAttachmentSnapshot(bench.candleZenId);
  for(auto& sample:bench.candleAttachments) {
    const auto child=std::find_if(children.begin(),children.end(),[&](const auto& item) {return item.zenId==sample.zenId;});
    if(child==children.end() || !child->hasRenderedPosition) {bench.candleSnapshotMissing=true;continue;}
    auto expected=sample.local;candle->transform.project(expected);
    sample.maximumError=std::max(sample.maximumError,(child->renderedPosition-expected).length());
    sample.maximumTravel=std::max(sample.maximumTravel,(child->renderedPosition-sample.initial).length());
    }
  if(tilt>45.f && bench.candleLeftTable && !bench.candleCaptured) {
    bench.shot="movable-candle-tip";bench.candleCaptured=true;
    }
  }

void movableCandlePush(World& world) {
  if(bench.candleAttachments.empty()) {arenaAbort("candle setup is required before push");return;}
  noiseBegin(world,"candle",bench.movableCenter);
  bench.candleStart=world.tickCount();bench.candleTrace=true;
  noisePushTable(world);
  movableCandleFields(world);
  }

void movableCandleCheckpoint(World& world) {
  movableCandleFields(world);
  std::string attachments="[";float maximumError=0;
  for(const auto& sample:bench.candleAttachments) {
    if(attachments.size()>1)attachments+=',';
    attachments+="{\"zen_id\":"+std::to_string(sample.zenId)+",\"visual\":"+jsonStr(sample.visual)+
      ",\"light\":"+(sample.light?"true":"false")+",\"max_local_offset_error_cm\":"+ragdollNumber(sample.maximumError)+
      ",\"max_displacement_cm\":"+ragdollNumber(sample.maximumTravel)+"}";
    maximumError=std::max(maximumError,sample.maximumError);
    }
  event("movable_candle_checkpoint","\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.candleStart)+
    ",\"samples\":"+std::to_string(bench.candleSamples)+",\"table_maximum_tilt_deg\":"+ragdollNumber(bench.candleMaximumTilt)+
    ",\"candle_maximum_displacement_cm\":"+ragdollNumber(bench.candleMaximumTravel)+
    ",\"candle_left_tabletop\":"+(bench.candleLeftTable?"true":"false")+
    ",\"missing_render_snapshot\":"+(bench.candleSnapshotMissing?"true":"false")+
    ",\"max_local_offset_error_cm\":"+ragdollNumber(maximumError)+",\"tip_frame_captured\":"+(bench.candleCaptured?"true":"false")+
    ",\"attachments\":"+attachments+"]");
  bench.candleTrace=false;
  }

void noiseTableCase(World& world,Npc& player,bool push=true) {
  noiseRetireListeners();
  noiseBegin(world,"table",bench.movableCenter);
  Vec3 near,wall,far;bool hasWall=false;
  if(!noiseFloor(world,bench.noiseSource,500,false,near)) {arenaAbort("noise fixture lacks a clear 5m listener position");return;}
  for(float distance:{500.f,600.f,700.f,800.f,900.f,1000.f,1100.f})
    if(noiseFloor(world,bench.noiseSource,distance,true,wall)) {hasWall=true;break;}
  if(!hasWall) {arenaAbort("noise fixture lacks an acoustically occluded floor behind a real wall");return;}
  bool hasFar=false;
  for(float distance:{1800.f,2000.f,2400.f,2800.f})
    if(noiseFloor(world,bench.noiseSource,distance,false,far,true)) {hasFar=true;break;}
  if(!hasFar) {arenaAbort("noise fixture lacks a supported distant listener position");return;}
  if(!noiseAddListener(world,"near",near) || !noiseAddListener(world,"wall",wall) || !noiseAddListener(world,"far",far))return;
  // The hero's bearing is visibly different from the table at the near NPC.
  const auto side=Vec3::normalize(near-bench.noiseSource);
  auto hero=near+Vec3(-side.z,0,side.x)*220.f;
  const auto floor=world.physic()->landRay(hero+Vec3(0,180,0),400);
  if(floor.hasCol)hero.y=floor.v.y;
  player.setPosition(hero);player.setDirection(bench.noiseSource-hero);player.updateTransform();
  bench.cam=C_Hold;lookFrom(near+Vec3(0,250,0),bench.noiseSource);
  event("physics_noise_case","\"phase\":\"table\",\"source_cm\":"+ragdollVector(bench.noiseSource)+
    ",\"listeners\":"+noiseListenerRows(world));
  if(push)noisePushTable(world);
  }

void noiseDropItems(World& world,unsigned count) {
  const auto instance=world.script().findSymbolIndex("ITFO_BEER");
  if(instance==size_t(-1)) {arenaAbort("noise fixture beer Item instance unavailable");return;}
  for(unsigned i=0;i<count;++i) {
    auto matrix=Matrix4x4::mkIdentity();
    // Native Item proxies have a 30 cm minimum diameter, even for this small
    // mesh. Keep them separate until their ordinary simultaneous floor hits.
    const float x=count==1 ? 0.f : (float(i%3)-1.f)*36.f;
    const float z=count==1 ? 0.f : (float(i/3)-1.5f)*36.f;
    matrix.translate(bench.noiseSource+Vec3(x,100.f,z));
    auto* item=world.addItemDyn(instance,matrix,size_t(-1));
    if(!item || item->physicsMass()<=0) {arenaAbort("noise fixture real dynamic cup Item has no native mass");return;}
    event("physics_noise_drop","\"phase\":"+jsonStr(bench.noisePhase)+",\"ordinal\":"+
      std::to_string(i)+",\"item\":\"ITFO_BEER\",\"mass_kg\":"+ragdollNumber(item->physicsMass())+
      ",\"position_cm\":"+ragdollVector(item->position())+",\"drop_height_cm\":100");
    }
  }

void noiseItemCase(World& world,Npc& player,std::string_view phase) {
  noiseRetireListeners();
  auto source=(bench.movableWalkStart+bench.movableWalkEnd)*.5f;
  noiseBegin(world,phase,source);
  Vec3 near,far;
  if(!noiseFloor(world,source,110,false,near) || !noiseFloor(world,source,600,false,far,true)) {
    arenaAbort("noise cup fixture lacks supported near/far floor");return;
    }
  if(phase=="dialogue" && bench.noiseHost==nullptr) {arenaAbort("physics noise dialogue needs the actual Coragon host");return;}
  auto* listener=noiseAddListener(world,phase=="dialogue"?"dialogue":"near",near,
    phase=="dialogue"?bench.noiseHost:nullptr);
  if(!listener)return;
  if(!noiseAddListener(world,"far",far))return;
  player.setPosition(near+Vec3(0,0,80));player.setDirection(near-player.position());player.updateTransform();
  event("physics_noise_case","\"phase\":"+jsonStr(phase)+",\"source_cm\":"+ragdollVector(source)+
    ",\"listeners\":"+noiseListenerRows(world));
  if(phase=="dialogue") {
    listener->clearAiQueue();listener->aiPush(AiQueue::aiProcessInfo(player));
    // Opening the actual output pipe takes a native tick; a separate command
    // verifies the real dialog state before releasing the cup.
    return;
    }
  noiseDropItems(world,phase=="burst"?10:1);
  }

void noiseCheckpoint(World& world) {
  std::string perceptions="[";
  for(const auto& [npc,count]:bench.noisePerceptions) {
    if(perceptions.size()>1)perceptions+=',';
    perceptions+="{\"npc_id\":"+std::to_string(npc)+",\"count\":"+std::to_string(count)+"}";
    }
  std::string table="null";
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(body.id==bench.movableTable) {table=ragdollMatrix(body.transform);break;}
  event("physics_noise_checkpoint","\"phase\":"+jsonStr(bench.noisePhase)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-bench.noiseStart)+",\"listeners\":"+noiseListenerRows(world)+
    ",\"perceptions\":"+perceptions+"],\"table_transform\":"+table+
    ",\"table_maximum_tilt_deg\":"+ragdollNumber(bench.noiseTableMaximumTilt)+
    ",\"table_tilt_above_45_deg\":"+(bench.noiseTableMaximumTilt>45.f?"true":"false"));
  }

void noiseFields(World& world) {
  if(!bench.noiseTrace || bench.movableMeasuring || !bench.noiseHasPushed || bench.noisePhase!="table" || world.tickCount()-bench.noiseStart>2000)return;
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(body.id==bench.movableTable) {
      const auto* before=bench.noiseTableInitial.data();const auto* after=body.transform.data();
      const Vec3 a(before[4],before[5],before[6]),b(after[4],after[5],after[6]);
      const float cosine=(a.x*b.x+a.y*b.y+a.z*b.z)/(a.length()*b.length());
      const float tilt=std::acos(std::clamp(cosine,-1.f,1.f))*180.f/float(M_PI);
      bench.noiseTableMaximumTilt=std::max(bench.noiseTableMaximumTilt,tilt);
      }
  for(const auto& listener:bench.noiseListeners)
    if(listener.role=="near")
      event("physics_noise_turn","\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.noiseStart)+
        ",\"npc_id\":"+std::to_string(noiseNpcId(listener.npc))+",\"facing_error_deg\":"+
        ragdollNumber(noiseFacingError(*listener.npc,bench.noiseSource))+
        ",\"hero_error_deg\":"+ragdollNumber(noiseFacingError(*listener.npc,world.player()->position())));
  }

void prepareMovableCast(World& world,Npc& player) {
  // Existing native actors stop only after the 60-second untouched observation.
  for(uint32_t i=0;i<world.npcCount();++i) {
    auto* npc=world.npcById(i);
    if(npc && npc!=&player && (npc->position()-bench.movableCenter).length()<1400) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
    }
  player.closeWeapon(true);
  Vec3 target,caster;bool found=false;
  const auto npcFreeLane=[&](const Vec3& from,const Vec3& to) {
    const auto segment=to-from;
    const float lengthSquared=segment.x*segment.x+segment.z*segment.z;
    if(lengthSquared<=1.f)return false;
    // A stopped NPC still intercepts the real projectile. Check every actor,
    // including the caster endpoint, with a conservative 100 cm lane radius.
    // Height separates the tavern floor from actors upstairs or downstairs.
    for(uint32_t id=0;id<world.npcCount();++id) {
      const auto* other=world.npcById(id);if(!other || other==&player)continue;
      const auto center=other->collosionCenter();
      const float t=std::clamp(((center.x-from.x)*segment.x+(center.z-from.z)*segment.z)/lengthSquared,0.f,1.f);
      const auto closest=from+segment*t+Vec3(0,100,0);
      const float dx=center.x-closest.x,dz=center.z-closest.z;
      if(dx*dx+dz*dz<100.f*100.f && std::abs(center.y-closest.y)<160.f)return false;
      }
    const auto side=Vec3(-segment.z,0,segment.x)*(60.f/std::sqrt(lengthSquared));
    for(const auto offset:{Vec3(),side,side*-1.f}) {
      const auto hit=world.physic()->rayNpc(from+offset+Vec3(0,100,0),to+offset+Vec3(0,100,0),&player);
      if(hit.npcHit!=nullptr)return false;
      }
    return true;
    };
  for(unsigned i=0;i<16 && !found;++i) {
    const float angle=float(i)*float(M_PI)/8.f;
    const Vec3 direction(std::cos(angle),0,std::sin(angle));
    caster=Vec3(bench.movableCenter.x,bench.movableWalkStart.y,bench.movableCenter.z)-direction*400.f;
    target=Vec3(bench.movableCenter.x,bench.movableWalkStart.y,bench.movableCenter.z)+direction*180.f;
    bool valid=true;
    for(auto* point:{&caster,&target}) {
      const auto floor=world.physic()->landRay(*point+Vec3(0,80,0),180);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-bench.movableWalkStart.y)>40) {valid=false;break;}
      point->y=floor.v.y;
      // Endpoint body volumes must not be hidden in furniture or walls.
      for(const auto side:{Vec3(25,0,0),Vec3(-25,0,0),Vec3(0,0,25),Vec3(0,0,-25)})
        if(world.physic()->ray(*point+Vec3(0,85,0),*point+side+Vec3(0,85,0)).hasCol)valid=false;
      }
    if(valid && npcFreeLane(caster,target) &&
       !world.physic()->ray(caster+Vec3(0,145,0),target+Vec3(0,145,0)).hasCol)found=true;
    }
  if(!found) {arenaAbort("tavern fireball needs supported clear caster/target endpoints across furniture");return;}
  player.setPosition(caster);player.setDirection(target-caster);player.updateTransform();
  bench.cam=C_Hold;lookFrom(caster+Vec3(0,185,0),bench.movableCenter+Vec3(0,40,0));
  auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),target);
  if(!npc) {arenaAbort("movable real fireball target insertion failed");return;}
  npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
  npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
  npc->setDirection(player.position()-target);npc->updateTransform();bench.spellActors.push_back({npc,"movable_target"});
  for(size_t i=0;i<world.script().symbolsCount() && !bench.spellRunes.contains(23);++i) {
    const auto* symbol=world.script().findSymbol(i);if(!symbol || symbol->type()!=zenkit::DaedalusDataType::INSTANCE)continue;
    auto name=symbol->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_"))continue;
    auto item=std::make_unique<Item>(world,i,Item::T_Inventory);
    if(item->spellId()==23) {bench.spellRunes[23]=i;player.addItem(i,item->isRune()?1:5);}
    }
  player.handle().attribute[ATR_MANA]=player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  Gothic::settingsSetI("GAME","spell_physics",1);
  event("movable_cast_setup","\"spell_id\":23,\"npc_free_lane_radius_cm\":100,\"target_cm\":"+
    ragdollVector(target)+",\"caster_cm\":"+ragdollVector(player.position()));
  }

bool destructionOwns(uintptr_t id) {
  return std::find(bench.destructionIds.begin(),bench.destructionIds.end(),id)!=bench.destructionIds.end();
  }

std::string destructionRows(World& world) {
  std::string result="[";
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(!destructionOwns(body.id))continue;
    if(result.size()>1)result+=',';
    result+="{\"id\":"+std::to_string(body.id)+",\"visual\":"+jsonStr(body.visual)+
      ",\"class\":"+jsonStr(body.className)+",\"destructible\":"+(body.destructible?"true":"false")+
      ",\"broken\":"+(body.broken?"true":"false")+",\"health\":"+ragdollNumber(body.health)+
      ",\"max_health\":"+ragdollNumber(body.maxHealth)+",\"mass_kg\":"+ragdollNumber(body.mass)+
      ",\"sleeping\":"+(body.sleeping?"true":"false")+",\"npc_contact\":"+(body.touchedByNpc?"true":"false")+
      ",\"position_cm\":"+ragdollVector(movablePosition(body.transform))+",\"velocity_cm_s\":"+
      ragdollVector(body.velocity)+"}";
    }
  return result+"]";
  }

std::string destructionFragments(World& world) {
  std::string result="[";
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(!destructionOwns(part.parentId))continue;
    if(result.size()>1)result+=',';
    const auto key=std::make_pair(part.parentId,part.piece);
    result+="{\"parent_id\":"+std::to_string(part.parentId)+",\"piece\":"+std::to_string(part.piece)+
      ",\"sleeping\":"+(part.sleeping?"true":"false")+",\"position_cm\":"+ragdollVector(movablePosition(part.transform))+
      ",\"velocity_cm_s\":"+ragdollVector(part.velocity)+",\"max_displacement_cm\":"+
      ragdollNumber(bench.destructionFragmentTravel[key])+"}";
    }
  return result+"]";
  }

void destructionFields(World& world) {
  if(!bench.destructionTrace)return;
  const auto elapsed=world.tickCount()-bench.destructionStart;
  if((bench.destructionCase=="drop1" || bench.destructionCase=="drop5") && elapsed<=2500 &&
     elapsed>=bench.destructionFallSample) {
    bench.destructionFallSample=elapsed+100;
    event("destruction_fall","\"case\":"+jsonStr(bench.destructionCase)+",\"elapsed_ms\":"+
      std::to_string(elapsed)+",\"bodies\":"+destructionRows(world));
    }
  const bool sleepSample=bench.destructionLastBreak!=0 && !bench.destructionSleepSampled &&
                         world.tickCount()-bench.destructionLastBreak>=15000;
  if(bench.movableMeasuring && !sleepSample)return;
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(!destructionOwns(part.parentId))continue;
    const auto key=std::make_pair(part.parentId,part.piece);
    const auto point=movablePosition(part.transform);
    const auto [entry,inserted]=bench.destructionFragmentOrigins.emplace(key,point);
    bench.destructionFragmentTravel[key]=std::max(bench.destructionFragmentTravel[key],(point-entry->second).length());
    }
  if(sleepSample) {
    bench.destructionSleepSampled=true;
    event("destruction_sleep15","\"case\":"+jsonStr(bench.destructionCase)+",\"elapsed_since_last_break_ms\":"+
      std::to_string(world.tickCount()-bench.destructionLastBreak)+",\"fragments\":"+destructionFragments(world));
    }
  }

uintptr_t destructionAdd(World& world,std::string_view name,const Vec3& support) {
  const std::string visual(name);
  const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
  if(!mesh || !rule)return 0;
  const auto* bounds=mesh->bbox();
  zenkit::VirtualObject data;data.type=zenkit::VirtualObjectType::zCVob;data.id=uint32_t(-1);
  data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
  data.position={support.x-(bounds[0].x+bounds[1].x)*.5f,support.y-bounds[0].y+.2f,support.z-(bounds[0].z+bounds[1].z)*.5f};
  data.visual=std::make_shared<zenkit::VisualMesh>();data.visual->type=zenkit::VisualType::MESH;data.visual->name=visual;
  const auto before=world.physic()->movableVobSnapshot();
  auto fixture=std::make_unique<Bench::MovableFixture>();
  fixture->visual.setVisual(data,world,false,false);
  auto matrix=Matrix4x4::mkIdentity();matrix.translate(data.position.x,data.position.y,data.position.z);
  fixture->visual.setObjMatrix(matrix);
  fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
  if(fixture->body.isEmpty())return 0;
  fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& pose){view->setObjMatrix(pose);});
  fixture->visual.setupFracture(world,fixture->body);
  bench.destructionFixtures.emplace_back(std::move(fixture));
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {
      bench.destructionIds.push_back(body.id);bench.movableIds.push_back(body.id);return body.id;
      }
  return 0;
  }

bool destructionDropFloor(World& world,Vec3& result) {
  // A five-metre drop must start under open sky, not intersect the tavern roof.
  for(float distance:{600.f,900.f,1200.f,1600.f,2000.f})
    for(unsigned i=0;i<64;++i) {
      const float angle=float(i)*float(M_PI)/32.f;
      const auto point=bench.movableCenter+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
      const auto floor=world.physic()->landRay(point+Vec3(0,180,0),500);
      if(!floor.hasCol || floor.n.y<.95f || std::abs(floor.v.y-bench.movableWalkStart.y)>200)continue;
      bool clear=true;
      for(const auto delta:{Vec3(),Vec3(70,0,0),Vec3(-70,0,0),Vec3(0,0,70),Vec3(0,0,-70)}) {
        const auto support=world.physic()->landRay(floor.v+delta+Vec3(0,30,0),60);
        if(!support.hasCol || support.n.y<.95f || std::abs(support.v.y-floor.v.y)>2 ||
           world.physic()->ray(floor.v+delta+Vec3(0,10,0),floor.v+delta+Vec3(0,720,0)).hasCol)clear=false;
        }
      if(clear) {result=floor.v;return true;}
      }
  return false;
  }

bool destructionSupported(World& world,std::string_view visual,const Vec3& at,Vec3& support,Vec3& failed) {
  const auto* mesh=Resources::loadMesh(std::string(visual));
  if(!mesh) {failed=at;return false;}
  const auto* bounds=mesh->bbox();
  const auto size=bounds[1]-bounds[0];
  const auto center=world.physic()->landRay(at+Vec3(0,30,0),80);
  if(!center.hasCol || center.n.y<.8f || std::abs(center.v.y-at.y)>20.f) {failed=at;return false;}
  // Check the whole identity-oriented footprint: all corners, edge midpoints,
  // and centre. The body also needs clear space up to its original mesh top.
  for(int x=-1;x<=1;++x)
    for(int z=-1;z<=1;++z) {
      const auto point=Vec3(at.x+size.x*.5f*float(x),center.v.y,at.z+size.z*.5f*float(z));
      const auto floor=world.physic()->landRay(point+Vec3(0,30,0),80);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-center.v.y)>3.f ||
         world.physic()->ray(point+Vec3(0,2,0),point+Vec3(0,size.y+2.f,0)).hasCol) {
        failed=point;return false;
        }
      }
  support=center.v;return true;
  }

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
        if(!destructionOwns(body.id))continue;
        auto center=(body.localBounds[0]+body.localBounds[1])*.5f;body.transform.project(center);
        const auto hit=world.physic()->ray(camera,center);
        if(hit.hasCol && (hit.v-center).length()>85.f)clear=false;
        }
      if(!clear)continue;
      bench.cam=C_Hold;lookFrom(camera,source+Vec3(0,55,0));
      event("destruction_camera","\"case\":"+jsonStr(bench.destructionCase)+",\"camera_cm\":"+
        ragdollVector(camera)+",\"target_cm\":"+ragdollVector(source+Vec3(0,55,0)));
      return;
      }
  arenaAbort("destruction before/after camera has no clear view of both native props");
  }

void destructionSetup(World& world,Npc& player,std::string_view name) {
  if(!bench.destructionReady) {
    if(!bench.movableReady)prepareMovableFixture(world,player);
    if(!bench.movableReady)return;
    prepareMovableCast(world,player);
    if(bench.spellActors.empty())return;
    bench.destructionCaster=player.position();bench.destructionTarget=bench.spellActors.back().npc->position();
    bench.destructionDirection=Vec3::normalize(bench.destructionTarget-bench.destructionCaster);
    bench.destructionDirection.y=0;
    bench.destructionReady=true;
    }
  noiseRetireListeners();
  if(bench.destructionActor && bench.destructionActor!=&player) {
    bench.destructionActor->clearAiQueue();bench.destructionActor->setExternalControl(true);
    }
  bench.destructionActor=nullptr;
  bench.destructionShooting=false;bench.destructionTrace=false;
  bench.destructionFixtures.clear();bench.destructionIds.clear();
  bench.destructionFragmentOrigins.clear();bench.destructionFragmentTravel.clear();
  bench.destructionCase=name;bench.destructionArrows=0;
  player.clearAiQueue();player.clearGoTo();player.stopWalking();player.closeWeapon(true);
  player.setPosition(bench.destructionCaster);player.setDirection(bench.destructionDirection);player.updateTransform();
  auto direction=bench.destructionDirection,side=Vec3(-direction.z,0,direction.x);
  auto source=bench.destructionTarget;
  if(name=="drop1" || name=="drop5") {
    if(!destructionDropFloor(world,source)) {arenaAbort("destruction drop needs flat open-sky native ground");return;}
    const float height=name=="drop1"?100.f:500.f;
    if(!destructionAdd(world,"NW_HARBOUR_CRATE_01.3DS",source+Vec3(0,height,0))) {
      arenaAbort("destruction drop crate insertion failed");return;
      }
    player.setPosition(source-direction*250.f);player.updateTransform();
    bench.cam=C_Hold;lookFrom(source-direction*450.f+side*180.f+Vec3(0,250,0),source+Vec3(0,200,0));
    event("destruction_drop_setup","\"case\":"+jsonStr(name)+",\"height_cm\":"+ragdollNumber(height)+
      ",\"floor_cm\":"+ragdollVector(source)+",\"clear_vertical_cm\":720");
    }
  else if(name=="fireball" || name=="five") {
    const unsigned count=name=="five"?5:2;
    // The functional pair sits beyond the edge of the authored tavern table.
    if(name=="five") {
      // The functional pair stays in Coragon's tavern. Performance uses real
      // open stock-world ground, where the complete ring and its flight lane
      // fit without walls or unsupported footprint corners.
      if(const auto* anchor=world.findPoint("NW_XARDAS_TOWER_VALLEY_091",false)) {
        player.setPosition(anchor->pos);player.updateTransform();
        }
      if(!chooseRagdollArena(world,player)) {arenaAbort("destruction five needs native open stock-world ground");return;}
      const auto original=bench.ragdollArenaTarget-Vec3(0,80,0);
      direction=Vec3(0,0,1);side=Vec3(-1,0,0);
      bool found=false;Vec3 caster;
      for(float forward:{0.f,-100.f,100.f,-200.f,200.f}) {
        for(float lateral:{0.f,-100.f,100.f,-200.f,200.f}) {
          const auto candidate=original+direction*forward+side*lateral;
          const auto floor=world.physic()->landRay(candidate+Vec3(0,300,0),900);
          if(!floor.hasCol || floor.n.y<.98f)continue;
          auto center=candidate;center.y=floor.v.y;
          bool supported=true;Vec3 failed;
          for(unsigned i=0;i<count;++i) {
            const float angle=float(i)*float(2*M_PI)/float(count)+float(M_PI)/10.f;
            const auto at=center+(side*std::cos(angle)+direction*std::sin(angle))*140.f;
            Vec3 support;
            if(!destructionSupported(world,(i%2)?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS",at,support,failed)) {
              supported=false;break;
              }
            // The native blast must reach every original centre through the
            // actual world mesh, before any fixture body is inserted.
            const auto* mesh=Resources::loadMesh((i%2)?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS");
            const auto height=mesh->bbox()[1].y-mesh->bbox()[0].y;
            if(world.physic()->ray(center+Vec3(0,92,0),support+Vec3(0,height*.5f,0)).hasCol) {
              supported=false;failed=at;break;
              }
            }
          auto hero=center-direction*500.f;
          const auto heroFloor=world.physic()->landRay(hero+Vec3(0,300,0),900);
          if(!heroFloor.hasCol || heroFloor.n.y<.98f || std::abs(heroFloor.v.y-center.y)>30.f) supported=false;
          else {
            hero.y=heroFloor.v.y;
            if(world.physic()->ray(hero+Vec3(0,145,0),center+Vec3(0,92,0)).hasCol ||
               world.physic()->rayNpc(hero+Vec3(0,100,0),center+Vec3(0,100,0),&player).npcHit!=nullptr) supported=false;
            }
          event("destruction_ring_candidate","\"source_cm\":"+ragdollVector(center)+",\"supported\":"+
            (supported?"true":"false")+",\"failed_probe_cm\":"+(supported?"null":ragdollVector(failed)));
          if(supported) {source=center;caster=hero;found=true;break;}
          }
        if(found)break;
        }
      if(!found) {arenaAbort("destruction five ring lacks native support, open blast rays, or clear caster lane");return;}
      bench.destructionCaster=caster;bench.destructionTarget=source;bench.destructionDirection=direction;
      player.setPosition(caster);player.setDirection(direction);player.updateTransform();
      auto* target=bench.spellActors[0].npc;target->setPosition(source);target->setDirection(caster-source);target->updateTransform();
      event("destruction_site","\"case\":\"five\",\"location\":\"stock_outdoor_arena\",\"waypoint\":"+
        jsonStr(bench.ragdollArenaWaypoint)+",\"caster_cm\":"+ragdollVector(caster)+",\"target_cm\":"+ragdollVector(source));
      }
    for(unsigned i=0;i<count;++i) {
      // Put a gap towards the caster, so the rear crate cannot intercept the
      // real fireball before it reaches the NPC at the centre of the ring.
      const float angle=float(i)*float(2*M_PI)/float(count)+float(M_PI)/10.f;
      const auto at=name=="five" ? source+(side*std::cos(angle)+direction*std::sin(angle))*140.f :
                                      source+side*(i?100.f:-100.f)+direction*40.f;
      // Start below tabletop height: the ground query must not mistake
      // nearby authored furniture for the floor of the tavern.
      const auto floor=world.physic()->landRay(at+Vec3(0,30,0),80);
      const bool supported=floor.hasCol && floor.n.y>.8f && std::abs(floor.v.y-source.y)<=20.f;
      event("destruction_preparation","\"case\":"+jsonStr(name)+",\"ordinal\":"+std::to_string(i)+
        ",\"at_cm\":"+ragdollVector(at)+",\"has_floor\":"+(floor.hasCol?"true":"false")+
        ",\"floor_cm\":"+ragdollVector(floor.v)+",\"normal\":"+ragdollVector(floor.n)+
        ",\"supported\":"+(supported?"true":"false"));
      if(!supported || !destructionAdd(world,(i%2)?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS",floor.v)) {
        arenaAbort("destruction fireball needs supported real crate/barrel positions");return;
        }
      }
    if(name=="five") {
      const auto blast=bench.spellActors[0].npc->collosionCenter();
      const bool flightClear=!world.physic()->ray(bench.destructionCaster+Vec3(0,145,0),blast).hasCol;
      unsigned visible=0;
      for(const auto& body:world.physic()->movableVobSnapshot()) {
        if(!destructionOwns(body.id))continue;
        auto center=(body.localBounds[0]+body.localBounds[1])*.5f;body.transform.project(center);
        const auto hit=world.physic()->ray(blast,center);
        // The final intersection is with the tested object's own shell.
        if(!hit.hasCol || (hit.v-center).length()<=85.f)++visible;
        }
      event("destruction_five_los","\"flight_clear\":"+std::string(flightClear?"true":"false")+
        ",\"unobstructed_bodies\":"+std::to_string(visible)+",\"blast_cm\":"+ragdollVector(blast));
      if(!flightClear || visible!=count) {arenaAbort("destruction five native bodies obstruct the real fireball or one another");return;}
      }
    if(name=="fireball") {
      noiseBegin(world,name,source+Vec3(0,60,0));
      Vec3 near;bool listenerFloor=false;
      // The same blast also knocks the authored table. Its earlier impact has
      // a 7.2m hearing radius; a listener beyond it can identify the louder break
      // through the unchanged native hearing cooldown and acoustic queries.
      for(float distance:{1000.f,1100.f,1200.f})
        if(noiseFloor(world,bench.noiseSource,distance,false,near)) {listenerFloor=true;break;}
      if(!listenerFloor) {arenaAbort("destruction needs a clear supported 10-12m listener floor");return;}
      if(!noiseAddListener(world,"destruction-near",near))return;
      event("destruction_listener_setup","\"case\":"+jsonStr(name)+",\"position_cm\":"+ragdollVector(near)+
        ",\"break_source_cm\":"+ragdollVector(bench.noiseSource)+",\"distance_cm\":"+
        ragdollNumber((near-bench.noiseSource).length())+",\"table_distance_cm\":"+
        ragdollNumber((near-bench.movableCenter).length()));
      } else bench.noiseTrace=false;
    destructionCamera(world,source,direction);
    }
  else if(name=="arrows" || name=="walk-hero" || name=="walk-npc") {
    source=bench.destructionTarget+direction*80.f;
    // The fireball target is no longer a participant in these cases.
    auto* spellTarget=bench.spellActors[0].npc;
    spellTarget->setPosition(bench.destructionCaster);spellTarget->updateTransform();
    const auto floor=world.physic()->landRay(source+Vec3(0,30,0),80);
    if(!floor.hasCol || !destructionAdd(world,"NW_HARBOUR_CRATE_01.3DS",floor.v)) {
      arenaAbort("destruction single-crate insertion failed");return;
      }
    source=floor.v;
    if(name!="arrows") {
      const auto approach=direction*-1.f;
      auto start=source-approach*170.f;
      const auto ground=world.physic()->landRay(start+Vec3(0,80,0),180);
      if(!ground.hasCol) {arenaAbort("destruction walking actor has no native floor");return;}
      start.y=ground.v.y;
      auto* actor=name=="walk-hero"?&player:world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),start);
      if(!actor) {arenaAbort("destruction walking actor unavailable");return;}
      actor->clearAiQueue();actor->clearState(false);actor->stopWalking();actor->closeWeapon(true);
      actor->setPosition(start);actor->setDirection(approach);actor->updateTransform();actor->setWalkMode(WalkBit::WM_Walk);
      if(actor!=&player) {actor->setExternalControl(false);actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        player.setPosition(start+side*160.f);player.updateTransform();}
      bench.destructionActor=actor;bench.destructionActorStart=start;
      bench.destructionDestination=WayPoint(source+approach*170.f,"FP_DESTRUCTION_CRATE_WALK");
      bench.destructionDestination.groundPos=source+approach*170.f;
      }
    bench.cam=C_Hold;lookFrom(source-direction*280.f+side*230.f+Vec3(0,190,0),source+Vec3(0,55,0));
    }
  else {arenaAbort("unknown destruction fixture case");return;}
  bench.destructionStart=world.tickCount();
  event("destruction_setup","\"case\":"+jsonStr(name)+",\"world\":"+jsonStr(world.name())+
    ",\"host\":"+jsonStr(symName(world,bench.noiseHost))+",\"enabled\":"+
    (world.physic()->destructionEnabled()?"true":"false")+",\"location\":"+
    jsonStr(name=="five"?"stock_outdoor_arena":name=="fireball"?"coragon_tavern":"coragon_area")+",\"bodies\":"+destructionRows(world)+
    ",\"spell_target_cm\":"+ragdollVector(bench.spellActors[0].npc->collosionCenter()));
  }

void destructionBegin(World& world) {
  bench.destructionTrace=true;bench.destructionStart=world.tickCount();
  bench.destructionSupportNext=1000;
  bench.destructionLastBreak=0;bench.destructionSleepSampled=false;
  bench.destructionFallSample=0;
  bench.destructionFragmentOrigins.clear();bench.destructionFragmentTravel.clear();
  if(bench.destructionCase=="arrows") {bench.destructionShooting=true;bench.destructionNextArrow=world.tickCount();}
  if(bench.destructionCase=="drop1" || bench.destructionCase=="drop5")
    for(auto& fixture:bench.destructionFixtures)fixture->body.addImpulse(Vec3(0,-.001f,0));
  if(bench.destructionCase=="walk-npc" && bench.destructionActor)
    bench.destructionActor->aiPush(AiQueue::aiGoToPoint(bench.destructionDestination));
  unsigned pins=0;
  for(size_t i=0;i<world.stuckArrowCount();++i) {uint32_t zenId;Matrix4x4 local;if(world.stuckArrowAttachment(i,zenId,local))++pins;}
  event("destruction_begin","\"case\":"+jsonStr(bench.destructionCase)+",\"bodies\":"+destructionRows(world)+
    ",\"world_pinned_arrows\":"+std::to_string(pins)+",\"spell_target_cm\":"+
    ragdollVector(bench.spellActors[0].npc->collosionCenter())+",\"spell_target\":"+
    jsonStr(symName(world,bench.spellActors[0].npc))+",\"spell_target_npc_id\":"+
    std::to_string(world.npcId(bench.spellActors[0].npc)));
  }

void destructionShoot(World& world,Npc& player) {
  if(!bench.destructionShooting || world.tickCount()<bench.destructionNextArrow)return;
  const auto all=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(all.begin(),all.end(),[](const auto& body){return destructionOwns(body.id);});
  if(found==all.end() || found->broken || bench.destructionArrows>=30) {bench.destructionShooting=false;return;}
  const auto* mesh=Resources::loadMesh(found->visual);if(!mesh) {arenaAbort("destruction arrow crate mesh missing");return;}
  const auto* bounds=mesh->bbox();
  auto center=(bounds[0]+bounds[1])*.5f;found->transform.project(center);
  auto top=Vec3((bounds[0].x+bounds[1].x)*.5f,bounds[1].y,(bounds[0].z+bounds[1].z)*.5f);found->transform.project(top);
  const auto from=top+Vec3(0,80,0);
  const auto hit=world.physic()->rayMovableVob(from,center);
  if(!hit || hit->visual!=found->visual || (movablePosition(hit->transform)-movablePosition(found->transform)).length()>1.f) {
    arenaAbort("destruction arrow has no exposed native path to its exact crate");return;
    }
  const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(symbol==size_t(-1)) {arenaAbort("destruction arrow instance unavailable");return;}
  Item arrow(world,symbol,Item::T_Inventory);
  auto& bullet=world.shootArrow(arrow,from,Vec3::normalize(center-from),DynamicWorld::bulletSpeed);
  bullet.setOrigin(&player);bullet.setPrecise(1.f);
  ++bench.destructionArrows;bench.destructionNextArrow=world.tickCount()+500;
  event("destruction_arrow","\"ordinal\":"+std::to_string(bench.destructionArrows)+",\"health_before\":"+
    ragdollNumber(found->health)+",\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(center));
  }

void destructionCheckpoint(World& world) {
  destructionFields(world);
  unsigned attached=0;
  for(size_t i=0;i<world.stuckArrowCount();++i) {
    uint32_t zenId;Matrix4x4 local;
    if(world.stuckArrowAttachment(i,zenId,local))++attached;
    }
  event("destruction_checkpoint","\"case\":"+jsonStr(bench.destructionCase)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-bench.destructionStart)+",\"arrows\":"+std::to_string(bench.destructionArrows)+
    ",\"bodies\":"+destructionRows(world)+",\"fragments\":"+destructionFragments(world)+
    ",\"listeners\":"+noiseListenerRows(world)+",\"world_pinned_arrows\":"+std::to_string(attached)+
    ",\"actor_start_cm\":"+ragdollVector(bench.destructionActorStart)+",\"actor_cm\":"+
    (bench.destructionActor?ragdollVector(bench.destructionActor->position()):"null"));
  bench.destructionShooting=false;
  }

std::string destructionFloorProbe(const Npc& npc,std::string_view label,const Vec3& point) {
  const auto root=npc.position();
  const Vec3 from(point.x,std::max(root.y+20.f,point.y+20.f),point.z);
  const Vec3 to(point.x,std::min(root.y,point.y)-600.f,point.z);
  const auto hit=npc.world().physic()->landRay(from,from.y-to.y);
  return "{\"label\":"+jsonStr(label)+",\"reference_cm\":"+ragdollVector(point)+
    ",\"ray_from_cm\":"+ragdollVector(from)+",\"ray_to_cm\":"+ragdollVector(to)+
    ",\"found\":"+(hit.hasCol?"true":"false")+
    ",\"point_cm\":"+(hit.hasCol?ragdollVector(hit.v):"null")+
    ",\"normal\":"+(hit.hasCol?ragdollVector(hit.n):"null")+
    ",\"material\":"+std::to_string(int(hit.mat))+
    ",\"sector\":"+(hit.sector?jsonStr(hit.sector):"null")+
    ",\"interactive\":"+(hit.vob?"true":"false")+
    ",\"vertical_delta_cm\":"+(hit.hasCol?ragdollNumber(point.y-hit.v.y):"null")+"}";
  }

void destructionSkinSample(const Npc& npc,const ProtoMesh& proto,const Matrix4x4& object,const Pose& pose) {
  auto [cache,inserted]=bench.destructionSkinGeometry.try_emplace(&proto);
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
  const auto* skeleton=npc.visualSkeleton();
  if(cache->second.empty() || !skeleton)return;
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
      if(vertex.weights[r]<=0.f || vertex.boneId[r]>=skeleton->nodes.size())continue;
      Vec3 p(vertex.pos[r][0],vertex.pos[r][1],vertex.pos[r][2]);
      npc.renderedBone(vertex.boneId[r]).project(p);
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
  for(const auto center:{std::pair("root",npc.position()),std::pair("skin_lowest",lowest)})
    for(const auto offset:{std::pair("+x",Vec3(30,0,0)),std::pair("-x",Vec3(-30,0,0)),
                           std::pair("+z",Vec3(0,0,30)),std::pair("-z",Vec3(0,0,-30))})
      probe(std::string(center.first)+offset.first,center.second+offset.second);
  probes+=']';
  bench.destructionSkinSample="{\"mesh\":"+jsonStr(proto.fname)+",\"indexed_vertices\":"+std::to_string(count)+
    ",\"object_matrix\":"+ragdollMatrix(object)+
    ",\"object_up\":"+ragdollVector(Vec3(object.at(1,0),object.at(1,1),object.at(1,2)))+
    ",\"raw_model_bones\":"+rawBones+
    ",\"lowest_world_cm\":"+ragdollVector(lowest)+",\"highest_world_cm\":"+ragdollVector(highest)+
    ",\"lowest_dominant_bone\":"+(lowestBone<skeleton->nodes.size()?jsonStr(skeleton->nodes[lowestBone].name):"null")+
    ",\"ray_from_cm\":"+ragdollVector(rayFrom)+",\"ray_to_cm\":"+ragdollVector(rayTo)+
    ",\"support_found\":"+(found?"true":"false")+
    ",\"support_kind\":"+jsonStr(kind)+",\"support_visual\":"+jsonStr(visual)+
    ",\"support_parent_id\":"+std::to_string(parent)+",\"support_piece\":"+
    (piece==size_t(-1)?"null":std::to_string(piece))+
    ",\"support_world_cm\":"+(found?ragdollVector(support):"null")+
    ",\"support_normal\":"+(found?ragdollVector(supportNormal):"null")+
    ",\"gap_cm\":"+(found?ragdollNumber(lowest.y-support.y):"null")+",\"floor_probes\":"+probes+"}";
  }

void destructionSupportSample(World& world) {
  if(bench.spellActors.empty() || bench.spellActors[0].npc==nullptr) {
    event("destruction_error","\"reason\":\"support probe has no native fireball target\"");
    return;
    }
  const auto& npc=*bench.spellActors[0].npc;
  bench.destructionSkinSample="null";
  bench.destructionSkinRequest=&npc;
  const bool drawable=TestBench::ragdollBodyDrawable(npc);
  bench.destructionSkinRequest=nullptr;
  const auto* rig=npc.ragdollBody()!=nullptr ? npc.ragdollBody() : npc.livingBody();
  std::vector<Matrix4x4> native;
  const bool nativePose=rig && rig->getPose(native);
  std::string bones="[";
  if(const auto* skeleton=npc.visualSkeleton())
    for(std::string_view name:{"BIP01 PELVIS","BIP01 SPINE2","BIP01 L THIGH","BIP01 L CALF","BIP01 L FOOT"}) {
      const auto bone=arrowBone(*skeleton,name);
      if(bone==size_t(-1))continue;
      if(bones.size()>1)bones+=',';
      bones+="{\"name\":"+jsonStr(name)+",\"bone\":"+std::to_string(bone)+
        ",\"render\":"+ragdollMatrix(npc.renderedBone(bone))+
        ",\"animation\":"+ragdollMatrix(npc.animationBone(bone));
      if(nativePose && bone<native.size())bones+=",\"native\":"+ragdollMatrix(native[bone]);
      bones+='}';
      }
  bones+=']';
  std::string contacts="[";
  size_t supports=0,invisible=0,unknown=0;
  float maximumGap=0;
  if(rig!=nullptr)
    for(const auto& contact:world.physic()->ragdollSupports(*rig,2.f)) {
      const bool support=contact.axis.y<-.25f;
      // Source-triangle contacts may penetrate the floor. Report their signed
      // separation, while hulls retain a distance that exposes empty cavities.
      const float visibleGap=contact.sourceTriangle!=uint32_t(-1) ?
          Vec3::dotProduct(contact.point1-contact.visiblePoint,contact.axis*-1.f) : contact.visibleDistance;
      if(support) {
        ++supports;
        if(!contact.visible)++invisible;
        if(contact.visibleDistance<0.f)++unknown;
        else maximumGap=std::max(maximumGap,visibleGap);
        }
      if(contacts.size()>1)contacts+=',';
      contacts+="{\"bone\":"+std::to_string(contact.bone)+
        ",\"part_body_id\":"+std::to_string(contact.partBodyId)+
        ",\"support_body_id\":"+std::to_string(contact.supportBodyId)+
        ",\"layer\":"+std::to_string(contact.category)+",\"kind\":"+jsonStr(contact.kind)+
        ",\"visual\":"+jsonStr(contact.visual)+",\"parent_id\":"+std::to_string(contact.parentId)+
        ",\"zen_id\":"+std::to_string(contact.zenId)+",\"piece\":"+
        (contact.piece==size_t(-1)?"null":std::to_string(contact.piece))+
        ",\"source_triangle\":"+(contact.sourceTriangle==uint32_t(-1)?"null":std::to_string(contact.sourceTriangle))+
        ",\"sleeping\":"+(contact.sleeping?"true":"false")+
        ",\"broken\":"+(contact.broken?"true":"false")+
        ",\"visible\":"+(contact.visible?"true":"false")+
        ",\"supports_weight\":"+(support?"true":"false")+
        ",\"depth_cm\":"+ragdollNumber(contact.depth)+
        ",\"normal\":"+ragdollVector(contact.axis*-1.f)+
        ",\"part_contact_cm\":"+ragdollVector(contact.point1)+
        ",\"support_contact_cm\":"+ragdollVector(contact.point2)+
        ",\"visible_distance_cm\":"+(contact.visibleDistance<0.f?"null":ragdollNumber(contact.visibleDistance))+
        ",\"visible_gap_cm\":"+(contact.visibleDistance<0.f?"null":ragdollNumber(visibleGap))+
        ",\"visible_penetration_cm\":"+(contact.visibleDistance<0.f?"null":ragdollNumber(std::max(0.f,-visibleGap)))+
        ",\"visible_point_cm\":"+(contact.visibleDistance<0.f?"null":ragdollVector(contact.visiblePoint))+"}";
      }
  contacts+=']';
  event("destruction_support_sample","\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.destructionStart)+
    ",\"actor\":"+jsonStr(symName(world,&npc))+",\"actor_id\":"+std::to_string(world.npcId(&npc))+
    ",\"body_drawable\":"+(drawable?"true":"false")+
    ",\"ragdoll\":"+(npc.ragdollBody()?"true":"false")+
    ",\"rig_kind\":"+jsonStr(npc.ragdollBody()?"ragdoll":npc.livingBody()?"living":"none")+
    ",\"kinematic\":"+(rig && rig->isKinematic()?"true":"false")+
    ",\"sleeping\":"+(rig && rig->isSleeping()?"true":"false")+
    ",\"reaction_weight\":"+ragdollNumber(rig?rig->reactionWeight():0.f)+
    ",\"ragdoll_enabled\":"+(Gothic::physicsFlags().ragdoll?"true":"false")+
    ",\"spell_physics_enabled\":"+(Gothic::physicsFlags().spellPhysics?"true":"false")+
    ",\"npc_character_virtual\":"+(Gothic::physicsFlags().npcCharacterVirtual?"true":"false")+
    ",\"cv_active\":"+(npc.characterVirtualActive()?"true":"false")+
    ",\"visual_root_cm\":"+ragdollVector(npc.characterVisualPosition())+
    ",\"external_control\":"+(npc.isExternalControl()?"true":"false")+
    ",\"process_policy\":"+std::to_string(int(npc.processPolicy()))+
    ",\"body_state\":"+std::to_string(int(npc.bodyState()))+
    ",\"state\":"+jsonStr(stateName(world,npc))+",\"state_time\":"+std::to_string(npc.stateTime())+
    ",\"quest_return_count\":"+std::to_string(npc.ragdollQuestReturnCount())+
    ",\"quest_return_armed\":"+(npc.ragdollQuestReturnState().armed?"true":"false")+
    ",\"quest_return_hazards\":"+std::to_string(npc.ragdollQuestReturnState().hazards)+
    ",\"animations\":"+jsonStr(npc.animNames())+
    ",\"hp\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
    ",\"dead\":"+(npc.isDead()?"true":"false")+
    ",\"down\":"+(npc.isDown()?"true":"false")+
    ",\"in_air\":"+(npc.isInAir()?"true":"false")+
    ",\"falling\":"+(npc.isFalling()?"true":"false")+
    ",\"standing\":"+(npc.isStanding()?"true":"false")+
    ",\"spell_velocity_cm_s\":"+ragdollVector(npc.spellVelocity())+
    ",\"bones\":"+bones+",\"rendered_skin\":"+bench.destructionSkinSample+
    ",\"position_cm\":"+ragdollVector(npc.position())+",\"supports\":"+std::to_string(supports)+
    ",\"invisible_supports\":"+std::to_string(invisible)+",\"unknown_supports\":"+std::to_string(unknown)+
    ",\"maximum_visible_gap_cm\":"+ragdollNumber(maximumGap)+",\"contacts\":"+contacts);
  }

bool destructionMeleeEquip(World& world,Npc& actor,std::string_view weapon) {
  actor.clearAiQueue();actor.clearState(false);actor.clearGoTo();actor.stopWalking();actor.closeWeapon(true);
  actor.setTarget(nullptr);actor.setExternalControl(true);
  actor.handle().attribute[ATR_STRENGTH]=100;
  actor.setTalentSkill(TALENT_1H,2);actor.setTalentSkill(TALENT_2H,2);
  if(weapon=="fist")actor.setToFistMode();
  else {
    const auto symbol=world.script().findSymbolIndex(weapon=="1h"?"ITMW_1H_COMMON_01":"ITMW_2H_BAU_AXE");
    if(symbol==size_t(-1)) {arenaAbort("destruction melee stock weapon symbol absent");return false;}
    actor.setToFightMode(symbol);
    const auto* equipped=actor.currentMeleeWeapon();
    if(!equipped || equipped->clsId()!=symbol || equipped->is2H()!=(weapon=="2h")) {
      arenaAbort("destruction melee stock weapon did not equip");return false;
      }
    }
  return true;
  }

bool destructionMeleeProbe(World& world,Npc& actor,const DynamicWorld::MovableVobSnapshot& body,std::string_view stage) {
  const auto& guild=world.script().guildVal();const auto gl=actor.guild();
  const bool fist=actor.weaponState()==WeaponState::Fist;
  const auto* weapon=fist?nullptr:actor.currentMeleeWeapon();
  const float reach=float(guild.fight_range_base[gl]+(fist?guild.fight_range_fist[gl]:
    weapon && weapon->is2H()?guild.fight_range_2ha[gl]:guild.fight_range_1ha[gl]))+float(weapon?weapon->swordLength():0);
  const auto origin=actor.collosionCenter();
  unsigned contacts=0;float closest=std::numeric_limits<float>::infinity();Vec3 point;
  for(int yaw=-(fist?20:60);yaw<=(fist?20:60);yaw+=10)
    for(int pitch=-40;pitch<=20;pitch+=20) {
      const float a=actor.rotationRad()+float(yaw)*float(M_PI)/180.f,p=float(pitch)*float(M_PI)/180.f;
      const Vec3 direction(std::cos(a)*std::cos(p),std::sin(p),std::sin(a)*std::cos(p));
      const auto end=origin+direction*reach;
      const auto target=world.physic()->rayMovableVob(origin,end);
      if(!target || target->visual!=body.visual || target->zenId!=body.zenId ||
         (movablePosition(target->transform)-movablePosition(body.transform)).length()>1.f)continue;
      const auto hit=world.physic()->ray(origin,end);
      if(!hit.hasCol)continue;
      ++contacts;const float distance=(hit.v-origin).length();
      if(distance<closest) {closest=distance;point=hit.v;}
      }
  auto local=point;auto inverse=body.transform;inverse.inverse();inverse.project(local);
  event("destruction2_melee_probe","\"case\":"+jsonStr(bench.destructionCase)+",\"stage\":"+jsonStr(stage)+
    ",\"actor_cm\":"+ragdollVector(actor.position())+",\"origin_cm\":"+ragdollVector(origin)+
    ",\"reach_cm\":"+ragdollNumber(reach)+",\"contact_rays\":"+std::to_string(contacts)+
    ",\"closest_contact_cm\":"+(contacts?ragdollNumber(closest):"null")+
    ",\"contact_point_cm\":"+(contacts?ragdollVector(point):"null")+
    ",\"contact_local_cm\":"+(contacts?ragdollVector(local):"null"));
  return contacts>0;
  }

void destructionMeleeSetup(World& world,Npc& player,std::string_view kind,std::string_view weapon,bool npc) {
  if((kind!="crate" && kind!="barrel" && kind!="fence") || (weapon!="fist" && weapon!="1h" && weapon!="2h")) {
    arenaAbort("destruction melee expects crate|barrel|fence and fist|1h|2h");return;
    }
  bench.destructionMeleeRunning=false;bench.destructionTrace=false;
  bench.destructionFixtures.clear();bench.destructionIds.clear();
  if(bench.destructionMeleeNpc) {
    world.removeNpc(*bench.destructionMeleeNpc);bench.destructionMeleeNpc=nullptr;
    }
  if(!bench.destructionMeleeArena) {
    if(const auto* anchor=world.findPoint("NW_XARDAS_TOWER_VALLEY_091",false)) {
      player.setPosition(anchor->pos);player.updateTransform();
      }
    if(!chooseRagdollArena(world,player)) {arenaAbort("destruction melee needs native flat outdoor ground");return;}
    bench.destructionMeleeArena=true;
    }
  const auto source=bench.ragdollArenaTarget-Vec3(0,80,0);
  std::string visual=kind=="barrel"?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS";
  if(kind=="fence") {
    visual.clear();
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(body.className=="fence") {visual=body.visual;break;}
    if(visual.empty()) {arenaAbort("destruction melee stock fence mesh is absent");return;}
    }
  if(!destructionAdd(world,visual,source)) {arenaAbort("destruction melee native mesh insertion failed");return;}
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(bodies.begin(),bodies.end(),[](const auto& body){return destructionOwns(body.id);});
  if(found==bodies.end() || !found->destructible || found->broken) {
    arenaAbort("destruction melee needs intact cache-backed native prop");return;
    }
  auto center=(found->localBounds[0]+found->localBounds[1])*.5f;found->transform.project(center);
  const float extent=(found->localBounds[1].z-found->localBounds[0].z)*.5f;
  auto stand=center-Vec3(0,0,extent+75.f);
  // This upright authored post spans the full strike height. The middle of
  // NW_NATURE_FENCE_01 is a gap between two narrow horizontal rails.
  if(kind=="fence")stand.x+=100.f;
  const auto floor=world.physic()->landRay(Vec3(stand.x,source.y+80,stand.z),180);
  if(!floor.hasCol) {arenaAbort("destruction melee actor has no native floor");return;}
  stand.y=floor.v.y;
  // The arena is an isolated acceptance fixture. It never changes map-vob IDs.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* other=world.npcById(id);other && other!=&player && (other->position()-source).length()<2300.f) {
      other->clearAiQueue();other->clearState(false);other->stopWalking();other->setExternalControl(true);
      }
  auto* actor=&player;
  if(npc) {
    actor=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),stand);
    if(!actor) {arenaAbort("destruction melee NPC actor insertion failed");return;}
    bench.destructionMeleeNpc=actor;
    }
  if(!destructionMeleeEquip(world,*actor,weapon))return;
  actor->setPosition(stand);actor->setDirection(Vec3(0,0,1));actor->updateTransform();
  if(npc) {player.closeWeapon(true);player.setPosition(stand+Vec3(200,0,-100));player.updateTransform();}
  bench.destructionMeleeActor=actor;bench.destructionMeleeFacing=Vec3(0,0,1);
  bench.destructionMeleeStand=stand;bench.destructionMeleeRestance=kind=="fence";
  bench.destructionMeleeAttempts=bench.destructionMeleeHits=0;
  bench.destructionCase=std::string(kind)+"-"+std::string(weapon)+(npc?"-npc":"");
  bench.destructionStart=world.tickCount();bench.destructionTrace=true;
  if(!destructionMeleeProbe(world,*actor,*found,"setup")) {
    arenaAbort("destruction melee stance has no real wood contact inside native attack reach");return;
    }
  bench.cam=C_Hold;lookFrom(center+Vec3(250,230,-310),center+Vec3(0,35,0));
  const auto* item=weapon=="fist"?nullptr:actor->currentMeleeWeapon();
  event("destruction2_melee_setup","\"case\":"+jsonStr(bench.destructionCase)+",\"class\":"+jsonStr(kind)+
    ",\"weapon\":"+jsonStr(weapon)+",\"actor\":"+jsonStr(npc?"npc":"hero")+",\"strength\":100,\"weapon_damage\":"+
    std::to_string(item?item->handle().damage_total:0)+",\"weapon_range_cm\":"+std::to_string(item?item->swordLength():0)+
    ",\"actor_cm\":"+ragdollVector(stand)+",\"bodies\":"+destructionRows(world));
  }

void destructionMeleeTick(World& world) {
  if(!bench.destructionMeleeRunning || !bench.destructionMeleeActor)return;
  if(bench.destructionMeleeBlows>0 && bench.destructionMeleeAttempts>=bench.destructionMeleeBlows) {
    if(world.tickCount()>=bench.destructionMeleeNext) {bench.destructionMeleeRunning=false;bench.waitUntil=0;}
    return;
    }
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(bodies.begin(),bodies.end(),[](const auto& body){return destructionOwns(body.id);});
  if(found==bodies.end() || world.tickCount()>=bench.destructionMeleeDeadline) {
    bench.destructionMeleeRunning=false;bench.waitUntil=0;
    arenaAbort("destruction melee timed out before native animation broke its prop");return;
    }
  if(found->broken) {
    bench.destructionMeleeRunning=false;bench.waitUntil=bench.time+1000;
    event("destruction2_melee_complete","\"case\":"+jsonStr(bench.destructionCase)+",\"attempts\":"+
      std::to_string(bench.destructionMeleeAttempts)+",\"hits\":"+std::to_string(bench.destructionMeleeHits)+
      ",\"bodies\":"+destructionRows(world)+",\"fragments\":"+destructionFragments(world));return;
    }
  if(world.tickCount()<bench.destructionMeleeNext)return;
  auto& actor=*bench.destructionMeleeActor;
  // Each fence sample starts outside its post. Forward fist root motion would
  // otherwise carry repeated attacks into the open rail gap after two swings.
  // The animation and its impact run freely between these setup boundaries.
  if(bench.destructionMeleeRestance) {
    actor.stopWalking();actor.clearSpeed();actor.setPosition(bench.destructionMeleeStand);
    actor.setDirection(bench.destructionMeleeFacing);actor.updateTransform();
    if(!destructionMeleeProbe(world,actor,*found,"before-animation")) {
      bench.destructionMeleeRunning=false;arenaAbort("destruction melee reset stance lost native wood contact");return;
      }
    }
  const bool accepted=actor.doAttack(Npc::Anim::Attack,BS_HIT);
  if(accepted)++bench.destructionMeleeAttempts;
  bench.destructionMeleeNext=world.tickCount()+(accepted?2200:100);
  event("destruction2_melee_attempt","\"case\":"+jsonStr(bench.destructionCase)+",\"accepted\":"+
    (accepted?"true":"false")+",\"ordinal\":"+std::to_string(bench.destructionMeleeAttempts)+
    ",\"actor_cm\":"+ragdollVector(actor.position())+",\"bodies\":"+destructionRows(world));
  }

std::string destructionRoutineRows(const Npc& npc) {
  std::string result="[";
  npc.forEachRoutine([&](const Npc::RoutineInfo& routine) {
    if(result.size()>1)result+=',';
    result+="{\"start\":"+std::to_string(routine.start.toInt())+",\"end\":"+std::to_string(routine.end.toInt())+
      ",\"state\":"+std::to_string(routine.state.ptr)+",\"point\":"+jsonStr(routine.point)+"}";
    });
  return result+"]";
  }

void destructionFarmSetup(World& world,Npc& player,uint32_t zenId,bool npc=false) {
  bench.destructionMeleeRunning=false;bench.destructionTrace=false;
  bench.destructionFixtures.clear();bench.destructionIds.clear();
  if(bench.destructionMeleeNpc) {
    world.removeNpc(*bench.destructionMeleeNpc);bench.destructionMeleeNpc=nullptr;
    }
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body){return body.zenId==zenId;});
  if(found==bodies.end() || found->className!="fence" || !found->destructible || found->broken) {
    arenaAbort("destruction farm needs an intact cache-backed stock map fence");return;
    }
  auto center=(found->localBounds[0]+found->localBounds[1])*.5f;found->transform.project(center);
  const bool thinZ=found->localBounds[1].z-found->localBounds[0].z<found->localBounds[1].x-found->localBounds[0].x;
  auto axis=(found->localBounds[0]+found->localBounds[1])*.5f+(thinZ?Vec3(0,0,1):Vec3(1,0,0));
  found->transform.project(axis);auto normal=axis-center;normal.y=0;normal=Vec3::normalize(normal);
  const float depth=(thinZ?found->localBounds[1].z-found->localBounds[0].z:found->localBounds[1].x-found->localBounds[0].x)*.5f;
  bool supported=false;Vec3 start,end;
  for(const auto sign:{1.f,-1.f}) {
    start=center-normal*(depth+75.f)*sign;end=center+normal*(depth+180.f)*sign;
    const auto first=world.physic()->landRay(start+Vec3(0,80,0),350);
    const auto last=world.physic()->landRay(end+Vec3(0,80,0),350);
    if(!first.hasCol || !last.hasCol || std::abs(first.v.y-last.v.y)>100.f)continue;
    start.y=first.v.y;end.y=last.v.y;normal*=sign;supported=true;break;
    }
  if(!supported) {arenaAbort("destruction farm fence lacks native walkable ground on both sides");return;}
  auto* actor=&player;
  if(npc) {
    actor=world.findNpcByInstance(world.script().findSymbolIndex("BAU_952_VINO"));
    if(!actor) {arenaAbort("destruction farm NPC actor Vino is absent");return;}
    }
  if(!destructionMeleeEquip(world,*actor,"1h"))return;
  actor->setPosition(start);actor->setDirection(normal);actor->updateTransform();
  if(npc) {player.closeWeapon(true);player.setPosition(start-normal*800.f);player.updateTransform();}
  bench.destructionIds.push_back(found->id);bench.destructionMeleeActor=actor;
  bench.destructionMeleeRestance=false;
  bench.destructionMeleeFacing=normal;
  bench.destructionMeleeAttempts=bench.destructionMeleeHits=0;
  bench.destructionFarmZen=zenId;bench.destructionFarmStart=start;bench.destructionFarmEnd=end;
  bench.destructionCase=npc?"farm-fence-1h-npc":"farm-fence-1h";
  bench.destructionTrace=true;bench.destructionStart=world.tickCount();
  auto* maleth=world.findNpcByInstance(world.script().findSymbolIndex("BAU_954_MALETH"));
  if(!maleth) {arenaAbort("destruction farm routine NPC Maleth is absent");return;}
  bench.destructionFarmNpc=maleth;bench.destructionFarmNpcStart=maleth->position();bench.destructionFarmNpcTravel=0;
  const auto side=Vec3(-normal.z,0,normal.x);
  bench.cam=C_Hold;lookFrom(center-normal*310.f+side*290.f+Vec3(0,170,0),center+Vec3(0,-10,0));
  event("destruction2_farm_setup","\"zen_id\":"+std::to_string(zenId)+",\"visual\":"+jsonStr(found->visual)+
    ",\"from_map\":true,\"start_cm\":"+ragdollVector(start)+",\"goal_cm\":"+ragdollVector(end)+
    ",\"actor\":"+jsonStr(symName(world,actor))+
    ",\"intact_passable\":"+(player.testMove(end)?"true":"false")+",\"npc\":"+jsonStr(symName(world,maleth))+
    ",\"npc_cm\":"+ragdollVector(maleth->position())+",\"npc_routines\":"+destructionRoutineRows(*maleth)+
    ",\"bodies\":"+destructionRows(world));
  }

void destructionFarmSetupNext(World& world,Npc& player) {
  const auto bodies=world.physic()->movableVobSnapshot();
  const DynamicWorld::MovableVobSnapshot* nearest=nullptr;
  float distance=1800.f;
  for(const auto& body:bodies) {
    if(body.zenId==uint32_t(-1) || body.className!="fence" || !body.destructible || body.broken)continue;
    const float cm=(movablePosition(body.transform)-bench.destructionFarmStart).length();
    if(cm<distance || (nearest && cm==distance && body.zenId<nearest->zenId)) {nearest=&body;distance=cm;}
    }
  if(!nearest) {arenaAbort("destruction farm needs another intact native fence within 18 m");return;}
  destructionFarmSetup(world,player,nearest->zenId);
  }

void destructionFarmNpcWalk(World& world,std::string_view phase) {
  auto* npc=bench.destructionFarmNpc;
  if(!npc) {arenaAbort("destruction farm NPC route requires farm setup");return;}
  const auto* start=world.findWayPoint("NW_FARM1_FIELD_08");
  if(!start) {arenaAbort("destruction farm authored route start absent");return;}
  const WayPoint* goal=nullptr;
  for(const auto& conn:start->connections())
    if(conn.point && !conn.point->underWater && (conn.point->pos-start->pos).length()>=250.f &&
       (conn.point->pos-start->pos).length()<=1400.f) {goal=conn.point;break;}
  if(!goal) {arenaAbort("destruction farm authored nearby route has no connected endpoint");return;}
  if(auto* player=world.player())player->closeWeapon(true);
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearGoTo();npc->stopWalking();npc->closeWeapon(true);
  npc->setExternalControl(false);npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Walk);
  npc->setPosition(start->pos);npc->setDirection(goal->pos-start->pos);npc->updateTransform();
  bench.destructionFarmNpcStart=npc->position();bench.destructionFarmNpcTravel=0;bench.destructionFarmNpcGoal=goal;
  bench.destructionFarmNpcBest=(npc->position()-goal->pos).length();
  npc->aiPush(AiQueue::aiGoToPoint(*goal));
  event("destruction2_farm_npc_begin","\"phase\":"+jsonStr(phase)+",\"npc\":"+jsonStr(symName(world,npc))+",\"start_waypoint\":"+jsonStr(start->name)+
    ",\"goal_waypoint\":"+jsonStr(goal->name)+",\"start_cm\":"+ragdollVector(npc->position())+
    ",\"goal_cm\":"+ragdollVector(goal->pos)+",\"routines\":"+destructionRoutineRows(*npc));
  }

void destructionFarmCheckpoint(World& world,Npc& player,std::string_view phase) {
  auto* npc=bench.destructionFarmNpc;
  event("destruction2_farm_checkpoint","\"phase\":"+jsonStr(phase)+",\"zen_id\":"+std::to_string(bench.destructionFarmZen)+
    ",\"start_cm\":"+ragdollVector(bench.destructionFarmStart)+",\"goal_cm\":"+ragdollVector(bench.destructionFarmEnd)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"broken_passable\":"+(player.testMove(bench.destructionFarmStart)?"true":"false")+
    ",\"npc_cm\":"+(npc?ragdollVector(npc->position()):"null")+",\"npc_travel_cm\":"+ragdollNumber(bench.destructionFarmNpcTravel)+
    ",\"npc_goal_distance_cm\":"+(npc && bench.destructionFarmNpcGoal?ragdollNumber((npc->position()-bench.destructionFarmNpcGoal->pos).length()):"null")+
    ",\"npc_best_goal_distance_cm\":"+(std::isfinite(bench.destructionFarmNpcBest)?ragdollNumber(bench.destructionFarmNpcBest):"null")+
    ",\"npc_routines\":"+(npc?destructionRoutineRows(*npc):"[]")+",\"bodies\":"+destructionRows(world)+",\"fragments\":"+destructionFragments(world));
  }

void destructionSessionSnapshot(World& world,std::string_view label) {
  std::string bodies="[",fragments="[";
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.zenId==uint32_t(-1) || (body.className!="crate" && body.className!="barrel" && body.className!="fence"))continue;
    if(bodies.size()>1)bodies+=',';
    bodies+="{\"zen_id\":"+std::to_string(body.zenId)+",\"class\":"+jsonStr(body.className)+",\"visual\":"+
      jsonStr(body.visual)+",\"broken\":"+(body.broken?"true":"false")+",\"health\":"+ragdollNumber(body.health)+
      ",\"max_health\":"+ragdollNumber(body.maxHealth)+",\"transform\":"+ragdollMatrix(body.transform)+"}";
    }
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(part.zenId==uint32_t(-1))continue;
    if(fragments.size()>1)fragments+=',';
    fragments+="{\"zen_id\":"+std::to_string(part.zenId)+",\"piece\":"+std::to_string(part.piece)+
      ",\"sleeping\":"+(part.sleeping?"true":"false")+",\"permanent\":"+(part.permanent?"true":"false")+
      ",\"asleep_since_min\":"+std::to_string(part.asleepSince)+",\"transform\":"+ragdollMatrix(part.transform)+
      ",\"position_cm\":"+ragdollVector(movablePosition(part.transform))+",\"velocity_cm_s\":"+
      ragdollVector(part.velocity)+",\"angular_velocity\":"+ragdollVector(part.angularVelocity)+"}";
    }
  event("destruction2_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"game_minutes\":"+std::to_string(world.time().toInt()/60000)+",\"enabled\":"+
    (world.physic()->destructionEnabled()?"true":"false")+",\"bodies\":"+bodies+"],\"fragments\":"+fragments+"]");
  }

void destructionAge(World& world,int64_t minutes,bool youngest) {
  int64_t asleep=-1;
  for(const auto& part:world.physic()->fragmentSnapshot())
    if(part.zenId!=uint32_t(-1) && !part.permanent && part.sleeping && part.asleepSince>=0)
      asleep=asleep<0?part.asleepSince:youngest?std::max(asleep,part.asleepSince):std::min(asleep,part.asleepSince);
  if(asleep<0 || minutes<0) {arenaAbort("destruction age requires sleeping map debris and nonnegative minutes");return;}
  Gothic::inst().gameSession()->setTime(gtime(int64_t(0),int64_t(0),asleep+minutes));
  event("destruction2_age","\"anchor\":"+jsonStr(youngest?"youngest":"oldest")+
    ",\"asleep_since_min\":"+std::to_string(asleep)+",\"age_min\":"+std::to_string(minutes)+
    ",\"game_minutes\":"+std::to_string(world.time().toInt()/60000));
  }

void movableSaveSnapshot(World& world,std::string_view label) {
  std::string npcPins="[",worldPins="[";
  for(uint32_t id=0;id<world.npcCount();++id) {
    const auto* npc=world.npcById(id);if(!npc)continue;
    for(size_t i=0;i<npc->stuckArrowCount();++i) {
      Npc::ArrowPinProbe pin;
      if(!npc->stuckArrowProbe(i,pin)) {arenaAbort("saved NPC arrow lacks a rendered node");return;}
      if(npcPins.size()>1)npcPins+=',';
      npcPins+="{\"npc_id\":"+std::to_string(world.npcId(npc))+",\"instance\":"+jsonStr(symName(world,npc))+
        ",\"index\":"+std::to_string(i)+",\"bone\":"+std::to_string(pin.bone)+
        ",\"local\":"+ragdollMatrix(pin.local)+",\"world\":"+ragdollMatrix(pin.world)+
        ",\"expected\":"+ragdollMatrix(npc->renderedBone(pin.bone)*pin.local)+"}";
      }
    }
  for(size_t i=0;i<world.stuckArrowCount();++i) {
    Matrix4x4 matrix,local;uint32_t zenId=uint32_t(-1);
    if(!world.stuckArrowTransform(i,matrix)) {arenaAbort("saved world arrow lacks a rendered node");return;}
    const bool attached=world.stuckArrowAttachment(i,zenId,local);
    if(worldPins.size()>1)worldPins+=',';
    worldPins+="{\"index\":"+std::to_string(i)+",\"zen_id\":"+(attached?std::to_string(zenId):"null")+
      ",\"world\":"+ragdollMatrix(matrix)+",\"local\":"+(attached?ragdollMatrix(local):"null")+"}";
    }
  event("movable_save_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"movable_vobs\":"+(Gothic::physicsFlags().movableVobs?"true":"false")+
    ",\"arrow_sticking\":"+(Gothic::physicsFlags().arrowSticking?"true":"false")+
    ",\"bodies\":"+movableRows(world,true)+",\"npc_pins\":"+npcPins+"],\"world_pins\":"+worldPins+"]");
  }

void movablePin(World& world,Npc& player,std::string_view target) {
  const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(symbol==size_t(-1)) {arenaAbort("movable save fixture arrow instance missing");return;}
  Item arrow(world,symbol,Item::T_Inventory);
  Vec3 from,to;
  const auto npcHit=[&](Npc& npc,const Vec3& start,const Vec3& end,float& distance) {
    return &npc!=&player && npc.rayHitZone(start,end,distance)!=HitZones::None;
    };
  if(target=="table") {
    bool found=false;
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(body.id!=bench.movableTable)continue;
      const auto* mesh=Resources::loadMesh(body.visual);
      if(!mesh)break;
      const auto* bounds=mesh->bbox();
      const float infinity=std::numeric_limits<float>::infinity();
      Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
      for(unsigned corner=0;corner<8;++corner) {
        Vec3 point(bounds[corner&1].x,bounds[(corner>>1)&1].y,bounds[(corner>>2)&1].z);
        body.transform.project(point);
        low.x=std::min(low.x,point.x);low.y=std::min(low.y,point.y);low.z=std::min(low.z,point.z);
        high.x=std::max(high.x,point.x);high.y=std::max(high.y,point.y);high.z=std::max(high.z,point.z);
        }
      // The fireball leaves test dishes on and around the table. Select an
      // exposed part of this exact map body, using the production visual query.
      for(float x:{.5f,.2f,.8f,.35f,.65f,.1f,.9f}) {
        for(float z:{.5f,.2f,.8f,.35f,.65f,.1f,.9f}) {
          const Vec3 start(low.x+(high.x-low.x)*x,high.y+80.f,low.z+(high.z-low.z)*z);
          const Vec3 end(start.x,low.y-10.f,start.z);
          const auto hit=world.physic()->rayMovableVob(start,end);
          if(!hit || hit->zenId!=body.zenId || world.physic()->rayArrow(start,end,npcHit).npcHit)continue;
          from=start;to=end;found=true;break;
          }
        if(found)break;
        }
      break;
      }
    if(!found) {arenaAbort("movable save table has no exposed native shot path");return;}
    }
  else if(target=="npc") {
    if(bench.spellActors.empty()) {arenaAbort("movable save NPC is absent");return;}
    auto& npc=*bench.spellActors[0].npc;
    if(npc.isDown()) {arenaAbort("movable save arrow needs its living NPC target");return;}
    const auto* skeleton=npc.visualSkeleton();
    const auto bone=skeleton?arrowBone(*skeleton,"SPINE2"):size_t(-1);
    if(bone==size_t(-1)) {arenaAbort("movable save NPC has no chest bone");return;}
    const auto chest=movablePosition(npc.renderedBone(bone));
    bool found=false;
    for(const auto direction:{Vec3(0,1,0),Vec3(1,0,0),Vec3(-1,0,0),Vec3(0,0,1),Vec3(0,0,-1)}) {
      const auto start=chest+direction*100.f,end=chest-direction*50.f;
      if(world.physic()->rayArrow(start,end,npcHit).npcHit!=&npc)continue;
      from=start;to=end;found=true;break;
      }
    if(!found) {arenaAbort("movable save NPC has no exposed precise shot path");return;}
    }
  else if(target=="world") {
    const auto point=bench.movableWalkStart;
    const auto floor=world.physic()->landRay(point+Vec3(0,80,0),180);
    if(!floor.hasCol) {arenaAbort("movable save static arrow has no floor");return;}
    to=floor.v;from=to+Vec3(0,60,0);
    }
  else {arenaAbort("unknown movable save arrow target");return;}
  auto& bullet=world.shootArrow(arrow,from,Vec3::normalize(to-from),DynamicWorld::bulletSpeed);
  bullet.setOrigin(&player);bullet.setPrecise(1.f);
  event("movable_save_shot","\"target\":"+jsonStr(target)+",\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(to));
  }

void pushMovablePinnedTable(World& world) {
  uint32_t zenId=uint32_t(-1);Matrix4x4 local;
  for(size_t i=0;i<world.stuckArrowCount();++i)
    if(world.stuckArrowAttachment(i,zenId,local))break;
  if(zenId==uint32_t(-1)) {arenaAbort("movable push needs an arrow attached to map furniture");return;}
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.zenId!=zenId)continue;
    const auto* mesh=Resources::loadMesh(body.visual);
    if(!mesh)break;
    const auto* bounds=mesh->bbox();auto center=(bounds[0]+bounds[1])*.5f;body.transform.project(center);
    const auto pushed=world.physic()->applyMovableVobImpulse(center,180.f,
      [](const Vec3&,float mass) {return Vec3(.9f,.3f,.45f)*mass;});
    event("movable_save_push","\"zen_id\":"+std::to_string(zenId)+",\"affected\":"+std::to_string(pushed));
    if(!pushed)arenaAbort("movable saved table impulse was blocked");
    return;
    }
  arenaAbort("movable pinned map furniture is absent");
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
      if(hit.mat==zenkit::MaterialGroup::EARTH && hit.n.y>.7f)return hit.v.y;
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
          if(hit.mat!=zenkit::MaterialGroup::EARTH && !hit.vob && (hit.v-target).length()<=20.f) {
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
      if(hit.mat!=zenkit::MaterialGroup::EARTH && !hit.vob && (hit.v-target).length()<=20.f)
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
      if(!entry || entry->type()!=zenkit::DaedalusDataType::INSTANCE)continue;
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

void saveMovableSession(World& world,const std::string& label) {
  if(label.empty() || label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
    throw std::runtime_error("invalid movable save label");
  std::vector<uint8_t> bytes;
  {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
  Gothic::inst().gameSession()->save(out,"movable "+label,preview);}
  std::ofstream file(outDir+"/session-"+label+".sav",std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file)throw std::runtime_error("cannot write complete movable GameSession archive");
  movableSaveSnapshot(world,"saved");
  event("movable_session_save","\"archive\":"+jsonStr("session-"+label+".sav")+",\"bytes\":"+
    std::to_string(bytes.size())+",\"world\":"+jsonStr(world.name())+",\"bodies\":"+movableRows(world));
  }

bool waterOwns(const DynamicWorld::WaterBodySnapshot& body) {
  if(body.ragdoll)
    return std::any_of(bench.waterActors.begin(),bench.waterActors.end(),[&](const auto& actor) {
      return actor.npc->ragdollBody()==body.ragdoll;
      });
  return bench.waterIds.contains(body.id) || bench.waterIds.contains(body.parentId);
  }

void waterFields(World& world) {
  if(!bench.waterTrace || bench.movableMeasuring || bench.waterSample==world.tickCount())return;
  bench.waterSample=world.tickCount();
  if(bench.waterKillOnThrow && !bench.waterActors.empty()) {
    auto* npc=bench.waterActors.front().npc;
    const auto velocity=npc->spellVelocity();
    const auto displacement=(npc->position()-bench.questWater.dry).length();
    if(!npc->isDown() && velocity.length()>100.f && displacement>50.f) {
      // Let the real projectile hit a living target first. Death during its
      // flight inherits ordinary live motion through the production birth path.
      bench.waterKillOnThrow=false;
      const auto at=npc->position();npc->setLastHitBy(world.player());
      if(!npc->setRagdollDown(true)) {arenaAbort("water corpse death during real wind flight refused");return;}
      npc->prepareRagdoll();
      const auto* rig=npc->ragdollBody();
      if(!rig) {arenaAbort("water corpse wind death created no native ragdoll");return;}
      const auto state=rig->state();
      event("water_bodies_death","\"actor_id\":"+std::to_string(world.npcId(npc))+
        ",\"position_cm\":"+ragdollVector(at)+",\"observed_wind_velocity_cm_s\":"+ragdollVector(velocity)+
        ",\"displacement_from_lip_cm\":"+ragdollNumber(displacement)+
        ",\"birth_velocity_cm_s\":"+(state.parts.empty()?"null":ragdollVector(state.parts.front().linearVelocity))+
        ",\"path\":\"production onNoHealth death during genuine live WindFist flight\"");
      }
    }
  for(auto& actor:bench.waterActors)
    if(const auto* rig=actor.npc->ragdollBody();rig && rig->isSleeping() && actor.sleepAt==uint64_t(-1)) {
      const auto pelvis=movablePosition(rig->pelvisTransform());
      const auto water=world.physic()->waterRay(pelvis,300.f);
      if(water.hasCol && pelvis.y<water.wdepth+50.f)actor.sleepAt=world.tickCount()-bench.waterStart;
      }
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(!waterOwns(body))continue;
    const auto point=movablePosition(body.transform);
    const auto [origin,inserted]=bench.waterReference.emplace(body.id,point);
    bench.waterTravel[body.id]=std::max(bench.waterTravel[body.id],(point-origin->second).length());
    // A newly inserted body may initially be inactive above the water. Count
    // only the first sleep after it has reached the real water or its bottom.
    if(body.sleeping && body.inWater && !bench.waterSleepAt.contains(body.id))
      bench.waterSleepAt.emplace(body.id,world.tickCount()-bench.waterStart);
    }
  if(bench.waterObserver && world.player() && world.tickCount()>=bench.waterObserverSample) {
    const auto& hero=*world.player();
    const auto floor=world.physic()->landRay(hero.position()+Vec3(0,180,0),1000.f);
    event("water_observer_sample","\"hero_cm\":"+ragdollVector(hero.position())+
      ",\"displacement_cm\":"+ragdollNumber((hero.position()-bench.waterObserverStart).length())+
      ",\"floor_found\":"+(floor.hasCol?"true":"false")+",\"floor_normal\":"+ragdollVector(floor.n)+
      ",\"slide\":"+(hero.isSlide()?"true":"false")+",\"falling\":"+(hero.isFalling()?"true":"false")+
      ",\"root_move_cm_per_33ms\":"+ragdollVector(hero.animMoveSpeed(33)));
    bench.waterObserverSample=world.tickCount()+1000;
    }
  if(bench.waterSwim && world.player()) {
    const auto point=world.player()->position();
    bench.waterHeroMaxStep=std::max(bench.waterHeroMaxStep,(point-bench.waterHeroLast).length());
    bench.waterHeroLast=point;
    bench.waterSawSwim|=world.player()->isSwim();
    }
  }

std::string waterRows(World& world) {
  std::string rows="[";
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(!waterOwns(body))continue;
    if(rows.size()>1)rows+=',';
    auto label=bench.waterLabels[body.id];
    if(label.empty() && body.category!=DynamicWorld::C_Fragment)label=bench.waterLabels[body.parentId];
    if(label.empty())label=body.category==DynamicWorld::C_Fragment?"crate_fragment":body.visual;
    std::string actorFields;
    if(body.ragdoll) {
      for(const auto& actor:bench.waterActors) {
        if(actor.npc->ragdollBody()!=body.ragdoll)continue;
        const auto* skeleton=actor.npc->visualSkeleton();
        const auto bone=skeleton && body.bone<skeleton->nodes.size()?skeleton->nodes[body.bone].name:std::string();
        label=actor.role+":"+bone;
        actorFields=",\"actor_id\":"+std::to_string(world.npcId(actor.npc))+
          ",\"role\":"+jsonStr(actor.role)+",\"bone\":"+std::to_string(body.bone)+
          ",\"bone_name\":"+jsonStr(bone)+",\"target_fraction\":"+ragdollNumber(body.targetFraction);
        break;
        }
      }
    const auto point=movablePosition(body.transform);
    const bool hasWater=body.inWater && std::isfinite(body.waterY);
    const auto floor=world.physic()->landRay(Vec3(point.x,hasWater?body.waterY+3.f:point.y,point.z),6000.f);
    const auto asleep=bench.waterSleepAt.find(body.id);
    std::string rendered="null";
    std::string itemFields;
    if(body.category==DynamicWorld::C_Item)
      world.detectItem(point,.1f,[&](Item& item) {
        if(std::abs(item.physicsMass()-body.mass)>.001f)return;
        itemFields=",\"item_instance\":"+std::to_string(item.clsId())+
          ",\"torch_burning\":"+(item.isTorchBurn()?"true":"false");
        });
    if(body.category==DynamicWorld::C_Item)
      if(const auto* mesh=Resources::loadMesh(body.visual)) {
        const auto* bounds=mesh->bbox();
        const float infinity=std::numeric_limits<float>::infinity();
        Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
        for(unsigned corner=0;corner<8;++corner) {
          Vec3 at(bounds[corner&1].x,bounds[(corner>>1)&1].y,bounds[(corner>>2)&1].z);
          body.transform.project(at);
          low.x=std::min(low.x,at.x);low.y=std::min(low.y,at.y);low.z=std::min(low.z,at.z);
          high.x=std::max(high.x,at.x);high.y=std::max(high.y,at.y);high.z=std::max(high.z,at.z);
          }
        rendered="{\"center_cm\":"+ragdollVector((low+high)*.5f)+
          ",\"min_cm\":"+ragdollVector(low)+",\"max_cm\":"+ragdollVector(high)+"}";
        }
    rows+="{\"label\":"+jsonStr(label)+",\"id\":"+std::to_string(body.id)+
      ",\"parent_id\":"+std::to_string(body.parentId)+",\"zen_id\":"+std::to_string(body.zenId)+
      ",\"piece\":"+std::to_string(body.piece)+",\"category\":"+std::to_string(body.category)+
      ",\"visual\":"+jsonStr(body.visual)+",\"mass_kg\":"+ragdollNumber(body.mass)+
      ",\"density_kg_m3\":"+ragdollNumber(body.density)+",\"position_cm\":"+ragdollVector(point)+
      ",\"world_bottom_cm\":"+ragdollNumber(body.worldBounds[0].y)+
      ",\"world_top_cm\":"+ragdollNumber(body.worldBounds[1].y)+
      ",\"rendered_bbox\":"+rendered+itemFields+actorFields+
      ",\"water_y_cm\":"+(hasWater?ragdollNumber(body.waterY):"null")+
      ",\"water_depth_cm\":"+(hasWater && floor.hasCol?ragdollNumber(body.waterY-floor.v.y):"null")+
      ",\"submerged_fraction\":"+ragdollNumber(body.submergedFraction)+
      ",\"sleeping\":"+(body.sleeping?"true":"false")+
      ",\"first_sleep_ms\":"+(asleep==bench.waterSleepAt.end()?"null":std::to_string(asleep->second))+
      ",\"velocity_cm_s\":"+ragdollVector(body.velocity)+
      ",\"max_displacement_cm\":"+ragdollNumber(bench.waterTravel[body.id])+
      ",\"transform\":"+ragdollMatrix(body.transform)+"}";
    }
  return rows+"]";
  }

void waterCheckpoint(World& world,std::string_view label) {
  waterFields(world);
  const auto* hero=world.player();
  event("water_checkpoint","\"label\":"+jsonStr(label)+",\"case\":"+jsonStr(bench.waterCase)+
    ",\"buoyancy_enabled\":"+(world.physic()->buoyancyEnabled()?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.waterStart)+
    ",\"bodies\":"+waterRows(world)+",\"hero_cm\":"+(hero?ragdollVector(hero->position()):"null")+
    ",\"hero_swimming\":"+(hero && hero->isSwim()?"true":"false")+
    ",\"hero_saw_swim\":"+(bench.waterSawSwim?"true":"false")+
    ",\"hero_max_step_cm\":"+ragdollNumber(bench.waterHeroMaxStep)+
    ",\"listeners\":"+noiseListenerRows(world));
  }

bool waterProbe(World& world,const Vec3& at,float minDepth,float maxDepth,Vec3& surface) {
  const auto floor=world.physic()->landRay(at+Vec3(0,180,0),6000.f);
  if(!floor.hasCol)return false;
  const auto water=world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
  if(!water.hasCol || water.wdepth-floor.v.y<minDepth || water.wdepth-floor.v.y>maxDepth ||
     water.wdepth>at.y+180.f)return false;
  surface={at.x,water.wdepth,at.z};return true;
  }

bool waterFireballCaster(World& world,const Vec3& target,float targetRadius,Vec3& caster) {
  for(float radius:{700.f,900.f,1100.f,1300.f,1500.f,1700.f,1900.f,2100.f,2300.f,2500.f})
    for(unsigned angle=0;angle<64;++angle) {
      const float a=float(angle)*float(M_PI)/32.f;
      const auto probe=target+Vec3(std::cos(a)*radius,2000.f,std::sin(a)*radius);
      const auto floor=world.physic()->landRay(probe,4000.f);
      if(!floor.hasCol || floor.n.y<.99f || world.physic()->waterRay(floor.v,1.f).hasCol)continue;
      bool supported=true;
      for(float dx:{-35.f,35.f})for(float dz:{-35.f,35.f}) {
        const auto foot=world.physic()->landRay(floor.v+Vec3(dx,120,dz),160.f);
        if(!foot.hasCol || foot.n.y<.99f || std::abs(foot.v.y-floor.v.y)>10.f)supported=false;
        }
      if(!supported)continue;
      const auto obstruction=world.physic()->ray(floor.v+Vec3(0,140,0),target);
      if(obstruction.hasCol && (obstruction.v-target).length()>targetRadius+5.f)continue;
      caster=floor.v;return true;
      }
  return false;
  }

bool waterFindWindSite(World& world,std::vector<Vec3>& places) {
  std::vector<const WayPoint*> points;
  const auto harbour=[](const std::string& name) {
    return name.find("HABOUR")!=std::string::npos || name.find("HARB")!=std::string::npos;
    };
  world.forEachWayPoint([&](const WayPoint& point) {
    const auto& name=point.name;
    if(harbour(name) || name.find("WATER")!=std::string::npos || name.find("LAKE")!=std::string::npos ||
       name.find("BEACH")!=std::string::npos || name.find("COAST")!=std::string::npos)
      points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[&](const auto* a,const auto* b) {
    if(harbour(a->name)!=harbour(b->name))return harbour(a->name);
    return a->name<b->name;
    });
  unsigned dryPoints=0,waterlines=0,diagnostics=0;
  for(const auto* point:points) {
    const auto support=world.physic()->landRay(point->pos+Vec3(0,400,0),3000.f);
    if(!support.hasCol || support.n.y<.75f || world.physic()->waterRay(support.v,1.f).hasCol)continue;
    ++dryPoints;
    for(unsigned angle=0;angle<16;++angle) {
      const Vec3 direction(std::cos(float(angle)*float(M_PI)/8.f),0,std::sin(float(angle)*float(M_PI)/8.f));
      for(float distance=25.f;distance<=1800.f;distance+=25.f) {
        Vec3 surface;
        if(!waterProbe(world,support.v+direction*distance,10.f,6000.f,surface))continue;
        ++waterlines;
        bool found=false;Vec3 lowDry(0,0,0);float bestDrop=std::numeric_limits<float>::infinity();
        // Trace this actual shoreline, rather than requiring the seven-body
        // deep-water grid. A low pier or bank avoids a destructive cliff fall.
        for(float d=std::max(0.f,distance-400.f);d<distance;d+=5.f) {
          const auto probe=support.v+direction*d;
          const auto floor=world.physic()->landRay(probe+Vec3(0,400,0),3000.f);
          if(!floor.hasCol || floor.n.y<.99f)continue;
          const float drop=floor.v.y-surface.y;
          bestDrop=std::min(bestDrop,drop);
          if(drop<0.f || drop>100.f || world.physic()->waterRay(floor.v,1.f).hasCol)continue;
          lowDry=floor.v;found=true;
          }
        if(diagnostics<24 || found) {
          ++diagnostics;
          event("water_wind_candidate","\"waypoint\":"+jsonStr(point->name)+
            ",\"authored_cm\":"+ragdollVector(point->pos)+",\"floor_cm\":"+ragdollVector(support.v)+
            ",\"floor_normal\":"+ragdollVector(support.n)+",\"water_cm\":"+ragdollVector(surface)+
            ",\"distance_cm\":"+ragdollNumber(distance)+",\"lowest_flat_drop_cm\":"+
            (std::isfinite(bestDrop)?ragdollNumber(bestDrop):"null")+",\"found_low_support\":"+(found?"true":"false")+
            ",\"low_dry_cm\":"+ragdollVector(lowDry));
          }
        if(found) {
          bench.waterDry=lowDry;bench.waterCenter=surface;bench.waterDirection=direction;bench.waterWaypoint=point->name;
          bench.waterRayTop=std::max(point->pos.y,support.v.y)+400.f;
          places={surface};return true;
          }
        break;
        }
      }
    }
  event("water_wind_site_search","\"points\":"+std::to_string(points.size())+
    ",\"dry_points\":"+std::to_string(dryPoints)+",\"waterlines\":"+std::to_string(waterlines)+
    ",\"max_drop_cm\":100,\"min_depth_cm\":10");
  return false;
  }

bool waterFindSite(World& world,bool shallow,unsigned count,std::vector<Vec3>& places,bool harbourOnly=false) {
  if(harbourOnly)return waterFindWindSite(world,places);
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    const auto& name=point.name;
    if(harbourOnly && name.find("HABOUR")==std::string::npos && name.find("HARB")==std::string::npos)return;
    if(name.find("HABOUR")!=std::string::npos || name.find("HARB")!=std::string::npos ||
       name.find("WATER")!=std::string::npos || name.find("LAKE")!=std::string::npos ||
       name.find("BEACH")!=std::string::npos || name.find("COAST")!=std::string::npos)
      points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[&](const auto* a,const auto* b) {
    // The deep-water film needs open water rather than the lake's reed beds.
    // Keep the shallow and thirty-body performance searches unchanged.
    if(!shallow && count==7) {
      const auto harbour=[](const std::string& name) {
        return name.find("HABOUR")!=std::string::npos || name.find("HARB")!=std::string::npos;
        };
      if(harbour(a->name)!=harbour(b->name))return harbour(a->name);
      }
    return a->name<b->name;
    });
  for(const auto* point:points) {
    const auto floor=world.physic()->landRay(point->pos+Vec3(0,180,0),700.f);
    if(!floor.hasCol || floor.n.y<.75f || world.physic()->waterRay(floor.v,1.f).hasCol)continue;
    const auto dry=floor.v;
    for(unsigned angle=0;angle<16;++angle) {
      const Vec3 direction(std::cos(float(angle)*float(M_PI)/8.f),0,std::sin(float(angle)*float(M_PI)/8.f));
      for(float distance=shallow?25.f:400.f;distance<=1800.f;distance+=shallow?25.f:100.f) {
        const auto candidate=dry+direction*distance;
        Vec3 center;
        if(!waterProbe(world,candidate,shallow?.5f:220.f,shallow?35.f:6000.f,center))continue;
        if(world.physic()->ray(dry+Vec3(0,160,0),Vec3(center.x,dry.y+160,center.z)).hasCol)continue;
        places.clear();
        if(shallow) {
          // Follow the real shoreline contour. Each measured object has its
          // own water/bottom sample; a quay with no shallows is rejected.
          for(int radius=0;radius<=1200 && places.size()<count;radius+=40)
            for(unsigned a=0;a<32 && places.size()<count;++a) {
              Vec3 at;
              const auto probe=center+Vec3(std::cos(float(a)*float(M_PI)/16.f)*float(radius),0,
                                          std::sin(float(a)*float(M_PI)/16.f)*float(radius));
              if(!waterProbe(world,probe,.5f,35.f,at))continue;
              if(std::any_of(places.begin(),places.end(),[&](const Vec3& used){return (used-at).length()<190.f;}))continue;
              places.push_back(at);
              }
          }
        else {
          const auto side=Vec3(-direction.z,0,direction.x);
          for(unsigned i=0;i<count;++i) {
            const auto offset=count==30?side*((float(i%6)-2.5f)*155.f)+direction*((float(i/6)-2.f)*155.f):
              side*((float(i%4)-1.5f)*230.f)+direction*((float(i/4)-.5f)*230.f);
            Vec3 at;
            if(!waterProbe(world,center+offset,220.f,6000.f,at))break;
            places.push_back(at);
            }
          }
        if(places.size()!=count)continue;
        if(!shallow && count==7 && !harbourOnly) {
          // Dense items need a level real seabed for the rest measurement.
          // A sloping harbour floor can correctly keep ore sliding forever.
          const auto levelProbe=[&](const Vec3& probe,Vec3& surface,float& floorY) {
            const auto support=world.physic()->landRay(probe+Vec3(0,180,0),6000.f);
            if(!support.hasCol || support.n.y<.99f)return false;
            const auto water=world.physic()->waterRay(support.v+Vec3(0,.1f,0),0);
            if(!water.hasCol || water.wdepth-support.v.y<220.f || water.wdepth>probe.y+180.f)return false;
            surface={probe.x,water.wdepth,probe.z};floorY=support.v.y;return true;
            };
          bool densePlaced=true;
          for(const unsigned index:{4u,5u}) {
            const auto original=places[index];
            bool found=false;
            for(int radius=0;radius<=1800 && !found;radius+=50)
              for(unsigned a=0;a<32 && !found;++a) {
                const auto probe=original+Vec3(std::cos(float(a)*float(M_PI)/16.f)*float(radius),0,
                                               std::sin(float(a)*float(M_PI)/16.f)*float(radius));
                Vec3 at;float floorY=0;
                if(!levelProbe(probe,at,floorY) || (at-dry).quadLength()>2500.f*2500.f)continue;
                bool separate=true;
                for(size_t other=0;other<places.size();++other)
                  if(other!=index && (places[other]-at).length()<190.f) {separate=false;break;}
                if(!separate)continue;
                float lowest=floorY,highest=floorY;
                bool level=true;
                for(float dx:{-20.f,20.f}) for(float dz:{-20.f,20.f}) {
                  Vec3 corner;float cornerY=0;
                  if(!levelProbe(at+Vec3(dx,0,dz),corner,cornerY) || std::abs(corner.y-at.y)>.1f) {level=false;break;}
                  lowest=std::min(lowest,cornerY);highest=std::max(highest,cornerY);
                  }
                if(!level || highest-lowest>2.f)continue;
                places[index]=at;found=true;
                }
            if(!found) {densePlaced=false;break;}
            }
          if(!densePlaced)continue;
          // Reject slots under moored boats or behind a quay before moving
          // anything. The same supported sight line is checked at the cast.
          Vec3 caster;
          if(!waterFireballCaster(world,places[0],0.f,caster))continue;
          }
        bench.waterDry=dry;bench.waterCenter=center;bench.waterDirection=direction;bench.waterWaypoint=point->name;
        return true;
        }
      }
    }
  return false;
  }

bool waterPlaceMap(World& world,const DynamicWorld::MovableVobSnapshot& body,const Vec3& surface,std::string label) {
  auto matrix=Matrix4x4::mkIdentity();
  const auto center=(body.localBounds[0]+body.localBounds[1])*.5f;
  matrix.translate(surface.x-center.x,surface.y-body.localBounds[0].y+55.f,surface.z-center.z);
  if(!world.physic()->setMovableVobTransform(body.zenId,matrix))return false;
  // Restoring an authored map pose intentionally leaves it asleep. This
  // fixture places it in mid-air, so provide an ordinary tiny native impulse
  // before observing gravity and water. The hull COM can differ from bbox COM.
  auto worldCenter=center;matrix.project(worldCenter);
  const float radius=(body.localBounds[1]-body.localBounds[0]).length()*.5f+1.f;
  const auto awakened=world.physic()->applyMovableVobImpulse(worldCenter,radius,
    [mass=body.mass](const Vec3&,float targetMass) {
      return std::abs(targetMass-mass)<.001f ? Vec3(0,-.1f*mass,0) : Vec3();
      });
  if(awakened!=1)return false;
  bench.waterIds.insert(body.id);bench.waterLabels[body.id]=std::move(label);
  bench.movableIds.push_back(body.id);bench.movableMapIds.insert(body.id);
  return true;
  }

void waterTorch(World& world,Npc& player) {
  if(!bench.waterReady || bench.waterCase!="deep" || bench.waterItems.size()!=4) {
    arenaAbort("water torch requires the seven-object deep-water fixture");return;
    }
  const auto symbol=world.script().findSymbolIndex("ItLsTorchburning");
  if(symbol==size_t(-1)) {arenaAbort("water torch script instance is unavailable");return;}
  const auto before=world.physic()->waterBodySnapshot();
  Vec3 surface;bool found=false;
  for(int radius=0;radius<=900 && !found;radius+=50)
    for(unsigned angle=0;angle<32 && !found;++angle) {
      const auto probe=bench.waterCenter+Vec3(std::cos(float(angle)*float(M_PI)/16.f)*float(radius),0,
                                             std::sin(float(angle)*float(M_PI)/16.f)*float(radius));
      if(!waterProbe(world,probe,220.f,6000.f,surface) ||
         (surface-bench.waterDry).quadLength()>2500.f*2500.f)continue;
      found=std::none_of(before.begin(),before.end(),[&](const auto& body) {
        if(!waterOwns(body))return false;
        auto delta=movablePosition(body.transform)-surface;delta.y=0;
        return delta.quadLength()<190.f*190.f;
        });
      }
  if(!found) {arenaAbort("water torch has no separate real deep-water position");return;}
  auto matrix=Matrix4x4::mkIdentity();matrix.translate(surface+Vec3(0,100,0));
  auto* item=world.addItemDyn(symbol,matrix,world.npcId(&player));
  if(!item || !item->isTorchBurn() || item->physicsMass()<=0) {
    arenaAbort("water torch lacks the burning subtype or native dynamic body");return;
    }
  uintptr_t id=0;
  for(const auto& body:world.physic()->waterBodySnapshot())
    if(body.category==DynamicWorld::C_Item &&
       std::none_of(before.begin(),before.end(),[&](const auto& previous){return previous.id==body.id;})) {
      if(id) {arenaAbort("water torch inserted more than one native body");return;}
      id=body.id;
      }
  if(!id) {arenaAbort("water torch inserted no native body");return;}
  bench.waterItems.push_back(item);bench.waterIds.insert(id);bench.waterLabels[id]="torch";
  event("water_torch_inserted","\"id\":"+std::to_string(id)+",\"item_instance\":"+std::to_string(symbol)+
    ",\"torch_burning\":true,\"surface_cm\":"+ragdollVector(surface)+",\"bodies\":"+waterRows(world));
  }

void prepareWaterFixture(World& world,Npc& player,std::string_view mode) {
  if(bench.waterReady || (mode!="deep" && mode!="shallow" && mode!="perf" && mode!="wind")) {
    arenaAbort("water setup expects a fresh fixture and deep, shallow, perf or wind");return;
    }
  const bool shallow=mode=="shallow",perf=mode=="perf",harbourOnly=mode=="wind";
  std::vector<Vec3> places;
  if(!waterFindSite(world,shallow,perf?30u:harbourOnly?1u:7u,places,harbourOnly)) {
    arenaAbort(shallow?"water fixture cannot find seven real shallow-water positions":
                       "water fixture cannot find a sufficiently deep unobstructed stock-world water area");return;
    }
  const auto all=world.physic()->movableVobSnapshot();
  const auto native=world.physic()->waterBodySnapshot();
  std::vector<DynamicWorld::MovableVobSnapshot> selected;
  if(perf) {
    // Performance does not save. Thirty copies keep ON/OFF meshes and masses
    // identical without depending on how many authored barrels pass contact
    // validation in a particular compatible old save.
    std::string visual;
    float density=std::numeric_limits<float>::infinity();
    for(const auto& body:all) {
      if(!body.enabled || body.broken || body.className!="barrel")continue;
      for(const auto& sample:native)
        if(sample.parentId==body.id && sample.density<density) {visual=body.visual;density=sample.density;}
      }
    if(visual.empty()) {arenaAbort("water performance fixture requires an available native barrel mesh");return;}
    for(unsigned i=0;i<30;++i) {
      const auto id=destructionAdd(world,visual,places[i]+Vec3(0,55,0));
      if(!id) {arenaAbort("water performance barrel insertion failed");return;}
      auto& handle=bench.destructionFixtures.back()->body;
      if(!handle.addImpulse(Vec3(0,-.1f*handle.mass(),0))) {
        arenaAbort("water performance barrel could not be activated");return;
        }
      bench.waterIds.insert(id);bench.waterLabels[id]="barrel_"+std::to_string(i);
      }
    }
  else for(const std::string name:{"crate","barrel","table"}) {
    if(harbourOnly && name!="barrel")continue;
    const DynamicWorld::MovableVobSnapshot* found=nullptr;
    float area=std::numeric_limits<float>::infinity(),density=std::numeric_limits<float>::infinity();
    for(const auto& body:all) {
      if(!body.enabled || body.broken || body.zenId==uint32_t(-1) || body.className!=name)continue;
      const auto extent=body.localBounds[1]-body.localBounds[0];
      const float footprint=extent.x*extent.z;
      float rho=std::numeric_limits<float>::infinity();
      for(const auto& sample:native)if(sample.parentId==body.id) {rho=sample.density;break;}
      if((name=="barrel" && (rho<density || (rho==density && footprint<area))) ||
         (name!="barrel" && footprint<area)) {found=&body;area=footprint;density=rho;}
      }
    if(!found) {arenaAbort("water fixture lacks an enabled saved map "+name);return;}
    selected.push_back(*found);
    }
  for(size_t i=0;i<selected.size();++i) {
    if(!waterPlaceMap(world,selected[i],places[i],perf?"barrel_"+std::to_string(i):selected[i].className)) {
      arenaAbort("water fixture cannot move its saved map body");return;
      }
    if(selected[i].className=="barrel")bench.waterBarrelZen=selected[i].zenId;
    if(selected[i].className=="crate")bench.waterCrateZen=selected[i].zenId;
    }
  if(!perf && !harbourOnly) {
    const char* symbols[]{"ITRW_ARROW","ITMW_1H_SLD_SWORD","ITMI_STONE","ITMI_LUTE"};
    const char* labels[]{"arrow","sword","stone","wood_item"};
    for(unsigned i=0;i<4;++i) {
      auto symbol=world.script().findSymbolIndex(symbols[i]);
      if(symbol==size_t(-1) && i==1)
        for(size_t candidate=0;candidate<world.script().symbolsCount();++candidate) {
          const auto* entry=world.script().findSymbol(candidate);
          if(!entry || entry->type()!=zenkit::DaedalusDataType::INSTANCE)continue;
          auto name=entry->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
          if(name.starts_with("ITMW_") && name.find("SWORD")!=std::string::npos) {symbol=candidate;break;}
          }
      if(symbol==size_t(-1) && i==2)symbol=world.script().findSymbolIndex("ITMI_NUGGET");
      if(symbol==size_t(-1)) {arenaAbort("water fixture item script instance missing: "+std::string(symbols[i]));return;}
      const auto before=world.physic()->waterBodySnapshot();
      auto matrix=Matrix4x4::mkIdentity();matrix.translate(places[i+3]+Vec3(0,100,0));
      auto* item=world.addItemDyn(symbol,matrix,world.npcId(&player));
      if(!item) {arenaAbort("water fixture world Item insertion failed");return;}
      bench.waterItems.push_back(item);
      bool found=false;
      for(const auto& body:world.physic()->waterBodySnapshot())
        if(body.category==DynamicWorld::C_Item &&
           std::none_of(before.begin(),before.end(),[&](const auto& previous){return previous.id==body.id;})) {
          bench.waterIds.insert(body.id);bench.waterLabels[body.id]=labels[i];found=true;
          }
      if(!found) {arenaAbort("water fixture world Item has no native dynamic body");return;}
      }
    }
  player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
  player.setPosition(bench.waterDry);player.setDirection(bench.waterCenter-bench.waterDry);player.updateTransform();
  // Preserve native hearing for the splash; unrelated routines cannot walk
  // into the caster or move the authored fixtures during observation.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && (npc->position()-bench.waterCenter).length()<2500.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  bench.waterCase=mode;bench.waterStart=world.tickCount();bench.waterReady=true;bench.waterTrace=true;
  noiseBegin(world,"water-entry",bench.waterCenter);
  bench.movableReady=true;bench.movableCenter=bench.waterCenter;
  bench.cam=C_Hold;
  if(mode=="deep") {
    // Keep the optical path over open water, above the shore's reeds.
    bench.cam=C_Water;bench.waterCameraTarget=bench.waterCenter;
    lookFrom(bench.waterCenter+bench.waterDirection*200.f+Vec3(0,1000,0),bench.waterCenter);
    }
  else
    lookFrom(bench.waterCenter-bench.waterDirection*600.f+Vec3(300,360,0),bench.waterCenter+Vec3(0,25,0));
  event("water_setup","\"case\":"+jsonStr(mode)+",\"waypoint\":"+jsonStr(bench.waterWaypoint)+
    ",\"dry_cm\":"+ragdollVector(bench.waterDry)+",\"center_cm\":"+ragdollVector(bench.waterCenter)+
    ",\"bodies\":"+waterRows(world));
  }

void waterInitial(World& world,Npc& player,PlayerControl& ctrl,std::string_view label) {
  const auto savedGoal=player.navigationGoal();
  const auto savedPosition=player.position();
  const auto savedFloor=world.physic()->landRay(savedPosition+Vec3(0,400,0),1000.f);
  const bool savedSlide=player.isSlide();
  std::vector<Vec3> observed;
  bench.waterCase="reload";bench.waterStart=world.tickCount();bench.waterTrace=true;
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    auto delta=movablePosition(body.transform)-player.position();delta.y=0;
    // Capture sleepers inside the ordinary thirty-metre debris lifetime area
    // before selecting a dry observer within 27 m of every observed body.
    if(delta.length()>2900.f || !body.inWater)continue;
    bench.waterIds.insert(body.id);
    bench.waterLabels[body.id]=body.visual;
    observed.push_back((body.worldBounds[0]+body.worldBounds[1])*.5f);
    }
  if(bench.waterIds.empty()) {arenaAbort("water reload has no nearby saved native objects");return;}
  waterCheckpoint(world,label);
  // The saved hero may stand on a real sliding slope. After the untouched
  // snapshot, select a flat native dry floor while keeping every observed
  // object comfortably inside the ordinary thirty-metre debris lifetime area.
  const auto flatDry=[&](const Vec3& probe,Vec3& stand) {
    const auto floor=world.physic()->landRay(probe+Vec3(0,400,0),1000.f);
    if(!floor.hasCol || floor.n.y<.99f)return false;
    stand=floor.v;
    if(std::any_of(observed.begin(),observed.end(),[&](const Vec3& body){return (body-stand).length()>2700.f;}))return false;
    float lowest=stand.y,highest=stand.y;
    for(float dx:{-35.f,0.f,35.f})for(float dz:{-35.f,0.f,35.f}) {
      const auto support=world.physic()->landRay(stand+Vec3(dx,180,dz),400.f);
      if(!support.hasCol || support.n.y<.99f)return false;
      const auto water=world.physic()->waterRay(support.v+Vec3(0,.1f,0),0);
      if(water.hasCol && water.wdepth>support.v.y)return false;
      if(world.physic()->ray(support.v+Vec3(0,5,0),support.v+Vec3(0,190,0)).hasCol)return false;
      lowest=std::min(lowest,support.v.y);highest=std::max(highest,support.v.y);
      }
    return highest-lowest<=10.f;
    };
  Vec3 observer;bool supported=false;
  for(int radius=0;radius<=1800 && !supported;radius+=50)
    for(unsigned a=0;a<32 && !supported;++a) {
      const auto probe=savedPosition+Vec3(std::cos(float(a)*float(M_PI)/16.f)*float(radius),0,
                                          std::sin(float(a)*float(M_PI)/16.f)*float(radius));
      supported=flatDry(probe,observer);
      }
  if(!supported) {arenaAbort("water reload observer has no nearby flat dry native floor");return;}
  ctrl.clearInput();bench.holds.clear();bench.taps.clear();bench.ctrl=&ctrl;
  player.clearState(false);player.clearAiQueue();player.closeWeapon(true);
  player.stopWalking();player.clearSpeed();player.setExternalControl(true);
  player.setPosition(observer);player.updateTransform();
  unsigned frozen=0;
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && (npc->position()-observer).length()<4000.f) {
      npc->clearAiQueue();npc->clearState(false);npc->setTarget(nullptr);npc->stopWalking();npc->clearSpeed();
      npc->setExternalControl(true);++frozen;
      }
  bench.waterObserver=true;bench.waterObserverStart=player.position();bench.waterObserverSample=world.tickCount()+1000;
  const auto observerFloor=world.physic()->landRay(observer+Vec3(0,180,0),400.f);
  event("water_observer_idle","\"hero_cm\":"+ragdollVector(player.position())+
    ",\"saved_hero_cm\":"+ragdollVector(savedPosition)+",\"saved_floor_normal\":"+ragdollVector(savedFloor.n)+
    ",\"saved_slide\":"+(savedSlide?"true":"false")+",\"observer_floor_normal\":"+ragdollVector(observerFloor.n)+
    ",\"nearby_npcs_paused\":"+std::to_string(frozen)+
    ",\"saved_goal_active\":"+(savedGoal.active?"true":"false")+
    ",\"saved_goal_kind\":"+jsonStr(savedGoal.kind)+",\"saved_goal_cm\":"+ragdollVector(savedGoal.position));

  }

std::string waterActorRows(World& world) {
  std::string rows="[";
  const auto samples=world.physic()->waterBodySnapshot();
  for(const auto& actor:bench.waterActors) {
    if(rows.size()>1)rows+=',';
    const auto& npc=*actor.npc;const auto* body=npc.ragdollBody();
    float torso=0,torsoMass=0;
    const auto* skeleton=npc.visualSkeleton();
    for(const auto& sample:samples) {
      if(sample.ragdoll!=body || !skeleton || sample.bone>=skeleton->nodes.size())continue;
      auto name=skeleton->nodes[sample.bone].name;
      for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.find("SPINE")==std::string::npos && name.find("PELVIS")==std::string::npos)continue;
      torso+=sample.submergedFraction*sample.mass;torsoMass+=sample.mass;
      }
    rows+="{\"actor_id\":"+std::to_string(world.npcId(&npc))+",\"role\":"+jsonStr(actor.role)+
      ",\"symbol\":"+jsonStr(symName(world,&npc))+",\"dead\":"+(npc.isDead()?"true":"false")+
      ",\"unconscious\":"+(npc.isUnconscious()?"true":"false")+
      ",\"part_count\":"+std::to_string(body?body->partCount():0)+
      ",\"position_cm\":"+ragdollVector(npc.position())+
      ",\"sleeping\":"+(body && body->isSleeping()?"true":"false")+
      ",\"first_sleep_ms\":"+(actor.sleepAt==uint64_t(-1)?"null":std::to_string(actor.sleepAt))+
      ",\"torso_submerged_fraction\":"+(torsoMass>0?ragdollNumber(torso/torsoMass):"null")+
      ",\"parts\":[";
    if(body) {
      const auto state=body->state();
      for(size_t i=0;i<state.parts.size();++i) {
        const auto& part=state.parts[i];if(i)rows+=',';
        rows+="{\"bone\":"+std::to_string(part.bone)+",\"transform\":"+ragdollMatrix(part.transform)+
          ragdollNativePrecision(part)+",\"angular_velocity_rad_s\":"+ragdollVector(part.angularVelocity)+
          ",\"sleeping\":"+(part.sleeping?"true":"false")+"}";
        }
      }
    rows+="]}";
    }
  return rows+"]";
  }

void waterBodiesCheckpoint(World& world,std::string_view label) {
  waterFields(world);
  event("water_bodies_checkpoint","\"label\":"+jsonStr(label)+",\"case\":"+jsonStr(bench.waterCase)+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.waterStart)+
    ",\"buoyancy_enabled\":"+(world.physic()->buoyancyEnabled()?"true":"false")+
    ",\"actors\":"+waterActorRows(world)+",\"bodies\":"+waterRows(world));
  }

bool waterBodiesAdd(World& world,Npc& player,std::string_view symbol,std::string role,const Vec3& at,bool dead=true) {
  const auto instance=world.script().findSymbolIndex(symbol);
  if(instance==size_t(-1)) {arenaAbort("water corpse instance is unavailable: "+std::string(symbol));return false;}
  auto* npc=world.addNpc(instance,at);
  if(!npc) {arenaAbort("water corpse insertion failed");return false;}
  npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
  npc->handle().flags=zenkit::NpcFlag(uint32_t(npc->handle().flags)&~uint32_t(zenkit::NpcFlag::IMMORTAL));
  npc->setDirection(bench.waterDirection);npc->updateTransform();
  if(npc->isHuman()) {
    const auto apple=world.script().findSymbolIndex("ITFO_APPLE");
    if(apple!=size_t(-1))npc->addItem(apple,2);
    }
  bench.waterActors.push_back({npc,std::move(role)});
  bench.crew.push_back(npc);
  if(dead) {
    npc->setLastHitBy(&player);
    if(!npc->setRagdollDown(true)) {arenaAbort("water corpse production death path refused");return false;}
    npc->prepareRagdoll();
    if(!npc->ragdollBody()) {arenaAbort("water corpse has no supported native ragdoll");return false;}
    }
  return true;
  }

void prepareWaterBodiesFixture(World& world,Npc& player,std::string_view mode) {
  if(bench.waterReady || bench.waterBodiesReady || (mode!="deep" && mode!="shallow" && mode!="perf")) {
    arenaAbort("water-bodies setup expects a fresh deep, shallow or perf fixture");return;
    }
  std::vector<Vec3> places;
  if(mode=="deep") {
    // Use the existing low harbour shore search: the inland quest-return lake
    // has rock and lilies between the camera and the floating body.
    if(bench.spellActors.empty() || !waterFindWindSite(world,places)) {
      arenaAbort("water corpse film requires a real low harbour water lip");return;
      }
    if(bench.waterWaypoint.find("HABOUR")==std::string::npos && bench.waterWaypoint.find("HARB")==std::string::npos) {
      arenaAbort("water corpse film requires the harbour rather than the inland lake");return;
      }
    const auto lip=bench.waterDry;
    const auto direction=bench.waterDirection;
    const auto side=Vec3(-direction.z,0,direction.x);
    Vec3 caster;bool supported=false;
    for(float distance:{150.f,200.f,250.f,300.f}) {
      const auto floor=world.physic()->landRay(lip-direction*distance+Vec3(0,400,0),600.f);
      if(!floor.hasCol || floor.n.y<.99f || world.physic()->waterRay(floor.v,1.f).hasCol ||
         world.physic()->ray(floor.v+Vec3(0,140,0),lip+Vec3(0,100,0)).hasCol)continue;
      bool flat=true;
      for(float dx:{-30.f,30.f})for(float dz:{-30.f,30.f}) {
        const auto foot=world.physic()->landRay(floor.v+Vec3(dx,100,dz),150.f);
        if(!foot.hasCol || foot.n.y<.99f || std::abs(foot.v.y-floor.v.y)>5.f ||
           world.physic()->waterRay(foot.v,1.f).hasCol)flat=false;
        }
      if(flat) {caster=floor.v;supported=true;break;}
      }
    if(!supported) {arenaAbort("water corpse harbour lip has no supported dry WindFist caster");return;}
    bool found=false;
    for(float distance=100.f;distance<=1800.f && !found;distance+=25.f) {
      Vec3 center,wolf,skeleton;
      if(!waterProbe(world,lip+direction*distance,220.f,6000.f,center) ||
         !waterProbe(world,center+side*220.f,220.f,6000.f,wolf) ||
         !waterProbe(world,center-side*220.f,220.f,6000.f,skeleton))continue;
      bench.waterCenter=center;places={wolf,skeleton};found=true;
      }
    if(!found) {arenaAbort("water corpse lip has no three separated deep-water slots");return;}
    auto* human=bench.spellActors[0].npc;
    for(size_t i=1;i<bench.spellActors.size();++i)world.removeNpc(*bench.spellActors[i].npc);
    bench.spellActors.resize(1);bench.crew.clear();bench.crew.push_back(human);
    human->handle().flags=zenkit::NpcFlag(uint32_t(human->handle().flags)&~uint32_t(zenkit::NpcFlag::IMMORTAL));
    human->setPosition(lip);human->setDirection(direction);human->updateTransform();
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
    player.setPosition(caster);player.setDirection(direction);player.updateTransform();
    bench.questWater={true,lip,direction,bench.waterCenter.y,bench.waterWaypoint};
    bench.waterDry=caster;
    const auto apple=world.script().findSymbolIndex("ITFO_APPLE");
    if(apple!=size_t(-1))human->addItem(apple,2);
    bench.waterActors.push_back({human,"human"});
    bench.spellGuardTrace=false;
    if(!waterBodiesAdd(world,player,"Wolf","wolf",places[0]+Vec3(0,55,0)) ||
       !waterBodiesAdd(world,player,"Skeleton","skeleton",places[1]+Vec3(0,55,0)))return;
    }
  else {
    const bool shallow=mode=="shallow";
    if(!waterFindSite(world,shallow,shallow?1u:10u,places)) {
      arenaAbort("water corpse fixture cannot find real separated water positions");return;
      }
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
    player.setPosition(bench.waterDry);player.setDirection(bench.waterCenter-bench.waterDry);player.updateTransform();
    bench.crew.clear();
    for(size_t i=0;i<places.size();++i)
      if(!waterBodiesAdd(world,player,"BAU_904_BAUER",shallow?"human":"human_"+std::to_string(i),
                         places[i]+Vec3(0,shallow?5.f:55.f,0)))return;
    }
  bench.waterCase="bodies-"+std::string(mode);bench.waterStart=world.tickCount();
  bench.waterReady=true;bench.waterBodiesReady=true;bench.waterTrace=true;
  bench.movableReady=true;bench.movableCenter=bench.waterCenter;
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && !npc->isDown() &&
       (npc->position()-bench.waterCenter).length()<2500.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  noiseBegin(world,"water-body-entry",bench.waterCenter);
  bench.cam=C_Water;bench.waterCameraTarget=bench.waterCenter;
  lookFrom(bench.waterCenter+bench.waterDirection*150.f+Vec3(0,850,0),bench.waterCenter);
  event("water_bodies_setup","\"case\":"+jsonStr(mode)+",\"waypoint\":"+jsonStr(bench.waterWaypoint)+
    ",\"dry_cm\":"+ragdollVector(bench.waterDry)+",\"center_cm\":"+ragdollVector(bench.waterCenter)+
    ",\"camera_cm\":"+ragdollVector(bench.camPos)+",\"actors\":"+waterActorRows(world));
  }

void waterBodiesInitial(World& world,Npc& player,std::string_view label) {
  bench.waterStart=world.tickCount();bench.waterCase="bodies-reload";
  bench.waterTrace=true;bench.waterReady=true;bench.waterBodiesReady=true;
  Vec3 center;size_t count=0;
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* npc=world.npcById(id);
    if(!npc || !npc->isDead() || !npc->ragdollBody() || (npc->position()-player.position()).length()>3000.f)continue;
    const auto* rig=npc->ragdollBody();bool wet=false;
    for(const auto& part:world.physic()->waterBodySnapshot())if(part.ragdoll==rig && part.inWater) {wet=true;break;}
    if(!wet)continue;
    auto symbol=symName(world,npc);for(auto& c:symbol)c=char(std::toupper(static_cast<unsigned char>(c)));
    const std::string role=npc->isHuman()?"human":symbol.find("WOLF")!=std::string::npos?"wolf":
      symbol.find("SKELETON")!=std::string::npos?"skeleton":symbol;
    bench.waterActors.push_back({npc,role});center+=movablePosition(rig->pelvisTransform());++count;
    }
  if(!count) {arenaAbort("water body reload has no nearby saved wet corpses");return;}
  // Capture the untouched native pose before restoring transient bench control.
  waterBodiesCheckpoint(world,label);
  for(auto& actor:bench.waterActors)actor.npc->setExternalControl(true);
  player.clearAiQueue();player.stopWalking();player.clearSpeed();player.setExternalControl(true);
  bench.waterCenter=center/float(count);bench.cam=C_Water;bench.waterCameraTarget=bench.waterCenter;
  lookFrom(bench.waterCenter+Vec3(0,850,150),bench.waterCenter);
  }

Npc* waterLootCorpse() {
  for(auto& actor:bench.waterActors)
    if(actor.npc->isHuman() && actor.npc->isDead() && actor.npc->ragdollBody())return actor.npc;
  return nullptr;
  }

void waterBodiesLootReady(World& world,Npc& player,std::string_view mode) {
  bench.waterLootMode=mode;bench.waterLootReady=false;
  auto* corpse=waterLootCorpse();
  if(!corpse || (mode!="shore" && mode!="swim")) {arenaAbort("water corpse loot expects a human and shore or swim");return;}
  const auto center=movablePosition(corpse->ragdollBody()->pelvisTransform());
  Vec3 at;bool found=false;
  if(mode=="swim") {
    Vec3 surface;
    if(waterProbe(world,center,160.f,6000.f,surface)) {
      at=surface-Vec3(0,100,0)+Vec3(100,0,0);found=true;
      }
    }
  else for(float radius:{80.f,120.f,160.f,200.f,240.f}) {
    for(unsigned angle=0;angle<32;++angle) {
      const auto probe=center+Vec3(std::cos(float(angle)*float(M_PI)/16.f)*radius,400.f,
                                   std::sin(float(angle)*float(M_PI)/16.f)*radius);
      const auto floor=world.physic()->landRay(probe,1000.f);
      if(!floor.hasCol || floor.n.y<.98f || world.physic()->waterRay(floor.v,1.f).hasCol ||
         (floor.v-center).length()>300.f)continue;
      at=floor.v;found=true;break;
      }
    if(found)break;
    }
  event("water_bodies_loot_position","\"mode\":"+jsonStr(mode)+",\"available\":"+(found?"true":"false")+
    ",\"actor_id\":"+std::to_string(world.npcId(corpse))+",\"corpse_cm\":"+ragdollVector(center)+
    ",\"hero_cm\":"+(found?ragdollVector(at):"null"));
  if(!found)return;
  player.setExternalControl(false);player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
  player.setPosition(at);player.setDirection(center-at);player.updateTransform();
  bench.waterLootReady=true;
  }

void waterBodiesLoot(World& world,Npc& player,PlayerControl& ctrl) {
  auto* corpse=waterLootCorpse();
  if(!bench.waterLootReady || !corpse) {
    event("water_bodies_loot","\"mode\":"+jsonStr(bench.waterLootMode)+",\"available\":false");return;
    }
  ctrl.clearFocus();ctrl.tickFocus();const auto focus=ctrl.focus();
  const auto inventory=ragdollInventory(*corpse,world);
  bench.expectedRagdollLoot=corpse;bench.ragdollLootDidOpen=false;
  const bool interacted=focus.npc==corpse && ctrl.interact(*corpse);
  bench.expectedRagdollLoot=nullptr;
  event("water_bodies_loot","\"mode\":"+jsonStr(bench.waterLootMode)+",\"available\":true"+
    ",\"actor_id\":"+std::to_string(world.npcId(corpse))+",\"focus_actor_id\":"+std::to_string(world.npcId(focus.npc))+
    ",\"hero_swimming\":"+(player.isSwim()?"true":"false")+
    ",\"interacted\":"+(interacted?"true":"false")+",\"opened\":"+(bench.ragdollLootDidOpen?"true":"false")+
    ",\"inventory_unchanged\":"+(inventory==ragdollInventory(*corpse,world)?"true":"false")+
    ",\"lootable_inventory\":"+ragdollInventory(*corpse,world,Inventory::T_Ransack));
  }

void prepareWaterSpell(World& world,Npc& player,bool wind) {
  if(!bench.waterReady) {arenaAbort("water real spell needs its fixture");return;}
  const auto all=world.physic()->movableVobSnapshot();
  const auto zen=wind?bench.waterBarrelZen:bench.waterCrateZen;
  const auto found=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==zen;});
  if(found==all.end() || found->broken) {arenaAbort("water spell map target is unavailable");return;}
  auto target=(found->localBounds[0]+found->localBounds[1])*.5f;found->transform.project(target);
  Vec3 caster=bench.waterDry,windDummy;
  if(!wind) {
    // A quay or a moored ship may block the route from the original dry WP.
    // Find a real supported firing position with a clear path to this crate.
    const float targetRadius=(found->localBounds[1]-found->localBounds[0]).length()*.5f;
    if(!waterFireballCaster(world,target,targetRadius,caster)) {
      arenaAbort("water fireball requires supported dry ground and a clear native shot path");return;
      }
    }
  if(wind) {
    // Follow the real terrain to the waterline, including any dry terraces
    // above it. A height change alone is not the edge of the water.
    const auto direction=Vec3::normalize(Vec3(target.x,bench.waterDry.y,target.z)-bench.waterDry);
    bool supported=false,reachedWater=false;Vec3 lip=bench.waterDry;
    float shoreWaterY=bench.waterCenter.y;
    for(float d=0;d<=1800.f;d+=5.f) {
      const auto at=bench.waterDry+direction*d;
      const auto floor=world.physic()->landRay(at+Vec3(0,400,0),3000.f);
      if(!floor.hasCol)continue;
      const auto water=world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
      if(water.hasCol && water.wdepth>floor.v.y) {reachedWater=true;shoreWaterY=water.wdepth;break;}
      lip=floor.v;supported=true;
      }
    if(!supported || !reachedWater) {arenaAbort("water wind barrel has no supported real shoreline lip");return;}
    const auto extent=(found->localBounds[1]-found->localBounds[0])*.5f;
    // The public impulse callback exposes native COM. Returning zero reads
    // it without waking the body, applying a force or damaging the barrel.
    Vec3 center;float nearest=extent.quadLength()+1.f;bool hasCenter=false;
    world.physic()->applyMovableVobImpulse(target,extent.length()+1.f,[&](const Vec3& nativeCenter,float mass) {
      const auto distance=(nativeCenter-target).quadLength();
      if(std::abs(mass-found->mass)<.001f && distance<nearest) {
        center=nativeCenter;nearest=distance;hasCenter=true;
        }
      return Vec3();
      });
    if(!hasCenter) {arenaAbort("water wind barrel has no native centre of mass");return;}
    auto inverse=found->transform;inverse.inverse();inverse.project(center);
    // Find the nearest small dry support footprint behind the real edge.
    // The rest of the hull may overhang, so normal wind still reaches water.
    const auto side=Vec3(-direction.z,0,direction.x);
    // The shoreline can be several metres below the dry dock. Start every
    // placement ray above both levels so it can still find that upper support.
    const auto shoreFloor=[&](const Vec3& probe) {
      return world.physic()->landRay(Vec3(probe.x,std::max(probe.y+400.f,bench.waterRayTop),probe.z),4000.f);
      };
    unsigned supportTrials=0,supportNoFloor=0,supportSteep=0,supportWet=0,supportUneven=0,supportRejected=0;
    std::string supportRejections="[";
    const auto rejectSupport=[&](std::string_view why,const Vec3& probe,const DynamicWorld::RayLandResult& support,bool hasWater,float waterY) {
      if(supportRejected>=3)return;
      if(supportRejected++)supportRejections+=',';
      supportRejections+="{\"reason\":"+jsonStr(why)+",\"probe_cm\":"+ragdollVector(probe)+
        ",\"floor_found\":"+(support.hasCol?"true":"false")+",\"floor_cm\":"+ragdollVector(support.v)+
        ",\"floor_normal\":"+ragdollVector(support.n)+",\"water_found\":"+(hasWater?"true":"false")+
        ",\"water_y_cm\":"+(hasWater?ragdollNumber(waterY):"null")+"}";
      };
    const auto drySupport=[&](const Vec3& probe,DynamicWorld::RayLandResult& support) {
      support=shoreFloor(probe);
      if(!support.hasCol) {++supportNoFloor;rejectSupport("no_floor",probe,support,false,0);return false;}
      const auto water=world.physic()->waterRay(support.v+Vec3(0,.1f,0),0);
      if(support.n.y<.99f) {++supportSteep;rejectSupport("steep_floor",probe,support,water.hasCol,water.wdepth);return false;}
      if(water.hasCol && water.wdepth>support.v.y) {++supportWet;rejectSupport("wet_floor",probe,support,true,water.wdepth);return false;}
      if(support.v.y-shoreWaterY>100.f) {++supportUneven;rejectSupport("high_above_water",probe,support,true,shoreWaterY);return false;}
      return true;
      };
    Vec3 at,floorPoint,floorNormal;float chosenRetreat=0,chosenLateral=0;bool footprint=false;
    for(float retreat=50.f;retreat<=250.f && !footprint;retreat+=10.f)
      for(float lateral:{0.f,-50.f,50.f,-100.f,100.f,-150.f,150.f,-200.f,200.f,-250.f,250.f,-300.f,300.f}) {
      if(footprint)break;
      ++supportTrials;
      const auto candidate=lip-direction*retreat+side*lateral;
      DynamicWorld::RayLandResult floor;
      if(!drySupport(candidate,floor))continue;
      bool supported=true;float lowest=floor.v.y,highest=floor.v.y;
      for(float across:{-extent.x,0.f,extent.x})for(float along:{-extent.z,0.f,extent.z}) {
        const auto probe=candidate+side*across+direction*along;
        DynamicWorld::RayLandResult support;
        if(!drySupport(probe,support)) {supported=false;continue;}
        lowest=std::min(lowest,support.v.y);highest=std::max(highest,support.v.y);
        }
      if(highest-lowest>10.f) {
        ++supportUneven;rejectSupport("height_span",candidate,floor,false,0);supported=false;
        }
      if(!supported)continue;
      at=candidate;floorPoint=floor.v;floorNormal=floor.n;chosenRetreat=retreat;chosenLateral=lateral;footprint=true;
      }
    event("water_wind_support","\"lip_cm\":"+ragdollVector(lip)+",\"shore_water_y_cm\":"+ragdollNumber(shoreWaterY)+
      ",\"found\":"+(footprint?"true":"false")+",\"retreat_cm\":"+(footprint?ragdollNumber(chosenRetreat):"null")+
      ",\"lateral_cm\":"+(footprint?ragdollNumber(chosenLateral):"null")+",\"floor_normal\":"+ragdollVector(floorNormal)+
      ",\"trials\":"+std::to_string(supportTrials)+",\"no_floor\":"+std::to_string(supportNoFloor)+
      ",\"steep_floor\":"+std::to_string(supportSteep)+",\"wet_floor\":"+std::to_string(supportWet)+
      ",\"height_span_failures\":"+std::to_string(supportUneven)+",\"first_rejections\":"+supportRejections+"]");
    if(!footprint) {arenaAbort("water barrel centre is unsupported at the real shoreline lip");return;}
    const auto up=Vec3::normalize(floorNormal);
    const auto axisX=Vec3::normalize(Vec3(1,0,0)-up*up.x);
    const auto axisZ=Vec3::crossProduct(axisX,up);
    Matrix4x4 matrix(axisX.x,up.x,axisZ.x,0,
                     axisX.y,up.y,axisZ.y,0,
                     axisX.z,up.z,axisZ.z,0,
                     0,0,0,1);
    auto rotatedCenter=center;matrix.project(rotatedCenter);
    float bottom=std::numeric_limits<float>::infinity();
    for(unsigned corner=0;corner<8;++corner) {
      Vec3 vertex(found->localBounds[corner&1].x,found->localBounds[(corner>>1)&1].y,found->localBounds[(corner>>2)&1].z);
      matrix.project(vertex);bottom=std::min(bottom,Vec3::dotProduct(vertex,up));
      }
    const float tx=at.x-rotatedCenter.x,tz=at.z-rotatedCenter.z;
    const float ty=(Vec3::dotProduct(floorPoint,up)+.5f-bottom-up.x*tx-up.z*tz)/up.y;
    matrix.set(3,0,tx);matrix.set(3,1,ty);matrix.set(3,2,tz);
    if(!world.physic()->setMovableVobTransform(zen,matrix,&player)) {arenaAbort("water shore barrel placement failed");return;}
    target=center;matrix.project(target);
    // The ordinary cone's impulse is 450*(1-distance/1000) Ns. Stay below
    // this native barrel's break threshold while still pushing it off the lip.
    const float safeDistance=std::clamp(1000.f*(1.f-.8f*found->impactThreshold/450.f),400.f,960.f);
    bool casterSupported=false;
    unsigned noFloor=0,steepFloor=0,wetFloor=0,occluded=0,rejectionCount=0;
    std::string rejections="[";
    const auto reject=[&](std::string_view why,const Vec3& candidate,const DynamicWorld::RayLandResult& floor,const Vec3* obstruction=nullptr) {
      if(rejectionCount>=3)return;
      if(rejectionCount++)rejections+=',';
      rejections+="{\"reason\":"+jsonStr(why)+",\"candidate_cm\":"+ragdollVector(candidate)+
        ",\"floor_found\":"+(floor.hasCol?"true":"false")+",\"floor_cm\":"+ragdollVector(floor.v)+
        ",\"floor_normal\":"+ragdollVector(floor.n)+",\"obstruction_cm\":"+(obstruction?ragdollVector(*obstruction):"null")+"}";
      };
    const auto actorCenterOffset=player.collosionCenter()-player.position();
    float chosenNativeDistance=0,chosenConeCosine=0;
    for(float distance=safeDistance;distance<=980.f && !casterSupported;distance+=10.f)
      for(unsigned a=0;a<64 && !casterSupported;++a) {
        const int turn=a==0?0:int((a+1)/2)*(a%2==1?1:-1);
        const float angle=float(turn)*float(M_PI)/32.f;
        const auto radial=direction*(-std::cos(angle))+side*std::sin(angle);
        // The real cone uses three-dimensional distance from the caster's
        // collision centre. A high quay needs a smaller horizontal radius.
        if(Vec3::dotProduct(radial,direction)>-.25f)continue;
        float horizontal=distance;
        DynamicWorld::RayLandResult stand;
        Vec3 candidate;bool converged=false;
        for(unsigned iteration=0;iteration<6;++iteration) {
          candidate=floorPoint+radial*horizontal;
          stand=shoreFloor(candidate);
          if(!stand.hasCol)break;
          const float height=stand.v.y+actorCenterOffset.y-target.y;
          const float along=Vec3::dotProduct(radial,actorCenterOffset);
          const float perpendicular=actorCenterOffset.x*actorCenterOffset.x+actorCenterOffset.z*actorCenterOffset.z;
          const float discriminant=along*along+distance*distance-height*height-perpendicular;
          if(discriminant<=0.f)break;
          const float next=std::sqrt(discriminant)-along;
          if(next<=0.f)break;
          if(std::abs(next-horizontal)<.25f) {converged=true;break;}
          horizontal=next;
          }
        if(!stand.hasCol) {++noFloor;reject("no_floor",candidate,stand);continue;}
        if(!converged || stand.n.y<.99f) {++steepFloor;reject("unsupported_cast_floor",candidate,stand);continue;}
        const auto standWater=world.physic()->waterRay(stand.v+Vec3(0,.1f,0),0);
        if(standWater.hasCol && standWater.wdepth>stand.v.y) {++wetFloor;reject("wet_floor",candidate,stand);continue;}
        const auto casterCenter=stand.v+actorCenterOffset;
        const auto obstruction=world.physic()->ray(casterCenter,target);
        if(obstruction.hasCol && (obstruction.v-target).length()>extent.length()+5.f) {
          ++occluded;reject("occluded",candidate,stand,&obstruction.v);continue;
          }
        auto firing=target-stand.v;firing.y=0;firing=Vec3::normalize(firing);
        const auto targetFloor=shoreFloor(target-firing*250.f);
        if(!targetFloor.hasCol || targetFloor.n.y<.99f) {++steepFloor;reject("unsupported_spell_target",candidate,stand);continue;}
        const auto targetWater=world.physic()->waterRay(targetFloor.v+Vec3(0,.1f,0),0);
        if(targetWater.hasCol && targetWater.wdepth>targetFloor.v.y) {++wetFloor;reject("wet_spell_target",candidate,stand);continue;}
        const auto axis=Vec3::normalize(targetFloor.v+actorCenterOffset-casterCenter);
        const float cosine=Vec3::dotProduct(Vec3::normalize(target-casterCenter),axis);
        if(cosine<.819152f) {++occluded;reject("outside_native_cone",candidate,stand);continue;}
        caster=stand.v;windDummy=targetFloor.v;casterSupported=true;
        chosenNativeDistance=(target-casterCenter).length();chosenConeCosine=cosine;
        }
    event("water_wind_site","\"lip_cm\":"+ragdollVector(lip)+",\"shore_water_y_cm\":"+ragdollNumber(shoreWaterY)+
      ",\"floor_cm\":"+ragdollVector(floorPoint)+",\"target_cm\":"+ragdollVector(target)+
      ",\"local_native_com_cm\":"+ragdollVector(center)+",\"extent_cm\":"+ragdollVector(extent)+
      ",\"safe_distance_cm\":"+ragdollNumber(safeDistance)+",\"caster_found\":"+(casterSupported?"true":"false")+
      ",\"native_caster_distance_cm\":"+ragdollNumber(chosenNativeDistance)+",\"native_cone_cosine\":"+ragdollNumber(chosenConeCosine)+
      ",\"ray_top_cm\":"+ragdollNumber(bench.waterRayTop)+
      ",\"no_floor\":"+std::to_string(noFloor)+",\"steep_floor\":"+std::to_string(steepFloor)+
      ",\"wet_floor\":"+std::to_string(wetFloor)+",\"occluded\":"+std::to_string(occluded)+",\"first_rejections\":"+rejections+"]");
    if(!casterSupported) {arenaAbort("water safe wind caster requires dry support and a clear native sight line");return;}
    const auto splashSource=Vec3(floorPoint.x,shoreWaterY,floorPoint.z)+direction*(chosenRetreat+80.f);
    noiseRetireListeners();noiseBegin(world,"water-splash",splashSource);
    // Choose a supported listener outside the quiet dry-contact radius, but
    // within the real acoustic range of a splash from this low shoreline.
    Npc* listener=nullptr;unsigned listenerNoFloor=0,listenerWet=0,listenerBlocked=0;
    float listenerOcclusion=0,listenerDistance=0;
    for(float radius:{350.f,400.f,450.f,500.f,550.f})
      for(unsigned a=0;a<32 && !listener;++a) {
        const float angle=float(a)*float(M_PI)/16.f;
        const auto offset=direction*std::cos(angle)*radius+side*std::sin(angle)*radius;
        // Keep the observer out of the projectile's line through its target.
        if(std::abs(Vec3::dotProduct(offset,side))<150.f)continue;
        const auto listenerAt=shoreFloor(floorPoint+offset);
        if(!listenerAt.hasCol || listenerAt.n.y<.65f) {++listenerNoFloor;continue;}
        if((listenerAt.v-floorPoint).length()<=300.f)continue;
        const auto water=world.physic()->waterRay(listenerAt.v+Vec3(0,.1f,0),0);
        if(water.hasCol && water.wdepth>listenerAt.v.y) {++listenerWet;continue;}
        const auto head=listenerAt.v+Vec3(0,160,0);
        const float occlusion=world.physic()->soundOclusion(splashSource,head);
        const float distance=(splashSource-head).length();
        const float setupOcclusion=world.physic()->soundOclusion(bench.waterCenter,head);
        if(distance>=650.f*(1.f-occlusion) ||
           (bench.waterCenter-head).length()>=650.f*(1.f-setupOcclusion)) {++listenerBlocked;continue;}
        listener=noiseAddListener(world,"water-near",listenerAt.v);
        listenerOcclusion=occlusion;listenerDistance=distance;
        }
    event("water_wind_listener",std::string("\"found\":")+(listener?"true":"false")+
      ",\"source_cm\":"+ragdollVector(splashSource)+",\"distance_cm\":"+ragdollNumber(listenerDistance)+
      ",\"occlusion\":"+ragdollNumber(listenerOcclusion)+",\"no_floor\":"+std::to_string(listenerNoFloor)+
      ",\"wet_floor\":"+std::to_string(listenerWet)+",\"acoustically_blocked\":"+std::to_string(listenerBlocked));
    if(!listener) {
      arenaAbort("water splash requires a real nearby supported listener");return;
      }
    }
  auto direction=target-caster;direction.y=0;
  if(direction.length()<1.f) {arenaAbort("water spell target has no horizontal firing direction");return;}
  direction=Vec3::normalize(direction);
  auto dummy=target+direction*100.f-Vec3(0,80,0);
  if(wind)dummy=windDummy; // Already checked with the actual caster and native cone.
  auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),dummy);
  if(!npc) {arenaAbort("water real spell target insertion failed");return;}
  npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
  npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
  npc->updateTransform();bench.spellActors.insert(bench.spellActors.begin(),{npc,"water_target"});
  int32_t spell=-1;
  for(size_t i=0;i<world.script().symbolsCount();++i) {
    const auto* symbol=world.script().findSymbol(i);
    if(!symbol || symbol->type()!=zenkit::DaedalusDataType::INSTANCE)continue;
    auto name=symbol->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_"))continue;
    auto item=std::make_unique<Item>(world,i,Item::T_Inventory);
    const auto kind=world.script().physicalSpell(item->spellId());
    if(kind!=(wind?GameScript::PhysicalSpell::WindFist:GameScript::PhysicalSpell::Fireball))continue;
    spell=item->spellId();bench.spellRunes[spell]=i;player.addItem(i,item->isRune()?1:5);break;
    }
  if(spell<0) {arenaAbort("water fixture real spell rune is missing");return;}
  player.closeWeapon(true);player.setPosition(caster);player.setDirection(direction);player.updateTransform();
  player.handle().attribute[ATR_MANA]=player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  Gothic::settingsSetI("GAME","spell_physics",1);
  player.useItem(bench.spellRunes[spell],Item::NSLOT,true);
  if(player.drawSpell(spell))bench.spellCastingId=spell;
  else {bench.spellEquipPending=spell;bench.spellEquipUntil=world.tickCount()+3000;}
  auto cameraTarget=target;
  if(wind)cameraTarget.y=(target.y+bench.waterCenter.y)*.5f;
  bench.cam=C_Hold;
  if(!wind && bench.waterCase=="deep") {
    bench.cam=C_Water;bench.waterCameraTarget=cameraTarget;
    lookFrom(target+bench.waterDirection*380.f+Vec3(0,650,0),cameraTarget);
    }
  else
    lookFrom(caster+Vec3(-direction.z*300.f,210,direction.x*300.f),cameraTarget);
  event("water_spell_ready","\"spell_kind\":"+jsonStr(wind?"wind":"fireball")+",\"spell_id\":"+
    std::to_string(spell)+",\"zen_id\":"+std::to_string(zen)+",\"target_cm\":"+ragdollVector(target)+
    ",\"caster_cm\":"+ragdollVector(caster)+",\"dummy_cm\":"+ragdollVector(dummy));
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
  for(const auto& actor:bench.waterActors) {
    if(actors.size()>1)actors+=',';
    actors+="{\"role\":"+jsonStr(actor.role)+",\"dead\":"+(actor.npc->isDead()?"true":"false")+
      ",\"swimming\":"+(actor.npc->isSwim()?"true":"false")+
      ",\"last_hit_spell\":"+std::to_string(actor.npc->lastHitSpellId())+
      ",\"ragdoll\":"+(actor.npc->ragdollBody()?"true":"false")+","+waterFilmPosition(world,actor.npc->position())+"}";
    }
  for(size_t i=0;i<bench.waterItems.size();++i) {
    if(i)items+=',';
    items+="{\"label\":"+jsonStr(std::array{"sword","ore","lute","arrow"}[i])+","+
      waterFilmPosition(world,bench.waterItems[i]->midPosition())+"}";
    }
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(body.category!=DynamicWorld::C_Fragment || !bench.waterIds.contains(body.parentId))continue;
    if(fragments.size()>1)fragments+=',';
    fragments+="{\"parent\":"+jsonStr(bench.waterLabels[body.parentId])+",\"piece\":"+std::to_string(body.piece)+
      ",\"submerged_fraction\":"+ragdollNumber(body.submergedFraction)+","+
      waterFilmPosition(world,(body.worldBounds[0]+body.worldBounds[1])*.5f)+"}";
    }
  event("water_film_checkpoint","\"scene\":"+jsonStr(bench.waterFilm)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-bench.waterFilmStart)+",\"actors\":"+actors+"],\"items\":"+items+
    "],\"props\":"+destructionRows(world)+",\"fragments\":"+fragments+"],\"bodies\":"+waterRows(world));
  }

void waterFilmTick(World& world) {
  if(!bench.waterFilmKill || bench.waterActors.empty())return;
  auto& npc=*bench.waterActors.front().npc;
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
    for(const auto& actor:bench.waterActors) {
      WaterFilmPixels pixels(camera->viewProj(),width,height);
      const auto& npc=*actor.npc;
      if(const auto* skeleton=npc.visualSkeleton())
        for(size_t i=0;i<skeleton->nodes.size();++i) {
          if(!skeleton->nodes[i].name.starts_with("BIP01"))continue;
          Vec3 point{};npc.renderedBone(i).project(point);pixels.point(point);
          }
      row(actor.role,"rendered_bones",pixels);
      }
  for(size_t i=0;i<bench.waterItems.size();++i) {
    WaterFilmPixels pixels(camera->viewProj(),width,height);
    pixels.box(bench.waterItems[i]->bBox(),bench.waterItems[i]->transform());
    row(std::array{"sword","ore","lute","arrow"}[i],"native_mesh_bbox",pixels);
    }
  if(bench.waterFilm=="fireball")
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(!bench.waterIds.contains(body.id) || body.broken)continue;
      WaterFilmPixels pixels(camera->viewProj(),width,height);
      if(const auto* mesh=Resources::loadMesh(body.visual))pixels.box(mesh->bbox(),body.transform);
      row(bench.waterLabels[body.id],"native_mesh_bbox",pixels);
      }
  event("water_film_framing","\"scene\":"+jsonStr(bench.waterFilm)+",\"elapsed_ms\":"+
    std::to_string(bench.time-bench.recStart)+",\"viewport_px\":["+std::to_string(width)+","+
    std::to_string(height)+"],\"objects\":"+objects+"]");
  }

void waterFilmSetup(World& world,Npc& player,std::string_view scene) {
  if(scene!="body" && scene!="skeleton" && scene!="fireball" && scene!="things") {
    arenaAbort("water-film setup expects body, skeleton, fireball or things");return;
    }
  if(bench.spellRunes.empty())prepareSpellFixture(world,player,"wind");
  if(bench.spellRunes.empty())return;
  player.setTarget(nullptr);player.closeWeapon(true);player.clearAiQueue();player.clearGoTo();player.stopWalking();player.clearSpeed();
  bench.subject=nullptr;bench.crew.clear();bench.waterActors.clear();bench.waterFilmKill=false;
  bench.spellTrace=false;bench.spellGuardTrace=false;bench.spellReleaseAt=0;bench.spellEquipPending=0;
  for(const auto& actor:bench.spellActors)world.removeNpc(*actor.npc);
  for(auto* item:bench.spellItems)world.removeItem(*item);
  for(auto* item:bench.waterItems)world.removeItem(*item);
  bench.spellActors.clear();bench.spellItems.clear();bench.waterItems.clear();
  bench.destructionFixtures.clear();bench.destructionIds.clear();bench.movableIds.clear();
  bench.waterIds.clear();bench.waterLabels.clear();bench.waterReference.clear();bench.waterTravel.clear();bench.waterSleepAt.clear();
  bench.destructionTrace=false;
  std::vector<Vec3> places;
  if(!waterFindWindSite(world,places) || bench.waterWaypoint!="NW_CITY_HABOUR_05") {
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
  auto lip=bench.waterDry;
  const auto direction=bench.waterDirection;
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
    if(slots) {bench.waterCenter=surface;deep=true;break;}
    }
  if(!deep) {arenaAbort("water film needs deep water 1-2m from the actual waterline");return;}
  bench.waterDry=lip;bench.waterDirection=direction;
  const auto side=Vec3(-direction.z,0,direction.x);
  bench.waterFilmLip=lip;bench.waterFilm=scene;bench.waterCase="film-"+std::string(scene);
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
    if(!waterBodiesAdd(world,player,scene=="skeleton"?"SKELETON":"BAU_904_BAUER",std::string(scene),lip,false))return;
    auto* target=bench.waterActors.back().npc;
    if(scene=="fireball") {target->setPosition(spellTarget);target->updateTransform();}
    target->handle().attribute[ATR_HITPOINTS]=target->handle().attribute[ATR_HITPOINTSMAX]=50000;
    bench.spellActors.push_back({target,"water_target"});
    }
  if(scene=="fireball") {
    // Fresh owned vobs keep this dry authored pose when movable is disabled.
    // The real projectile hits the actor in the gap; only its ordinary blast
    // can break the two props or send their fragments over the edge.
    for(unsigned i=0;i<2;++i) {
      const auto id=destructionAdd(world,visuals[i],supports[i]);
      if(!id) {arenaAbort("water film fresh crate/barrel insertion failed");return;}
      bench.waterIds.insert(id);bench.waterLabels[id]=i?"barrel":"crate";
      }
    }
  bench.waterReady=true;bench.waterTrace=true;bench.waterStart=world.tickCount();bench.waterSample=uint64_t(-1);
  bench.noiseTrace=true;bench.noisePhase="water-film-"+std::string(scene);
  const bool bodies=scene=="body" || scene=="skeleton";
  // Broadside close shots keep the surface legible. Do not widen the body
  // shot to chase the stock corpse ten metres away from its buoyant partner.
  Vec3 focus,eye;
  bool camera=false;
  if(bodies) {
    focus=lip+direction*450.f;focus.y=bench.waterCenter.y;
    for(float lateral:{420.f,-420.f}) {
      eye=focus+side*lateral+Vec3(0,160,0);
      if(world.physic()->ray(eye,bench.waterCenter+Vec3(0,20,0)).hasCol ||
         world.physic()->ray(eye,lip+Vec3(0,110,0)).hasCol)continue;
      camera=true;break;
      }
    }
  else if(scene=="fireball") {
    const float pierY=(supports[0].y+supports[1].y)*.5f;
    // Behind the target and above its head: it stays low in the frame and the
    // crate, the barrel and the water where their fragments land stay in view.
    focus=lip+direction*150.f;focus.y=bench.waterCenter.y;
    for(float lateral:{0.f,-40.f,40.f}) {
      eye=lip-direction*480.f+side*lateral;eye.y=pierY+330.f;
      bool clear=!world.physic()->ray(eye,bench.waterCenter+Vec3(0,20,0)).hasCol;
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
    focus=bench.waterCenter+Vec3(0,3,0);
    eye=bench.waterCenter-direction*210.f+Vec3(0,100,0);
    camera=true;
    for(unsigned i=0;i<4;++i)
      if(world.physic()->ray(eye,bench.waterCenter+side*(float(i)-1.5f)*65.f+Vec3(0,5,0)).hasCol)camera=false;
    }
  if(!camera) {arenaAbort("water film camera has no clear close side view");return;}
  bench.cam=C_Water;bench.waterCameraTarget=focus;lookFrom(eye,focus);
  event("water_film_setup","\"scene\":"+jsonStr(scene)+",\"waypoint\":"+jsonStr(bench.waterWaypoint)+
    ",\"lip_cm\":"+ragdollVector(lip)+",\"water_cm\":"+ragdollVector(bench.waterCenter)+
    ",\"caster_cm\":"+ragdollVector(caster)+",\"camera_cm\":"+ragdollVector(bench.camPos)+
    ",\"camera_target_cm\":"+ragdollVector(focus));
  }

void waterFilmBegin(World& world,Npc& player) {
  bench.waterFilmStart=world.tickCount();
  bench.waterFilmKill=bench.waterFilm=="body" || bench.waterFilm=="skeleton";
  if(bench.waterFilm=="things") {
    const auto side=Vec3(-bench.waterDirection.z,0,bench.waterDirection.x);
    const char* symbols[]{"ITMW_1H_SLD_SWORD","ITMI_NUGGET","ITMI_LUTE","ITRW_ARROW"};
    for(unsigned i=0;i<4;++i) {
      auto symbol=world.script().findSymbolIndex(symbols[i]);
      if(symbol==size_t(-1) && i==0)
        for(size_t candidate=0;candidate<world.script().symbolsCount();++candidate) {
          const auto* entry=world.script().findSymbol(candidate);
          if(entry && entry->type()==zenkit::DaedalusDataType::INSTANCE && entry->name().starts_with("ITMW_") &&
             entry->name().find("SWORD")!=std::string::npos) {symbol=candidate;break;}
          }
      if(symbol==size_t(-1)) {arenaAbort("water film item instance missing");return;}
      Vec3 surface;
      if(!waterProbe(world,bench.waterCenter+side*(float(i)-1.5f)*65.f,220.f,6000.f,surface)) {
        arenaAbort("water film item drop needs four adjacent deep-water slots");return;
        }
      const auto before=world.physic()->waterBodySnapshot();
      auto matrix=Matrix4x4::mkIdentity();matrix.translate(surface+Vec3(0,100,0));
      auto* item=world.addItemDyn(symbol,matrix,world.npcId(&player));
      if(!item) {arenaAbort("water film native item drop failed");return;}
      bench.waterItems.push_back(item);
      for(const auto& body:world.physic()->waterBodySnapshot())
        if(body.category==DynamicWorld::C_Item &&
           std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {
          bench.waterIds.insert(body.id);bench.waterLabels[body.id]=std::array{"sword","ore","lute","arrow"}[i];
          }
      }
    }
  event("water_film_begin","\"scene\":"+jsonStr(bench.waterFilm));
  }

void arrowFields(World& world) {
  auto* npc=bench.arrowNpc;
  if(!bench.arrowTrace || npc==nullptr || world.tickCount()==bench.arrowSampleTick) return;
  bench.arrowSampleTick=world.tickCount();
  const auto& hit=npc->arrowPhysicsProbe();
  bench.arrowSeenHits=hit.hits;
  const auto* living=npc->livingBody();const auto* corpse=npc->ragdollBody();
  const auto* body=living!=nullptr ? living : corpse;
  const auto* skeleton=npc->visualSkeleton();
  const auto physics=world.physic()->physicsStats();
  std::vector<Matrix4x4> native;
  const bool posed=body!=nullptr && body->getPose(native);
  std::string bones="[",active="[";
  if(body!=nullptr) {
    for(const auto bone:body->reactionBones()) {
      if(active.size()>1) active+=',';
      active+=std::to_string(bone);
      }
    if(posed && skeleton!=nullptr)
      for(const auto bone:body->boneIndices()) {
        if(bone>=native.size() || bone>=skeleton->nodes.size()) continue;
        if(bones.size()>1) bones+=',';
        const auto parent=skeleton->nodes[bone].parent;
        bones+="{\"id\":"+std::to_string(bone)+",\"name\":"+jsonStr(skeleton->nodes[bone].name)+
          ",\"parent\":"+(parent<skeleton->nodes.size() ? std::to_string(parent) : "null")+
          ",\"native\":"+ragdollMatrix(native[bone])+",\"animation\":"+ragdollMatrix(npc->animationBone(bone));
        if(parent<native.size())
          bones+=",\"native_parent\":"+ragdollMatrix(native[parent])+",\"animation_parent\":"+ragdollMatrix(npc->animationBone(parent));
        bones+='}';
        }
    }
  const auto& flinch=Flinch::subjectProbe;
  event("arrow_reaction_frame","\"case\":"+jsonStr(bench.arrowCase)+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"feature\":"+(Gothic::settingsGetI("GAME","arrow_physics") ? "true" : "false")+
    ",\"root_cm\":"+ragdollVector(npc->position())+
    ",\"dead\":"+(npc->isDead()?"true":"false")+",\"ko\":"+(npc->isUnconscious()?"true":"false")+
    ",\"living_parts\":"+std::to_string(living ? living->partCount() : 0)+
    ",\"corpse_parts\":"+std::to_string(corpse ? corpse->partCount() : 0)+
    ",\"world_bodies\":"+std::to_string(physics.bodies)+",\"physics_ms\":"+ragdollNumber(physics.stepMilliseconds)+
    ",\"kinematic\":"+(body!=nullptr && body->isKinematic()?"true":"false")+
    ",\"weight\":"+ragdollNumber(body ? body->reactionWeight() : 0.f)+
    ",\"reaction_bones\":"+active+"]"+
    ",\"hits\":"+std::to_string(hit.hits)+",\"applied\":"+std::to_string(hit.applied)+
    ",\"corpse_impulses\":"+std::to_string(hit.corpseImpulses)+
    ",\"hit_bone\":"+(hit.bone!=size_t(-1) ? std::to_string(hit.bone) : "null")+
    ",\"point_cm\":"+ragdollVector(hit.point)+",\"impulse_ns\":"+ragdollVector(hit.impulseNs)+
    ",\"strength\":"+ragdollNumber(hit.strength)+
    ",\"flinch_active\":"+(flinch.active?"true":"false")+",\"flinch_hits\":"+std::to_string(flinch.hits)+
    ",\"bones\":"+bones+"]");
  }

void livingSeatFields(World& world) {
  auto* npc=bench.seatNpc;
  if(npc==nullptr || bench.seatPhase.empty() || bench.seatSampleTick==world.tickCount()) return;
  const float seconds=float(world.tickCount()-bench.seatSampleTick)*.001f;
  bench.seatSampleTick=world.tickCount();
  std::vector<Matrix4x4> pose;
  const auto* body=npc->livingBody();
  float nativeStep=0,corpseSpeed=0,itemSpeed=0;
  if(body && body->getPose(pose)) {
    if(bench.seatPreviousParts.size()==pose.size())
      for(auto bone:body->boneIndices()) nativeStep=std::max(nativeStep,(Vec3(pose[bone].at(3,0),pose[bone].at(3,1),pose[bone].at(3,2))-bench.seatPreviousParts[bone]).length());
    bench.seatPreviousParts.resize(pose.size());
    for(auto bone:body->boneIndices()) bench.seatPreviousParts[bone]={pose[bone].at(3,0),pose[bone].at(3,1),pose[bone].at(3,2)};
    }
  else {
    bench.seatPreviousParts.clear();
    }
  const auto* corpse=bench.seatCorpse ? bench.seatCorpse->ragdollBody() : nullptr;
  if(corpse) for(const auto& part:corpse->state().parts) corpseSpeed=std::max(corpseSpeed,part.linearVelocity.length());
  std::string items="[";
  for(size_t i=0;i<bench.seatItems.size();++i) {
    const auto p=bench.seatItems[i]->position();
    itemSpeed=std::max(itemSpeed,bench.seatItems[i]->physicsVelocity().length());
    if(bench.seatPreviousItems.size()==bench.seatItems.size() && seconds>0)
      itemSpeed=std::max(itemSpeed,(p-bench.seatPreviousItems[i]).length()/seconds);
    if(i) items+=',';
    items+=ragdollVector(p);
    }
  bench.seatPreviousItems.clear();
  for(const auto* item:bench.seatItems) bench.seatPreviousItems.push_back(item->position());
  const auto* mob=npc->interactive();
  event("living_seat_frame","\"phase\":"+jsonStr(bench.seatPhase)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"attached\":"+(mob==bench.seatMob?"true":"false")+",\"seated\":"+(mob==bench.seatMob && npc->bodyStateMasked()==BS_SIT?"true":"false")+
    ",\"mob_state\":"+std::to_string(mob?mob->stateId():-99)+",\"animations\":"+jsonStr(npc->animNames())+
    ",\"living_parts\":"+std::to_string(body?body->partCount():0)+",\"corpse_parts\":"+std::to_string(corpse?corpse->partCount():0)+
    ",\"native_step_cm\":"+ragdollNumber(nativeStep)+",\"corpse_speed_cm_s\":"+ragdollNumber(corpseSpeed)+
    ",\"item_speed_cm_s\":"+ragdollNumber(itemSpeed)+",\"items_cm\":"+items+"]");
  }

void questFields(World& world, Npc& npc, bool csv) {
  const auto& q=npc.ragdollQuestReturnState();
  const auto pos=npc.position();
  auto pelvis=pos;
  if(const auto* body=npc.ragdollBody()) {
    const auto matrix=body->pelvisTransform();pelvis={matrix.at(3,0),matrix.at(3,1),matrix.at(3,2)};
    }
  const auto water=world.physic()->waterRay(pelvis,0);
  const bool submerged=water.hasCol && pelvis.y<water.wdepth-5;
  if(bench.questLaunched) {
    bench.questSawDrop|=bench.questStable.y-pelvis.y>RagdollSave::QuestReturn::maxFall;
    bench.questSawWater|=submerged;
    }
  const auto count=npc.ragdollQuestReturnCount();
  if(count>bench.questLastReturns)
    event("immortal_guard_return","\"case\":"+jsonStr(bench.questCase)+",\"reason\":"+jsonStr(questReason(q.hazards))+
      ",\"from_cm\":"+ragdollVector(npc.ragdollQuestReturnFrom())+",\"to_cm\":"+ragdollVector(pos)+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"count\":"+std::to_string(count));
  bench.questLastReturns=count;
  if(!csv) return;
  const auto now=std::chrono::steady_clock::now().time_since_epoch();
  const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  const double wall=bench.questWallNs ? double(ns-bench.questWallNs)/1e6 : 0;bench.questWallNs=ns;
  std::fprintf(bench.csv,",%d,%d,%d,%d,%u,%u,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%.6f",
    int(npc.isImmortal()),Gothic::settingsGetI("GAME","ragdoll_quest_guard"),int(q.hasAnchor),int(q.armed),unsigned(q.hazards),count,
    questReason(q.hazards),double(q.anchor.x),double(q.anchor.y),double(q.anchor.z),double(pos.x),double(pos.y),double(pos.z),
    double(bench.questStable.y-pos.y),int(submerged),int(npc.isUnconscious()),wall);
  event("immortal_guard_frame","\"case\":"+jsonStr(bench.questCase)+
    ",\"actor_id\":"+std::to_string(world.npcId(&npc))+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"immortal\":"+(npc.isImmortal()?"true":"false")+",\"enabled\":"+(Gothic::settingsGetI("GAME","ragdoll_quest_guard")?"true":"false")+
    ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+
    ",\"hazards\":"+std::to_string(q.hazards)+",\"returns\":"+std::to_string(count)+",\"reason\":"+jsonStr(questReason(q.hazards))+
    ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"position_cm\":"+ragdollVector(pos)+",\"pelvis_cm\":"+ragdollVector(pelvis)+
    ",\"water_y_cm\":"+(water.hasCol?ragdollNumber(water.wdepth):"null")+",\"in_water\":"+(submerged?"true":"false")+
    ",\"unconscious\":"+(npc.isUnconscious()?"true":"false")+",\"dead\":"+(npc.isDead()?"true":"false")+
    ",\"parts\":"+std::to_string(npc.ragdollBody()?npc.ragdollBody()->partCount():0));
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
  // flinch of the watched one (or the hero)
  const auto& fl = bench.subject!=nullptr ? Flinch::subjectProbe : Flinch::playerProbe;
  std::fprintf(bench.csv,",%d,%.2f,%.1f,%.2f,%.2f,%u,%.2f,%.2f",int(fl.active),fl.bend,fl.bendYaw,fl.twist,fl.head,
               fl.hits,fl.strength,fl.height);
  // hit points: the hero's, the watched one's (a fight's course)
  std::fprintf(bench.csv,",%d,%d",pl.attribute(ATR_HITPOINTS),bench.subject!=nullptr ? bench.subject->attribute(ATR_HITPOINTS) : -1);
  // the watched npc's archer by hand (game/archerai)
  const auto& ai = ArcherAi::subjectProbe;
  std::fprintf(bench.csv,",%d,%d,%.3f,%.3f,%.3f,%.0f,%.2f,%.2f",int(ai.valid && ai.active),int(ai.want),ai.sigma,ai.err.x,ai.err.y,
               ai.lead,ai.pv,ai.ps);
  if(ragdollTrace)
    writeRagdoll(pl);
  if(bench.questTrace && bench.questNpc!=nullptr)
    questFields(*Gothic::inst().world(),*bench.questNpc,true);
  if(bench.livingFixture)
    livingFields(*Gothic::inst().world());
  livingSeatFields(*Gothic::inst().world());
  std::fprintf(bench.csv,"\n");
  }

void exec(const std::string& line, World& w, Npc& pl, PlayerControl& ctrl);

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

void arenaAbort(std::string_view reason) {
  say("ragdoll arena failed: "+std::string(reason));
  event("ragdoll_error","\"reason\":"+jsonStr(reason));
  bench.pc=bench.prog.size(); bench.waitUntil=0;
  }

bool chooseRagdollArena(World& world, Npc& player) {
  if(!bench.crew.empty()) return false;
  std::vector<const WayPoint*> candidates;
  world.findWayPoint(player.position(),[&](const WayPoint& wp) { candidates.push_back(&wp); return false; });
  const auto origin=player.position();
  std::sort(candidates.begin(),candidates.end(),[&](const auto* a,const auto* b) {
    const auto da=(a->pos-origin).quadLength(),db=(b->pos-origin).quadLength();
    return da==db ? a->name<b->name : da<db;
    });
  size_t rejected=0;
  for(const auto* waypoint:candidates) {
    std::vector<Vec3> places;
    float lowest=std::numeric_limits<float>::infinity(),highest=-lowest;
    bool valid=true;
    for(int i=0;i<20 && valid;++i) {
      Vec3 place=waypoint->pos+Vec3(float(i%5)*250.f-500.f,0,float(i/5)*250.f-375.f);
      const auto floor=world.physic()->landRay(place+Vec3(0,300,0),900);
      if(!floor.hasCol || floor.n.y<.98f) { valid=false; break; }
      place.y=floor.v.y;
      for(float dx:{-150.f,0.f,150.f}) for(float dz:{-150.f,0.f,150.f}) {
        const auto support=world.physic()->landRay(place+Vec3(dx,300,dz),600);
        if(!support.hasCol || support.n.y<.98f || std::abs(support.v.y-place.y)>20.f) valid=false;
        }
      if(world.physic()->ray(place+Vec3(0,40,0),place+Vec3(0,3200,0)).hasCol) valid=false;
      lowest=std::min(lowest,place.y); highest=std::max(highest,place.y);
      places.push_back(place);
      }
    if(!valid || places.size()!=20 || highest-lowest>40.f) { ++rejected; continue; }
    const Vec3 target{waypoint->pos.x,(lowest+highest)*.5f+80.f,waypoint->pos.z};
    const Vec3 eye=target+Vec3(0,2600,1100);
    for(const auto& place:places)
      if(hidden(world,eye,place+Vec3(0,30,0))) valid=false;
    if(!valid) { ++rejected; continue; }
    bench.ragdollArenaPlaces=std::move(places); bench.ragdollArenaWaypoint=waypoint->name;
    player.setPosition(target+Vec3(0,-80,-1200)); player.updateTransform();
    bench.ragdollArenaTarget=target;
    event("ragdoll_arena_site","\"waypoint\":"+jsonStr(waypoint->name)+",\"geometry_rejections\":"+
          std::to_string(rejected)+",\"center\":"+ragdollVector(target)+",\"ground_min_cm\":"+
          std::to_string(lowest)+",\"ground_max_cm\":"+std::to_string(highest)+",\"places\":20");
    return true;
    }
  return false;
  }

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
    bench.ragdollArenaRadii.push_back(bounds.rConservative+std::sqrt(3.f)*50.f);
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
  bench.ragdollArenaTarget=(high+low)*.5f;
  bench.camPos=bench.ragdollArenaTarget+Vec3(0,.9238795f*distance,.3826834f*distance);
  bench.cam=C_RagdollArena; bench.ragdollArenaCamera=true; bench.camAuto=false; bench.smValid=false;
  lookFrom(bench.camPos,bench.ragdollArenaTarget);
  event("ragdoll_arena_camera","\"origin\":"+ragdollVector(bench.camPos)+",\"target\":"+
        ragdollVector(bench.ragdollArenaTarget)+",\"bounds_min\":"+ragdollVector(low)+",\"bounds_max\":"+
        ragdollVector(high)+",\"fov_y\":"+std::to_string(camera->fovY())+",\"aspect\":"+std::to_string(aspect));
  return true;
  }

bool arenaFullSchema(World& world, std::string_view mode) {
  if(bench.crew.size()!=20 || !bench.ragdollArenaCamera) return false;
  std::unordered_set<size_t> ids;
  std::vector<uint32_t> topology;
  for(const auto* npc:bench.crew) {
    const auto* skeleton=npc->visualSkeleton(); const auto* body=npc->ragdollBody();
    if(!ids.insert(world.npcId(npc)).second || skeleton==nullptr || skeleton->name()!="HUMANS.MDS" ||
       symName(world,npc)!="BDT_1013_BANDIT_L" || !npc->isDead() || !arenaMeshVisible(world,*npc)) return false;
    if(mode=="disabled") { if(body!=nullptr) return false; continue; }
    if(body==nullptr || body->partCount()!=24 || body->boneIndices().size()!=24) return false;
    const auto& bones=body->boneIndices();
    if(topology.empty()) topology.assign(bones.begin(),bones.end());
    if(!std::equal(topology.begin(),topology.end(),bones.begin())) return false;
    }
  return true;
  }

// Stage 5 fixtures select stock geometry before driving real player keys. No
// per-frame teleport is used on a route, and a missing fixture is an error.
bool characterFloor(World& world,const Vec3& at,Vec3& floor,float depth=600.f) {
  const auto hit=world.physic()->landRay(at+Vec3(0,180,0),depth);
  // C_Object floors (including city stair meshes) intentionally return a zero
  // normal through the old land query. Height sampling below verifies the path.
  if(!hit.hasCol || (hit.n.length()>0 && hit.n.y<.35f)) return false;
  floor=hit.v;return true;
  }

bool characterLane(World& world,const Vec3& start,const Vec3& end,float maximumStep=35.f) {
  const auto delta=end-start;
  Vec3 previous=start;
  for(unsigned i=0;i<=12;++i) {
    Vec3 floor;
    if(!characterFloor(world,start+delta*(float(i)/12.f),floor) || std::abs(floor.y-previous.y)>maximumStep) return false;
    if(i && world.physic()->ray(previous+Vec3(0,110,0),floor+Vec3(0,110,0)).hasCol) return false;
    previous=floor;
    }
  return true;
  }

bool characterStairsProfile(World& world,const Vec3& low,const Vec3& high,unsigned& risers,float& maximumRiser,
                            float minimumRiser=6.f,float minimumRise=45.f) {
  risers=0;maximumRiser=0;unsigned plateaus=0;
  Vec3 previous=low;
  for(unsigned i=1;i<=40;++i) {
    Vec3 floor;
    if(!characterFloor(world,low+(high-low)*(float(i)/40.f),floor))return false;
    const float rise=floor.y-previous.y;
    if(rise< -2.f || rise>32.f)return false;
    if(rise>=minimumRiser) {++risers;maximumRiser=std::max(maximumRiser,rise);}
    if(std::abs(rise)<2.f)++plateaus;
    previous=floor;
    }
  // Several discrete risers and level treads distinguish stairs from one
  // furniture top or a sloping ramp picked by the broad IK roughness probe.
  return risers>=3 && plateaus>=8 && high.y-low.y>=minimumRise;
  }

void characterFreezeNear(World& world,Npc& player,const Vec3& start,const Vec3& goal) {
  size_t count=0;
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player &&
       ((npc->position()-start).length()<2200.f || (npc->position()-goal).length()<2200.f)) {
      // Match the existing `freeze` bench command before the teleport's first
      // native tick, so automatic important dialogs cannot consume player keys.
      npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);++count;
      }
  event("character_fixture_freeze","\"npcs\":"+std::to_string(count)+",\"radius_cm\":2200");
  }

void characterIdleGuards(World& world,const Npc& player) {
  size_t count=0;
  // A loaded archive does not retain external control. Guard the full ten
  // second idle window against nearby creatures, without relying on one
  // archive's NPC indices or changing the measured human's state or pose.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && !npc->isHuman() &&
       (npc->position()-player.position()).length()<10000.f) {
      npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);++count;
      }
  event("character_idle_guards","\"creatures\":"+std::to_string(count)+",\"radius_cm\":10000");
  }

bool prepareCharacterRoute(World& world,Npc& player,const std::string& name,bool abortMissing=true,std::string_view onlyWaypoint={}) {
  Bench::CharacterRoute route;route.name=name;
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && (onlyWaypoint.empty() || point.name==onlyWaypoint)) points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  auto* physics=world.physic();
  const auto& guild=world.script().guildVal();
  const float legacySlideAngle=float(guild.slide_angle[player.guild()]);
  const float legacySlideEnd=float(guild.slide_angle2[player.guild()]);
  const float steepMinimum=std::max(40.f,legacySlideAngle+1.f);
  const float steepMaximum=std::min(65.f,89.f-legacySlideEnd);
  const auto city=[](const WayPoint& point) {return point.name.starts_with("NW_CITY_");};
  const auto harbour=[](const WayPoint& point) {
    return point.name.starts_with("NW_CITY_") && (point.name.find("HABOUR")!=std::string::npos ||
      point.name.find("HARBOUR")!=std::string::npos || point.name.find("HARBOR")!=std::string::npos);
    };
  if(name=="port_edge")
    std::stable_partition(points.begin(),points.end(),[](const auto* point){return point->name=="NW_CITY_HABOUR_POOR_AREA_PATH_01_03";});
  bool found=false;
  if(name=="stairs_up" || name=="stairs_down" || name=="steep_slope") {
    const bool stairs=name!="steep_slope";
    for(const auto* point:points) {
      if(stairs && (!city(*point) || point->name.find("BED")!=std::string::npos)) continue;
      const std::vector<float> offsets=stairs?std::vector<float>{0.f,-80.f,80.f}:std::vector<float>{0.f,-120.f,120.f,-240.f,240.f};
      for(const float x:offsets)for(const float z:offsets) {
        if(found)continue;
        Vec3 center;
        if(!characterFloor(world,point->position()+Vec3(x,0,z),center))continue;
        float angle=0,rough=0;Vec3 downhill;
        if(!slopeAt(world,center,angle,downhill,rough)) continue;
        if(stairs ? !(angle>=18 && angle<=40 && rough>=6 && rough<=25) : !(angle>=steepMinimum && angle<=steepMaximum && rough<6)) continue;
        Vec3 low,high;
        if(!characterFloor(world,center+downhill*100.f,low) || !characterFloor(world,center-downhill*100.f,high)) continue;
        unsigned risers=0;float maximumRiser=0;
        if(stairs && (!characterLane(world,low,high,32) || !characterStairsProfile(world,low,high,risers,maximumRiser)))continue;
        if(!stairs && high.y-low.y<100) continue;
        if(!stairs) {
          const auto surface=physics->landRay(center+Vec3(0,20,0),50);
          const float k=float(M_PI)/180.f;
          if(!surface.hasCol || !(std::sin(legacySlideEnd*k)<surface.n.y && surface.n.y<std::cos(legacySlideAngle*k)))continue;
          }
        route.waypoint=point->name;
        // Enter the steep route on its measured surface and face downhill.
        // Starting at its low foot instead only tests walking into a cliff.
        route.start=stairs ? (name=="stairs_down" ? high : low) : center;
        route.goal=stairs ? (name=="stairs_down" ? low : high) : low;
        route.tolerance=55;found=true;
        event("character_route_surface","\"route\":"+jsonStr(name)+",\"angle_deg\":"+ragdollNumber(angle)+
          ",\"roughness_cm\":"+ragdollNumber(rough)+",\"riser_count\":"+std::to_string(risers)+
          ",\"maximum_riser_cm\":"+ragdollNumber(maximumRiser)+",\"legacy_slide_angle_deg\":"+ragdollNumber(legacySlideAngle)+
          ",\"legacy_slide_end_deg\":"+ragdollNumber(legacySlideEnd)+",\"sample_cm\":"+ragdollVector(center));
        }
      if(found)break;
      }
    }
  else if(name=="port_edge") {
    for(const auto* point:points) {
      if(!harbour(*point)) continue;
      for(float x:{0.f,-80.f,80.f,-160.f,160.f})for(float z:{0.f,-80.f,80.f,-160.f,160.f}) {
        if(found)continue;
        Vec3 center;if(!characterFloor(world,point->position()+Vec3(x,0,z),center)) continue;
        for(unsigned i=0;i<24 && !found;++i) {
          const float angle=float(i)*float(M_PI)/12.f;
          const Vec3 along(std::cos(angle),0,std::sin(angle)),side(-along.z,0,along.x);
          Vec3 end;
          if(!characterFloor(world,center+along*300.f,end) || !characterLane(world,center,end,20) || std::abs(end.y-center.y)>30) continue;
          bool nearEdge=true;
          for(float distance:{0.f,150.f,300.f}) {
            Vec3 floor;
            if(!characterFloor(world,center+along*distance,floor)) {nearEdge=false;break;}
            // A missing floor beyond a wall is not an exposed harbour edge.
            const auto from=floor+Vec3(0,80,0),to=from+side*95.f;
            const auto edge=physics->landRay(to,3000);
            if(physics->ray(from,to).hasCol || !edge.hasCol || floor.y-edge.v.y<=120.f) {nearEdge=false;break;}
            for(float width:{-25.f,25.f}) {
              Vec3 support;
              if(!characterFloor(world,floor+side*width,support) || std::abs(support.y-floor.y)>20.f)nearEdge=false;
              }
            if(!nearEdge)break;
            }
          if(!nearEdge) continue;
          route.waypoint=point->name;route.start=center;route.goal=end;route.tolerance=70;found=true;
          }
        }
      if(found)break;
      }
    }
  else if(name=="doorway") {
    for(const auto* point:points) {
      if(!city(*point))continue;
      Vec3 center;if(!characterFloor(world,point->position(),center)) continue;
      for(unsigned i=0;i<12 && !found;++i) {
        const float angle=float(i)*float(M_PI)/12.f;
        const Vec3 direction(std::cos(angle),0,std::sin(angle)),side(-direction.z,0,direction.x);
        for(float shift:{-160.f,-80.f,0.f,80.f,160.f}) {
          const auto doorway=center+direction*shift;
          const auto left=physics->ray(doorway+Vec3(0,110,0),doorway+side*130.f+Vec3(0,110,0));
          const auto right=physics->ray(doorway+Vec3(0,110,0),doorway-side*130.f+Vec3(0,110,0));
          if(!left.hasCol || !right.hasCol)continue;
          const float width=(left.v-right.v).length();
          if(width<95 || width>220 || (left.v-doorway-Vec3(0,110,0)).length()<42 ||
             (right.v-doorway-Vec3(0,110,0)).length()<42)continue;
          Vec3 start,end;
          if(!characterFloor(world,doorway-direction*180.f,start) || !characterFloor(world,doorway+direction*180.f,end) ||
             !characterLane(world,start,end,25) || std::abs(end.y-start.y)>25)continue;
          // A doorway opens out on at least one side; a narrow street is not this case.
          if(physics->ray(start+Vec3(0,110,0),start+side*150.f+Vec3(0,110,0)).hasCol &&
             physics->ray(start+Vec3(0,110,0),start-side*150.f+Vec3(0,110,0)).hasCol)continue;
          route.waypoint=point->name;route.start=start;route.goal=end;route.tolerance=75;found=true;break;
          }
        }
      if(found)break;
      }
    }
  else if(name=="tavern") {
    if(!bench.movableReady)prepareMovableFixture(world,player);
    if(bench.movableReady) {
      route.start=bench.movableWalkStart;route.goal=bench.movableWalkEnd;
      route.waypoint=std::string(symName(world,bench.noiseHost));route.tolerance=55;found=true;
      }
    }
  else if(name=="jump_ledge") {
    for(const auto* point:points) {
      if(!city(*point)) continue;
      Vec3 start;if(!characterFloor(world,point->position(),start))continue;
      for(unsigned i=0;i<24 && !found;++i) {
        const float angle=float(i)*float(M_PI)/12.f;
        const Vec3 direction(std::cos(angle),0,std::sin(angle));
        for(float distance:{150.f,200.f,250.f}) {
          const auto landing=physics->landRay(start+direction*distance+Vec3(0,210,0),250);
          if(!landing.hasCol || landing.n.y<.8f || landing.v.y-start.y<55 || landing.v.y-start.y>110)continue;
          if(physics->ray(start+Vec3(0,180,0),landing.v+Vec3(0,180,0)).hasCol)continue;
          Vec3 beyond;if(!characterFloor(world,landing.v+direction*80.f,beyond) || std::abs(beyond.y-landing.v.y)>15)continue;
          route.waypoint=point->name;route.start=start;route.goal=beyond;route.tolerance=75;found=true;break;
          }
        }
      if(found)break;
      }
    }
  else if(name=="water_entry") {
    // The city waterfront is a quay above sea level. Walking off it exercises
    // walking -> legacy falling -> swimming, including the actual handoff.
    for(const auto* point:points) {
      if(!harbour(*point) && point->name.find("WATER")==std::string::npos &&
         point->name.find("BEACH")==std::string::npos && point->name.find("COAST")==std::string::npos)continue;
      Vec3 dry;if(!characterFloor(world,point->position(),dry) || physics->waterRay(dry,15).hasCol)continue;
      for(unsigned i=0;i<24 && !found;++i) {
        const float angle=float(i)*float(M_PI)/12.f;
        const Vec3 direction(std::cos(angle),0,std::sin(angle));
        for(float distance:{200.f,350.f,500.f,700.f,1000.f,1400.f}) {
          const auto bottom=physics->landRay(dry+direction*distance+Vec3(0,100,0),3000);
          if(!bottom.hasCol)continue;
          const auto liquid=physics->waterRay(bottom.v+Vec3(0,5,0),0);
          if(!liquid.hasCol || liquid.wdepth-bottom.v.y<160 || liquid.wdepth>dry.y+30 || dry.y-liquid.wdepth>1000)continue;
          if(physics->ray(dry+Vec3(0,120,0),Vec3(bottom.v.x,dry.y+120,bottom.v.z)).hasCol)continue;
          route.waypoint=point->name;route.start=dry;route.goal={bottom.v.x,liquid.wdepth,bottom.v.z};
          route.tolerance=100;found=true;break;
          }
        }
      if(found)break;
      }
    }
  if(!found) {
    event("character_error","\"route\":"+jsonStr(name)+",\"reason\":\"no stock geometry satisfies fixture preconditions\"");
    if(abortMissing)arenaAbort("character route preparation failed: "+name);
    return false;
    }
  route.direction=route.goal-route.start;route.direction.y=0;route.direction=Vec3::normalize(route.direction);
  route.prepared=true;route.lastSupported=route.start;
  characterFreezeNear(world,player,route.start,route.goal);
  player.stopWalking();player.clearAiQueue();player.setPosition(route.start);player.setDirection(route.direction);player.updateTransform();
  bench.hasPrev=false;bench.characterRoute=route;
  bench.cam=C_Follow;bench.camDist=240;bench.camHeight=70;bench.camAngle=110;bench.camAuto=true;
  event("character_route_setup","\"route\":"+jsonStr(name)+",\"waypoint\":"+jsonStr(route.waypoint)+
    ",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"tolerance_cm\":"+ragdollNumber(route.tolerance)+",\"yaw_deg\":"+ragdollNumber(player.rotation()));
  return true;
  }

void characterSnapshot(World& world,Npc& player,std::string_view operation) {
  event("character_snapshot","\"operation\":"+jsonStr(operation)+",\"world\":"+jsonStr(world.name())+
    ",\"root_cm\":"+ragdollVector(player.position())+",\"visual_root_cm\":"+ragdollVector(player.characterVisualPosition())+",\"cv_enabled\":"+
    (Gothic::physicsFlags().characterVirtual?"true":"false")+
    ",\"cv_active\":"+(player.characterVirtualActive()?"true":"false"));
  }

void characterRouteEnd(World& world,Npc& player) {
  auto& route=bench.characterRoute;
  if(!route.active) {arenaAbort("character route end without a begun route");return;}
  route.active=false;
  event("character_route_end","\"route\":"+jsonStr(route.name)+",\"success\":"+(route.reached?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string((route.reached?route.arrived:world.tickCount())-route.began)+
    ",\"window_ms\":"+std::to_string(world.tickCount()-route.began)+",\"start_cm\":"+ragdollVector(route.start)+
    ",\"goal_cm\":"+ragdollVector(route.goal)+",\"end_cm\":"+ragdollVector(player.position())+
    ",\"fall_count\":"+std::to_string(route.fallCount)+",\"saw_slide\":"+(route.sawSlide?"true":"false")+",\"saw_swim\":"+(route.sawSwim?"true":"false"));
  }

Matrix4x4 characterSupportMatrix(World& world) {
  if(bench.characterMover)return bench.characterMover->transform();
  if(bench.characterPlatform)return bench.characterPlatformMatrix;
  for(const auto& row:world.physic()->movableVobSnapshot())
    if(row.id==bench.characterPropId)return row.transform;
  return Matrix4x4::mkIdentity();
  }

void characterRideSample(World& world,const Npc& rider) {
  const auto matrix=characterSupportMatrix(world);
  auto expected=bench.characterRideLocalStart;matrix.project(expected);
  const auto error=rider.position()-expected;
  auto local=rider.position();auto inverse=matrix;inverse.inverse();inverse.project(local);
  bench.characterRideMaximumDrift=std::max(bench.characterRideMaximumDrift,error.length());
  bench.characterRideMaximumHorizontalDrift=std::max(bench.characterRideMaximumHorizontalDrift,std::hypot(error.x,error.z));
  bench.characterRideMaximumLocalDrift=std::max(bench.characterRideMaximumLocalDrift,(local-bench.characterRideLocalStart).length());
  event("character_ride_sample","\"support_kind\":"+jsonStr(bench.characterMover?"mover":bench.characterPlatform?"kinematic":"dynamic")+
    ",\"rider\":"+jsonStr(bench.characterRideNpc?"npc":"hero")+",\"rotation\":"+(bench.characterRideRotating?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.characterRideBegan)+
    ",\"support_cm\":"+ragdollVector(movablePosition(matrix))+",\"rider_cm\":"+ragdollVector(rider.position())+
    ",\"expected_cm\":"+ragdollVector(expected)+",\"error_cm\":"+ragdollVector(error)+
    ",\"local_cm\":"+ragdollVector(local)+",\"angle_deg\":"+ragdollNumber(bench.characterRideAngle)+
    ",\"ground_velocity_cm_s\":"+ragdollVector(rider.characterVirtualGroundVelocity())+
    ",\"cv_active\":"+(rider.characterVirtualActive()?"true":"false")+
    ",\"falling\":"+(rider.isFalling()?"true":"false")+",\"in_air\":"+(rider.isInAir()?"true":"false"));
  }

void characterFields(World& world,Npc& player,uint64_t dt,PlayerControl& ctrl) {
  auto& route=bench.characterRoute;
  if(!route.active && !bench.characterMeasuring && bench.characterPropRole.empty() && !bench.characterRiding)return;
  const auto now=std::chrono::steady_clock::now();
  const float wall=bench.characterHasWall?std::chrono::duration<float,std::milli>(now-bench.characterLastWall).count():0;
  bench.characterHasWall=true;bench.characterLastWall=now;
  const auto& character=bench.characterRiding && bench.characterRideNpc?*bench.characterRideNpc:player;
  const auto position=character.position();
  bool move=false;
  for(const auto& hold:bench.holds)
    move|=hold.a==KeyCodec::Forward || hold.a==KeyCodec::Back || hold.a==KeyCodec::Left || hold.a==KeyCodec::Right;
  bool normal=false;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  normal=AppCadence::normalClock();
#endif
  const std::string rideLabel=std::string(bench.characterMover?"ride_mover":bench.characterPlatform?
    bench.characterRideRotating?"ride_rotation":"ride_kinematic":"ride_dynamic")+(bench.characterRideNpc?"_npc":"");
  const std::string label=bench.characterMeasuring?"performance":route.active?route.name:bench.characterRiding?rideLabel:bench.characterPropRole;
  if(!normal || wall>0)bench.characterFrames.push_back({label,bench.characterFrame++,world.tickCount(),dt,position,
    character.characterVirtualGroundVelocity(),character.characterVisualPosition(),move,character.isInAir(),character.isFalling(),character.isSwim(),
    bench.characterRideNpc?bool(Gothic::settingsGetI("GAME","npc_character_virtual")):Gothic::physicsFlags().characterVirtual,
    character.characterVirtualActive(),bench.rec && bench.recVideo,normal,
    world.physic()->physicsStats().stepMilliseconds,wall});
  if(bench.characterMeasuring) {
    const float sign=((world.tickCount()-bench.characterMeasureStart)/700)%2==0?1.f:-1.f;
    player.setDirection(bench.characterRoute.direction*sign);
    }
  if(route.active) {
    route.sawSwim|=player.isSwim();route.sawSlide|=player.isSlide();
    if(!player.isInAir() && !player.isFalling() && !player.isSwim())route.lastSupported=position;
    const bool edgeFall=route.name!="jump_ledge" && route.name!="water_entry" && player.isFalling() &&
      route.lastSupported.y-position.y>100.f;
    if(edgeFall && !route.falling) {
      ++route.fallCount;
      event("character_fall","\"route\":"+jsonStr(route.name)+",\"root_cm\":"+ragdollVector(position)+
        ",\"support_cm\":"+ragdollVector(route.lastSupported));
      }
    route.falling=edgeFall;
    const auto offset=position-route.goal;
    const float remaining=-Vec3::dotProduct(offset,route.direction);
    const float side=std::abs(offset.x*route.direction.z-offset.z*route.direction.x);
    const bool height=route.name=="water_entry"?route.sawSwim:std::abs(position.y-route.goal.y)<60.f;
    if(!route.reached && remaining<=route.tolerance && side<route.tolerance && height) {
      route.reached=true;route.arrived=world.tickCount();
      std::erase_if(bench.holds,[&](const Hold& hold) {
        const bool moving=hold.a==KeyCodec::Forward || hold.a==KeyCodec::Back || hold.a==KeyCodec::Left || hold.a==KeyCodec::Right;
        if(moving)release(ctrl,hold.a);
        return moving;
        });
      event("character_route_arrive","\"route\":"+jsonStr(route.name)+",\"elapsed_ms\":"+
        std::to_string(route.arrived-route.began)+",\"root_cm\":"+ragdollVector(position));
      }
    }
  if(bench.characterRideMoving)characterRideSample(world,character);
  if(bench.characterItem)
    bench.characterPropTravel=std::max(bench.characterPropTravel,(bench.characterItem->position()-bench.characterPropInitial).length());
  if(bench.characterPropId)
    for(const auto& body:world.physic()->movableVobSnapshot())if(body.id==bench.characterPropId)
      bench.characterPropTravel=std::max(bench.characterPropTravel,(movablePosition(body.transform)-bench.characterPropInitial).length());
  }

void characterPropCheckpoint(World& world,Npc& player) {
  if(bench.characterPropRole.empty()) {arenaAbort("character prop checkpoint needs a prop");return;}
  Vec3 position,velocity;float mass=0;bool contact=false,blocks=false;
  if(bench.characterItem) {position=bench.characterItem->position();velocity=bench.characterItem->physicsVelocity();mass=bench.characterItem->physicsMass();contact=bench.characterItem->physicsTouchedByNpc();}
  else for(const auto& body:world.physic()->movableVobSnapshot()) if(body.id==bench.characterPropId) {
    position=movablePosition(body.transform);velocity=body.velocity;mass=body.mass;contact=body.touchedByNpc;blocks=body.blocksWalking;break;
    }
  float nearPlane=0,farPlane=0;bool crossed=false;
  for(const auto& body:world.physic()->movableVobSnapshot())if(body.id==bench.characterPropId) {
    nearPlane=std::numeric_limits<float>::infinity();farPlane=-nearPlane;
    for(unsigned corner=0;corner<8;++corner) {
      Vec3 point((corner&1)?body.localBounds[1].x:body.localBounds[0].x,
                 (corner&2)?body.localBounds[1].y:body.localBounds[0].y,
                 (corner&4)?body.localBounds[1].z:body.localBounds[0].z);
      body.transform.project(point);
      const float projected=Vec3::dotProduct(point,bench.characterPropDirection);
      nearPlane=std::min(nearPlane,projected);farPlane=std::max(farPlane,projected);
      }
    crossed=Vec3::dotProduct(player.position(),bench.characterPropDirection)>farPlane+25.f;
    break;
    }
  event("character_prop_checkpoint","\"role\":"+jsonStr(bench.characterPropRole)+",\"id\":"+
    std::to_string(bench.characterPropId)+",\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.characterPropBegan)+",\"mass_kg\":"+ragdollNumber(mass)+",\"position_cm\":"+ragdollVector(position)+
    ",\"velocity_cm_s\":"+ragdollVector(velocity)+",\"max_displacement_cm\":"+ragdollNumber(bench.characterPropTravel)+
    ",\"initial_cm\":"+ragdollVector(bench.characterPropInitial)+",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"hero_initial_cm\":"+ragdollVector(bench.characterPropHeroInitial)+",\"native_contact\":"+(contact?"true":"false")+
    ",\"passed_through\":"+(crossed?"true":"false")+",\"near_plane_cm\":"+ragdollNumber(nearPlane)+
    ",\"far_plane_cm\":"+ragdollNumber(farPlane)+",\"blocks_walking\":"+(blocks?"true":"false"));
  }

void prepareCharacterProp(World& world,Npc& player,const std::string& role) {
  if(bench.characterItem) {world.removeItem(*bench.characterItem);bench.characterItem=nullptr;}
  bench.characterProp.reset();bench.characterPropId=0;bench.characterPropTravel=0;bench.characterPropRole=role;
  if(!bench.movableReady) prepareMovableFixture(world,player);
  if(!bench.movableReady)return;
  // The general tavern stand adds a floor dish at this lane's midpoint. Remove
  // those temporary stand props so no overlap can propel a test item by itself.
  bench.movableFixtures.clear();
  const auto direction=Vec3::normalize(bench.movableWalkEnd-bench.movableWalkStart);
  const auto center=(bench.movableWalkStart+bench.movableWalkEnd)*.5f;
  if(role=="item" || role=="mug") {
    const auto symbol=world.script().findSymbolIndex(role=="mug"?"ITFO_BEER":"ItPo_Health_01");
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(center+Vec3(0,10,0));
    bench.characterItem=world.addItemDyn(symbol,matrix,size_t(-1));
    if(!bench.characterItem) {arenaAbort("character real Item insertion failed");return;}
    bench.characterItem->setPhysicsEnable(world);
    bench.characterPropInitial=bench.characterItem->position();
    }
  else {
    std::string visual;
    for(const auto& body:world.physic()->movableVobSnapshot())if(body.className==role) {visual=body.visual;break;}
    if(visual.empty() && role=="barrel") visual="NW_HARBOUR_BARREL_01.3DS";
    const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
    if(!mesh || !rule) {arenaAbort("character stock prop mesh/class unavailable: "+role);return;}
    const auto* bounds=mesh->bbox();
    zenkit::VirtualObject data;data.type=zenkit::VirtualObjectType::zCVob;data.id=uint32_t(-1);
    data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
    data.position={center.x-(bounds[0].x+bounds[1].x)*.5f,center.y-bounds[0].y+.2f,center.z-(bounds[0].z+bounds[1].z)*.5f};
    data.visual=std::make_shared<zenkit::VisualMesh>();data.visual->type=zenkit::VisualType::MESH;data.visual->name=visual;
    auto fixture=std::make_unique<Bench::MovableFixture>();fixture->visual.setVisual(data,world,false,false);
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(data.position.x,data.position.y,data.position.z);fixture->visual.setObjMatrix(matrix);
    const auto before=world.physic()->movableVobSnapshot();
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
    if(fixture->body.isEmpty()) {arenaAbort("character native prop insertion failed");return;}
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& matrix){view->setObjMatrix(matrix);});
    bench.characterProp=std::move(fixture);bench.characterPropInitial=movablePosition(matrix);
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;}))bench.characterPropId=body.id;
    }
  characterFreezeNear(world,player,bench.movableWalkStart,bench.movableWalkEnd);
  player.stopWalking();player.clearAiQueue();player.setPosition(bench.movableWalkStart);player.setDirection(direction);player.updateTransform();
  bench.characterPropHeroInitial=player.position();bench.characterPropDirection=direction;bench.characterPropBegan=world.tickCount();bench.characterHasWall=false;
  event("character_prop_setup","\"role\":"+jsonStr(role)+",\"initial_cm\":"+ragdollVector(bench.characterPropInitial)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"direction\":"+ragdollVector(direction));
  }

void characterRideCheckpoint(World& world,Npc& player,bool setup=false) {
  if(!bench.characterRiding) {arenaAbort("character ride checkpoint needs a support body");return;}
  const auto& rider=bench.characterRideNpc?*bench.characterRideNpc:player;
  const auto matrix=characterSupportMatrix(world);
  const auto body=movablePosition(matrix);
  auto expected=bench.characterRideLocalStart;matrix.project(expected);
  auto local=rider.position();auto inverse=matrix;inverse.inverse();inverse.project(local);
  const auto error=rider.position()-expected;
  event(setup?"character_ride_setup":"character_ride_checkpoint","\"support_kind\":"+jsonStr(bench.characterMover?"mover":bench.characterPlatform?"kinematic":"dynamic")+
    ",\"support_cm\":"+ragdollVector(body)+",\"support_initial_cm\":"+ragdollVector(bench.characterRideStart)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"hero_initial_cm\":"+ragdollVector(bench.characterRideHeroStart)+
    ",\"rider\":"+jsonStr(bench.characterRideNpc?"npc":"hero")+",\"rider_cm\":"+ragdollVector(rider.position())+
    ",\"local_initial_cm\":"+ragdollVector(bench.characterRideLocalStart)+",\"local_cm\":"+ragdollVector(local)+
    ",\"expected_cm\":"+ragdollVector(expected)+",\"error_cm\":"+ragdollVector(error)+
    ",\"support_displacement_cm\":"+ragdollNumber((body-bench.characterRideStart).length())+
    ",\"angle_deg\":"+ragdollNumber(bench.characterRideAngle)+",\"elapsed_ms\":"+std::to_string(world.tickCount()-bench.characterRideBegan)+
    ",\"max_drift_cm\":"+ragdollNumber(bench.characterRideMaximumDrift)+
    ",\"max_horizontal_drift_cm\":"+ragdollNumber(bench.characterRideMaximumHorizontalDrift)+
    ",\"max_local_drift_cm\":"+ragdollNumber(bench.characterRideMaximumLocalDrift)+
    ",\"mass_kg\":"+ragdollNumber(bench.characterProp?bench.characterProp->body.mass():0.f)+
    ",\"support_velocity_cm_s\":"+ragdollVector(bench.characterProp?bench.characterProp->body.velocity():Vec3())+
    ",\"ground_velocity_cm_s\":"+ragdollVector(rider.characterVirtualGroundVelocity())+
    ",\"falling\":"+(rider.isFalling()?"true":"false")+",\"in_air\":"+(rider.isInAir()?"true":"false"));
  }

// NPC stands use real AI_GoToPoint actions. Only fixture preparation moves an
// actor directly; route frames are ordinary animation-driven MoveAlgo ticks.
std::string npcCharacterBodyJson(const CharacterVirtualDiagnostics::Body& body,
                                const std::vector<DynamicWorld::MovableVobSnapshot>& movable) {
  std::string visual=body.label,kind;
  if(body.pointerType==DynamicWorld::P_Item)
    kind="Item";
  else if(body.pointerType==DynamicWorld::P_Interactive) {
    if(const auto* rule=MovableVobs::classifyVisual(visual))kind=rule->className;
    else kind="Interactive";
    }
  else if(body.pointerType==DynamicWorld::P_MovableVob) {
    const auto id=reinterpret_cast<uintptr_t>(body.payload);
    for(const auto& entry:movable)if(entry.id==id) {visual=entry.visual;kind=entry.className;break;}
    }
  if(kind.empty()) {
    if(const auto* rule=MovableVobs::classifyVisual(visual))kind=rule->className;
    else if(body.layer==DynamicWorld::C_Landscape)kind="landscape";
    else kind="vob";
    }
  return "{\"present\":"+std::string(body.present?"true":"false")+",\"id\":"+std::to_string(body.id)+
    ",\"layer\":"+std::to_string(body.layer)+",\"pointer_type\":"+std::to_string(body.pointerType)+
    ",\"motion_type\":"+std::to_string(body.motionType)+",\"mass_kg\":"+ragdollNumber(body.mass)+
    ",\"label\":"+jsonStr(body.label)+",\"visual\":"+jsonStr(visual)+",\"class\":"+jsonStr(kind)+
    ",\"position_cm\":"+ragdollVector(body.position)+",\"linear_velocity_cm_s\":"+ragdollVector(body.linearVelocity)+
    ",\"angular_velocity_rad_s\":"+ragdollVector(body.angularVelocity)+",\"point_velocity_cm_s\":"+ragdollVector(body.pointVelocity)+"}";
  }

std::string npcCharacterPoseJson(const CharacterVirtualDiagnostics::Pose& pose,
                                const std::vector<DynamicWorld::MovableVobSnapshot>& movable) {
  std::string contacts="[";
  const auto flag=[](bool value){return value?"true":"false";};
  for(const auto& c:pose.contacts) {
    if(contacts.size()>1)contacts+=',';
    contacts+="{\"body\":"+npcCharacterBodyJson(c.body,movable)+",\"subshape\":"+std::to_string(c.subShape)+
      ",\"position_cm\":"+ragdollVector(c.position)+",\"normal\":"+ragdollVector(c.contactNormal)+
      ",\"surface_normal\":"+ragdollVector(c.surfaceNormal)+",\"point_velocity_cm_s\":"+ragdollVector(c.pointVelocity)+
      ",\"distance_cm\":"+ragdollNumber(c.distance)+",\"supporting_volume_distance_cm\":"+ragdollNumber(c.supportingVolumeDistance)+
      ",\"had_collision\":"+flag(c.hadCollision)+",\"discarded\":"+flag(c.discarded)+",\"sensor\":"+flag(c.sensor)+
      ",\"can_push_character\":"+flag(c.canPushCharacter)+",\"within_supporting_volume\":"+flag(c.withinSupportingVolume)+
      ",\"walkable_slope\":"+flag(c.walkableSlope)+",\"support_allowed_by_policy\":"+flag(c.supportAllowedByPolicy)+
      ",\"support_eligible\":"+flag(c.supportEligible)+"}";
    }
  return "{\"position_cm\":"+ragdollVector(pose.position)+",\"velocity_cm_s\":"+ragdollVector(pose.velocity)+
    ",\"ground_state\":"+std::to_string(pose.groundState)+",\"ground\":"+npcCharacterBodyJson(pose.ground,movable)+
    ",\"ground_subshape\":"+std::to_string(pose.groundSubShape)+",\"ground_position_cm\":"+ragdollVector(pose.groundPosition)+
    ",\"ground_normal\":"+ragdollVector(pose.groundNormal)+",\"ground_velocity_cm_s\":"+ragdollVector(pose.groundVelocity)+
    ",\"contacts\":"+contacts+"]}";
  }

std::string npcCharacterCollisionJson(World& world,const CharacterVirtualDiagnostics::NpcCollision& collision) {
  // Resolve a borrowed neighbour immediately; historical samples retain JSON
  // values, never pointers to actors that may despawn later in the frame.
  const auto id=world.npcId(collision.npc);
  const auto* blocker=id!=uint32_t(-1)?world.npcById(id):nullptr;
  return "{\"npc_id\":"+(blocker?std::to_string(id):"null")+",\"npc\":"+jsonStr(blocker?symName(world,blocker):"none")+
    ",\"position_cm\":"+ragdollVector(collision.position)+",\"normal\":"+ragdollVector(collision.normal)+
    ",\"allowed_depth_cm\":"+ragdollNumber(collision.allowedDepth)+",\"rejected_depth_cm\":"+ragdollNumber(collision.rejectedDepth)+
    ",\"tested_position_cm\":"+ragdollVector(collision.testedPosition)+"}";
  }

std::string npcCharacterMoveJson(World& world,const CharacterVirtualDiagnostics::NpcMove& move) {
  const auto flag=[](bool value){return value?"true":"false";};
  return "{\"initial_cm\":"+ragdollVector(move.initial)+",\"requested_cm\":"+ragdollVector(move.requested)+
    ",\"clipped_request_cm\":"+ragdollVector(move.clippedRequest)+",\"native_position_cm\":"+ragdollVector(move.nativePosition)+
    ",\"accepted_cm\":"+ragdollVector(move.accepted)+",\"request_clipped\":"+flag(move.requestClipped)+
    ",\"resolved_clipped\":"+flag(move.resolvedClipped)+",\"blocked\":"+flag(move.blocked)+
    ",\"sequence\":"+std::to_string(move.sequence)+",\"dt_ms\":"+std::to_string(move.dt)+
    ",\"advancement_ratio\":"+ragdollNumber(move.advancementRatio)+",\"steep_contact\":"+flag(move.steepContact)+
    ",\"steep_contact_normal\":"+ragdollVector(move.steepContactNormal)+
    ",\"request_collision\":"+npcCharacterCollisionJson(world,move.requestCollision)+
    ",\"resolved_collision\":"+npcCharacterCollisionJson(world,move.resolvedCollision)+"}";
  }

Bench::CharacterMoveFrame characterMoveFrame(const Npc& npc) {
  CharacterVirtualDiagnostics diagnostic;
  Bench::CharacterMoveFrame frame;
  frame.valid=npc.characterVirtualDiagnostics(diagnostic);
  if(!frame.valid)return frame;
  const auto& move=diagnostic.npcMove;
  frame.sequence=move.sequence;frame.dt=move.dt;frame.requested=move.requested;
  frame.acceptedDelta=move.accepted-move.initial;frame.advancementRatio=move.advancementRatio;
  frame.steepContact=move.steepContact;frame.steepNormal=move.steepContactNormal;frame.blocked=move.blocked;
  return frame;
  }

bool npcCharacterNeedsMovableLookup(const CharacterVirtualDiagnostics::Pose& pose) {
  if(pose.ground.pointerType==DynamicWorld::P_MovableVob)return true;
  return std::any_of(pose.contacts.begin(),pose.contacts.end(),[](const auto& contact) {
    return contact.body.pointerType==DynamicWorld::P_MovableVob;
    });
  }

std::string npcCharacterSample(World& world,const Npc& npc,float physicalJump,float visualJump,uint64_t frame,uint64_t dt) {
  const auto goal=npc.navigationGoal();
  const auto observed=npc.bodyObservationState();
  const auto goalId=world.npcId(goal.npc);
  const auto* target=goalId!=uint32_t(-1)?world.npcById(goalId):nullptr;
  CharacterVirtualDiagnostics d;
  const bool valid=npc.characterVirtualDiagnostics(d);
  std::string row="\"tick\":"+std::to_string(world.tickCount())+",\"frame\":"+std::to_string(frame)+
    ",\"frame_dt_ms\":"+std::to_string(dt)+",\"npc_id\":"+std::to_string(world.npcId(&npc))+",\"npc\":"+jsonStr(symName(world,&npc))+
    ",\"root_cm\":"+ragdollVector(npc.position())+",\"visual_root_cm\":"+ragdollVector(npc.characterVisualPosition())+
    ",\"root_motion_model_cm\":"+ragdollVector(npc.animMoveSpeed(dt))+
    ",\"physical_dy_cm\":"+ragdollNumber(physicalJump)+",\"visual_dy_cm\":"+ragdollNumber(visualJump)+
    ",\"yaw_deg\":"+ragdollNumber(npc.rotation())+",\"body_state\":"+std::to_string(unsigned(npc.bodyState()))+
    ",\"body_state_masked\":"+std::to_string(unsigned(npc.bodyStateMasked()))+",\"state\":"+jsonStr(stateName(world,npc))+
    ",\"in_air\":"+(npc.isInAir()?"true":"false")+",\"falling\":"+(npc.isFalling()?"true":"false")+
    ",\"interactive\":"+(npc.interactive()?"true":"false")+",\"external_control\":"+(npc.isExternalControl()?"true":"false")+
    ",\"process_policy\":"+std::to_string(int(npc.processPolicy()))+
    ",\"ai_wait_ms\":"+std::to_string(observed.aiWaitMs)+",\"ani_wait_ms\":"+std::to_string(observed.aniWaitMs)+
    ",\"output_wait_ms\":"+std::to_string(observed.outputWaitMs)+",\"fai_wait_ms\":"+std::to_string(observed.faiWaitMs)+
    ",\"animations\":"+jsonStr(npc.animNames())+",\"ai_queue_empty\":"+(npc.isAiQueueEmpty()?"true":"false")+
    ",\"ai_busy\":"+(npc.isAiBusy()?"true":"false")+",\"move_hint\":"+std::to_string(unsigned(npc.moveHint()))+
    ",\"cv_enabled\":"+((npc.isPlayer() ? Gothic::physicsFlags().characterVirtual :
                                         Gothic::physicsFlags().npcCharacterVirtual)?"true":"false")+
    ",\"cv_active\":"+(npc.characterVirtualActive()?"true":"false")+",\"diagnostics_valid\":"+(valid?"true":"false")+
    ",\"nav_goal\":{\"active\":"+(goal.active?"true":"false")+",\"kind\":"+jsonStr(goal.kind)+",\"name\":"+jsonStr(goal.name)+
    ",\"position_cm\":"+ragdollVector(goal.position)+",\"npc_id\":"+(target?std::to_string(goalId):"null")+
    ",\"npc\":"+jsonStr(target?symName(world,target):"none")+"}";
  if(valid) {
    // Look up movable visual/class names only when a sampled native body needs
    // them. The trace writes referenced contacts, not the world's full catalog.
    std::vector<DynamicWorld::MovableVobSnapshot> movable;
    if(npcCharacterNeedsMovableLookup(d.before) || npcCharacterNeedsMovableLookup(d.afterExtendedUpdate) ||
       npcCharacterNeedsMovableLookup(d.afterStairs))movable=world.physic()->movableVobSnapshot();
    row+=",\"dt_ms\":"+std::to_string(d.dt)+",\"requested_cm\":"+ragdollVector(d.requested)+
      ",\"ground_normal_fallbacks\":"+std::to_string(d.groundNormalFallbacks)+
      ",\"requested_velocity_cm_s\":"+ragdollVector(d.requestedVelocity)+
      ",\"stair_desired_velocity_cm_s\":"+ragdollVector(d.stairDesiredVelocity)+
      ",\"max_slope_rad\":"+ragdollNumber(d.maxSlopeRadians)+",\"stair_height_cm\":"+ragdollNumber(d.stairHeight)+
      ",\"stair_desired_distance_cm\":"+ragdollNumber(d.stairDesiredDistance)+
      ",\"stair_achieved_distance_cm\":"+ragdollNumber(d.stairAchievedDistance)+
      ",\"stair_supported\":"+(d.stairSupported?"true":"false")+",\"stair_can_walk\":"+(d.stairCanWalk?"true":"false")+
      ",\"stair_blocked_by_policy\":"+(d.stairBlockedByPolicy?"true":"false")+
      ",\"stair_progress_blocked\":"+(d.stairProgressBlocked?"true":"false")+
      ",\"stair_attempted\":"+(d.stairAttempted?"true":"false")+",\"stair_succeeded\":"+(d.stairSucceeded?"true":"false")+
      ",\"stair_probe\":{\"reason\":"+jsonStr(d.stairProbe.reason)+
      ",\"up_cm\":"+ragdollVector(d.stairProbe.upPosition)+",\"forward_cm\":"+ragdollVector(d.stairProbe.forwardPosition)+
      ",\"final_cm\":"+ragdollVector(d.stairProbe.finalPosition)+",\"test_cm\":"+ragdollVector(d.stairProbe.testPosition)+
      ",\"test_final_cm\":"+ragdollVector(d.stairProbe.testFinalPosition)+
      ",\"landing_normal\":"+ragdollVector(d.stairProbe.landingNormal)+
      ",\"test_landing_normal\":"+ragdollVector(d.stairProbe.testLandingNormal)+
      ",\"up_fraction\":"+ragdollNumber(d.stairProbe.upFraction)+
      ",\"landing_fraction\":"+ragdollNumber(d.stairProbe.landingFraction)+
      ",\"test_landing_fraction\":"+ragdollNumber(d.stairProbe.testLandingFraction)+"}"+
      ",\"stair_obstacle_id\":"+std::to_string(d.stairObstacleId)+",\"npc_move\":"+npcCharacterMoveJson(world,d.npcMove)+
      ",\"before\":"+npcCharacterPoseJson(d.before,movable)+
      ",\"after_extended_update\":"+npcCharacterPoseJson(d.afterExtendedUpdate,movable)+
      ",\"after_stairs\":"+npcCharacterPoseJson(d.afterStairs,movable);
    }
  return row;
  }

void npcCharacterSupportTrace(World& world,const Npc& npc,float physicalJump,float visualJump) {
  if(!npcCharacterTrace)return;
  auto& route=bench.npcCharacterRoute;
  auto row="\"route\":"+jsonStr(route.name)+","+
    npcCharacterSample(world,npc,physicalJump,visualJump,bench.npcCharacterFrames.size()-1,bench.frameMs);
  route.supportHistory.push_back(std::move(row));
  if(route.supportHistory.size()>6)route.supportHistory.pop_front();
  // This threshold selects diagnostic output only; it never changes motion.
  if(std::abs(physicalJump)<=10.f && std::abs(visualJump)<=10.f)return;
  const int count=int(route.supportHistory.size());
  for(int i=0;i<count;++i)
    event("npc_character_support_trace","\"spike_frame\":"+std::to_string(bench.npcCharacterFrames.size()-1)+
      ",\"relative_frame\":"+std::to_string(i-count+1)+","+route.supportHistory[size_t(i)]);
  }

void npcCharacterMotionTrace(World& world,uint64_t dt) {
  for(auto& actor:bench.npcCharacterTraceActors) {
    const auto& npc=*actor.npc;
    const auto root=npc.position(),visual=npc.characterVisualPosition();
    const float physicalJump=actor.hasPrevious?root.y-actor.lastRoot.y:0.f;
    const float visualJump=actor.hasPrevious?visual.y-actor.lastVisual.y:0.f;
    event("npc_character_motion_trace","\"window\":"+jsonStr(bench.npcCharacterObserving?bench.npcCharacterWindow:std::string())+","+
      npcCharacterSample(world,npc,physicalJump,visualJump,bench.step,dt));
    actor.lastRoot=root;actor.lastVisual=visual;actor.hasPrevious=true;
    }
  }

void npcCharacterSnapshot(World& world,Npc& player,std::string_view label) {
  std::string rows="[";
  for(uint32_t id=0;id<world.npcCount();++id) {
    const auto* npc=world.npcById(id);
    if(!npc || npc==&player || !npc->isHuman() || npc->isDown() ||
       (npc->position()-player.position()).length()>2300.f)continue;
    if(rows.size()>1)rows+=',';
    rows+="{\"npc_id\":"+std::to_string(id)+",\"npc\":"+jsonStr(symName(world,npc))+
      ",\"root_cm\":"+ragdollVector(npc->position())+",\"visual_root_cm\":"+ragdollVector(npc->characterVisualPosition())+
      ",\"cv_active\":"+(npc->characterVirtualActive()?"true":"false")+
      ",\"interactive\":"+(npc->interactive()?"true":"false")+"}";
    }
  event("npc_character_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false")+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"actors\":"+rows+"]");
  }

Npc* npcCharacterActor(World& world,const Vec3& position) {
  auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),position);
  if(!npc) {arenaAbort("NPC CharacterVirtual fixture actor insertion failed");return nullptr;}
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->closeWeapon(true);
  // Keep native queued actions and movement; suppress source-save routines
  // and perception callbacks between scripted fixture legs.
  npc->setExternalControl(true);npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Run);
  npc->aiPush(AiQueue::aiWait(60000));npc->setPosition(position);npc->updateTransform();
  return npc;
  }

void npcCharacterPlaceHero(World& world,Npc& player,const Vec3& start,const Vec3& goal) {
  auto along=goal-start;along.y=0;
  along=along.length()>0?Vec3::normalize(along):Vec3(1,0,0);
  const Vec3 side(-along.z,0,along.x);
  Vec3 hero;bool found=false;
  for(float distance:{400.f,-400.f,600.f,-600.f,800.f,-800.f}) {
    const auto at=start+side*distance;
    if(!characterFloor(world,at,hero) || std::abs(hero.y-start.y)>150)continue;
    if(world.physic()->ray(hero+Vec3(0,100,0),hero+along*40.f+Vec3(0,100,0)).hasCol)continue;
    found=true;break;
    }
  if(!found)hero=start-along*250.f;
  player.stopWalking();player.clearAiQueue();player.setPosition(hero);player.setDirection(start-hero);player.updateTransform();
  }

std::string ownMeshChestInventory(World& world,const Npc& player) {
  const auto& fixture=bench.ownMeshChest;
  std::string rows="[";
  for(const auto& [id,count]:fixture.contents) {
    if(rows.size()>1)rows+=',';
    rows+="{\"class\":"+jsonStr(world.script().findSymbol(id)->name())+
      ",\"chest_before\":"+std::to_string(count)+",\"chest_now\":"+
      std::to_string(fixture.chest->inventory().itemCount(id))+
      ",\"hero_before\":"+std::to_string(fixture.heroBefore.at(id))+
      ",\"hero_now\":"+std::to_string(player.inventory().itemCount(id))+"}";
    }
  return rows+"]";
  }

void ownMeshChestSnapshot(World& world,const Npc& player,std::string_view label) {
  const auto& fixture=bench.ownMeshChest;
  if(!fixture.chest) {arenaAbort("own-mesh-chest snapshot needs setup");return;}
  const auto& visual=fixture.chest->objVisual();
  const auto& lid=visual.bone(fixture.lid);
  // Midpoint of the original lid's upper front bounding-box edge, in the
  // lid node's local centimetres. Both models measure the same fixed point.
  Vec3 front(0,27.36672f,-58.41916f);
  lid.project(front);
  event("own_mesh_chest_snapshot","\"label\":"+jsonStr(label)+
    ",\"zen_id\":"+std::to_string(fixture.chest->zenId())+
    ",\"mob_id\":"+std::to_string(world.mobsiId(fixture.chest))+
    ",\"mob_cm\":"+ragdollVector(fixture.chest->position())+
    ",\"state\":"+std::to_string(fixture.chest->stateId())+
    ",\"animation\":"+jsonStr(visual.animNames())+
    ",\"lid_node\":\"BIP01 CHEST_BIG_1\",\"lid_world\":"+ragdollMatrix(lid)+
    ",\"front_probe_local_cm\":[0,27.36672,-58.41916],\"front_probe_world_cm\":"+ragdollVector(front)+
    ",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"hero_attached\":"+(player.interactive()==fixture.chest?"true":"false")+
    ",\"hero_gold\":"+std::to_string(player.inventory().itemCount(world.script().goldId()->index()))+
    ",\"hero_inventory\":"+ragdollInventory(player,world)+
    ",\"chest_empty\":"+(fixture.chest->inventory().isEmpty()?"true":"false")+
    ",\"items\":"+ownMeshChestInventory(world,player));
  }

void ownMeshChestFields(World& world,const Npc& player) {
  auto& fixture=bench.ownMeshChest;
  if(!fixture.chest || !fixture.opened)return;
  const auto animation=fixture.chest->objVisual().animNames();
  if(animation.find("T_S0_2_S1")!=std::string::npos)fixture.sawTransition=true;
  if(animation==fixture.lastAnimation)return;
  fixture.lastAnimation=animation;
  ownMeshChestSnapshot(world,player,"animation");
  }

void ownMeshChestSetup(World& world,Npc& player,const Vec3& near,uint32_t zenId,bool reload) {
  auto& fixture=bench.ownMeshChest;
  fixture=Bench::OwnMeshChest{};
  for(uint32_t id=0;auto* mob=world.mobsiById(id);++id) {
    if(mob->zenId()!=zenId || (mob->position()-near).length()>5.f)continue;
    const auto* mesh=mob->objVisual().protoMesh();
    if(!mob->isContainer() || !mesh || !mesh->fname.starts_with("CHESTBIG_NW_NORMAL_OPEN") ||
       mesh->findNode("BIP01 CHEST_BIG_1")==size_t(-1) || mesh->findNode("ZS_POS0")==size_t(-1))continue;
    fixture.chest=mob;fixture.lid=mesh->findNode("BIP01 CHEST_BIG_1");break;
    }
  if(!fixture.chest) {arenaAbort("own-mesh-chest could not find the exact authored CHESTBIG_NW_NORMAL_OPEN");return;}
  auto& chest=*fixture.chest;
  if(reload) {
    // Observe the complete loaded session before moving its hero or changing
    // container contents. The caller compares this snapshot with the save.
    ownMeshChestSnapshot(world,player,"loaded");
    const bool pass=chest.stateId()==1 && chest.inventory().isEmpty() &&
      chest.objVisual().animNames().find("S_S1")!=std::string::npos;
    event("own_mesh_chest_loaded","\"pass\":"+std::string(pass?"true":"false"));
    if(!pass)arenaAbort("own-mesh-chest loaded session lost open state or taken contents");
    return;
    }
  if(chest.isLocked() || !chest.isAvailable()) {arenaAbort("own-mesh-chest needs the free unlocked authored chest");return;}
  const bool seeded=chest.inventory().isEmpty();
  if(seeded)chest.inventory().addItem("ITMI_GOLD",1,world);
  for(auto item=chest.inventory().iterator(Inventory::T_Inventory);item.isValid();++item)
    fixture.contents[item->clsId()]+=item.count();
  if(fixture.contents.empty()) {arenaAbort("own-mesh-chest has no transferable contents");return;}
  for(const auto& [id,count]:fixture.contents)fixture.heroBefore[id]=player.inventory().itemCount(id);
  player.setInteraction(nullptr,true);player.clearAiQueue();player.clearState(false);player.stopWalking();player.closeWeapon(true);
  const auto socket=chest.nearestPoint(player);
  auto away=socket-chest.position();away.y=0;
  if(away.length()<1.f) {arenaAbort("own-mesh-chest has no horizontal use socket");return;}
  away=Vec3::normalize(away);
  characterFreezeNear(world,player,socket,socket);
  Vec3 start;bool prepared=false;
  for(float distance:{65.f,90.f,35.f,0.f}) {
    auto at=socket+away*distance;
    const auto floor=world.physic()->landRay(Vec3(at.x,socket.y+80.f,at.z),250.f);
    const bool supported=floor.hasCol && floor.n.y>=.8f && std::abs(floor.v.y-chest.position().y)<=80.f;
    if(!supported) {
      event("own_mesh_chest_approach","\"socket_cm\":"+ragdollVector(socket)+
        ",\"distance_cm\":"+ragdollNumber(distance)+",\"floor_cm\":"+ragdollVector(floor.v)+
        ",\"supported\":false");
      continue;
      }
    player.setPosition(floor.v);player.setDirection(chest.position()-floor.v);player.updateTransform();
    const bool collision=player.hasCollision();
    const float useDistance=(player.centerPosition()-socket).length();
    const bool clear=!world.physic()->ray(floor.v+Vec3(0,100,0),socket+Vec3(0,100,0)).hasCol;
    event("own_mesh_chest_approach","\"socket_cm\":"+ragdollVector(socket)+
      ",\"distance_cm\":"+ragdollNumber(distance)+",\"floor_cm\":"+ragdollVector(floor.v)+
      ",\"supported\":true,\"collision\":"+(collision?"true":"false")+
      ",\"use_distance_cm\":"+ragdollNumber(useDistance)+",\"clear\":"+(clear?"true":"false"));
    if(collision || useDistance>155.f || !clear)continue;
    start=floor.v;prepared=true;break;
    }
  if(!prepared) {arenaAbort("own-mesh-chest has no supported collision-free ordinary interaction approach");return;}
  const Vec3 side(-away.z,0,away.x),target=chest.position()+Vec3(0,65,0);
  const auto camera=target+away*150.f+side*100.f+Vec3(0,100,0);
  bench.cam=C_Hold;bench.camAuto=false;lookFrom(camera,target);
  event("own_mesh_chest_setup","\"zen_id\":"+std::to_string(zenId)+
    ",\"mob_id\":"+std::to_string(world.mobsiId(&chest))+
    ",\"visual\":"+jsonStr(chest.objVisual().protoMesh()->fname)+
    ",\"world\":"+jsonStr(world.name())+",\"mob_cm\":"+ragdollVector(chest.position())+
    ",\"socket_cm\":"+ragdollVector(socket)+",\"hero_start_cm\":"+ragdollVector(start)+
    ",\"camera_cm\":"+ragdollVector(camera)+",\"camera_target_cm\":"+ragdollVector(target)+
    ",\"seeded_gold\":"+(seeded?"true":"false")+",\"items\":"+ownMeshChestInventory(world,player));
  ownMeshChestSnapshot(world,player,"closed");
  }

void characterIdleSlopeSample(World& world,Npc& player,std::string_view label) {
  auto& fixture=bench.characterIdleSlope;
  auto* actor=fixture.npc?bench.npcCharacterRoute.npc:&player;
  if(!fixture.prepared || !actor) {arenaAbort("character idle slope sample needs a prepared fixture");return;}
  const auto position=actor->position();
  const auto displacement=position-fixture.start,interval=position-fixture.previous;
  event("character_idle_slope_sample","\"label\":"+jsonStr(label)+",\"actor\":"+jsonStr(fixture.npc?"npc":"hero")+
    ",\"angle_deg\":"+ragdollNumber(fixture.angle)+",\"elapsed_ms\":"+std::to_string(world.tickCount()-fixture.began)+
    ",\"interval_ms\":"+std::to_string(world.tickCount()-fixture.previousTick)+
    ",\"start_cm\":"+ragdollVector(fixture.start)+",\"displacement_cm\":"+ragdollVector(displacement)+
    ",\"displacement_length_cm\":"+ragdollNumber(displacement.length())+
    ",\"interval_displacement_cm\":"+ragdollVector(interval)+",\"interval_displacement_length_cm\":"+ragdollNumber(interval.length())+
    ","+npcCharacterSample(world,*actor,0,0,bench.step,bench.frameMs));
  fixture.previous=position;fixture.previousTick=world.tickCount();
  }

void characterIdleSlopePrepare(World& world,Npc& player,float requestedAngle,bool npc) {
  if(!std::isfinite(requestedAngle) || requestedAngle<5.f || requestedAngle>65.f) {
    arenaAbort("character idle slope angle must be between 5 and 65 degrees");return;
    }
  npcCharacterStopMotionTrace(&world);
  auto& route=bench.npcCharacterRoute;
  if(route.npc) {
    if(bench.subject==route.npc)bench.subject=nullptr;
    world.removeNpc(*route.npc);
    }
  route=Bench::NpcCharacterRoute();bench.characterIdleSlope=Bench::CharacterIdleSlope();
  bench.characterRoute.active=false;
  auto* actor=&player;
  if(npc) {
    route.npc=npcCharacterActor(world,player.position()+Vec3(400,0,0));
    if(!route.npc)return;
    actor=route.npc;
    }
  const auto capsule=actor->navigationCapsule();
  const float radius=capsule.x;
  const auto* bounds=actor->bBoxCol();
  const float height=bounds?std::max(bounds[1].y-bounds[0].y,2.f*radius):capsule.y;
  // Match the native capsule's 1 cm padding. Placing the logical feet directly
  // on a sloping triangle would insert its lower sphere into the ground and
  // count the first depenetration as idle movement. This is an analytic tangent
  // pose, with no simulation tick, settling period or discarded measurement.
  constexpr float padding=1.f;
  const float footprint=std::max(60.f,2.f*(radius+padding));
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {if(!point.underWater)points.push_back(&point);});
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  auto* physics=world.physic();
  Vec3 floor,normal;std::string waypoint;float measuredAngle=0,maximumPlaneError=0,maximumNormalAngle=0;
  const float radians=float(M_PI)/180.f;
  // The waypoint order and offsets are fixed, so separate ON/OFF processes
  // select exactly the same authored landscape triangle.
  for(const auto* point:points) {
    for(float x:{0.f,-80.f,80.f,-160.f,160.f,-240.f,240.f,-400.f,400.f})
      for(float z:{0.f,-80.f,80.f,-160.f,160.f,-240.f,240.f,-400.f,400.f}) {
        if(!waypoint.empty())continue;
        const auto center=physics->landRay(point->position()+Vec3(x,300,z),1200.f);
        // landRay only exposes normals for C_Landscape, which is static stock
        // geometry; zero-normal object tops and movable props are excluded.
        if(!center.hasCol || center.n.length()<.99f || center.n.y<=.25f)continue;
        const auto n=Vec3::normalize(center.n);
        const float angle=std::acos(std::clamp(n.y,-1.f,1.f))/radians;
        const float angleTolerance=requestedAngle>=45.f && requestedAngle<50.f?.75f:2.f;
        if(std::abs(angle-requestedAngle)>angleTolerance)continue;
        float planeError=0,normalAngle=0;bool planar=true;
        for(int ix=-2;ix<=2 && planar;++ix)for(int iz=-2;iz<=2;++iz) {
          const auto offset=Vec3(float(ix)*footprint*.5f,0,float(iz)*footprint*.5f);
          const float y=center.v.y-Vec3::dotProduct(n,offset)/n.y;
          const auto hit=physics->landRay(Vec3(center.v.x+offset.x,y+30.f,center.v.z+offset.z),60.f);
          if(!hit.hasCol || hit.n.length()<.99f) {planar=false;break;}
          const float error=std::abs(Vec3::dotProduct(hit.v-center.v,n));
          const float normalError=std::acos(std::clamp(Vec3::dotProduct(n,Vec3::normalize(hit.n)),-1.f,1.f))/radians;
          if(error>.25f || normalError>1.f) {planar=false;break;}
          planeError=std::max(planeError,error);normalAngle=std::max(normalAngle,normalError);
          }
        if(!planar)continue;
        const auto feet=center.v+Vec3(0,(radius+padding)/n.y-radius-padding,0);
        if(physics->ray(feet+Vec3(0,radius+padding,0),feet+Vec3(0,height+padding,0)).hasCol)continue;
        for(unsigned direction=0;direction<8 && planar;++direction) {
          const float yaw=float(direction)*float(M_PI)/4.f;
          const Vec3 delta(std::cos(yaw)*(radius+padding),0,std::sin(yaw)*(radius+padding));
          for(float y:{radius+padding,height-radius})
            if(physics->ray(feet+Vec3(0,y,0),feet+Vec3(0,y,0)+delta).hasCol) {planar=false;break;}
          }
        if(!planar)continue;
        floor=center.v;normal=n;waypoint=point->name;measuredAngle=angle;
        maximumPlaneError=planeError;maximumNormalAngle=normalAngle;
        }
    if(!waypoint.empty())break;
    }
  if(waypoint.empty()) {arenaAbort("no static planar landscape footprint for idle slope "+std::to_string(requestedAngle));return;}
  auto downhill=Vec3(normal.x,0,normal.z);downhill=Vec3::normalize(downhill);
  // Jolt also translates the shape upward by its padding.
  const float verticalOffset=(radius+padding)/normal.y-radius-padding;
  const auto start=floor+Vec3(0,verticalOffset,0);
  characterFreezeNear(world,player,floor,floor);
  actor->setInteraction(nullptr,true);actor->clearAiQueue();actor->stopWalking();actor->clearSpeed();
  actor->setPosition(start);actor->setDirection(downhill);actor->updateTransform();
  actor->setCharacterVirtualDiagnosticsEnabled(true);
  bench.npcCharacterTraceActors.push_back({actor,start,actor->characterVisualPosition(),false});
  if(npc) {
    actor->aiPush(AiQueue::aiWait(60000));
    npcCharacterPlaceHero(world,player,start,start+downhill*200.f);
    bench.subject=actor;
    } else bench.subject=nullptr;
  auto& fixture=bench.characterIdleSlope;
  fixture.prepared=true;fixture.npc=npc;fixture.angle=measuredAngle;
  fixture.began=world.tickCount();fixture.previousTick=world.tickCount();fixture.start=start;fixture.previous=start;
  bench.hasPrev=false;
  bench.cam=C_Follow;bench.camDist=300;bench.camHeight=100;bench.camAngle=110;bench.camAuto=true;
  event("character_idle_slope_setup","\"actor\":"+jsonStr(npc?"npc":"hero")+",\"waypoint\":"+jsonStr(waypoint)+
    ",\"requested_angle_deg\":"+ragdollNumber(requestedAngle)+",\"angle_deg\":"+ragdollNumber(measuredAngle)+
    ",\"floor_cm\":"+ragdollVector(floor)+",\"normal\":"+ragdollVector(normal)+",\"start_cm\":"+ragdollVector(start)+
    ",\"capsule_radius_cm\":"+ragdollNumber(radius)+",\"capsule_height_cm\":"+ragdollNumber(height)+
    ",\"character_padding_cm\":"+ragdollNumber(padding)+",\"vertical_offset_cm\":"+ragdollNumber(verticalOffset)+
    ",\"footprint_half_width_cm\":"+ragdollNumber(footprint)+",\"footprint_samples\":25,\"warmup_ms\":0"+
    ",\"maximum_plane_error_cm\":"+ragdollNumber(maximumPlaneError)+",\"maximum_normal_angle_deg\":"+ragdollNumber(maximumNormalAngle));
  characterIdleSlopeSample(world,player,"initial");
  }

// Idle regressions use ordinary MoveAlgo ticks, including a real AI wait for
// the NPC. Coordinates change only here; sampling never pins the actor in place.
void characterIdlePrepare(World& world,Npc& player,PlayerControl& ctrl,std::string_view role,std::string_view site) {
  if(role!="hero" && role!="npc") {arenaAbort("character idle requires hero or npc");return;}
  Vec3 start(13881.523f,1672.283f,-15549.843f);
  float yaw=0;
  std::string waypoint="NW_FARM1_PATH_01";
  if(site=="market") {
    start=Vec3(6718.370605f,270.145355f,-6029.716797f);yaw=-89.73f;waypoint="market_void";
    }
  else if(site=="steeper") {
    const auto& guild=world.script().guildVal();
    const float maximum=std::min({32.f,float(guild.slide_angle[player.guild()])-2.f,float(guild.slide_angle[GIL_BDT])-2.f});
    std::vector<const WayPoint*> points;
    world.forEachWayPoint([&](const WayPoint& point) {if(!point.underWater)points.push_back(&point);});
    std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
    bool found=false;
    for(const auto* point:points) {
      for(float x:{0.f,-120.f,120.f,-240.f,240.f})for(float z:{0.f,-120.f,120.f,-240.f,240.f}) {
        if(found)continue;
        Vec3 floor;if(!characterFloor(world,point->position()+Vec3(x,0,z),floor))continue;
        const auto hit=world.physic()->landRay(floor+Vec3(0,20,0),50);
        if(!hit.hasCol || hit.n.y<=0)continue;
        const float normalAngle=std::acos(std::clamp(hit.n.y,0.f,1.f))*180.f/float(M_PI);
        if(normalAngle<18.f || normalAngle>maximum)continue;
        float angle=0,rough=0;Vec3 downhill;
        if(!slopeAt(world,floor,angle,downhill,rough) || angle<18.f || angle>maximum || rough>3.f)continue;
        Vec3 low,high;
        if(!characterFloor(world,floor+downhill*80.f,low) || !characterFloor(world,floor-downhill*80.f,high))continue;
        if(!characterLane(world,low,high,35.f))continue;
        start=floor;waypoint=point->name;found=true;
        }
      if(found)break;
      }
    if(!found) {arenaAbort("character idle steeper slope lacks stock walkable geometry");return;}
    }
  else if(site!="gentle") {arenaAbort("unknown character idle site");return;}
  const auto floor=world.physic()->landRay(start+Vec3(0,20,0),80);
  if(!floor.hasCol || std::abs(floor.v.y-start.y)>20.f) {arenaAbort("character idle start has no expected floor");return;}
  npcCharacterStopMotionTrace(&world);
  auto& idle=bench.characterIdle;
  bench.subject=nullptr;
  if(idle.fixtureNpc) {world.removeNpc(*idle.fixtureNpc);idle.fixtureNpc=nullptr;}
  idle.actor=nullptr;
  ctrl.clearInput();bench.holds.clear();bench.taps.clear();
  bench.characterRoute=Bench::CharacterRoute();bench.npcCharacterRoute.active=false;
  characterFreezeNear(world,player,start,start);
  player.setInteraction(nullptr,true);player.clearAiQueue();player.clearGoTo();player.clearSpeed();player.stopWalking();player.closeWeapon(true);
  if(role=="npc") {
    npcCharacterPlaceHero(world,player,start,start+Vec3(1,0,0));
    idle.fixtureNpc=world.addNpc(world.script().findSymbolIndex("BDT_1013_BANDIT_L"),start);
    if(!idle.fixtureNpc) {arenaAbort("character idle NPC insertion failed");return;}
    auto& npc=*idle.fixtureNpc;
    npc.setInteraction(nullptr,true);npc.clearAiQueue();npc.clearState(false);npc.clearGoTo();npc.clearSpeed();npc.stopWalking();npc.closeWeapon(true);
    npc.setExternalControl(true);npc.setProcessPolicy(NpcProcessPolicy::AiNormal);npc.aiPush(AiQueue::aiWait(60000));
    idle.actor=&npc;bench.subject=&npc;
    }
  else idle.actor=&player;
  idle.actor->setPosition(start);idle.actor->setDirection(yaw);idle.actor->updateTransform();
  idle.site=site;idle.role=role;idle.initial=idle.actor->position();idle.began=world.tickCount();
  idle.actor->setCharacterVirtualDiagnosticsEnabled(true);
  bench.npcCharacterTraceActors.push_back({idle.actor,idle.actor->position(),idle.actor->characterVisualPosition(),false});
  bench.hasPrev=false;bench.cam=C_Follow;bench.camDist=350;bench.camHeight=110;bench.camAngle=110;bench.camAuto=true;
  event("character_idle_setup","\"role\":"+jsonStr(role)+",\"site\":"+jsonStr(site)+",\"waypoint\":"+jsonStr(waypoint)+
    ",\"root_cm\":"+ragdollVector(start)+",\"floor_cm\":"+ragdollVector(floor.v)+",\"floor_normal\":"+ragdollVector(floor.n)+
    ",\"slope_deg\":"+ragdollNumber(std::acos(std::clamp(floor.n.y,0.f,1.f))*180.f/float(M_PI))+
    ",\"yaw_deg\":"+ragdollNumber(yaw)+",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"hero_distance_cm\":"+ragdollNumber((player.position()-idle.actor->position()).length()));
  }

void characterIdleSnapshot(World& world,std::string_view label,bool begin=false) {
  auto& idle=bench.characterIdle;
  if(!idle.actor) {arenaAbort("character idle snapshot needs a prepared actor");return;}
  if(begin) {idle.initial=idle.actor->position();idle.began=world.tickCount();}
  const auto delta=idle.actor->position()-idle.initial;
  event("character_idle_snapshot","\"label\":"+jsonStr(label)+",\"role\":"+jsonStr(idle.role)+",\"site\":"+jsonStr(idle.site)+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-idle.began)+",\"initial_root_cm\":"+ragdollVector(idle.initial)+
    ",\"drift_cm\":"+ragdollNumber(delta.length())+",\"horizontal_drift_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+","+
    npcCharacterSample(world,*idle.actor,0,0,bench.step,bench.frameMs));
  }

// Monster fixtures use the same MDH capsule and ordinary AI/animation movement
// as the game. Only preparation places actors; measurements never move them.
void monsterCharacterError(std::string_view reason) {
  event("monster_character_error","\"reason\":"+jsonStr(reason));arenaAbort(reason);
  }

Npc* monsterCharacterActor(World& world,std::string_view instance,const Vec3& at) {
  auto* npc=world.addNpc(world.script().findSymbolIndex(instance),at);
  if(!npc || npc->isHuman() || npc->isDown() || !npc->visualSkeleton()) {
    monsterCharacterError("monster fixture instance unavailable: "+std::string(instance));return nullptr;
    }
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->stopWalking();
  npc->setExternalControl(true);npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Run);
  npc->setPosition(at);npc->updateTransform();
  const auto capsule=npc->navigationCapsule();const auto& guild=world.script().guildVal();const auto g=npc->guild();
  const auto& collision=npc->visualSkeleton()->bboxCol;const float mdhHeight=collision[1].y-collision[0].y;
  event("monster_character_body","\"instance\":"+jsonStr(instance)+",\"npc_id\":"+std::to_string(world.npcId(npc))+
    ",\"guild\":"+std::to_string(g)+",\"radius_cm\":"+ragdollNumber(capsule.x)+",\"height_cm\":"+ragdollNumber(mdhHeight)+
    ",\"mdh_collision_height_cm\":"+ragdollNumber(mdhHeight)+",\"cv_height_cm\":"+ragdollNumber(mdhHeight)+
    ",\"legacy_query_height_cm\":"+ragdollNumber(capsule.y)+
    ",\"step_height_cm\":"+ragdollNumber(guild.step_height[g]>0?float(guild.step_height[g]):50.f)+
    ",\"slide_angle_deg\":"+ragdollNumber(float(guild.slide_angle[g]))+",\"mass_kg\":"+ragdollNumber(npc->physicsMass())+
    ",\"water_knee_cm\":"+ragdollNumber(float(guild.water_depth_knee[g]))+
    ",\"water_chest_cm\":"+ragdollNumber(float(guild.water_depth_chest[g]))+
    ",\"has_swim_animations\":"+(npc->hasSwimAnimations()?"true":"false"));
  return npc;
  }

void monsterCharacterClear(World& world) {
  auto& route=bench.monsterCharacterRoute;
  route.active=false;
  if(route.npc) {
    auto* npc=route.npc;route.npc=nullptr;
    for(auto& actor:bench.monsterCharacterExtras)if(actor.npc==npc)actor.npc=nullptr;
    world.removeNpc(*npc);
    }
  for(auto& actor:bench.monsterCharacterExtras)if(actor.npc) {
    auto* npc=actor.npc;actor.npc=nullptr;world.removeNpc(*npc);
    }
  bench.monsterCharacterExtras.clear();bench.monsterCharacterProps.clear();
  bench.monsterCharacterBarrel=Bench::MonsterCharacterProp();
  bench.monsterCharacterMeasuring=false;bench.monsterCharacterCrowd=false;bench.monsterCharacterPack=false;
  bench.characterProp.reset();bench.characterPropId=0;bench.characterPropTravel=0;
  bench.monsterCharacterLedgeBody=DynamicWorld::Item();bench.monsterCharacterLedgeShape.reset();
  route=Bench::MonsterCharacterRoute();
  }

void monsterCharacterLedge(World& world,Npc& player,std::string_view species,float height,float bottom=0.f) {
  const bool wolf=species=="wolf",troll=species=="troll";
  if((!wolf && !troll) || (wolf && height!=20.f && height!=40.f && height!=55.f) ||
     (troll && height!=40.f && height!=80.f) ||
     (bottom!=0.f && !(wolf && height==40.f && bottom==-100.f))) {
    monsterCharacterError("ledge expects wolf 20|40|55 or troll 40|80 centimetres");return;
    }
  monsterCharacterClear(world);
  Vec3 anchor;
  if(!characterFloor(world,Vec3(38674.5156f,4308.43604f,11976.749f),anchor)) {
    monsterCharacterError("isolated ledge lacks its verified stock anchor");return;
    }
  // One analytic riser, with a 6 m running approach and 15 m landing. The
  // elevated support excludes stock rocks/actors while remaining part of the
  // real World queried by both legacy MoveAlgo and CharacterVirtual.
  anchor.y+=10000.f;
  zenkit::Mesh mesh;mesh.materials.resize(1);mesh.materials[0].group=zenkit::MaterialGroup::STONE;
  const auto quad=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
    const auto index=uint32_t(mesh.vertices.size());
    for(const auto& point:{a,b,c,d})mesh.vertices.push_back({point.x,point.y,point.z});
    mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),
      {index,index+2,index+1,index,index+3,index+2});
    mesh.polygons.material_indices.insert(mesh.polygons.material_indices.end(),{0,0});
    };
  // Gothic source winding is reversed by PhysicVbo.
  quad({-1200,0,-900},{-1200,0,900},{1500,0,900},{1500,0,-900});
  quad({0,height,-900},{0,height,900},{1500,height,900},{1500,height,-900});
  quad({0,bottom,-900},{0,bottom,900},{0,height,900},{0,height,-900});
  bench.monsterCharacterLedgeShape.reset(PhysicMeshShape::load(PackedMesh(mesh,PackedMesh::PK_Physic)));
  auto matrix=Matrix4x4::mkIdentity();matrix.translate(anchor);
  bench.monsterCharacterLedgeBody=world.physic()->addStaticObj(bench.monsterCharacterLedgeShape.get(),matrix,"BENCH_MONSTER_SINGLE_LEDGE");
  if(bench.monsterCharacterLedgeBody.isEmpty()) {monsterCharacterError("isolated ledge collision creation failed");return;}
  auto& route=bench.monsterCharacterRoute;
  route.name=std::string(species)+"_ledge_"+std::to_string(unsigned(height));route.isolatedLedge=true;
  if(bottom<0.f)route.name+="_buried";
  route.start=anchor+Vec3(-600,0,0);route.goal=anchor+Vec3(800,height,0);route.tolerance=60;
  for(const auto& point:{route.start,anchor+Vec3(200,height,0),route.goal}) {
    const auto floor=world.physic()->landRay(point+Vec3(0,20,0),40);
    if(!floor.hasCol || std::abs(floor.v.y-point.y)>.1f) {
      monsterCharacterError("isolated ledge floor proof disagrees with authored surface");return;
      }
    }
  const auto face=world.physic()->ray(anchor+Vec3(-30,height*.5f,0),anchor+Vec3(30,height*.5f,0));
  if(!face.hasCol || std::abs(face.v.x-anchor.x)>.1f) {
    monsterCharacterError("isolated ledge lacks its single vertical face");return;
    }
  player.setInteraction(nullptr,true);player.clearAiQueue();player.clearGoTo();player.stopWalking();
  player.setPosition(anchor+Vec3(-600,0,500));player.updateTransform();
  route.npc=monsterCharacterActor(world,wolf?"Wolf":"Troll",route.start);if(!route.npc)return;
  route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();
  route.npc->setCharacterVirtualDiagnosticsEnabled(true);
  bench.subject=route.npc;bench.monsterCharacterHeroStart=player.position();
  event("monster_character_route_setup","\"route\":"+jsonStr(route.name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"waypoint\":\"verified_wolf_lunge+100m\",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"arrival_radius_cm\":60,\"arrival_height_tolerance_cm\":5"+
    ",\"ledge_height_cm\":"+ragdollNumber(height)+",\"face_bottom_cm\":"+ragdollNumber(bottom)+
    ",\"running_approach_cm\":600,\"landing_length_cm\":1500"+
    ",\"native_action\":\"Npc::goToPosition\",\"synthetic_geometry\":true,\"floor_probes\":3,\"face_proof\":true");
  }

bool monsterCharacterOpenLane(World& world,const Vec3& center,const Vec3& direction,float halfLength,float radius,float height,
                              Vec3& start,Vec3& goal,float maximumStep=25.f) {
  const Vec3 side(-direction.z,0,direction.x);
  if(!characterFloor(world,center-direction*halfLength,start) || !characterFloor(world,center+direction*halfLength,goal))return false;
  for(float across:{-radius,0.f,radius}) {
    Vec3 previous;
    for(unsigned sample=0;sample<=20;++sample) {
      Vec3 floor;
      if(!characterFloor(world,start+(goal-start)*(float(sample)/20.f)+side*across,floor))return false;
      if(sample && std::abs(floor.y-previous.y)>maximumStep)return false;
      if(sample)for(float y:{25.f,height*.5f,height-10.f})
        if(world.physic()->ray(previous+Vec3(0,y,0),floor+Vec3(0,y,0)).hasCol)return false;
      if(std::abs(floor.y-(start.y+(goal.y-start.y)*(float(sample)/20.f)))>50.f)return false;
      previous=floor;
      }
    }
  return true;
  }

Bench::MonsterCharacterExtra& monsterCharacterExtra(World& world,Npc* npc,std::string_view role) {
  bench.monsterCharacterExtras.emplace_back();auto& actor=bench.monsterCharacterExtras.back();
  actor.npc=npc;actor.role=role;actor.start=npc->position();
  actor.lastPosition=actor.start;actor.lastVisual=npc->characterVisualPosition();
  actor.progressPosition=actor.start;actor.lastSupported=actor.start;actor.progressTime=world.tickCount();
  return actor;
  }

void monsterCharacterPoint(Bench::MonsterCharacterExtra& actor,const Vec3& at) {
  actor.points.emplace_back(at,"FP_MONSTER_VIDEO_"+actor.role+"_"+std::to_string(actor.points.size()));
  actor.points.back().groundPos=at;
  }

bool monsterCharacterProp(World& world,std::string_view role,const Vec3& at) {
  const std::string visual=role=="crate"?"NW_HARBOUR_CRATE_01.3DS":"NW_HARBOUR_BARREL_01.3DS";
  const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
  if(!mesh || !rule) {monsterCharacterError("stock monster prop mesh/class unavailable: "+std::string(role));return false;}
  Vec3 floor;if(!characterFloor(world,at,floor)) {monsterCharacterError("monster prop has no stock support");return false;}
  const auto* bounds=mesh->bbox();auto matrix=Matrix4x4::mkIdentity();
  matrix.translate(floor.x-(bounds[0].x+bounds[1].x)*.5f,floor.y-bounds[0].y+.2f,floor.z-(bounds[0].z+bounds[1].z)*.5f);
  zenkit::VirtualObject data;data.type=zenkit::VirtualObjectType::zCVob;data.id=uint32_t(-1);
  data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
  data.visual=std::make_shared<zenkit::VisualMesh>();data.visual->type=zenkit::VisualType::MESH;data.visual->name=visual;
  const auto before=world.physic()->movableVobSnapshot();
  auto fixture=std::make_unique<Bench::MovableFixture>();fixture->visual.setVisual(data,world,false,false);fixture->visual.setObjMatrix(matrix);
  fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
  if(fixture->body.isEmpty()) {monsterCharacterError("stock monster prop body unavailable");return false;}
  fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& m){view->setObjMatrix(m);});
  fixture->visual.setupFracture(world,fixture->body);
  uintptr_t id=0;
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {id=body.id;break;}
  if(!id) {monsterCharacterError("stock monster prop snapshot unavailable");return false;}
  event("monster_character_prop_setup","\"role\":"+jsonStr(role)+",\"mass_kg\":"+ragdollNumber(fixture->body.mass())+
    ",\"position_cm\":"+ragdollVector(movablePosition(matrix))+",\"blocks_walking\":"+(rule->blocksWalking?"true":"false"));
  bench.monsterCharacterProps.push_back({std::move(fixture),std::string(role),movablePosition(matrix),movablePosition(matrix),id});return true;
  }

void monsterCharacterSampleProps(World& world) {
  if(bench.monsterCharacterProps.empty() && !bench.monsterCharacterBarrel.id)return;
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto fragments=world.physic()->fragmentSnapshot();
  auto sample=[&](Bench::MonsterCharacterProp& prop) {
    for(const auto& body:bodies)if(body.id==prop.id) {
      // Native snapshots retain the last intact matrix after fracture. Item's
      // body pointer has been released then, so Item::matrix() is identity.
      prop.lastIntact=movablePosition(body.transform);prop.broken=body.broken;
      prop.travel=std::max(prop.travel,(prop.lastIntact-prop.start).length());
      break;
      }
    prop.fragments=unsigned(std::count_if(fragments.begin(),fragments.end(),[&](const auto& part){return part.parentId==prop.id;}));
    };
  for(auto& prop:bench.monsterCharacterProps)sample(prop);
  if(bench.monsterCharacterBarrel.id) {
    sample(bench.monsterCharacterBarrel);
    bench.characterPropTravel=bench.monsterCharacterBarrel.travel;
    }
  }

void monsterCharacterPropEnd(const Bench::MonsterCharacterProp& prop,std::string_view route) {
  event("monster_character_prop_end","\"route\":"+jsonStr(route)+",\"role\":"+jsonStr(prop.role)+
    ",\"max_displacement_cm\":"+ragdollNumber(prop.travel)+",\"initial_cm\":"+ragdollVector(prop.start)+
    ",\"broken\":"+(prop.broken?"true":"false")+",\"last_intact_cm\":"+ragdollVector(prop.lastIntact)+
    ",\"fragment_count\":"+std::to_string(prop.fragments)+",\"end_cm\":"+(prop.broken?"null":ragdollVector(prop.lastIntact)));
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
  bench.monsterCharacterRenderRequest=&npc;const bool drawable=TestBench::ragdollBodyDrawable(npc);bench.monsterCharacterRenderRequest=nullptr;
  event("monster_character_render","\"route\":"+jsonStr(bench.monsterCharacterRoute.name)+",\"role\":"+jsonStr(role)+
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
  const auto& route=bench.monsterCharacterRoute;
  if(bench.monsterCharacterCrowd) {
    if(requested) {monsterCharacterError("monster camera angle selection requires a prepared video fixture");return;}
    event("monster_character_camera","\"route\":\"performance\",\"eye_cm\":"+ragdollVector(bench.camPos)+",\"fixed\":true");return;
    }
  if(!route.npc) {monsterCharacterError("monster camera requires a prepared fixture");return;}
  std::vector<Vec3> points{route.start,route.goal,bench.monsterCharacterHeroStart};
  for(const auto& actor:bench.monsterCharacterExtras) {
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
  for(const auto& actor:bench.monsterCharacterExtras)if(actor.npc)envelope(*actor.npc);
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
      if(pointId>=actorPoints && bench.characterProp) {
        const auto delta=point-bench.characterPropInitial;propFootprint=std::hypot(delta.x,delta.z)<120;
        }
      if(pointId>=actorPoints)for(const auto& prop:bench.monsterCharacterProps) {
        const auto delta=point-prop.start;propFootprint|=std::hypot(delta.x,delta.z)<120;
        }
      if(!propFootprint && hidden(world,candidate,point+Vec3(0,y,0))) {visible=false;break;}
      }
    if(!visible)continue;
    eye=candidate;found=true;selectedYaw=yaw;selectedElevation=elevation;fallback=requested && shot!=0;break;
    }
  if(!found) {monsterCharacterError("monster fixed camera cannot see the complete route and feet");return;}
  bench.cam=C_Water;bench.waterCameraTarget=target;bench.camAuto=false;bench.smValid=false;lookFrom(eye,target);
  std::string actors="[{\"npc_id\":"+std::to_string(world.npcId(route.npc))+",\"role\":\"primary\",\"start_cm\":"+ragdollVector(route.start)+"}";
  for(const auto& actor:bench.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    actors+=",{\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"role\":"+jsonStr(actor.role)+",\"start_cm\":"+ragdollVector(actor.start)+"}";
  std::string ready="[{\"npc_id\":"+std::to_string(world.npcId(route.npc))+",\"root_cm\":"+ragdollVector(route.npc->position())+"}";
  for(const auto& actor:bench.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    ready+=",{\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"root_cm\":"+ragdollVector(actor.npc->position())+"}";
  event("monster_character_camera","\"route\":"+jsonStr(route.name)+",\"camera_cm\":"+ragdollVector(eye)+
    ",\"requested_shot\":"+(requested?"true":"false")+",\"requested_yaw_deg\":"+ragdollNumber(preferredYaw)+
    ",\"requested_elevation_fraction\":"+ragdollNumber(preferredElevation)+",\"selected_yaw_deg\":"+ragdollNumber(selectedYaw)+
    ",\"selected_elevation_fraction\":"+ragdollNumber(selectedElevation)+",\"used_fallback\":"+(fallback?"true":"false")+
    ",\"seed\":"+std::to_string(bench.monsterCharacterSeed)+",\"hour\":"+std::to_string(world.time().hour())+
    ",\"minute\":"+std::to_string(world.time().minute())+",\"hero_cm\":"+ragdollVector(bench.monsterCharacterHeroStart)+",\"actors\":"+actors+"]"+
    ",\"ready_actors\":"+ready+"],\"hero_ready_cm\":"+ragdollVector(player.position())+
    ",\"target_cm\":"+ragdollVector(target)+",\"bounds_min_cm\":"+ragdollVector(low)+",\"bounds_max_cm\":"+ragdollVector(high)+
    ",\"horizontal_actor_padding_cm\":"+ragdollNumber(horizontalPadding)+",\"fov_y\":"+ragdollNumber(camera->fovY())+",\"aspect\":"+ragdollNumber(aspect)+",\"fixed\":true");
  bool rendered=monsterCharacterRenderProof(world,player,"hero");
  rendered=monsterCharacterRenderProof(world,*route.npc,"primary") && rendered;
  for(auto& actor:bench.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    rendered=monsterCharacterRenderProof(world,*actor.npc,actor.role) && rendered;
  if(!rendered)monsterCharacterError("monster fixed camera requires drawable actor meshes and their complete projected poses");
  }

void monsterCharacterRemoveAmbient(World& world,const Npc& player,const Vec3& center,float radius,std::string_view site) {
  std::vector<Npc*> ambient;
  for(uint32_t id=0;id<world.npcCount();++id)if(auto* npc=world.npcById(id);npc && npc!=&player && !npc->isHuman() &&
      npc!=bench.monsterCharacterRoute.npc && (npc->position()-center).length()<=radius &&
      std::none_of(bench.monsterCharacterExtras.begin(),bench.monsterCharacterExtras.end(),[&](const auto& actor){return actor.npc==npc;}))
    ambient.push_back(npc);
  for(auto* npc:ambient) {
    if(bench.subject==npc)bench.subject=nullptr;
    world.removeNpc(*npc);
    }
  event("monster_character_ambient_removed","\"site\":"+jsonStr(site)+",\"center_cm\":"+ragdollVector(center)+
    ",\"radius_cm\":"+ragdollNumber(radius)+",\"nonhuman_count\":"+std::to_string(ambient.size()));
  }

void monsterCharacterPack(World& world,Npc& player) {
  monsterCharacterClear(world);auto& route=bench.monsterCharacterRoute;route.name="wolf_pack";
  std::vector<const WayPoint*> points;
  if(const auto* known=world.findPoint("FP_ROAM_NW_BIGFARM_LAKE_MONSTER_02_01"))points.push_back(known);
  std::vector<Vec3> ring;Vec3 center;std::string waypoint;float roughness=0,heightRange=0;unsigned stoneSupports=0;
  for(const auto* point:points) {
    if(!characterFloor(world,point->pos,center))continue;
    float angle=0,rough=0;Vec3 downhill;
    if(!slopeAt(world,center,angle,downhill,rough) || angle>25)continue;
    for(float radius:{230.f,300.f,370.f}) {
      ring.clear();float low=1e9f,high=-1e9f;bool valid=true;unsigned stone=0;
      for(unsigned i=0;i<12;++i) {
        const float yaw=float(i)*float(M_PI)/6.f;Vec3 floor;
        if(!characterFloor(world,center+Vec3(std::cos(yaw)*radius,0,std::sin(yaw)*radius),floor) ||
           std::abs(floor.y-center.y)>180) {valid=false;break;}
        const auto support=world.physic()->landRay(floor+Vec3(0,15,0),30);
        if(!support.hasCol || support.n.y<std::cos(30.f*float(M_PI)/180.f) ||
           world.physic()->waterRay(floor+Vec3(0,1,0),0).hasCol) {valid=false;break;}
        stone+=support.mat==zenkit::MaterialGroup::STONE;
        if(!ring.empty() && !characterLane(world,ring.back(),floor,35)) {valid=false;break;}
        low=std::min(low,floor.y);high=std::max(high,floor.y);ring.push_back(floor);
        }
      if(!valid || ring.size()!=12 || stone<4 || !characterLane(world,ring.back(),ring.front(),35) || high-low<30 || high-low>220)continue;
      // Real terrain height variation keeps the ring visibly uneven. Stock
      // forest surfaces provide the rock/ground, with no synthetic nav mesh.
      waypoint=point->name;roughness=rough;heightRange=high-low;stoneSupports=stone;break;
      }
    if(!waypoint.empty())break;
    }
  if(waypoint.empty()) {monsterCharacterError("wolf pack lacks a supported uneven stock terrain ring");return;}
  monsterCharacterRemoveAmbient(world,player,center,3000,"wolf_pack");
  characterFreezeNear(world,player,center,center);player.stopWalking();player.clearAiQueue();player.setPosition(center);player.updateTransform();
  for(unsigned i=0;i<3;++i) {
    const auto index=i*4;auto* npc=monsterCharacterActor(world,"Wolf",ring[index]);if(!npc)return;
    npc->setDirection(ring[(index+11)%12]-ring[index]);npc->updateTransform();
    auto& actor=monsterCharacterExtra(world,npc,"wolf"+std::to_string(i+1));actor.loop=true;
    for(unsigned j=1;j<=12;++j)monsterCharacterPoint(actor,ring[(index+24-j)%12]);
    if(i==0)route.npc=npc;
    }
  route.start=ring.front();route.goal=ring[6];bench.subject=route.npc;bench.monsterCharacterPack=true;bench.monsterCharacterHeroStart=player.position();
  event("monster_character_pack_setup","\"route\":\"wolf_pack\",\"actors\":3,\"waypoint\":"+jsonStr(waypoint)+
    ",\"hero_cm\":"+ragdollVector(center)+",\"ring_height_range_cm\":"+ragdollNumber(heightRange)+
    ",\"roughness_cm\":"+ragdollNumber(roughness)+",\"stone_supports\":"+std::to_string(stoneSupports)+
    ",\"ring_supports\":12,\"native_action\":\"AI_GoToPoint\"");
  }

void monsterCharacterCliff(World& world,Npc& player,bool higher=false) {
  monsterCharacterClear(world);auto& route=bench.monsterCharacterRoute;
  route.name=higher?"molerat_cliff_46":"molerat_cliff_36";
  const std::string waypoint=higher?"NW_XARDAS_TOWER_VALLEY_02":"NW_XARDAS_TOWER_VALLEY_03_C";
  const auto* point=world.findPoint(waypoint);
  Vec3 center,downhill;float angle=0,rough=0;
  if(!point || !characterFloor(world,point->groundPos,center,2000) || !slopeAt(world,center,angle,downhill,rough) ||
     !characterFloor(world,center-rotY(Vec3(1,0,0),higher?135.f:90.f)*100.f,route.start,2000) ||
     !characterFloor(world,center+rotY(Vec3(1,0,0),higher?135.f:90.f)*500.f,route.goal,2000)) {
    monsterCharacterError("known cliff waypoint has no supported ledge endpoints: "+waypoint);return;
    }
  monsterCharacterRemoveAmbient(world,player,center,3000,"cliff");
  characterFreezeNear(world,player,route.start,route.goal);npcCharacterPlaceHero(world,player,route.start,route.goal);
  route.npc=monsterCharacterActor(world,"Molerat",route.start);if(!route.npc)return;
  route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();bench.subject=route.npc;
  route.destination=WayPoint(route.goal,"FP_MONSTER_CHARACTER_LEDGE");route.destination.groundPos=route.goal;
  bench.monsterCharacterHeroStart=player.position();
  const float limit=float(world.script().guildVal().slide_angle[route.npc->guild()]);
  const auto support=world.physic()->landRay(center+Vec3(0,15,0),30);
  event("monster_character_cliff_setup","\"route\":"+jsonStr(route.name)+",\"waypoint\":"+jsonStr(waypoint)+
    ",\"angle_deg\":"+ragdollNumber(angle)+",\"guild_slide_angle_deg\":"+ragdollNumber(limit)+
    ",\"support_walkable\":"+(support.hasCol && support.n.y>=std::cos(limit*float(M_PI)/180.f)?"true":"false")+
    ",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal));
  }

void monsterCharacterCrowd(World& world,Npc& player) {
  monsterCharacterClear(world);
  if(!chooseRagdollArena(world,player)) {monsterCharacterError("monster performance lacks twenty supported stock lanes");return;}
  const auto center=bench.ragdollArenaTarget-Vec3(0,80,0);
  monsterCharacterRemoveAmbient(world,player,center+Vec3(0,0,-950),2500,"performance");
  characterFreezeNear(world,player,center,center);player.stopWalking();player.clearAiQueue();player.setPosition(center+Vec3(0,0,-950));player.updateTransform();
  const char* species[]={"Wolf","Molerat","Scavenger"};
  for(size_t i=0;i<20;++i) {
    const auto at=bench.ragdollArenaPlaces[i];Vec3 end;bool found=false;
    for(const auto& direction:{Vec3(1,0,0),Vec3(-1,0,0),Vec3(0,0,1),Vec3(0,0,-1)}) {
      if(characterFloor(world,at+direction*140.f,end) && characterLane(world,at,end,20)) {found=true;break;}
      }
    if(!found) {monsterCharacterError("monster performance lane has unsupported endpoint");return;}
    auto* npc=monsterCharacterActor(world,species[i%3],at);if(!npc)return;
    npc->setDirection(end-at);npc->updateTransform();
    auto& actor=monsterCharacterExtra(world,npc,"crowd"+std::to_string(i+1));actor.loop=true;
    monsterCharacterPoint(actor,end);monsterCharacterPoint(actor,at);
    }
  bench.monsterCharacterHeroStart=player.position();
  bench.monsterCharacterCrowd=true;bench.subject=nullptr;bench.cam=C_Hold;bench.camAuto=false;
  lookFrom(center+Vec3(0,1200,1300),center+Vec3(0,40,0));
  event("monster_character_crowd_setup","\"actors\":20,\"wolves\":7,\"molerats\":7,\"scavengers\":6,\"radius_cm\":2000,\"hero_cm\":"+
    ragdollVector(player.position())+",\"waypoint\":"+jsonStr(bench.ragdollArenaWaypoint));
  }

bool monsterCharacterSlope(World& world,Vec3& start,Vec3& goal,std::string& waypoint) {
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& wp){if(!wp.underWater)points.push_back(&wp);});
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  for(const auto* point:points) {
    Vec3 center;if(!characterFloor(world,point->pos,center))continue;
    float angle=0,rough=0;Vec3 downhill;
    if(!slopeAt(world,center,angle,downhill,rough) || angle<12 || angle>25 || rough>4)continue;
    if(!monsterCharacterOpenLane(world,center,-downhill,400,20,85,start,goal,25))continue;
    if(goal.y-start.y<120)continue;
    waypoint=point->name;
    event("monster_character_surface","\"route\":\"wolf_slope\",\"angle_deg\":"+ragdollNumber(angle)+
      ",\"roughness_cm\":"+ragdollNumber(rough));return true;
    }
  return false;
  }

bool monsterCharacterCourtyard(World& world,Npc& player,const Npc&,Vec3& start,Vec3& goal,std::string& waypoint) {
  if(!characterFloor(world,Vec3(8727.44434f,128.25412f,-745.565796f),start) ||
     !characterFloor(world,Vec3(8727.44434f,128.269653f,54.4342041f),goal))return false;
  waypoint="KNOWN_CORAGON_COURTYARD";
  event("monster_character_courtyard_setup","\"outdoor\":true,\"known_site\":true,\"lane_width_cm\":700");
  characterFreezeNear(world,player,start,goal);return true;
  }

bool monsterCharacterWater(World& world,float chest,Vec3& dry,Vec3& wet,std::string& waypoint) {
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& wp) {
    if(wp.name.find("BEACH")!=std::string::npos || wp.name.find("WATER")!=std::string::npos ||
       wp.name.find("LAKE")!=std::string::npos || wp.name.find("COAST")!=std::string::npos ||
       wp.name.find("HABOUR")!=std::string::npos)points.push_back(&wp);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  for(const auto* point:points) {
    const auto ground=world.physic()->landRay(point->pos+Vec3(0,300,0),2500);
    if(!ground.hasCol || world.physic()->waterRay(ground.v,1).hasCol)continue;
    for(unsigned azimuth=0;azimuth<24;++azimuth) {
      const float angle=float(azimuth)*float(M_PI)/12.f;const Vec3 along(std::cos(angle),0,std::sin(angle));
      Vec3 previous=ground.v;
      for(float distance=25;distance<=1600;distance+=25) {
        const auto at=ground.v+along*distance;
        const auto floor=world.physic()->landRay(Vec3(at.x,ground.v.y+300,at.z),2500);
        if(!floor.hasCol || std::abs(floor.v.y-previous.y)>20 ||
           world.physic()->ray(previous+Vec3(0,40,0),floor.v+Vec3(0,40,0)).hasCol)break;
        previous=floor.v;
        const auto water=world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
        if(!water.hasCol || water.wdepth-floor.v.y<chest+25)continue;
        dry=ground.v;wet=Vec3(at.x,water.wdepth-chest,at.z);waypoint=point->name;
        event("monster_character_water_surface","\"surface_y_cm\":"+ragdollNumber(water.wdepth)+
          ",\"depth_cm\":"+ragdollNumber(water.wdepth-floor.v.y)+",\"chest_cm\":"+ragdollNumber(chest));return true;
        }
      }
    }
  return false;
  }

bool monsterCharacterChaseGoal(World& world,const Npc& npc,const Npc& player,const Vec3& start,const Vec3& obstacleEnd,
                               Vec3& hero,bool stairs=false) {
  const auto& guild=world.script().guildVal();const auto g=npc.guild();
  const float step=guild.step_height[g]>0?float(guild.step_height[g]):50.f;
  const auto shape=npc.navigationCapsule();const float height=npc.visualSkeleton()->bboxCol[1].y-npc.visualSkeleton()->bboxCol[0].y;
  const float nativeStop=npc.bodyObservationTarget(player).attackRangeCm;
  const auto along=Vec3::normalize(Vec3(obstacleEnd.x-start.x,0,obstacleEnd.z-start.z));
  const Vec3 across(-along.z,0,along.x);const float obstacleDistance=std::hypot(obstacleEnd.x-start.x,obstacleEnd.z-start.z);
  unsigned attempts=0,noFloor=0,stepRejected=0,wallRejected=0,slopeRejected=0;
  for(float extension=nativeStop+50.f;extension<=nativeStop+800.f;extension+=25.f)for(float yaw:{0.f,10.f,-10.f,20.f,-20.f}) {
    const float angle=yaw*float(M_PI)/180.f;
    const auto candidate=obstacleEnd+(along*std::cos(angle)+across*std::sin(angle))*extension;
    const auto delta=candidate-start;const float forward=Vec3::dotProduct(delta,along);
    // The actual straight native chase must still cross the selected stairs,
    // rather than walking around them to a laterally displaced observer.
    const float crossingSide=std::abs(Vec3::dotProduct(delta,across))*obstacleDistance/forward;
    if(crossingSide>45.f || extension*std::cos(angle)<nativeStop+40.f)continue;
    ++attempts;bool valid=true;Vec3 centerEnd;
    const auto direction=Vec3::normalize(Vec3(delta.x,0,delta.z));const Vec3 side(-direction.z,0,direction.x);
    const unsigned samples=unsigned(std::ceil(std::hypot(delta.x,delta.z)/15.f));
    for(float width:{0.f,-shape.x,shape.x}) {
      Vec3 previous=start+side*width;
      if(!characterFloor(world,previous,previous)) {++noFloor;valid=false;break;}
      for(unsigned sample=1;sample<=samples;++sample) {
        const float fraction=float(sample)/float(samples);
        const Vec3 probe(start.x+delta.x*fraction+side.x*width,previous.y,start.z+delta.z*fraction+side.z*width);
        Vec3 floor;
        // Follow the previously sampled support height. A long uphill segment
        // can rise beyond characterFloor's +180 cm window from its first point.
        if(!characterFloor(world,probe,floor)) {++noFloor;valid=false;break;}
        if(std::abs(floor.y-previous.y)>step) {++stepRejected;valid=false;break;}
        const float head=std::max(step+5.f,height-5.f);
        if(world.physic()->ray(previous+Vec3(0,head,0),floor+Vec3(0,head,0)).hasCol) {
          ++wallRejected;valid=false;break;
          }
        // Stair tread/riser seams can return a riser normal. Their distinct
        // risers and tread heights were checked by characterStairsProfile;
        // applying a smooth-slope normal limit here rejects valid stairs.
        if(!stairs) {
          const auto support=world.physic()->landRay(floor+Vec3(0,15,0),30.f);
          const float normalLength=support.n.length();
          if(support.hasCol && normalLength>0.f &&
             support.n.y/normalLength<std::cos(float(guild.slide_angle[g])*float(M_PI)/180.f)) {
            ++slopeRejected;valid=false;break;
            }
          }
        previous=floor;
        }
      if(!valid)break;
      if(width==0)centerEnd=previous;
      }
    if(!valid)continue;
    hero=centerEnd;
    event("monster_character_chase_destination","\"found\":true,\"attempts\":"+std::to_string(attempts)+
      ",\"native_stop_cm\":"+ragdollNumber(nativeStop)+",\"extension_cm\":"+ragdollNumber(extension)+
      ",\"yaw_offset_deg\":"+ragdollNumber(yaw)+",\"crossing_side_cm\":"+ragdollNumber(crossingSide)+
      ",\"obstacle_end_cm\":"+ragdollVector(obstacleEnd)+",\"hero_cm\":"+ragdollVector(hero));return true;
    }
  event("monster_character_chase_destination","\"found\":false,\"attempts\":"+std::to_string(attempts)+
    ",\"native_stop_cm\":"+ragdollNumber(nativeStop)+",\"no_floor\":"+std::to_string(noFloor)+
    ",\"step_rejected\":"+std::to_string(stepRejected)+",\"wall_rejected\":"+std::to_string(wallRejected)+
    ",\"slope_rejected\":"+std::to_string(slopeRejected)+",\"obstacle_end_cm\":"+ragdollVector(obstacleEnd));
  return false;
  }

bool monsterCharacterStairs(World& world,Npc& npc,Npc& player,Vec3& start,Vec3& goal,std::string& waypoint,bool stoneOnly=false) {
  std::vector<const WayPoint*> points;
  if(!stoneOnly) {
    if(const auto* point=world.findPoint("NW_CITY_TO_FARM2_09"))points.push_back(point);
    } else world.forEachWayPoint([&](const WayPoint& wp) {
    if(!wp.underWater && wp.name.starts_with("NW_CITY_") && wp.name.find("BED")==std::string::npos &&
       (!stoneOnly || wp.name.find("DITCH")==std::string::npos))points.push_back(&wp);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  if(stoneOnly)std::stable_partition(points.begin(),points.end(),[](const auto* wp){return wp->name.find("STAIR")!=std::string::npos;});
  std::stable_partition(points.begin(),points.end(),[](const auto* wp){return wp->name=="NW_CITY_TO_FOREST_STAIRS01";});
  if(stoneOnly)std::stable_partition(points.begin(),points.end(),[](const auto* point) {
    return point->name.find("_TO_")==std::string::npos && point->name.find("FOREST")==std::string::npos &&
           point->name.find("DITCH")==std::string::npos;
    });
  const auto offsets=stoneOnly?std::vector<float>{0.f,-80.f,80.f,-160.f,160.f,-240.f,240.f,-400.f,400.f,-600.f,600.f}:std::vector<float>{0.f,-80.f,80.f};
  for(const auto* point:points)for(float x:offsets)for(float z:offsets) {
    Vec3 center;if(!characterFloor(world,point->pos+Vec3(x,0,z),center))continue;
    if(stoneOnly) {
      const auto support=world.physic()->landRay(center+Vec3(0,15,0),30);
      if(!support.hasCol || support.mat!=zenkit::MaterialGroup::STONE)continue;
      }
    float angle=0,rough=0;Vec3 downhill;
    if(!slopeAt(world,center,angle,downhill,rough) || angle<(stoneOnly?10.f:18.f) || angle>40 ||
       rough<(stoneOnly?2.f:6.f) || rough>25)continue;
    Vec3 low,high;
    if(!characterFloor(world,center+downhill*100.f,low) || !characterFloor(world,center-downhill*100.f,high))continue;
    unsigned risers=0;float maximumRiser=0;
    if(!characterStairsProfile(world,low,high,risers,maximumRiser,stoneOnly?4.f:6.f,stoneOnly?40.f:45.f))continue;
    Vec3 hero;if(!monsterCharacterChaseGoal(world,npc,player,low,high,hero,true))continue;
    start=low;goal=hero;waypoint=point->name;
    event("monster_character_surface","\"route\":"+jsonStr(stoneOnly?"wolf_terrain":"wolf_stairs")+",\"waypoint\":"+jsonStr(waypoint)+
      ",\"stone_surface\":"+(stoneOnly?"true":"false")+
      ",\"angle_deg\":"+ragdollNumber(angle)+",\"roughness_cm\":"+ragdollNumber(rough)+
      ",\"riser_count\":"+std::to_string(risers)+",\"maximum_riser_cm\":"+ragdollNumber(maximumRiser));return true;
    }
  return false;
  }

void monsterCharacterPrepare(World& world,Npc& player,const std::string& name) {
  monsterCharacterClear(world);auto& route=bench.monsterCharacterRoute;route.name=name;
  route.npc=monsterCharacterActor(world,name=="troll_barrel"?"Troll":"Wolf",player.position());if(!route.npc)return;
  std::string waypoint;
  if(name=="wolf_terrain") {
    if(!prepareCharacterRoute(world,player,"stairs_up",true,"NW_CITY_TO_FOREST_STAIRS01"))return;
    const auto& stairs=bench.characterRoute;
    auto direction=stairs.goal-stairs.start;direction.y=0;
    if(direction.length()<1) {monsterCharacterError("known stairs lack a forward direction");return;}
    direction=Vec3::normalize(direction);
    auto floor=[&](const Vec3& at,Vec3& result) {
      const auto hit=world.physic()->landRay(at+Vec3(0,500,0),1000);
      if(!hit.hasCol || std::abs(hit.v.y-at.y)>180)return false;
      result=hit.v;return true;
      };
    Vec3 escape;
    if(!floor(stairs.start-direction*150.f,route.start) || !floor(stairs.goal,route.goal) ||
       !floor(stairs.goal+direction*150.f,escape)) {
      monsterCharacterError("known stairs lack supported chase and hero escape endpoints");return;
      }
    waypoint=stairs.waypoint;route.chase=true;route.heroRun=true;
    route.heroDestination=WayPoint(escape,"FP_MONSTER_VIDEO_HERO_ESCAPE");route.heroDestination.groundPos=escape;
    event("monster_character_surface","\"route\":\"wolf_terrain\",\"waypoint\":"+jsonStr(waypoint)+
      ",\"known_stairs\":true,\"hero_escape_cm\":"+ragdollVector(escape));
    } else if(name=="wolf_stairs") {
    route.npc->setCharacterVirtualDiagnosticsEnabled(true);
    if(!monsterCharacterStairs(world,*route.npc,player,route.start,route.goal,waypoint)) {
      monsterCharacterError("wolf stairs lack a supported native chase continuation");return;
      }
    route.chase=true;
    } else if(name=="wolf_slope") {
    if(!monsterCharacterSlope(world,route.start,route.goal,waypoint)) {monsterCharacterError("wolf slope lacks a supported stock lane");return;}
    route.chase=true;
    } else if(name=="troll_barrel") {
    if(!monsterCharacterCourtyard(world,player,*route.npc,route.start,route.goal,waypoint)) {monsterCharacterError("troll lacks a body-size outdoor courtyard lane");return;}
    } else if(name=="wolf_water") {
    // Read the wolf's actual guild instead of assuming human swimming depth.
    const float chest=float(world.script().guildVal().water_depth_chest[route.npc->guild()]);
    const bool canSwim=route.npc->hasSwimAnimations();
    if(!canSwim || !monsterCharacterWater(world,chest,route.start,route.goal,waypoint)) {
      monsterCharacterError("wolf water requires a continuous walkable stock bank and native swim animation");return;
      }
    } else {monsterCharacterError("unknown monster route: "+name);return;}
  characterFreezeNear(world,player,route.start,route.goal);
  if(route.chase) {
    if(name=="wolf_slope") {
      Vec3 hero;
      if(!monsterCharacterChaseGoal(world,*route.npc,player,route.start,route.goal,hero)) {
        monsterCharacterError("wolf slope has no supported hero destination beyond its obstacle");return;
        }
      route.goal=hero;
      }
    player.stopWalking();player.clearAiQueue();player.setPosition(route.goal);player.updateTransform();
    } else npcCharacterPlaceHero(world,player,route.start,route.goal);
  if(name=="troll_barrel") {
    Vec3 hero;
    if(!characterFloor(world,Vec3(9127.44434f,128.25412f,-745.565796f),hero)) {monsterCharacterError("courtyard hero has no floor");return;}
    player.setPosition(hero);player.updateTransform();
    }
  route.npc->setPosition(route.start);route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();bench.subject=route.npc;
  bench.monsterCharacterHeroStart=player.position();
  if(route.chase)route.tolerance=route.npc->bodyObservationTarget(player).attackRangeCm+30.f;
  route.destination=WayPoint(route.goal,"FP_MONSTER_CHARACTER_GOAL");route.destination.groundPos=route.goal;
  if(name=="troll_barrel") {
    // Reuse the stock barrel mesh and production dynamic-vob constructor.
    const std::string visual="NW_HARBOUR_BARREL_01.3DS";const auto* mesh=Resources::loadMesh(visual);
    const auto* rule=MovableVobs::classifyVisual(visual);
    if(!mesh || !rule) {monsterCharacterError("stock barrel mesh/class unavailable");return;}
    const auto center=route.start+(route.goal-route.start)*.35f;const auto* bounds=mesh->bbox();
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(center.x-(bounds[0].x+bounds[1].x)*.5f,
      center.y-bounds[0].y+.2f,center.z-(bounds[0].z+bounds[1].z)*.5f);
    zenkit::VirtualObject data;data.type=zenkit::VirtualObjectType::zCVob;data.id=uint32_t(-1);
    data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
    data.visual=std::make_shared<zenkit::VisualMesh>();data.visual->type=zenkit::VisualType::MESH;data.visual->name=visual;
    const auto before=world.physic()->movableVobSnapshot();
    auto fixture=std::make_unique<Bench::MovableFixture>();fixture->visual.setVisual(data,world,false,false);fixture->visual.setObjMatrix(matrix);
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
    if(fixture->body.isEmpty()) {monsterCharacterError("stock barrel native body unavailable");return;}
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& m){view->setObjMatrix(m);});
    fixture->visual.setupFracture(world,fixture->body);
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {bench.characterPropId=body.id;break;}
    if(!bench.characterPropId) {monsterCharacterError("stock barrel snapshot unavailable");return;}
    bench.characterProp=std::move(fixture);bench.characterPropInitial=movablePosition(matrix);bench.characterPropTravel=0;
    auto& barrel=bench.monsterCharacterBarrel;barrel.role="barrel";barrel.id=bench.characterPropId;
    barrel.start=bench.characterPropInitial;barrel.lastIntact=barrel.start;
    event("monster_character_barrel","\"mass_kg\":"+ragdollNumber(bench.characterProp->body.mass())+
      ",\"position_cm\":"+ragdollVector(bench.characterPropInitial)+",\"blocks_walking\":"+(rule->blocksWalking?"true":"false"));
    if(!monsterCharacterProp(world,"crate",route.start+(route.goal-route.start)*.68f))return;
    }
  event("monster_character_route_setup","\"route\":"+jsonStr(name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"waypoint\":"+jsonStr(waypoint)+",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"arrival_radius_cm\":"+ragdollNumber(route.tolerance)+
    ",\"native_action\":"+jsonStr(route.chase?"AI_GoToNpc":"AI_GoToPoint"));
  }

void monsterCharacterBegin(World& world) {
  auto& route=bench.monsterCharacterRoute;
  if(!route.npc) {monsterCharacterError("monster begin requires fixture actor");return;}
  if(!bench.monsterCharacterCrowd)route.npc->setCharacterVirtualDiagnosticsEnabled(true);
  route.active=true;route.reached=false;route.stalled=false;route.falling=false;route.wasAirborne=false;route.began=world.tickCount();
  route.arrived=0;route.progressTime=route.began;route.stalls=0;route.falls=0;route.cvFrames=0;route.swimFrames=0;
  route.maxPhysicalJump=0;route.maxVisualJump=0;route.lastPosition=route.npc->position();
  route.lastVisual=route.npc->characterVisualPosition();route.progressPosition=route.lastPosition;route.lastSupported=route.lastPosition;
  for(auto& actor:bench.monsterCharacterExtras)if(actor.npc) {
    if(!bench.monsterCharacterCrowd)actor.npc->setCharacterVirtualDiagnosticsEnabled(true);
    actor.npc->clearAiQueue();actor.npc->clearGoTo();actor.npc->stopWalking();
    actor.start=actor.npc->position();actor.lastPosition=actor.start;actor.lastVisual=actor.npc->characterVisualPosition();
    actor.progressPosition=actor.start;actor.lastSupported=actor.start;actor.progressTime=world.tickCount();
    actor.next=0;actor.queued=false;actor.stalled=false;actor.falling=false;actor.wasAirborne=false;
    actor.arrived=0;actor.arrivals=0;actor.stalls=0;actor.falls=0;actor.cvFrames=0;actor.maxVisualJump=0;actor.maxPhysicalJump=0;actor.travel=0;
    }
  if(route.heroRun)if(auto* hero=Gothic::inst().player()) {
    hero->clearAiQueue();hero->clearGoTo();hero->setWalkMode(WalkBit::WM_Run);
    hero->aiPush(AiQueue::aiGoToPoint(route.heroDestination));
    }
  if(!route.attack && !bench.monsterCharacterPack) {
    route.npc->clearAiQueue();route.npc->stopWalking();
    if(route.isolatedLedge) {
      if(!route.npc->goToPosition(route.goal)) {monsterCharacterError("isolated ledge native running goal was rejected");return;}
      }
    else if(route.chase)route.npc->aiPush(AiQueue::aiGoToNpc(Gothic::inst().player()));
    else route.npc->aiPush(AiQueue::aiGoToPoint(route.destination));
    }
  event("monster_character_route_begin","\"route\":"+jsonStr(route.name)+",\"cv_enabled\":"+
    (Gothic::settingsGetI("GAME","monster_character_virtual")?"true":"false")+",\"start_cm\":"+ragdollVector(route.lastPosition));
  }

void monsterCharacterEnd(World& world) {
  auto& route=bench.monsterCharacterRoute;
  if(!route.active || !route.npc) {monsterCharacterError("monster end without active fixture");return;}
  monsterCharacterSampleProps(world);
  route.active=false;
  event("monster_character_route_end","\"route\":"+jsonStr(route.name)+",\"success\":"+(route.reached?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string((route.reached?route.arrived:world.tickCount())-route.began)+
    ",\"window_ms\":"+std::to_string(world.tickCount()-route.began)+",\"stalls_over_1s\":"+std::to_string(route.stalls)+
    ",\"max_physical_jump_cm\":"+ragdollNumber(route.maxPhysicalJump)+",\"max_visual_jump_cm\":"+ragdollNumber(route.maxVisualJump)+
    ",\"falls\":"+std::to_string(route.falls)+",\"cv_frames\":"+std::to_string(route.cvFrames)+",\"swim_frames\":"+std::to_string(route.swimFrames)+
    ",\"saw_swim\":"+(route.sawSwim?"true":"false")+",\"left_water\":"+(route.leftWater?"true":"false")+
    ",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+",\"end_cm\":"+ragdollVector(route.npc->position())+
    ",\"attack_distance_cm\":"+ragdollNumber(route.attackDistance)+",\"attack_rise_cm\":"+ragdollNumber(route.attackRise)+
    ",\"attack_duration_ms\":"+(route.attackEnded?std::to_string(route.attackEnded-route.began):"null")+
    ",\"charge_collision_callbacks\":"+std::to_string(route.callbacks)+",\"barrel_displacement_cm\":"+ragdollNumber(bench.characterPropTravel)+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","monster_character_virtual")?"true":"false"));
  for(auto& actor:bench.monsterCharacterExtras)if(actor.npc) {
    event("monster_character_actor_end","\"route\":"+jsonStr(route.name)+",\"role\":"+jsonStr(actor.role)+
      ",\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"instance\":"+jsonStr(symName(world,actor.npc))+
      ",\"arrivals\":"+std::to_string(actor.arrivals)+",\"elapsed_ms\":"+(actor.arrived?std::to_string(actor.arrived-route.began):"null")+
      ",\"stalls_over_1s\":"+std::to_string(actor.stalls)+",\"max_visual_jump_cm\":"+ragdollNumber(actor.maxVisualJump)+
      ",\"max_physical_jump_cm\":"+ragdollNumber(actor.maxPhysicalJump)+",\"falls\":"+std::to_string(actor.falls)+
      ",\"travel_cm\":"+ragdollNumber(actor.travel)+",\"cv_frames\":"+std::to_string(actor.cvFrames)+
      ",\"start_cm\":"+ragdollVector(actor.start)+",\"end_cm\":"+ragdollVector(actor.npc->position()));
    actor.npc->clearAiQueue();actor.npc->clearGoTo();actor.npc->stopWalking();
    }
  for(const auto& prop:bench.monsterCharacterProps)monsterCharacterPropEnd(prop,route.name);
  if(bench.monsterCharacterBarrel.id)monsterCharacterPropEnd(bench.monsterCharacterBarrel,route.name);
  route.npc->clearAiQueue();route.npc->clearGoTo();route.npc->stopWalking();
  }

// The attack needs three supported roots and one short native attack lane,
// rather than the twenty-place arena used by the performance fixture.
bool chooseMonsterAttackSite(World& world,const Npc&,const Npc&,bool withFly,Vec3& start,Vec3& goal,Vec3& flyAt) {
  // Both native attacks use the verified open lunge lane. The old short valley
  // lane can stop the fieldraider on terrain before its target collision.
  if(!characterFloor(world,Vec3(38674.5156f,4308.43604f,11976.749f),start) ||
     !characterFloor(world,start+Vec3(withFly?350.f:1200.f,0,0),goal) ||
     (withFly && !characterFloor(world,start+Vec3(150,0,-140),flyAt)))return false;
  event("monster_character_attack_site","\"site\":"+jsonStr(withFly?"verified_fieldraider_charge":"verified_wolf_lunge")+
    ",\"known_site\":true");
  return true;
  }

void monsterCharacterAttack(World& world,Npc& player,const std::string& species) {
  monsterCharacterClear(world);
  if(species!="wolf" && species!="fieldraider") {monsterCharacterError("unknown monster attack species");return;}
  auto& route=bench.monsterCharacterRoute;route.name=species+"_attack";route.attack=true;route.animation="T_FISTATTACKMOVE";
  route.npc=monsterCharacterActor(world,species=="wolf"?"Wolf":"Giant_Bug",player.position());if(!route.npc)return;
  Vec3 flyAt;
  if(!chooseMonsterAttackSite(world,*route.npc,player,species=="fieldraider",route.start,route.goal,flyAt)) {
    monsterCharacterError("monster attack requires supported roots and a clear native attack lane on outdoor city stone near the market");return;
    }
  monsterCharacterRemoveAmbient(world,player,route.start,3000,"attack");
  characterFreezeNear(world,player,route.start,route.goal);
  player.stopWalking();player.clearAiQueue();player.setPosition(route.goal);player.updateTransform();
  route.npc->setPosition(route.start);route.npc->setDirection(Vec3(1,0,0));route.npc->updateTransform();route.npc->setTarget(&player);route.npc->drawWeaponFist();
  bench.monsterCharacterHeroStart=player.position();
  if(species=="fieldraider") {
    auto* fly=monsterCharacterActor(world,"Bloodfly",flyAt);if(!fly)return;
    fly->setDirection(Vec3(1,0,0));fly->updateTransform();monsterCharacterExtra(world,fly,"bloodfly");
    const auto& guild=world.script().guildVal();const auto g=fly->guild();
    if(guild.water_depth_knee[g]!=999999 || guild.water_depth_chest[g]!=999999) {
      monsterCharacterError("bloodfly control lacks the production flying exclusion marker");return;
      }
    event("monster_character_bloodfly_setup","\"npc_id\":"+std::to_string(world.npcId(fly))+
      ",\"root_cm\":"+ragdollVector(flyAt)+",\"expected_cv_active\":false");
    }
  bench.subject=route.npc;
  event("monster_character_attack_setup","\"route\":"+jsonStr(route.name)+",\"animation\":"+jsonStr(route.animation)+
    ",\"target_cm\":"+ragdollVector(player.position())+",\"collision_required\":"+(species=="fieldraider"?"true":"false"));
  }

void monsterCharacterSnapshot(World& world,Npc& player,std::string_view label) {
  std::string actors="[";
  for(uint32_t id=0;id<world.npcCount();++id)if(const auto* npc=world.npcById(id);npc && npc!=&player && !npc->isHuman() &&
      !npc->isDown() && (npc->position()-player.position()).length()<=2300.f) {
    if(actors.size()>1)actors+=',';
    actors+="{\"npc_id\":"+std::to_string(id)+",\"instance\":"+jsonStr(symName(world,npc))+
      ",\"root_cm\":"+ragdollVector(npc->position())+",\"cv_active\":"+(npc->characterVirtualActive()?"true":"false")+"}";
    }
  event("monster_character_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","monster_character_virtual")?"true":"false")+",\"actors\":"+actors+"]");
  }

void monsterCharacterSave(World& world,Npc& player) {
  auto& route=bench.monsterCharacterRoute;
  if(!route.npc || !route.npc->characterVirtualActive()) {monsterCharacterError("monster save requires an active ON controller");return;}
  try {
    route.npc->clearAiQueue();route.npc->clearGoTo();route.npc->stopWalking();
    route.savedPosition=route.npc->position();monsterCharacterSnapshot(world,player,"saved");
    std::vector<uint8_t> session;
    {MemWriter writer(session);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
    Gothic::inst().gameSession()->save(out,"monster character",preview);}
    std::ofstream file(outDir+"/session-monster.sav",std::ios::binary);
    file.write(reinterpret_cast<const char*>(session.data()),std::streamsize(session.size()));
    if(!file)throw std::runtime_error("cannot write monster session archive");
    std::vector<uint8_t> bytes;const auto id=world.npcId(route.npc);
    {MemWriter writer(bytes);Serialize out(writer);out.setContext(&world);out.setVersion(Serialize::Current);route.npc->save(out,id,"/npc/");}
    {MemReader reader(bytes);Serialize in(reader);in.setContext(&world);in.setVersion(Serialize::Current);route.npc->load(in,id,"/npc/");route.npc->postValidate();}
    route.npc->setExternalControl(true);route.npc->updateAnimation(0,true);
    const float delta=(route.npc->position()-route.savedPosition).length();
    event("monster_character_archive","\"operation\":\"native_npc_restore\",\"position_delta_cm\":"+ragdollNumber(delta)+
      ",\"limit_cm\":0.5,\"session\":\"session-monster.sav\",\"session_bytes\":"+std::to_string(session.size())+
      ",\"npc_archive_bytes\":"+std::to_string(bytes.size()));
    if(delta>.5f) {monsterCharacterError("monster NPC archive moved more than 0.5 cm");return;}
    route.restoreSteps=2;
    } catch(const std::exception& e) {monsterCharacterError(e.what());}
  }

void monsterCharacterFields(World& world,uint64_t dt) {
  auto& route=bench.monsterCharacterRoute;
  if(route.active || bench.monsterCharacterCrowd)for(auto& actor:bench.monsterCharacterExtras)if(actor.npc) {
    auto& npc=*actor.npc;const auto at=npc.position(),visual=npc.characterVisualPosition();
    if(!bench.monsterCharacterCrowd)bench.monsterCharacterFrames.push_back({route.name+"_"+actor.role,npc.animNames(),world.tickCount(),dt,
      world.npcId(&npc),at,visual,bool(Gothic::settingsGetI("GAME","monster_character_virtual")),npc.characterVirtualActive(),
      npc.navigationGoal().active,npc.isInAir(),npc.isFalling(),npc.isSwim(),npc.rotation(),characterMoveFrame(npc)});
    actor.maxPhysicalJump=std::max(actor.maxPhysicalJump,std::abs(at.y-actor.lastPosition.y));
    actor.maxVisualJump=std::max(actor.maxVisualJump,std::abs(visual.y-actor.lastVisual.y));
    actor.travel+=std::hypot(at.x-actor.lastPosition.x,at.z-actor.lastPosition.z);
    actor.cvFrames+=npc.characterVirtualActive();actor.lastPosition=at;actor.lastVisual=visual;
    const bool air=npc.isInAir() || npc.isFalling();
    if(!npc.isSwim() && (air || actor.wasAirborne) && actor.lastSupported.y-at.y>100 && !actor.falling) {
      ++actor.falls;actor.falling=true;
      }
    if(!air) {actor.falling=false;if(!npc.isSwim())actor.lastSupported=at;}
    actor.wasAirborne=air;
    if(npc.navigationGoal().active) {
      if(std::hypot(at.x-actor.progressPosition.x,at.z-actor.progressPosition.z)>=5) {
        actor.progressPosition=at;actor.progressTime=world.tickCount();actor.stalled=false;
        }
      if(world.tickCount()-actor.progressTime>1000 && !actor.stalled) {++actor.stalls;actor.stalled=true;}
      }
    if(!actor.points.empty() && actor.next<actor.points.size()) {
      const auto& target=actor.points[actor.next];
      if(actor.queued && (at-target.pos).length()<90 && npc.isAiQueueEmpty()) {
        ++actor.arrivals;actor.arrived=world.tickCount();actor.queued=false;++actor.next;
        if(actor.loop)actor.next%=actor.points.size();
        }
      if(!actor.queued && actor.next<actor.points.size()) {
        npc.aiPush(AiQueue::aiGoToPoint(actor.points[actor.next]));actor.queued=true;
        actor.progressPosition=at;actor.progressTime=world.tickCount();actor.stalled=false;
        }
      }
    }
  if(bench.monsterCharacterMeasuring
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
     && !AppCadence::done()
#endif
     ) {
    unsigned nearby=0,virtualNearby=0,moving=0;float maximumDistance=0;
    const auto* player=Gothic::inst().player();
    for(const auto& actor:bench.monsterCharacterExtras)if(actor.npc && player) {
      const float distance=(actor.npc->position()-player->position()).length();
      nearby+=distance<2000;virtualNearby+=distance<2000 && actor.npc->characterVirtualActive();
      moving+=actor.npc->navigationGoal().active;maximumDistance=std::max(maximumDistance,distance);
      }
    bench.monsterCharacterMinNearby=std::min(bench.monsterCharacterMinNearby,nearby);
    bench.monsterCharacterMaxNearby=std::max(bench.monsterCharacterMaxNearby,nearby);
    bench.monsterCharacterMaximumDistance=std::max(bench.monsterCharacterMaximumDistance,maximumDistance);
    const auto now=std::chrono::steady_clock::now();
    const float wall=bench.monsterCharacterHasWall?std::chrono::duration<float,std::milli>(now-bench.monsterCharacterLastWall).count():0;
    bench.monsterCharacterLastWall=now;bench.monsterCharacterHasWall=true;bool normal=false;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
    normal=AppCadence::normalClock();
#endif
    bench.monsterCharacterPerformance.push_back({world.tickCount(),dt,world.physic()->physicsStats().stepMilliseconds,wall,
      maximumDistance,nearby,virtualNearby,moving,bench.rec && bench.recVideo,normal});
    }
  if(route.active)monsterCharacterSampleProps(world);
  auto* npc=route.npc;if(!npc)return;
  if(route.restoreSteps) {
    const float delta=(npc->position()-route.savedPosition).length();
    event("monster_character_archive","\"operation\":\"native_restore_step\",\"step\":"+std::to_string(3-route.restoreSteps)+
      ",\"position_delta_cm\":"+ragdollNumber(delta)+",\"limit_cm\":0.5,\"cv_active\":"+(npc->characterVirtualActive()?"true":"false"));
    --route.restoreSteps;if(delta>.5f)monsterCharacterError("monster native post-load step moved more than 0.5 cm");
    }
  if(!route.active)return;
  const auto at=npc->position(),visual=npc->characterVisualPosition();const auto animation=npc->animNames();
  bench.monsterCharacterFrames.push_back({route.name,animation,world.tickCount(),dt,world.npcId(npc),at,visual,
    bool(Gothic::settingsGetI("GAME","monster_character_virtual")),npc->characterVirtualActive(),npc->navigationGoal().active,
    npc->isInAir(),npc->isFalling(),npc->isSwim(),npc->rotation(),characterMoveFrame(*npc)});
  if(route.name=="wolf_stairs") {
    CharacterVirtualDiagnostics d;
    const bool valid=npc->characterVirtualDiagnostics(d);
    event("monster_character_stair_step","\"route\":"+jsonStr(route.name)+",\"tick\":"+std::to_string(world.tickCount())+
      ",\"dt_ms\":"+std::to_string(dt)+",\"body_state\":"+std::to_string(unsigned(npc->bodyStateMasked()))+
      ",\"attack_state\":"+(npc->bodyStateMasked()==BS_HIT?"true":"false")+
      ",\"root_cm\":"+ragdollVector(at)+",\"visual_root_cm\":"+ragdollVector(visual)+
      ",\"physical_dy_cm\":"+ragdollNumber(at.y-route.lastPosition.y)+
      ",\"visual_dy_cm\":"+ragdollNumber(visual.y-route.lastVisual.y)+
      ",\"visual_offset_cm\":"+ragdollNumber(visual.y-at.y)+",\"animation\":"+jsonStr(animation)+
      ",\"in_air\":"+(npc->isInAir()?"true":"false")+",\"diagnostics_valid\":"+(valid?"true":"false")+
      ",\"requested_cm\":"+ragdollVector(d.requested)+",\"stair_height_cm\":"+ragdollNumber(d.stairHeight)+
      ",\"cv_before_cm\":"+ragdollVector(d.before.position)+
      ",\"cv_extended_cm\":"+ragdollVector(d.afterExtendedUpdate.position)+
      ",\"cv_stairs_cm\":"+ragdollVector(d.afterStairs.position)+
      ",\"stair_attempted\":"+(d.stairAttempted?"true":"false")+
      ",\"stair_succeeded\":"+(d.stairSucceeded?"true":"false")+",\"stair_reason\":"+jsonStr(d.stairProbe.reason));
    }
  route.cvFrames+=npc->characterVirtualActive();route.swimFrames+=npc->isSwim();route.sawSwim|=npc->isSwim();
  route.maxPhysicalJump=std::max(route.maxPhysicalJump,std::abs(at.y-route.lastPosition.y));
  route.maxVisualJump=std::max(route.maxVisualJump,std::abs(visual.y-route.lastVisual.y));
  route.lastPosition=at;route.lastVisual=visual;
  const bool airborne=npc->isInAir() || npc->isFalling();
  // Short drops stay InAir; the Falling state is reserved for larger falls.
  // Test the landing before replacing its previous support height as well.
  if(!route.attack && !npc->isSwim() && (airborne || route.wasAirborne) &&
     route.lastSupported.y-at.y>100.f && !route.falling) {
    ++route.falls;route.falling=true;
    }
  if(!airborne) {
    route.falling=false;
    if(!npc->isSwim())route.lastSupported=at;
    }
  route.wasAirborne=airborne;
  bool arrived=false;
  if(route.attack) {
    if(!route.attackEnded) {
      const auto travel=at-route.attackStart;
      route.attackDistance=std::max(route.attackDistance,std::hypot(travel.x,travel.z));route.attackRise=std::max(route.attackRise,travel.y);
      if(animation.find(route.animation)==std::string::npos)route.attackEnded=world.tickCount();
      }
    arrived=route.attackEnded && (route.name!="fieldraider_attack" || route.callbacks>0);
    } else {
    const auto* hero=Gothic::inst().player();
    const auto delta=at-(route.heroRun && hero?hero->position():route.goal);
    arrived=std::hypot(delta.x,delta.z)<=route.tolerance && std::abs(delta.y)<80;
    if(route.isolatedLedge)arrived&=std::abs(delta.y)<5.f && !npc->isInAir() && !npc->isFalling();
    if(route.name=="wolf_water")arrived&=npc->isSwim();
    if(route.name=="wolf_water_exit") {route.leftWater=route.sawSwim && !npc->isSwim() && !npc->isInAir();arrived&=route.leftWater;}
    }
  if(bench.monsterCharacterPack)arrived=std::all_of(bench.monsterCharacterExtras.begin(),bench.monsterCharacterExtras.end(),
    [](const auto& actor){return actor.arrivals>=3;});
  if(arrived && !route.reached) {
    route.reached=true;route.arrived=world.tickCount();
    if(route.name=="wolf_terrain" || route.name=="wolf_stairs" || route.isolatedLedge) {
      if(route.heroRun)if(auto* hero=Gothic::inst().player()) {hero->clearAiQueue();hero->clearGoTo();hero->stopWalking();}
      // The test ends the authored chase after a measured catch. Keep native
      // idle physics running for the rest of the movie without a second goal.
      npc->clearAiQueue();npc->clearGoTo();npc->stopWalking();
      event("monster_character_native_goal_stop","\"route\":"+jsonStr(route.name)+",\"elapsed_ms\":"+
        std::to_string(route.arrived-route.began)+",\"root_cm\":"+ragdollVector(at)+",\"native_idle\":true");
      }
    }
  if(!route.attack && !route.reached && (npc->navigationGoal().active || route.isolatedLedge)) {
    if(std::hypot(at.x-route.progressPosition.x,at.z-route.progressPosition.z)>=5.f) {
      route.progressPosition=at;route.progressTime=world.tickCount();route.stalled=false;
      }
    if(world.tickCount()-route.progressTime>1000 && !route.stalled) {++route.stalls;route.stalled=true;}
    }
  }

void npcCharacterPrepare(World& world,Npc& player,const std::string& name) {
  auto& route=bench.npcCharacterRoute;
  if(route.npc) {world.removeNpc(*route.npc);route.npc=nullptr;}
  if(bench.characterItem) {world.removeItem(*bench.characterItem);bench.characterItem=nullptr;}
  route=Bench::NpcCharacterRoute();route.name=name;
  if(name=="stairs") {
    if(!prepareCharacterRoute(world,player,"stairs_up",true,"NW_CITY_TO_FOREST_STAIRS01"))return;
    } else if(name=="doorway" || name=="tavern") {
    if(!prepareCharacterRoute(world,player,name))return;
    } else if(name=="market_void") {
    // The target deliberately has no floor. Drive the ordinary AI action
    // toward it; retaining a real supported position is the expected result.
    bench.characterRoute=Bench::CharacterRoute();
    bench.characterRoute.start=Vec3(6718.370605f,270.145355f,-6029.716797f);
    bench.characterRoute.goal=Vec3(6754.f,262.f,-6115.f);
    bench.characterRoute.waypoint="market_void";
    const auto floor=world.physic()->landRay(bench.characterRoute.start+Vec3(0,20,0),80);
    if(!floor.hasCol || std::abs(floor.v.y-bench.characterRoute.start.y)>20.f) {
      arenaAbort("NPC market void start has no expected floor");return;
      }
    characterFreezeNear(world,player,bench.characterRoute.start,bench.characterRoute.goal);
    } else {arenaAbort("unknown NPC CharacterVirtual route: "+name);return;}
  route.start=bench.characterRoute.start;route.goal=bench.characterRoute.goal;
  route.destination=WayPoint(route.goal,"FP_NPC_CHARACTER_GOAL");route.destination.groundPos=route.goal;
  npcCharacterPlaceHero(world,player,route.start,route.goal);
  route.npc=npcCharacterActor(world,route.start);if(!route.npc)return;
  route.npc->setCharacterVirtualDiagnosticsEnabled(npcCharacterTrace);
  if(name=="market_void")route.npc->setDirection(-89.73f);
  else route.npc->setDirection(route.goal-route.start);
  route.npc->updateTransform();bench.subject=route.npc;
  if(name=="tavern") {
    // Use an actual dropped Item in the free lane beside authored furniture.
    // Remove only the general stand's temporary props so ON/OFF starts agree.
    bench.movableFixtures.clear();
    auto matrix=Matrix4x4::mkIdentity();matrix.translate((route.start+route.goal)*.5f);
    bench.characterItem=world.addItemDyn(world.script().findSymbolIndex("ITFO_BEER"),matrix,size_t(-1));
    if(!bench.characterItem) {arenaAbort("NPC tavern real beer mug insertion failed");return;}
    // Item proxies are centred boxes with at least 15 cm half-height. Keep
    // both the proxy and real visual bounds above the floor before settling.
    const auto* bounds=bench.characterItem->bBox();
    if(!bounds) {arenaAbort("NPC tavern beer mug has no native visual bounds");return;}
    const float height=std::max({15.f,(bounds[1].y-bounds[0].y)*.5f,-bounds[0].y})+2.f;
    matrix.translate(0,height,0);bench.characterItem->setObjMatrix(matrix);
    bench.characterItem->setPhysicsEnable(world);
    bench.characterPropInitial=bench.characterItem->position();bench.characterPropTravel=0;
    route.mugMassKg=bench.characterItem->physicsMass();route.mugContact=false;
    event("npc_character_mug_setup","\"position_cm\":"+ragdollVector(bench.characterPropInitial)+
      ",\"floor_cm\":"+ragdollVector((route.start+route.goal)*.5f)+",\"bounds_min_cm\":"+ragdollVector(bounds[0])+
      ",\"bounds_max_cm\":"+ragdollVector(bounds[1])+",\"mass_kg\":"+ragdollNumber(bench.characterItem->physicsMass()));
    }
  bench.cam=C_Follow;bench.camDist=350;bench.camHeight=110;bench.camAngle=110;bench.camAuto=true;bench.hasPrev=false;
  event("npc_character_route_setup","\"route\":"+jsonStr(name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"waypoint\":"+jsonStr(bench.characterRoute.waypoint)+",\"start_cm\":"+ragdollVector(route.start)+
    ",\"goal_cm\":"+ragdollVector(route.goal)+",\"hero_cm\":"+ragdollVector(player.position()));
  }

void npcCharacterBegin(World& world,std::string_view phase={}) {
  auto& route=bench.npcCharacterRoute;
  if(!route.npc) {arenaAbort("NPC character begin requires its fixture actor");return;}
  if(!phase.empty())route.name=phase;
  route.active=true;route.reached=false;route.arrived=0;route.began=world.tickCount();route.progressTime=route.began;
  route.lastPosition=route.npc->position();route.lastVisual=route.npc->characterVisualPosition();route.progressPosition=route.lastPosition;
  route.maxVisualJump=0;route.maxPhysicalJump=0;route.maxAboveFloor=0;route.stalls=0;route.stalled=false;
  // Route evidence includes steep-contact sliding without changing the native
  // human controller. Diagnostics remain transient and owned by this fixture.
  route.supportHistory.clear();route.npc->setCharacterVirtualDiagnosticsEnabled(true);
  bench.npcCharacterHasWall=false;
  event("npc_character_route_begin","\"route\":"+jsonStr(route.name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"start_cm\":"+ragdollVector(route.npc->position())+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
  }

void npcCharacterEnd(World& world) {
  auto& route=bench.npcCharacterRoute;
  if(!route.active || !route.npc) {arenaAbort("NPC character end without active fixture");return;}
  route.active=false;
  std::string mug;
  if(bench.characterItem)
    mug=",\"mug_mass_kg\":"+ragdollNumber(route.mugMassKg)+
      ",\"mug_max_displacement_cm\":"+ragdollNumber(bench.characterPropTravel)+
      ",\"mug_native_contact\":"+(route.mugContact?"true":"false");
  event("npc_character_route_end","\"route\":"+jsonStr(route.name)+",\"success\":"+(route.reached?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string((route.reached?route.arrived:world.tickCount())-route.began)+
    ",\"window_ms\":"+std::to_string(world.tickCount()-route.began)+",\"start_cm\":"+ragdollVector(route.start)+
    ",\"goal_cm\":"+ragdollVector(route.goal)+",\"end_cm\":"+ragdollVector(route.npc->position())+
    ",\"stalls\":"+std::to_string(route.stalls)+",\"max_visual_jump_cm\":"+ragdollNumber(route.maxVisualJump)+
    ",\"max_physical_jump_cm\":"+ragdollNumber(route.maxPhysicalJump)+",\"max_above_floor_cm\":"+ragdollNumber(route.maxAboveFloor)+
    ",\"sat\":"+(route.sat?"true":"false")+",\"standing\":"+(route.standing?"true":"false")+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false")+mug);
  }

void characterStairPerformance(World& world,Npc& player) {
  if(!Gothic::physicsFlags().characterVirtual || !Gothic::settingsGetI("GAME","npc_character_virtual")) {
    arenaAbort("stair performance requires both human CharacterVirtual flags on");return;
    }
  std::vector<const Npc*> sources{&player};
  for(uint32_t id=0;id<world.npcCount() && sources.size()<11;++id) {
    const auto* npc=world.npcById(id);
    if(npc && npc!=&player && npc->isHuman() && !npc->isDown() && npc->visualSkeleton() &&
       npc->navigationCapsule().x>0.f && npc->navigationCapsule().y>0.f)
      sources.push_back(npc);
    }
  if(sources.size()!=11 || !player.visualSkeleton()) {
    arenaAbort("stair performance needs one loaded hero and ten loaded live human skeletons");return;
    }
  constexpr float radians=float(M_PI)/180.f, angle=55.f*radians, padding=1.f;
  const float cosine=std::cos(angle), tangent=std::tan(angle);
  zenkit::Mesh plane;
  plane.vertices={{-2000,-2000*tangent,-2000},{-2000,-2000*tangent,2000},
                  {2000,2000*tangent,2000},{2000,2000*tangent,-2000}};
  plane.materials.resize(1);plane.materials[0].group=zenkit::MaterialGroup::STONE;
  // PhysicVbo reverses Gothic source winding into upward-facing native triangles.
  plane.polygons.vertex_indices={0,2,1,0,3,2};plane.polygons.material_indices={0,0};
  DynamicWorld isolated(nullptr,plane);
  struct Actor {
    DynamicWorld::NpcItem body;
    const Npc* source=nullptr;
    float step=0,slope=0,mass=0;
    };
  std::vector<Actor> actors;actors.reserve(11);
  const auto& guild=world.script().guildVal();
  for(size_t index=0;index<sources.size();++index) {
    const auto* source=sources[index];
    auto body=isolated.addGhostObj(source->visualSkeleton());
    const auto capsule=source->navigationCapsule();
    if(std::abs(body.capsuleRadius()-capsule.x)>.01f || std::abs(body.capsuleHeight()-capsule.y)>.01f) {
      arenaAbort("stair performance human skeleton must retain its actual landscape capsule dimensions");return;
      }
    const float step=std::max(float(guild.step_height[source->guild()]),0.f);
    const float slope=float(guild.slide_angle[source->guild()])*radians;
    if(!(slope>0.f && slope<angle && std::isfinite(slope))) {
      arenaAbort("stair performance requires every human to treat 55 degrees as non-walkable");return;
      }
    const Vec3 initial(0,(capsule.x+padding)/cosine-capsule.x-padding,(float(index)-5.f)*150.f);
    body.setPosition(initial);
    const float mass=source->physicsMass(), stepHeight=step>0.f?step:50.f;
    const auto state=body.moveVirtual({},0,stepHeight,slope,mass);
    if(!state.steep || (state.position-initial).length()>.001f) {
      arenaAbort("stair performance must acquire all eleven analytic steep supports without settling");return;
      }
    actors.push_back({std::move(body),source,stepHeight,slope,mass});
    }
  // Flush only zero-dt contact preparation from the production timer. No
  // positive-dt warmup or measured steps are discarded.
  isolated.tick(0);
  struct Sample { uint64_t frame=0,time=0,dt=0;float physics=0,wall=0;unsigned steep=0,grounded=0; };
  std::vector<Sample> samples;samples.reserve(600);
  uint64_t elapsed=0;
  unsigned minimumSteep=11;
  for(uint64_t frame=0;frame<600;++frame) {
    const uint64_t dt=frame%3==0?16:17;
    const Vec3 request(4.f*float(dt)/17.f,0,0);
    unsigned steep=0,grounded=0;
    const auto began=std::chrono::steady_clock::now();
    // All eleven controllers coexist. Direct wrapper requests keep stressing
    // the uphill stair sweeps even though normal MoveAlgo would enter Slide.
    for(auto& actor:actors) {
      const auto state=actor.body.moveVirtual(request,dt,actor.step,actor.slope,actor.mass);
      steep+=state.steep;grounded+=state.grounded;
      }
    isolated.tick(dt);
    const float wall=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-began).count();
    elapsed+=dt;
    samples.push_back({frame,elapsed,dt,isolated.physicsStats().stepMilliseconds,wall,steep,grounded});
    minimumSteep=std::min(minimumSteep,steep);
    }
  auto* file=std::fopen((outDir+"/character-stair-performance.csv").c_str(),"w");
  if(!file) {arenaAbort("cannot write stair performance samples");return;}
  std::fprintf(file,"frame,t_sim_ms,dt_ms,actors,uphill_requests,steep_actors,grounded_actors,step_ms,batch_wall_ms\n");
  std::vector<float> physics,wall;physics.reserve(samples.size());wall.reserve(samples.size());
  for(const auto& sample:samples) {
    std::fprintf(file,"%llu,%llu,%llu,11,11,%u,%u,%.9g,%.9g\n",(unsigned long long)sample.frame,
                 (unsigned long long)sample.time,(unsigned long long)sample.dt,sample.steep,sample.grounded,
                 double(sample.physics),double(sample.wall));
    physics.push_back(sample.physics);wall.push_back(sample.wall);
    }
  std::fclose(file);std::sort(physics.begin(),physics.end());std::sort(wall.begin(),wall.end());
  const auto p95=size_t(std::ceil(.95*double(samples.size())))-1;
  event("character_stair_performance","\"actors\":11,\"hero\":1,\"npcs\":10,\"samples\":"+std::to_string(samples.size())+
    ",\"simulated_ms\":"+std::to_string(elapsed)+",\"angle_deg\":55,\"minimum_steep_actors\":"+std::to_string(minimumSteep)+
    ",\"p95_step_ms\":"+ragdollNumber(physics[p95])+",\"p95_batch_wall_ms\":"+ragdollNumber(wall[p95])+
    ",\"timing_diagnostics\":false,\"source\":\"eleven independent human NpcItem controllers in isolated DynamicWorld\"");
  // Enable tracing only after formal timing. One extra uphill step per actor
  // proves the measured configuration without charging diagnostics to p95.
  for(size_t index=0;index<actors.size();++index) {
    auto& actor=actors[index];actor.body.setVirtualDiagnosticsEnabled(true);
    const auto state=actor.body.moveVirtual({4,0,0},17,actor.step,actor.slope,actor.mass);
    CharacterVirtualDiagnostics diagnostic;
    if(!actor.body.virtualDiagnostics(diagnostic)) {arenaAbort("stair performance coverage diagnostics are unavailable");return;}
    event("character_stair_performance_actor","\"actor\":"+std::to_string(index)+",\"role\":"+jsonStr(index?"npc":"hero")+
      ",\"source_npc_id\":"+std::to_string(world.npcId(actor.source))+",\"source_npc\":"+jsonStr(symName(world,actor.source))+
      ",\"radius_cm\":"+ragdollNumber(actor.body.capsuleRadius())+",\"height_cm\":"+ragdollNumber(actor.body.capsuleHeight())+
      ",\"mass_kg\":"+ragdollNumber(actor.mass)+",\"step_height_cm\":"+ragdollNumber(actor.step)+
      ",\"max_slope_rad\":"+ragdollNumber(actor.slope)+",\"root_cm\":"+ragdollVector(state.position)+
      ",\"ground_normal\":"+ragdollVector(state.groundNormal)+",\"ground_state\":"+std::to_string(diagnostic.afterStairs.groundState)+
      ",\"steep\":"+(state.steep?"true":"false")+",\"stair_attempted\":"+(diagnostic.stairAttempted?"true":"false")+
      ",\"stair_succeeded\":"+(diagnostic.stairSucceeded?"true":"false")+",\"reason\":"+jsonStr(diagnostic.stairProbe.reason));
    }
  if(minimumSteep!=11 || elapsed!=10000)arenaAbort("stair performance lost its eleven continuous steep-slope actors or ten-second window");
  }

void npcCharacterMarket(World& world,Npc& player) {
  const WayPoint* center=world.findPoint("NW_CITY_MERCHANT_PATH_20");
  if(!center) {arenaAbort("NPC character market fixture has no stock market waypoint");return;}
  if(bench.npcCharacterRoute.npc) {world.removeNpc(*bench.npcCharacterRoute.npc);bench.npcCharacterRoute=Bench::NpcCharacterRoute();}
  std::vector<Npc*> actors;
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* npc=world.npcById(id);
    if(npc && npc!=&player && npc->isHuman() && !npc->isDown() && (npc->position()-center->position()).length()<2000.f)
      actors.push_back(npc);
    }
  characterFreezeNear(world,player,center->position(),center->position());
  // Keep precisely twenty live humans inside the measured radius even when
  // the source save has a larger native market population.
  for(size_t i=20;i<actors.size();++i) {
    actors[i]->setPosition(center->position()+Vec3(3000.f+float(i-20)*100.f,0,0));actors[i]->updateTransform();
    }
  std::vector<std::pair<Vec3,Vec3>> lanes;
  for(float radius:{200.f,400.f,600.f,800.f,1000.f,1200.f,1400.f})for(unsigned step=0;step<24 && lanes.size()<20;++step) {
    const float angle=float(step)*float(M_PI)/12.f;
    Vec3 start,end;
    if(!characterFloor(world,center->position()+Vec3(std::cos(angle)*radius,0,std::sin(angle)*radius),start) ||
       std::abs(start.y-center->position().y)>100)continue;
    if(std::any_of(lanes.begin(),lanes.end(),[&](const auto& lane){return (start-lane.first).length()<120.f;}))continue;
    bool found=false;
    for(unsigned direction=0;direction<8;++direction) {
      const float yaw=float(direction)*float(M_PI)/4.f;
      const Vec3 along(std::cos(yaw),0,std::sin(yaw));
      if(characterFloor(world,start+along*280.f,end) && characterLane(world,start,end,20.f) && std::abs(end.y-start.y)<45 &&
         std::none_of(lanes.begin(),lanes.end(),[&](const auto& lane){return (end-lane.first).length()<100.f || (end-lane.second).length()<100.f;})) {found=true;break;}
      }
    if(found)lanes.emplace_back(start,end);
    }
  if(lanes.size()!=20) {arenaAbort("market fixture lacks twenty supported walking lanes");return;}
  player.stopWalking();player.setPosition(center->position());player.updateTransform();
  bench.npcCharacterCrowd.clear();
  for(size_t i=0;i<lanes.size();++i) {
    auto* npc=i<actors.size()?actors[i]:npcCharacterActor(world,lanes[i].first);
    if(!npc)return;
    npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
    npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Run);
    npc->setPosition(lanes[i].first);npc->setDirection(lanes[i].second-lanes[i].first);npc->updateTransform();
    bench.npcCharacterCrowd.emplace_back();auto& walker=bench.npcCharacterCrowd.back();walker.npc=npc;
    walker.ends[0]=WayPoint(lanes[i].second,"FP_NPC_CHARACTER_MARKET_B");walker.ends[1]=WayPoint(lanes[i].first,"FP_NPC_CHARACTER_MARKET_A");
    for(auto& point:walker.ends)point.groundPos=point.pos;
    }
  bench.subject=nullptr;bench.cam=C_World;bench.camDist=1000;bench.camHeight=800;bench.camAngle=45;bench.camAuto=false;
  event("npc_character_market_setup","\"actors\":20,\"reused_npcs\":"+std::to_string(std::min(size_t(20),actors.size()))+
    ",\"waypoint\":"+jsonStr(center->name)+",\"hero_cm\":"+ragdollVector(player.position()));
  }

void npcCharacterFields(World& world,Npc& player,uint64_t dt) {
  auto& route=bench.npcCharacterRoute;
  if(route.npc && !route.active && route.seat) {
    const auto at=route.npc->position();
    const bool supported=!route.npc->isInAir() && !route.npc->isFalling() && std::abs(at.y-route.start.y)<5.f;
    if(!supported || !route.sawPreparationSample || (at-route.preparationSample).length()>1.f)route.supportSince=world.tickCount();
    route.preparationSample=at;route.sawPreparationSample=true;
    }
  if(!route.active && !bench.npcCharacterMeasuring && !bench.npcCharacterObserving)return;
  unsigned nearby=0,virtualNearby=0,movingNearby=0,movingVirtualNearby=0;
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* npc=world.npcById(id);
    if(!npc || npc==&player || !npc->isHuman() || npc->isDown())continue;
    const bool near=(npc->position()-player.position()).quadLength()<2000.f*2000.f;
    nearby+=near;virtualNearby+=near && npc->characterVirtualActive();
    const auto goal=npc->navigationGoal();
    movingNearby+=near && goal.active;movingVirtualNearby+=near && goal.active && npc->characterVirtualActive();
    if(!bench.npcCharacterObserving || (npc->position()-player.position()).length()>3000.f)continue;
    if(npc->characterVirtualActive())++bench.npcCharacterCvFrames;
    auto& prior=bench.npcCharacterGoals[npc];
    const bool same=goal.active && prior.active && prior.kind==goal.kind && prior.name==goal.name && prior.npc==goal.npc &&
      (goal.npc!=nullptr || (goal.position-prior.target).length()<10.f);
    if(prior.active && !same) {
      const auto delta=npc->position()-prior.target;
      if(delta.x*delta.x+delta.z*delta.z<=150.f*150.f && std::abs(delta.y)<=200.f) {
        ++bench.npcCharacterArrivals;
        event("npc_character_goal_arrive","\"window\":"+jsonStr(bench.npcCharacterWindow)+",\"npc_id\":"+std::to_string(id)+
          ",\"goal\":"+jsonStr(prior.name)+",\"goal_cm\":"+ragdollVector(prior.target)+",\"root_cm\":"+ragdollVector(npc->position()));
        }
      }
    prior.active=goal.active;
    if(goal.active) {prior.target=goal.position;prior.npc=goal.npc;prior.kind=goal.kind;prior.name=goal.name;bench.npcCharacterWalkers.insert(npc);}
    }
  if(bench.npcCharacterObserving) {++bench.npcCharacterObservedFrames;bench.npcCharacterMaxNearby=std::max(bench.npcCharacterMaxNearby,nearby);}
  if(!route.active && !bench.npcCharacterMeasuring)return;
  if(bench.npcCharacterMeasuring && !bench.npcCharacterCrowd.empty()) {
    unsigned close=0;float maximumDistance=0;
    for(const auto& walker:bench.npcCharacterCrowd) {
      const float distance=(walker.npc->position()-player.position()).length();
      close+=distance<2000.f;maximumDistance=std::max(maximumDistance,distance);
      }
    bench.npcCharacterCrowdMinNearby=std::min(bench.npcCharacterCrowdMinNearby,close);
    bench.npcCharacterCrowdMaxNearby=std::max(bench.npcCharacterCrowdMaxNearby,close);
    bench.npcCharacterCrowdMaximumDistance=std::max(bench.npcCharacterCrowdMaximumDistance,maximumDistance);
    }
  const auto now=std::chrono::steady_clock::now();
  const float wall=bench.npcCharacterHasWall?std::chrono::duration<float,std::milli>(now-bench.npcCharacterLastWall).count():0;
  bench.npcCharacterLastWall=now;bench.npcCharacterHasWall=true;
  bool normal=false;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  normal=AppCadence::normalClock();
#endif
  const auto* npc=route.active?route.npc:nullptr;
  bench.npcCharacterFrames.push_back({bench.npcCharacterMeasuring?"performance":route.name,
    uint64_t(bench.npcCharacterFrames.size()),world.tickCount(),dt,npc?uint64_t(world.npcId(npc)):uint64_t(-1),
    npc?npc->position():player.position(),npc?npc->characterVisualPosition():player.characterVisualPosition(),
    bool(Gothic::settingsGetI("GAME","npc_character_virtual")),npc && npc->characterVirtualActive(),
    npc && npc->navigationGoal().active,npc && npc->isInAir(),npc && npc->interactive(),bench.rec && bench.recVideo,normal,
    world.physic()->physicsStats().stepMilliseconds,wall,nearby,virtualNearby,movingNearby,movingVirtualNearby,
    npc?characterMoveFrame(*npc):Bench::CharacterMoveFrame()});
  if(!route.active || !npc)return;
  const auto position=npc->position(),visual=npc->characterVisualPosition();
  npcCharacterSupportTrace(world,*npc,position.y-route.lastPosition.y,visual.y-route.lastVisual.y);
  route.maxVisualJump=std::max(route.maxVisualJump,std::abs(visual.y-route.lastVisual.y));
  route.maxPhysicalJump=std::max(route.maxPhysicalJump,std::abs(position.y-route.lastPosition.y));
  route.lastVisual=visual;route.lastPosition=position;
  if(const auto floor=world.physic()->landRay(position+Vec3(0,20,0),600);floor.hasCol)
    route.maxAboveFloor=std::max(route.maxAboveFloor,position.y-floor.v.y);
  if(bench.characterItem) {
    bench.characterPropTravel=std::max(bench.characterPropTravel,(bench.characterItem->position()-bench.characterPropInitial).length());
    route.mugContact|=bench.characterItem->physicsTouchedByNpc();
    route.mugMassKg=std::max(route.mugMassKg,bench.characterItem->physicsMass());
    }
  bool arrived=false;
  if(route.seat) {
    if(npc->interactive()!=nullptr && npc->interactive()!=route.seat) {arenaAbort("NPC native AI_UseMob selected a different BENCH");return;}
    route.sat|=npc->interactive()==route.seat && route.seat->stateId()>=1;
    route.standing=route.sat && npc->interactive()==nullptr && npc->isStanding();
    arrived=route.standing;
    } else {
    const auto delta=position-route.goal;
    arrived=delta.x*delta.x+delta.z*delta.z<=70.f*70.f && std::abs(delta.y)<60;
    }
  if(!route.reached && arrived) {
    route.reached=true;route.arrived=world.tickCount();
    event("npc_character_route_arrive","\"route\":"+jsonStr(route.name)+",\"elapsed_ms\":"+std::to_string(route.arrived-route.began)+
      ",\"root_cm\":"+ragdollVector(position));
    }
  if(!route.reached && !route.seat && npc->navigationGoal().active) {
    if((position-route.progressPosition).length()>=10.f) {route.progressPosition=position;route.progressTime=world.tickCount();route.stalled=false;}
    if(world.tickCount()-route.progressTime>=2000 && !route.stalled) {
      ++route.stalls;route.stalled=true;
      event("npc_character_route_stall","\"route\":"+jsonStr(route.name)+",\"root_cm\":"+ragdollVector(position));
      }
    }
  }

bool prepareCharacterRideSupport(World& world,Npc& player,bool kinematic,bool rotating,Vec3& top) {
  // The former fixture moved a tavern table along global X, unrelated to the
  // inspected free walking lane. A support test needs clearance for the whole
  // moving hull and the standing actor, including the elevated kinematic top.
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && point.name.starts_with("NW_CITY_"))points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  Vec3 floor,along;std::string waypoint;
  for(const auto* point:points) {
    Vec3 center;if(!characterFloor(world,point->position(),center))continue;
    for(unsigned step=0;step<8 && waypoint.empty();++step) {
      const float yaw=float(step)*float(M_PI)/4.f;
      const Vec3 direction(std::cos(yaw),0,std::sin(yaw)),side(-direction.z,0,direction.x);
      bool clear=true;
      for(float offset:{-120.f,-60.f,0.f,60.f,120.f}) {
        Vec3 previous;
        for(int distance=-120;distance<=480 && clear;distance+=60) {
          Vec3 ground;
          if(!characterFloor(world,center+direction*float(distance)+side*offset,ground) ||
             std::abs(ground.y-center.y)>3.f) {clear=false;break;}
          if(world.physic()->ray(ground+Vec3(0,5,0),ground+Vec3(0,450,0)).hasCol) {clear=false;break;}
          if(distance>-120)
            for(float height:{10.f,60.f,120.f,200.f,300.f,420.f})
              if(world.physic()->ray(previous+Vec3(0,height,0),ground+Vec3(0,height,0)).hasCol)clear=false;
          previous=ground;
          }
        if(!clear)break;
        }
      if(!clear)continue;
      for(uint32_t id=0;id<world.npcCount();++id) {
        const auto* npc=world.npcById(id);if(!npc || npc==&player)continue;
        const auto delta=npc->position()-(center+direction*180.f);
        if(std::abs(Vec3::dotProduct(delta,direction))<420.f && std::abs(Vec3::dotProduct(delta,side))<180.f &&
           std::abs(delta.y)<300.f) {clear=false;break;}
        }
      if(clear) {floor=center;along=direction;waypoint=point->name;}
      }
    if(!waypoint.empty())break;
    }
  if(waypoint.empty()) {arenaAbort("character support requires a clear level 6m city lane and 4.5m headroom");return false;}
  std::string visual;
  for(const auto& row:world.physic()->movableVobSnapshot())if(row.className=="table" && row.mass>=10.f) {visual=row.visual;break;}
  const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
  if(!mesh || !rule) {arenaAbort("character support stock table unavailable");return false;}
  const auto* bounds=mesh->bbox();
  bench.characterRidePivot=(bounds[0]+bounds[1])*.5f;bench.characterRidePivot.y=0;
  auto matrix=Matrix4x4::mkIdentity();
  matrix.translate(floor.x-(bounds[0].x+bounds[1].x)*.5f,floor.y-bounds[0].y+.2f+(kinematic?150.f:0.f),
                   floor.z-(bounds[0].z+bounds[1].z)*.5f);
  top=(bounds[0]+bounds[1])*.5f;top.y=bounds[1].y;
  if(rotating) {
    // A noncentral point proves angular carry; the actor remains inside the
    // real tabletop rather than standing over the rotation axis.
    if(bounds[1].x-bounds[0].x>=bounds[1].z-bounds[0].z)top.x+=std::min(40.f,(bounds[1].x-bounds[0].x)*.2f);
    else top.z+=std::min(40.f,(bounds[1].z-bounds[0].z)*.2f);
    }
  matrix.project(top);
  zenkit::VirtualObject data;data.type=zenkit::VirtualObjectType::zCVob;data.id=uint32_t(-1);
  data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
  data.visual=std::make_shared<zenkit::VisualMesh>();data.visual->type=zenkit::VisualType::MESH;data.visual->name=visual;
  if(kinematic) {
    bench.characterPlatformVisual=std::make_unique<ObjVisual>();bench.characterPlatformVisual->setVisual(data,world,false,false);
    bench.characterPlatform=std::make_unique<PhysicMesh>(*mesh,*world.physic(),true);
    bench.characterPlatformMatrix=matrix;bench.characterPlatform->setObjMatrix(matrix);bench.characterPlatformVisual->setObjMatrix(matrix);
    }
  else {
    const auto before=world.physic()->movableVobSnapshot();
    auto fixture=std::make_unique<Bench::MovableFixture>();fixture->visual.setVisual(data,world,false,false);fixture->visual.setObjMatrix(matrix);
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
    if(fixture->body.isEmpty() || fixture->body.mass()<10.f) {arenaAbort("character dynamic support requires at least 10kg");return false;}
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& pose){view->setObjMatrix(pose);});
    bench.characterProp=std::move(fixture);
    for(const auto& row:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==row.id;}))bench.characterPropId=row.id;
    }
  bench.characterRideDirection=along;
  event("character_support_site","\"waypoint\":"+jsonStr(waypoint)+",\"floor_cm\":"+ragdollVector(floor)+
    ",\"direction\":"+ragdollVector(along)+",\"visual\":"+jsonStr(visual)+
    ",\"bounds_min_cm\":"+ragdollVector(bounds[0])+",\"bounds_max_cm\":"+ragdollVector(bounds[1]));
  return true;
  }

void exec(const std::string& line, World& w, Npc& pl, PlayerControl& ctrl) {
  const auto a = split(line);
  if(a.empty())
    return;
  const std::string& c = a[0];

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
    if(!Resources::isHeadless()) throw std::runtime_error("navdemos is runner-only");
    bench.navigationOnly = true;
    NavDemos::command(a,w,pl,outDir);
    return;
  }
  if(c=="navmesh") {
    bench.navigationOnly = true;
    if(a.size()!=2 || a[1]!="wait") NavBench::command(a,w,pl,outDir);
    }
  else if(c=="monstercharacter" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","monster_character_virtual",a[1]=="on");
    event("monster_character_toggle","\"enabled\":"+std::string(a[1]=="on"?"true":"false"));
    }
  else if(c=="monstercharacter" && a.size()==3 && a[1]=="route")monsterCharacterPrepare(w,pl,a[2]);
  else if(c=="monstercharacter" && (a.size()==4 || a.size()==5) && a[1]=="ledge")
    monsterCharacterLedge(w,pl,a[2],num(a,3,NAN),a.size()==5?num(a,4,NAN):0.f);
  else if(c=="monstercharacter" && a.size()>=2 && a[1]=="camera") {
    if((a.size()!=2 && a.size()!=4) || (a.size()==4 && (!isNumber(a[2]) || !isNumber(a[3])))) {
      monsterCharacterError("monster camera expects no arguments or yaw_degrees elevation_fraction");return;
      }
    monsterCharacterCamera(w,pl,a.size()==4,a.size()==4?num(a,2,NAN):0,a.size()==4?num(a,3,NAN):.42f);
    }
  else if(c=="monstercharacter" && a.size()==5 && a[1]=="follow") {
    auto& route=bench.monsterCharacterRoute;
    auto* watched=route.npc;
    if(bench.monsterCharacterPack && bench.monsterCharacterExtras.size()>=2)watched=bench.monsterCharacterExtras[1].npc;
    if(!watched) {monsterCharacterError("follow requires prepared monster");return;}
    exec("watch id:"+std::to_string(w.npcId(watched)),w,pl,ctrl);
    exec("cam follow "+a[2]+" "+a[3]+" "+a[4],w,pl,ctrl);
    exec("cam look "+ragdollNumber(std::min(130.f,std::max(45.f,(watched->bounds().bbox[1].y-watched->bounds().bbox[0].y)*.45f))),w,pl,ctrl);
    exec(route.name=="troll_barrel"?"view clean 500":"view clean 300",w,pl,ctrl);
    updateCamera(w,*watched);
    std::string actors="[{\"npc_id\":"+std::to_string(w.npcId(route.npc))+",\"role\":\"primary\",\"start_cm\":"+ragdollVector(route.start)+"}";
    for(const auto& actor:bench.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
      actors+=",{\"npc_id\":"+std::to_string(w.npcId(actor.npc))+",\"role\":"+jsonStr(actor.role)+",\"start_cm\":"+ragdollVector(actor.start)+"}";
    event("monster_character_camera","\"route\":"+jsonStr(route.name)+",\"mode\":\"follow\",\"fixed\":false,\"watch_npc_id\":"+std::to_string(w.npcId(watched))+
      ",\"distance_cm\":"+ragdollNumber(bench.camDist)+",\"height_cm\":"+ragdollNumber(bench.camHeight)+",\"angle_deg\":"+ragdollNumber(bench.camAngle)+
      ",\"look_height_cm\":"+ragdollNumber(bench.camLook)+",\"seed\":"+std::to_string(bench.monsterCharacterSeed)+",\"hour\":"+std::to_string(w.time().hour())+
      ",\"minute\":"+std::to_string(w.time().minute())+",\"hero_cm\":"+ragdollVector(bench.monsterCharacterHeroStart)+",\"actors\":"+actors+"]");
    monsterCharacterRenderProof(w,*watched,"followed");
    }
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="pack")monsterCharacterPack(w,pl);
  else if(c=="monstercharacter" && a.size()==2 && (a[1]=="cliff" || a[1]=="cliff46"))monsterCharacterCliff(w,pl,a[1]=="cliff46");
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="crowd")monsterCharacterCrowd(w,pl);
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="monstercharacter" && a.size()==3 && a[1]=="measure") {
    if(!bench.monsterCharacterCrowd || !AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {
      monsterCharacterError("monster normal cadence arm requires the twenty-monster fixture");return;
      }
    bench.monsterCharacterMeasuring=true;bench.monsterCharacterHasWall=false;bench.monsterCharacterPerformance.clear();
    bench.monsterCharacterMinNearby=20;bench.monsterCharacterMaxNearby=0;bench.monsterCharacterMaximumDistance=0;
    AppCadence::counts(true,uint32_t(bench.monsterCharacterExtras.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("monster_character_measure_begin","\"cv_enabled\":"+std::string(Gothic::settingsGetI("GAME","monster_character_virtual")?"true":"false"));
    }
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="report") {
    bench.monsterCharacterMeasuring=false;
    AppCadence::counts(false,uint32_t(bench.monsterCharacterExtras.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("monster_character_measure_end","\"frames\":"+std::to_string(bench.monsterCharacterPerformance.size())+
      ",\"fixture_actors\":"+std::to_string(bench.monsterCharacterExtras.size())+",\"fixture_min_nearby\":"+std::to_string(bench.monsterCharacterMinNearby)+
      ",\"fixture_max_nearby\":"+std::to_string(bench.monsterCharacterMaxNearby)+",\"fixture_max_distance_cm\":"+ragdollNumber(bench.monsterCharacterMaximumDistance));
    }
#endif
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="begin")monsterCharacterBegin(w);
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="end")monsterCharacterEnd(w);
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="reverse") {
    auto& route=bench.monsterCharacterRoute;
    if(!route.npc || route.name!="wolf_water" || !route.sawSwim) {monsterCharacterError("water exit requires the same wolf after actual swimming entry");return;}
    route.goal=route.start;route.start=route.npc->position();route.name="wolf_water_exit";
    route.destination=WayPoint(route.goal,"FP_MONSTER_CHARACTER_DRY");route.destination.groundPos=route.goal;
    route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();
    event("monster_character_water_reverse","\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal));
    }
  else if(c=="monstercharacter" && a.size()==3 && a[1]=="attack")monsterCharacterAttack(w,pl,a[2]);
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="stair-attack") {
    auto& route=bench.monsterCharacterRoute;
    if(!route.npc || !route.active || route.name!="wolf_stairs") {
      monsterCharacterError("stair-attack requires the active stair chase");return;
      }
    const float pending=route.npc->characterVisualPosition().y-route.npc->position().y;
    route.npc->clearAiQueue();route.npc->clearGoTo();route.npc->stopWalking();
    const auto* attack=route.npc->playAnimByName("T_FISTATTACKMOVE",BS_HIT);
    event("monster_character_stair_attack","\"pending_offset_cm\":"+ragdollNumber(pending)+
      ",\"accepted\":"+(attack?"true":"false"));
    if(!attack)monsterCharacterError("stair handoff lacks the wolf lunge animation");
    }
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="attack-start") {
    auto& route=bench.monsterCharacterRoute;
    if(!route.npc || !route.attack) {monsterCharacterError("attack-start requires an authored monster attack fixture");return;}
    route.npc->clearAiQueue();
    const Animation::Sequence* sequence=nullptr;
    if(route.name=="fieldraider_attack") {
      // Giant_Bug's layer-2 attack does not animate root 0. Prepare the
      // native S_FISTRUNL layer-1 clip before the ordinary attack entry point;
      // both translation and collision remain owned by production movement.
      const auto before=route.npc->animNames();
      event("monster_character_attack_preparation","\"route\":"+jsonStr(route.name)+",\"before\":"+jsonStr(before)+
        ",\"root_cm\":"+ragdollVector(route.npc->position())+",\"target_cm\":"+ragdollVector(pl.position())+
        ",\"target_distance_cm\":"+ragdollNumber((route.npc->position()-pl.position()).length())+
        ",\"native_stop_cm\":"+ragdollNumber(route.npc->bodyObservationTarget(pl).attackRangeCm));
      const auto* running=route.npc->playAnimByName("S_FISTRUNL",BS_RUN);
      event("monster_character_attack_base","\"accepted\":"+std::string(running?"true":"false")+
        ",\"layers\":"+jsonStr(route.npc->animNames()));
      if(!running) {monsterCharacterError("fieldraider native running base unavailable");return;}
      sequence=route.npc->visualSkeleton()->sequence(route.animation);
      const bool accepted=sequence && route.npc->doAttack(Npc::Anim::Attack,BS_HIT);
      const auto after=route.npc->animNames();
      event("monster_character_attack_running_base","\"accepted\":"+std::string(accepted?"true":"false")+
        ",\"before\":"+jsonStr(before)+",\"after\":"+jsonStr(after)+
        ",\"target_distance_cm\":"+ragdollNumber((route.npc->position()-pl.position()).length()));
      if(!accepted || after.find(route.animation)==std::string::npos) {
        monsterCharacterError("native fieldraider running attack was not accepted");return;
        }
      } else {
      route.npc->clearGoTo();route.npc->stopWalking();
      sequence=route.npc->playAnimByName(route.animation,BS_HIT);
      if(!sequence) {monsterCharacterError("monster has no authored "+route.animation);return;}
      }
    route.attackStart=route.npc->position();
    route.attackEnded=0;route.attackDistance=0;route.attackRise=0;route.callbacks=0;
    monsterCharacterBegin(w);
    event("monster_character_attack_begin","\"route\":"+jsonStr(route.name)+",\"animation\":"+jsonStr(route.animation)+
      ",\"authored_duration_ms\":"+ragdollNumber(sequence->totalTime())+",\"root_cm\":"+ragdollVector(route.attackStart));
    }
  else if(c=="monstercharacter" && a.size()==3 && a[1]=="distance") {
    const auto* npc=bench.monsterCharacterRoute.npc;
    if(!npc) {monsterCharacterError("monster distance requires fixture actor");return;}
    pl.stopWalking();pl.clearAiQueue();pl.setPosition(npc->centerPosition()+Vec3(num(a,2,400),0,0)-(pl.centerPosition()-pl.position()));pl.updateTransform();
    event("monster_character_distance","\"distance_cm\":"+ragdollNumber((npc->centerPosition()-pl.centerPosition()).length()));
    }
  else if(c=="monstercharacter" && a.size()==4 && a[1]=="expect") {
    const auto* npc=bench.monsterCharacterRoute.npc;
    if(!npc) {monsterCharacterError("monster expectation requires fixture actor");return;}
    const bool actual=npc->characterVirtualActive(),expected=num(a,3,0)!=0;
    event("monster_character_expect","\"label\":"+jsonStr(a[2])+",\"npc_id\":"+std::to_string(w.npcId(npc))+
      ",\"instance\":"+jsonStr(symName(w,npc))+",\"expected_cv_active\":"+(expected?"true":"false")+
      ",\"cv_active\":"+(actual?"true":"false")+",\"passed\":"+(actual==expected?"true":"false")+
      ",\"distance_cm\":"+ragdollNumber((npc->centerPosition()-pl.centerPosition()).length()));
    if(actual!=expected)monsterCharacterError("monster CV activity disagrees with "+a[2]);
    }
  else if(c=="monstercharacter" && a.size()==3 && a[1]=="flying") {
    monsterCharacterClear(w);
    if(!chooseRagdollArena(w,pl)) {monsterCharacterError("flying exclusion requires supported stock ground");return;}
    auto& route=bench.monsterCharacterRoute;route.name="flying_"+a[2];route.start=bench.ragdollArenaPlaces.front();
    characterFreezeNear(w,pl,route.start,route.start);npcCharacterPlaceHero(w,pl,route.start,route.start+Vec3(1,0,0));
    route.npc=monsterCharacterActor(w,a[2],route.start);if(!route.npc)return;bench.subject=route.npc;
    const auto& guild=w.script().guildVal();const auto g=route.npc->guild();
    const bool flying=guild.water_depth_knee[g]==999999 && guild.water_depth_chest[g]==999999;
    event("monster_character_flying_setup","\"instance\":"+jsonStr(a[2])+",\"can_fly_over_water\":"+(flying?"true":"false")+
      ",\"root_cm\":"+ragdollVector(route.start));
    if(!flying)monsterCharacterError("flying fixture instance lacks production flying guild marker");
    }
  else if(c=="monstercharacter" && a.size()==2 && a[1]=="save")monsterCharacterSave(w,pl);
  else if(c=="monstercharacter" && a.size()==3 && a[1]=="snapshot")monsterCharacterSnapshot(w,pl,a[2]);
  else if(c=="npccharacter" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","npc_character_virtual",a[1]=="on");
    event("npc_character_toggle","\"enabled\":"+std::string(a[1]=="on"?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()>=3 && a[1]=="trace") {
    if(a.size()==3 && a[2]=="off") {
      const auto count=bench.npcCharacterTraceActors.size();npcCharacterStopMotionTrace(&w);
      event("npc_character_trace_end","\"actors\":"+std::to_string(count));
      return;
      }
    std::vector<Npc*> selected;
    for(size_t i=2;i<a.size();++i) {
      const bool numeric=!a[i].empty() && std::all_of(a[i].begin(),a[i].end(),[](char ch){return ch>='0' && ch<='9';});
      auto* npc=findNpc(w,pl,numeric?"id:"+a[i]:a[i]);
      if(!npc) {arenaAbort("NPC character trace has no existing actor: "+a[i]);return;}
      if(std::find(selected.begin(),selected.end(),npc)==selected.end())selected.push_back(npc);
      }
    npcCharacterStopMotionTrace(&w);
    std::string actors="[";
    for(auto* npc:selected) {
      npc->setCharacterVirtualDiagnosticsEnabled(true);
      bench.npcCharacterTraceActors.push_back({npc,npc->position(),npc->characterVisualPosition(),false});
      if(actors.size()>1)actors+=',';
      actors+="{\"npc_id\":"+std::to_string(w.npcId(npc))+",\"npc\":"+jsonStr(symName(w,npc))+
        ",\"root_cm\":"+ragdollVector(npc->position())+"}";
      }
    event("npc_character_trace_begin","\"actors\":"+actors+"]");
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="route") {
    npcCharacterPrepare(w,pl,a[2]);
    if(bench.npcCharacterRoute.npc)exec("watch watched",w,pl,ctrl);
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="begin") {
    auto& route=bench.npcCharacterRoute;
    if(!route.npc) {arenaAbort("NPC character begin needs a prepared route");return;}
    if(bench.characterItem) {
      const auto position=bench.characterItem->position(),velocity=bench.characterItem->physicsVelocity();
      auto along=route.goal-route.start;along.y=0;along=Vec3::normalize(along);
      const auto delta=position-(route.start+route.goal)*.5f;
      const float side=std::abs(delta.x*along.z-delta.z*along.x),forward=std::abs(Vec3::dotProduct(delta,along));
      const bool ready=side<45.f && forward<60.f && velocity.length()<5.f && delta.y>=0 && delta.y<60.f;
      event("npc_character_mug_prebegin","\"position_cm\":"+ragdollVector(position)+",\"velocity_cm_s\":"+ragdollVector(velocity)+
        ",\"settling_displacement_cm\":"+ragdollNumber((position-bench.characterPropInitial).length())+
        ",\"lane_side_cm\":"+ragdollNumber(side)+",\"lane_forward_cm\":"+ragdollNumber(forward)+",\"ready\":"+(ready?"true":"false"));
      if(!ready) {arenaAbort("NPC tavern mug did not settle inside its route lane");return;}
      bench.characterPropInitial=position;bench.characterPropTravel=0;
      }
    route.npc->clearAiQueue();route.npc->stopWalking();route.npc->setPosition(route.start);route.npc->updateTransform();
    npcCharacterBegin(w);
    route.npc->aiPush(AiQueue::aiGoToPoint(route.destination));route.npc->aiPush(AiQueue::aiWait(60000));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="end") npcCharacterEnd(w);
  else if(c=="npccharacter" && (a.size()==2 || a.size()==3) && a[1]=="seat") {
    auto& route=bench.npcCharacterRoute;
    if(route.npc) {w.removeNpc(*route.npc);route.npc=nullptr;}
    if(bench.characterItem) {w.removeItem(*bench.characterItem);bench.characterItem=nullptr;}
    route=Bench::NpcCharacterRoute();route.name="seat";
    const WayPoint* requestedAnchor=a.size()==3?w.findPoint(a[2],false):nullptr;
    if(a.size()==3 && !requestedAnchor) {arenaAbort("NPC character seat requested an unknown exact waypoint");return;}
    struct Candidate {Interactive* mob=nullptr;const WayPoint* waypoint=nullptr;uint32_t id=0;unsigned region=0;float distance=0;};
    std::vector<Candidate> candidates;
    Npc* coragon=nullptr;
    for(uint32_t id=0;id<w.npcCount();++id)
      if(auto* npc=w.npcById(id);npc && symName(w,npc)=="VLK_420_CORAGON") {coragon=npc;break;}
    for(uint32_t id=0;;++id) {
      auto* mob=w.mobsiById(id);if(!mob)break;
      if(mob->schemeName()!="BENCH")continue;
      const auto* waypoint=w.findWayPoint(mob->position());
      if(requestedAnchor && (!waypoint || waypoint->name!=requestedAnchor->name))continue;
      unsigned region=4;
      if(waypoint) {
        const auto& name=waypoint->name;
        const bool harbour=name.starts_with("NW_CITY_") &&
          (name.find("HABOUR")!=std::string::npos || name.find("HARBOUR")!=std::string::npos || name.find("HARBOR")!=std::string::npos);
        region=harbour?0:(name.starts_with("NW_BIGFARM") || name.starts_with("NW_FARM")?1:name.starts_with("NW_CITY_")?3:2);
        }
      candidates.push_back({mob,waypoint,id,region,waypoint?(waypoint->position()-mob->position()).length():1e10f});
      }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b) {
      if(a.region!=b.region)return a.region<b.region;
      if(a.distance!=b.distance)return a.distance<b.distance;
      return a.id<b.id;
      });
    // An explicit waypoint pins one authored BENCH by geometry. Occupancy or
    // failed native preconditions must not silently select a different seat.
    if(requestedAnchor && candidates.size()>1)candidates.resize(1);
    event("npc_character_seat_candidates","\"count\":"+std::to_string(candidates.size())+
      ",\"requested_waypoint\":"+jsonStr(requestedAnchor?requestedAnchor->name:std::string()));
    Vec3 socket,camera,focus;std::string anchorName;bool selected=false;
    uint32_t selectedId=uint32_t(-1);
    for(const auto& candidate:candidates) {
      auto* mob=candidate.mob;
      const auto candidateEvent=[&](std::string_view reason) {
        event("npc_character_seat_candidate","\"mob_id\":"+std::to_string(candidate.id)+",\"scheme\":"+jsonStr(mob->schemeName())+
          ",\"vob\":"+jsonStr(mob->vobNameStr())+",\"mob_cm\":"+ragdollVector(mob->position())+
          ",\"waypoint\":"+jsonStr(candidate.waypoint?candidate.waypoint->name:std::string())+
          ",\"available\":"+(mob->isAvailable()?"true":"false")+",\"state\":"+std::to_string(mob->stateId())+
          ",\"reason\":"+jsonStr(reason));
        };
      if(!mob->isAvailable() || mob->stateId()>0) {candidateEvent("occupied_or_active");continue;}
      if(coragon && (mob->position()-coragon->position()).length()<1500.f) {candidateEvent("coragon_bar_excluded");continue;}
      if(!candidate.waypoint || candidate.distance>1000.f) {candidateEvent("no_nearby_authored_waypoint");continue;}
      const auto reference=w.physic()->landRay(candidate.waypoint->position()+Vec3(0,80,0),200.f);
      if(!reference.hasCol) {candidateEvent("waypoint_has_no_supported_floor");continue;}
      const float floorReference=reference.v.y;
      if(std::abs(mob->position().y-floorReference)>200.f) {candidateEvent("floor_in_different_storey");continue;}
      if(!route.npc)route.npc=npcCharacterActor(w,reference.v);
      if(!route.npc)return;
      route.npc->setPosition(reference.v);route.npc->updateTransform();
      socket=mob->nearestPoint(*route.npc);
      bool prepared=false;
      // A native animation socket is not an approach point. Find ordinary
      // supported floor for a real GoToPoint, within UseMob's 165 cm range.
      for(float distance:{240.f,280.f,320.f})for(unsigned step=0;step<24 && !prepared;++step) {
        const float yaw=float(step)*float(M_PI)/12.f;
        auto at=socket+Vec3(std::cos(yaw)*distance,0,std::sin(yaw)*distance);
        const auto floor=w.physic()->landRay(Vec3(at.x,floorReference+50.f,at.z),150.f);
        if(!floor.hasCol || std::abs(floor.v.y-floorReference)>20.f)continue;
        at.y=floor.v.y;route.npc->setPosition(at);route.npc->updateTransform();
        if(route.npc->hasCollision() || w.availableMob(*route.npc,"BENCH")!=mob)continue;
        auto toward=socket-at;toward.y=0;toward=Vec3::normalize(toward);
        const auto approachEnd=socket-toward*120.f;
        auto previous=at;bool lane=true;
        for(unsigned sample=1;sample<=12;++sample) {
          auto point=at+(approachEnd-at)*(float(sample)/12.f);
          const auto support=w.physic()->landRay(Vec3(point.x,floorReference+50.f,point.z),150.f);
          if(!support.hasCol || std::abs(support.v.y-at.y)>20.f) {lane=false;break;}
          point.y=support.v.y;
          if(w.physic()->ray(previous+Vec3(0,100,0),point+Vec3(0,100,0)).hasCol) {lane=false;break;}
          previous=point;
          }
        if(!lane)continue;
        route.npc->setPosition(previous);route.npc->updateTransform();
        const auto usePosition=mob->nearestPoint(*route.npc);
        if(route.npc->hasCollision() || (route.npc->centerPosition()-usePosition).length()>140.f ||
           w.availableMob(*route.npc,"BENCH")!=mob)continue;
        route.destination=WayPoint(previous,"FP_NPC_CHARACTER_SEAT_APPROACH");route.destination.groundPos=previous;
        route.start=at;prepared=true;
        }
      if(!prepared) {candidateEvent("no_supported_collision_free_approach");continue;}
      // Check one fixed camera against both the approach and the actual
      // seated/standing socket area, rather than hiding room geometry.
      focus=(route.start+socket)*.5f;focus.y=floorReference+130.f;
      const Vec3 targets[]{route.start+Vec3(0,10,0),route.start+Vec3(0,130,0),
                           route.destination.position()+Vec3(0,10,0),route.destination.position()+Vec3(0,130,0),
                           Vec3(socket.x,floorReference+50.f,socket.z),socket+Vec3(0,60,0),socket+Vec3(0,150,0)};
      bool visible=false;
      for(float distance:{450.f,550.f,350.f})for(float height:{170.f,140.f,220.f})for(unsigned step=0;step<48 && !visible;++step) {
        const float yaw=float(step)*float(M_PI)/24.f;
        const Vec3 at(focus.x+std::cos(yaw)*distance,floorReference+height,focus.z+std::sin(yaw)*distance);
        bool clear=true;
        for(const auto& target:targets)if(hidden(w,at,target)) {clear=false;break;}
        if(clear) {camera=at;visible=true;}
        }
      if(!visible) {candidateEvent("no_clear_fixed_view");continue;}
      route.seat=mob;route.goal=socket;anchorName=candidate.waypoint->name;selectedId=candidate.id;
      selected=true;candidateEvent("selected");break;
      }
    if(!selected) {arenaAbort("no free authored BENCH has a supported native approach and clear fixed view");return;}
    characterFreezeNear(w,pl,route.start,route.goal);
    route.npc->stopWalking();route.npc->setPosition(route.start);route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();
    route.npc->setExternalControl(true);route.npc->aiPush(AiQueue::aiWait(60000));
    route.preparationSample=route.start;route.sawPreparationSample=true;route.supportSince=w.tickCount();
    bench.subject=route.npc;npcCharacterPlaceHero(w,pl,route.start,route.goal);
    bench.cam=C_Hold;bench.camAuto=false;lookFrom(camera,focus);
    event("npc_character_seat_setup","\"scheme\":"+jsonStr(route.seat->schemeName())+",\"vob\":"+jsonStr(route.seat->vobNameStr())+
      ",\"mob_id\":"+std::to_string(selectedId)+",\"anchor\":"+jsonStr(anchorName)+",\"mob_cm\":"+ragdollVector(route.seat->position())+
      ",\"socket_cm\":"+ragdollVector(socket)+",\"start_cm\":"+ragdollVector(route.start)+
      ",\"approach_cm\":"+ragdollVector(route.destination.position())+",\"camera_cm\":"+ragdollVector(camera)+
      ",\"camera_target_cm\":"+ragdollVector(focus)+",\"camera_clear\":true");
    exec("watch watched",w,pl,ctrl);
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="sit") {
    auto& route=bench.npcCharacterRoute;
    if(!route.npc || !route.seat) {arenaAbort("NPC character sit needs its BENCH fixture");return;}
    const bool stable=route.sawPreparationSample && w.tickCount()-route.supportSince>=300 &&
      !route.npc->isInAir() && !route.npc->isFalling() && (route.npc->position()-route.start).length()<5.f;
    event("npc_character_seat_prebegin","\"ready\":"+std::string(stable?"true":"false")+
      ",\"root_cm\":"+ragdollVector(route.npc->position())+",\"supported_ms\":"+std::to_string(w.tickCount()-route.supportSince)+
      ",\"selected_vob\":"+jsonStr(route.seat->vobNameStr()));
    if(!stable) {arenaAbort("NPC character seat approach did not settle supported before sit");return;}
    npcCharacterBegin(w);
    route.npc->clearAiQueue();
    // Scripted routines normally approach a grounded waypoint before UseMob.
    // A direct far UseMob chases the elevated animation socket to a 20 cm
    // position-goal, which can send the legacy actor onto adjacent furniture.
    route.npc->aiPush(AiQueue::aiGoToPoint(route.destination));
    route.npc->aiPush(AiQueue::aiUseMob(route.seat->schemeName(),1));route.npc->aiPush(AiQueue::aiWait(60000));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="stand") {
    auto& route=bench.npcCharacterRoute;
    if(!route.npc || !route.seat || !route.sat) {arenaAbort("NPC character did not reach real sitting state before stand");return;}
    route.npc->clearAiQueue();route.npc->aiPush(AiQueue::aiUseMob(route.seat->schemeName(),-1));route.npc->aiPush(AiQueue::aiWait(60000));
    event("npc_character_stand","\"root_cm\":"+ragdollVector(route.npc->position()));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="market") npcCharacterMarket(w,pl);
  else if(c=="npccharacter" && a.size()>=3 && a[1]=="observe" && a[2]=="begin") {
    bench.npcCharacterObserving=true;bench.npcCharacterWindow=a.size()>3?a[3]:"window";
    bench.npcCharacterGoals.clear();bench.npcCharacterWalkers.clear();bench.npcCharacterArrivals=0;
    bench.npcCharacterObservedFrames=0;bench.npcCharacterCvFrames=0;bench.npcCharacterMaxNearby=0;bench.npcCharacterObserveStart=w.tickCount();
    event("npc_character_observe_begin","\"window\":"+jsonStr(bench.npcCharacterWindow)+",\"radius_cm\":3000,\"humans_only\":true,\"cv_enabled\":"+
      (Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="observe" && a[2]=="end") {
    bench.npcCharacterObserving=false;
    event("npc_character_observe_end","\"window\":"+jsonStr(bench.npcCharacterWindow)+",\"window_ms\":"+std::to_string(w.tickCount()-bench.npcCharacterObserveStart)+
      ",\"radius_cm\":3000,\"humans_only\":true,\"unique_walkers\":"+std::to_string(bench.npcCharacterWalkers.size())+
      ",\"reached_goals\":"+std::to_string(bench.npcCharacterArrivals)+",\"frames\":"+std::to_string(bench.npcCharacterObservedFrames)+
      ",\"cv_actor_frames\":"+std::to_string(bench.npcCharacterCvFrames)+",\"max_nearby_humans\":"+std::to_string(bench.npcCharacterMaxNearby)+
      ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="snapshot") npcCharacterSnapshot(w,pl,a[2]);
  else if(c=="npccharacter" && a.size()==3 && a[1]=="distance") {
    const auto* npc=bench.npcCharacterRoute.npc;
    if(!npc) {arenaAbort("NPC character distance needs a fixture actor");return;}
    pl.setPosition(npc->centerPosition()+Vec3(num(a,2,400),0,0)-(pl.centerPosition()-pl.position()));pl.updateTransform();
    event("npc_character_distance","\"distance_cm\":"+ragdollNumber((npc->centerPosition()-pl.centerPosition()).length())+
      ",\"npc_cm\":"+ragdollVector(npc->position()));
    }
  else if(c=="npccharacter" && a.size()==2 && (a[1]=="teleport" || a[1]=="setposition")) {
    auto& route=bench.npcCharacterRoute;
    if(!route.npc) {arenaAbort("NPC character relocation needs its fixture actor");return;}
    route.npc->setInteraction(nullptr,true);route.npc->clearAiQueue();route.npc->stopWalking();
    const auto target=a[1]=="teleport"?route.start:route.goal;
    if(a[1]=="teleport") {
      route.destination=WayPoint(target,"FP_NPC_CHARACTER_TELEPORT");route.destination.groundPos=target;
      route.npc->aiPush(AiQueue::aiTeleport(route.destination));
      } else {route.npc->setPosition(target);route.npc->updateTransform();}
    route.npc->aiPush(AiQueue::aiWait(60000));
    event("npc_character_relocate","\"kind\":"+jsonStr(a[1])+",\"target_cm\":"+ragdollVector(target));
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="save") {
    if(a[2].empty() || a[2].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
      arenaAbort("invalid NPC character save label");return;
      }
    try {
      npcCharacterSnapshot(w,pl,"saved");std::vector<uint8_t> bytes;
      {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
      Gothic::inst().gameSession()->save(out,"npc character "+a[2],preview);}
      std::ofstream file(outDir+"/session-"+a[2]+".sav",std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
      if(!file)throw std::runtime_error("cannot write complete NPC CharacterVirtual session save");
      event("npc_character_save","\"archive\":"+jsonStr("session-"+a[2]+".sav")+",\"bytes\":"+std::to_string(bytes.size()));
      } catch(const std::exception& error) {arenaAbort(error.what());}
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="npccharacter" && a.size()==3 && a[1]=="measure") {
    if(!AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {arenaAbort("NPC character normal cadence arm failed");return;}
    bench.npcCharacterMeasuring=true;bench.npcCharacterHasWall=false;
    bench.npcCharacterCrowdMinNearby=20;bench.npcCharacterCrowdMaxNearby=0;bench.npcCharacterCrowdMaximumDistance=0;
    AppCadence::counts(true,uint32_t(bench.npcCharacterCrowd.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("npc_character_measure_begin","\"cv_enabled\":"+std::string(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="report") {
    bench.npcCharacterMeasuring=false;
    AppCadence::counts(false,uint32_t(bench.npcCharacterCrowd.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("npc_character_measure_end","\"frames\":"+std::to_string(bench.npcCharacterFrames.size())+
      ",\"fixture_actors\":"+std::to_string(bench.npcCharacterCrowd.size())+",\"fixture_min_nearby\":"+std::to_string(bench.npcCharacterCrowdMinNearby)+
      ",\"fixture_max_nearby\":"+std::to_string(bench.npcCharacterCrowdMaxNearby)+",\"fixture_max_distance_cm\":"+ragdollNumber(bench.npcCharacterCrowdMaximumDistance));
    }
#endif
  else if(c=="character" && (a.size()==3 || a.size()==4) && a[1]=="idle" && (a[2]=="hero" || a[2]=="npc"))
    characterIdlePrepare(w,pl,ctrl,a[2],a.size()==4?a[3]:"gentle");
  else if(c=="character" && a.size()==3 && a[1]=="idle" && a[2]=="begin") characterIdleSnapshot(w,"begin",true);
  else if(c=="character" && a.size()==4 && a[1]=="idle" && a[2]=="snapshot") characterIdleSnapshot(w,a[3]);
  else if(c=="character" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","character_virtual",a[1]=="on");
    event("character_toggle","\"enabled\":"+std::string(a[1]=="on"?"true":"false"));
    }
  else if(c=="character" && a.size()==2 && a[1]=="probe") {
    size_t prepared=0;
    for(const std::string route:{"stairs_up","stairs_down","steep_slope","port_edge","doorway","tavern","jump_ledge","water_entry"})
      prepared+=prepareCharacterRoute(w,pl,route,false);
    event("character_probe_complete","\"prepared\":"+std::to_string(prepared)+",\"expected\":8");
    }
  else if(c=="character" && a.size()==3 && a[1]=="route") prepareCharacterRoute(w,pl,a[2]);
  else if(c=="character" && a.size()==2 && a[1]=="stair-performance") characterStairPerformance(w,pl);
  else if(c=="character" && a.size()==4 && a[1]=="idle-slope") {
    if(a[3]!="hero" && a[3]!="npc") {arenaAbort("character idle-slope expects hero or npc");return;}
    characterIdleSlopePrepare(w,pl,num(a,2,0),a[3]=="npc");
    }
  else if(c=="character" && a.size()==3 && a[1]=="idle-slope-sample") characterIdleSlopeSample(w,pl,a[2]);
  else if(c=="character" && a.size()==6 && a[1]=="approach") {
    auto& route=bench.characterRoute;
    if(!route.prepared || route.active) {arenaAbort("character approach needs a prepared route");return;}
    const Vec3 side(-route.direction.z,0,route.direction.x);
    const auto shifted=route.start+side*num(a,3,0)+route.direction*num(a,4,0);
    Vec3 floor;
    if(!characterFloor(w,shifted,floor)) {arenaAbort("character approach has no floor");return;}
    route.start=floor;route.name+='_'+a[2];
    pl.setPosition(route.start);pl.setDirection(route.direction);
    const float yaw=(pl.rotation()+num(a,5,0))*float(M_PI)/180.f;
    pl.setDirection(Vec3(std::cos(yaw),0,std::sin(yaw)));pl.updateTransform();
    event("character_approach","\"route\":"+jsonStr(route.name)+",\"start_cm\":"+ragdollVector(route.start)+
      ",\"goal_cm\":"+ragdollVector(route.goal)+",\"yaw_deg\":"+ragdollNumber(pl.rotation()));
    }
  else if(c=="character" && a.size()==2 && a[1]=="begin") {
    auto& route=bench.characterRoute;
    if(!route.prepared || route.active) {arenaAbort("character begin needs a prepared route");return;}
    route.active=true;route.reached=false;route.sawSwim=false;route.sawSlide=false;route.fallCount=0;route.falling=false;
    // Idle settling can already slide on the steep route. Start held input
    // from the same prepared coordinate in both modes after the camera settles.
    pl.setPosition(route.start);pl.updateTransform();
    route.began=w.tickCount();route.lastSupported=route.start;
    bench.characterHasWall=false;
    event("character_route_begin","\"route\":"+jsonStr(route.name)+",\"start_cm\":"+ragdollVector(route.start)+
      ",\"visual_start_cm\":"+ragdollVector(pl.characterVisualPosition())+
      ",\"goal_cm\":"+ragdollVector(route.goal)+",\"tolerance_cm\":"+ragdollNumber(route.tolerance)+
      ",\"cv_enabled\":"+(Gothic::physicsFlags().characterVirtual?"true":"false"));
    }
  else if(c=="character" && a.size()==2 && a[1]=="end") characterRouteEnd(w,pl);
  else if(c=="character" && a.size()==3 && a[1]=="snapshot") characterSnapshot(w,pl,a[2]);
  else if(c=="character" && a.size()==3 && a[1]=="save") {
    if(a[2].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
      arenaAbort("invalid character save label");return;
      }
    try {
      characterSnapshot(w,pl,"saved");
      std::vector<uint8_t> bytes;
      {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
      Gothic::inst().gameSession()->save(out,"character "+a[2],preview);}
      std::ofstream file(outDir+"/session-"+a[2]+".sav",std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
      if(!file)throw std::runtime_error("cannot write complete CharacterVirtual save");
      event("character_save","\"archive\":"+jsonStr("session-"+a[2]+".sav")+",\"bytes\":"+std::to_string(bytes.size()));
      } catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="character" && a.size()==3 && a[1]=="prop") prepareCharacterProp(w,pl,a[2]);
  else if(c=="character" && a.size()==2 && a[1]=="prop-checkpoint") characterPropCheckpoint(w,pl);
  else if(c=="character" && a.size()==2 && a[1]=="cleanup") {
    if(bench.characterItem)w.removeItem(*bench.characterItem);
    bench.characterItem=nullptr;bench.characterProp.reset();bench.characterPropId=0;bench.characterPropRole.clear();
    bench.characterPlatform.reset();bench.characterPlatformVisual.reset();bench.characterMover=nullptr;
    if(bench.characterRideNpc) {
      if(bench.subject==bench.characterRideNpc)bench.subject=nullptr;
      w.removeNpc(*bench.characterRideNpc);bench.characterRideNpc=nullptr;
      }
    bench.characterRiding=false;bench.characterRideMoving=false;bench.characterRideRotating=false;
    }
  else if(c=="character" && (a.size()==3 || a.size()==4) && a[1]=="ride") {
    if(a.size()==4 && a[3]!="npc") {arenaAbort("character ride rider must be npc");return;}
    if(bench.characterRideNpc || bench.characterPlatform || bench.characterProp) {arenaAbort("character ride needs cleanup before another support");return;}
    bench.characterRiding=false;bench.characterRideMoving=false;bench.characterMover=nullptr;
    bench.characterRideRotating=a[2]=="rotation";
    bench.characterRideDirection=Vec3(1,0,0);
    bench.characterRideAngle=0;bench.characterRideTravel=0;
    bench.characterRideMaximumDrift=0;bench.characterRideMaximumHorizontalDrift=0;bench.characterRideMaximumLocalDrift=0;
    Vec3 top;
    if(a[2]=="mover") {
      std::vector<MoveTrigger*> movers;
      w.detectMover([&](MoveTrigger& mover){movers.push_back(&mover);});
      std::sort(movers.begin(),movers.end(),[](const auto* a,const auto* b){return a->name()<b->name();});
      bool selected=false;
      for(auto* mover:movers) {
        const auto* mesh=Resources::loadMesh(mover->visualNameView());
        if(!mesh || mover->keyFrameCount()<2)continue;
        const auto* bounds=mesh->bbox();
        auto localTop=(bounds[0]+bounds[1])*.5f;localTop.y=bounds[1].y;
        auto supported=localTop;mover->transform().project(supported);
        const auto ground=w.physic()->landRay(supported+Vec3(0,25,0),60);
        bool eligible=bounds[1].x-bounds[0].x>=100 && bounds[1].z-bounds[0].z>=100 &&
          ground.hasCol && std::abs(ground.v.y-supported.y)<15;
        // The legacy query deliberately clears C_Object normals. Compare five
        // actual surface heights instead of treating that zero normal as slope.
        for(const auto delta:{Vec3(-35,0,0),Vec3(35,0,0),Vec3(0,0,-35),Vec3(0,0,35)}) {
          const auto corner=w.physic()->landRay(supported+delta+Vec3(0,25,0),60);
          eligible&=corner.hasCol && std::abs(corner.v.y-supported.y)<15;
          }
        event("character_mover_candidate","\"name\":"+jsonStr(mover->name())+",\"visual\":"+jsonStr(mover->visualNameView())+
          ",\"frames\":"+std::to_string(mover->keyFrameCount())+",\"standable\":"+(eligible?"true":"false")+
          ",\"root_cm\":"+ragdollVector(mover->position())+",\"top_cm\":"+ragdollVector(supported));
        if(eligible && !selected) {bench.characterMover=mover;top=ground.v;selected=true;}
        }
      if(!selected) {event("character_mover_unavailable","\"movers\":"+std::to_string(movers.size())+
        ",\"reason\":\"no mover has an exposed supported horizontal top at least 1m wide\"");return;}
      }
    else if(a[2]=="dynamic" || a[2]=="kinematic" || a[2]=="rotation") {
      if(!prepareCharacterRideSupport(w,pl,a[2]!="dynamic",bench.characterRideRotating,top))return;
      }
    else {arenaAbort("unknown character ride support: "+a[2]);return;}
    characterFreezeNear(w,pl,top,top+bench.characterRideDirection*400.f);
    if(a.size()==4) {
      npcCharacterPlaceHero(w,pl,top,top+bench.characterRideDirection*400.f);
      bench.characterRideNpc=npcCharacterActor(w,top+Vec3(0,1,0));if(!bench.characterRideNpc)return;
      bench.subject=bench.characterRideNpc;
      }
    else {pl.stopWalking();pl.clearAiQueue();pl.setPosition(top+Vec3(0,1,0));pl.updateTransform();}
    const auto& rider=bench.characterRideNpc?*bench.characterRideNpc:pl;
    bench.characterRideMatrixStart=characterSupportMatrix(w);bench.characterRideStart=movablePosition(bench.characterRideMatrixStart);
    auto inverse=bench.characterRideMatrixStart;inverse.inverse();bench.characterRideLocalStart=rider.position();inverse.project(bench.characterRideLocalStart);
    bench.characterRideBegan=w.tickCount();bench.characterPropRole.clear();
    bench.characterRideHeroStart=pl.position();bench.characterHasWall=false;bench.characterRiding=true;
    characterRideCheckpoint(w,pl,true);
    }
  else if(c=="character" && a.size()==2 && a[1]=="ride-start") {
    if(!bench.characterRiding)return; // explicit unavailable mover event already identifies missing coverage
    // The setup already selects Idle through stopWalking(). Animation blending
    // and native contact recovery can still settle during its 700ms wait; begin
    // the carry measurement from the actual pose immediately before movement.
    const auto& rider=bench.characterRideNpc?*bench.characterRideNpc:pl;
    bench.characterRideMatrixStart=characterSupportMatrix(w);bench.characterRideStart=movablePosition(bench.characterRideMatrixStart);
    auto inverse=bench.characterRideMatrixStart;inverse.inverse();bench.characterRideLocalStart=rider.position();inverse.project(bench.characterRideLocalStart);
    bench.characterRideBegan=w.tickCount();
    bench.characterRideMaximumDrift=0;bench.characterRideMaximumHorizontalDrift=0;bench.characterRideMaximumLocalDrift=0;
    bench.characterRideHeroStart=pl.position();
    characterRideCheckpoint(w,pl);
    bench.characterRideMoving=true;
    if(bench.characterMover)w.triggerEvent(TriggerEvent(std::string(bench.characterMover->name()),"CHARACTER_BENCH",TriggerEvent::T_Trigger));
    else if(bench.characterProp) {
      const auto impulse=bench.characterRideDirection*(5.f*bench.characterProp->body.mass());
      const bool accepted=bench.characterProp->body.addImpulse(impulse,&pl);
      event("character_ride_impulse","\"mass_kg\":"+ragdollNumber(bench.characterProp->body.mass())+
        ",\"impulse_ns\":"+ragdollVector(impulse)+",\"accepted\":"+(accepted?"true":"false")+
        ",\"velocity_cm_s\":"+ragdollVector(bench.characterProp->body.velocity()));
      }
    }
  else if(c=="character" && a.size()==2 && a[1]=="ride-checkpoint") {
    if(bench.characterRiding)characterRideCheckpoint(w,pl);
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="character" && a.size()==3 && a[1]=="measure") {
    if(!AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {arenaAbort("character normal cadence arm failed");return;}
    bench.characterMeasuring=true;bench.characterHasWall=false;bench.characterMeasureStart=w.tickCount();
    AppCadence::counts(true,1,0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("character_measure_begin","\"cv_enabled\":"+std::string(Gothic::physicsFlags().characterVirtual?"true":"false"));
    }
  else if(c=="character" && a.size()==2 && a[1]=="report") {
    bench.characterMeasuring=false;
    AppCadence::counts(false,1,0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("character_measure_end","\"frames\":"+std::to_string(bench.characterFrames.size()));
    }
#endif
  else if(Resources::isHeadless() && c=="cheat" && a.size()==2 && a[1]=="god") {
    // Marvin also draws a notification with a GPU font. The CPU runner needs the same game flag.
    Gothic::inst().setGodMode(!Gothic::inst().isGodMode());
    say("cheat god");
    }
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
      {"movable_vobs",&Gothic::PhysicsFlags::movableVobs},
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
  else if(c=="destruction_traces" && a.size()>=2) {
    std::string command=a[1],value,error;
    for(size_t i=2;i<a.size();++i)command+=' '+a[i];
    if(!TestBench::destructionTracesDebug(w,pl,command,value,error)) {arenaAbort(error);return;}
    event("destruction_traces_debug","\"command\":"+jsonStr(command)+",\"sample\":"+value);
    }
  else if(c=="destruction" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","destruction",a[1]=="on");
    w.physic()->setDestructionEnabled(a[1]=="on");
    }
  else if(c=="destruction" && a.size()==3 && a[1]=="setup") destructionSetup(w,pl,a[2]);
  else if(c=="destruction" && a.size()==2 && a[1]=="begin") destructionBegin(w);
  else if(c=="destruction" && a.size()==2 && a[1]=="checkpoint") destructionCheckpoint(w);
  else if(c=="destruction" && a.size()==3 && a[1]=="support" && (a[2]=="on" || a[2]=="off")) {
    bench.destructionSupportTrace=a[2]=="on";
    bench.destructionSupportNext=0;
    }
  else if(c=="destruction" && a.size()==3 && a[1]=="support" && a[2]=="sample") destructionSupportSample(w);
  else if(c=="destruction2" && (a.size()==4 || a.size()==5) && a[1]=="setup")
    destructionMeleeSetup(w,pl,a[2],a[3],a.size()==5 && a[4]=="npc");
  else if(c=="destruction2" && (a.size()==2 || a.size()==3) && a[1]=="attack") {
    if(!bench.destructionMeleeActor || bench.destructionIds.empty()) {arenaAbort("destruction melee setup required");return;}
    // `attack <n>`: n blows and no more, whether the prop broke or not (a take with destruction off)
    bench.destructionMeleeBlows=a.size()==3 ? unsigned(num(a,2,0)) : 0;
    bench.destructionMeleeRunning=true;bench.destructionMeleeNext=w.tickCount();
    bench.destructionMeleeDeadline=w.tickCount()+90000;bench.waitUntil=bench.time+90034;
    }
  else if(c=="destruction2" && a.size()==2 && a[1]=="checkpoint") destructionCheckpoint(w);
  else if(c=="destruction2" && (a.size()==2 || a.size()==3 || (a.size()==4 && a[3]=="npc")) && a[1]=="farm")
    destructionFarmSetup(w,pl,uint32_t(num(a,2,19355)),a.size()==4);
  else if(c=="destruction2" && a.size()==2 && a[1]=="farm-nearby") destructionFarmSetupNext(w,pl);
  else if(c=="destruction2" && a.size()==2 && a[1]=="farm-walk") {
    pl.closeWeapon(true);pl.stopWalking();pl.clearAiQueue();pl.setDirection(bench.destructionMeleeFacing);pl.updateTransform();
    exec("hold forward 2800",w,pl,ctrl);bench.waitUntil=bench.time+2834;
    }
  else if(c=="destruction2" && a.size()==2 && a[1]=="farm-ready") {
    if(!destructionMeleeEquip(w,pl,"1h"))return;
    pl.setPosition(bench.destructionFarmStart);
    pl.setDirection(bench.destructionMeleeFacing);pl.updateTransform();
    }
  else if(c=="destruction2" && a.size()==3 && a[1]=="farm-npc-walk") destructionFarmNpcWalk(w,a[2]);
  else if(c=="destruction2" && a.size()==3 && a[1]=="farm-checkpoint") destructionFarmCheckpoint(w,pl,a[2]);
  else if(c=="destruction2" && a.size()==3 && a[1]=="snapshot") destructionSessionSnapshot(w,a[2]);
  else if(c=="destruction2" && a.size()==4 && a[1]=="age" && (a[3]=="oldest" || a[3]=="youngest"))
    destructionAge(w,int64_t(num(a,2,0)),a[3]=="youngest");
  else if(c=="destruction2" && a.size()==3 && a[1]=="save") {
    try {destructionSessionSnapshot(w,"saved-"+a[2]);saveMovableSession(w,a[2]);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="destruction2" && a.size()==2 && a[1]=="far") {
    const auto parts=w.physic()->fragmentSnapshot();
    if(parts.empty()) {arenaAbort("destruction far requires map debris");return;}
    const auto origin=movablePosition(parts.front().transform);
    std::vector<const WayPoint*> candidates;
    w.findWayPoint(origin,[&](const WayPoint& point) {
      const auto distance=(point.pos-origin).length();
      if(!point.underWater && distance>=3600.f && distance<=10000.f)candidates.push_back(&point);
      return false;
      });
    std::sort(candidates.begin(),candidates.end(),[&](const auto* first,const auto* second) {
      const auto a=(first->pos-origin).quadLength(),b=(second->pos-origin).quadLength();
      return a==b?first->name<second->name:a<b;
      });
    Vec3 destination;std::string waypoint;float minimum=0;
    for(const auto* point:candidates) {
      const auto floor=w.physic()->landRay(point->pos+Vec3(0,200,0),800);
      if(!floor.hasCol || (floor.n.length()>0 && floor.n.y<.6f))continue;
      float distance=std::numeric_limits<float>::infinity();
      for(const auto& part:parts)distance=std::min(distance,(floor.v-movablePosition(part.transform)).length());
      if(distance<3500.f)continue;
      destination=floor.v;waypoint=point->name;minimum=distance;break;
      }
    if(waypoint.empty()) {arenaAbort("destruction far needs a supported distant authored waypoint");return;}
    pl.closeWeapon(true);pl.clearAiQueue();pl.stopWalking();pl.setPosition(destination);pl.updateTransform();
    event("destruction2_far","\"debris_cm\":"+ragdollVector(origin)+",\"hero_cm\":"+ragdollVector(destination)+
      ",\"waypoint\":"+jsonStr(waypoint)+",\"minimum_debris_distance_cm\":"+ragdollNumber(minimum)+
      ",\"distance_cm\":"+ragdollNumber((destination-origin).length()));
    }
  else if(c=="water-film" && a.size()==3 && a[1]=="setup")waterFilmSetup(w,pl,a[2]);
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
  else if(c=="water" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","buoyancy",a[1]=="on");
    w.physic()->setBuoyancyEnabled(a[1]=="on");
    }
  else if(c=="water" && a.size()==3 && a[1]=="setup")prepareWaterFixture(w,pl,a[2]);
  else if(c=="water-bodies" && a.size()==3 && a[1]=="setup") {
    if(a[2]=="deep")prepareSpellFixture(w,pl,"wind");
    prepareWaterBodiesFixture(w,pl,a[2]);
    }
  else if(c=="water-bodies" && a.size()==2 && a[1]=="kill-human") {
    if(bench.waterActors.empty() || bench.waterCase!="bodies-deep" || bench.waterActors.front().npc->isDown()) {
      arenaAbort("water corpse kill requires the living human at the prepared water lip");return;
      }
    bench.waterKillOnThrow=true;
    event("water_bodies_death_armed","\"actor_id\":"+std::to_string(w.npcId(bench.waterActors.front().npc))+
      ",\"minimum_wind_speed_cm_s\":100,\"minimum_displacement_cm\":50");
    }
  else if(c=="water-bodies" && a.size()==3 && a[1]=="checkpoint")waterBodiesCheckpoint(w,a[2]);
  else if(c=="water-bodies" && a.size()==3 && a[1]=="initial")waterBodiesInitial(w,pl,a[2]);
  else if(c=="water-bodies" && a.size()==2 && a[1]=="reset-motion") {
    bench.waterReference.clear();bench.waterTravel.clear();bench.waterSample=uint64_t(-1);waterFields(w);
    waterBodiesCheckpoint(w,"motion-reference");
    }
  else if(c=="water-bodies" && a.size()==3 && a[1]=="save") {
    try {
      if(bench.waterActors.empty() || std::any_of(bench.waterActors.begin(),bench.waterActors.end(),[](const auto& actor) {
        return !actor.npc->ragdollBody() || !actor.npc->ragdollBody()->isSleeping();
        })) {arenaAbort("water body save requires every observed corpse to have slept");return;}
      waterBodiesCheckpoint(w,"saved");saveMovableSession(w,a[2]);
      }
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="water-bodies" && a.size()==3 && a[1]=="loot-ready")waterBodiesLootReady(w,pl,a[2]);
  else if(c=="water-bodies" && a.size()==2 && a[1]=="loot")waterBodiesLoot(w,pl,ctrl);
  else if(c=="water" && a.size()==2 && a[1]=="torch")waterTorch(w,pl);
  else if(c=="water" && a.size()==3 && a[1]=="checkpoint")waterCheckpoint(w,a[2]);
  else if(c=="water" && a.size()==3 && a[1]=="initial")waterInitial(w,pl,ctrl,a[2]);
  else if(c=="water" && a.size()==2 && a[1]=="push-ready")prepareWaterSpell(w,pl,true);
  else if(c=="water" && a.size()==2 && a[1]=="fireball-ready")prepareWaterSpell(w,pl,false);
  else if(c=="water" && a.size()==3 && a[1]=="save") {
    try {
      for(const auto& body:w.physic()->waterBodySnapshot())
        if(waterOwns(body) && !body.sleeping) {
          arenaAbort("water save requires every observed body to have settled and slept");return;
          }
      waterCheckpoint(w,"saved");saveMovableSession(w,a[2]);
      }
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="water" && a.size()==2 && a[1]=="swim-ready") {
    const auto all=w.physic()->movableVobSnapshot();
    const auto barrel=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==bench.waterBarrelZen;});
    if(barrel==all.end() || barrel->broken) {arenaAbort("water swim requires the intact native barrel");return;}
    auto center=(barrel->localBounds[0]+barrel->localBounds[1])*.5f;barrel->transform.project(center);
    Vec3 surface;
    if(!waterProbe(w,Vec3(center.x,bench.waterCenter.y,center.z),160.f,6000.f,surface)) {
      arenaAbort("water swim barrel has not reached deep enough real water");return;
      }
    pl.closeWeapon(true);pl.clearAiQueue();pl.stopWalking();pl.setDirection(bench.waterDirection);
    pl.setPosition(surface-bench.waterDirection*280.f-Vec3(0,100,0));pl.updateTransform();
    bench.waterHeroStart=pl.position();bench.waterHeroLast=pl.position();bench.waterHeroMaxStep=0;
    bench.waterSwim=true;bench.waterSawSwim=false;
    bench.cam=C_Hold;lookFrom(surface-bench.waterDirection*600.f+Vec3(250,250,0),surface);
    event("water_swim_begin","\"barrel_cm\":"+ragdollVector(center)+",\"hero_cm\":"+ragdollVector(pl.position())+
      ",\"direction\":"+ragdollVector(bench.waterDirection));
    }
  else if(c=="water" && a.size()==2 && a[1]=="pickup-ready") {
    if(bench.waterItems.empty()) {arenaAbort("water pickup requires the fixture arrow");return;}
    bench.waterSwim=false;
    const auto point=bench.waterItems.front()->midPosition();
    const auto floor=w.physic()->landRay(point+Vec3(70,100,0),6000.f);
    if(!floor.hasCol) {arenaAbort("water pickup requires real ground beneath the item");return;}
    const auto water=w.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
    pl.closeWeapon(true);pl.clearAiQueue();pl.stopWalking();
    auto at=point+Vec3(70,0,0);at.y=water.hasCol && water.wdepth-floor.v.y>150.f?water.wdepth-100.f:floor.v.y;
    pl.setPosition(at);pl.setDirection(point-at);pl.updateTransform();
    event("water_pickup_ready","\"item_cm\":"+ragdollVector(point)+",\"hero_cm\":"+ragdollVector(at));
    }
  else if(c=="water" && a.size()==2 && a[1]=="pickup") {
    if(bench.waterItems.empty()) {arenaAbort("water pickup fixture arrow is absent");return;}
    auto* item=bench.waterItems.front();
    const auto symbol=item->clsId();
    const auto before=pl.itemCount(symbol);
    const auto point=item->midPosition();const auto state=pl.bodyStateMasked();
    const bool swimming=pl.isSwim();
    const bool accepted=ctrl.interact(*item);
    if(accepted)bench.waterItems.erase(bench.waterItems.begin());
    event("water_pickup","\"accepted\":"+std::string(accepted?"true":"false")+
      ",\"body_state\":"+std::to_string(unsigned(state))+",\"swimming\":"+(swimming?"true":"false")+
      ",\"inventory_before\":"+std::to_string(before)+",\"inventory_after\":"+std::to_string(pl.itemCount(symbol))+
      ",\"distance_cm\":"+ragdollNumber((pl.position()-point).length())+
      ",\"item_cm\":"+ragdollVector(point));
    }
  else if(c=="noise" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","physics_noise",a[1]=="on");
    w.physic()->setPhysicsNoiseEnabled(a[1]=="on");
    }
  else if(c=="noise" && a.size()==2 && a[1]=="setup") prepareNoiseFixture(w,pl,true);
  else if(c=="noise" && a.size()==2 && a[1]=="table") noiseTableCase(w,pl);
  else if(c=="noise" && a.size()==2 && a[1]=="listeners") noiseTableCase(w,pl,false);
  else if(c=="noise" && a.size()==2 && a[1]=="push") noisePushTable(w);
  else if(c=="noise" && a.size()==2 && (a[1]=="cup" || a[1]=="burst" || a[1]=="dialogue")) noiseItemCase(w,pl,a[1]);
  else if(c=="noise" && a.size()==2 && a[1]=="dialogue-drop") {
    const auto* listener=bench.noiseListeners.empty()?nullptr:bench.noiseListeners.front().npc;
    const bool active=listener && Gothic::inst().isNpcInDialog(*listener);
    event("physics_noise_dialogue","\"active\":"+std::string(active?"true":"false")+
      ",\"npc_id\":"+std::to_string(noiseNpcId(listener)));
    if(!active) {arenaAbort("physics noise dialogue fixture did not open an actual native dialogue");return;}
    bench.noiseStart=w.tickCount();
    noiseDropItems(w,1);
    }
  else if(c=="noise" && a.size()==2 && a[1]=="checkpoint") noiseCheckpoint(w);
  else if(c=="own-mesh-chest" && a.size()==6 && (a[1]=="setup" || a[1]=="reload"))
    ownMeshChestSetup(w,pl,Vec3(num(a,2,0),num(a,3,0),num(a,4,0)),uint32_t(std::stoul(a[5])),a[1]=="reload");
  else if(c=="own-mesh-chest" && a.size()==2 && a[1]=="open") {
    auto& fixture=bench.ownMeshChest;
    if(!fixture.chest) {arenaAbort("own-mesh-chest open needs setup");return;}
    ctrl.clearInput();ctrl.clearFocus();ctrl.tickFocus();
    const bool selected=ctrl.focus().interactive==fixture.chest;
    event("own_mesh_chest_focus","\"pass\":"+std::string(selected?"true":"false")+
      ",\"selected_mob_id\":"+std::to_string(w.mobsiId(ctrl.focus().interactive))+
      ",\"expected_mob_id\":"+std::to_string(w.mobsiId(fixture.chest))+
      ",\"hero_cm\":"+ragdollVector(pl.position()));
    if(!selected) {arenaAbort("own-mesh-chest ordinary focus did not select the exact authored chest");return;}
    fixture.opened=ctrl.interact(*fixture.chest) && pl.interactive()==fixture.chest;
    event("own_mesh_chest_open","\"pass\":"+std::string(fixture.opened?"true":"false")+
      ",\"native_path\":\"PlayerControl::interact/InventoryMenu::open/Npc::setInteraction\"");
    if(!fixture.opened)arenaAbort("own-mesh-chest native player interaction did not attach");
    }
  else if(c=="own-mesh-chest" && a.size()==3 && a[1]=="snapshot")ownMeshChestSnapshot(w,pl,a[2]);
  else if(c=="own-mesh-chest" && a.size()==2 && a[1]=="take") {
    auto& fixture=bench.ownMeshChest;
    if(!fixture.chest || !fixture.opened || fixture.chest->stateId()!=1 ||
       fixture.chest->objVisual().animNames().find("S_S1")==std::string::npos || !fixture.sawTransition) {
      arenaAbort("own-mesh-chest take needs native T_S0_2_S1 followed by S_S1");return;
      }
    // Exactly the transfer called by InventoryMenu::onTakeStuff for a chest;
    // this preserves native inventory removal and the player's taking deed.
    for(const auto& [id,count]:fixture.contents)pl.addItem(id,*fixture.chest,count);
    fixture.taken=true;
    ownMeshChestSnapshot(w,pl,"taken");
    }
  else if(c=="own-mesh-chest" && a.size()==2 && a[1]=="check") {
    const auto& fixture=bench.ownMeshChest;
    bool pass=fixture.chest && fixture.opened && fixture.sawTransition && fixture.taken &&
      fixture.chest->stateId()==1 && fixture.chest->inventory().isEmpty() &&
      fixture.chest->objVisual().animNames().find("S_S1")!=std::string::npos;
    for(const auto& [id,count]:fixture.contents)
      pass=pass && pl.inventory().itemCount(id)==fixture.heroBefore.at(id)+count;
    event("own_mesh_chest_check","\"pass\":"+std::string(pass?"true":"false")+
      ",\"saw_transition\":"+(fixture.sawTransition?"true":"false")+
      ",\"items\":"+ownMeshChestInventory(w,pl));
    if(!pass)arenaAbort("own-mesh-chest did not preserve native open state and exact item gain");
    }
  else if(c=="own-mesh-chest" && a.size()==3 && a[1]=="save") {
    if(!bench.ownMeshChest.chest || a[2].empty() ||
       a[2].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
      arenaAbort("own-mesh-chest save needs a chest fixture and a simple label");return;
      }
    try {
      ownMeshChestSnapshot(w,pl,"saved");std::vector<uint8_t> bytes;
      {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
      Gothic::inst().gameSession()->save(out,"own mesh chest "+a[2],preview);}
      std::ofstream file(outDir+"/session-"+a[2]+".sav",std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
      if(!file)throw std::runtime_error("cannot write complete own-mesh-chest session save");
      event("own_mesh_chest_save","\"archive\":"+jsonStr("session-"+a[2]+".sav")+",\"bytes\":"+std::to_string(bytes.size()));
      } catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="own-mesh" && a.size()==5 && a[1]=="setup")
    ownMeshSetup(w,pl,uint32_t(std::stoul(a[2])),Vec3(num(a,3,0),0,num(a,4,0)));
  else if(c=="own-mesh" && a.size()==2 && a[1]=="arrow")ownMeshArrow(w,pl);
  else if(c=="own-mesh" && a.size()==2 && a[1]=="clearance")ownMeshClearance(w,pl);
  else if(c=="own-mesh" && a.size()==2 && a[1]=="walk")ownMeshWalk(w,pl);
  else if(c=="own-mesh" && a.size()==2 && a[1]=="checkpoint")ownMeshCheckpoint(w,pl);
  else if(c=="movable" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","movable_vobs",a[1]=="on");
    w.physic()->setMovableVobsEnabled(a[1]=="on");
    }
  else if(c=="movable" && a.size()==2 && a[1]=="setup") prepareMovableFixture(w,pl);
  else if(c=="movable" && a.size()==2 && a[1]=="candle-setup") movableCandleSetup(w,pl);
  else if(c=="movable" && a.size()==2 && a[1]=="candle-push") movableCandlePush(w);
  else if(c=="movable" && a.size()==2 && a[1]=="candle-checkpoint") movableCandleCheckpoint(w);
  else if(c=="movable" && a.size()==2 && a[1]=="wake") {
    bench.movableWakeStart=w.tickCount();bench.movableWakeTrace=true;bench.movableWakeTravel.clear();
    say("movable wake: "+std::to_string(w.physic()->wakeMovableVobs()));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="drift") {
    std::FILE* f = std::fopen((outDir+"/movable-drift.jsonl").c_str(),"a");
    size_t moved = 0, total = 0;
    for(const auto& body:w.physic()->movableVobSnapshot()) {
      if(!body.enabled) continue;
      ++total;
      const auto shift = (movablePosition(body.transform)-movablePosition(body.origin)).length();
      const auto* before=body.origin.data();const auto* after=body.transform.data();
      const Vec3 ua(before[4],before[5],before[6]),ub(after[4],after[5],after[6]);
      const float tilt=std::acos(std::clamp((ua.x*ub.x+ua.y*ub.y+ua.z*ub.z)/(ua.length()*ub.length()),-1.f,1.f))*180.f/float(M_PI);
      if(shift>1.f || tilt>1.f) ++moved;
      if(f!=nullptr)
        std::fprintf(f,"%s\n",("{\"world\":"+jsonStr(w.name())+",\"zen_id\":"+std::to_string(body.zenId)+
          ",\"visual\":"+jsonStr(body.visual)+",\"shift_cm\":"+ragdollNumber(shift)+",\"tilt_deg\":"+ragdollNumber(tilt)+
          ",\"max_shift_cm\":"+ragdollNumber(std::max(shift,bench.movableWakeTravel[body.id]))+
          ",\"elapsed_ms\":"+std::to_string(w.tickCount()-bench.movableWakeStart)+
          ",\"sleeping\":"+(body.sleeping?"true":"false")+"}").c_str());
      }
    if(f!=nullptr) std::fclose(f);
    say("movable drift: "+std::to_string(moved)+" of "+std::to_string(total)+" moved more than 1 cm or 1 deg");
    }
  else if(c=="movable" && (a.size()==2 || a.size()==3) && a[1]=="clamps") {
    const float margin = a.size()==3 ? std::stof(a[2]) : 1.f;
    const auto all = w.physic()->movableVobContacts(margin);
    std::FILE* f = std::fopen((outDir+"/movable-clamps.jsonl").c_str(),"a");
    size_t touched = 0;
    for(const auto& body:all) {
      std::string row = "{\"world\":"+jsonStr(w.name())+",\"zen_id\":"+std::to_string(body.zenId)+
        ",\"visual\":"+jsonStr(body.visual)+",\"class\":"+jsonStr(body.className)+
        ",\"mass_kg\":"+ragdollNumber(body.mass)+",\"position_cm\":"+ragdollVector(body.position)+
        ",\"enabled\":"+(body.enabled?"true":"false")+",\"disposition\":"+jsonStr(body.disposition)+
        ",\"classification_ms\":"+ragdollNumber(float(w.physic()->movableVobValidationMilliseconds()))+
        ",\"hull_min_cm\":"+ragdollVector(body.hullMin)+",\"hull_max_cm\":"+ragdollVector(body.hullMax)+
        ",\"contacts\":[";
      for(size_t i=0;i<body.contacts.size();++i) {
        const auto& t = body.contacts[i];
        if(i>0) row += ",";
        row += "{\"other\":"+jsonStr(t.other)+",\"category\":"+std::to_string(t.category)+
          ",\"other_zen_id\":"+std::to_string(t.otherZenId)+
          ",\"interactive\":"+(t.interactive?"true":"false")+",\"below\":"+(t.below?"true":"false")+",\"side\":"+std::to_string(t.side)+
          ",\"other_min_cm\":"+ragdollVector(t.otherMin)+",\"other_max_cm\":"+ragdollVector(t.otherMax)+
          ",\"depth_cm\":"+ragdollNumber(t.depth)+",\"count\":"+std::to_string(t.count)+
          ",\"axis\":"+ragdollVector(t.axis)+",\"point_cm\":"+ragdollVector(t.point)+
          ",\"point_y_min_cm\":"+ragdollNumber(t.pointYMin)+",\"point_y_max_cm\":"+ragdollNumber(t.pointYMax)+
          ",\"material\":"+jsonStr(t.material)+",\"sector\":"+jsonStr(t.sector)+"}";
        }
      row += "]}";
      if(!body.contacts.empty()) ++touched;
      if(f!=nullptr) std::fprintf(f,"%s\n",row.c_str());
      }
    if(f!=nullptr) std::fclose(f);
    say("movable clamps: "+std::to_string(all.size())+" bodies, "+std::to_string(touched)+" with contacts");
    event("movable_clamps","\"world\":"+jsonStr(w.name())+",\"bodies\":"+std::to_string(all.size())+
      ",\"with_contacts\":"+std::to_string(touched)+",\"margin_cm\":"+ragdollNumber(margin)+
      ",\"classification_ms\":"+ragdollNumber(float(w.physic()->movableVobValidationMilliseconds())));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="observe") {
    bench.movableIds.clear();bench.movableMapIds.clear();
    for(const auto& body:w.physic()->movableVobSnapshot())
      if(body.zenId!=uint32_t(-1)) {bench.movableIds.push_back(body.id);bench.movableMapIds.insert(body.id);}
    for(uint32_t id=0;id<w.npcCount();++id)
      if(auto* npc=w.npcById(id);npc && npc!=&pl && (npc->position()-pl.position()).length()<1800) {
        if(!npc->isDown()) {
          npc->clearAiQueue();npc->clearState(false);npc->stopWalking();
          }
        npc->setExternalControl(true);
        }
    }
  else if(c=="movable" && a.size()==3 && a[1]=="snapshot") movableSaveSnapshot(w,a[2]);
  else if(c=="movable" && a.size()==3 && a[1]=="pin") movablePin(w,pl,a[2]);
  else if(c=="movable" && a.size()==2 && a[1]=="settle-target") {
    if(bench.spellActors.empty()) {arenaAbort("movable save NPC is absent");return;}
    bench.subject=bench.spellActors[0].npc;
    if(!bench.subject->stuckArrowCount()) {arenaAbort("movable save NPC arrow did not pin before death");return;}
    bench.subject->setRagdollDown(true);bench.subject->prepareRagdoll();
    }
  else if(c=="movable" && a.size()==2 && a[1]=="push-pinned") pushMovablePinnedTable(w);
  else if(c=="movable" && a.size()==3 && a[1]=="begin") movableBegin(w,a[2]);
  else if(c=="movable" && a.size()==2 && a[1]=="checkpoint") movableCheckpoint(w);
  else if(c=="movable" && a.size()==2 && a[1]=="walk") {
    if(!bench.movableReady) {arenaAbort("movable walking test needs the tavern fixture");return;}
    auto* npc=w.addNpc(w.script().findSymbolIndex("BAU_904_BAUER"),bench.movableWalkStart);
    if(!npc) {arenaAbort("movable walker insertion failed");return;}
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
    npc->setDirection(bench.movableWalkEnd-bench.movableWalkStart);npc->updateTransform();npc->setWalkMode(WalkBit::WM_Walk);
    bench.movableWalker=npc;
    const auto side=Vec3::normalize(bench.movableWalkEnd-bench.movableWalkStart);
    pl.setPosition(bench.movableWalkStart+Vec3(-side.z,0,side.x)*110.f);pl.updateTransform();
    const auto target=Vec3(bench.movableCenter.x,bench.movableWalkStart.y,bench.movableCenter.z);
    const bool enabled=w.physic()->movableVobsEnabled();
    w.physic()->setMovableVobsEnabled(false);const bool off=npc->testMove(target);
    w.physic()->setMovableVobsEnabled(true);const bool on=npc->testMove(target);
    w.physic()->setMovableVobsEnabled(enabled);
    event("movable_walk_setup","\"off_passable\":"+std::string(off?"true":"false")+
      ",\"on_passable\":"+(on?"true":"false")+",\"actor_id\":"+std::to_string(w.npcId(npc))+
      ",\"table_target_cm\":"+ragdollVector(target)+",\"floor_dish_id\":"+std::to_string(bench.movableDish));
    bench.movableDestination=WayPoint(bench.movableWalkEnd,"FP_MOVABLE_TAVERN_WALK");
    bench.movableDestination.groundPos=bench.movableWalkEnd;
    movableBegin(w,"walk");npc->aiPush(AiQueue::aiGoToPoint(bench.movableDestination));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="cast-ready") prepareMovableCast(w,pl);
  else if(c=="movable" && a.size()==2 && a[1]=="blast") {
    movableBegin(w,"blast");exec("spell cast 600",w,pl,ctrl);
    event("movable_blast","\"spell_id\":"+std::to_string(bench.spellCastingId));
    }
  else if(c=="movable" && a.size()==3 && a[1]=="save") {
    try {saveMovableSession(w,a[2]);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="movable" && a.size()==2 && a[1]=="reload") {
    size_t tables=0,utensils=0;
    for(const auto& body:w.physic()->movableVobSnapshot()) {tables+=body.className=="table";utensils+=body.className=="utensil";}
    event("movable_session_reload","\"world\":"+jsonStr(w.name())+",\"world_load_ms\":"+
      ragdollNumber(float(w.loadMilliseconds()))+",\"tables\":"+std::to_string(tables)+",\"utensils\":"+
      std::to_string(utensils)+",\"bodies\":"+std::to_string(w.physic()->physicsStats().bodies));
    // The fixture's extra dishes are temporary vobs, outside the map's saved
    // tree. A complete reload must recreate the authored map furniture.
    if(!tables)arenaAbort("full movable save reload has no registered map furniture");
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="movable" && a.size()==3 && a[1]=="measure") {
    if(!bench.movableReady || !AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {
      arenaAbort("movable normal-game cadence measurement cannot start");return;
      }
    bench.movableMeasuring=true;bench.movablePhysics.clear();bench.movablePhysics.reserve(20000);bench.movableOverflow=false;
    bench.noiseMeasuredImpacts=0;bench.noiseMeasuredHeard=0;bench.noiseMeasuredRays=0;
    for(auto& listener:bench.noiseListeners)listener.heard=0;
    AppCadence::counts(true,uint32_t(bench.movableIds.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("movable_measure_before","\"enabled\":"+std::string(w.physic()->movableVobsEnabled()?"true":"false")+
      ",\"bodies\":"+movableRows(w));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="report") {
    bench.movableMeasuring=false;
    AppCadence::counts(false,uint32_t(bench.movableIds.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("movable_measure_after","\"overflow\":"+std::string(bench.movableOverflow?"true":"false")+
      ",\"bodies\":"+movableRows(w));
    if(bench.noiseTrace)
      event("physics_noise_measure_after","\"enabled\":"+std::string(w.physic()->physicsNoiseEnabled()?"true":"false")+
        ",\"impacts\":"+std::to_string(bench.noiseMeasuredImpacts)+",\"perceptions\":"+
        std::to_string(bench.noiseMeasuredHeard)+",\"native_rays\":"+std::to_string(bench.noiseMeasuredRays)+
        ",\"listeners\":"+noiseListenerRows(w));
    if(auto* file=std::fopen((outDir+"/movable-physics.csv").c_str(),"w")) {
      std::fprintf(file,"sample,t_game_ms,dt_ms,step_ms,native_bodies\n");
      for(size_t i=0;i<bench.movablePhysics.size();++i) {const auto& row=bench.movablePhysics[i];
        std::fprintf(file,"%zu,%llu,%llu,%.9g,%zu\n",i,(unsigned long long)row.time,(unsigned long long)row.dt,double(row.milliseconds),row.bodies);}
      std::fclose(file);
      } else arenaAbort("cannot write movable native step timing samples");
    }
#endif
  else if(c=="spell" && a.size()==3 && a[1]=="fixture") {
    prepareSpellFixture(w,pl,a[2]);
    if(!bench.spellActors.empty()) exec("watch watched",w,pl,ctrl);
    }
  else if(c=="spell" && (a.size()==2 || a.size()==3) && a[1]=="guard-fixture") {
    prepareSpellFixture(w,pl,"wind");
    if(bench.spellActors.empty()) return;
    const bool water=a.size()==3 && a[2]=="water";
    bench.spellGuardCase=water?"water":"cliff";
    auto& site=water?bench.questWater:bench.questCliff;
    // Find a supported lip within reach of the finite wind impulse. Include
    // caster height and intermediate shelves when choosing the trajectory.
    Vec3 cliffFloor,hero;
    const auto targetOffset=bench.spellActors[0].npc->collosionCenter()-bench.spellActors[0].npc->position();
    const auto casterOffset=pl.collosionCenter()-pl.position();
    const auto reachesHazard=[&](const Vec3& start,const Vec3& caster) {
      const auto delta=start+targetOffset-caster-casterOffset;
      const float distance=delta.length();
      if(distance<1.f || distance>=1000.f) return false;
      auto velocity=Vec3::normalize(delta/distance+Vec3(0,.35f,0))*(450.f*(1.f-distance/1000.f)/75.f*100.f);
      auto at=start;
      // Reject short landings on the lip or an intermediate shelf. This only
      // chooses a fixture; the recorded throw still comes from the real spell.
      for(unsigned frame=0;frame<180;++frame) {
        at+=velocity/30.f;
        velocity.y-=DynamicWorld::gravityMS*100.f/30.f;
        const auto floor=w.physic()->landRay(Vec3(at.x,start.y+150.f,at.z),6000);
        if(!floor.hasCol) return false;
        if(water) {
          const auto liquid=w.physic()->waterRay(floor.v+Vec3(0,5,0),0);
          if(liquid.hasCol && at.y<liquid.wdepth-150.f && floor.v.y<liquid.wdepth-150.f) return true;
          }
        if(at.y<=floor.v.y)
          return !water && start.y-floor.v.y>1000.f && floor.n.y>.5f;
        }
      return false;
      };
    const auto findLip=[&](Bench::QuestSite& site) {
      const auto original=site.dry;
      const auto side=Vec3(-site.direction.z,0,site.direction.x);
      for(float lateral : {0.f,-50.f,50.f,-100.f,100.f,-200.f,200.f}) {
        for(float advance=700;advance>=-150;advance-=10) {
          const auto at=original+site.direction*advance+side*lateral;
          const auto center=w.physic()->landRay(at+Vec3(0,150,0),500);
          bool supported=center.hasCol && center.n.y>.85f;
          for(const auto delta:{Vec3(),site.direction*50.f,side*35.f,side*-35.f,site.direction*-35.f}) {
            const auto floor=w.physic()->landRay(Vec3(at.x,center.v.y,at.z)+delta+Vec3(0,40,0),100);
            supported=supported && floor.hasCol && floor.n.y>.85f && std::abs(floor.v.y-center.v.y)<15.f &&
                      !w.physic()->waterRay(floor.v,15.f).hasCol;
            }
          for(float distance:{25.f,50.f,75.f}) {
            const auto floor=w.physic()->landRay(center.v+site.direction*distance+Vec3(0,40,0),100);
            // A low upward hop can land on a rising lip before reaching the drop.
            supported=supported && (!floor.hasCol || floor.v.y<=center.v.y+2.f);
            }
          if(!supported) continue;
          bool reachableEdge=false;
          for(float distance:{100.f,125.f,150.f}) {
            const auto floor=w.physic()->landRay(center.v+site.direction*distance+Vec3(0,40,0),500);
            if(!floor.hasCol || center.v.y-floor.v.y>60.f) {reachableEdge=true;break;}
            }
          if(!reachableEdge) continue;
          bool hazard=false;
          for(float distance:{100.f,150.f,200.f,350.f,500.f,700.f}) {
            const auto edge=center.v+site.direction*distance;
            const auto floor=w.physic()->landRay(edge+Vec3(0,100,0),6000);
            if(!floor.hasCol || w.physic()->ray(center.v+Vec3(0,110,0),edge+Vec3(0,110,0)).hasCol) continue;
            const auto liquid=w.physic()->waterRay(floor.v+Vec3(0,5,0),0);
            if(water ? !liquid.hasCol || liquid.wdepth-floor.v.y<150.f || liquid.wdepth>center.v.y : center.v.y-floor.v.y<=1200.f) continue;
            cliffFloor=floor.v;
            if(water) site.waterY=liquid.wdepth;
            hazard=true;break;
            }
          if(!hazard) continue;
          for(float distance:{150.f,200.f,250.f,300.f}) {
            const auto caster=center.v-site.direction*distance;
            const auto floor=w.physic()->landRay(caster+Vec3(0,100,0),300);
            if(!floor.hasCol || floor.n.y<.85f ||
               w.physic()->waterRay(floor.v,15.f).hasCol ||
               w.physic()->ray(floor.v+Vec3(0,110,0),center.v+Vec3(0,110,0)).hasCol) continue;
            if(!reachesHazard(center.v,floor.v)) continue;
            site.dry=center.v;hero=floor.v;return true;
            }
          }
        }
      return false;
    };
    if(!chooseQuestSite(w,water,site,findLip)) {arenaAbort("wind guard lacks supported lip and viable throw");return;}
    auto* npc=bench.spellActors[0].npc;
    for(size_t i=1;i<bench.spellActors.size();++i) w.removeNpc(*bench.spellActors[i].npc);
    bench.spellActors.resize(1);bench.crew.clear();bench.crew.push_back(npc);
    // The hazard site is separate from the spell arena cleared above. Keep
    // nearby monsters from interrupting the real cast with a hurt animation.
    constexpr float clearRadius=3500.f;
    std::vector<Npc*> monsters;
    for(uint32_t i=0;i<w.npcCount();++i)
      if(auto* other=w.npcById(i);other && other->isMonster() &&
         ((other->position()-hero).length()<clearRadius || (other->position()-site.dry).length()<clearRadius))
        monsters.push_back(other);
    for(auto* monster:monsters) w.removeNpc(*monster);
    npc->handle().flags=zenkit::NpcFlag(uint32_t(npc->handle().flags)&~uint32_t(zenkit::NpcFlag::IMMORTAL));
    npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
    npc->setPosition(site.dry);npc->setDirection(site.direction);npc->updateTransform();
    pl.setPosition(hero);pl.setDirection(site.direction);pl.updateTransform();
    Gothic::settingsSetI("GAME","spell_physics",1);Gothic::settingsSetI("GAME","ragdoll_quest_guard",1);
    bench.spellGuardTrace=true;bench.spellGuardObservedReturns=0;
    bench.subject=npc;exec("watch watched",w,pl,ctrl);
    event("spell_guard_setup","\"actor_id\":"+std::to_string(w.npcId(npc))+
      ",\"case\":"+jsonStr(bench.spellGuardCase)+",\"water_y_cm\":"+ragdollNumber(site.waterY)+
      ",\"site\":"+jsonStr(site.waypoint)+",\"stable_cm\":"+ragdollVector(site.dry)+
      ",\"hero_cm\":"+ragdollVector(hero)+",\"direction\":"+ragdollVector(site.direction)+
      ",\"cleared_monsters\":"+std::to_string(monsters.size())+",\"clear_radius_cm\":"+ragdollNumber(clearRadius)+
      ",\"cliff_floor_cm\":"+ragdollVector(cliffFloor)+",\"support_margin_cm\":50");
    }
  else if(c=="spell" && a.size()==3 && a[1]=="target") {
    auto actor=std::find_if(bench.spellActors.begin(),bench.spellActors.end(),[&](const auto& value) {return value.role==a[2];});
    if(actor==bench.spellActors.end()) {arenaAbort("spell target role missing");return;}
    if(a[2]=="troll") {
      actor->npc->setPosition(bench.spellActors[0].npc->position()+Vec3(220,0,0));
      actor->npc->updateTransform();
      }
    std::iter_swap(bench.spellActors.begin(),actor);
    bench.subject=bench.spellActors[0].npc;
    exec("watch watched",w,pl,ctrl);
    event("spell_selected_target","\"role\":"+jsonStr(a[2])+",\"actor_id\":"+std::to_string(w.npcId(bench.subject)));
    }
  else if(c=="spell" && a.size()==3 && a[1]=="case") {
    bench.spellCase=a[2];bench.spellTrace=true;
    event("spell_physics_case","\"case\":"+jsonStr(a[2]));
    }
  else if(c=="spell" && a.size()==3 && a[1]=="feature")
    Gothic::settingsSetI("GAME","spell_physics",a[2]=="on");
  else if(c=="spell" && a.size()==3 && a[1]=="equip") {
    const int32_t id=int32_t(num(a,2,0));
    const auto rune=bench.spellRunes.find(id);
    if(rune==bench.spellRunes.end()) {arenaAbort("requested real spell rune not found");return;}
    pl.useItem(rune->second,Item::NSLOT,true);
    if(pl.drawSpell(id)) bench.spellCastingId=id;
    else {bench.spellEquipPending=id;bench.spellEquipUntil=w.tickCount()+3000;}
    }
  else if(c=="spell" && a.size()>=2 && a[1]=="cast") {
    if(bench.spellActors.empty()) {arenaAbort("real spell needs a target");return;}
    auto* target=bench.spellActors[0].npc;pl.setTarget(target);pl.setDirection(target->position()-pl.position());pl.updateTransform();
    const auto result=pl.beginCastSpell();
    event("spell_physics_cast","\"id\":"+std::to_string(bench.spellCastingId)+",\"begin_result\":"+
      std::to_string(int(result))+",\"active_item\":"+std::to_string(pl.bodyObservationState().weaponSymbol)+
      ",\"target_in_air\":"+(target->isInAir()?"true":"false"));
    if(result!=Npc::BC_Invest && result!=Npc::BC_Cast) {arenaAbort("real spell beginCastSpell rejected");return;}
    bench.spellReleaseAt=result==Npc::BC_Invest?w.tickCount()+uint64_t(num(a,2,500)):0;
    }
  else if(c=="spell" && a.size()==3 && a[1]=="wait-return") {
    bench.spellWaitFrozen=4;bench.spellWaitUntil=w.tickCount()+uint64_t(num(a,2,20000));
    }
  else if(c=="spell" && a.size()>=3 && a[1]=="wait-frozen") {
    bench.spellWaitFrozen=a[2]=="on"?1:0;bench.spellWaitUntil=w.tickCount()+uint64_t(num(a,3,25000));
    }
  else if(c=="spell" && a.size()>=2 && (a[1]=="wait-airborne" || a[1]=="wait-field")) {
    bench.spellWaitFrozen=a[1]=="wait-airborne"?2:3;bench.spellWaitUntil=w.tickCount()+uint64_t(num(a,2,6000));
    }
  else if(c=="spell" && a.size()==2 && a[1]=="stop") bench.spellTrace=false;
  else if(c=="spell" && a.size()==2 && a[1]=="clear") {
    bench.spellTrace=false;bench.spellReleaseAt=0;bench.spellWaitFrozen=-1;bench.spellGuardTrace=false;
    pl.setTarget(nullptr);exec("watch player",w,pl,ctrl);
    for(auto& actor:bench.spellActors) w.removeNpc(*actor.npc);
    for(auto* item:bench.spellItems) w.removeItem(*item);
    bench.spellActors.clear();bench.spellItems.clear();bench.crew.clear();
    }
  else if(c=="sticking" && a.size()==2 && a[1]=="setup") {
    bench.stickingTrace=true;bench.arrowTrace=false;
    event("arrow_sticking_setup","\"default_enabled\":"+
      std::string(Gothic::settingsGetI("GAME","arrow_sticking")?"true":"false"));
    }
  else if(c=="sticking" && a.size()==3 && a[1]=="case") {
    bench.stickingCase=a[2];bench.stickingTrace=true;
    event("arrow_sticking_case","\"case\":"+jsonStr(a[2])+
      ",\"npc_pins_before\":"+std::to_string(bench.arrowNpc?bench.arrowNpc->stuckArrowCount():0)+
      ",\"world_pins_before\":"+std::to_string(w.stuckArrowCount()));
    }
  else if(c=="sticking" && a.size()==3 && a[1]=="feature")
    Gothic::settingsSetI("GAME","arrow_sticking",a[2]=="on");
  else if(c=="sticking" && a.size()==2 && a[1]=="world") {
    const auto ahead=facing(pl),side=Vec3(-ahead.z,0,ahead.x);
    const auto target=pl.position()+ahead*450.f+side*220.f;
    const auto floor=w.physic()->landRay(target+Vec3(0,300,0),900.f);
    if(!floor.hasCol) {arenaAbort("arrow sticking ground target has no world surface");return;}
    bench.stickingWorldPoint=floor.v;bench.arrowAim="world";bench.arrowDraw=1.f;
    bench.aimNpc=nullptr;bench.aimFixed=false;
    bench.cam=C_Hold;
    lookFrom(floor.v-ahead*230.f+side*180.f+Vec3(0,125,0),floor.v+Vec3(0,10,0));
    event("arrow_sticking_world_target","\"point_cm\":"+ragdollVector(floor.v)+",\"normal\":"+ragdollVector(floor.n));
    }
  else if(c=="sticking" && a.size()==2 && a[1]=="motion") {
    auto* npc=bench.arrowNpc;
    if(npc==nullptr || npc->isDown()) {arenaAbort("arrow pin motion needs a living target");return;}
    bench.livingWalkers.clear();bench.livingWalkers.emplace_back();auto& walk=bench.livingWalkers.back();
    walk.npc=npc;
    walk.ends[0]=WayPoint(npc->position()+Vec3(250,0,0),"FP_ARROW_PIN_A");
    walk.ends[1]=WayPoint(npc->position(),"FP_ARROW_PIN_B");
    for(auto& point:walk.ends) {
      const auto floor=w.physic()->landRay(point.pos+Vec3(0,200,0),500);
      if(!floor.hasCol) {arenaAbort("arrow pin walking path has no ground");return;}
      point.pos.y=floor.v.y;point.groundPos=point.pos;
      }
    npc->setWalkMode(WalkBit::WM_Walk);bench.livingMotion=true;
    }
  else if(c=="sticking" && a.size()==2 && (a[1]=="still" || a[1]=="turn")) {
    if(auto* npc=bench.arrowNpc) {
      bench.livingMotion=false;bench.livingWalkers.clear();npc->clearAiQueue();npc->stopWalking();
      npc->setExternalControl(true);npc->setAnim(Npc::Anim::Idle);
      npc->setDirection(a[1]=="turn" ? rotY(facing(*npc),90.f) : pl.position()-npc->position());npc->updateTransform();
      }
    }
  else if(c=="sticking" && a.size()==2 && a[1]=="remove") {
    const auto pins=npcArrowPins(w),bodies=w.physic()->physicsStats().bodies,worldPins=w.stuckArrowCount();
    exec("arrow cleanup",w,pl,ctrl);
    event("arrow_sticking_remove","\"npc_pins_before\":"+std::to_string(pins)+
      ",\"npc_pins_after\":"+std::to_string(npcArrowPins(w))+
      ",\"world_pins_before\":"+std::to_string(worldPins)+",\"world_pins_after\":"+std::to_string(w.stuckArrowCount())+
      ",\"bodies_before\":"+std::to_string(bodies)+",\"bodies_after\":"+std::to_string(w.physic()->physicsStats().bodies));
    }
  else if(c=="sticking" && a.size()==2 && a[1]=="stop") {
    bench.stickingTrace=false;bench.arrowAim.clear();
    }
  else if(c=="arrow" && a.size()==2 && (a[1]=="setup" || a[1]=="replace")) {
    if(a[1]=="setup") {
      if(!chooseRagdollArena(w,pl)) {arenaAbort("arrow fixture needs supported empty ground");return;}
      exec("clear 2500",w,pl,ctrl);
      pl.setDirection({0,0,1});pl.updateTransform();
      } else exec("arrow cleanup",w,pl,ctrl);
    const auto symbol=w.script().findSymbolIndex("BAU_904_BAUER");
    auto at=bench.ragdollArenaTarget-Vec3(0,80,400);
    auto* npc=w.addNpc(symbol,at);
    if(npc==nullptr) {arenaAbort("arrow fixture NPC insertion failed");return;}
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
    npc->setDirection(pl.position()-at);npc->updateTransform();
    npc->handle().attribute[ATR_HITPOINTS]=10000;
    npc->handle().attribute[ATR_HITPOINTSMAX]=10000;
    bench.arrowNpc=npc;bench.subject=npc;bench.crew.push_back(npc);
    bench.arrowSeenHits=0;bench.arrowSampleTick=0;
    exec("watch watched",w,pl,ctrl);
    event("arrow_reaction_setup","\"actors\":1,\"root_pinned\":false,\"position_cm\":"+ragdollVector(npc->position()));
    }
  else if(c=="arrow" && a.size()==3 && a[1]=="case") {
    bench.arrowCase=a[2];bench.arrowTrace=true;
    event("arrow_reaction_case","\"case\":"+jsonStr(a[2])+",\"hits_before\":"+
      std::to_string(bench.arrowNpc ? bench.arrowNpc->arrowPhysicsProbe().hits : 0));
    }
  else if(c=="arrow" && a.size()>=3 && a[1]=="aim") {
    bench.arrowAim=a[2];bench.arrowDraw=std::clamp(num(a,3,1.f),.1f,1.f);
    bench.aimNpc=nullptr;bench.aimFixed=false;
    }
  else if(c=="arrow" && a.size()==3 && a[1]=="feature")
    Gothic::settingsSetI("GAME","arrow_physics",a[2]=="on");
  else if(c=="arrow" && a.size()==2 && a[1]=="ko-on-hit")
    bench.arrowKoOnHit=true;
  else if(c=="arrow" && a.size()==2 && a[1]=="cleanup") {
    if(auto* npc=bench.arrowNpc) {
      bench.arrowTrace=false;bench.arrowAim.clear();bench.arrowNpc=nullptr;bench.arrowKoOnHit=false;
      const auto before=w.physic()->physicsStats().bodies;
      const auto parts=(npc->livingBody()?npc->livingBody()->partCount():0)+(npc->ragdollBody()?npc->ragdollBody()->partCount():0);
      exec("watch player",w,pl,ctrl);bench.crew.clear();w.removeNpc(*npc);
      const auto after=w.physic()->physicsStats().bodies;
      event("arrow_reaction_cleanup","\"parts\":"+std::to_string(parts)+",\"before\":"+
        std::to_string(before)+",\"after\":"+std::to_string(after));
      }
    }
  else if(c=="arrow" && a.size()==2 && a[1]=="stop") {
    bench.arrowTrace=false;bench.arrowAim.clear();
    }
  else if(c=="living-seat" && a.size()==2 && a[1]=="setup") {
    Interactive* mob=w.availableMob(pl,"BENCH");
    if(!mob) mob=w.availableMob(pl,"CHAIR");
    if(!mob) {arenaAbort("sitting fixture needs an available real BENCH or CHAIR nearby");return;}
    bench.seatMob=mob;
    auto stand=mob->nearestPoint(pl)+Vec3(0,0,-80);
    const auto ground=w.physic()->landRay(stand+Vec3(0,200,0),500);
    if(!ground.hasCol) {arenaAbort("sitting fixture has no supported floor");return;}
    stand.y=ground.v.y;
    pl.setPosition(stand-Vec3(0,0,350));pl.updateTransform();
    exec("clear 600",w,pl,ctrl);
    const auto symbol=w.script().findSymbolIndex("BDT_1013_BANDIT_L");
    auto* npc=w.addNpc(symbol,stand);
    auto* corpse=w.addNpc(symbol,stand+Vec3(100,0,60));
    if(!npc || !corpse) {arenaAbort("sitting fixture actors unavailable");return;}
    for(auto* actor:{npc,corpse}) {actor->clearAiQueue();actor->clearState(false);actor->setExternalControl(true);}
    npc->setDirection(mob->position()-stand);npc->updateTransform();
    corpse->setLastHitBy(&pl);corpse->setRagdollDown(true);corpse->prepareRagdoll();
    bench.seatNpc=npc;bench.seatCorpse=corpse;bench.subject=npc;
    for(const float x:{-35.f,35.f}) {
      auto matrix=Matrix4x4::mkIdentity();matrix.translate(stand+Vec3(x,25,35));
      auto* item=w.addItemDyn(w.script().findSymbolIndex("ItPo_Health_01"),matrix,size_t(-1));
      if(!item) {arenaAbort("sitting fixture dropped item unavailable");return;}
      bench.seatItems.push_back(item);
      }
    Gothic::settingsSetI("GAME","living_bodies",1);
    event("living_seat_setup","\"scheme\":"+jsonStr(mob->schemeName())+",\"mob_cm\":"+ragdollVector(mob->position())+
      ",\"start_cm\":"+ragdollVector(stand)+",\"items\":2");
    }
  else if(c=="living-seat" && a.size()==2 && a[1]=="sit") {
    if(!bench.seatNpc || !bench.seatNpc->livingBody()) {arenaAbort("sitting fixture requires its existing native living rig");return;}
    for(auto* item:bench.seatItems) item->setPhysicsEnable(w);
    bench.seatPhase="sit";bench.seatPreviousItems.clear();bench.seatSampleTick=w.tickCount();
    for(const auto* item:bench.seatItems) bench.seatPreviousItems.push_back(item->position());
    std::vector<Matrix4x4> native;
    if(bench.seatNpc->livingBody()->getPose(native)) {
      bench.seatPreviousParts.resize(native.size());
      for(auto bone:bench.seatNpc->livingBody()->boneIndices()) bench.seatPreviousParts[bone]={native[bone].at(3,0),native[bone].at(3,1),native[bone].at(3,2)};
      }
    const auto from=bench.seatNpc->position();
    const bool attached=bench.seatNpc->setInteraction(bench.seatMob);
    event("living_seat_attach",std::string("\"attached\":")+(attached?"true":"false")+",\"jump_cm\":"+
      ragdollNumber((bench.seatNpc->position()-from).length()));
    if(!attached) arenaAbort("real NPC bench attachment failed");
    }
  else if(c=="living-seat" && a.size()==2 && a[1]=="stand") {
    if(!bench.seatNpc) return;
    bench.seatNpc->setInteraction(nullptr,false);
    bench.seatPhase="stand";
    }
  else if(c=="living-seat" && a.size()==2 && a[1]=="shift") {
    if(!bench.seatNpc) return;
    bench.seatNpc->setInteraction(nullptr,true);
    bench.seatNpc->setPosition(bench.seatNpc->position()+Vec3(80,0,0));bench.seatNpc->updateTransform();
    bench.seatPhase="shift";
    event("living_seat_shift","\"distance_cm\":80");
    }
  else if(c=="living" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","living_bodies",a[1]=="on");bench.livingPhase=a[1];
    }
  else if(c=="living" && a.size()==2 && a[1]=="radius-check") {
    auto* npc=bench.subject;
    if(!npc || npc->isDown()) {arenaAbort("living radius needs a live watched actor");return;}
    const auto hero=pl.position();
    const auto offset=pl.centerPosition()-pl.position();
    const float distances[]{1950,2010,2190,2210,2100,2010,1990,2010};
    const bool expected[]{true,true,true,false,false,false,true,true};
    bool pass=true;
    for(size_t i=0;i<8;++i) {
      pl.setPosition(npc->centerPosition()+Vec3(distances[i],0,0)-offset);pl.updateTransform();
      npc->prepareLivingBody();
      const bool present=npc->livingBody()!=nullptr;
      pass&=present==expected[i];
      event("living_radius_check","\"distance_cm\":"+ragdollNumber((pl.centerPosition()-npc->centerPosition()).length())+
        ",\"expected\":"+(expected[i]?"true":"false")+",\"present\":"+(present?"true":"false"));
      }
    pl.setPosition(hero);pl.updateTransform();npc->prepareLivingBody();
    if(!pass) arenaAbort("living radius hysteresis check failed");
    }
  else if(c=="living" && a.size()==2 && a[1]=="setup") {
    if(!chooseRagdollArena(w,pl)) {arenaAbort("living fixture needs an empty supported arena");return;}
    exec("clear 2500",w,pl,ctrl);
    const auto symbol=w.script().findSymbolIndex("BDT_1013_BANDIT_L");
    for(size_t i=0;i<10;++i) {
      auto* npc=w.addNpc(symbol,bench.ragdollArenaPlaces[i+5]);
      if(!npc) {arenaAbort("living ten-NPC insertion failed");return;}
      npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
      npc->setDirection(pl.position()-npc->position());npc->updateTransform();bench.crew.push_back(npc);
      if(i<4) {
        bench.livingWalkers.emplace_back();auto& walker=bench.livingWalkers.back();walker.npc=npc;
        walker.ends[0]=WayPoint(npc->position()-Vec3(0,0,300),"FP_LIVING_BENCH_A");
        walker.ends[1]=WayPoint(npc->position(),"FP_LIVING_BENCH_B");
        for(auto& point:walker.ends) {
          const auto floor=w.physic()->landRay(point.pos+Vec3(0,200,0),500);
          if(!floor.hasCol) {arenaAbort("living walking path has no floor");return;}
          point.pos.y=floor.v.y;point.groundPos=point.pos;
          }
        npc->setWalkMode(WalkBit::WM_Walk);
        }
      }
    bench.livingFixture=true;bench.livingHero=pl.position();bench.subject=bench.crew.back();
    exec("watch watched",w,pl,ctrl);
    if(!frameRagdollArena(10)) {arenaAbort("living ten-model camera failed");return;}
    event("living_setup","\"actors\":10,\"walkers\":4,\"hero_cm\":"+ragdollVector(bench.livingHero));
    }
  else if(c=="living" && a.size()==2 && a[1]=="motion") bench.livingMotion=true;
  else if(c=="living" && a.size()==3 && a[1]=="debug") bench.livingDebug=a[2]=="on";
  else if(c=="living" && a.size()==2 && (a[1]=="far" || a[1]=="near")) {
    pl.setPosition(bench.livingHero+(a[1]=="far" ? Vec3(0,0,-6000) : Vec3()));pl.updateTransform();
    bench.livingPhase=a[1]=="far" ? "far" : "on";
    }
  else if(c=="living" && a.size()==2 && a[1]=="ko") {
    auto* npc=bench.subject;if(!npc || npc->isDown()) {arenaAbort("living handoff needs a live watched NPC");return;}
    npc->setLastHitBy(&pl);npc->setRagdollDown(false);npc->prepareRagdoll();bench.livingPhase="ko";
    }
  else if(c=="living" && a.size()==3 && a[1]=="check") {
    const auto counts=livingCounts();const bool ko=a[2]=="ko",on=a[2]=="on" || ko;
    size_t hits=0;
    for(const auto* npc:bench.crew) if(!npc->isDown()) {
      const auto head=npc->mapHeadBone();float fraction=1;
      if(npc->rayHitZone(head+Vec3(100,0,0),head-Vec3(100,0,0),fraction)!=HitZones::None) ++hits;
      }
    const bool pass=counts.models==10 && counts.alive==(ko?9u:10u) && counts.duplicates==0 &&
      counts.kinematic==(on?(ko?9u:10u):0u) && counts.parts==(on?(ko?216u:240u):0u) &&
      counts.dynamic==(ko?1u:0u) && counts.dynamicParts==(ko?24u:0u) && hits==counts.alive;
    event("living_check","\"mode\":"+jsonStr(a[2])+",\"pass\":"+(pass?"true":"false")+
      ",\"ordinary_hits\":"+std::to_string(hits)+","+livingCountFields(counts,w));
    bench.livingPhase=a[2];
    if(!pass) arenaAbort("living body/count/hit checkpoint failed");
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="living" && a.size()==3 && a[1]=="measure") {
    const auto counts=livingCounts();bench.livingMode=a[2]=="on";
    if(!bench.livingFixture || counts.models!=10 || counts.alive!=10 ||
       counts.parts!=(bench.livingMode?240u:0u) || !AppCadence::normalClock()) {arenaAbort("living measurement precondition failed");return;}
    bench.livingPhysics.reserve(20000);bench.livingPhysics.clear();bench.livingOverflow=false;
    AppCadence::counts(true,10,0,uint32_t(counts.parts),0,0);
    if(!AppCadence::arm(bench.livingMode)) {arenaAbort("living cadence arm failed");return;}
    bench.livingMeasuring=true;
    event("living_measure_before",livingCountFields(counts,w));
    }
  else if(c=="living" && a.size()==2 && a[1]=="report") {
    bench.livingMeasuring=false;const auto counts=livingCounts();
    AppCadence::counts(false,uint32_t(counts.models),0,uint32_t(counts.parts),0,0);
    event("living_measure_after",livingCountFields(counts,w)+",\"overflow\":"+(bench.livingOverflow?"true":"false"));
    if(auto* file=std::fopen((outDir+"/living-physics.csv").c_str(),"w")) {
      std::fprintf(file,"sample,t_game_ms,dt_ms,step_ms,native_bodies\n");
      for(size_t i=0;i<bench.livingPhysics.size();++i) {
        const auto& sample=bench.livingPhysics[i];std::fprintf(file,"%zu,%llu,%llu,%.9g,%zu\n",i,
          (unsigned long long)sample.time,(unsigned long long)sample.dt,double(sample.milliseconds),sample.bodies);
        }
      std::fclose(file);
      }
    }
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
  else if(c=="ragdoll" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","ragdoll",a[1]=="on" ? 1 : 0);
    say("ragdoll "+a[1]);
    event("ragdoll_toggle","\"enabled\":"+std::string(a[1]=="on" ? "true" : "false"));
    }
  else if(c=="ragdoll" && a.size()>1 && (a[1]=="save" || a[1]=="restore")) {
    try {
      ragdollNpcArchive(a,w,pl);
      } catch(const std::exception& error) {
      say("ragdoll archive failed: "+std::string(error.what()));
      event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
      }
    }
  else if(c=="ragdoll" && a.size()>1 && (a[1]=="session-save" || a[1]=="session-snapshot" || a[1]=="session-initial")) {
    try {
      if(a[1]=="session-initial")
        throw std::runtime_error("session-initial must be the first scenario command at dt=0");
      ragdollSessionArchive(a,w,pl);
      } catch(const std::exception& error) {
      say("ragdoll session failed: "+std::string(error.what()));
      event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
      }
    }
  else if(c=="ragdoll" && a.size()==3 && a[1]=="loot") {
    try {
      Npc* npc=findNpc(w,pl,a[2]);
      if(!ragdollTrace || npc==nullptr || npc==&pl || !npc->isDead() || npc->ragdollBody()==nullptr || pl.isDown())
        throw std::runtime_error("loot fixture requires trace=1 and a physical corpse with a live player");
      const auto inventory=ragdollInventory(*npc,w);
      const auto lootable=ragdollInventory(*npc,w,Inventory::T_Ransack);
      if(lootable=="[]")
        throw std::runtime_error("loot fixture needs nonempty actual ransack inventory");
      ctrl.clearFocus(); ctrl.tickFocus();
      const auto focus=ctrl.focus();
      const bool matched=focus.npc==npc;
      event("ragdoll_loot_focus","\"expected_actor_id\":"+std::to_string(w.npcId(npc))+
        ",\"selected_actor_id\":"+std::to_string(w.npcId(focus.npc))+
        ",\"actor\":"+jsonStr(TestBench::who(npc))+
        ",\"matched\":"+(matched ? "true" : "false")+
        ",\"position_cm\":"+ragdollVector(npc->position())+",\"lootable_inventory\":"+lootable);
      if(!matched)
        throw std::runtime_error("ordinary player focus did not select the exact moved corpse");
      bench.expectedRagdollLoot=npc; bench.ragdollLootDidOpen=false;
      const bool interacted=ctrl.interact(*npc);
      bench.expectedRagdollLoot=nullptr;
      if(!interacted || !bench.ragdollLootDidOpen || inventory!=ragdollInventory(*npc,w))
        throw std::runtime_error("ordinary rendered ransack did not open the exact corpse or changed inventory");
      say("ragdoll loot "+symName(w,npc)+" verified exact native Ransack target; closed synchronously without transfer");
      } catch(const std::exception& error) {
      bench.expectedRagdollLoot=nullptr;
      say("ragdoll loot failed: "+std::string(error.what()));
      event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
      }
    }

  else if(c=="guard" && a.size()==6 && a[1]=="setup") {
    Npc* npc=&subjectOf(pl);
    const bool water=a[3]=="water", enabled=a[5]=="on";
    if(npc==nullptr || (a[3]!="water" && a[3]!="cliff") || a[4]!="normal" || (a[5]!="on" && a[5]!="off")) {
      arenaAbort("guard setup expects watched actor, label cliff|water normal on|off");return;
      }
    auto& site=water ? bench.questWater : bench.questCliff;
    if(!chooseQuestSite(w,water,site)) { arenaAbort("no supported dry quest-guard hazard site");return; }
    Gothic::settingsSetI("GAME","ragdoll_quest_guard",enabled);
    auto& flags=npc->handle().flags;
    flags=zenkit::NpcFlag(uint32_t(flags)&~uint32_t(zenkit::NpcFlag::IMMORTAL));
    npc->setAttitude(ATT_NEUTRAL);
    if(npc!=&pl && npc->isHuman()) {
      npc->handle().attribute[ATR_HITPOINTSMAX]=100;
      npc->handle().attribute[ATR_HITPOINTS]=2;
      for(auto& protection:npc->handle().protection) protection=0;
      }
    pl.closeWeapon(true);pl.handle().attribute[ATR_STRENGTH]=1000;
    pl.handle().damage_type=1<<size_t(zenkit::DamageType::BLUNT);
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);npc->setPosition(site.dry);npc->setDirection(site.direction);npc->updateTransform();
    auto hero=site.dry-site.direction*300;
    if(const auto floor=w.physic()->landRay(hero+Vec3(0,300,0),1500);floor.hasCol) hero.y=floor.v.y;
    if(npc!=&pl) {pl.setPosition(hero);pl.updateTransform();}
    // Keep unrelated attacks from changing the dry-anchor setup or the filmed recovery.
    for(uint32_t id=0;id<w.npcCount();++id)
      if(auto* other=w.npcById(id);other!=nullptr && other!=&pl && other!=npc &&
         (other->position()-site.dry).length()<4000)
        exec("freeze id:"+std::to_string(id),w,pl,ctrl);
    bench.questTrace=true;bench.questNpc=npc;bench.questCase=a[2];bench.questWaterCase=water;
    bench.questSawDrop=bench.questSawWater=bench.questWait=bench.questLaunched=bench.questStandingWait=false;bench.questLastReturns=0;bench.questWallNs=0;
    event("immortal_guard_case","\"case\":"+jsonStr(a[2])+",\"actor_id\":"+std::to_string(w.npcId(npc))+
      ",\"immortal\":"+(npc->isImmortal()?"true":"false")+",\"enabled\":"+(enabled?"true":"false")+
      ",\"site\":"+jsonStr(site.waypoint)+",\"dry_cm\":"+ragdollVector(site.dry)+",\"direction\":"+ragdollVector(site.direction)+
      ",\"water_y_cm\":"+ragdollNumber(site.waterY));
    }
  else if(c=="guard" && (a.size()==2 || (a.size()==3 && a[2]=="safe")) && a[1]=="launch") {
    auto* npc=bench.questNpc;if(npc==nullptr || npc->isDown()) {arenaAbort("guard launch needs living actor");return;}
    bench.questStable=npc->position();const auto& q=npc->ragdollQuestReturnState();
    if(npc->isHuman() && !npc->isPlayer() && Gothic::settingsGetI("GAME","ragdoll_quest_guard") && (!q.hasAnchor || (q.anchor-bench.questStable).length()>3)) {
      arenaAbort("guard launch lacks a real stable dry anchor");return;
      }
    event("immortal_guard_launch","\"case\":"+jsonStr(bench.questCase)+",\"stable_cm\":"+ragdollVector(bench.questStable)+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"immortal\":"+(npc->isImmortal()?"true":"false"));
    bench.questLaunched=true;
    const auto before=npc->attribute(ATR_HITPOINTS);
    npc->takeDamage(pl,nullptr);npc->prepareRagdoll();
    event("immortal_guard_damage","\"case\":"+jsonStr(bench.questCase)+",\"hp_before\":"+std::to_string(before)+
      ",\"hp_after\":"+std::to_string(npc->attribute(ATR_HITPOINTS))+",\"unconscious\":"+(npc->isUnconscious()?"true":"false")+
      ",\"dead\":"+(npc->isDead()?"true":"false")+",\"melee\":true");
    if(before!=2 || npc->attribute(ATR_HITPOINTS)!=1 || !npc->isUnconscious() || npc->isDead() || !npc->ragdollBody()) {arenaAbort("real melee damage did not produce HP2 to HP1 native KO");return;}
    const auto direction=(bench.questWaterCase?bench.questWater:bench.questCliff).direction;
    if(a.size()==2) npc->addRagdollImpulse(direction*1000.f+Vec3(0,50,0));
    }
  else if(c=="guard" && a.size()==2 && a[1]=="excluded") {
    const auto* npc=bench.questNpc;
    if(npc==nullptr || (!npc->isPlayer() && npc->isHuman())) {arenaAbort("guard exclusion needs hero or non-human");return;}
    const auto& q=npc->ragdollQuestReturnState();
    event("immortal_guard_excluded","\"case\":"+jsonStr(bench.questCase)+",\"human\":"+(npc->isHuman()?"true":"false")+
      ",\"hero\":"+(npc->isPlayer()?"true":"false")+",\"monster\":"+(npc->isMonster()?"true":"false")+
      ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+
      ",\"returns\":"+std::to_string(npc->ragdollQuestReturnCount()));
    if(q.hasAnchor || q.armed || npc->ragdollQuestReturnCount()!=0) arenaAbort("excluded actor acquired return protection");
    }
  else if(c=="guard" && a.size()==2 && a[1]=="hazard") {
    bench.questWait=true;bench.questDeadline=bench.time+10000;
    }
  else if(c=="guard" && a.size()==3 && a[1]=="checkpoint") {
    const auto* npc=bench.questNpc;if(npc==nullptr) {arenaAbort("guard checkpoint lost actor");return;}
    const auto& q=npc->ragdollQuestReturnState();
    event("immortal_guard_checkpoint","\"case\":"+jsonStr(bench.questCase)+",\"label\":"+jsonStr(a[2])+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"angle\":"+ragdollNumber(q.angle)+
      ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+",\"hazards\":"+std::to_string(q.hazards));
    bench.questLastReturns=npc->ragdollQuestReturnCount();
    }
  else if(c=="guard" && a.size()==3 && a[1]=="wait-standing") {
    if(bench.questNpc==nullptr) {arenaAbort("guard standing wait lost actor");return;}
    // Let the actual get-up/draw animation finish while preventing a new combat action.
    bench.questNpc->setExternalControl(true);
    bench.questStandingWait=true;bench.questDeadline=bench.time+uint64_t(num(a,2,5000));
    }
  else if(c=="guard" && a.size()==2 && a[1]=="finish") {
    const auto* npc=bench.questNpc;
    if(npc==nullptr) {arenaAbort("guard finish lost actor");return;}
    event("immortal_guard_end","\"case\":"+jsonStr(bench.questCase)+",\"position_cm\":"+ragdollVector(npc->position())+
      ",\"stable_cm\":"+ragdollVector(bench.questStable)+",\"returns\":"+std::to_string(npc->ragdollQuestReturnCount())+
      ",\"drop_observed\":"+(bench.questSawDrop?"true":"false")+",\"water_observed\":"+(bench.questSawWater?"true":"false")+
      ",\"dead\":"+(npc->isDead()?"true":"false")+",\"unconscious\":"+(npc->isUnconscious()?"true":"false")+
      ",\"standing\":"+(npc->isStanding()?"true":"false")+",\"armed\":"+(npc->ragdollQuestReturnState().armed?"true":"false")+
      ",\"parts\":"+std::to_string(npc->ragdollBody()?npc->ragdollBody()->partCount():0));
    }
  else if(c=="guard" && a.size()==2 && a[1]=="cleanup") {
    auto* npc=bench.questNpc;
    if(npc==nullptr) {arenaAbort("guard cleanup lost actor");return;}
    bench.questNpc=nullptr;
    if(bench.subject==npc) exec("watch player",w,pl,ctrl);
    if(npc!=&pl) w.removeNpc(*npc); else npc->setExternalControl(false);
    }
  else if(c=="waitragdoll") {
    Npc* npc=a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    const std::string state=a.size()>2 ? a[2] : "";
    const float timeout=num(a,3,30000);
    if(npc==nullptr || a.size()<3 || a.size()>4 || !std::isfinite(timeout) || timeout<=0 ||
       (state!="sleep" && state!="recovery" && state!="getup")) {
      say("waitragdoll failed: expected <npc> sleep|recovery|getup [timeout ms]");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    bench.waitRagdollNpc=npc; bench.waitRagdollState=state;
    bench.waitRagdollEnd=bench.time+uint64_t(std::min(timeout,300000.f));
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
  else if(c=="ragdoll" && a.size()==4 && a[1]=="queued-standup") {
    Npc* npc=findNpc(w,pl,a[2]);
    const float delay=num(a,3,0);
    if(npc==nullptr || npc->isDead() || !npc->isUnconscious() || npc->ragdollBody()==nullptr ||
       !std::isfinite(delay) || delay<1 || delay>30000 || std::floor(delay)!=delay) {
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    npc->setExternalControl(false);
    // Keep ZS_Unconscious: ending it would let prepareRagdoll auto-recover before the queue.
    npc->clearAiQueue();
    npc->aiPush(AiQueue::aiWait(uint64_t(delay)));
    npc->aiPush(AiQueue::aiStandup());
    event("ragdoll_queued_standup","\"actor\":"+jsonStr(TestBench::who(npc))+
          ",\"actor_id\":"+std::to_string(w.npcId(npc))+",\"t_game_ms\":"+std::to_string(w.tickCount())+
          ",\"delay_ms\":"+std::to_string(uint64_t(delay))+
          ",\"control\":\"normal AI_Wait then AI_StandUp queue; no direct recovery\"");
    say("ragdoll queued-standup "+symName(w,npc));
    }
  else if(c=="ragdoll" && a.size()==3 && a[1]=="remove") {
    Npc* npc=findNpc(w,pl,a[2]);
    if(npc==nullptr || npc==&pl || npc->ragdollBody()==nullptr) {
      say("ragdoll remove failed: expected a physical NPC corpse");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return;
      }
    const auto parts=npc->ragdollBody()->partCount();
    const auto before=w.physic()->physicsStats().bodies;
    w.removeNpc(*npc);
    const auto after=w.physic()->physicsStats().bodies;
    const bool removed=npc->ragdollBody()==nullptr && before>=parts && after==before-parts;
    event("ragdoll_remove","\"parts\":"+std::to_string(parts)+",\"bodies_before\":"+
          std::to_string(before)+",\"bodies_after\":"+std::to_string(after)+",\"matched\":"+(removed ? "true" : "false"));
    if(!removed)
      event("ragdoll_error","\"reason\":\"removing NPC retained native ragdoll parts\"");
    if(bench.questNpc==npc) bench.questNpc=nullptr;
    bench.crew.erase(std::remove(bench.crew.begin(),bench.crew.end(),npc),bench.crew.end());
    if(bench.waitRagdollNpc==npc) bench.waitRagdollNpc=nullptr;
    if(bench.subject==npc) exec("watch player",w,pl,ctrl);
    say("ragdoll remove "+std::to_string(parts)+" parts");
    }
  else if(c=="ragdoll" && a.size()==2 && a[1]=="arena") {
    if(!chooseRagdollArena(w,pl)) arenaAbort("no geometry-valid twenty-actor arena");
    else exec("clear 2500",w,pl,ctrl);
    }
  else if(c=="ragdoll" && a.size()==2 && a[1]=="frame20") {
    if(!frameRagdollArena()) arenaAbort("twenty loaded models required before framing arena");
    }
  else if(c=="ragdoll" && a.size()==2 && a[1]=="awake20") {
    if(bench.crew.size()!=20 || std::any_of(bench.crew.begin(),bench.crew.end(),[](const auto* npc) {
         return npc->ragdollBody()==nullptr || npc->ragdollBody()->partCount()!=24;
         })) { arenaAbort("awake20 requires twenty full physical humans"); return; }
    for(auto* npc:bench.crew)
      const_cast<Ragdoll*>(npc->ragdollBody())->setAllowSleeping(false);
    event("ragdoll_benchmark_sleep_policy","\"actors\":20,\"allow_sleeping\":false,\"scope\":\"benchmark ACTIVE only; normal gameplay unchanged\"");
    }
  else if(c=="ragdoll" && a.size()==4 && a[1]=="birth-track") {
    bench.birthActor=findNpc(w,pl,a[2]); bench.birthLabel=a[3]; bench.birthHistory.clear();
    if(bench.birthActor==nullptr || bench.birthActor->isDown()) arenaAbort("birth-track requires a live actor");
    }
  else if(c=="ragdoll" && a.size()==5 && a[1]=="birth-down") {
    auto* npc=findNpc(w,pl,a[2]);
    if(npc==nullptr || npc!=bench.birthActor || bench.birthHistory.size()<4 || (a[3]!="dead" && a[3]!="ko")) {
      arenaAbort("birth-down requires four ordinary live history samples and a matching actor"); return;
      }
    const auto current=bench.birthHistory.back(),previous=bench.birthHistory[bench.birthHistory.size()-2];
    const auto elapsed=current.tick-previous.tick;
    if(elapsed==0 || elapsed>250 || current.tick!=w.tickCount()) { arenaAbort("birth history must end at the actual current world tick"); return; }
    const auto velocity=(current.position-previous.position)*(1000.f/float(elapsed));
    std::string history="[";
    for(const auto& sample:bench.birthHistory) {
      if(history.size()>1) history+=',';
      history+="{\"t_game_ms\":"+std::to_string(sample.tick)+",\"in_air\":"+(sample.inAir ? "true" : "false")+
               ",\"position_cm\":"+ragdollVector(sample.position)+"}";
      }
    history+=']'; bench.birthActor=nullptr;
    npc->setLastHitBy(&pl);
    if(!npc->setRagdollDown(a[3]=="dead")) { arenaAbort("actual production down path refused birth oracle"); return; }
    npc->prepareRagdoll();
    const auto* body=npc->ragdollBody();
    if(body==nullptr || body->partCount()!=24) { arenaAbort("birth oracle requires a complete native human"); return; }
    const auto state=body->state();
    std::string velocities="[";
    for(const auto& part:state.parts) { if(velocities.size()>1) velocities+=','; velocities+=ragdollVector(part.linearVelocity); }
    velocities+=']';
    event("ragdoll_birth","\"label\":"+jsonStr(a[4])+",\"actor_id\":"+std::to_string(w.npcId(npc))+
          ",\"t_game_ms\":"+std::to_string(w.tickCount())+",\"mode\":"+jsonStr(a[4]=="stationary" ? "stationary" : a[3])+
          ",\"history\":"+history+",\"expected_velocity_cm_s\":"+ragdollVector(velocity)+",\"parts\":24,\"velocities_cm_s\":"+
          velocities+",\"birth_pelvis_y_cm\":"+std::to_string(body->pelvisTransform().data()[13]));
    ragdollSessionArchive({"ragdoll","session-snapshot",a[2],a[4]},w,pl,0);
    bench.birthContinuation=npc; bench.birthContinuationLabel=a[4]; bench.birthTick=w.tickCount();
    }
  else if(c=="ragdoll" && a.size()==3 && (a[1]=="verify20" || a[1]=="coverage")) {
    const auto& mode=a[2];
    if((mode!="disabled" && mode!="active" && mode!="sleeping") || !arenaFullSchema(w,mode)) {
      arenaAbort("twenty complete visible model/schema precondition failed"); return;
      }
    if(a[1]=="coverage") {
      if(ragdollPerf) { arenaAbort("untimed coverage requires OPENGOTHIC_RAGDOLL_PERF=0"); return; }
      bench.ragdollCoverageMode=mode; bench.ragdollCoverageFrames=0; bench.ragdollCoverageActive=true;
      } else event("ragdoll_arena_verified","\"mode\":"+jsonStr(mode)+",\"actors\":20,\"visible_meshes\":20");
    }
  else if(c=="ragdoll" && a.size()==3 && a[1]=="churn" && (a[2]=="on" || a[2]=="off")) {
    bench.ragdollChurn=a[2]=="on";
    bench.ragdollPulseFrame=0;
    say("ragdoll churn "+a[2]);
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
      Vec3 at=bench.ragdollArenaPlaces.size()==20 ? bench.ragdollArenaPlaces[size_t(i)] :
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
    if(bench.ragdollArenaPlaces.size()==20 && bench.crew.size()!=20) { arenaAbort("arena insertion must retain all twenty actors"); return; }
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
  else if(c=="wait") {
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
      release(ctrl,k);
      bench.holds.erase(std::remove_if(bench.holds.begin(),bench.holds.end(),[k](const Hold& h){ return h.a==k; }),bench.holds.end());
      }
    else if(c=="tap") {
      press(ctrl,k);
      bench.taps.push_back(k);
      }
    else {
      press(ctrl,k);
      bench.holds.push_back({k,bench.time+uint64_t(num(a,2,1000))});
      }
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
      bench.smValid = false;  // the next shot starts where it is, without moving there
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
  else if(c=="hp") {
    // hp <npc> [full]: hit points of the 3 nearest npcs of that script symbol (sheep are all over the world),
    // and how far (did the arrow hit); full: the nearest gets them all back first
    const std::string name = a.size()>1 ? a[1] : "";
    const size_t      id   = w.script().findSymbolIndex(name);
    std::vector<std::pair<float,Npc*>> list;
    for(uint32_t i=0; i<w.npcCount(); ++i)
      if(Npc* n = w.npcById(i); n!=nullptr && n->instanceSymbol()==id)
        list.emplace_back((n->position()-pl.position()).length(),n);
    std::sort(list.begin(),list.end(),[](auto& x, auto& y) { return x.first<y.first; });
    if(a.size()>2 && a[2]=="full" && !list.empty() && !list[0].second->isDown()) {
      // the nearest gets its hit points back
      Npc& n = *list[0].second;
      n.changeAttribute(ATR_HITPOINTS,n.attribute(ATR_HITPOINTSMAX)-n.attribute(ATR_HITPOINTS),false);
      }
    std::string out = "hp " + name + ":";
    for(size_t i=0; i<list.size() && i<3; ++i)
      out += " " + std::to_string(list[i].second->attribute(ATR_HITPOINTS)) + (list[i].second->isDead() ? " dead" : "") +
             " at " + std::to_string(int(list[i].first/100.f)) + " m;";
    say(out);
    }
  else if(c=="give" || c=="equip") {
    // give <item> [count]: into the hero's inventory; equip <item>: give one and put it on (a bow, a rune)
    const std::string name = a.size()>1 ? a[1] : "";
    const size_t      id   = w.script().findSymbolIndex(name);
    if(id==size_t(-1)) {
      say(c + ": no item " + name);
      return;
      }
    pl.addItem(id,size_t(c=="give" ? num(a,2,1) : 1));
    if(c=="equip")
      pl.useItem(id,Item::NSLOT,true); // whatever the hero's strength
    say(c + " " + name);
    }
  else if(c=="disarm") {
    // disarm: deterministic bench setup, with no weapon-key toggle or holstering animation.
    ctrl.clearInput();
    bench.holds.clear();
    bench.taps.clear();
    pl.endManualAim(false);
    pl.closeWeapon(true);
    pl.clearState(true);
    pl.clearAiQueue();
    pl.clearSpeed();
    pl.stopAnim("");
    say("disarm ws=" + std::to_string(int(pl.weaponState())));
    }
  else if(c=="draw") {
    // draw bow: draw the equipped bow/crossbow; calling it again never puts the weapon away.
    if(a.size()!=2 || a[1]!="bow") {
      say("draw: expected draw bow");
      return;
      }
    const bool ok = pl.currentRangedWeapon()!=nullptr && pl.drawWeaponBow();
    say(std::string("draw bow ") + (ok ? "ok" : "failed") + " ws=" + std::to_string(int(pl.weaponState())));
    }
  else if(c=="put") {
    // put <npc|nearest> <dist> <angle> [dy]: the npc stands there from the hero (angle from the hero's
    // facing, 0 in front), on the ground, frozen, facing the hero
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("put: no npc " + line);
      return;
      }
    const float d   = num(a,2,500);
    const float an  = (pl.rotation()+num(a,3,0))*float(M_PI)/180.f;
    Vec3        pos = pl.position() + Vec3(std::cos(an),0,std::sin(an))*d;
    // the ground there may be far above or below (a slope): the first one from above, by short rays
    // (a long one misses the landscape)
    DynamicWorld::RayLandResult r;
    for(float y=pos.y+num(a,4,0)+1500; y>pos.y-1500 && !r.hasCol; y-=400)
      r = w.physic()->ray({pos.x,y,pos.z},{pos.x,y-450,pos.z});
    if(r.hasCol)
      pos.y = r.v.y;
    n->setInteraction(nullptr,true);
    n->clearAiQueue();
    n->clearState(false);
    n->setExternalControl(true);
    n->setPosition(pos);
    const Vec3 dir = pl.position()-pos;
    n->setDirection(Vec3(dir.x,0,dir.z));
    n->updateTransform();
    char buf[200];
    std::snprintf(buf,sizeof(buf),"put %s at %.0f %.0f %.0f (%.0f above the hero%s)",symName(w,n).c_str(),pos.x,pos.y,pos.z,
                  pos.y-pl.position().y,r.hasCol ? "" : ", no ground found");
    say(buf);
    }
  else if(c=="pin") {
    // pin <npc|nearest> <dist> <angle> <height> | pin off: the npc hangs there from the hero (angle from
    // the hero's facing, height above the hero's feet), frozen, every frame: a target above or below
    if(a.size()>1 && a[1]=="off") {
      bench.pinned = nullptr;
      say("pin off");
      return;
      }
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("pin: no npc " + line);
      return;
      }
    n->setInteraction(nullptr,true);
    n->clearAiQueue();
    n->clearState(false);
    n->setExternalControl(true);
    const float an = (pl.rotation()+num(a,3,0))*float(M_PI)/180.f;
    bench.pinned   = n;
    bench.pinAt    = pl.position() + Vec3(std::cos(an)*num(a,2,800),num(a,4,0),std::sin(an)*num(a,2,800));
    char buf[200];
    std::snprintf(buf,sizeof(buf),"pin %s at %.0f %.0f %.0f",symName(w,n).c_str(),bench.pinAt.x,bench.pinAt.y,bench.pinAt.z);
    say(buf);
    }
  else if(c=="npcgo") {
    // npcgo <npc|nearest> <waypoint> [walk|run]: the npc goes there (AI path), like a script sends it
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    auto wp = a.size()>2 ? w.findPoint(a[2]) : nullptr;
    if(n==nullptr || n==&pl || wp==nullptr) {
      say("npcgo: no npc or waypoint " + line);
      return;
      }
    n->setWalkMode(a.size()>3 && a[3]=="walk" ? WalkBit::WM_Walk : WalkBit::WM_Run);
    n->aiPush(AiQueue::aiGoToPoint(*wp));
    say("npcgo " + symName(w,n) + " " + a[2]);
    }
  else if(c=="manual") {
    // manual on|off: the hero aims a bow by hand (game/archery)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Archery::setEnabled(m=="on"); else
      say("unknown manual command: " + line);
    }
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
  else if(c=="bowdraw") {
    // bowdraw on|off: the hand draws the string as far as the bow is drawn, the reload holds the aim
    // (graphics/mesh/bowdraw)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      BowDraw::setEnabled(m=="on"); else
      say("unknown bowdraw command: " + line);
    }
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
  else if(c=="aimwalk") {
    // aimwalk on|off: the legs step under the aim (graphics/mesh/aimwalk)
    AimWalk::setEnabled(a.size()<2 || a[1]!="off");
    }
  else if(c=="bowgeom") {
    // bowgeom: the bow and the bow hand in the arrow's frame, as posed now
    say(pl.bowDump());
    }
  else if(c=="stats") {
    // stats [npc <npc>] [str <n>] [dex <n>] [bow <n>] [cbow <n>] [hp <n>] [hpmax <n>] [talent auto|0|1|2]:
    // the hero's (an npc's) strength, dexterity, hit chances (the skill of the manual aim), hit points;
    // talent: the animations of the trained (G2 sets level 1 at 30%, 2 at 60%: _BOWT1, _BOWT2), auto as
    // the hit chances say
    Npc*   np = &pl;
    size_t i0 = 1;
    if(a.size()>1 && a[1]=="npc") {
      np = a.size()>2 ? findNpc(w,pl,a[2]) : nullptr;
      if(np==nullptr) {
        say("stats: no npc " + line);
        return;
        }
      i0 = 3;
      }
    Npc&   n    = *np;
    auto&  h    = n.handle();
    bool   more = &n!=&pl;
    for(size_t i=i0; i+1<a.size(); i+=2) {
      if(a[i]=="talent") {
        auto level = [&](int32_t chance) {
          if(a[i+1]=="auto")
            return chance>=60 ? 2 : (chance>=30 ? 1 : 0);
          return int(num(a,i+1,0));
          };
        n.setTalentSkill(TALENT_BOW,     level(h.hitchance[TALENT_BOW]));
        n.setTalentSkill(TALENT_CROSSBOW,level(h.hitchance[TALENT_CROSSBOW]));
        more = true;
        continue;
        }
      const int v = int(num(a,i+1,0));
      if(a[i]=="str")
        h.attribute[ATR_STRENGTH] = v;
      else if(a[i]=="dex")
        h.attribute[ATR_DEXTERITY] = v;
      else if(a[i]=="bow")
        h.hitchance[TALENT_BOW] = v;
      else if(a[i]=="cbow")
        h.hitchance[TALENT_CROSSBOW] = v;
      else if(a[i]=="hp")
        h.attribute[ATR_HITPOINTS] = v;
      else if(a[i]=="hpmax")
        h.attribute[ATR_HITPOINTSMAX] = v;
      else
        say("stats: unknown " + a[i]);
      }
    const auto  wp = n.currentRangedWeapon();
    const auto  st = Archery::stats(n,wp);
    char buf[440];
    if(more) {
      // whose, the reload as long as the animation is (the trained ones' is shorter), the hit points
      const bool cbow = wp!=nullptr && wp->isCrossbow();
      std::snprintf(buf,sizeof(buf),"stats of %s: talent bow %d cbow %d, reload %.0f ms, hp %d of %d, point protection %d",
                    symName(w,&n).c_str(),n.talentSkill(TALENT_BOW),n.talentSkill(TALENT_CROSSBOW),
                    double(n.animTime(Archery::reloadAnim(cbow))),n.attribute(ATR_HITPOINTS),n.attribute(ATR_HITPOINTSMAX),
                    n.protection(PROT_POINT));
      say(buf);
      }
    // an npc's: by name (the report takes `stats str` for the hero's)
    const std::string who = &n!=&pl ? " npc " + symName(w,&n) : "";
    std::snprintf(buf,sizeof(buf),"stats%s str %d dex %d bow %d cbow %d, weapon damage %d: skill %.2f strength %.2f sway %.2f deg settle %.2f s breath %.1f s draw %.2f s hold %.1f s speed %.1f m/s cone %.2f deg reload x%.2f",
                  who.c_str(),n.attribute(ATR_STRENGTH),n.attribute(ATR_DEXTERITY),n.hitChance(TALENT_BOW),n.hitChance(TALENT_CROSSBOW),
                  wp!=nullptr ? int(wp->handle().damage_total) : -1,
                  double(st.skill),double(st.strength),double(st.sway*180.f/float(M_PI)),double(st.settle),double(st.breath),
                  double(st.drawTime),double(std::min(st.holdLimit,999.f)),double(st.speed*10.f),double(st.cone*180.f/float(M_PI)),
                  double(st.reload));
    say(buf);
    }
  else if(c=="seed") {
    // seed <n>: the random numbers start over: the scripts' (the dice, the release), the bench's, the
    // hero's sway
    const uint32_t s = uint32_t(num(a,1,1));
    w.script().setSeed(s);
    bench.lcg = s*2654435761u + 1u;
    bench.monsterCharacterSeed=s;
    pl.archer().reseed(s);
    say("seed " + std::to_string(s));
    }
  else if(c=="bodyarena") {
    setupArena(w,pl);
    }
  else if(c=="bodylive") {
    // Live engine commands only: no controller state, watch list, policy, or lease is modified.
    if(a.size()==2 && a[1]=="off") {
      stopBodyLive();
      event("body_live","\"enabled\":false");
      }
    else if(a.size()==2 && a[1]=="on" && !BodyArena::enabled() && !BodyReplay::active()) {
      bench.bodyLive = true;
      BodyController::setSink([](std::string_view row) {
        if(bench.bodyLive)
          event("body_live_control","\"control\":"+std::string(row));
        });
      event("body_live","\"enabled\":true");
      }
    else if(a.size()>=3 && a.size()<=4 && a[1]=="wait" && std::filesystem::path(a[2]).is_absolute() && num(a,3,120000)>0) {
      bench.bodyLiveWait=a[2];
      bench.bodyLiveDeadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(int64_t(num(a,3,120000)));
      event("body_live_wait","\"path\":"+jsonStr(bench.bodyLiveWait));
      }
    else {
      event("body_live_error","\"reason\":\"expected bodylive on/off or wait /absolute/path [timeout ms]; live capture requires arena/replay off\"");
      bench.pc=bench.prog.size();
      }
    }
  else if(c=="bodyreplay") {
    // bodyreplay <file> [tail ms 3000]: a recorded lockstep arena episode, replayed for the camera
    // (game/bodyreplay.h); put it in place of `bodyarena`, the following `wait` ends with the replay
    try {
      if(bench.bodyLive)
        throw std::runtime_error("bodylive must be off before bodyreplay");
      BodyReplay::load(a.size()>1 ? a[1] : "",outDir,uint64_t(num(a,2,3000)));
      setupArena(w,pl);
      }
    catch(const std::exception& error) {
      event("body_replay_error","\"reason\":"+jsonStr(error.what()));
      say("bodyreplay: "+std::string(error.what()));
      BodyReplay::stop("error");
      bench.pc=bench.prog.size();
      }
    }
  else if(c=="bodyobserve" || c=="bodyapi") {
    const bool bodyApi = c=="bodyapi";
    const size_t actorCount = bodyApi ? 2 : 4;
    // Four mortal native fighters. High HP keeps real damage/defence alive for the ten-minute
    // observation test; no healing, invulnerability, held input or policy owns their bodies.
    const auto fail = [&](const std::string& reason) {
      event(bodyApi ? "body_api_error" : "body_observe_error","\"reason\":"+jsonStr(reason));
      say(c+": "+reason);
      bench.pc = bench.prog.size();
      };
    const char* names[] = {"BDT_1003_BANDIT_M","PAL_255_RITTER","ItMw_1h_Common_01","ItAr_Leather_L",
                           "ZS_ATTACK","FAI_HUMAN_MASTER","AIV_ATTACKREASON","AR_KILL"};
    size_t ids[std::size(names)] = {};
    for(size_t i=0;i<std::size(names);++i) {
      ids[i] = w.script().findSymbolIndex(names[i]);
      if(ids[i]==size_t(-1)) { fail(std::string("missing symbol ")+names[i]); return; }
      }
    const int reason = w.script().findSymbol(names[6])->get_int();
    if(reason<0 || size_t(reason)>=zenkit::INpc::aivar_count) { fail("invalid attack reason"); return; }
    for(auto* actor:bench.observeActors)
      w.removeNpc(*actor);
    bench.observeActors.clear();
    const uint32_t seed = uint32_t(num(a,1,17));
    const uint64_t duration = uint64_t(std::clamp(num(a,2,600000),1000.f,600000.f));
    w.script().setSeed(seed);
    const float an = pl.rotation()*float(M_PI)/180.f;
    const Vec3 forward(std::cos(an),0,std::sin(an)), side(-forward.z,0,forward.x);
    Vec3 positions[4];
    for(size_t i=0;i<actorCount;++i) {
      auto p = pl.position()+forward*1200.f+side*((i<2 ? -550.f : 550.f)+(i%2==0 ? -150.f : 150.f));
      const float y = groundY(w,p.x,p.z,p.y);
      if(!std::isfinite(y)) { fail("arena ground not found"); return; }
      p.y = y;
      positions[i] = p;
      }
    for(size_t i=0;i<actorCount;++i) {
      Npc* actor = w.addNpc(ids[i%2],positions[i]);
      if(actor==nullptr) { fail("NPC creation failed"); return; }
      bench.observeActors.push_back(actor);
      actor->setExternalControl(true);
      actor->setInteraction(nullptr,true);
      actor->clearAiQueue(); actor->clearState(true); actor->clearGoTo();
      std::vector<size_t> equipped;
      for(auto it=actor->inventory().iterator(Inventory::T_Inventory);it.isValid();++it)
        if(it.isEquipped()) equipped.push_back(it->clsId());
      for(auto item:equipped) actor->unequipItem(item);
      actor->clearInventory();
      actor->addItem(ids[2],1); actor->useItem(ids[2],Item::NSLOT,true);
      actor->addItem(ids[3],1); actor->useItem(ids[3],Item::NSLOT,true);
      auto& h = actor->handle();
      h.flags = zenkit::NpcFlag(uint32_t(h.flags)&~uint32_t(zenkit::NpcFlag::IMMORTAL));
      h.attribute[ATR_HITPOINTS] = h.attribute[ATR_HITPOINTSMAX] = 1000000;
      h.attribute[ATR_STRENGTH] = h.attribute[ATR_DEXTERITY] = 60;
      h.hitchance[TALENT_1H] = 60; actor->setTalentSkill(TALENT_1H,2);
      h.protection[PROT_EDGE] = h.protection[PROT_BLUNT] = 30;
      h.guild = i%2==0 ? GIL_BDT : GIL_PAL;
      h.fight_tactic = w.script().findSymbol(names[5])->get_int();
      h.aivar[size_t(reason)] = w.script().findSymbol(names[7])->get_int();
      actor->setPosition(positions[i]); actor->setDirection(positions[i^1]-positions[i]);
      actor->updateTransform();
      if(actor->currentMeleeWeapon()==nullptr || actor->currentRangedWeapon()!=nullptr || actor->isImmortal()) {
        fail("native melee setup validation failed"); return;
        }
      }
    w.script().setSeed(seed);
    std::string actors = "[";
    for(size_t i=0;i<actorCount;++i) {
      auto* actor = bench.observeActors[i];
      auto* target = bench.observeActors[i^1];
      actor->setExternalControl(false); actor->setTarget(target);
      actor->aiPush(AiQueue::aiStartState(ScriptFn(ids[4]),0,target,nullptr,""));
      TestBench::bodyEvent(*actor,target,"observation_actor_mapping",w.tickCount(),
                          "\"observation_entity_id\":"+std::to_string(BodyObservations::entityId(*actor)));
      if(i>0) actors += ",";
      actors += "{\"entity_id\":"+std::to_string(BodyObservations::entityId(*actor))+
                ",\"symbol\":"+jsonStr(names[i%2])+",\"hp\":1000000,\"hp_max\":1000000,\"immortal\":false}";
      }
    bench.observeFrom = w.tickCount(); bench.observeEnd = bench.observeFrom+duration;
    // Stage 3 explicitly forbids external/frozen actors. Stage 4 intentionally tests those
    // transitions and owns its assertions in the API suite, while retaining the same real setup.
    bench.observeActive = !bodyApi; bench.observeVisibleSaved = false;
    event(bodyApi ? "body_api_setup" : "body_observe_setup","\"seed\":"+std::to_string(seed)+",\"t_game_ms\":"+
          std::to_string(bench.observeFrom)+",\"duration_ms\":"+std::to_string(duration)+
          ",\"ownership\":\"native\",\"actors\":"+actors+"]");
    }
  else if(c=="bodyduel") {
    // bodyduel <seed> [timeout ms]: native human melee reference, both participants mortal. The
    // nearby player is only an observer. These constants are versioned in evals/npc-body/melee-v1.json.
    auto fail = [&](const std::string& reason) {
      say("bodyduel: setup_failed " + reason);
      event("body_duel_error","\"reason\":" + jsonStr(reason));
      bench.pc = bench.prog.size();
      };
    if(!TestBench::bodyTracing()) {
      fail("OPENGOTHIC_BODY_TRACE=1 is required");
      return;
      }
    const char* names[] = {"BDT_1003_BANDIT_M", "PAL_255_RITTER", "ItMw_1h_Common_01", "ItAr_Leather_L",
                           "ZS_ATTACK", "FAI_HUMAN_MASTER", "AIV_ATTACKREASON", "AR_KILL"};
    size_t ids[std::size(names)] = {};
    for(size_t i=0; i<std::size(names); ++i) {
      ids[i] = w.script().findSymbolIndex(names[i]);
      if(ids[i]==size_t(-1)) {
        fail(std::string("missing symbol ") + names[i]);
        return;
        }
      }
    const int reason = w.script().findSymbol(names[6])->get_int();
    if(reason<0 || size_t(reason)>=zenkit::INpc::aivar_count) {
      fail("invalid AIV_ATTACKREASON index");
      return;
      }
    for(Npc* n:{bench.duelA,bench.duelB}) {
      if(n==nullptr)
        continue;
      if(bench.subject==n)
        bench.subject = nullptr;
      w.removeNpc(*n);
      }
    bench.duelA = bench.duelB = nullptr;
    bench.duelWaiting = false;
    const uint32_t seed = uint32_t(num(a,1,17));
    const uint64_t timeout = uint64_t(std::clamp(num(a,2,60000),1000.f,60000.f));
    w.script().setSeed(seed); // constructors/equipment callbacks get the same RNG start too
    const float an = pl.rotation()*float(M_PI)/180.f;
    const Vec3 forward(std::cos(an),0,std::sin(an));
    const Vec3 side(-forward.z,0,forward.x);
    const Vec3 center = pl.position()+forward*1200.f;
    Vec3 pos[2] = {center-side*150.f,center+side*150.f};
    for(auto& p:pos) {
      DynamicWorld::RayLandResult ground;
      for(float y=p.y+1500; y>p.y-1500 && !ground.hasCol; y-=400)
        ground = w.physic()->ray({p.x,y,p.z},{p.x,y-450,p.z});
      if(!ground.hasCol) {
        fail("arena ground not found");
        return;
        }
      p.y = ground.v.y;
      }
    bench.duelA = w.addNpc(ids[0],pos[0]);
    bench.duelB = w.addNpc(ids[1],pos[1]);
    if(bench.duelA==nullptr || bench.duelB==nullptr) {
      fail("NPC creation failed");
      return;
      }
    Npc* fighters[] = {bench.duelA,bench.duelB};
    for(size_t i=0; i<2; ++i) {
      Npc& n = *fighters[i];
      n.setInteraction(nullptr,true);
      n.clearAiQueue();
      n.clearState(true);
      n.clearGoTo();
      n.setExternalControl(true);
      // Inventory::clear deliberately preserves equipped items. Unequip first, including passive
      // jewellery modifiers, so an archer source cannot keep its bow or inherited armour bonuses.
      std::vector<size_t> equipped;
      for(auto it=n.inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
        if(it.isEquipped())
          equipped.push_back(it->clsId());
      for(auto cls:equipped)
        n.unequipItem(cls);
      n.clearInventory();
      n.addItem(ids[2],1);
      n.useItem(ids[2],Item::NSLOT,true);
      n.addItem(ids[3],1);
      n.useItem(ids[3],Item::NSLOT,true);
      auto& h = n.handle();
      h.flags = zenkit::NpcFlag(uint32_t(h.flags) & ~uint32_t(zenkit::NpcFlag::IMMORTAL));
      h.attribute[ATR_STRENGTH] = 60;
      h.attribute[ATR_DEXTERITY] = 60;
      h.attribute[ATR_HITPOINTSMAX] = 300;
      h.attribute[ATR_HITPOINTS] = 300;
      h.hitchance[TALENT_1H] = 60;
      n.setTalentSkill(TALENT_1H,2);
      h.protection[PROT_EDGE] = 30;
      h.protection[PROT_BLUNT] = 30;
      h.guild = i==0 ? GIL_BDT : GIL_PAL;
      h.fight_tactic = w.script().findSymbol(names[5])->get_int();
      h.aivar[size_t(reason)] = w.script().findSymbol(names[7])->get_int();
      n.setPosition(pos[i]);
      n.setDirection(pos[1-i]-pos[i]);
      n.updateTransform();
      auto sword = n.currentMeleeWeapon();
      if(sword==nullptr || n.currentRangedWeapon()!=nullptr || n.currentArmor()==nullptr || n.isImmortal()) {
        fail("melee-only equipment or mortality validation failed");
        return;
        }
      char fields[1000];
      std::snprintf(fields,sizeof(fields),
                    "\"side\":%zu,\"seed\":%u,\"hp\":%d,\"hpmax\":%d,\"str\":%d,\"dex\":%d,"
                    "\"onehand\":%d,\"onehand_talent\":%d,\"edge_protection\":%d,\"blunt_protection\":%d,"
                    "\"fight_tactic\":%d,\"guild\":%d,\"weapon_damage\":%d,\"weapon_length\":%d,"
                    "\"immortal\":false,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"observer_distance_cm\":%.3f",
                    i,seed,n.attribute(ATR_HITPOINTS),n.attribute(ATR_HITPOINTSMAX),n.attribute(ATR_STRENGTH),
                    n.attribute(ATR_DEXTERITY),n.hitChance(TALENT_1H),n.talentSkill(TALENT_1H),
                    n.protection(PROT_EDGE),n.protection(PROT_BLUNT),h.fight_tactic,h.guild,
                    sword->handle().damage_total,sword->handle().range,double(pos[i].x),double(pos[i].y),double(pos[i].z),
                    double((pos[i]-pl.position()).length()));
      TestBench::bodyEvent(n,fighters[1-i],"actor_setup",w.tickCount(),std::string(fields) +
                           ",\"weapon\":"+jsonStr(names[2])+",\"armor\":"+jsonStr(names[3]));
      }
    // Separate the native fight RNG from constructor/equipment callback consumption. No scripted
    // attacks, strike/hit commands, invulnerability, held inputs or model take part in the duel.
    w.script().setSeed(seed);
    bench.lcg = seed*2654435761u+1u;
    bench.duelFrom = w.tickCount();
    bench.duelEnd = bench.duelFrom+timeout;
    if(!beginAnimationControl(seed,*fighters[0],*fighters[1])) {
      fail("cannot initialize requested block-variant control");
      return;
      }
    bench.duelWaiting = true;
    TestBench::bodyEvent(*fighters[0],fighters[1],"episode_start",w.tickCount(),
                         "\"seed\":"+std::to_string(seed)+",\"timeout_ms\":"+std::to_string(timeout)+
                         ",\"scenario\":\"melee-v1\",\"source\":\"native\"");
    for(size_t i=0; i<2; ++i) {
      fighters[i]->setExternalControl(false);
      fighters[i]->setTarget(fighters[1-i]);
      fighters[i]->aiPush(AiQueue::aiStartState(ScriptFn(ids[4]),0,fighters[1-i],nullptr,""));
      }
    say("bodyduel seed "+std::to_string(seed)+" timeout "+std::to_string(timeout));
    }
  else if(c=="bodyobserveprobe") {
    // A separately labelled privacy fixture: native geometry and visibility, deliberately frozen
    // participants. It is not a combat episode or evidence of autonomous policy behavior.
    const auto actors = TestBench::bodyObservationActors();
    const auto fail = [&](const std::string& reason) {
      event("body_observe_error","\"reason\":"+jsonStr(reason));
      bench.pc = bench.prog.size();
      };
    if(actors.size()<2 || bench.duelWaiting) { fail("a completed native setup is required"); return; }
    Npc& first = *actors[0]; Npc& second = *actors[1];
    const std::string mode = a.size()>1 ? a[1] : "visible";
    if(mode!="visible" && mode!="occluded" && mode!="hidden_move" && mode!="reappear") {
      fail("expected visible, occluded, hidden_move or reappear"); return;
      }
    bench.observeActive = false;
    for(auto* actor:actors) {
      actor->clearAiQueue(); actor->clearState(true); actor->clearGoTo(); actor->clearSpeed();
      actor->setExternalControl(true); actor->setAnim(Npc::Anim::Idle);
      }
    const auto place = [&](const Vec3& p0,const Vec3& p1) {
      first.setPosition(p0); second.setPosition(p1);
      BodyObservations::discontinuity(first); BodyObservations::discontinuity(second);
      first.setDirection(p1-p0); second.setDirection(p0-p1);
      first.updateTransform(); second.updateTransform();
      };
    if(mode=="visible") {
      bench.observeVisibleA = first.position(); bench.observeVisibleB = second.position();
      bench.observeVisibleSaved = true;
      place(bench.observeVisibleA,bench.observeVisibleB);
      }
    else if(mode=="reappear") {
      if(!bench.observeVisibleSaved) { fail("visible fixture was not initialized"); return; }
      place(bench.observeVisibleA,bench.observeVisibleB);
      }
    else if(mode=="occluded") {
      std::vector<const WayPoint*> points;
      const auto origin = first.position();
      w.findWayPoint(origin,[&](const WayPoint& p) { points.push_back(&p); return false; });
      std::sort(points.begin(),points.end(),[&](const WayPoint* p0,const WayPoint* p1) {
        const auto d0 = (p0->pos-origin).quadLength(), d1 = (p1->pos-origin).quadLength();
        return d0!=d1 ? d0<d1 : p0->name<p1->name;
        });
      bool found = false; size_t attempts = 0;
      const float range = float(first.handle().senses_range);
      for(const auto* p0:points) {
        if(found || attempts>=8192) break;
        const float y0 = groundY(w,p0->pos.x,p0->pos.z,p0->pos.y);
        if(!std::isfinite(y0) || std::abs(y0-p0->pos.y)>120.f) continue;
        for(const auto* p1:points) {
          const float distance = (p1->pos-p0->pos).length();
          if(distance<200.f || distance>std::min(range-250.f,2500.f)) continue;
          const float y1 = groundY(w,p1->pos.x,p1->pos.z,p1->pos.y);
          if(!std::isfinite(y1) || std::abs(y1-p1->pos.y)>120.f) continue;
          ++attempts; place({p0->pos.x,y0,p0->pos.z},{p1->pos.x,y1,p1->pos.z});
          const auto center = w.physic()->ray(first.mapHeadBone(),second.collosionCenter());
          const auto head = w.physic()->ray(first.mapHeadBone(),second.mapHeadBone());
          if(center.hasCol && head.hasCol && center.hitFraction>0.01f && center.hitFraction<0.99f &&
             head.hitFraction>0.01f && head.hitFraction<0.99f && !first.canSeeNpc(second,true)) {
            found = true; break;
            }
          if(attempts>=8192) break;
          }
        }
      if(!found) { fail("bounded search found no real occlusion"); return; }
      }
    else {
      const auto p0 = first.position(), p1 = second.position();
      bool found = false;
      const Vec3 offsets[] = {{50,0,0},{-50,0,0},{0,0,50},{0,0,-50}};
      for(const auto& offset:offsets) {
        auto candidate = p1+offset;
        const float y = groundY(w,candidate.x,candidate.z,candidate.y);
        if(!std::isfinite(y)) continue;
        candidate.y = y; place(p0,candidate);
        if(!first.canSeeNpc(second,true)) { found = true; break; }
        }
      if(!found) { fail("hidden movement lost geometric occlusion"); return; }
      second.changeAttribute(ATR_HITPOINTS,-17,false);
      }
    first.setTarget(&second); second.setTarget(&first);
    const bool visible = first.canSeeNpc(second,false);
    const auto center = w.physic()->ray(first.mapHeadBone(),second.collosionCenter());
    const auto head = w.physic()->ray(first.mapHeadBone(),second.mapHeadBone());
    const bool hidden = mode=="occluded" || mode=="hidden_move";
    if(visible==hidden || (hidden && (!center.hasCol || !head.hasCol))) {
      fail("privacy fixture visibility disagrees with real rays"); return;
      }
    pl.setPosition(first.position()); pl.updateTransform();
    Gothic::inst().gameSession()->camera().reset(&pl);
    const auto pos = second.position();
    bench.observeProbeCase = mode;
    event("body_observe_probe","\"case\":"+jsonStr(mode)+",\"t_game_ms\":"+std::to_string(w.tickCount())+
          ",\"actor_id\":"+std::to_string(BodyObservations::entityId(first))+
          ",\"target_id\":"+std::to_string(BodyObservations::entityId(second))+
          ",\"visible\":"+(visible ? "true" : "false")+
          ",\"center_ray_hit\":"+(center.hasCol ? "true" : "false")+
          ",\"head_ray_hit\":"+(head.hasCol ? "true" : "false")+
          ",\"truth_self_hp\":"+std::to_string(first.attribute(ATR_HITPOINTS))+
          ",\"truth_self_yaw_rad\":"+std::to_string(first.rotationRad())+
          ",\"truth_self_position_cm\":["+std::to_string(first.position().x)+","+
          std::to_string(first.position().y)+","+std::to_string(first.position().z)+"]"+
          ",\"truth_target_hp\":"+std::to_string(second.attribute(ATR_HITPOINTS))+
          ",\"truth_target_position_cm\":["+std::to_string(pos.x)+","+std::to_string(pos.y)+","+std::to_string(pos.z)+"]");
    }
  else if(c=="bodyprobe") {
    // Separate stage 2 proof: real landscape/object collision rays and native sight/sound APIs.
    // A short bodyduel first provides the same real skeletons and 30 native simulation steps.
    auto fail = [&](const std::string& reason) {
      say("bodyprobe: failed " + reason);
      event("body_probe_error","\"reason\":"+jsonStr(reason));
      bench.pc = bench.prog.size();
      };
    if(!TestBench::bodyTracing() || bench.duelA==nullptr || bench.duelB==nullptr || bench.duelWaiting) {
      fail("a completed traced bodyduel setup is required");
      return;
      }
    Npc& first = *bench.duelA;
    Npc& second = *bench.duelB;
    const std::string mode = a.size()>1 ? a[1] : "visible";
    const auto vec = [](const Vec3& v) {
      return "["+std::to_string(v.x)+","+std::to_string(v.y)+","+std::to_string(v.z)+"]";
      };
    if(mode=="sound_end") {
      if(!bench.probeSound || bench.probeSoundDuration==0) {
        fail("no sound probe is active");
        return;
        }
      const bool finished = bench.probeSound->isFinished();
      const bool sourceVisible = w.sound()->canSeeSource(first.mapHeadBone());
      const uint64_t elapsed = w.tickCount()-bench.probeSoundStart;
      TestBench::bodyEvent(first,&second,"senses_sound_end",w.tickCount(),
                           "\"case\":"+jsonStr(bench.probeOccluded ? "occluded" : "visible")+
                           ",\"duration_ms\":"+std::to_string(bench.probeSoundDuration)+
                           ",\"elapsed_ms\":"+std::to_string(elapsed)+
                           ",\"finished\":"+(finished ? "true" : "false")+
                           ",\"source_visible_after_cleanup\":"+(sourceVisible ? "true" : "false"));
      bench.probeSound.reset();
      if(!finished || elapsed<bench.probeSoundDuration || sourceVisible)
        fail("sound duration or effect3d cleanup invariant failed");
      return;
      }
    if(mode!="visible" && mode!="occluded") {
      fail("expected visible, occluded or sound_end");
      return;
      }
    if(bench.probeSound) {
      fail("finish the preceding sound probe first");
      return;
      }
    const bool occluded = mode=="occluded";
    const auto nativePolicy = first.processPolicy();
    if(!occluded && nativePolicy!=NpcProcessPolicy::AiNormal) {
      fail("the native fighter was not processed with AiNormal");
      return;
      }
    for(Npc* n:{&first,&second}) {
      n->clearAiQueue();
      n->clearState(true);
      n->clearGoTo();
      n->clearSpeed();
      n->setTarget(nullptr);
      n->setExternalControl(true);
      n->setAnim(Npc::Anim::Idle);
      n->updateTransform();
      }
    const auto place = [&](const Vec3& p0, const Vec3& p1) {
      first.setPosition(p0);
      second.setPosition(p1);
      first.setDirection(p1-p0);
      second.setDirection(p0-p1);
      first.updateTransform();
      second.updateTransform();
      };
    std::string pointA = "native_arena", pointB = "native_arena";
    size_t attempts = 0;
    const float range = float(first.handle().senses_range);
    if(occluded) {
      // Both endpoints must be real ground waypoints, in range. This excludes a false result caused
      // only by FOV, LOD, distance or putting the source inside landscape geometry.
      std::vector<const WayPoint*> points;
      const Vec3 origin = first.position();
      w.findWayPoint(origin,[&](const WayPoint& wp) { points.push_back(&wp); return false; });
      std::sort(points.begin(),points.end(),[&](const WayPoint* x, const WayPoint* y) {
        const auto dx = (x->pos-origin).quadLength(), dy = (y->pos-origin).quadLength();
        return dx!=dy ? dx<dy : x->name<y->name;
        });
      bool found = false;
      for(const auto* p0:points) {
        if(found || attempts>=8192)
          break;
        const float y0 = groundY(w,p0->pos.x,p0->pos.z,p0->pos.y);
        if(!std::isfinite(y0) || std::abs(y0-p0->pos.y)>120.f)
          continue;
        for(const auto* p1:points) {
          const float distance = (p1->pos-p0->pos).length();
          if(distance<200.f || distance>std::min(range-250.f,2500.f))
            continue;
          const float y1 = groundY(w,p1->pos.x,p1->pos.z,p1->pos.y);
          if(!std::isfinite(y1) || std::abs(y1-p1->pos.y)>120.f)
            continue;
          ++attempts;
          place({p0->pos.x,y0,p0->pos.z},{p1->pos.x,y1,p1->pos.z});
          const auto head = first.mapHeadBone();
          const auto centerRay = w.physic()->ray(head,second.collosionCenter());
          const auto headRay = w.physic()->ray(head,second.mapHeadBone());
          if(first.qDistTo(second.collosionCenter())<=range*range && first.qDistTo(second.mapHeadBone())<=range*range &&
             centerRay.hasCol && centerRay.hitFraction>0.01f && centerRay.hitFraction<0.99f &&
             headRay.hasCol && headRay.hitFraction>0.01f && headRay.hitFraction<0.99f && !first.canSeeNpc(second,true)) {
            pointA = p0->name;
            pointB = p1->name;
            found = true;
            break;
            }
          if(attempts>=8192)
            break;
          }
        }
      if(!found) {
        fail("no genuine center/head occlusion found in bounded waypoint search");
        return;
        }
      } else {
      place(first.position(),second.position());
      }
    const auto head = first.mapHeadBone(), targetHead = second.mapHeadBone(), targetCenter = second.collosionCenter();
    const auto centerRay = w.physic()->ray(head,targetCenter), headRay = w.physic()->ray(head,targetHead);
    const bool visible = first.canSeeNpc(second,true), fovVisible = first.canSeeNpc(second,false);
    const bool centerInRange = first.qDistTo(targetCenter)<=range*range, headInRange = first.qDistTo(targetHead)<=range*range;
    TestBench::bodyEvent(first,&second,"senses_vision",w.tickCount(),
                         "\"case\":"+jsonStr(mode)+",\"process_policy\":"+jsonStr(policyName(nativePolicy))+
                         ",\"can_see_npc_free_los\":"+(visible ? "true" : "false")+
                         ",\"can_see_npc_fov\":"+(fovVisible ? "true" : "false")+
                         ",\"center_ray_hit\":"+(centerRay.hasCol ? "true" : "false")+
                         ",\"head_ray_hit\":"+(headRay.hasCol ? "true" : "false")+
                         ",\"center_hit_fraction\":"+std::to_string(centerRay.hitFraction)+
                         ",\"head_hit_fraction\":"+std::to_string(headRay.hitFraction)+
                         ",\"center_in_range\":"+(centerInRange ? "true" : "false")+
                         ",\"head_in_range\":"+(headInRange ? "true" : "false")+
                         ",\"distance_cm\":"+std::to_string(std::sqrt(first.qDistTo(second)))+
                         ",\"senses_range_cm\":"+std::to_string(range)+",\"actor_head\":"+vec(head)+
                         ",\"target_head\":"+vec(targetHead)+",\"target_center\":"+vec(targetCenter)+
                         ",\"center_hit\":"+(centerRay.hasCol ? vec(centerRay.v) : "null")+
                         ",\"head_hit\":"+(headRay.hasCol ? vec(headRay.v) : "null")+
                         ",\"actor_waypoint\":"+jsonStr(pointA)+",\"target_waypoint\":"+jsonStr(pointB)+
                         ",\"search_attempts\":"+std::to_string(attempts));
    if(!centerInRange || !headInRange || visible==occluded || (!occluded && (centerRay.hasCol || headRay.hasCol))) {
      fail("native visibility predicate disagrees with real in-range collision rays");
      return;
      }
    // Keep the listener close to these probe actors. WorldSound still executes its own range and
    // occlusion rules, using the loaded CPU camera; no rendering or listener device is involved.
    pl.setPosition(first.position());
    pl.updateTransform();
    Gothic::inst().gameSession()->camera().reset(&pl);
    w.sound()->tick(pl);
    const bool before = w.sound()->canSeeSource(head);
    if(before) {
      fail("an unrelated live 3D sound would mask this probe");
      return;
      }
    bench.probeSound = std::make_unique<::Sound>(w,::Sound::T_3D,"LEVELUP.WAV",targetHead,5000.f,false);
    bench.probeSoundDuration = bench.probeSound->effectPrefferedTime();
    if(bench.probeSound->isEmpty() || bench.probeSoundDuration==0 || bench.probeSoundDuration>10000) {
      fail("LEVELUP.WAV is missing or has an invalid duration");
      return;
      }
    bench.probeSound->setLooping(false);
    bench.probeSound->play();
    bench.probeSoundStart = w.tickCount();
    bench.probeOccluded = occluded;
    const bool finished = bench.probeSound->isFinished(), sourceVisible = w.sound()->canSeeSource(head);
    TestBench::bodyEvent(first,&second,"senses_sound_start",w.tickCount(),
                         "\"case\":"+jsonStr(mode)+",\"asset\":\"LEVELUP.WAV\",\"duration_ms\":"+
                         std::to_string(bench.probeSoundDuration)+",\"finished\":"+(finished ? "true" : "false")+
                         ",\"source_visible_before\":false,\"source_visible\":"+(sourceVisible ? "true" : "false")+
                         ",\"source\":"+vec(targetHead)+",\"listener\":"+vec(head)+
                         ",\"source_ray_hit\":"+(headRay.hasCol ? "true" : "false")+
                         ",\"clock\":"+jsonStr("simulation"));
    if(finished || sourceVisible==occluded) {
      fail("3D sound lifecycle or source visibility invariant failed");
      return;
      }
    bench.waitUntil = bench.time+bench.probeSoundDuration+34;
    say("bodyprobe "+mode+" verified; sound duration "+std::to_string(bench.probeSoundDuration));
    }
  else if(c=="npcequip") {
    // npcequip <npc> <item> [count]: into its inventory, the first one put on (a bow and its arrows)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    const size_t id = w.script().findSymbolIndex(a.size()>2 ? a[2] : "");
    if(n==nullptr || id==size_t(-1)) {
      say("npcequip: no npc or item " + line);
      return;
      }
    n->addItem(id,size_t(num(a,3,1)));
    n->useItem(id,Item::NSLOT,true);
    say("npcequip " + symName(w,n) + " " + a[2]);
    }
  else if(c=="arc") {
    // arc <npc> <count> <dist> <step deg>: that many of it stand that far in front of the hero, a step
    // apart round it, facing it (`all` of fight)
    const size_t id = w.script().findSymbolIndex(a.size()>1 ? a[1] : "");
    if(id==size_t(-1)) {
      say("arc: no npc " + line);
      return;
      }
    const int   cnt  = int(num(a,2,4));
    const float d    = num(a,3,1500);
    const float step = num(a,4,15);
    std::string out  = "arc";
    bench.crew.clear();
    for(int i=0; i<cnt; ++i) {
      const float an  = (pl.rotation() + (float(i)-float(cnt-1)*0.5f)*step)*float(M_PI)/180.f;
      Vec3        pos = pl.position() + Vec3(std::cos(an),0,std::sin(an))*d;
      DynamicWorld::RayLandResult r;
      for(float y=pos.y+1500; y>pos.y-1500 && !r.hasCol; y-=400)
        r = w.physic()->ray({pos.x,y,pos.z},{pos.x,y-450,pos.z});
      if(r.hasCol)
        pos.y = r.v.y;
      Npc* n = w.addNpc(id,pos);
      if(n==nullptr)
        continue;
      const Vec3 dir = pl.position()-pos;
      n->setDirection(Vec3(dir.x,0,dir.z));
      n->updateTransform();
      bench.crew.push_back(n);
      char buf[100];
      std::snprintf(buf,sizeof(buf)," %.0f %.0f %.0f",pos.x,pos.y,pos.z);
      out += buf;
      }
    say(out);
    }
  else if(c=="wander" && a.size()>1 && a[1]=="stats") {
    size_t walking = 0;
    for(auto* n:bench.wanderers)
      if(!n->isDown() && n->isAiBusy())
        ++walking;
    say("wander stats: " + std::to_string(bench.wanderers.size()) + " npcs, " + std::to_string(walking) +
        " walking, " + std::to_string(bench.wanderLegs) + " legs");
    }
  else if(c=="stuck") {
    // stuck begin <label> [radius cm 3000] | stuck end: observe native routine movement without
    // taking control of any NPC. The logger is absent unless OPENGOTHIC_STUCK_LOG is set.
    if(a.size()>=3 && a.size()<=4 && a[1]=="begin") {
      const float radius = num(a,3,3000);
      if(!std::isfinite(radius) || radius<=0 || (a.size()==4 && !isNumber(a[3]))) {
        say("stuck begin failed: radius must be positive and finite");
        return;
        }
      const bool ok = TestBench::stuckBegin && TestBench::stuckBegin(w,a[2],radius);
      say(std::string(ok ? "stuck begin " : "stuck begin failed: ") + a[2] +
          " radius_cm=" + std::to_string(radius));
      }
    else if(a.size()==2 && a[1]=="end") {
      const bool ok = TestBench::stuckEnd && TestBench::stuckEnd(w);
      say(ok ? "stuck end" : "stuck end failed: logger unavailable or no observation");
      }
    else {
      say("stuck: expected begin <label> [radius cm 3000] or end");
      }
    }
  else if(c=="wander") {
    // wander [radius cm 3000] [walk|run]: each npc set out by `arc` is held as the AI server holds a body and
    // walks by the waynet to a waypoint within the radius of the hero, then to another: a crowd for the speed
    // probe (docs/navigation.md, stage 0)
    const float r = num(a,1,3000);
    bench.wanderPoints.clear();
    w.findWayPoint(pl.position(),[&](const WayPoint& p) {
      if((p.pos-pl.position()).quadLength()<r*r)
        bench.wanderPoints.push_back(&p);
      return false;
      });
    const bool run = a.size()>2 && a[2]=="run";
    bench.wanderers.clear();
    bench.wanderLegs = 0;
    for(auto* n:bench.crew) {
      n->clearAiQueue();
      n->clearState(false);
      n->setExternalControl(true);
      n->setWalkMode(run ? WalkBit::WM_Run : WalkBit::WM_Walk);
      bench.wanderers.push_back(n);
      }
    say("wander " + std::to_string(bench.wanderers.size()) + " npcs, " + std::to_string(bench.wanderPoints.size()) +
        " waypoints within " + std::to_string(int(r)) + " cm");
    }
  else if(c=="fight") {
    // fight <npc|all> <target|player>: it attacks as the game's fight starts (ZS_ATTACK, to kill: an
    // archer readies its bow from 9 m); all: the npcs of arc
    std::vector<Npc*> who;
    if(a.size()>1 && a[1]=="all")
      who = bench.crew; else
    if(Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest"))
      who.push_back(n);
    Npc*         tg     = findNpc(w,pl,a.size()>2 ? a[2] : "player");
    const size_t state  = w.script().findSymbolIndex("ZS_ATTACK");
    auto         reason = w.script().findSymbol("AIV_ATTACKREASON");
    auto         kill   = w.script().findSymbol("AR_KILL");
    if(tg==nullptr || state==size_t(-1)) {
      say("fight: no target or no ZS_ATTACK " + line);
      return;
      }
    size_t sent = 0;
    for(auto n:who) {
      if(n==tg || n->isDown())
        continue;
      ++sent;
      if(reason!=nullptr && kill!=nullptr && size_t(reason->get_int())<std::size(n->handle().aivar))
        n->handle().aivar[size_t(reason->get_int())] = kill->get_int();
      n->setExternalControl(false);
      n->setTarget(tg);
      n->aiPush(AiQueue::aiStartState(ScriptFn(state),0,tg,nullptr,""));
      }
    say("fight " + std::to_string(sent) + " at " + symName(w,tg));
    }
  else if(c=="waitdown") {
    // waitdown <npc|player> [ms]: the scenario waits till it is down (dead, unconscious): `fight over`
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr) {
      say("waitdown: no npc " + line);
      return;
      }
    bench.downNpc  = n;
    bench.downFrom = bench.time;
    bench.downEnd  = bench.time + uint64_t(num(a,2,120000));
    }
  else if(c=="npcaim") {
    // npcaim on [second|arrow] | off | sigma <deg> | sigma off | lead on|off: npc archers aim by hand
    // (game/archerai): as many hits a second as G2's dice (second) or as sure an arrow (arrow); an error
    // forced (degrees per axis); the lead on a runner
    const std::string m = a.size()>1 ? a[1] : "";
    ++ArcherAi::epoch;
    if(m=="on") {
      ArcherAi::setEnabled(true);
      if(a.size()>2)
        ArcherAi::mode = a[2]=="arrow" ? ArcherAi::PerArrow : ArcherAi::PerSecond;
      }
    else if(m=="off")
      ArcherAi::setEnabled(false);
    else if(m=="sigma")
      ArcherAi::forcedSigma = a.size()>2 && a[2]!="off" ? num(a,2,0) : -1.f;
    else if(m=="lead")
      ArcherAi::leadOn = a.size()<3 || a[2]!="off";
    say("npcaim " + std::string(ArcherAi::isEnabled() ? "on" : "off") + (ArcherAi::mode==ArcherAi::PerArrow ? " arrow" : " second") +
        " sigma " + std::to_string(ArcherAi::forcedSigma) + " lead " + (ArcherAi::leadOn ? "on" : "off"));
    }
  else if(c=="archer") {
    // archer <npc|all> at <npc|player> [dice] | archer off: it stands where it is and shoots at the target
    // every time it can, by hand (npcaim on) or by the dice: no fight AI, no perception's wait (the aim's
    // calibration); all: the npcs of arc
    auto release = [&]() {
      for(auto& [n,at]:bench.archers)
        if(n!=nullptr) {
          n->archerAi().cancel(*n);
          n->setExternalControl(false);
          }
      bench.archers.clear();
      };
    if(a.size()>1 && a[1]=="off") {
      release();
      bench.archerAt = nullptr;
      say("archer off");
      return;
      }
    std::vector<Npc*> who;
    if(a.size()>1 && a[1]=="all")
      who = bench.crew; else
    if(Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest"))
      who.push_back(n);
    release();
    bench.archerAt   = findNpc(w,pl,a.size()>3 ? a[3] : "player");
    bench.archerDice = a.size()>4 && a[4]=="dice";
    for(auto n:who) {
      if(n==bench.archerAt || n->isDown())
        continue;
      n->setInteraction(nullptr,true);
      n->clearAiQueue();
      n->clearState(false);
      n->setExternalControl(true);
      bench.archers.emplace_back(n,n->position());
      }
    say("archer " + std::to_string(bench.archers.size()) + " at " + symName(w,bench.archerAt) + (bench.archerDice ? " by the dice" : ""));
    }
  else if(c=="still") {
    // still <npc|player> on|off: hit, it neither stumbles nor flinches (a target that keeps its pose, as
    // its zones were dumped)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr) {
      say("still: no npc " + line);
      return;
      }
    n->setBenchStill(a.size()<3 || a[2]!="off");
    say("still " + symName(w,n) + (n->isBenchStill() ? " on" : " off"));
    }
  else if(c=="zones" && a.size()>2 && a[1]=="fit") {
    // zones fit on|off: the hit zones as thick as the body's mesh, or by a guess from the bone names (the
    // hero refitted now, the others when they change their body)
    HitZones::setFitEnabled(a[2]=="on");
    pl.fitHitZones();
    say("zones fit " + a[2]);
    }
  else if(c=="zones") {
    // zones <npc|nearest|player>: where its hit zones are now (the capsules on its bones), how well they
    // cover its body
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr) {
      say("zones: no npc " + line);
      return;
      }
    Vec3 h, b;
    n->hitZoneCenter(HitZones::Head,h);
    n->hitZoneCenter(HitZones::Body,b);
    char buf[200];
    std::snprintf(buf,sizeof(buf),"zones %s at %.0f %.0f %.0f: head %.0f %.0f %.0f body %.0f %.0f %.0f",symName(w,n).c_str(),
                  n->position().x,n->position().y,n->position().z,h.x,h.y,h.z,b.x,b.y,b.z);
    say(buf + std::string("\n") + n->hitZonesDump());
    }
  else if(c=="open") {
    // open [dist cm]: how far it is open around the hero at head height and at the knees, every 15
    // degrees from the facing (a target for the bow needs open ground)
    const float dist = num(a,1,4000);
    const Vec3  p0   = pl.position();
    std::string out  = "open (yaw from the facing: head/knees m):";
    for(int i=0; i<24; ++i) {
      const float an = (pl.rotation()+float(i*15))*float(M_PI)/180.f;
      const Vec3  d  = Vec3(std::cos(an),0,std::sin(an));
      float       m[2];
      for(int k=0; k<2; ++k) {
        const Vec3 from = p0 + Vec3(0,k==0 ? 160.f : 40.f,0);
        auto       r    = w.physic()->ray(from,from+d*dist);
        m[k] = r.hasCol ? (r.v-from).length()/100.f : dist/100.f;
        }
      char buf[48];
      std::snprintf(buf,sizeof(buf)," %d:%.0f/%.0f",i*15,double(m[0]),double(m[1]));
      out += buf;
      }
    say(out);
    }
  else if(c=="volley") {
    // volley <shots> [hold ms] [draw 0..1] [jitter ms]: shots by hand in a row (manual on): the string drawn
    // that far (1: full), held (up to jitter longer, at random), let go; the scenario goes on after the last
    // one is reloaded
    bench.volleyLeft  = int(num(a,1,5));
    bench.volleyHold  = uint64_t(num(a,2,300));
    bench.volleyDraw  = num(a,3,1);
    bench.volleyJitter = uint64_t(num(a,4,0));
    bench.volleyPhase = 0;
    bench.volleyT     = bench.time;
    say("volley " + std::to_string(bench.volleyLeft));
    }
  else if(c=="aimat") {
    // aimat <npc|nearest> [head|body|limb|leg] | aimat <x> <y> <z> | aimat ahead [dist] [height] | aimat off: the
    // manual aim aims there
    // instead of the mouse - the middle of a zone of the npc, every frame. A last word `mouse`: the game's camera
    // turns onto it as the mouse would, the hero with it, the shot goes from the center of the screen
    auto a = split(line);
    bench.aimNpc   = nullptr;
    bench.aimFixed = false;
    bench.aimMouse = a.size()>2 && a.back()=="mouse";
    if(bench.aimMouse) {
      a.pop_back();
      ctrl.setAimOverride(false);
      }
    if(a.size()>1 && a[1]=="off") {
      ctrl.setAimOverride(false);
      say("aimat off");
      return;
      }
    if(a.size()>1 && a[1]=="ahead") {
      // aimat ahead [dist cm 2000] [height cm 140]: a point that far ahead of the hero as it faces now
      const Vec3 f   = facing(pl);
      bench.aimFixed = true;
      bench.aimAt    = pl.position() + f*num(a,2,2000) + Vec3(0,num(a,3,140),0);
      ctrl.setAimOverride(!bench.aimMouse,bench.aimAt);
      say(std::string("aimat ahead") + (bench.aimMouse ? " mouse" : ""));
      return;
      }
    if(a.size()>3 && (std::isdigit(uint8_t(a[1][0])) || a[1][0]=='-')) {
      bench.aimFixed = true;
      bench.aimAt    = Vec3(num(a,1,0),num(a,2,0),num(a,3,0));
      ctrl.setAimOverride(!bench.aimMouse,bench.aimAt);
      say("aimat " + a[1] + " " + a[2] + " " + a[3] + (bench.aimMouse ? " mouse" : ""));
      return;
      }
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("aimat: no npc " + line);
      return;
      }
    const std::string z = a.size()>2 ? a[2] : "body";
    bench.aimNpc  = n;
    bench.aimZone = z=="head" ? HitZones::Head : (z=="limb" || z=="leg" ? HitZones::Limb : HitZones::Body);
    bench.aimLeg  = z=="leg";
    bench.aimRaw  = a.size()>3 && a[3]=="raw";
    say("aimat " + symName(w,n) + " " + std::string(HitZones::name(bench.aimZone)) + (bench.aimRaw ? " raw" : "") +
        (bench.aimMouse ? " mouse" : ""));
    }
  else if(c=="immortal") {
    // immortal <npc|nearest> on|off: arrows hit it, it does not die (counting hits)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr) {
      say("immortal: no npc " + line);
      return;
      }
    auto& h = n->handle();
    if(a.size()>2 && a[2]=="off")
      h.flags = zenkit::NpcFlag(uint32_t(h.flags) & ~uint32_t(zenkit::NpcFlag::IMMORTAL)); else
      h.flags = zenkit::NpcFlag(uint32_t(h.flags) |  uint32_t(zenkit::NpcFlag::IMMORTAL));
    say("immortal " + symName(w,n) + " " + (n->isImmortal() ? "on" : "off"));
    }
  else if(c=="clear") {
    // clear [radius]: the npcs around the hero leave the world (rats, wolves that would join in), not the
    // watched one
    const float r = num(a,1,2500);
    std::vector<Npc*> gone;
    for(uint32_t i=0; i<w.npcCount(); ++i)
      if(Npc* n = w.npcById(i); n!=nullptr && n!=&pl && n!=bench.subject && (n->position()-pl.position()).length()<r)
        gone.push_back(n);
    std::string out = "clear:";
    for(auto n:gone) {
      out += " " + symName(w,n);
      bench.crew.erase(std::remove(bench.crew.begin(),bench.crew.end(),n),bench.crew.end());
      bench.archers.erase(std::remove_if(bench.archers.begin(),bench.archers.end(),[n](auto& x){ return x.first==n; }),bench.archers.end());
      if(bench.archerAt==n) {
        // their target gone: archer off
        for(auto& [x,at]:bench.archers)
          if(x!=nullptr) {
            x->archerAi().cancel(*x);
            x->setExternalControl(false);
            }
        bench.archers.clear();
        bench.archerAt = nullptr;
        }
      if(bench.pinned==n)
        bench.pinned = nullptr;
      if(bench.downNpc==n)
        bench.downNpc = nullptr;
      w.removeNpc(*n);
      }
    say(out);
    }
  else if(c=="flinch") {
    // flinch on|off: the upper body gives way to a blow (graphics/mesh/flinch)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Flinch::setEnabled(m=="on"); else
      say("unknown flinch command: " + line);
    }
  else if(c=="hit") {
    // hit <npc|player> <angle> [strength] [height] [twist]: a blow from that side of it (angle from its
    // facing, 0 in front, as place), only the flinch: no damage, no stumble
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr) {
      say("hit: no npc " + line);
      return;
      }
    const float an = (n->rotation()+num(a,2,0))*float(M_PI)/180.f;
    Flinch::Hit h;
    h.dir      = Vec3(-std::cos(an),0,-std::sin(an));
    h.strength = num(a,3,1);
    h.height   = num(a,4,0.6f);
    h.twist    = num(a,5,0);
    n->flinch(h);
    char buf[160];
    std::snprintf(buf,sizeof(buf),"hit %s from %.0f strength %.2f height %.2f twist %.2f",symName(w,n).c_str(),
                  double(num(a,2,0)),double(h.strength),double(h.height),double(h.twist));
    say(buf);
    }
  else if(c=="strike") {
    // strike <victim|player> [attacker|player]: the attacker's blow lands as the game deals it (melee: the
    // stumble, the damage, the flinch); the victim gets its hit points back below a third
    Npc* n  = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    Npc* by = findNpc(w,pl,a.size()>2 ? a[2] : "player");
    if(n==nullptr || by==nullptr || n==by) {
      say("strike: no npc " + line);
      return;
      }
    const int32_t hp0 = n->attribute(ATR_HITPOINTS);
    n->takeDamage(*by,nullptr);
    const int32_t hp1 = n->attribute(ATR_HITPOINTS);
    say("strike " + symName(w,n) + " by " + symName(w,by) + ": hp " + std::to_string(hp0) + " -> " + std::to_string(hp1) +
        " anim " + n->animNames());
    if(!n->isDown() && hp1*3<n->attribute(ATR_HITPOINTSMAX))
      n->changeAttribute(ATR_HITPOINTS,n->attribute(ATR_HITPOINTSMAX)-hp1,false);
    }
  else if(c=="aim") {
    // aim on|off: the upper body turns and bends to the target with a bow, a crossbow, a spell
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Aim::setEnabled(m=="on"); else
      say("unknown aim command: " + line);
    }
  else if(c=="aimlean") {
    // aimlean auto|degrees: force a settled pose for the head prediction check, without changing aim direction.
    if(a.size()>1 && a[1]=="auto") {
      Aim::playerLeanOverride = -1.f;
      say("aimlean auto");
      }
    else if(a.size()>1) {
      const float degrees = std::clamp(num(a,1,0),0.f,Aim::LeanMax);
      Aim::playerLeanOverride = degrees*float(M_PI)/180.f;
      say("aimlean " + std::to_string(degrees));
      }
    else {
      say("aimlean: expected auto or degrees 0..70");
      }
    }
  else if(c=="lean") {
    // lean on|off: the lean into turns and on slopes (graphics/mesh/lean)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Lean::setEnabled(m=="on"); else
      say("unknown lean command: " + line);
    }
  else if(c=="blend") {
    // blend on|off, blend param <min|scale|max> <value>: transitions between animations (AnimBlend)
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
    // ani <npc|nearest|player> <anim>: it plays an animation of its model script (T_YES, T_DIALOGGESTURE_03)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || a.size()<3) {
      say("ani: no npc or anim " + line);
      return;
      }
    auto sq = n->playAnimByName(a[2],BS_STAND);
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
    // text on the screen of a short from here (scripts/shorts cut): caption <text>, caption off
    event("caption","\"text\":" + jsonStr(rest(line)=="off" ? "" : rest(line)));
    }
  else if(c=="mark") {
    event("mark","\"text\":" + jsonStr(rest(line)));
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

void tickVolley(PlayerControl& ctrl) {
  const auto& mp = Archer::playerProbe;
  const auto  t  = bench.time - bench.volleyT;
  auto stop = [&](const char* why) {
    say(std::string("volley: ") + why);
    release(ctrl,KeyCodec::ActionGeneric);
    bench.volleyLeft = 0;
    };
  switch(bench.volleyPhase) {
    case 0:
      press(ctrl,KeyCodec::ActionGeneric);
      bench.volleyPhase = 1;
      bench.volleyT     = bench.time;
      break;
    case 1: {
      const bool drawn = mp.valid && (mp.stage==Archer::Held || (mp.stage==Archer::Drawing && mp.draw>=bench.volleyDraw));
      if(drawn) {
        bench.volleyPhase   = 2;
        bench.volleyT       = bench.time;
        bench.volleyHoldNow = bench.volleyHold;
        if(bench.volleyJitter>0) {
          bench.lcg = bench.lcg*1664525u + 1013904223u;
          bench.volleyHoldNow += (bench.lcg>>8)%(bench.volleyJitter+1);
          }
        }
      else if(t>10000)
        stop("the string is not drawn");
      break;
      }
    case 2:
      if(t>=bench.volleyHoldNow) {
        if(bench.aimNpc!=nullptr) {
          // where it aims, the middle of the zone: the log compares the arrow's flight with it
          Vec3 c = bench.aimNpc->collosionCenter();
          aimPoint(*bench.aimNpc,bench.aimZone,bench.aimLeg,c);
          char buf[160];
          std::snprintf(buf,sizeof(buf),"aim zone %.1f %.1f %.1f",c.x,c.y,c.z);
          TestBench::note(buf);
          }
        release(ctrl,KeyCodec::ActionGeneric);
        bench.volleyPhase = 3;
        bench.volleyT     = bench.time;
        }
      break;
    case 3:
      if(mp.valid && mp.stage==Archer::Nocked && t>200) {
        --bench.volleyLeft;
        bench.volleyPhase = 0;
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

bool waitingForBodyLive() {
  if(bench.bodyLiveWait.empty())
    return false;
  std::error_code error;
  if(std::filesystem::is_regular_file(bench.bodyLiveWait,error)) {
    event("body_live_ready","\"path\":"+jsonStr(bench.bodyLiveWait));
    bench.bodyLiveWait.clear();
    return false;
    }
  if(std::chrono::steady_clock::now()<bench.bodyLiveDeadline)
    return true;
  event("body_live_error","\"reason\":\"readiness file timeout\",\"path\":"+jsonStr(bench.bodyLiveWait));
  bench.bodyLiveWait.clear();
  bench.pc=bench.prog.size();
  bench.waitUntil=0;
  return false;
  }

void run(World& w, Npc& pl, PlayerControl& ctrl) {
  if(NavBench::running() || NavDemos::running()) return;
  if(bench.spellEquipPending) {
    if(pl.drawSpell(bench.spellEquipPending)) {bench.spellCastingId=bench.spellEquipPending;bench.spellEquipPending=0;}
    else if(w.tickCount()<bench.spellEquipUntil) return;
    else {bench.spellEquipPending=0;arenaAbort("real spell draw animation did not finish");return;}
    }
  if(bench.spellWaitFrozen>=0) {
    const bool frozen=!bench.spellActors.empty() && bench.spellActors[0].npc->spellFrozen();
    const bool ready=bench.spellWaitFrozen==2 ? !bench.spellActors.empty() && bench.spellActors[0].npc->isInAir() :
                     bench.spellWaitFrozen==3 ? w.spellFieldCount()>0 :
                     bench.spellWaitFrozen==4 ? !bench.spellActors.empty() &&
                       bench.spellActors[0].npc->ragdollQuestReturnCount()==1 &&
                       bench.spellActors[0].npc->isStanding() && !bench.spellActors[0].npc->isInAir() &&
                       !bench.spellActors[0].npc->isDown() && !bench.spellActors[0].npc->ragdollQuestReturnState().armed :
                       int(frozen)==bench.spellWaitFrozen;
    if(!ready && w.tickCount()<bench.spellWaitUntil) return;
    event("spell_physics_freeze_wait","\"frozen\":"+std::string(frozen?"true":"false")+",\"pass\":"+(ready?"true":"false"));
    bench.spellWaitFrozen=-1;
    if(!ready) {arenaAbort(bench.spellGuardTrace?"real wind throw did not return standing":"real freeze state did not reach requested transition");return;}
    }
  if(!bench.wanderPoints.empty())
    for(auto* n:bench.wanderers) {
      if(n->isDown() || n->isAiBusy())
        continue;
      bench.lcg = bench.lcg*1664525u + 1013904223u;
      n->aiPush(AiQueue::aiGoToPoint(*bench.wanderPoints[(bench.lcg>>8) % bench.wanderPoints.size()]));
      ++bench.wanderLegs;
      }
  if(bench.livingMotion)
    for(auto& walker:bench.livingWalkers) {
      if(walker.npc->isDown()) continue;
      const auto& target=walker.ends[walker.next?1:0];
      if(!walker.queued || ((walker.npc->position()-target.pos).length()<120 && walker.npc->isAiQueueEmpty())) {
        if(walker.queued) walker.next=!walker.next;
        walker.npc->aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
        }
      }
  for(auto& walker:bench.npcCharacterCrowd) {
    if(walker.npc->isDown())continue;
    const auto& target=walker.ends[walker.next?1:0];
    if(!walker.queued || ((walker.npc->position()-target.pos).length()<90 && walker.npc->isAiQueueEmpty())) {
      if(walker.queued)walker.next=!walker.next;
      walker.npc->aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
      }
    }
  if(bench.questNpc!=nullptr && !bench.questCase.empty()) questFields(w,*bench.questNpc,false);
  if(bench.questStandingWait) {
    const auto* npc=bench.questNpc;
    const bool ready=npc && npc->isStanding() && !npc->isDown() && !npc->ragdollBody() &&
                     !npc->isRagdollBlending() && !npc->ragdollQuestReturnState().armed;
    if(!ready && bench.time<bench.questDeadline) return;
    bench.questStandingWait=false;
    event("immortal_guard_standing","\"case\":"+jsonStr(bench.questCase)+",\"standing\":"+(ready?"true":"false"));
    if(!ready) {arenaAbort("real get-up did not finish standing");return;}
    }
  if(bench.questWait) {
    const bool observed=bench.questWaterCase?bench.questSawWater:bench.questSawDrop;
    const uint8_t wanted=bench.questWaterCase ? RagdollSave::QuestReturn::water : RagdollSave::QuestReturn::fell;
    const bool guarded=bench.questNpc->isHuman() && !bench.questNpc->isPlayer() && Gothic::settingsGetI("GAME","ragdoll_quest_guard");
    const bool reached=observed && (!guarded || (bench.questNpc->ragdollQuestReturnState().hazards & wanted));
    if(!reached && bench.time<bench.questDeadline) return;
    bench.questWait=false;
    if(!reached) {arenaAbort("quest guard physical hazard was not reached");return;}
    const auto* npc=bench.questNpc;
    const auto* body=npc->ragdollBody();
    auto pelvis=npc->position();
    if(body) {const auto tr=body->pelvisTransform();pelvis={tr.at(3,0),tr.at(3,1),tr.at(3,2)};}
    const auto water=w.physic()->waterRay(pelvis,0);
    event("immortal_guard_hazard","\"case\":"+jsonStr(bench.questCase)+",\"pelvis_cm\":"+ragdollVector(pelvis)+
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
  if(bench.volleyLeft>0) {
    tickVolley(ctrl);
    if(bench.volleyLeft>0)
      return;
    }
  while(bench.pc<bench.prog.size() || bench.speaking || bench.waitReply || !bench.waitAiKey.empty() || !bench.bodyLiveWait.empty() ||
        bench.downNpc!=nullptr || bench.waitRagdollNpc!=nullptr || bench.duelWaiting) {
    if(bench.speaking) {
      if(bench.time<bench.speakUntil)
        return;
      endSpeaking();
      }
    if(waitingForReply() || waitingForAi() || waitingForBodyLive())
      return;
    if(bench.waitRagdollNpc!=nullptr) {
      const auto* npc=bench.waitRagdollNpc;
      const auto* body=npc->ragdollBody();
      const auto& quest=npc->ragdollQuestReturnState();
      const bool guardedReturn=quest.armed && quest.hazards!=0;
      const bool ready=bench.waitRagdollState=="sleep" ? body!=nullptr && body->isSleeping() :
                       bench.waitRagdollState=="recovery" ? npc->isRagdollBlending() :
                       body==nullptr && !npc->isDown() && !npc->isRagdollBlending() &&
                       (!guardedReturn || (npc->isStanding() && !quest.armed));
      if(!ready && bench.time<bench.waitRagdollEnd)
        return;
      say("waitragdoll "+bench.waitRagdollState+(ready ? " reached" : " timeout"));
      event(ready ? "ragdoll_wait" : "ragdoll_error","\"state\":"+jsonStr(bench.waitRagdollState)+
            ",\"reached\":"+(ready ? "true" : "false"));
      bench.waitRagdollNpc=nullptr;
      }
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
    if(bench.downNpc!=nullptr) {
      if(!bench.downNpc->isDown() && bench.time<bench.downEnd)
        return;
      char buf[160];
      if(bench.downNpc->isDown())
        std::snprintf(buf,sizeof(buf),"fight over: %s down after %llu",symName(w,bench.downNpc).c_str(),
                      (unsigned long long)(bench.time-bench.downFrom)); else
        std::snprintf(buf,sizeof(buf),"waitdown: timeout, %s hp %d",symName(w,bench.downNpc).c_str(),
                      bench.downNpc->attribute(ATR_HITPOINTS));
      TestBench::note(buf);
      bench.downNpc = nullptr;
      }
    if(bench.pc>=bench.prog.size())
      break;
    if(bench.time<bench.waitUntil || bench.volleyLeft>0)
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
    if(NavBench::running() || NavDemos::running()) return;
    if(bench.spellWaitFrozen>=0 || bench.spellEquipPending) return;
    if(bench.questWait || bench.questStandingWait) return;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
    if(bench.cadenceWait) return;
#endif
    }
  if(bench.time>=bench.waitUntil && !bench.waitArrive && bench.volleyLeft==0 && bench.downNpc==nullptr && bench.waitRagdollNpc==nullptr && !bench.duelWaiting && bench.bodyLiveWait.empty())
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
  if(cam!=nullptr && (!bench.zonesNpc.empty() || bench.livingDebug)) {
    auto* w = Gothic::inst().world();
    const auto actors=bench.livingDebug ? bench.crew : std::vector<Npc*>{bench.zonesNpc=="player" ? Gothic::inst().player() : (w!=nullptr ? npcBySymbol(*w,bench.zonesNpc) : nullptr)};
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

bool TestBench::isActive() {
  return scenarioPath!=nullptr;
  }

bool TestBench::isPhysicsNoiseDebug() {
  return bodyTraceAlive && bench.noiseBridgeDebug;
  }

void TestBench::physicsNoiseBridgeEvent(const Npc& listener,uint64_t tick) {
  if(isPhysicsNoiseDebug() && &listener==bench.noiseHost)bench.noiseBridgeEvents.push_back(tick);
  }

bool TestBench::physicsNoiseDebug(World& world,Npc& player,std::string_view command,
                                  std::string& value,std::string& error) {
  const auto fail=[&](std::string_view message) {error=message;return false;};
  const auto resetCounts=[] {
    bench.noiseBridgeImpacts=0;bench.noiseBridgeLoudImpacts=0;bench.noiseBridgeCulpritImpacts=0;
    bench.noiseBridgeUnseenCulpritImpacts=0;bench.noiseBridgeCupImpacts=0;bench.noiseBridgeCupMaxRadius=0;
    bench.noiseBridgeDialogueImpacts=0;bench.noiseBridgeCombatImpacts=0;
    bench.noiseBridgeHeard.clear();
    bench.noiseBridgeEvents.clear();
    };
  if(command=="setup") {
    world.physic()->setMovableVobsEnabled(true);world.physic()->setPhysicsNoiseEnabled(true);
    prepareNoiseFixture(world,player);
    if(!bench.movableReady || !bench.noiseHost)return fail("Coragon tavern fixture unavailable in this save");
    world.script().skipImportantInfos(*bench.noiseHost,player);
    player.handle().attribute[ATR_HITPOINTS]=100000;
    player.handle().attribute[ATR_HITPOINTSMAX]=100000;
    bench.noiseBridgeDebug=true;
    }
  if(!bench.noiseBridgeDebug || !bench.noiseHost)return fail("physics_noise setup required");
  auto& npc=*bench.noiseHost;
  if(command.ends_with("-ready")) {
    noiseRetireListeners();
    const bool quiet=command=="quiet-ready";
    if(command!="loud-ready" && command!="quiet-ready" && command!="dialogue-ready" &&
       command!="combat-ready" && command!="throttle-ready")return fail("unknown physics_noise ready mode");
    const auto source=quiet ? (bench.movableWalkStart+bench.movableWalkEnd)*.5f : bench.movableCenter;
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
  for(auto tick:bench.noiseBridgeHeard) {if(heard.size()>1)heard+=',';heard+=std::to_string(tick);}
  std::string bridgeEvents="[";
  for(auto tick:bench.noiseBridgeEvents) {if(bridgeEvents.size()>1)bridgeEvents+=',';bridgeEvents+=std::to_string(tick);}
  value="{\"npc\":"+jsonStr(symName(world,&npc))+",\"position_cm\":"+ragdollVector(npc.position())+
    ",\"source_cm\":"+ragdollVector(bench.noiseSource)+",\"distance_cm\":"+
    ragdollNumber((npc.position()-bench.noiseSource).length())+",\"facing_error_deg\":"+
    ragdollNumber(noiseFacingError(npc,bench.noiseSource))+",\"can_see_hero\":"+(npc.canSeeNpc(player,false)?"true":"false")+
    ",\"eligible\":"+(npc.canHearPhysicsNoise()?"true":"false")+
    ",\"external\":"+(npc.isExternalControl()?"true":"false")+
    ",\"investigating\":"+(npc.isPhysicsInvestigating()?"true":"false")+
    ",\"player_distance_cm\":"+ragdollNumber((npc.position()-player.position()).length())+
    ",\"dialogue\":"+(Gothic::inst().isNpcInDialog(npc)?"true":"false")+
    ",\"combat\":"+(npc.weaponState()!=WeaponState::NoWeapon?"true":"false")+
    ",\"ai_queue_empty\":"+(npc.isAiQueueEmpty()?"true":"false")+
    ",\"routine_state\":"+(routine?"true":"false")+",\"state\":"+jsonStr(stateName(world,npc))+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"native_impacts\":"+std::to_string(bench.noiseBridgeImpacts)+
    ",\"native_loud_impacts\":"+std::to_string(bench.noiseBridgeLoudImpacts)+
    ",\"native_culprit_impacts\":"+std::to_string(bench.noiseBridgeCulpritImpacts)+
    ",\"native_unseen_culprit_loud_impacts\":"+std::to_string(bench.noiseBridgeUnseenCulpritImpacts)+
    ",\"native_cup_impacts\":"+std::to_string(bench.noiseBridgeCupImpacts)+
    ",\"native_cup_max_radius_cm\":"+ragdollNumber(bench.noiseBridgeCupMaxRadius)+
    ",\"audible_dialogue_loud_impacts\":"+std::to_string(bench.noiseBridgeDialogueImpacts)+
    ",\"audible_combat_loud_impacts\":"+std::to_string(bench.noiseBridgeCombatImpacts)+
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
    for(float distance:command=="far"?std::initializer_list<float>{1700,1900,2100,2400}:
                                       std::initializer_list<float>{300,450,600,800,1000}) {
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
    npc.setDirection(bench.destructionTraceSource-point);npc.updateTransform();
    npc.aiPush(AiQueue::aiWait(120000));
    };
  const auto floorAt=[&](Vec3& point) {
    const auto floor=world.physic()->landRay(point+Vec3(0,220,0),600);
    if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-bench.destructionTraceSource.y)>350)return false;
    point.y=floor.v.y;
    for(const auto offset:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,0,40),Vec3(0,0,-40)})
      if(world.physic()->ray(point+Vec3(0,85,0),point+offset+Vec3(0,85,0)).hasCol)return false;
    return true;
    };
  if(command=="setup" || command.starts_with("reload ")) {
    bench.destructionTraceDebug=false;bench.destructionMeleeRunning=false;
    bench.destructionTraceHost=nullptr;bench.destructionTraceWorld=nullptr;
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
      if(name.find("LOBART")!=std::string::npos) {bench.destructionTraceHost=npc;break;}
      }
    if(!bench.destructionTraceHost)return fail("destruction trace fixture requires Lobart");
    bench.destructionTraceZen=zenId;
    bench.destructionTraceSource=(found->localBounds[0]+found->localBounds[1])*.5f;
    found->transform.project(bench.destructionTraceSource);
    if(command=="setup") {
      destructionFarmSetup(world,player,zenId);
      if(bench.destructionFarmZen!=zenId || bench.destructionMeleeActor!=&player)
        return fail("destruction trace native sword stance preparation failed");
      }
    world.script().skipImportantInfos(*bench.destructionTraceHost,player);
    player.handle().attribute[ATR_HITPOINTS]=100000;player.handle().attribute[ATR_HITPOINTSMAX]=100000;
    bool supported=false;
    for(unsigned i=0;i<96;++i) {
      const float angle=float(i)*float(M_PI)/48.f;
      auto point=bench.destructionTraceSource+Vec3(std::cos(angle)*500,0,std::sin(angle)*500);
      if(!floorAt(point))continue;
      park(*bench.destructionTraceHost,point);supported=true;break;
      }
    if(!supported)return fail("destruction trace fixture lacks supported observer floor");
    bench.destructionTraceWorld=&world;bench.destructionTraceDebug=true;
    }
  if(!bench.destructionTraceDebug || bench.destructionTraceWorld!=&world || !bench.destructionTraceHost)
    return fail("destruction_traces setup or reload required");
  auto& npc=*bench.destructionTraceHost;
  const auto traceId=std::string(world.name())+":vob:"+std::to_string(bench.destructionTraceZen);
  if(command=="break") {
    if(bench.destructionFarmZen!=bench.destructionTraceZen || bench.destructionMeleeActor!=&player)
      return fail("destruction trace break requires the original intact setup");
    if(bench.destructionMeleeRunning)return fail("destruction trace native sword attack is already running");
    const auto bodies=world.physic()->movableVobSnapshot();
    const auto found=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body){return body.zenId==bench.destructionTraceZen;});
    if(found==bodies.end() || found->broken)return fail("destruction trace break requires its intact farm fence");
    bench.destructionMeleeRunning=true;bench.destructionMeleeNext=world.tickCount();
    bench.destructionMeleeDeadline=world.tickCount()+90000;
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
        auto point=bench.destructionTraceSource+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
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
    ",\"zen_id\":"+std::to_string(bench.destructionTraceZen)+",\"trace_id\":"+jsonStr(traceId)+
    ",\"source_position_cm\":"+ragdollVector(bench.destructionTraceSource)+
    ",\"npc_position_cm\":"+ragdollVector(npc.position())+
    ",\"game_minutes\":"+std::to_string(world.time().toInt()/60000)+
    ",\"melee_running\":"+(bench.destructionMeleeRunning?"true":"false")+
    ",\"melee_attempts\":"+std::to_string(bench.destructionMeleeAttempts)+",\"truth\":"+rows+"]}";
  return true;
  }

void TestBench::destructionBreak(uintptr_t body,size_t pieces,float milliseconds) {
  if(scenarioPath==nullptr || !bench.destructionTrace || !destructionOwns(body))return;
  auto* world=Gothic::inst().world();
  if(world)destructionFields(*world);
  bench.destructionLastBreak=world?world->tickCount():0;bench.destructionSleepSampled=false;
  event("destruction_break","\"case\":"+jsonStr(bench.destructionCase)+",\"body\":"+std::to_string(body)+
    ",\"pieces\":"+std::to_string(pieces)+",\"break_ms\":"+ragdollNumber(milliseconds)+
    ",\"wall_ns\":"+std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count())+
    ",\"t_game_ms\":"+std::to_string(world?world->tickCount():0)+",\"arrows\":"+std::to_string(bench.destructionArrows));
  }

void TestBench::destructionMeleeHit(const Npc& actor,size_t affected,float damage) {
  if(scenarioPath==nullptr || !bench.destructionMeleeRunning || bench.destructionMeleeActor!=&actor)return;
  if(affected)++bench.destructionMeleeHits;
  auto* world=Gothic::inst().world();
  event("destruction2_melee_hit","\"case\":"+jsonStr(bench.destructionCase)+",\"affected\":"+
    std::to_string(affected)+",\"hits\":"+std::to_string(bench.destructionMeleeHits)+
    ",\"raw_hit_power\":"+ragdollNumber(damage)+",\"bodies\":"+(world?destructionRows(*world):"[]"));
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
  if(bench.noiseBridgeDebug && (noise.position-bench.noiseSource).quadLength()<250.f*250.f) {
    ++bench.noiseBridgeImpacts;
    if(noise.volume>=.35f)++bench.noiseBridgeLoudImpacts;
    if(noise.culprit)++bench.noiseBridgeCulpritImpacts;
    if(noise.what.find("BEER")!=std::string::npos) {
      ++bench.noiseBridgeCupImpacts;
      bench.noiseBridgeCupMaxRadius=std::max(bench.noiseBridgeCupMaxRadius,noise.radius);
      }
    if(auto* world=Gothic::inst().world();world && bench.noiseHost && noise.volume>=.35f) {
      auto& npc=*bench.noiseHost;
      const float radius=noise.radius*std::max(0.f,1.f-world->physic()->soundOclusion(npc.mapHeadBone(),noise.position));
      if(npc.qDistTo(noise.position)<=radius*radius) {
        if(noise.culprit && !npc.canSeeNpc(*noise.culprit,false))++bench.noiseBridgeUnseenCulpritImpacts;
        if(Gothic::inst().isNpcInDialog(npc))++bench.noiseBridgeDialogueImpacts;
        if(npc.weaponState()!=WeaponState::NoWeapon && stateName(*world,npc)=="ZS_ATTACK")++bench.noiseBridgeCombatImpacts;
        }
      }
    }
  if(scenarioPath==nullptr || !bench.noiseTrace)return;
  if(bench.movableMeasuring) {++bench.noiseMeasuredImpacts;return;}
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
  if(world)for(const auto& listener:bench.noiseListeners) {
    if(listeners.size()>1)listeners+=',';
    const auto head=listener.npc->mapHeadBone();
    const auto occlusion=world->physic()->soundOclusion(head,noise.position);
    listeners+="{\"role\":"+jsonStr(listener.role)+",\"npc_id\":"+std::to_string(noiseNpcId(listener.npc))+
      ",\"distance_cm\":"+ragdollNumber((head-noise.position).length())+
      ",\"occlusion\":"+ragdollNumber(occlusion)+",\"effective_radius_cm\":"+
      ragdollNumber(noise.radius*(1.f-occlusion))+"}";
    }
  event("physics_noise","\"phase\":"+jsonStr(bench.noisePhase)+",\"t_game_ms\":"+
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
  if(bench.noiseBridgeDebug && &npc==bench.noiseHost)
    if(auto* world=Gothic::inst().world())bench.noiseBridgeHeard.push_back(world->tickCount());
  if(scenarioPath==nullptr || !bench.noiseTrace)return;
  if(bench.movableMeasuring) {
    ++bench.noiseMeasuredHeard;
    for(auto& listener:bench.noiseListeners)
      if(listener.npc==&npc)++listener.heard;
    return;
    }
  auto* world=Gothic::inst().world();if(!world)return;
  const auto id=noiseNpcId(&npc);
  ++bench.noisePerceptions[id];
  std::string_view role;
  for(auto& listener:bench.noiseListeners)
    if(listener.npc==&npc) {++listener.heard;role=listener.role;}
  event("physics_noise_heard","\"phase\":"+jsonStr(bench.noisePhase)+",\"t_game_ms\":"+
    std::to_string(world->tickCount())+",\"npc_id\":"+std::to_string(id)+",\"role\":"+jsonStr(role)+
    ",\"body\":"+std::to_string(noise.body)+",\"position_cm\":"+ragdollVector(noise.position)+
    ",\"perception\":"+std::to_string(int(PERC_ASSESSQUIETSOUND))+
    ",\"callback_dispatched\":"+(callbackDispatched?"true":"false")+
    ",\"culprit\":"+(noise.culprit?std::to_string(noiseNpcId(noise.culprit)):"null"));
  }

void TestBench::configure(std::string scenario, std::string output, int64_t seedOverride) {
  stopBodyLive();
  npcCharacterStopMotionTrace(Gothic::inst().world());
  KnownRoadsBench::reset();
  NavigationMemoryBench::reset();
  NavigationEyeBench::reset();
  NpcKeysBench::reset();
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
  if(bodyTraceAlive) {
    if(bench.monsterCharacterRoute.npc==npc)bench.monsterCharacterRoute.npc=nullptr;
    for(auto& actor:bench.monsterCharacterExtras)if(actor.npc==npc)actor.npc=nullptr;
    if(bench.characterIdle.actor==npc)bench.characterIdle.actor=nullptr;
    if(bench.characterIdle.fixtureNpc==npc)bench.characterIdle.fixtureNpc=nullptr;
    bench.npcCharacterGoals.erase(npc);bench.npcCharacterWalkers.erase(npc);
    std::erase_if(bench.npcCharacterTraceActors,[npc](const auto& actor){return actor.npc==npc;});
    }
  if(bodyTraceAlive && bench.noiseTrace)bench.noiseNpcIds.erase(npc);
  if(bodyTracing() || (ragdollTrace && bodyTraceAlive && bench.started && !bench.done)) {
    bench.bodyIds.erase(npc);
    for(auto it=bench.savedRagdollNpcs.begin();it!=bench.savedRagdollNpcs.end();) {
      if(it->second.npc==npc) it=bench.savedRagdollNpcs.erase(it); else ++it;
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
    if(bench.arrowKoOnHit && std::string_view(text).starts_with("arrow hit ") && bench.arrowNpc!=nullptr &&
       bench.arrowNpc->arrowPhysicsProbe().hits>bench.arrowSeenHits && !bench.arrowNpc->isDown()) {
      // This note runs inside real projectile damage, after queueArrowPhysics
      // and before the next sequential native prepare. Ordinary arrows kill;
      // only the benchmark forces KO to exercise the same pending-hit handoff.
      bench.arrowKoOnHit=false;
      bench.arrowNpc->setRagdollDown(false);
      event("arrow_reaction_forced_ko","\"case\":"+jsonStr(bench.arrowCase)+
        ",\"hits\":"+std::to_string(bench.arrowNpc->arrowPhysicsProbe().hits));
      }
    }
  }

void TestBench::ownMeshArrowHit(uint32_t zenId,const Vec3& point,const Vec3& normal) {
  if(!isActive() || !bench.started || bench.done || bench.ownMeshZen==uint32_t(-1))return;
  const auto delta=point-bench.ownMeshAxis;
  const bool matched=zenId==bench.ownMeshZen;
  if(matched)++bench.ownMeshArrowHits;
  event("own_mesh_arrow_hit","\"zen_id\":"+std::to_string(zenId)+
    ",\"expected_zen_id\":"+std::to_string(bench.ownMeshZen)+",\"matched\":"+(matched?"true":"false")+
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
  if(bench.slow<=1.f)
    return bench.frameMs;
  bench.slowAcc += uint64_t(float(bench.frameMs*1000)/bench.slow);
  const uint64_t dtGame = bench.slowAcc/1000;
  bench.slowAcc -= dtGame*1000;
  return dtGame;
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
  if(scenarioPath!=nullptr && bench.started && !bench.done && bench.waterObserver && bench.ctrl)
    bench.ctrl->clearInput();
  // The live bridge stand has no bench program, but uses the same real sword
  // animation and its normal hit frame as the scripted destruction fixture.
  if(scenarioPath==nullptr && bench.destructionTraceDebug && bench.destructionMeleeRunning && dt!=0) {
    auto* world=Gothic::inst().world();
    if(world && world==bench.destructionTraceWorld)destructionMeleeTick(*world);
    else {bench.destructionTraceDebug=false;bench.destructionMeleeRunning=false;}
    }
  const bool destructionCast=bench.destructionTrace &&
                             (bench.destructionCase=="fireball" || bench.destructionCase=="five");
  const bool movableCast=bench.movableTrace && bench.movablePhase=="blast";
  const bool waterCast=bench.waterReady && !bench.spellActors.empty() && bench.spellActors[0].role=="water_target";
  if(scenarioPath!=nullptr && bench.started && !bench.done && (destructionCast || movableCast || waterCast) && !bench.spellActors.empty()) {
    // PlayerControl refreshes auto-focus after each world step. Keep this
    // fixture's selected native target until commitSpell has emitted the real
    // projectile, so a nearby tavern host cannot steal the cast mid-animation.
    if(auto* world=Gothic::inst().world();world && world->player())world->player()->setTarget(bench.spellActors[0].npc);
    }
  if(scenarioPath==nullptr || !bench.started || bench.done || !bench.characterRideMoving || !bench.characterPlatform || dt==0)return;
  auto matrix=bench.characterRideMatrixStart;
  if(bench.characterRideRotating) {
    bench.characterRideAngle+=25.f*float(dt)*.001f;
    matrix.translate(bench.characterRidePivot);matrix.rotate(bench.characterRideAngle,0,1,0);matrix.translate(-bench.characterRidePivot);
    }
  else {
    bench.characterRideTravel+=80.f*float(dt)*.001f;
    matrix.translate(bench.characterRideDirection*bench.characterRideTravel);
    }
  bench.characterPlatformMatrix=matrix;
  bench.characterPlatform->setObjMatrix(matrix);bench.characterPlatformVisual->setObjMatrix(matrix);
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
std::function<bool(std::string_view,std::string_view)> TestBench::aiReached;
std::function<bool(World&,std::string_view,float)> TestBench::stuckBegin;
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
  if(bench.monsterCharacterRenderRequest==&npc) {
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
  if(bench.destructionSkinRequest==&npc && mesh.protoMesh()!=nullptr)
    destructionSkinSample(npc,*mesh.protoMesh(),npc.visual.transform(),npc.visual.pose());
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

  auto world  = Gothic::inst().world();
  auto player = Gothic::inst().player();
  KnownRoadsBench::tick(Gothic::inst().checkLoading()==Gothic::LoadState::Idle?world:nullptr);
  if(world==nullptr || player==nullptr || Gothic::inst().checkLoading()!=Gothic::LoadState::Idle) {
    renderSound(dt);
    return false;
    }
  if(!bench.started) {
    bench.started = true;
    FootIk::setProbing(true);
    say("start");
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
        ++bench.pc;waterInitial(*world,*player,ctrl,args[2]);
        }
      else if(args.size()==3 && args[0]=="water-bodies" && args[1]=="initial") {
        ++bench.pc;waterBodiesInitial(*world,*player,args[2]);
        }
      }
    renderSound(dt);
    return false;
    }
  if(dt==0)
    return false;
  NavBench::tick(*world);
  NavDemos::tick(*world,dt);
  NavigationEyeBench::tick(world->tickCount());
  NavigationMemoryBench::tick(*world);
  NpcKeysBench::tick(*world,*player,ctrl,dt);
  arrowFields(*world);
  stickingFields(*world);
  spellFields(*world);
  destructionFields(*world);
  if(bench.destructionSupportTrace && bench.destructionTrace &&
     world->tickCount()-bench.destructionStart>=bench.destructionSupportNext) {
    bench.destructionSupportNext=world->tickCount()-bench.destructionStart+1000;
    destructionSupportSample(*world);
    }
  destructionShoot(*world,*player);
  destructionMeleeTick(*world);
  if(bench.destructionFarmNpc) {
    bench.destructionFarmNpcTravel=std::max(bench.destructionFarmNpcTravel,
      (bench.destructionFarmNpc->position()-bench.destructionFarmNpcStart).length());
    if(bench.destructionFarmNpcGoal)
      bench.destructionFarmNpcBest=std::min(bench.destructionFarmNpcBest,
        (bench.destructionFarmNpc->position()-bench.destructionFarmNpcGoal->pos).length());
    }
  movableFields(*world);
  ownMeshFields(*world,*player);
  ownMeshChestFields(*world,*player);
  waterFields(*world);
  waterFilmTick(*world);
  clothFields(*world);
  movableCandleFields(*world);
  noiseFields(*world);
  if(bench.spellReleaseAt && world->tickCount()>=bench.spellReleaseAt) {
    player->endCastSpell(false);bench.spellReleaseAt=0;
    event("spell_physics_release","\"id\":"+std::to_string(bench.spellCastingId));
    }
  if(ragdollPerf) {
    const auto now=std::chrono::steady_clock::now();
    FrameProfile::observer(dt,now);
    FrameProfile::setActors(bench.crew);
    bench.perfWallMs=bench.hasPerfFrame ? std::chrono::duration<float,std::milli>(now-bench.lastPerfFrame).count() : 0;
    bench.lastPerfFrame=now; bench.hasPerfFrame=true;
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
  characterFields(*world,*player,dt,ctrl);
  monsterCharacterFields(*world,dt);
  npcCharacterFields(*world,*player,dt);
  npcCharacterMotionTrace(*world,dt);
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
  if(bench.birthActor!=nullptr && !bench.birthActor->isDown()) {
    const Bench::BirthLive live{bench.birthActor->position(),world->tickCount(),bench.birthActor->isInAir()};
    if(bench.birthHistory.empty() || live.tick>bench.birthHistory.back().tick) {
      bench.birthHistory.push_back(live); if(bench.birthHistory.size()>4) bench.birthHistory.pop_front();
      event("ragdoll_birth_live","\"label\":"+jsonStr(bench.birthLabel)+",\"actor_id\":"+std::to_string(world->npcId(bench.birthActor))+
            ",\"t_game_ms\":"+std::to_string(live.tick)+",\"in_air\":"+(live.inAir ? "true" : "false")+
            ",\"position_cm\":"+ragdollVector(live.position));
      }
    }
  if(bench.birthContinuation!=nullptr && world->tickCount()>bench.birthTick) {
    const auto* body=bench.birthContinuation->ragdollBody();
    if(body!=nullptr)
      event("ragdoll_birth_continuation","\"label\":"+jsonStr(bench.birthContinuationLabel)+",\"actor_id\":"+
            std::to_string(world->npcId(bench.birthContinuation))+",\"t_game_ms\":"+std::to_string(world->tickCount())+
            ",\"pelvis_y_cm\":"+std::to_string(body->pelvisTransform().data()[13])+",\"native_parts\":"+std::to_string(body->partCount()));
    else event("ragdoll_error","\"reason\":\"birth body missing on the next native tick\"");
    bench.birthContinuation=nullptr;
    }
  if((ragdollPerf || bench.ragdollArenaCamera)
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
     && !AppCadence::normalClock()
#endif
     ) {
    // Coverage is sampled after the actual world step and BEFORE fixture commands
    // can wake bodies. A post-impulse "awake" flag alone could hide a sleeping tick.
    bench.perfPhysicalBeforeImpulse=0; bench.perfSleepingBeforeImpulse=0;
    for(const auto* npc:bench.crew)
      if(const auto* body=npc->ragdollBody()) {
        ++bench.perfPhysicalBeforeImpulse;
        if(body->isSleeping()) ++bench.perfSleepingBeforeImpulse;
        }
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  if(bench.clothMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    if(bench.clothPhysics.size()<20000)
      bench.clothPhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.clothOverflow=true;
    }
  if(bench.movableMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    bench.noiseMeasuredRays+=stats.rays;
    if(bench.movablePhysics.size()<20000)
      bench.movablePhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.movableOverflow=true;
    }
  if(bench.livingMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    if(bench.livingPhysics.size()<20000)
      bench.livingPhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.livingOverflow=true;
    }
#endif
  run(*world,*player,ctrl);
  renderSound(dt);
  if(bench.done)
    return false;
  if(bench.ragdollChurn) {
    for(size_t i=0;i<bench.crew.size();++i)
      if(bench.crew[i]->ragdollBody()!=nullptr) {
        const float sign=((bench.ragdollPulseFrame+2)/5+i)%2==0 ? 1.f : -1.f;
        bench.crew[i]->addRagdollImpulse({50.f*sign,5.f,25.f*sign});
        }
    ++bench.ragdollPulseFrame;
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
  for(auto& [n,at]:bench.archers) {
    // the archer harness: where it was set, at the target, the bow drawn, arrows; an arrow every time it
    // can (no fight AI, no perception's wait)
    if(n==nullptr || bench.archerAt==nullptr || n->isDown())
      continue;
    const Vec3 d = bench.archerAt->position()-at;
    n->setPosition(at);
    n->setDirection(Vec3(d.x,0,d.z));
    n->updateTransform();
    n->setTarget(bench.archerAt);
    const auto ws = n->weaponState();
    if(auto rw = n->currentRangedWeapon(); rw!=nullptr && n->itemCount(size_t(rw->handle().munition))<5)
      n->addItem(size_t(rw->handle().munition),20);
    if(ws!=WeaponState::Bow && ws!=WeaponState::CBow) {
      n->drawWeaponBow();
      continue;
      }
    if(!bench.archerDice && n->archerAi().isDriving(*n))
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
  if(bench.aimNpc!=nullptr || (bench.aimMouse && bench.aimFixed)) {
    Vec3 at = bench.aimAt;
    if(bench.aimNpc!=nullptr && !aimPoint(*bench.aimNpc,bench.aimZone,bench.aimLeg,at) &&
       !bench.aimNpc->hitZoneCenter(HitZones::Body,at))
      at = bench.aimNpc->collosionCenter();
    if(bench.aimNpc!=nullptr && !bench.aimRaw) {
      // as an archer does: over it by how much the arrow drops on the way (at full draw), from the nock
      const auto st = Archery::stats(*player,player->currentRangedWeapon());
      Vec3       nock, dir;
      if(!player->arrowRay(nock,dir))
        nock = player->position() + Vec3(0,145,0);
      at = Archery::launch(nock,at,Archery::speedAt(st,1.f));
      }
    auto cam = Gothic::inst().camera();
    if(!bench.aimMouse) {
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
  if(bench.arrowNpc!=nullptr && !bench.arrowAim.empty()) {
    Vec3 at;
    if(arrowAimPoint(*bench.arrowNpc,at)) {
      Vec3 nock,direction;
      if(!player->arrowRay(nock,direction)) nock=player->position()+Vec3(0,145,0);
      const auto stats=Archery::stats(*player,player->currentRangedWeapon());
      ctrl.setAimOverride(true,Archery::launch(nock,at,Archery::speedAt(stats,bench.arrowDraw)));
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

  if(bench.ragdollCoverageActive) {
    auto sample=ragdollCoverageSample(*world);
    sample.segment=bench.ragdollCoverageMode; sample.frame=bench.ragdollCoverageFrames++;
    for(const auto* npc:bench.crew) arenaModelProof(*world,*npc,size_t(sample.frame));
    if(!arenaModeMatches(sample,bench.ragdollCoverageMode) && !bench.ragdollCoverageFailed) {
      bench.ragdollCoverageFailed=true;
      event("ragdoll_error","\"reason\":\"untimed twenty-body coverage failed\",\"coverage_frame\":"+std::to_string(sample.frame));
      }
    bench.ragdollCoverage.push_back(std::move(sample));
    if(bench.ragdollCoverageFrames==1020) {
      bench.ragdollCoverageActive=false;
      event("ragdoll_coverage_complete","\"mode\":"+jsonStr(bench.ragdollCoverageMode)+",\"frames\":1020,\"pass\":"+
            (bench.ragdollCoverageFailed ? "false" : "true"));
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

bool TestBench::withUi() {
  return isActive() && bench.ui;
  }

bool TestBench::ragdollLootOpened(const Npc& player, const Npc& target) {
  if((!ragdollTrace && !bench.waterBodiesReady) || !bodyTraceAlive || scenarioPath==nullptr || !bench.started || bench.done || bench.expectedRagdollLoot!=&target)
    return false;
  auto* world=Gothic::inst().world();
  if(world==nullptr)
    return false;
  bench.ragdollLootDidOpen=true;
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

// Closest points of two closed segments. The first parameter also locates the closest
// point along the actual camera ray; capsules are the same posed shapes as arrow hits.
float aimSegmentDistance(const Vec3& p, const Vec3& q, const Vec3& a, const Vec3& b, float& along) {
  const Vec3 u=q-p, v=b-a, w=p-a;
  const float uu=Vec3::dotProduct(u,u), vv=Vec3::dotProduct(v,v);
  const float uv=Vec3::dotProduct(u,v), uw=Vec3::dotProduct(u,w), vw=Vec3::dotProduct(v,w);
  float s=0, t=0;
  if(uu<=1e-6f) {
    t=vv>1e-6f ? std::clamp(vw/vv,0.f,1.f) : 0.f;
    }
  else if(vv<=1e-6f) {
    s=std::clamp(-uw/uu,0.f,1.f);
    }
  else {
    const float den=uu*vv-uv*uv;
    if(den>1e-6f)
      s=std::clamp((uv*vw-uw*vv)/den,0.f,1.f);
    t=(uv*s+vw)/vv;
    if(t<0) { t=0; s=std::clamp(-uw/uu,0.f,1.f); }
    if(t>1) { t=1; s=std::clamp((uv-uw)/uu,0.f,1.f); }
    }
  along=s;
  return (p+u*s-a-v*t).length();
  }

// Exact segment/AABB distance: between crossings of the six box planes the squared
// distance is one quadratic. The runtime attachment matrix turns this into an OBB test.
float aimBoxDistance(const Vec3& from, const Vec3& to, const Vec3 (&box)[2]) {
  const float p[3]={from.x,from.y,from.z}, d[3]={to.x-from.x,to.y-from.y,to.z-from.z};
  const float low[3]={box[0].x,box[0].y,box[0].z}, high[3]={box[1].x,box[1].y,box[1].z};
  float cuts[8]={0,1};
  size_t count=2;
  for(size_t i=0;i<3;++i)
    if(std::abs(d[i])>1e-6f)
      for(float bound:{low[i],high[i]}) {
        const float t=(bound-p[i])/d[i];
        if(t>0 && t<1) cuts[count++]=t;
        }
  // at most 8: insertion sort (std::sort over the array trips GCC 15's -Warray-bounds on Linux)
  for(size_t a=1;a<count;++a)
    for(size_t b=a;b>0 && cuts[b-1]>cuts[b];--b)
      std::swap(cuts[b-1],cuts[b]);
  auto distance2=[&](float t) {
    float value=0;
    for(size_t i=0;i<3;++i) {
      const float x=p[i]+d[i]*t;
      const float gap=x-std::clamp(x,low[i],high[i]);
      value+=gap*gap;
      }
    return value;
    };
  float closest=distance2(0);
  for(size_t k=1;k<count;++k) {
    const float mid=(cuts[k-1]+cuts[k])*0.5f;
    float dd=0, pd=0;
    for(size_t i=0;i<3;++i) {
      const float x=p[i]+d[i]*mid;
      if(x>=low[i] && x<=high[i]) continue;
      dd+=d[i]*d[i];
      pd+=(p[i]-(x<low[i] ? low[i] : high[i]))*d[i];
      }
    const float t=dd>1e-6f ? std::clamp(-pd/dd,cuts[k-1],cuts[k]) : mid;
    closest=std::min(closest,std::min(distance2(t),distance2(cuts[k])));
    }
  return std::sqrt(closest);
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
  if(pm.w()>0 && bench.livingDebug) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    if(!bench.overlay) drawZones(cv,Gothic::inst().camera());
    const auto counts=livingCounts();char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"LIVING BODIES: "+bench.livingPhase,2,Text);
    std::snprintf(label,sizeof(label),"LIVE %zu  NATIVE KIN %zu / %zu PARTS  DYNAMIC %zu",counts.alive,counts.kinematic,counts.parts,counts.dynamic);
    cv.text(6,26,label,2,Text);cv.text(6,46,"OVERLAY = GAME HIT ZONES (NOT NATIVE SHAPES)",2,Text);
    }
  if(pm.w()>0 && bench.arrowTrace && bench.arrowNpc!=nullptr) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    const auto& npc=*bench.arrowNpc;
    const auto* body=npc.livingBody()!=nullptr ? npc.livingBody() : npc.ragdollBody();
    const auto& hit=npc.arrowPhysicsProbe();
    const auto* skeleton=npc.visualSkeleton();
    const auto bone=skeleton!=nullptr && hit.bone<skeleton->nodes.size() ?
                    std::string_view(skeleton->nodes[hit.bone].name) : std::string_view("-");
    const char* mode=body==nullptr ? "NONE" : (body->isKinematic() ? "KINEMATIC" :
                     (body->reactionWeight()>0.f ? "ACTIVE MOTORS" : "LIMP"));
    char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"ARROW PHYSICS: "+bench.arrowCase,2,Text);
    std::snprintf(label,sizeof(label),"NATIVE %s  WEIGHT %.3f  PARTS %zu",mode,
                  double(body!=nullptr ? body->reactionWeight() : 0.f),body!=nullptr ? body->partCount() : 0);
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"HIT %.*s  IMPULSE %.2f NS",int(std::min<size_t>(bone.size(),38)),bone.data(),double(hit.impulseNs.length()));
    cv.text(6,46,label,2,Text);
    }
  if(pm.w()>0 && bench.stickingTrace) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    const auto* npc=bench.arrowNpc;auto* world=Gothic::inst().world();
    float error=0;
    if(npc)
      for(size_t i=0;i<npc->stuckArrowCount();++i) {
        Npc::ArrowPinProbe pin;
        if(!npc->stuckArrowProbe(i,pin)) continue;
        const auto expected=npc->renderedBone(pin.bone)*pin.local;
        error=std::max(error,Vec3(pin.world.at(3,0)-expected.at(3,0),pin.world.at(3,1)-expected.at(3,1),pin.world.at(3,2)-expected.at(3,2)).length());
        }
    char label[200];cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"ARROW STICKING: "+bench.stickingCase,2,Text);
    std::snprintf(label,sizeof(label),"ACTUAL DRAW PINS  NPC %zu  WORLD %zu  ERROR %.4f CM",npc?npc->stuckArrowCount():0,
      world?world->stuckArrowCount():0,double(error));
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"BONE LOCAL -> RENDER POSE  %s  NATIVE BODIES %zu",npc && npc->isDown()?"LIMP":"LIVING",
      world?world->physic()->physicsStats().bodies:0);
    cv.text(6,46,label,2,Text);
    }
  if(pm.w()>0 && bench.spellTrace) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    auto* world=Gothic::inst().world();char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});cv.text(6,6,"SPELL PHYSICS: "+bench.spellCase,2,Text);
    std::snprintf(label,sizeof(label),"REAL CAST %d  FIELDS %zu  NATIVE BODIES %zu",bench.spellCastingId,
      world?world->spellFieldCount():0,world?world->physic()->physicsStats().bodies:0);cv.text(6,26,label,2,Text);
    if(!bench.spellActors.empty()) {
      const auto& npc=*bench.spellActors[0].npc;
      std::snprintf(label,sizeof(label),"HUMAN %.0f KG  SPEED %.0f CM/S  FROZEN %d  PROPS %zu",double(npc.physicsMass()),
        double(npc.spellVelocity().length()),int(npc.spellFrozen()),bench.spellItems.size());cv.text(6,46,label,2,Text);
      }
    }
  if(pm.w()>0 && bench.questTrace && bench.questNpc!=nullptr) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    cv.rect(0,0,std::min(cv.w,640),66,{0,0,0,190});
    cv.text(6,6,bench.questCase,2,Text);
    const auto& npc=*bench.questNpc;const auto& q=npc.ragdollQuestReturnState();char label[160];
    std::snprintf(label,sizeof(label),"IMMORTAL %d GUARD %d KO %d RETURN %u",int(npc.isImmortal()),
      Gothic::settingsGetI("GAME","ragdoll_quest_guard"),int(npc.isUnconscious()),npc.ragdollQuestReturnCount());
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"REASON %s DROP %.0f CM",questReason(q.hazards),double(bench.questStable.y-npc.position().y));
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
  auto& route=bench.monsterCharacterRoute;
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
