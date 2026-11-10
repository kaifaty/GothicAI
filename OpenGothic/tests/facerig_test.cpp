// The own head's face (graphics/mesh/facerig): a synthetic head and profile - the weights summed and clamped, fade in
// and out, frames in time, one anim per layer, VISEME shapes set by hand, the voice quieting the mouth, the deformation
// (normals normalized, laid out through renderVertexIds with padding), no blend while the weights stay, profile
// rejections with their reason; the gestures (cues): their curve, the sum with the other layers, the side, and when a
// line plays them (SpeechCues: the time of words, pauses, the estimate by characters); the leak of a felt emotion
// (leaks): its curve, one a line, the liar's blinks, the posed smile, say.leak read by the bridge. CPU only: no device,
// no game data.
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/speechcues.h"
#include "gltfmesh.h"
#include "linecues.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

enum Target : uint32_t { JawOpen, BrowDown, EyeBlink, VisemeSil, VisemeAa, BrowUpLeft, BrowUpRight, SmileLeft, SmileRight,
                         EyeSquint, CheekSquint };

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
  target("browUpLeft",     {{1,{0,1,0},{}}});
  target("browUpRight",    {{2,{0,1,0},{}}});
  target("mouthSmileLeft", {{1,{1,0,0},{}}});
  target("mouthSmileRight",{{2,{1,0,0},{}}});
  target("eyeSquintLeft",  {{2,{0,0,1},{}}});
  target("cheekSquintLeft",{{2,{0,0,-1},{}}});
  head.renderVertexIds = {3,0,UINT32_MAX,1,2,0};
  return head;
  }

