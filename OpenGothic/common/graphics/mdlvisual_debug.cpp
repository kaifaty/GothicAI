#include "mdlvisual.h"

#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/animmath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Tempest;

// the bones of the model at a frame of the animation (f past its end: the bind pose), in the model's frame
static void modelFrame(const Skeleton& skeleton, const Animation::AnimData& d, uint32_t f,
                       std::vector<Tempest::Matrix4x4>& world) {
  auto& nodes = skeleton.nodes;
  std::vector<Tempest::Matrix4x4> local(nodes.size());
  world.resize(nodes.size());
  for(size_t i=0; i<nodes.size(); ++i)
    local[i] = nodes[i].tr;
  for(size_t i=0; i<d.nodeIndex.size() && f<d.numFrames; ++i)
    if(d.nodeIndex[i]<nodes.size())
      local[d.nodeIndex[i]] = mkMatrix(d.samples[size_t(f)*d.nodeIndex.size()+i]);
  std::vector<bool> done(nodes.size(),false);
  for(size_t i=0; i<nodes.size(); ++i) {
    size_t chain[Resources::MAX_NUM_SKELETAL_NODES];
    size_t n = 0;
    for(size_t b=i; b<nodes.size() && !done[b] && n<nodes.size(); b=nodes[b].parent)
      chain[n++] = b;
    while(n>0) {
      const size_t b = chain[--n];
      const size_t p = nodes[b].parent;
      world[b] = p<nodes.size() ? world[p]*local[b] : local[b];
      done[b]  = true;
      }
    }
  }

std::string MdlVisual::animInfo(std::string_view name) const {
  const Animation::Sequence* sq = solver.solveFrm(name);
  if(sq==nullptr || sq->data==nullptr)
    return std::string(name) + ": none";
  const auto& d  = *sq->data;
  const float ms = sq->totalTime();
  char buf[512];
  std::snprintf(buf,sizeof(buf),"%s: layer %u %s frames %u fps %.1f %.0f ms nodes %zu move %d tr %.0f %.0f %.0f (%.0f cm/s) next %s",
                sq->name.c_str(),unsigned(sq->layer),sq->animCls==Animation::Loop ? "loop" : "trans",unsigned(d.numFrames),
                double(d.fpsRate),double(ms),d.nodeIndex.size(),int(d.hasMoveTr),double(d.moveTr.x),double(d.moveTr.y),
                double(d.moveTr.z),ms>0.f ? double(d.moveTr.length()/ms*1000.f) : 0.0,sq->next.c_str());
  std::string out = buf;
  // the blow's frames (the attack windows)
  for(const auto& e:d.events)
    if(e.type==asset::MdsEventType::OPTIMAL_FRAME) {
      out += "; optimal";
      for(auto f:e.frames)
        out += " " + std::to_string(f);
      }
  if(skeleton==nullptr || d.nodeIndex.empty() || d.numFrames==0)
    return out;
  // the nodes it moves (not all: an upper body only animation)
  auto& nodes = skeleton->nodes;
  if(d.nodeIndex.size()<nodes.size()) {
    out += "; moves";
    for(auto i:d.nodeIndex)
      if(i<nodes.size())
        out += " " + nodes[i].name;
    }
  // the hips at 0, 1/4, 1/2, 3/4: the thighs' line, degrees of yaw in the model (the bind pose 0); the
  // root's height
  const size_t lt = skeleton->findNode("BIP01 L THIGH");
  const size_t rt = skeleton->findNode("BIP01 R THIGH");
  if(lt>=nodes.size() || rt>=nodes.size())
    return out;
  std::vector<Tempest::Matrix4x4> world;
  auto yaw = [&](const Tempest::Matrix4x4* m) {
    const float dx = m[lt].at(3,0)-m[rt].at(3,0);
    const float dz = m[lt].at(3,2)-m[rt].at(3,2);
    return std::atan2(dz,dx)*180.f/float(M_PI);
    };
  auto pose = [&](uint32_t f) { modelFrame(*skeleton,d,f,world); };
  pose(uint32_t(-1));
  std::snprintf(buf,sizeof(buf),"; hips bind %.0f, at frames",double(yaw(world.data())));
  out += buf;
  for(uint32_t q=0; q<4; ++q) {
    const uint32_t f = std::min(d.numFrames-1,d.numFrames*q/4);
    pose(f);
    std::snprintf(buf,sizeof(buf)," %u:%.0f/%.0f",unsigned(f),double(yaw(world.data())),double(world[0].at(3,1)));
    out += buf;
    }
  return out;
  }

