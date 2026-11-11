// The own head's face (graphics/mesh/facerig): a synthetic head and profile - the weights summed and clamped, fade in
// and out, frames in time, one anim per layer, VISEME shapes set by hand, the voice quieting the mouth, the deformation
// (normals normalized, laid out through renderVertexIds with padding), no blend while the weights stay, profile
// rejections with their reason; the gestures (cues): their curve, the sum with the other layers, the side, and when a
// line plays them (SpeechCues: the time of words, pauses, the estimate by characters); the leak of a felt emotion
// (leaks): its curve, one a line, the liar's blinks, the posed smile, say.leak read by the bridge. The eyes
// (graphics/mesh/eyes) on a head with two balls: the balls turned rigidly around their fitted centres (positions and
// normals), the limits, convergence on a near point, the eyes turning back as the head turns, saccades by the main
// sequence with a bell of speed and no overshoot, smooth pursuit and catch-up jumps, the lids by the profile's table
// summed with the other weights, the same jumps for the same seed, rejections of the "eyes" section. The gestures of
// the eyes (cues of zone "eyes": glance by saccades, roll along an arc): how far and when, back on the point, no
// shift of the gaze for EyeBlink, the point Gaze turns the head to stays, when a line plays them, a profile without
// eyes leaving them out, rejections of "look". CPU only: no device, no game data.
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/eyes.h"
#include "graphics/mesh/speechcues.h"
#include "gltfmesh.h"
#include "linecues.h"

#include <algorithm>
#include <vector>

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

