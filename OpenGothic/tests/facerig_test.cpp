// The own head's face (graphics/mesh/facerig): a synthetic head and profile - the weights summed and clamped, fade in
// and out, frames in time, one anim per layer, VISEME shapes set by hand, the voice quieting the mouth, the deformation
// (normals normalized, laid out through renderVertexIds with padding), no blend while the weights stay, profile
// rejections with their reason. The eyes (graphics/mesh/eyes) on a head with two balls: the balls turned rigidly
// around their fitted centres (positions and normals), the limits, convergence on a near point, the eyes turning back
// as the head turns, saccades by the main sequence with a bell of speed and no overshoot, smooth pursuit and catch-up
// jumps, the lids by the profile's table summed with the other weights, the same jumps for the same seed, rejections
// of the "eyes" section. CPU only: no device, no game data.
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/eyes.h"
#include "gltfmesh.h"

#include <algorithm>
#include <vector>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

bool close(float a, float b, float eps = 2e-3f) {
  return std::abs(a-b)<=eps;
  }

enum Target : uint32_t { JawOpen, BrowDown, EyeBlink, VisemeSil, VisemeAa };

GltfHeadMesh makeHead() {
  GltfHeadMesh head;
  head.mesh.positions = {{0,0,0},{10,0,0},{0,10,0},{0,0,10}};
  head.mesh.normals   = {{0,0,1},{0,0,1},{0,0,1},{0,0,1}};
  const auto target = [&](const char* name, std::vector<GltfHeadMesh::Delta> d) {
    head.targets.push_back({name,std::move(d)});
    };
  target("jawOpen",  {{0,{0,-2,0},{2,0,0}}});
  target("browDown", {{1,{0,0,-1},{}},{3,{0,0,0},{5e-4f,0,0}}});  // vertex 3: export noise, not blended
  target("eyeBlink", {{2,{0,-3,0},{}}});
  target("viseme_sil",{{0,{0,-.5f,0},{}}});
  target("viseme_aa",{{0,{0,-4,0},{}},{3,{0,-4,0},{}}});
  head.renderVertexIds = {3,0,UINT32_MAX,1,2,0};
  return head;
  }

const char* Profile = R"({
  "version": 1, "about": "synthetic", "mouth": ["jaw", "viseme_"],
  "anims": {
    "S_ANGRY":     {"layer": 1, "blend_in_ms": 1000, "blend_out_ms": 1000, "duration_ms": -1, "frame_ms": 1000,
                    "frames": [{"browDown": 0.8, "jawOpen": 0.5}]},
    "S_FRIENDLY":  {"layer": 1, "blend_in_ms": 1000, "blend_out_ms": 1000, "duration_ms": -1, "frame_ms": 1000,
                    "frames": [{"browDown": 0.4}]},
    "S_EXTRA":     {"layer": 3, "blend_in_ms": 0, "blend_out_ms": 0, "duration_ms": -1, "frame_ms": 1000,
                    "frames": [{"browDown": 0.6}]},
    "R_EYESBLINK": {"layer": 4, "blend_in_ms": 40, "blend_out_ms": 40, "duration_ms": 0, "frame_ms": 1000,
                    "frames": [{"eyeBlink": 1.0}]},
    "T_EAT":       {"layer": 1, "blend_in_ms": 0, "blend_out_ms": 0, "duration_ms": 2000, "frame_ms": 100,
                    "frames": [{"jawOpen": 0.0}, {"jawOpen": 1.0}]},
    "VISEME":      {"layer": 2, "blend_in_ms": 100, "blend_out_ms": 100, "duration_ms": -1, "frame_ms": 200,
                    "frames": [{"viseme_sil": 1.0}, {"viseme_aa": 0.5}, {"viseme_aa": 1.0}]}
  }
})";