std::string MdlVisual::bladePath(std::string_view name, float length) const {
  // the blade's tip (-length along the hand's x, as the weapon meshes lie) at each frame, in the model's
  // frame, and how fast it goes; the root bone, whose x and z the game keeps at the actor's position (the
  // actor goes the root's way instead); the frames of the fight tags as the MDS gives them
  const Animation::Sequence* sq = solver.solveFrm(name);
  if(sq==nullptr || sq->data==nullptr || skeleton==nullptr)
    return std::string(name) + ": none";
  const auto& d = *sq->data;
  const size_t hand = skeleton->findNode("ZS_RIGHTHAND");
  const size_t root = skeleton->findRootNode();
  if(hand>=skeleton->nodes.size() || root>=skeleton->nodes.size() || d.numFrames==0)
    return std::string(name) + ": no hand";
  char buf[256];
  std::snprintf(buf,sizeof(buf),"%s: frames %u..%u (%u) fps %.1f reverse %d",sq->name.c_str(),unsigned(d.firstFrame),
                unsigned(d.lastFrame),unsigned(d.numFrames),double(d.fpsRate),int(sq->reverse));
  std::string out = buf;
  for(const auto& e:d.events) {
    const char* tag = e.type==asset::MdsEventType::OPTIMAL_FRAME ? "optimal" :
                      e.type==asset::MdsEventType::HIT_END       ? "hit_end" :
                      e.type==asset::MdsEventType::COMBO_WINDOW  ? "window"  : nullptr;
    if(tag==nullptr)
      continue;
    out += std::string("; ") + tag;
    for(auto f:e.frames)
      out += " " + std::to_string(f);
    }
  std::vector<Tempest::Matrix4x4> world;
  Tempest::Vec3 previous;
  for(uint32_t f=0; f<d.numFrames; ++f) {
    modelFrame(*skeleton,d,f,world);
    Tempest::Vec3 tip(-length,0,0), grip;
    world[hand].project(tip);
    world[hand].project(grip);
    const float speed = f>0 ? (tip-previous).length()*d.fpsRate : 0.f;
    previous = tip;
    std::snprintf(buf,sizeof(buf),"; %u: tip %.0f %.0f %.0f grip %.0f %.0f %.0f root %.0f %.0f %.0f %.0f cm/s",
                  unsigned(f),double(tip.x),double(tip.y),double(tip.z),double(grip.x),double(grip.y),double(grip.z),
                  double(world[root].at(3,0)),double(world[root].at(3,1)),double(world[root].at(3,2)),double(speed));
    out += buf;
    }
  return out;
  }