std::string eyeSection(const std::string& body, const std::string& cues = "") {
  return R"({"version":1,"mouth":[],"anims":{"R_EYESBLINK":{"layer":4,"blend_in_ms":0,"blend_out_ms":0,"duration_ms":0,)"
         R"("frame_ms":1000,"frames":[{"eyeBlinkLeft":1,"eyeBlinkRight":1}]}})" + (body.empty() ? "" : ",\"eyes\":"+body) +
         (cues.empty() ? "" : ",\"cues\":"+cues) + "}";
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

// the gestures of the eyes as in test-head.face.json
const std::string EyeCues = R"({"glance":{"zone":"eyes","side":"R","look":{"move":"jump","side_deg":22,"up_deg":-4,)"
  R"("on_ms":80,"hold_ms":600,"off_ms":120}},"roll":{"zone":"eyes","side":"R","look":{"move":"arc","side_deg":10,)"
  R"("up_deg":22,"on_ms":250,"hold_ms":300,"off_ms":300},"parts":[{"weights":{"eyeBlinkLeft":0.2,"eyeBlinkRight":0.2},)"
  R"("delay_ms":0,"on_ms":250,"hold_ms":300,"off_ms":300}]}})";
const std::string EyeBody = R"({"parts":{"left":"eye_L","right":"eye_R"},"limits_deg":{"out":35,"in":30,"up":25,"down":30},)" +
                            EyeLids + "}";

void eyeCueCases(const GltfHeadMesh& head) {
  const auto p = FaceProfile::parse(eyeSection(EyeBody,EyeCues),head);
  const auto* glance = p->findCue("glance");
  const auto* roll   = p->findCue("roll");
  require(glance!=nullptr && glance->look.has_value() && glance->parts.empty() && glance->side=='R' &&
          roll!=nullptr && roll->look.has_value() && roll->parts.size()==1 &&
          roll->look->move==FaceProfile::Cue::Look::Move::Arc && close(roll->span(),850.f),"the gestures of the eyes must be read");
  // a head without eyes: the profile is read, its gestures of the eyes are left out
  const auto blind = FaceProfile::parse(eyeSection("",EyeCues),head);
  require(!blind->eyes.has_value() && blind->findCue("glance")==nullptr && blind->findCue("roll")==nullptr,
          "without eyes the gestures of the eyes must be left out");
  const auto rejected = [&](const std::string& cues, std::string_view reason) {
    try {
      (void)FaceProfile::parse(eyeSection(EyeBody,cues),head);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,
              "the look rejection must say why: want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"the gesture of the eyes must be rejected: "+std::string(reason));
    };
  const std::string look = R"("on_ms":80,"hold_ms":600,"off_ms":120})";
  rejected(R"({"g":{"zone":"eyes"}})",                                              "cue \"g\": a gesture of the eyes must have look");
  rejected(R"({"g":{"zone":"eyes","look":{"move":"spin","side_deg":22,"up_deg":0,)"+look+"}}", "cue \"g\": look: move must be");
  rejected(R"({"g":{"zone":"eyes","look":{"move":"jump","side_deg":22,"up_deg":0,"speed":1,)"+look+"}}",
           "cue \"g\": look: unknown field \"speed\"");
  rejected(R"({"g":{"zone":"eyes","look":{"move":"jump","side_deg":22,)"+look+"}}", "cue \"g\": look: no up_deg");
  rejected(R"({"g":{"zone":"eyes","look":{"move":"jump","side_deg":25,"up_deg":0,"on_ms":50,"hold_ms":600,"off_ms":120}}})",
           "cue \"g\": look: a jump that far takes 76 ms");
  rejected(R"({"g":{"zone":"eyes","look":{"move":"arc","side_deg":10,"up_deg":28,)"+look+"}}",
           "cue \"g\": look: the turn must be within limits_deg");
  rejected(R"({"g":{"zone":"upper","look":{"move":"jump","side_deg":22,"up_deg":0,)"+look+R"(,"parts":[]}})",
           "cue \"g\": only a gesture of the eyes");

  // the other one 110 cm ahead (talking), the eyes on it; Eyes keeps to it (no own jumps) between the gestures
  const Tempest::Vec3 face = {EyeX, 0, EyeZ+110};  // the head bone is the frame (headMatrix(0) turned to the world)
  Eyes eyes;
  eyes.setup(&*p->eyes,7);
  Eyes::Input in;
  in.roam   = false;
  in.target = true;
  in.head   = headMatrix(0);
  in.at     = headPoint(in.head,0,0,110);
  Eyes::Probe pr;
  uint64_t now = 1000;
  const auto step = [&](uint64_t ms) {
    now += ms;
    in.now = now;
    eyes.update(in);
    eyes.probe(pr);
    };
  // degrees each eye looks away from the other one's face, and its turn from where it looked at it
  float restYaw[2] = {}, restPitch[2] = {};
  const auto off = [&](int s) {
    const float y = pr.yaw[s]*Deg, pt = pr.pitch[s]*Deg;
    const Tempest::Vec3 look = {std::sin(pt), std::cos(pt)*std::sin(y), std::cos(pt)*std::cos(y)};
    Tempest::Vec3 to = face - Tempest::Vec3(EyeX, s==0 ? EyeY : -EyeY, EyeZ);
    to /= to.length();
    return std::acos(std::clamp(Tempest::Vec3::dotProduct(look,to),-1.f,1.f))/Deg;
    };
  for(int i=0; i<100; ++i)
    step(10);
  for(int s=0; s<2; ++s) {
    restYaw[s]   = pr.yaw[s];
    restPitch[s] = pr.pitch[s];
    }
  const float rise = pr.rise, down = pr.down;

  // glance: a saccade 22 degrees aside and 4 down, the hold, a saccade back; on its side, mirrored to the other one
  for(bool mirror:{false,true}) {
    const auto big = pr.big, jumps = pr.saccades;
    const uint64_t t0 = now;
    require(eyes.startCue(*glance,mirror,t0),"glance must start");
    float worstRise = 0;
    while(now<t0+400) {
      step(1);
      worstRise = std::max({worstRise,std::abs(pr.rise-rise),std::abs(pr.down-down)});
      }
    for(int s=0; s<2; ++s) {
      const float aside = (pr.yaw[s]-restYaw[s])*(mirror ? 1.f : -1.f);
      require(off(s)>=19 && off(s)<=26 && aside>=19,"glance must look 20-25 degrees aside to its side, got "+
              std::to_string(off(s))+" ("+std::to_string(aside)+" to its side)");
      require(pr.pitch[s]-restPitch[s]<=-3 && pr.pitch[s]-restPitch[s]>=-5,"glance must look 3-5 degrees down");
      }
    require(pr.kind==Eyes::Kind::Gesture && pr.cue=="glance" && close(pr.cueWeight,1) && close(eyes.cueWeight("glance",now),1) &&
            pr.saccades==jumps+1,"the probe must show glance at its apex, after one saccade");
    while(now<t0+800-16) {
      step(1);
      worstRise = std::max({worstRise,std::abs(pr.rise-rise),std::abs(pr.down-down)});
      }
    require(off(0)<1 && off(1)<1,"0.8 s after it starts glance must be back on the other one (a frame early), got "+
            std::to_string(off(0)));
    while(now<t0+1000)
      step(1);
    require(off(0)<1 && off(1)<1 && pr.kind==Eyes::Kind::Target && pr.cue.empty() && eyes.cueWeight("glance",now)==0,
            "after glance the eyes must stay on the other one");
    require(pr.saccades==jumps+2 && pr.big==big,"glance must be two saccades and no shift of the gaze for EyeBlink");
    require(worstRise<1e-3f,"the point Gaze turns the head to must stay while the eyes glance");
    }

  // roll: smoothly up near the limit, over the top from one side to the other, back down; no saccades
  {
    const auto big = pr.big, jumps = pr.saccades;
    const uint64_t t0 = now;
    require(eyes.startCue(*roll,false,t0),"roll must start");
    float top = 0, left = 0, right = 0, worstStep = 0, prev = pr.pitch[0], prevYaw = pr.yaw[0], over = 0;
    while(now<t0+900) {
      step(1);
      top       = std::max(top,std::min(pr.pitch[0]-restPitch[0],pr.pitch[1]-restPitch[1]));
      left      = std::max(left,pr.yaw[0]-restYaw[0]);
      right     = std::max(right,restYaw[0]-pr.yaw[0]);
      worstStep = std::max({worstStep,std::abs(pr.pitch[0]-prev),std::abs(pr.yaw[0]-prevYaw)});
      over      = std::max(over,pr.over);
      prev      = pr.pitch[0];
      prevYaw   = pr.yaw[0];
      require(pr.pitch[0]<=25.001f && pr.pitch[1]<=25.001f,"roll must stay within the sockets");
      }
    require(top>=20,"roll must go at least 20 degrees up, got "+std::to_string(top));
    require(right>=5 && left>=5,"roll must swing to its side and over to the other one");
    require(worstStep<0.5f && pr.saccades==jumps && pr.big==big,
            "roll must be smooth (no saccade, no shift of the gaze): a step of "+std::to_string(worstStep)+" deg/ms");
    require(off(0)<1 && off(1)<1 && pr.cue.empty(),"after roll the eyes must be back on the other one");
    }

  // a line plays them as upper gestures: 0.1 s before the word, on the side of the seed
  auto plan = std::make_shared<SpeechCues::Plan>("one two three four");
  plan->cues = {{"glance",1,'L'},{"roll",3,'R'}};
  plan->setWord(0,0.f,.4f); plan->setWord(1,.5f,.9f); plan->setWord(2,1.f,1.3f); plan->setWord(3,1.4f,2.f);
  const auto f = play(*p,plan,voice(2000,{{0,2000}}));
  require(f.size()==2 && f[0].name=="glance" && f[0].at==400 && f[0].mirror && f[1].name=="roll" && f[1].at==1300 && !f[1].mirror,
          "a line must start the gestures of the eyes 0.1 s before their words, on their side");
  const auto none = play(*blind,plan,voice(2000,{{0,2000}}));
  require(none.empty(),"a head without eyes must leave the gestures of the eyes out");
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
    const auto eyeHead    = makeEyeHead();
    const auto eyeProfile = FaceProfile::parse(EyeProfile,eyeHead);
    eyeProfileCases(eyeHead,*eyeProfile);
    eyeRigCases(eyeHead,*eyeProfile);
    eyeMotionCases(*eyeProfile);
    eyeCueCases(eyeHead);
    }
  catch(const std::exception& e) {
    std::cerr << "facerig test: FAIL: " << e.what() << " (after " << assertions << " checks)\n";
    return 1;
    }
  std::cout << "facerig test: " << assertions << " checks passed\n";
  return 0;
  }