void profileCases(const GltfHeadMesh& head, const FaceProfile& p) {
  require(p.anims.size()==6 && p.find("T_EAT")!=nullptr && p.find("S_NONE")==nullptr,"the profile must list its anims");
  require(p.mouth[JawOpen]==1 && p.mouth[VisemeAa]==1 && p.mouth[VisemeSil]==1 && p.mouth[BrowDown]==0 && p.mouth[EyeBlink]==0,
          "mouth targets must be found by the prefixes of their names");
  const auto& eat = *p.find("T_EAT");
  require(eat.duration==2000 && eat.frameMs==100 && eat.layer==1 && eat.blendIn==0,"timing must be read as given");
  require(p.find("R_EYESBLINK")->duration==0 && p.find("S_ANGRY")->duration==0,"-1 and 0 durations must leave the time to the caller");
  // VISEME: sil moves vertex 0 by 0.5, aa vertices 0 and 3 by 2 and 4, over the two vertices the anim moves
  const auto& v = p.find("VISEME")->strength;
  require(v.size()==3 && close(v[0],0.0625f) && close(v[1],0.5f) && close(v[2],1.f),
          "VISEME strengths must be the mean shift of a frame relative to the strongest one");

  const auto rejected = [&](const std::string& json, std::string_view reason) {
    try {
      (void)FaceProfile::parse(json,head);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,
              "the rejection must say why: want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"the profile must be rejected: "+std::string(reason));
    };
  const auto anim = [](const std::string& body) {
    return R"({"version":1,"mouth":["jaw"],"anims":{"S_ANGRY":)"+body+"}}";
    };
  const std::string good = R"({"layer":1,"blend_in_ms":100,"blend_out_ms":100,"duration_ms":-1,"frame_ms":1000,"frames":[{"browDown":1}]})";
  require(FaceProfile::parse(anim(good),head)!=nullptr,"the minimal profile must load");
  rejected("{\"version\":1,",                                "invalid JSON");
  rejected(R"({"version":2,"mouth":[],"anims":{"S_ANGRY":)"+good+"}}", "version must be 1");
  rejected(R"({"version":1,"mouth":[],"anims":{"S_ANGRY":)"+good+R"(},"extra":1})", "unknown field \"extra\"");
  rejected(R"({"version":1,"mouth":"jaw","anims":{"S_ANGRY":)"+good+"}}", "mouth");
  rejected(R"({"version":1,"mouth":[],"anims":{}})",       "anims must be a non-empty object");
  rejected(anim(R"({"layer":1,"blend_in_ms":100,"blend_out_ms":100,"duration_ms":-1,"frame_ms":1000,"frames":[{"nose":1}]})"),
           "anim \"S_ANGRY\" frame 0: unknown target \"nose\"");
  rejected(anim(R"({"layer":"1","blend_in_ms":100,"blend_out_ms":100,"duration_ms":-1,"frame_ms":1000,"frames":[{}]})"),
           "layer must be");
  rejected(anim(R"({"layer":1,"blend_in":100,"blend_out_ms":100,"duration_ms":-1,"frame_ms":1000,"frames":[{}]})"),
           "unknown field \"blend_in\"");
  rejected(anim(R"({"layer":1,"blend_in_ms":100,"blend_out_ms":100,"duration_ms":-1,"frame_ms":0,"frames":[{}]})"),
           "frame_ms must be above 0");
  rejected(anim(R"({"layer":1,"blend_in_ms":100,"blend_out_ms":100,"duration_ms":-2,"frame_ms":10,"frames":[{}]})"),
           "duration_ms");
  rejected(anim(R"({"layer":1,"blend_in_ms":100,"blend_out_ms":100,"duration_ms":-1,"frame_ms":10,"frames":[]})"),
           "frames must be a non-empty list");
  rejected(anim(R"({"layer":1,"blend_in_ms":100,"blend_out_ms":100,"duration_ms":-1,"frame_ms":10,"frames":[{"browDown":1.5}]})"),
           "must be a number 0..1");
  }

void rigCases(const GltfHeadMesh& head, const FaceProfile& p) {
  using R = FaceRig::Result;
  FaceRig none;
  none.setup(&head,nullptr);
  none.startAnim("S_ANGRY",1,uint64_t(-1),0);
  require(!none.isActive() && none.update(10)==R::Same && none.weight("S_ANGRY",10)==0,"a head without a profile shows no face");

  FaceRig rig;
  rig.setup(&head,&p);
  require(rig.update(0)==R::Rest && rig.update(1)==R::Same,"a neutral head sends the rest once");
  const auto& w = rig.weights();

  // fade in by blend_in_ms
  rig.startAnim("S_ANGRY",1,uint64_t(-1),1000);
  require(rig.update(1500)==R::Deformed && close(rig.weight("S_ANGRY",1500),0.5f),"S_ANGRY must be half in after 500 of 1000 ms");
  require(close(w[BrowDown],0.4f) && close(w[JawOpen],0.25f),"target weights must follow the anim's fade");
  require(rig.update(2000)==R::Deformed && close(w[BrowDown],0.8f) && close(w[JawOpen],0.5f),"S_ANGRY must be fully in at 1000 ms");

  // the deformation: normals normalized after the sum, laid out through renderVertexIds with padding
  const auto& d = rig.deformation();
  require(d.size()==head.renderVertexIds.size()*2,"the deformation must hold a position and a normal per drawn vertex");
  const float s = 1.f/std::sqrt(2.f);
  require(close(d[2].x,0) && close(d[2].y,-1) && close(d[2].z,0) && d[2].w==1.f,"vertex 0 must move by jawOpen 0.5");
  require(close(d[3].x,s) && close(d[3].y,0) && close(d[3].z,s) && d[3].w==0.f,"the blended normal must be normalized");
  require(d[4].x==0 && d[4].y==0 && d[4].z==0 && d[4].w==0 && d[5].x==0 && d[5].z==0 && d[5].w==0,
          "padding vertices must stay zero");
  require(close(d[0].z,10) && close(d[1].z,1),"an unmoved vertex must keep its base position and normal");
  require(d[1].x==0.f,"a normal delta below FaceRig::StillNormal must not be blended");
  require(close(d[6].x,10) && close(d[6].z,-.8f) && close(d[10].x,0) && close(d[10].y,-1),
          "every drawn vertex must take its logical vertex, a duplicated one too");

  // no blend while the weights stay
  FaceRig::Probe pr;
  rig.probe(pr,2000);
  const auto blends = pr.blends;
  require(rig.update(2000)==R::Same && rig.update(2600)==R::Same,"unchanged weights must not blend again");
  rig.probe(pr,2600);
  require(pr.valid && pr.blends==blends && pr.anims==1 && pr.targets==2 && pr.shown=="S_ANGRY 1.00",
          "the probe must show the active anims, their strength and the targets with a weight");

  // the sum of all active anims, clamped to 0..1
  rig.startAnim("S_EXTRA",1,uint64_t(-1),3000);
  require(rig.update(3000)==R::Deformed && close(w[BrowDown],1.f) && close(w[JawOpen],0.5f),"summed weights must be clamped to 1");
  rig.startAnim("S_EXTRA",1,0,3000);
  require(rig.weight("S_EXTRA",3000)==0,"an anim without blend_out_ms must stop at once");

  // one anim per layer: S_FRIENDLY fades in while S_ANGRY fades out from where it is
  rig.startAnim("S_FRIENDLY",1,uint64_t(-1),3000);
  rig.update(3500);
  require(close(rig.weight("S_ANGRY",3500),0.5f) && close(rig.weight("S_FRIENDLY",3500),0.5f),"the layer must cross-fade");
  require(close(w[BrowDown],0.6f) && close(w[JawOpen],0.25f),"the cross-fade must mix both anims");
  rig.update(4001);
  require(rig.weight("S_ANGRY",4001)==0 && close(w[BrowDown],0.4f) && w[JawOpen]==0,"the replaced anim must be gone");
  require(close(d[2].y,0) && close(d[3].x,0) && close(d[3].z,1) && close(d[10].y,0) && close(d[6].z,-.4f),
          "a vertex no shape moves any more must return to its neutral position and normal");

  // fade out by blend_out_ms, then the rest
  rig.startAnim("S_FRIENDLY",1,0,5000);
  require(close(rig.weight("S_FRIENDLY",5500),0.5f),"a stopped anim must fade out");
  require(rig.update(6001)==R::Rest && w[BrowDown]==0,"all weights zero must return to the rest");

  // frames in time by frame_ms, neighbours mixed; its own duration
  rig.startAnim("T_EAT",1,uint64_t(-1),7000);
  rig.update(7050);
  require(close(w[JawOpen],0.5f),"half a frame must mix two frames");
  require(rig.changed()==std::pair<uint32_t,uint32_t>(0,6),"after the rest the whole head must be sent");
  rig.update(7100);
  require(close(w[JawOpen],1.f),"the next frame must show at frame_ms");
  require(rig.changed()==std::pair<uint32_t,uint32_t>(1,6),"only the drawn vertices of moved vertices must be marked changed");
  rig.update(7150);
  require(close(w[JawOpen],0.5f),"the frames must loop");
  require(rig.weight("T_EAT",8999)==1 && rig.weight("T_EAT",9001)==0,"duration_ms must end the anim");
  rig.update(9001);

  // the blink: the time given plus blend_out_ms
  rig.startAnim("R_EYESBLINK",1,9050,9000);
  require(close(rig.weight("R_EYESBLINK",9020),0.5f) && close(rig.weight("R_EYESBLINK",9050),1.f) &&
          close(rig.weight("R_EYESBLINK",9070),0.5f) && rig.weight("R_EYESBLINK",9091)==0,"a blink must fade in, hold and fade out");

  // VISEME frames set by hand: frame0/frame1/alpha/intensity, gone when not set again
  rig.setShape("VISEME",1,2,0.25f,0.8f,10000);
  rig.update(10000);
  require(close(rig.weight("VISEME",10000),0.8f) && close(w[VisemeAa],0.5f),"a shape by hand must mix its frames and intensity");
  rig.setShape("VISEME",9,9,2.f,1.f,10100);
  rig.update(10100);
  require(close(w[VisemeAa],1.f),"frames and alpha by hand must be clamped");
  require(rig.weight("VISEME",10351)==0,"a shape no longer set must go");

  // the voice quiets the mouth of the other layers
  rig.startAnim("S_ANGRY",1,uint64_t(-1),11000);
  rig.update(12000);
  require(close(w[JawOpen],0.5f) && close(w[BrowDown],0.8f),"S_ANGRY must be in");
  rig.setShape("VISEME",0,0,0,1,12000);
  rig.update(12000);
  require(close(w[JawOpen],0.15f) && close(w[BrowDown],0.8f) && close(w[VisemeSil],1.f),
          "speech must multiply the other mouth targets by 1 - 0.7 s and leave the rest");
  rig.setShape("VISEME",0,0,0,0.5f,12010);
  rig.update(12010);
  require(close(w[JawOpen],0.325f) && close(w[VisemeSil],0.5f),"the quieting must follow the speech strength");

  // a new view of the same head gets the deformation again
  rig.setup(&head,&p);
  require(rig.update(12010)==R::Deformed,"invalidated view must receive the deformation again");
  }
}