const char* Profile = R"({
  "version": 1, "about": "synthetic", "mouth": ["jaw", "viseme_"],
  "anims": {
    "S_ANGRY":     {"layer": 1, "blend_in_ms": 1000, "blend_out_ms": 1000, "duration_ms": -1, "frame_ms": 1000,
                    "frames": [{"browDown": 0.8, "jawOpen": 0.5}]},
    "S_FRIENDLY":  {"layer": 1, "blend_in_ms": 1000, "blend_out_ms": 1000, "duration_ms": -1, "frame_ms": 1000,
                    "frames": [{"browDown": 0.4, "eyeSquintLeft": 0.2, "cheekSquintLeft": 0.3}]},
    "S_EXTRA":     {"layer": 3, "blend_in_ms": 0, "blend_out_ms": 0, "duration_ms": -1, "frame_ms": 1000,
                    "frames": [{"browDown": 0.6}]},
    "R_EYESBLINK": {"layer": 4, "blend_in_ms": 40, "blend_out_ms": 40, "duration_ms": 0, "frame_ms": 1000,
                    "frames": [{"eyeBlink": 1.0}]},
    "T_EAT":       {"layer": 1, "blend_in_ms": 0, "blend_out_ms": 0, "duration_ms": 2000, "frame_ms": 100,
                    "frames": [{"jawOpen": 0.0}, {"jawOpen": 1.0}]},
    "VISEME":      {"layer": 2, "blend_in_ms": 100, "blend_out_ms": 100, "duration_ms": -1, "frame_ms": 200,
                    "frames": [{"viseme_sil": 1.0}, {"viseme_aa": 0.5}, {"viseme_aa": 1.0}]}
  },
  "cues": {
    "flash": {"zone": "upper", "parts": [{"weights": {"browUpLeft": 0.7, "browUpRight": 0.7},
                                          "delay_ms": 0, "on_ms": 100, "hold_ms": 80, "off_ms": 160}]},
    "doubt": {"zone": "upper", "side": "L", "parts": [{"weights": {"browUpLeft": 0.8, "browDown": 0.3},
                                          "delay_ms": 0, "on_ms": 250, "hold_ms": 550, "off_ms": 300}]},
    "smirk": {"zone": "mouth", "side": "L", "parts": [{"weights": {"mouthSmileLeft": 0.75},
                                          "delay_ms": 0, "on_ms": 200, "hold_ms": 400, "off_ms": 300}]},
    "press": {"zone": "mouth", "parts": [{"weights": {"jawOpen": 0.7},
                                          "delay_ms": 0, "on_ms": 150, "hold_ms": 400, "off_ms": 200}]},
    "chin":  {"zone": "mouth", "parts": [{"weights": {"jawOpen": 0.5},
                                          "delay_ms": 0, "on_ms": 150, "hold_ms": 50, "off_ms": 200}]}
  },
  "leaks": {"on_ms": 120, "hold_ms": 120, "off_ms": 160, "felt": {
    "afraid":   {"upper": {"browUpLeft": 0.45, "browUpRight": 0.43}, "lower": {"mouthSmileLeft": 0.2}},
    "friendly": {"upper": {"eyeSquintLeft": 0.45, "browUpLeft": 0.258}, "lower": {"mouthSmileLeft": 0.45, "cheekSquintLeft": 0.05}}
  }}
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
  require(FaceProfile::parse(anim(good),head)!=nullptr && FaceProfile::parse(anim(good),head)->cues.empty() &&
          FaceProfile::parse(anim(good),head)->leaks.empty(),"the minimal profile must load, a profile without cues and leaks as before");
  require(p.leaks.size()==4 && p.findLeak("afraid",true)!=nullptr && p.findLeak("angry",false)==nullptr &&
          close(p.findLeak("afraid",false)->peak,0.45f) && close(p.leakTime.span(),400.f),"leaks must be read by feeling and half");
  const auto leak = [](const std::string& body) {
    return R"({"version":1,"mouth":["jaw"],"anims":{"S_ANGRY":{"layer":1,"blend_in_ms":100,"blend_out_ms":100,)"
           R"("duration_ms":-1,"frame_ms":1000,"frames":[{"browDown":1}]}},"leaks":)"+body+"}";
    };
  require(p.cues.size()==5 && p.findCue("doubt")->side=='L' && p.findCue("smirk")->mouth && !p.findCue("flash")->mouth &&
          p.findCue("doubt")->parts[0].mirrored[0].first==BrowUpRight && p.findCue("doubt")->parts[0].mirrored[1].first==BrowDown,
          "cues must be read with their zone, side and the shapes of the other side");
  const auto cue = [](const std::string& body) {
    return R"({"version":1,"mouth":["jaw"],"anims":{"S_ANGRY":{"layer":1,"blend_in_ms":100,"blend_out_ms":100,)"
           R"("duration_ms":-1,"frame_ms":1000,"frames":[{"browDown":1}]}},"cues":{"flash":)"+body+"}}";
    };
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
  rejected(cue(R"({"zone":"upper","parts":[{"weights":{"nose":1},"delay_ms":0,"on_ms":100,"hold_ms":80,"off_ms":160}]})"),
           "cue \"flash\" part 0: unknown target \"nose\"");
  rejected(cue(R"({"zone":"upper","side":"L","parts":[{"weights":{"mouthSmileLeft":1},"delay_ms":0,"on_ms":100,"hold_ms":80,"off_ms":160,"x":1}]})"),
           "unknown field \"x\"");
  rejected(cue(R"({"zone":"lower","parts":[{"weights":{"browDown":1},"delay_ms":0,"on_ms":100,"hold_ms":80,"off_ms":160}]})"),
           "zone must be");
  rejected(cue(R"({"zone":"upper","parts":[{"weights":{"browDown":1},"delay_ms":0,"on_ms":0,"hold_ms":80,"off_ms":160}]})"),
           "on_ms and off_ms must be above 0");
  rejected(leak(R"({"on_ms":120,"hold_ms":120,"off_ms":160,"felt":{"sad":{"upper":{"browDown":0.4}}}})"),
           "leak \"sad\": unknown feeling");
  rejected(leak(R"({"on_ms":120,"hold_ms":120,"off_ms":160,"felt":{"afraid":{"middle":{"browDown":0.4}}}})"),
           "leak \"afraid\": unknown half \"middle\"");
  rejected(leak(R"({"on_ms":120,"hold_ms":120,"off_ms":160,"felt":{"afraid":{"upper":{"nose":0.4}}}})"),
           "leak \"afraid\" upper: unknown target \"nose\"");
  rejected(leak(R"({"on_ms":120,"hold_ms":120,"off_ms":160,"peak":1,"felt":{}})"),"leaks: unknown field \"peak\"");
  rejected(leak(R"({"on_ms":0,"hold_ms":120,"off_ms":160,"felt":{}})"),"leaks: on_ms and off_ms must be above 0");
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
float ease(float x) {
  x = std::min(std::max(x,0.f),1.f);
  return x*x*(3.f-2.f*x);
  }