std::string MdlVisual::bowDump() const {
  // points of the bow and the bow hand in the frame of the arrow: x along it from the slot it sits in
  // (forward), y up across it, z to the side
  auto proto = bow.view.protoMesh();
  if(proto==nullptr || skeleton==nullptr || bow.boneId>=skInst->boneCount())
    return "bow: none";
  const size_t aSlot = skeleton->findNode("ZS_RIGHTHAND");
  if(aSlot>=skInst->boneCount())
    return "bow: no arrow slot";
  const auto& am = skInst->bone(aSlot);
  const Vec3  o  = Vec3(am.at(3,0),am.at(3,1),am.at(3,2));
  const Vec3  ax = Vec3::normalize(Vec3(-am.at(0,0),-am.at(0,1),-am.at(0,2)));
  Vec3        up = Vec3(0,1,0) - ax*Vec3::dotProduct(Vec3(0,1,0),ax);
  up             = Vec3::normalize(up);
  const Vec3  sd = Vec3::crossProduct(ax,up);
  const auto& bm = skInst->bone(bow.boneId);
  std::string out;
  char        buf[200];
  auto put = [&](const char* name, Vec3 p) {
    p = p - o;
    std::snprintf(buf,sizeof(buf),"  %-14s %6.1f %6.1f %6.1f\n",name,double(Vec3::dotProduct(p,ax)),double(Vec3::dotProduct(p,up)),
                  double(Vec3::dotProduct(p,sd)));
    out += buf;
    };
  auto inBow = [&](Vec3 p) {
    bm.project(p);
    return p;
    };
  auto dirOf = [&](const Vec3& d) {
    return Vec3(bm.at(0,0)*d.x + bm.at(1,0)*d.y + bm.at(2,0)*d.z,
                bm.at(0,1)*d.x + bm.at(1,1)*d.y + bm.at(2,1)*d.z,
                bm.at(0,2)*d.x + bm.at(1,2)*d.y + bm.at(2,2)*d.z);
    };
  const Vec3* bb = proto->bboxMesh;
  std::snprintf(buf,sizeof(buf),"bow %s: bbox %.1f %.1f %.1f .. %.1f %.1f %.1f (mesh)\n",proto->fname.c_str(),
                double(bb[0].x),double(bb[0].y),double(bb[0].z),double(bb[1].x),double(bb[1].y),double(bb[1].z));
  out += buf;
  const Vec3 mid = (bb[0]+bb[1])*0.5f;
  for(int i=0; i<3; ++i) {
    const Vec3 e  = i==0 ? Vec3(1,0,0)        : (i==1 ? Vec3(0,1,0)        : Vec3(0,0,1));
    const Vec3 lo = i==0 ? Vec3(bb[0].x,mid.y,mid.z) : (i==1 ? Vec3(mid.x,bb[0].y,mid.z) : Vec3(mid.x,mid.y,bb[0].z));
    const Vec3 hi = i==0 ? Vec3(bb[1].x,mid.y,mid.z) : (i==1 ? Vec3(mid.x,bb[1].y,mid.z) : Vec3(mid.x,mid.y,bb[1].z));
    const Vec3 d = dirOf(e);
    std::snprintf(buf,sizeof(buf),"  mesh axis %d in the arrow frame: %.2f %.2f %.2f\n",i,double(Vec3::dotProduct(d,ax)),
                  double(Vec3::dotProduct(d,up)),double(Vec3::dotProduct(d,sd)));
    out += buf;
    char n0[16], n1[16];
    std::snprintf(n0,sizeof(n0),"bbox-%d",i);
    std::snprintf(n1,sizeof(n1),"bbox+%d",i);
    put(n0,inBow(lo));
    put(n1,inBow(hi));
    }
  put("grip (slot)",inBow(Vec3()));
  put("bbox middle",inBow((bb[0]+bb[1])*0.5f));
  for(auto& m:proto->morph) {
    std::snprintf(buf,sizeof(buf),"  morph %s: peak %.1f at %.1f %.1f %.1f move %.1f %.1f %.1f (mesh)\n",m.name.c_str(),double(m.peak),
                  double(m.peakAt.x),double(m.peakAt.y),double(m.peakAt.z),double(m.peakMove.x),double(m.peakMove.y),double(m.peakMove.z));
    out += buf;
    if(m.name==BowHeld) {
      put("string rest",inBow(m.peakAt));
      put("string drawn",inBow(m.peakAt+m.peakMove));
      }
    if(m.name==BowDraw::StringDrawn) {
      put("straight rest",inBow(m.peakAt));
      put("straight drawn",inBow(m.peakAt+m.peakMove));
      }
    }
  if(ammunition.boneId<skInst->boneCount()) {
    // the arrow (the bolt) as it lies: where the shot leaves from, along what
    const auto& m = skInst->bone(ammunition.boneId);
    const Vec3  d = Vec3::normalize(Vec3(-m.at(0,0),-m.at(0,1),-m.at(0,2)));
    std::snprintf(buf,sizeof(buf),"  ammo in %s%s, along %.3f %.3f %.3f (the arrow frame)\n",std::string(ammunition.bone).c_str(),
                  ammunition.view.isEmpty() ? " (not shown)" : "",double(Vec3::dotProduct(d,ax)),double(Vec3::dotProduct(d,up)),
                  double(Vec3::dotProduct(d,sd)));
    out += buf;
    put("ammo",Vec3(m.at(3,0),m.at(3,1),m.at(3,2)));
    }
  for(auto name:{"BIP01 L HAND","BIP01 L FINGER0","BIP01 L FINGER1","BIP01 L FINGER2","BIP01 L FOREARM","BIP01 R HAND","BIP01 HEAD"}) {
    const size_t id = skeleton->findNode(name);
    if(id<skInst->boneCount()) {
      const auto& m = skInst->bone(id);
      put(name+6,Vec3(m.at(3,0),m.at(3,1),m.at(3,2)));
      }
    }
  return out;
  }

void MdlVisual::hitZonesPosed(std::vector<HitZones::Posed>& out) const {
  const Vec3 sx = Vec3(pos.at(0,0),pos.at(0,1),pos.at(0,2));
  hitZones.posed(skInst->transform(),skInst->boneCount(),sx.length(),out);
  }

std::string MdlVisual::hitZonesDump() const {
  const Vec3 sx = Vec3(pos.at(0,0),pos.at(0,1),pos.at(0,2));
  auto       body = view.protoMesh();
  return hitZones.dump(skInst->transform(),skInst->boneCount(),sx.length()) +
         hitZones.coverage(skInst->transform(),skInst->boneCount(),sx.length()) +
         " body " + (body!=nullptr ? body->fname : std::string("-"));
  }