// the eyes: a face of four vertices the lid shapes move, two balls of radius 1.5 at (10, +-3, 9) - x up, y across
// (+ the character's left), z forward as the head bone - and a flat part that is no ball
constexpr float Deg = 3.14159265f/180.f;
constexpr float EyeX = 10, EyeY = 3, EyeZ = 9, EyeR = 1.5f;
enum EyeTarget : uint32_t { LookDownL, LookDownR, LookUpL, LookUpR, LookInL, LookInR, LookOutL, LookOutR, BlinkL, BlinkR };

GltfHeadMesh makeEyeHead() {
  GltfHeadMesh head;
  auto& m = head.mesh;
  m.positions = {{12,0,9},{12,3,10},{12,-3,10},{8,0,12}};
  head.parts.push_back({"face",0,4});
  for(int side=0; side<2; ++side) {
    const size_t first = m.positions.size();
    const float  y     = side==0 ? EyeY : -EyeY;
    // the front pole (the pupil) first, rings of 8 around the forward axis, the back pole
    for(int t=0; t<=6; ++t)
      for(int f=0; f<(t==0 || t==6 ? 1 : 8); ++f) {
        const float th = float(t)*30.f*Deg, ph = float(f)*45.f*Deg;
        m.positions.push_back({EyeX + EyeR*std::sin(th)*std::cos(ph), y + EyeR*std::sin(th)*std::sin(ph), EyeZ + EyeR*std::cos(th)});
        }
    head.parts.push_back({side==0 ? "eye_L" : "eye_R",first,m.positions.size()-first});
    }
  const size_t flat = m.positions.size();
  for(int i=0; i<6; ++i)
    m.positions.push_back({float(i%3),float(i/3),5});
  head.parts.push_back({"flat",flat,6});
  for(size_t i=0; i<m.positions.size(); ++i) {
    asset::Vec3 n = {0,0,1};
    for(int side=0; side<2; ++side)
      if(i>=head.parts[size_t(1+side)].firstVertex && i<head.parts[size_t(1+side)].firstVertex+head.parts[size_t(1+side)].vertexCount)
        n = {(m.positions[i].x-EyeX)/EyeR,(m.positions[i].y-(side==0 ? EyeY : -EyeY))/EyeR,(m.positions[i].z-EyeZ)/EyeR};
    m.normals.push_back(n);
    }
  for(const char* name:{"eyeLookDownLeft","eyeLookDownRight","eyeLookUpLeft","eyeLookUpRight","eyeLookInLeft","eyeLookInRight",
                        "eyeLookOutLeft","eyeLookOutRight","eyeBlinkLeft","eyeBlinkRight"})
    head.targets.push_back({name,{{uint32_t(head.targets.size()%4),{0,-.1f,0},{}}}});
  for(uint32_t i=0; i<m.positions.size(); ++i)
    head.renderVertexIds.push_back(i);
  return head;
  }