void cueCases(const GltfHeadMesh& head, const FaceProfile& p) {
  FaceRig rig;
  rig.setup(&head,&p);
  const auto& w = rig.weights();
  require(!rig.startCue("glance",false,-1,0,0),"a gesture the profile does not have must not start");

  // the curve of a part: a smooth step up over on_ms, the hold, a smooth step down over off_ms (cues.py env)
  require(rig.startCue("flash",false,-1,1000,1000),"flash must start");
  rig.update(1025);
  require(close(w[BrowUpLeft],0.7f*ease(0.25f)) && close(w[BrowUpRight],0.7f*ease(0.25f)),"the rise must follow the smooth step");
  rig.update(1050);
  require(close(w[BrowUpLeft],0.35f),"half of on_ms must give half of the weight");
  rig.update(1140);
  require(std::abs(w[BrowUpLeft]-0.7f)<=0.01f && std::abs(w[BrowUpRight]-0.7f)<=0.01f && close(rig.cueWeight("flash",1140),1.f),
          "the apex must give the dictionary's weights");
  rig.update(1260);
  require(close(w[BrowUpLeft],0.35f),"half of off_ms must give half of the weight");
  rig.update(1300);
  require(close(w[BrowUpLeft],0.7f*(1.f-ease(0.75f))),"the fall must follow the smooth step");
  require(rig.update(1341)==FaceRig::Result::Rest && rig.cueWeight("flash",1341)==0,"the gesture must end after its span");

  // a gesture, another one and a face sum up, none of them puts out another; the sum is clamped to 1
  rig.startAnim("S_FRIENDLY",1,uint64_t(-1),2000);
  rig.startCue("doubt",false,-1,3000,3000);
  rig.startCue("flash",false,-1,3200,3200);
  rig.update(3300);
  require(close(w[BrowDown],0.4f+0.3f) && close(w[BrowUpLeft],1.f) && close(w[BrowUpRight],0.7f),
          "two gestures and S_FRIENDLY must sum, clamped to 0..1");
  require(close(rig.weight("S_FRIENDLY",3300),1.f) && close(rig.cueWeight("doubt",3300),1.f) && close(rig.cueWeight("flash",3300),1.f),
          "none of them may put out another");
  FaceRig::Probe pr;
  rig.probe(pr,3300);
  require(pr.cues==2 && pr.shown=="S_FRIENDLY 1.00 doubt:L 1.00 flash 1.00","the probe must show the gestures and their strength: "+pr.shown);
  rig.startAnim("S_FRIENDLY",1,0,3300);
  rig.update(5000);

  // a one-sided gesture on the other side; the hold given instead of its own
  rig.startCue("doubt",true,-1,5000,5000);
  rig.update(5400);
  require(close(w[BrowUpRight],0.8f) && w[BrowUpLeft]==0 && close(w[BrowDown],0.3f),"a mirrored gesture must use the other side's shapes");
  rig.probe(pr,5400);
  require(pr.shown=="doubt:R 1.00","the probe must show the side it is on: "+pr.shown);
  rig.startCue("press",false,100,7000,7000);
  rig.update(7200);
  require(close(w[JawOpen],0.7f),"press must be at its apex");
  rig.update(7350);
  require(close(w[JawOpen],0.35f),"a shortened hold must bring the fall earlier");

  // the voice does not quiet a gesture of the mouth
  rig.startCue("press",false,-1,8000,8000);
  rig.setShape("VISEME",0,0,0,1,8200);
  rig.update(8200);
  require(close(w[JawOpen],0.7f) && close(w[VisemeSil],1.f),"speech must not quiet a gesture");
  }

// a voice of 16 kHz: loud where `loud` says (ms ranges), silent elsewhere, `ms` long
std::shared_ptr<LipSync::Track> voice(uint64_t ms, std::vector<std::pair<uint64_t,uint64_t>> loud, bool finish = true) {
  auto t = std::make_shared<LipSync::Track>(16000);
  std::vector<int16_t> pcm(size_t(ms*16));
  for(auto [a,b]:loud)
    for(size_t i=size_t(a*16); i<size_t(b*16) && i<pcm.size(); ++i)
      pcm[i] = int16_t(12000*std::sin(double(i)*2.0*3.14159265358979*500.0/16000.0));
  t->append(pcm.data(),pcm.size());
  if(finish)
    t->finish();
  return t;
  }

