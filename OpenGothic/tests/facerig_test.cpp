// The own head's face (graphics/mesh/facerig): a synthetic head and profile - the weights summed and clamped, fade in
// and out, frames in time, one anim per layer, VISEME shapes set by hand, the voice quieting the mouth, the deformation
// (normals normalized, laid out through renderVertexIds with padding), no blend while the weights stay, profile
// rejections with their reason. CPU only: no device, no game data.
#include "graphics/mesh/facerig.h"
#include "gltfmesh.h"

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
  target("browDown", {{1,{0,0,-1},{}}});
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

  // fade out by blend_out_ms, then the rest
  rig.startAnim("S_FRIENDLY",1,0,5000);
  require(close(rig.weight("S_FRIENDLY",5500),0.5f),"a stopped anim must fade out");
  require(rig.update(6001)==R::Rest && w[BrowDown]==0,"all weights zero must return to the rest");

  // frames in time by frame_ms, neighbours mixed; its own duration
  rig.startAnim("T_EAT",1,uint64_t(-1),7000);
  rig.update(7050);
  require(close(w[JawOpen],0.5f),"half a frame must mix two frames");
  rig.update(7100);
  require(close(w[JawOpen],1.f),"the next frame must show at frame_ms");
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

int main() {
  try {
    const auto head    = makeHead();
    const auto profile = FaceProfile::parse(Profile,head);
    profileCases(head,*profile);
    rigCases(head,*profile);
    }
  catch(const std::exception& e) {
    std::cerr << "facerig test: FAIL: " << e.what() << " (after " << assertions << " checks)\n";
    return 1;
    }
  std::cout << "facerig test: " << assertions << " checks passed\n";
  return 0;
  }