std::string eyeSection(const std::string& body) {
  return R"({"version":1,"mouth":[],"anims":{"R_EYESBLINK":{"layer":4,"blend_in_ms":0,"blend_out_ms":0,"duration_ms":0,)"
         R"("frame_ms":1000,"frames":[{"eyeBlinkLeft":1,"eyeBlinkRight":1}]}},"eyes":)" + body + "}";
  }

const std::string EyeLids = R"("lids":{"down":[{"target":"eyeLookDown{side}","from":0,"to":20,"max":1},)"
  R"({"target":"eyeBlink{side}","from":15,"to":30,"max":0.3}],"up":[{"target":"eyeLookUp{side}","from":0,"to":22,"max":1}],)"
  R"("in":[{"target":"eyeLookIn{side}","from":0,"to":30,"max":1}],"out":[{"target":"eyeLookOut{side}","from":0,"to":30,"max":1}]})";
const std::string EyeProfile = eyeSection(R"({"about":"synthetic","parts":{"left":"eye_L","right":"eye_R"},)"
  R"("limits_deg":{"out":35,"in":30,"up":25,"down":30},)" + EyeLids + "}");

void eyeProfileCases(const GltfHeadMesh& head, const FaceProfile& p) {
  require(p.eyes.has_value(),"the eyes section must be read");
  const auto& e = *p.eyes;
  for(int s=0; s<2; ++s) {
    const float y = s==0 ? EyeY : -EyeY;
    require(close(e.eye[s].center.x,EyeX) && close(e.eye[s].center.y,y) && close(e.eye[s].center.z,EyeZ) && close(e.eye[s].radius,EyeR),
            "the centre of a ball must be the sphere fitted to its vertices");
    }
  require(e.eye[0].outSign==1.f && e.eye[1].outSign==-1.f,"the left eye (+y) must turn out to +y, the right one to -y");
  require(close(e.limit[FaceProfile::Eyes::Out],35*Deg) && close(e.limit[FaceProfile::Eyes::In],30*Deg) &&
          close(e.limit[FaceProfile::Eyes::Up],25*Deg) && close(e.limit[FaceProfile::Eyes::Down],30*Deg),"the limits must be read in radians");
  require(e.eye[0].lids[FaceProfile::Eyes::Down].size()==2 && e.eye[1].lids[FaceProfile::Eyes::Down][1].target==BlinkR &&
          e.eye[0].lids[FaceProfile::Eyes::Out][0].target==LookOutL,"{side} must name the shapes of each eye");
  const auto none = FaceProfile::parse(R"({"version":1,"mouth":[],"anims":{"S_A":{"layer":1,"blend_in_ms":0,"blend_out_ms":0,)"
                                      R"("duration_ms":-1,"frame_ms":1,"frames":[{}]}}})",head);
  require(!none->eyes.has_value(),"a profile without eyes must keep the eyes rigid");

  const auto rejected = [&](const std::string& body, std::string_view reason) {
    try {
      (void)FaceProfile::parse(eyeSection(body),head);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,
              "the eyes rejection must say why: want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"the eyes section must be rejected: "+std::string(reason));
    };
  const std::string parts  = R"("parts":{"left":"eye_L","right":"eye_R"},)";
  const std::string limits = R"("limits_deg":{"out":35,"in":30,"up":25,"down":30},)";
  rejected("[]",                                                     "eyes must be an object");
  rejected("{"+parts+limits+EyeLids+R"(,"speed":1})",                "eyes: unknown field \"speed\"");
  rejected("{"+limits+EyeLids+"}",                                   "eyes: parts must be");
  rejected(R"({"parts":{"left":"eye_L","right":"eye_X"},)"+limits+EyeLids+"}", "eyes: parts: no head part \"eye_X\"");
  rejected(R"({"parts":{"left":"eye_L","right":"eye_L"},)"+limits+EyeLids+"}", "must be different parts");
  rejected(R"({"parts":{"left":"eye_L","right":"flat"},)"+limits+EyeLids+"}",  "eyes: part \"flat\" is not a ball");
  rejected("{"+parts+R"("limits_deg":{"out":35,"in":30,"up":0,"down":30},)"+EyeLids+"}", "eyes: limits_deg: up must be above 0");
  rejected("{"+parts+R"("limits_deg":{"out":35,"in":30,"up":25},)"+EyeLids+"}",         "eyes: limits_deg: no down");
  rejected("{"+parts+limits+R"("lids":{"aside":[]}})",               "eyes: lids: unknown field \"aside\"");
  rejected("{"+parts+limits+R"("lids":{"down":[{"target":"eyeLookFar{side}","from":0,"to":20,"max":1}]}})",
           "eyes: lids down 0: unknown target \"eyeLookFarLeft\"");
  rejected("{"+parts+limits+R"("lids":{"up":[{"target":"eyeLookUp{side}","from":10,"to":10,"max":1}]}})",
           "eyes: lids up 0: from must be 0 or more and to above it");
  rejected("{"+parts+limits+R"("lids":{"in":[{"target":"eyeLookIn{side}","from":0,"to":10,"max":2}]}})",
           "eyes: lids in 0: max must be a number 0..1");
  rejected("{"+parts+limits+R"("lids":{"out":[{"target":"eyeLookOut{side}","from":0,"to":10,"weight":1}]}})",
           "eyes: lids out 0: unknown field \"weight\"");
  }

Tempest::Vec3 vertex(const std::vector<Tempest::Vec4>& d, uint32_t v) {
  return {d[v*2].x,d[v*2].y,d[v*2].z};
  }

Tempest::Vec3 normal(const std::vector<Tempest::Vec4>& d, uint32_t v) {
  return {d[v*2+1].x,d[v*2+1].y,d[v*2+1].z};
  }

void eyeRigCases(const GltfHeadMesh& head, const FaceProfile& p) {
  using R = FaceRig::Result;
  FaceRig rig;
  rig.setup(&head,&p);
  require(rig.hasEyes() && rig.update(0)==R::Rest,"eyes looking straight on a neutral face are the rest");
  const auto& w = rig.weights();
  const auto& d = rig.deformation();
  const uint32_t left = uint32_t(head.parts[1].firstVertex), right = uint32_t(head.parts[2].firstVertex);
  const uint32_t count = uint32_t(head.parts[1].vertexCount);

  // the left eye 20 degrees to its left (out): every vertex turned around the centre, normals too
  rig.setEyes({20*Deg,0},{});
  require(rig.update(10)==R::Deformed,"a turned eye must deform the head");
  const Tempest::Vec3 c = {EyeX,EyeY,EyeZ};
  const Tempest::Vec3 pupil = vertex(d,left) - c;
  require(close(pupil.x,0) && close(pupil.y,EyeR*std::sin(20*Deg)) && close(pupil.z,EyeR*std::cos(20*Deg)),
          "the front of the ball must turn 20 degrees to +y");
  for(uint32_t v=left; v<left+count; ++v) {
    const auto& b = head.mesh.positions[v];
    const Tempest::Vec3 at = vertex(d,v)-c, n = normal(d,v);
    require(close(at.length(),EyeR) && close(n.length(),1),"a turned ball must keep its radius and unit normals");
    require(close(Tempest::Vec3::dotProduct(at/EyeR,n),1,1e-3f),"a normal must turn with its vertex");
    // a turn around x (up) adds the yaw to the angle in the y-z plane (the front half: no wrap)
    if(b.z-EyeZ>0.1f)
      require(close(std::atan2(at.y,at.z)-std::atan2(b.y-EyeY,b.z-EyeZ),20*Deg,1e-3f),"every vertex must turn by the yaw");
    }
  for(uint32_t v=right; v<right+count; ++v) {
    const auto& b = head.mesh.positions[v];
    require(close(vertex(d,v).x,b.x) && close(vertex(d,v).y,b.y) && close(vertex(d,v).z,b.z),"the other eye must stay");
    }
  require(close(w[LookOutL],20.f/30.f) && w[LookInL]==0 && w[LookOutR]==0 && w[LookInR]==0,
          "a look out must move the lid of that eye by the table: max*(angle-from)/(to-from)");

  // both eyes 10 degrees down, the right one 12 degrees to the left (in): the pupils down, the lids by the table
  rig.setEyes({0,-10*Deg},{12*Deg,-10*Deg});
  rig.update(20);
  const Tempest::Vec3 rp = vertex(d,right) - Tempest::Vec3(EyeX,-EyeY,EyeZ);
  require(close(rp.x,-EyeR*std::sin(10*Deg)) && close(rp.y,EyeR*std::cos(10*Deg)*std::sin(12*Deg)),
          "pitch then yaw: forward must go to (sin p, cos p sin y, cos p cos y)");
  require(close(w[LookDownL],0.5f) && close(w[LookDownR],0.5f) && close(w[LookInR],0.4f) && w[BlinkL]==0,
          "a look down must lower the lids, a look in move the lid in");

  // far down: eyeLookDown full, the lid table's part of eyeBlink, summed with the blink anim and clamped
  rig.setEyes({0,-25*Deg},{0,-25*Deg});
  rig.update(30);
  require(close(w[LookDownL],1) && close(w[BlinkL],0.2f) && close(w[BlinkR],0.2f),"past 15 degrees down a part of the blink must come");
  rig.startAnim("R_EYESBLINK",0.5f,uint64_t(-1),40);
  rig.update(40);
  require(close(w[BlinkL],0.7f),"the lids must add to the weights of the anims");
  rig.startAnim("R_EYESBLINK",1,uint64_t(-1),50);
  rig.update(50);
  require(close(w[BlinkL],1),"the sum must be clamped to 1");
  rig.startAnim("R_EYESBLINK",1,0,60);

  // a turn past the lid tables: only the balls are written, no blend
  rig.setEyes({31*Deg,0},{});
  rig.update(70);
  FaceRig::Probe pr;
  rig.probe(pr,70);
  const auto blends = pr.blends;
  rig.setEyes({33*Deg,0},{});
  require(rig.update(80)==R::Deformed,"a turned eye must deform the head without a change of the weights");
  rig.probe(pr,80);
  require(pr.blends==blends && rig.changed().first>=left && rig.changed().second<=right+count,
          "a turn alone must write the balls only, without a blend");
  require(close((vertex(d,left)-c).y,EyeR*std::sin(33*Deg)),"the ball must show the new turn");
  require(rig.update(90)==R::Same,"no change must send nothing");

  rig.setEyes({},{});
  require(rig.update(100)==R::Rest,"straight eyes and no anims must return to the rest");
  }

// the head bone: its x up (world y), z forward turned by yaw degrees around up, at a man's height
Tempest::Matrix4x4 headMatrix(float yaw) {
  const float a = yaw*Deg;
  const Tempest::Vec3 x = {0,1,0}, z = {std::sin(a),0,std::cos(a)}, y = {-std::cos(a),0,std::sin(a)};
  Tempest::Matrix4x4 m;
  m.identity();
  for(int r=0; r<3; ++r) {
    m.set(0,r,(&x.x)[r]);
    m.set(1,r,(&y.x)[r]);
    m.set(2,r,(&z.x)[r]);
    }
  m.set(3,1,160);
  return m;
  }

// a point in the frame of the head: from the middle of the eyes, yaw (+ to +y) and pitch degrees, dist cm away
Tempest::Vec3 headPoint(const Tempest::Matrix4x4& head, float yaw, float pitch, float dist) {
  Tempest::Vec3 p = {EyeX + dist*std::sin(pitch*Deg), dist*std::cos(pitch*Deg)*std::sin(yaw*Deg), EyeZ + dist*std::cos(pitch*Deg)*std::cos(yaw*Deg)};
  head.project(p);
  return p;
  }

void eyeMotionCases(const FaceProfile& p) {
  require(close(Eyes::bell(0),0) && close(Eyes::bell(0.5f),0.5f) && close(Eyes::bell(1),1) &&
          close(Eyes::bell(0.25f)+Eyes::bell(0.75f),1),"the saccade's path must be a symmetric S from 0 to 1");
  require(close(Eyes::duration(10),43) && close(Eyes::duration(20),65),"a saccade must take 21 ms + 2.2 ms per degree");

  Eyes eyes;
  eyes.setup(&*p.eyes,7);
  Eyes::Input in;
  in.roam   = false;
  in.target = true;
  in.head   = headMatrix(0);
  Eyes::Probe pr;
  uint64_t now = 1000;
  const auto step = [&](uint64_t ms) {
    now += ms;
    in.now = now;
    eyes.update(in);
    eyes.probe(pr);
    };

  // a near point converges the eyes, a far one barely
  in.at = headPoint(in.head,0,0,30);
  step(0);
  require(close(pr.yaw[0],-std::atan2(EyeY,30.f)/Deg,0.05f) && close(pr.yaw[1],std::atan2(EyeY,30.f)/Deg,0.05f) &&
          pr.error<0.05f,"each eye must aim from its own centre: in on a near point");
  in.at = headPoint(in.head,0,0,1000);
  step(1000);
  require(std::abs(pr.yaw[0])<0.25f && std::abs(pr.yaw[1])<0.25f && !pr.moving,"far away the eyes must be nearly parallel");

  // limits: out 35 and in 30 to the side, 25 up, 30 down
  in.at = headPoint(in.head,60,0,1000);
  step(1);
  for(int i=0; i<200; ++i)
    step(1);
  require(close(pr.yaw[0],35,0.01f) && close(pr.yaw[1],30,0.01f) && pr.over==0,"to the side the left eye must stop at out, the right one at in");
  in.at = headPoint(in.head,0,50,1000);
  for(int i=0; i<200; ++i)
    step(1);
  require(close(pr.pitch[0],25,0.01f) && close(pr.pitch[1],25,0.01f) && pr.error>20,"up the eyes must stop at the limit");
  in.at = headPoint(in.head,0,-60,1000);
  for(int i=0; i<200; ++i)
    step(1);
  require(close(pr.pitch[0],-30,0.01f),"down the eyes must stop at the limit");

  // a saccade of 10 degrees: 43 ms, the speed a bell, no overshoot, then on the point
  in.at = headPoint(in.head,0,0,1000);
  for(int i=0; i<300; ++i)
    step(1);
  const auto before = pr.saccades;
  const float from = pr.yaw[0];
  in.at = headPoint(in.head,-10,0,1000);
  const float goal = -10.f + (from - 0.f);  // the left eye: 10 degrees to -y from where it was
  std::vector<float> path;
  step(1);
  require(pr.moving && pr.saccades==before+1,"a jump of the point must start a saccade");
  path.push_back(pr.yaw[0]);
  while(pr.moving && path.size()<500) {
    step(1);
    path.push_back(pr.yaw[0]);
    }
  const float ms = float(path.size()-1);
  require(std::abs(ms-Eyes::duration(10))<=2.f,"the saccade must take 21 + 2.2*10 ms, took "+std::to_string(ms));
  size_t peak = 0;
  float  fastest = 0;
  for(size_t i=1; i<path.size(); ++i) {
    require(path[i]<=path[i-1]+1e-4f,"a saccade must not go back");
    require(path[i]>=goal-0.05f,"a saccade must not overshoot");
    if(path[i-1]-path[i]>fastest) {
      fastest = path[i-1]-path[i];
      peak    = i;
      }
    }
  require(float(peak)>0.35f*ms && float(peak)<0.65f*ms,"the speed of a saccade must peak in its middle");
  require(close(path.back(),goal,0.05f) && pr.error<0.05f,"a saccade must land on the point");

  // the head turns 15 degrees: the eyes turn back in their sockets at once, the gaze stays, no jump
  const float straight = pr.yaw[0];
  const auto  jumps    = pr.saccades;
  for(int i=1; i<=15; ++i) {
    in.head = headMatrix(float(i));
    step(16);
    require(pr.error<0.05f,"the gaze must stay on the point while the head turns");
    }
  require(close(pr.yaw[0]-straight,15,0.3f) && pr.saccades==jumps,"the eye must turn back by the turn of the head, without a jump");
  in.head = headMatrix(0);
  in.at   = headPoint(in.head,0,0,1000);
  for(int i=0; i<300; ++i)
    step(1);

  // a point moving at 20 deg/s is followed smoothly, at 70 deg/s caught up by jumps
  for(float speed:{20.f,70.f}) {
    in.at = headPoint(in.head,-15,0,1000);
    for(int i=0; i<300; ++i)
      step(1);
    const auto n0 = pr.saccades;
    float worst = 0;
    for(int i=1; i<=600; ++i) {
      in.at = headPoint(in.head,-15+speed*float(i)/1000.f,0,1000);
      step(1);
      worst = std::max(worst,pr.error);
      }
    if(speed<30) {
      require(pr.saccades==n0+0 && worst<0.5f,"a slow point must be followed without jumps: worst "+std::to_string(worst));
      } else {
      require(pr.saccades>=n0+2 && worst<10,"a fast point must be caught up by jumps: worst "+std::to_string(worst));
      }
    }

  // the same seed gives the same jumps over a face; another seed other ones
  const auto talk = [&](uint32_t seed) {
    Eyes e;
    e.setup(&*p.eyes,seed);
    Eyes::Input t;
    t.head   = headMatrix(0);
    t.target = true;
    t.talk   = true;
    t.at     = headPoint(t.head,0,0,110);
    std::vector<float> out;
    Eyes::Probe ep;
    uint32_t first = 0;
    for(uint64_t ms=0; ms<=20000; ms+=16) {
      t.now = 5000+ms;
      e.update(t);
      e.probe(ep);
      out.push_back(ep.yaw[0]);
      out.push_back(ep.pitch[0]);
      out.push_back(float(ep.kind));
      if(ms==0)
        first = ep.saccades;
      }
    out.push_back(float(ep.saccades-first));
    return out;
    };
  const auto a = talk(11), b = talk(11), c = talk(12);
  require(a==b,"the same seed must give the same saccades");
  require(a!=c,"another seed must give other saccades");
  const float rate = a.back()/20.f;
  require(rate>=1 && rate<=3,"listening the eyes must jump 1-3 times a second, got "+std::to_string(rate));
  }

int main() {
  try {
    const auto head    = makeHead();
    const auto profile = FaceProfile::parse(Profile,head);
    profileCases(head,*profile);
    rigCases(head,*profile);
    const auto eyeHead    = makeEyeHead();
    const auto eyeProfile = FaceProfile::parse(EyeProfile,eyeHead);
    eyeProfileCases(eyeHead,*eyeProfile);
    eyeRigCases(eyeHead,*eyeProfile);
    eyeMotionCases(*eyeProfile);
    }
  catch(const std::exception& e) {
    std::cerr << "facerig test: FAIL: " << e.what() << " (after " << assertions << " checks)\n";
    return 1;
    }
  std::cout << "facerig test: " << assertions << " checks passed\n";
  return 0;
  }