struct Fired final {
  std::string name;
  uint64_t    at   = 0;
  float       hold = 0;
  bool        mirror = false;
  };

// plays the line from tick 0 in 10 ms steps: the gestures it starts
std::vector<Fired> play(const FaceProfile& p, std::shared_ptr<SpeechCues::Plan> plan, std::shared_ptr<LipSync::Track> track,
                        uint64_t until = 4000) {
  SpeechCues sc;
  sc.voice(track,0);
  sc.start(plan,true,0);
  std::vector<Fired>             ret;
  SpeechCues::Out out;
  for(uint64_t now=0; now<=until; now+=10) {
    out.clear();
    sc.update(p,now,0,out);
    for(auto& s:out.cues)
      ret.push_back({std::string(s.name),s.at,s.hold,s.mirror});
    }
  return ret;
  }

bool near(uint64_t a, uint64_t b, uint64_t eps) {
  return (a>b ? a-b : b-a)<=eps;
  }

void timingCases(const FaceProfile& p) {
  using Plan = SpeechCues::Plan;
  // the words of the text by whitespace, as the server counts them
  Plan words("  one two\tthree  four five ");
  require(words.words.size()==5 && words.wordChar[0]==2 && words.wordChar[2]==10 && words.chars==27,
          "the words must be split by whitespace");

  // say.words: an upper gesture 0.1 s before its word
  auto plan = std::make_shared<Plan>("one two three four");
  plan->cues = {{"flash",2,0}};
  plan->setWord(0,0.f,.4f); plan->setWord(1,.5f,.9f); plan->setWord(2,1.f,1.3f); plan->setWord(3,1.4f,2.f);
  auto loud = voice(2000,{{0,2000}});
  auto f    = play(p,plan,loud);
  require(f.size()==1 && f[0].name=="flash" && f[0].at==900 && f[0].hold<0,"an upper gesture must start 0.1 s before its word");

  // a mouth gesture into the pause of 0.3 s between words, its hold shortened; one that does not fit - after the end
  plan = std::make_shared<Plan>("one two three four");
  plan->cues = {{"press",1,0},{"smirk",1,'R'}};
  plan->setWord(0,0.f,.4f); plan->setWord(1,.5f,.9f); plan->setWord(2,1.2f,1.6f); plan->setWord(3,1.7f,2.f);
  f = play(p,plan,loud);
  require(f.size()==2 && f[0].name=="press" && f[0].at==800 && close(f[0].hold,200.f),
          "a mouth gesture must go into the pause between words, widened by -0.10/+0.15 s, with its hold shortened");
  require(f[1].name=="smirk" && f[1].at==2000 && f[1].hold<0 && f[1].mirror,
          "a mouth gesture without a pause it fits in must come right after the end of the sound, on its side");

  // without words: the estimate by characters, the share of the sound's length
  plan = std::make_shared<Plan>("aaaa bbbb cccc dddd");
  plan->cues = {{"flash",2,0},{"press",1,0}};
  f = play(p,plan,loud);
  require(f.size()==2 && f[0].name=="flash" && near(f[0].at,2000*10/19-100,10),"without words an upper gesture must go by its characters");
  require(f[1].name=="press" && f[1].at==2000,"without words and pauses a mouth gesture must come after the end");

  // a pause by the loudness of the voice
  f = play(p,plan,voice(2000,{{0,1200},{1500,2000}}));
  require(f.size()==2 && f[1].name=="press" && near(f[1].at,1100,30) && near(uint64_t(f[1].hold),200,30),
          "a quiet voice of 0.3 s must take a mouth gesture as a pause between words does");

  // a streamed voice: 14 characters a second from its start, not past the sound that has arrived
  plan = std::make_shared<Plan>("aaaa bbbb cccc dddd eeee ffff gggg");
  plan->cues = {{"flash",1,0},{"doubt",6,0}};
  plan->streamed = true;
  auto stream = voice(1000,{{0,1000}},false);
  SpeechCues sc;
  sc.voice(stream,0);
  sc.start(plan,true,0);
  SpeechCues::Out out;
  stream->setPlayed(0);
  sc.update(p,500,0,out);
  require(out.cues.empty(),"a stream that has not started playing must wait");
  stream->setPlayed(260);
  sc.update(p,760,0,out);
  require(out.cues.size()==1 && out.cues[0].name=="flash" && out.cues[0].at==760-(260-(5000/14-100)),"a stream must go at 14 characters a second");
  out.clear();
  stream->setPlayed(899);
  sc.update(p,1399,0,out);
  require(out.cues.empty(),"the estimate must wait for more of the sound");
  stream->setPlayed(900);
  sc.update(p,1400,0,out);
  require(out.cues.size()==1 && out.cues[0].name=="doubt","the estimate must not go past the sound that has arrived");
  // the word's time, once it comes, takes over before the gesture starts
  plan = std::make_shared<Plan>("aaaa bbbb cccc dddd");
  plan->cues = {{"flash",3,0}};
  auto late = std::make_shared<SpeechCues>();
  late->voice(loud,0);
  late->start(plan,true,0);
  out.clear();
  late->update(p,300,0,out);
  plan->setWord(3,.5f,.8f);
  late->update(p,400,0,out);
  require(out.cues.size()==1 && out.cues[0].at==400,"the time of a word that comes later must replace the estimate");
  // a mouth gesture with a hold under 0.1 s found late in its pause: it starts now with its own hold
  plan = std::make_shared<Plan>("aaaa bbbb cccc dddd");
  plan->cues = {{"chin",1,0}};
  SpeechCues shortHold;
  shortHold.voice(loud,0);
  shortHold.start(plan,true,0);
  out.clear();
  shortHold.update(p,300,0,out);
  require(out.cues.empty(),"without words the mouth gesture must wait for the end of the sound");
  plan->setWord(0,0.f,.1f); plan->setWord(1,.15f,.2f); plan->setWord(2,.5f,1.f);
  shortHold.update(p,400,0,out);
  require(out.cues.size()==1 && out.cues[0].name=="chin" && out.cues[0].at==400 && close(out.cues[0].hold,50.f),
          "a late mouth gesture shorter than 0.1 s must keep its own hold");
  }

// plays a line with a leak from tick 0 in 10 ms steps: what it starts, and while its smile is posed
struct Played final {
  std::vector<SpeechCues::Out> leaks;   // the steps that start a leak
  std::vector<uint64_t>        blinks;  // ticks of the liar's blinks
  std::vector<uint64_t>        reserve;
  uint64_t                     posedUntil = 0;  // the last tick posed
  SpeechCues                   sc;
  };

void playLeak(const FaceProfile& p, std::shared_ptr<SpeechCues::Plan> plan, std::shared_ptr<LipSync::Track> track,
              uint64_t lastBlink, Played& r, uint64_t until = 4000) {
  r.sc.voice(track,0);
  r.sc.start(plan,true,0);
  SpeechCues::Out out;
  for(uint64_t now=0; now<=until; now+=10) {
    out.clear();
    r.sc.update(p,now,lastBlink,out);
    if(out.leak)
      r.leaks.push_back(out);
    for(uint32_t i=0; i<out.blinks; ++i)
      r.blinks.push_back(now);
    r.reserve.insert(r.reserve.end(),out.reserve.begin(),out.reserve.end());
    if(out.posed)
      r.posedUntil = now;
    }
  }

void leakCases(const GltfHeadMesh& head, const FaceProfile& p) {
  // the curve: as a cue's part, on 120 / hold 120 / off 160 ms; at the apex the table's weights
  FaceRig rig;
  rig.setup(&head,&p);
  const auto& w = rig.weights();
  require(!rig.startLeak("angry",false,0,0),"a leak the table does not have must not start");
  require(rig.startLeak("afraid",false,1000,1000),"afraid upper must start");
  rig.update(1060);
  require(close(w[BrowUpLeft],0.45f*ease(0.5f)),"the leak must rise by the smooth step");
  rig.update(1180);
  require(std::abs(w[BrowUpLeft]-0.45f)<=0.01f && std::abs(w[BrowUpRight]-0.43f)<=0.01f && w[SmileLeft]==0,
          "the apex must give the weights of the table, of its half only");
  FaceRig::Probe pr;
  rig.probe(pr,1180);
  require(pr.shown=="leak:afraid:upper 0.45" && close(rig.leakWeight(1180),0.45f),"the probe must show the leak: "+pr.shown);
  rig.update(1320);
  require(close(w[BrowUpLeft],0.45f*(1.f-ease(0.5f))),"the leak must fall by the smooth step");
  rig.update(1399);
  require(w[BrowUpLeft]>0 && rig.leakWeight(1399)>0,"the leak must last 0.40 s");
  require(rig.update(1401)==FaceRig::Result::Rest && rig.leakWeight(1401)==0,"the leak must end after 0.40 s");

  // it adds to a face and a gesture and puts out neither; a new one takes the place of the one going
  rig.startAnim("S_FRIENDLY",1,uint64_t(-1),1500);
  rig.startCue("smirk",false,-1,3000,3000);
  rig.startLeak("afraid",true,3300,3300);
  rig.update(3480);
  require(close(w[SmileLeft],0.75f+0.2f) && close(w[BrowDown],0.4f) && close(w[EyeSquint],0.2f) && close(w[CheekSquint],0.3f),
          "a leak must add to the gesture and the face");
  require(close(rig.weight("S_FRIENDLY",3480),1.f) && close(rig.cueWeight("smirk",3480),1.f),"a leak must put out nothing");
  rig.startLeak("afraid",false,3480,3480);
  rig.update(3660);
  require(close(w[SmileLeft],0.75f*(1.f-ease(0.2f))) && close(w[BrowUpLeft],0.45f),"a new leak must take the place of the one going");

  // a posed smile: the squint of the expression cut, the leak's own kept; back over 0.3 s after it
  rig.setPosed(true,4000);
  rig.startLeak("friendly",false,4000,4000);
  rig.update(4180);
  require(w[CheekSquint]==0 && close(w[EyeSquint],0.45f) && close(w[BrowDown],0.4f),
          "a posed smile must cut the expression's squint, not the leak's");
  rig.update(4500);
  require(w[EyeSquint]==0 && w[CheekSquint]==0 && close(w[BrowDown],0.4f),"a posed smile must keep the rest of the face");
  rig.setPosed(false,5000);
  rig.update(5150);
  require(close(w[EyeSquint],0.1f) && close(w[CheekSquint],0.15f),"the squint must come back over 0.3 s");
  rig.update(5300);
  require(close(w[EyeSquint],0.2f) && close(w[CheekSquint],0.3f),"after it the expression must have its squint");

  // a line with a leak: one, 0.1 s before its word; the liar's two blinks in the sound, 0.5 s from each other and from
  // the ordinary blink, outside the leak +-0.15 s; the line friendly: posed until the end of its sound
  auto plan = std::make_shared<SpeechCues::Plan>("one two three four five six");
  plan->setWord(0,0.f,.4f); plan->setWord(1,.5f,.9f); plan->setWord(2,1.f,1.3f); plan->setWord(3,1.4f,2.f);
  plan->leak     = SpeechCues::Plan::Leak{"friendly",false,2};
  plan->friendly = true;
  plan->seed     = SpeechCues::seedOf("NONE_100_XARDAS/AIB_1/leak");
  auto loud = voice(3000,{{0,3000}});
  Played r;
  playLeak(p,plan,loud,300,r);
  require(r.leaks.size()==1 && r.leaks[0].leakAt==900 && r.leaks[0].leakFelt=="friendly" && !r.leaks[0].leakLower,
          "a line must start its leak once, 0.1 s before its word");
  const auto& b = r.sc.liarBlinks();
  require(r.blinks.size()==2 && b.size()==2 && r.reserve.size()==2 && near(r.reserve[0],r.blinks[0],10) &&
          near(r.reserve[1],r.blinks[1],10),
          "a line with a leak must have two more blinks, reserved for when they come");
  for(auto x:b)
    require(x>=0 && x+int64_t(SpeechCues::BlinkMs)<=3000 && std::abs(x-300)>=500 && (x+160<=750 || x>=1450),
            "a liar's blink must lie in the sound, 0.5 s from the ordinary one and outside the leak: "+std::to_string(x));
  require(b[1]-b[0]>=500,"the liar's blinks must be 0.5 s apart");
  require(r.posedUntil>=2990 && r.posedUntil<3010,"a friendly line with a leak must be posed until its sound ends");
  Played again;
  playLeak(p,plan,loud,300,again);
  require(again.sc.liarBlinks()==b,"the liar's blinks must come by the seed of the npc and the line");

  // without a leak: no leak, no blinks, a friendly line not posed
  auto truth = std::make_shared<SpeechCues::Plan>("one two three four");
  truth->cues     = {{"flash",2,0}};
  truth->friendly = true;
  Played t;
  playLeak(p,truth,loud,300,t);
  require(t.leaks.empty() && t.blinks.empty() && t.posedUntil==0 && !truth->posed(),"a line without a leak must not leak or pose");

  // the ordinary blinks keep away from the liar's
  ::EyeBlink::setParam("MinGap",100);
  ::EyeBlink::setParam("RestMean",50);
  ::EyeBlink::setParam("DoubleProb",0);
  ::EyeBlink e;
  ::EyeBlink::Input in;
  in.allowed = true;
  in.dt      = 10;
  std::vector<uint64_t> ordinary;
  for(uint64_t now=10; now<=4000; now+=10) {
    in.now = now;
    if(now==1000)
      e.reserve(2000);
    uint64_t hold = 0;
    if(e.update(in,hold))
      ordinary.push_back(now);
    }
  ::EyeBlink::setParam("MinGap",1200);
  ::EyeBlink::setParam("RestMean",3000);
  ::EyeBlink::setParam("DoubleProb",0.1f);
  bool away = ordinary.size()>10;
  for(auto o:ordinary)
    away = away && (o<=1500 || o>=2500);
  require(away && e.lastBlink()==ordinary.back(),"the ordinary blinks must keep 0.5 s from a reserved one");

  // the bridge: say with a leak and without
  const auto read = [](const char* json) {
    rapidjson::Document d;
    d.Parse(json);
    const std::string text = d.HasMember("text") ? d["text"].GetString() : "";
    return LineCues::read(d,"NONE_100_XARDAS","AIB_1",text,false);
    };
  auto said = read(R"({"type":"say","npc":"NONE_100_XARDAS","line":"AIB_1","text":"one two three four","mood":"friendly",)"
                   R"("leak":{"felt":"afraid","half":"lower","word":2},"words":[[0,0,0.4],[1,0.5,0.9],[2,1.0,1.3],[3,1.4,2.0]]})");
  require(said!=nullptr && said->leak.has_value() && said->leak->felt=="afraid" && said->leak->lower && said->leak->word==2 &&
          said->cues.empty() && said->posed() && said->seed==SpeechCues::seedOf("NONE_100_XARDAS/AIB_1/leak"),
          "say.leak must give a plan of its own, without gestures");
  Played s;
  playLeak(p,said,loud,0,s);
  require(s.leaks.size()==1 && s.leaks[0].leakAt==900 && s.leaks[0].leakLower,"the leak of a say must start 0.1 s before its word");
  require(read(R"({"type":"say","text":"one two","mood":"friendly"})")==nullptr,"a say without gestures and leak needs no plan");
  auto cuesOnly = read(R"({"type":"say","text":"one two","cues":[{"cue":"flash","word":1}]})");
  require(cuesOnly!=nullptr && cuesOnly->cues.size()==1 && !cuesOnly->leak.has_value(),"a say with gestures only must have no leak");
  require(read(R"({"type":"say","text":"one two","leak":{"felt":"sad","half":"upper","word":0}})")==nullptr &&
          read(R"({"type":"say","text":"one two","leak":{"felt":"afraid","half":"upper","word":2}})")==nullptr,
          "a wrong leak must be left out");
  }
}

int main() {
  try {
    const auto head    = makeHead();
    const auto profile = FaceProfile::parse(Profile,head);
    profileCases(head,*profile);
    rigCases(head,*profile);
    cueCases(head,*profile);
    timingCases(*profile);
    leakCases(head,*profile);
    }
  catch(const std::exception& e) {
    std::cerr << "facerig test: FAIL: " << e.what() << " (after " << assertions << " checks)\n";
    return 1;
    }
  std::cout << "facerig test: " << assertions << " checks passed\n";
  return 0;
  }
