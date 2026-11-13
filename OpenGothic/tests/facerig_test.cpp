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
// eyes leaving them out, rejections of "look". The tone of a voice (LipSync::Track, YIN) on harmonics and pulses, noise
// and silence without one, the same tone from a stream in pieces; the brows on its accents (SpeechBrows, speech_brows):
// one accent on a glide, the brows from 0.1 s before its syllable, down in an angry line, silent under a brow gesture,
// the same moves for the same seed, none without the section, rejections of it. The face of the one listening
// (ListenFace, listen_face): on its nods, by chance and gap the same for the same seed, a smaller shorter flash, a
// friendly half smile within its part of S_FRIENDLY, an angry or hostile frown and never a smile or a flash, silent
// while the face is taken, none without the section, rejections of it. The lids' shade over the balls (lid_shade,
// LidShade): the edges of the lids found on a head with lid sheets, darker under the upper lid than over the lower,
// more shade as the lids close (a blink, a squint) and as the eye looks up under its lid, the same for the head turned
// whole, a neutral face that keeps it, none without the section, rejections of it. CPU only: no device, no game data.
#include "graphics/eyeshader.h"
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/eyes.h"
#include "graphics/mesh/listenface.h"
#include "graphics/mesh/speechbrows.h"
#include "graphics/mesh/speechcues.h"
#include "graphics/mesh/speechface.h"
#include "gltfmesh.h"
#include "linecues.h"
#include "playervoice.h"
#include "voicebuffer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include <cmath>
#include <cstdint>
#include <cstdlib>
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

// A head for the lids' shade: two balls in rings of 10 degrees, lid sheets over each hugging the ball 1 mm off it
// (the upper one from 15 degrees up, the lower one from 15 down, 60 degrees to each side), a far flat part. The blink
// turns the upper sheets down by 25 degrees, the squint the lower ones up by 10, as the deltas of their vertices
constexpr float LidOpen = 15;
enum LidTarget : uint32_t { LidBlinkL, LidBlinkR, LidSquintL, LidSquintR };

GltfHeadMesh makeLidHead() {
  GltfHeadMesh head;
  auto& m = head.mesh;
  m.sub_meshes.emplace_back();
  auto& sub = m.sub_meshes.back();
  const auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    asset::MeshTriangle t;
    for(uint32_t k=0; k<3; ++k) {
      t.wedges[k] = uint16_t(sub.wedges.size());
      asset::MeshWedge w;
      w.index = uint16_t(k==0 ? a : k==1 ? b : c);
      sub.wedges.push_back(w);
      }
    sub.triangles.push_back(t);
    };
  // a point at yaw, pitch degrees around the centre of the eye of `side`, dist cm away
  const auto at = [](int side, float yaw, float pitch, float dist) {
    const float y = side==0 ? EyeY : -EyeY;
    return asset::Vec3{EyeX + dist*std::sin(pitch*Deg), y + dist*std::cos(pitch*Deg)*std::sin(yaw*Deg),
                       EyeZ + dist*std::cos(pitch*Deg)*std::cos(yaw*Deg)};
    };
  std::vector<std::vector<uint32_t>> moved(4);  // per target: its vertices
  std::vector<asset::Vec3> moveTo;              // per vertex: where its target takes it
  const size_t skin = 0;
  head.parts.push_back({"skin",0,0});
  for(int side=0; side<2; ++side)
    for(int lid=0; lid<2; ++lid) {
      // rows from the edge out, columns of yaw; the edge row is the lid's edge
      const uint32_t first = uint32_t(m.positions.size());
      for(int r=0; r<6; ++r)
        for(int c=0; c<=12; ++c) {
          const float yaw = -60.f+10.f*float(c), off = LidOpen+9.f*float(r);
          const float pitch = lid==0 ? off : -off;
          m.positions.push_back(at(side,yaw,pitch,EyeR+0.1f));
          moveTo.push_back(at(side,yaw,lid==0 ? pitch-25.f : pitch+10.f,EyeR+0.1f));
          moved[size_t(lid*2+side)].push_back(uint32_t(m.positions.size()-1));
          }
      for(uint32_t r=0; r<5; ++r)
        for(uint32_t c=0; c<12; ++c) {
          const uint32_t a = first+r*13+c;
          tri(a,a+1,a+13);
          tri(a+1,a+14,a+13);
          }
      }
  // a fold of the skin behind the left ball, as near to it as the lids but more than 80 degrees from its forward
  // (the pocket of the socket): not a lid
  {
    const uint32_t first = uint32_t(m.positions.size());
    for(float yaw:{150.f,170.f,160.f}) {
      m.positions.push_back(at(0,yaw,yaw==160.f ? 10.f : 0.f,EyeR+0.1f));
      moveTo.push_back(m.positions.back());
      }
    tri(first,first+1,first+2);
  }
  head.parts[skin].vertexCount = m.positions.size();
  for(int side=0; side<2; ++side) {
    const size_t first = m.positions.size();
    for(int t=0; t<=18; ++t)
      for(int f=0; f<(t==0 || t==18 ? 1 : 36); ++f) {
        const float th = float(t)*10.f*Deg, ph = float(f)*10.f*Deg;
        m.positions.push_back({EyeX + EyeR*std::sin(th)*std::sin(ph), (side==0 ? EyeY : -EyeY) + EyeR*std::sin(th)*std::cos(ph),
                               EyeZ + EyeR*std::cos(th)});
        }
    head.parts.push_back({side==0 ? "eye_L" : "eye_R",first,m.positions.size()-first});
    }
  const size_t flat = m.positions.size();
  for(int i=0; i<6; ++i)
    m.positions.push_back({float(i%3),float(i/3),-20});
  tri(uint32_t(flat),uint32_t(flat+1),uint32_t(flat+3));
  head.parts.push_back({"flat",flat,6});
  m.normals.assign(m.positions.size(),{0,0,1});
  for(const char* name:{"eyeBlinkLeft","eyeBlinkRight","eyeSquintLeft","eyeSquintRight"}) {
    head.targets.push_back({name,{}});
    for(auto v:moved[head.targets.size()-1]) {
      const auto& a = m.positions[v];
      const auto& b = moveTo[v];
      head.targets.back().vertices.push_back({v,{b.x-a.x,b.y-a.y,b.z-a.z},{}});
      }
    }
  for(uint32_t i=0; i<m.positions.size(); ++i)
    head.renderVertexIds.push_back(i);
  return head;
  }

const std::string LidShadeBody = R"("part":"skin","upper_cm":0.6,"lower_cm":0.2,"upper_edge":0.2,"lower_edge":0.5)";
std::string lidProfile(const std::string& shade) {
  return R"({"version":1,"mouth":[],"anims":{)"
         R"("R_EYESBLINK":{"layer":4,"blend_in_ms":0,"blend_out_ms":0,"duration_ms":0,"frame_ms":1000,"frames":[{"eyeBlinkLeft":1,"eyeBlinkRight":1}]},)"
         R"("S_SQUINT":{"layer":2,"blend_in_ms":0,"blend_out_ms":0,"duration_ms":0,"frame_ms":1000,"frames":[{"eyeSquintLeft":1,"eyeSquintRight":1}]}},)"
         R"("eyes":{"parts":{"left":"eye_L","right":"eye_R"},"limits_deg":{"out":35,"in":30,"up":25,"down":30},"lids":{})" +
         (shade.empty() ? "" : R"(,"lid_shade":{)"+shade+"}") + "}}";
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

void lidShadeCases() {
  using R = FaceRig::Result;
  const auto head = makeLidHead();
  const auto p    = FaceProfile::parse(lidProfile(LidShadeBody),head);
  require(p->eyes->shade.has_value() && p->eyes->shade->lids[0].corners.size()==2*120*3 && p->eyes->shade->lids[1].corners.size()==2*120*3,
          "the lids of each eye must be the part's triangles around its ball: the far ones and the fold behind it left out");
  const uint32_t left = uint32_t(head.parts[1].firstVertex);
  // a vertex of the left ball at yaw, pitch degrees (rings of 10 degrees from the front pole, 36 around)
  const auto ball = [&](int yaw, int pitch) {
    const float th = std::acos(std::cos(float(pitch)*Deg)*std::cos(float(yaw)*Deg));
    const int   t  = int(std::lround(th/Deg/10.f));
    if(t==0)
      return left;
    const float ph = std::atan2(std::sin(float(pitch)*Deg),std::cos(float(pitch)*Deg)*std::sin(float(yaw)*Deg))/Deg;
    const int   f  = (int(std::lround(ph/10.f))+36)%36;
    return left + 1 + uint32_t((t-1)*36+f);
    };
  const uint32_t pupil = left, high = ball(0,10), low = ball(0,-10);

  FaceRig rig;
  rig.setup(&head,&*p);
  require(rig.hasLidShade() && rig.update(0)==R::Deformed,"with the lids' shade a neutral face must keep its deformation");
  const float open = rig.lidLight(pupil);
  require(open>0.2f && open<1,"the pupil 15 degrees under the upper lid's edge must be in its soft shade");
  require(rig.lidLight(high)<rig.lidLight(low),"under the upper lid the ball must be darker than over the lower one");
  require(close(rig.lidLight(ball(0,30)),0.2f,0.01f),"under the lid the ball must keep the edge's share");
  require(rig.lidLight(uint32_t(head.parts[0].firstVertex))==-1,"the lids' own vertices must carry no shade");

  rig.startAnim("R_EYESBLINK",0.5f,uint64_t(-1),10);
  rig.update(10);
  const float half = rig.lidLight(pupil);
  rig.startAnim("R_EYESBLINK",1,uint64_t(-1),20);
  rig.update(20);
  const float shut = rig.lidLight(pupil);
  require(half<open-0.05f && shut<half && shut<=0.2f+1e-3f,"the shade must grow as the upper lid closes over the pupil");
  rig.startAnim("R_EYESBLINK",1,0,30);
  rig.update(30);
  require(close(rig.lidLight(pupil),open,1e-4f),"the lid open again must give the shade back");

  // the squint: the lower lid up 10 degrees over the ball
  rig.startAnim("S_SQUINT",1,uint64_t(-1),32);
  rig.update(32);
  const float squint = rig.lidLight(low);
  require(squint<rig.lidLight(pupil) && squint<0.5f+1e-3f,"the lower lid up over the ball must shade it");
  rig.startAnim("S_SQUINT",1,0,34);
  rig.update(34);

  // the eye looking up under its upper lid
  rig.setEyes({0,20*Deg},{});
  rig.update(40);
  require(rig.lidLight(pupil)<open-0.1f,"the pupil turned up under the lid must go into its shade");
  rig.setEyes({0,-10*Deg},{});
  rig.update(50);
  require(rig.lidLight(pupil)>open,"the pupil turned down, away from the upper lid, must come out of its shade");
  rig.setEyes({},{});
  rig.update(60);


  // without the section: no shade written, the neutral face rests as before
  const auto plain = FaceProfile::parse(lidProfile(""),head);
  FaceRig none;
  none.setup(&head,&*plain);
  require(!none.hasLidShade() && none.update(0)==R::Rest,"without lid_shade the neutral face must rest");
  none.setEyes({0,20*Deg},{});
  none.update(10);
  require(none.lidLight(pupil)==-1,"without lid_shade no shade must be written");
  // a head of the old export: the profile has eyes but no lid_shade (the rest after a deformation: eyeRigCases).
  // A blink blends the lids as before and writes nothing of the shade
  none.startAnim("R_EYESBLINK",1,uint64_t(-1),20);
  none.update(20);
  require(none.lidLight(pupil)==-1,"without lid_shade a blink must write no shade");

  // OPENGOTHIC_EYE_SHADER=0: a profile with lid_shade is rigged as one without
  setEyeShaderEnabled(false);
  FaceRig off;
  off.setup(&head,&*p);
  const auto offRest = off.update(0);
  off.setEyes({0,20*Deg},{});
  off.update(10);
  setEyeShaderEnabled(true);
  require(!off.hasLidShade() && offRest==R::Rest && off.lidLight(pupil)==-1,
          "with the eye shader off the face rig must not compute the lids' shade");

  const auto rejected = [&](const std::string& shade, std::string_view reason) {
    try {
      (void)FaceProfile::parse(lidProfile(shade),head);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,
              "the lid_shade rejection must say why: want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"lid_shade must be rejected: "+std::string(reason));
    };
  rejected(LidShadeBody+R"(,"soft":1)",                                   "eyes: lid_shade: unknown field \"soft\"");
  rejected(R"("part":"lids","upper_cm":0.6,"lower_cm":0.2,"upper_edge":0.2,"lower_edge":0.5)", "eyes: lid_shade: no head part \"lids\"");
  rejected(R"("part":"eye_L","upper_cm":0.6,"lower_cm":0.2,"upper_edge":0.2,"lower_edge":0.5)", "eyes: lid_shade: part must be the lids'");
  rejected(R"("part":"skin","upper_cm":0,"lower_cm":0.2,"upper_edge":0.2,"lower_edge":0.5)",    "eyes: lid_shade: upper_cm must be above 0");
  rejected(R"("part":"skin","upper_cm":0.6,"lower_cm":1.6,"upper_edge":0.2,"lower_edge":0.5)",  "eyes: lid_shade: lower_cm must be above 0 and up to the ball's radius");
  rejected(R"("part":"skin","upper_cm":0.6,"lower_cm":0.2,"upper_edge":0.2,"lower_edge":1.5)",  "eyes: lid_shade: lower_edge must be a number 0..1");
  rejected(R"("part":"skin","upper_cm":0.6,"upper_edge":0.2,"lower_edge":0.5)",                 "eyes: lid_shade: no lower_cm");
  rejected(R"("part":"flat","upper_cm":0.6,"lower_cm":0.2,"upper_edge":0.2,"lower_edge":0.5)",  "has no lids around the left eye");
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

// a voice: pieces of [ms, Hz at the start, Hz at the end, amplitude], the phase going on through them; harmonics 1..8
// at 1/k, or pulses (one sample a period)
struct Piece final {
  uint64_t ms = 0;
  float    from = 0, to = 0, amp = 0;
  };

std::vector<int16_t> sing(uint32_t rate, const std::vector<Piece>& pieces, bool pulses = false) {
  std::vector<int16_t> pcm;
  double phase = 0;
  for(auto& p:pieces) {
    const size_t n = size_t(p.ms*rate/1000);
    for(size_t i=0; i<n; ++i) {
      const double f = p.from + (p.to-p.from)*double(i)/double(n);
      const double before = phase;
      phase += f/rate;
      double x = 0;
      if(pulses)
        x = std::floor(phase)!=std::floor(before) ? 1.0 : 0.0;
      else
        for(int k=1; k<=8; ++k)
          x += std::sin(2.0*M_PI*k*phase)/k;
      pcm.push_back(int16_t(std::lround(std::clamp(x*0.4*p.amp,-1.0,1.0)*32000.0)));
      }
    }
  return pcm;
  }

std::shared_ptr<LipSync::Track> trackOf(uint32_t rate, const std::vector<int16_t>& pcm, size_t piece = 0) {
  auto t = std::make_shared<LipSync::Track>(rate);
  for(size_t at=0; at<pcm.size();) {
    // in pieces of uneven length, as a stream arrives; 0: at once
    const size_t n = piece==0 ? pcm.size() : std::min(pcm.size()-at, piece + (at/piece)%7*113);
    t->append(pcm.data()+at,n);
    at += n;
    }
  t->finish();
  return t;
  }

void playerVoiceCases() {
  const uint32_t rate = 16000;
  const auto pcm = sing(rate,{{200,150,150,1},{200,0,0,0}});
  PlayerVoice voice;
  require(!voice.append(rate,pcm.data(),320,1000) && voice.track()==nullptr,
          "microphone chunks outside PTT must not create a track");
  voice.begin();
  require(voice.append(rate,pcm.data(),320,1000),"the first microphone chunk must start a live track");
  const auto first = voice.track();
  require(!first->isFinished() && first->isLiveInput() && first->playedMs()==0 && first->name().find("player-microphone:")==0,
          "the microphone track must start live, with its own name and waiting clock");
  LipSync mouth;
  mouth.setShapes({0,0.5f,1});
  mouth.start(first,9000);  // world time differs from the microphone's arrival clock
  const uint64_t buffer = LipSync::liveBufferMs();
  voice.tick(1000+buffer);
  require(first->playedMs()==0,"the microphone buffer must hold the clock behind the first chunk");
  voice.append(rate,pcm.data()+320,320,1020);
  voice.append(rate,pcm.data()+640,320,1040);
  LipSync::Shape shape;
  mouth.update(9000,16,shape);
  LipSync::Probe probe;
  mouth.probe(probe);
  require(uint64_t(first->playedMs())==40-buffer && probe.time==40-buffer && probe.level>0 &&
          close(probe.level,first->level(40-buffer)),
          "a silent live track must drive the mouth by its buffered arrival time and level");
  for(size_t i=3; i<pcm.size()/320; ++i)
    voice.append(rate,pcm.data()+i*320,320,1000+i*20);
  voice.tick(1000+voice.durationMs());
  require(uint64_t(first->playedMs())+buffer==voice.durationMs() && mouth.isSpeaking(9001),
          "the hero must still be speaking with exactly one microphone buffer left before finish");
  voice.finish();
  require(first->isFinished() && !mouth.isSpeaking(9001),"finishing the microphone must end the hero's speech");
  require(!voice.append(rate,pcm.data(),320,5001),"cancelled microphone chunks must be ignored");
  voice.begin();
  require(voice.append(rate,pcm.data(),320,6000) && voice.track()!=first && voice.track()->playedMs()==0 &&
          voice.track()->name()!=first->name() && voice.durationMs()==20,
          "a new key press must create a fresh microphone track and reset its clock and duration");
  voice.tick(10000);
  const auto restarted = voice.track();
  require(restarted->playedMs()==20 && restarted->playedMs()<=int64_t(voice.durationMs()) &&
          restarted->level(uint64_t(restarted->playedMs()))==restarted->level(10) && restarted->level(10)>0,
          "the microphone clock must not run ahead of appended samples, and a step not here yet holds the last one");
  voice.finish();

  // a breath before the first word (30 dB below the voice): a stream of an npc stretches it to its own loudest
  // moment, the microphone measures it against a speech level of its own and keeps the mouth shut
  const auto breath = sing(rate,{{200,150,150,0.03f}});
  auto streamed = std::make_shared<LipSync::Track>(rate);
  streamed->append(breath.data(),breath.size());
  voice.begin();
  voice.append(rate,breath.data(),breath.size(),20000);
  require(streamed->level(150)>0.5f && voice.track()->level(150)<0.05f,
          "a breath must not open the hero's mouth: live "+std::to_string(voice.track()->level(150))+", streamed "+
          std::to_string(streamed->level(150)));
  voice.finish();
  // an npc's voice from the speakers in the microphone, 12 dB below the hero's: ducked while it sounds and
  // DuckHoldMs after, then heard as it is
  const auto leak = sing(rate,{{400,150,150,0.25f}});
  voice.begin();
  uint64_t at = 30000;
  for(size_t i=0; i+320<=leak.size(); i+=320, at+=20)
    voice.append(rate,leak.data()+i,320,at,true);
  const uint64_t ducked = voice.durationMs()-20;
  for(uint64_t end=at+PlayerVoice::DuckHoldMs+40; at<end; at+=20)
    voice.append(rate,leak.data(),320,at,false);
  const auto heard = voice.track();
  require(heard->peak(0,ducked)<0.05f && heard->level(voice.durationMs()-10)>0.3f,
          "an npc's voice in the microphone must not open the hero's mouth while it sounds: "+
          std::to_string(heard->peak(0,ducked))+", after it "+std::to_string(heard->level(voice.durationMs()-10)));
  voice.finish();

  VoiceBuffer npc(rate);
  npc.append(reinterpret_cast<const uint8_t*>(pcm.data()),640);
  require(npc.lipTrack()->playedMs()==0 && !npc.lipTrack()->isLiveInput(),"an NPC stream must still wait for its audio thread");
  std::vector<int16_t> rendered(160);
  npc.render(rendered.data(),rendered.size());
  require(npc.lipTrack()->playedMs()==10 && std::equal(rendered.begin(),rendered.end(),pcm.begin()),
          "an NPC stream must still follow rendered audio and preserve its samples");
  npc.finish();
  npc.render(rendered.data(),rendered.size());
  require(npc.isDrained() && npc.lipTrack()->playedMs()==20,"an NPC stream must still drain at its audio clock");
  auto recorded = trackOf(rate,sing(rate,{{40,0,0,0},{100,150,150,1}}));
  recorded->setPlayed(10);
  LipSync npcMouth;
  npcMouth.setShapes({0,0.5f,1});
  npcMouth.start(recorded,0);
  npcMouth.update(1000,16,shape);
  npcMouth.probe(probe);
  require(probe.time==10 && probe.level>0 && recorded->level(10)==0 && close(probe.level,recorded->level(50)),
          "an NPC voice must retain its existing 40 ms mouth lead");
  const auto original = trackOf(rate,pcm);
  require(original->playedMs()==-1 && !original->isLiveInput(),"an original NPC voice must retain its world-time clock");
  }

void toneCases() {
  // the tone of harmonics and of pulses at 90, 150, 250 Hz within 2 %, at the rates of the game and of the voice service
  for(uint32_t rate:{22050u,24000u,44100u})
    for(bool pulses:{false,true})
      for(float hz:{90.f,150.f,250.f}) {
        const auto t = trackOf(rate,sing(rate,{{1000,hz,hz,1}},pulses));
        size_t toned = 0;
        float  worst = 0;
        for(uint64_t ms=50; ms<950; ms+=10) {
          const float f = t->toneHz(ms);
          if(f<=0)
            continue;
          ++toned;
          worst = std::max(worst,std::abs(f-hz)/hz);
          }
        const auto what = std::to_string(int(hz))+" Hz "+(pulses ? "pulses" : "harmonics")+" at "+std::to_string(rate);
        require(toned==90 && worst<0.02f,"the tone of "+what+": "+std::to_string(toned)+" of 90 steps, worst error "+
                std::to_string(worst*100)+" %");
        float st = 1;
        require(t->tone(500,st) && std::abs(st)<0.2f,"a steady voice must be at its median: "+what);
        }

  // noise and silence: no tone
  std::vector<int16_t> noise(24000);
  uint32_t r = 12345;
  for(auto& x:noise) {
    r = r*1664525u + 1013904223u;
    x = int16_t(int32_t(r>>16)-32768)/3;
    }
  const auto n = trackOf(24000,noise);
  const auto q = trackOf(24000,std::vector<int16_t>(24000,0));
  size_t noisy = 0, quiet = 0;
  for(uint64_t ms=0; ms<1000; ms+=10) {
    noisy += n->toneHz(ms)>0;
    quiet += q->toneHz(ms)>0;
    }
  require(noisy==0 && quiet==0,"noise and silence must have no tone: "+std::to_string(noisy)+" and "+std::to_string(quiet)+" steps");
  float st = 0;
  require(!q->tone(500,st) && n->durationMs()==1000 && n->toneKnownMs()==1000,"a finished track must know the tone of every step");

  // a stream in pieces: the same tone as the sound at once
  const auto glide = sing(24000,{{300,110,110,1},{100,0,0,0},{150,110,160,1},{200,160,120,1},{150,0,0,0}});
  const auto whole = trackOf(24000,glide), parts = trackOf(24000,glide,480);
  bool same = whole->durationMs()==parts->durationMs();
  for(uint64_t ms=0; ms<whole->durationMs() && same; ms+=10)
    same = whole->toneHz(ms)==parts->toneHz(ms);
  require(same,"a stream in pieces must give the same tone as the sound at once");
  auto stream = std::make_shared<LipSync::Track>(24000);
  stream->append(glide.data(),4800);
  require(stream->toneKnownMs()<=200 && stream->toneKnownMs()>=170,"a stream must know the tone up to 25 ms before what has come: "+
          std::to_string(stream->toneKnownMs()));

  // the cost: ms of counting a second of sound (the tone with the loudness)
  const auto speech = sing(44100,{{10000,100,180,1}});
  for(uint32_t rate:{24000u,44100u}) {
    const auto pcm = rate==44100 ? speech : sing(rate,{{10000,100,180,1}});
    const auto t0  = std::chrono::steady_clock::now();
    const auto t   = trackOf(rate,pcm);
    const auto ms  = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
    std::printf("tone and loudness at %u Hz: %.2f ms of counting a second of sound\n",rate,ms/10.0);
    }
  }

const char* BrowsSection = R"(, "speech_brows": {
  "about": "synthetic", "rise_st": 2.5, "rise_ms": 200, "syllable_ms": 150, "loud": 0.4, "lead_ms": 100,
  "on_ms": 120, "hold_ms": 120, "off_ms": 300, "gap_ms": 700, "chance": 1,
  "up": {"browUpLeft": 0.5, "browUpRight": 0.5}, "down": {"browDown": 0.6}, "quiet_cues": ["flash", "doubt"]})";

std::string withBrows(const std::string& section) {
  std::string json = Profile;
  json.insert(json.rfind('}'),section);
  return json;
  }

// plays the track from tick 0 in 10 ms steps: the ticks the brows start at
std::vector<uint64_t> playBrows(const FaceProfile::Brows& b, std::shared_ptr<LipSync::Track> t, uint32_t seed,
                                SpeechBrows& sb) {
  sb.start(t,seed,0);
  std::vector<uint64_t> ret;
  SpeechBrows::Start s;
  for(uint64_t now=0; now<=t->durationMs()+500; now+=10)
    while(sb.update(b,now,s))
      ret.push_back(s.at);
  return ret;
  }

void browCases(const GltfHeadMesh& head, const FaceProfile& plain) {
  const auto p = FaceProfile::parse(withBrows(BrowsSection),head);
  require(p->brows.has_value() && p->brows->quiet.size()==2 && p->brows->up.size()==2 && !plain.brows.has_value(),
          "speech_brows must be read; a profile without it has none");

  // a glide of 5.8 semitones over 150 ms in a loud syllable: one accent, the syllables around it level
  const auto glide = sing(16000,{{200,100,100,1},{120,0,0,0},{150,100,140,1},{100,140,140,1},{120,0,0,0},
                                 {220,140,135,1},{200,0,0,0}});
  SpeechBrows sb;
  const auto starts = playBrows(*p->brows,trackOf(16000,glide),7,sb);
  require(sb.accents().size()==1 && near(uint64_t(sb.accents()[0].ms),320,20) && sb.accents()[0].rise>4.f,
          "a glide 100 -> 140 Hz over 150 ms must be one accent at its syllable: "+std::to_string(sb.accents().size())+
          (sb.accents().empty() ? "" : " at "+std::to_string(sb.accents()[0].ms)+" ms, rise "+std::to_string(sb.accents()[0].rise)));
  require(starts.size()==1 && starts[0]==uint64_t(sb.accents()[0].ms)-100 && sb.moves()==1,
          "the brows must start 0.1 s before the syllable of the accent");

  // the face: up from 0.1 s before the syllable (within a frame), the weights of up; an angry line - down
  FaceRig rig;
  rig.setup(&head,p.get());
  const auto& w = rig.weights();
  const uint64_t syl = uint64_t(sb.accents()[0].ms);
  for(bool down:{false,true}) {
    const uint64_t base = down ? 5000 : 0;
    rig.startBrows(down && SpeechBrows::lowers(TalkGestures::moodOf("angry")),1,base+syl-100,base+syl-100);
    uint64_t first = 0;
    for(uint64_t now=base+syl-200; now<base+syl+100 && first==0; now+=33) {
      rig.update(now);
      if(rig.browWeight(now)!=0)
        first = now;
      }
    require(first>=base+syl-100 && first<=base+syl-100+34,"the brows must start rising 0.1 s before the syllable, within a frame");
    rig.update(base+syl+100);
    if(!down)
      require(close(w[BrowUpLeft],0.5f) && close(w[BrowUpRight],0.5f) && w[BrowDown]==0 && rig.browWeight(base+syl+100)>0.99f,
              "an accent must raise the brows by the weights of up");
    else
      require(close(w[BrowDown],0.6f) && w[BrowUpLeft]==0 && rig.browWeight(base+syl+100)<-0.99f,
              "an accent of an angry line must lower the brows by the weights of down");
    rig.update(base+syl+1000);
    }
  // found late: a move up to 50 ms late starts in the past, at its time; later, before its syllable - now
  {
    const int64_t at = int64_t(syl)-100;
    SpeechBrows::Start s;
    SpeechBrows late;
    late.start(trackOf(16000,glide),7,0);
    require(late.update(*p->brows,uint64_t(at+30),s) && s.at==uint64_t(at),
            "a move found 30 ms late must start back at its time: "+std::to_string(s.at)+" for "+std::to_string(at));
    late.start(trackOf(16000,glide),7,0);
    require(late.update(*p->brows,uint64_t(at+80),s) && s.at==uint64_t(at+80),"a move found 80 ms late must start now");
    late.start(trackOf(16000,glide),7,0);
    require(!late.update(*p->brows,syl+20,s),"a move whose syllable has gone by must not start");
  }
  // the live microphone: no sound ahead, so the accent is found in its syllable and the brows start then
  {
    auto live = std::make_shared<LipSync::Track>(16000);
    live->setLiveInput();
    live->setPlayed(0);
    SpeechBrows mic;
    mic.start(live,7,0);
    SpeechBrows::Start s;
    std::vector<uint64_t> at;
    for(size_t k=0; k*320<glide.size(); ++k) {
      const uint64_t now = k*20;
      live->append(glide.data()+k*320,std::min<size_t>(320,glide.size()-k*320));
      live->setPlayed(now>LipSync::liveBufferMs() ? now-LipSync::liveBufferMs() : 0);
      while(mic.update(*p->brows,now,s))
        at.push_back(s.at);
      }
    require(at.size()==1 && at[0]>=syl && at[0]<=syl+SpeechBrows::LiveLateMs+LipSync::liveBufferMs(),
            "the brows of a live voice must start within its accented syllable: "+std::to_string(at.size())+" moves"+
            (at.empty() ? "" : " at "+std::to_string(at[0])+" ms, syllable "+std::to_string(syl)));
  }

  // the mood of the line into the face (SpeechBrows::play, as MdlVisual plays it): an angry line of the AI whose voice
  // comes after its subtitle lowers the brows; its sound file (<id>.WAV) too, the mood after the voice; a line of the
  // game after it raises them
  const auto played = [&](std::string_view name, bool moodFirst) {
    FaceRig face;
    face.setup(&head,p.get());
    SpeechBrows sb;
    auto t = trackOf(16000,glide);
    t->setName(name);
    if(moodFirst)
      sb.setMood(TalkGestures::moodOf("angry"),"AIB_7");
    sb.start(t,7,0);
    if(!moodFirst)
      sb.setMood(TalkGestures::moodOf("angry"),"AIB_7");
    uint32_t moves = 0;
    for(uint64_t now=0; now<=syl+40; now+=10) {
      moves += sb.play(face,now);
      face.update(now);
      }
    return std::make_pair(moves,std::make_pair(face.weights()[BrowDown],face.weights()[BrowUpLeft]));
    };
  const auto stream = played("AIB_7",true), file = played("aib_7.wav",false), game = played("DIA_X.WAV",true);
  require(stream.first==1 && stream.second.first>0.3f && stream.second.second==0,
          "an angry line whose voice comes after its mood must lower the brows");
  require(file.first==1 && file.second.first>0.3f && file.second.second==0,
          "an angry line's sound file, its mood set after the voice, must lower the brows");
  require(game.first==1 && game.second.first==0 && game.second.second>0.2f,"a line of the game after an angry one must raise them");

  require(SpeechBrows::lowers(TalkGestures::moodOf("hostile")) && !SpeechBrows::lowers(TalkGestures::moodOf("friendly")) &&
          !SpeechBrows::lowers(TalkGestures::moodOf("neutral")),"only angry and hostile lines must lower the brows");
  FaceRig::Probe pr;
  rig.startBrows(false,0.5f,8000,8000);
  rig.update(8200);
  rig.probe(pr,8200);
  require(pr.shown=="brows:up 0.50" && close(pr.brows,0.5f),"the probe must show the brows of the voice: "+pr.shown);

  // a brow gesture has the brows: the layer is silent, the sum is the gesture's alone
  rig.startCue("flash",false,-1,10000,10000);
  rig.startBrows(false,1,10000,10000);
  rig.update(10150);
  require(rig.browWeight(10150)==0 && close(w[BrowUpLeft],0.7f) && close(w[BrowUpRight],0.7f),
          "the brows of the voice must be silent while a brow gesture shows");
  rig.startLeak("afraid",false,12000,12000);
  rig.startBrows(false,1,12000,12000);
  rig.update(12200);
  require(rig.browWeight(12200)==0,"the brows of the voice must be silent while a leak of the upper face shows");

  // without the section: no layer
  FaceRig none;
  none.setup(&head,&plain);
  none.startBrows(false,1,0,0);
  require(none.update(200)==FaceRig::Result::Rest && none.browWeight(200)==0,"a profile without speech_brows must have no brows of the voice");

  // many accents: by chance and the gap, the same moves for the same seed
  std::vector<Piece> many;
  for(int i=0; i<12; ++i) {
    many.push_back({150,110,160,1});
    many.push_back({100,160,150,1});
    many.push_back({150,0,0,0});
    }
  auto chancy = withBrows(BrowsSection);
  chancy.replace(chancy.find("\"chance\": 1"),11,"\"chance\": 0.5");
  const auto half = FaceProfile::parse(chancy,head);
  const auto voiceOf = trackOf(16000,sing(16000,many));
  SpeechBrows a, b, c;
  const auto ma = playBrows(*half->brows,voiceOf,11,a), mb = playBrows(*half->brows,voiceOf,11,b), mc = playBrows(*half->brows,voiceOf,12,c);
  require(a.accents().size()==12 && ma==mb && !ma.empty() && ma.size()<12,"one seed must give the same moves, not on every accent: "+
          std::to_string(ma.size())+" of "+std::to_string(a.accents().size()));
  for(size_t i=1; i<ma.size(); ++i)
    require(ma[i]-ma[i-1]>=700,"the moves must keep the gap");
  require(mc!=ma || c.accents().size()!=12,"another seed must give other moves");

  // rejections
  const auto rejected = [&](const std::string& section, std::string_view reason) {
    try {
      (void)FaceProfile::parse(withBrows(section),head);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,
              "the rejection must say why: want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"the profile must be rejected: "+std::string(reason));
    };
  auto with = [](std::string from, const std::string& what, const std::string& to) {
    from.replace(from.find(what),what.size(),to);
    return from;
    };
  const std::string sec = BrowsSection;
  rejected(with(sec,"\"about\"","\"x\": 1, \"about\""),"speech_brows: unknown field \"x\"");
  rejected(with(sec,"\"browDown\"","\"nose\""),"speech_brows: down: unknown target \"nose\"");
  rejected(with(sec,"\"doubt\"]","\"wink\"]"),"speech_brows: quiet_cues: no cue \"wink\"");
  rejected(with(sec,"\"chance\": 1","\"chance\": 2"),"loud and chance must be numbers 0..1");
  rejected(with(sec,"\"rise_st\": 2.5, ",""),"speech_brows: no rise_st");
  rejected(with(sec,"\"on_ms\": 120","\"on_ms\": 0"),"on_ms and off_ms must be above 0");
  }

const char* ListenSection = R"(, "listen_face": {
  "about": "synthetic", "chance": 0.3, "gap_ms": 3000,
  "flash": {"cue": "flash", "weight": 0.5, "time": 0.5},
  "frown": {"cue": "frown", "weight": 0.6, "time": 0.4},
  "smile": {"weight": 0.5, "on_ms": 300, "hold_ms": 600, "off_ms": 400, "chance": 0.7, "flash": 0.4}})";

std::string withListen(const std::string& section) {
  std::string json = Profile;
  const std::string chin = "\"chin\":";
  json.insert(json.find(chin),R"("frown": {"zone": "upper", "parts": [{"weights": {"browDown": 0.75},
                                          "delay_ms": 0, "on_ms": 300, "hold_ms": 900, "off_ms": 400}]},
    )");
  json.insert(json.rfind('}'),section);
  return json;
  }

void listenCases(const GltfHeadMesh& head, const FaceProfile& plain) {
  const auto p = FaceProfile::parse(withListen(ListenSection),head);
  require(p->listen.has_value() && !plain.listen.has_value() && p->cues[p->listen->flash.cue].name=="flash" &&
          p->cues[p->listen->frown.cue].name=="frown","listen_face must be read; a profile without it has none");
  const auto& l = *p->listen;

  // the flash: the cue's weights times weight, its times times time (flash: 0.7 over 100/80/160 ms -> 0.35 over 50/40/80)
  FaceRig rig;
  rig.setup(&head,p.get());
  const auto& w = rig.weights();
  rig.startListen(FaceRig::ListenKind::Flash,1000,1000);
  rig.update(1070);
  require(close(w[BrowUpLeft],0.35f) && close(w[BrowUpRight],0.35f) && close(rig.listenWeight(1070),1.f),
          "the flash of the one listening must be the flash's weights times weight: "+std::to_string(w[BrowUpLeft]));
  rig.update(1100);
  require(w[BrowUpLeft]<0.35f-0.01f,"the flash of the one listening must be shorter than the flash: falling at 100 ms");
  require(rig.update(1180)==FaceRig::Result::Rest && rig.listenWeight(1180)==0,"the flash of the one listening must be over by 170 ms");

  // the half smile: S_FRIENDLY (browDown 0.4, eyeSquint 0.2, cheekSquint 0.3) times 0.5, rising over on_ms
  float most[3] = {};
  rig.startListen(FaceRig::ListenKind::Smile,2000,2000);
  for(uint64_t now=2000; now<3400; now+=20) {
    rig.update(now);
    most[0] = std::max(most[0],w[BrowDown]);
    most[1] = std::max(most[1],w[EyeSquint]);
    most[2] = std::max(most[2],w[CheekSquint]);
    if(now==2600)
      require(close(w[BrowDown],0.2f) && close(w[EyeSquint],0.1f) && close(w[CheekSquint],0.15f),
              "the half smile must hold S_FRIENDLY times its weight: "+std::to_string(w[BrowDown]));
    }
  require(most[0]<=0.2f+1e-4f && most[1]<=0.1f+1e-4f && most[2]<=0.15f+1e-4f,
          "the half smile must never be stronger than its part of S_FRIENDLY");
  rig.update(3400);
  require(rig.listenWeight(3400)==0 && w[BrowDown]==0 && w[CheekSquint]==0,"the half smile must be over after on, hold and off");

  // the probe shows it
  FaceRig::Probe pr;
  rig.startListen(FaceRig::ListenKind::Frown,5000,5000);
  rig.update(5200);
  rig.probe(pr,5200);
  require(pr.shown=="listen:frown 1.00" && close(pr.listen,1.f) && close(w[BrowDown],0.45f),
          "the probe must show the frown of the one listening, the frown's weights times weight: "+pr.shown);
  rig.update(6000);

  // the face is taken: a gesture, a leak, its own voice, an expression stronger than the smile; a weaker one is not
  require(!rig.listenBusy(7000),"a quiet face must not be taken");
  rig.startCue("doubt",false,-1,7000,7000);
  require(rig.listenBusy(7000),"a gesture of its own must take the face");
  rig.update(9000);
  rig.startLeak("afraid",false,9000,9000);
  require(rig.listenBusy(9000),"a leak must take the face");
  rig.update(10000);
  rig.setShape("VISEME",1,2,0.5f,1,10000);
  require(rig.listenBusy(10000),"its own voice must take the face");
  rig.update(11000);
  require(!rig.listenBusy(11000),"the face must be free again");
  rig.startAnim("S_FRIENDLY",0.3f,uint64_t(-1),11000);
  rig.update(13000);
  require(!rig.listenBusy(13000),"an expression weaker than the smile must leave the face");
  rig.startAnim("S_ANGRY",1,uint64_t(-1),13000);
  rig.update(15000);
  require(rig.listenBusy(15000),"an expression stronger than the smile must take the face");
  // and one coming while it shows silences it
  FaceRig q;
  q.setup(&head,p.get());
  q.startListen(FaceRig::ListenKind::Flash,0,0);
  q.startCue("flash",false,-1,0,0);
  q.update(120);
  require(q.listenWeight(120)==0 && close(q.weights()[BrowUpLeft],0.7f),"a gesture must silence the face of the one listening");
  q.update(1000);
  q.startListen(FaceRig::ListenKind::Smile,1000,1000);
  q.setShape("VISEME",1,2,0.5f,1,1000);
  q.setShape("VISEME",1,2,0.5f,1,1450);
  q.update(1500);
  require(q.listenWeight(1500)==0 && q.weights()[CheekSquint]==0,"its own voice must silence the face of the one listening");

  // the decision: a nod every 1.2 s for two minutes
  using R = ListenFace::React;
  using M = TalkGestures::Mood;
  const auto run = [&](M mood, uint32_t seed, const FaceProfile::Listen& rule) {
    ListenFace lf;
    lf.seed(seed);
    std::vector<std::pair<uint64_t,R>> ret;
    for(uint64_t now=0; now<120000; now+=1200)
      if(const R r = lf.nod(rule,mood,false,now); r!=R::None)
        ret.emplace_back(now,r);
    require(lf.nods()==100 && lf.faces()==ret.size(),"the nods and faces must be counted");
    return ret;
    };
  const auto friendly = run(M::Friendly,5,l), again = run(M::Friendly,5,l), other = run(M::Friendly,6,l);
  require(friendly==again,"one seed must give the same faces");
  require(friendly!=other,"another seed must give other faces");
  require(friendly.size()>=10 && friendly.size()<=34,"about one nod in two or three must get a face: "+std::to_string(friendly.size()));
  for(size_t i=1; i<friendly.size(); ++i)
    require(friendly[i].first-friendly[i-1].first>=3000,"the faces must keep the gap");
  const auto count = [](const std::vector<std::pair<uint64_t,R>>& v, R r) {
    return std::count_if(v.begin(),v.end(),[r](auto& x) { return x.second==r; });
    };
  // without the gap a nod gets a face by the chance alone (0.3 of 100 nods)
  {
    auto nogap  = l;
    nogap.gapMs = 0;
    const auto v = run(M::Neutral,5,nogap);
    require(v.size()>=20 && v.size()<=40,"without the gap about 0.3 of the nods must get a face: "+std::to_string(v.size()));
    // a face on every nod: a friendly one smiles with smile.chance 0.7, flashes as well with smile.flash 0.4 of those:
    // smile alone 0.42, smile and flash 0.28, flash alone 0.3
    nogap.chance = 1;
    const auto f = run(M::Friendly,5,nogap);
    const auto smile = count(f,R::Smile), both = count(f,R::SmileFlash), flash = count(f,R::Flash);
    require(f.size()==100 && smile>=30 && smile<=55 && both>=16 && both<=40 && flash>=18 && flash<=42,
            "a friendly one must smile, smile and flash, or flash by smile.chance and smile.flash: "+std::to_string(smile)+
            " / "+std::to_string(both)+" / "+std::to_string(flash));
  }
  require(count(friendly,R::Smile)>0 && count(friendly,R::SmileFlash)>0 && count(friendly,R::Flash)>0 &&
          count(friendly,R::Frown)==0,"a friendly one must smile, smile and flash, or flash, never frown");
  for(auto mood:{M::Neutral,M::Afraid}) {
    const auto v = run(mood,5,l);
    require(!v.empty() && size_t(count(v,R::Flash))==v.size(),"one neither friendly nor angry must only flash");
    }
  for(auto mood:{M::Angry,M::Hostile})
    for(uint32_t seed=1; seed<=20; ++seed) {
      const auto v = run(mood,seed,l);
      require(!v.empty() && size_t(count(v,R::Frown))==v.size(),"an angry or hostile one must only frown, never smile or flash");
      }
  // the face taken: no face, none drawn by chance
  {
    ListenFace lf;
    lf.seed(5);
    for(uint64_t now=0; now<60000; now+=1200)
      require(lf.nod(l,M::Friendly,true,now)==R::None,"no face while the face is taken");
    require(lf.faces()==0,"no face while the face is taken");
  }
  // into the face
  {
    FaceRig f;
    f.setup(&head,p.get());
    ListenFace::play(f,R::SmileFlash,0);
    f.update(150);
    require(f.listenWeight(FaceRig::ListenKind::Smile,150)>0 && f.listenWeight(FaceRig::ListenKind::Flash,150)>0 &&
            f.listenWeight(FaceRig::ListenKind::Frown,150)==0,"a smile with a flash must show both");
    // an angry one's frown shows only the frown
    ListenFace::play(f,R::Frown,2000);
    f.update(2200);
    require(f.listenWeight(FaceRig::ListenKind::Frown,2200)>0 && f.listenWeight(FaceRig::ListenKind::Flash,2200)==0 &&
            f.listenWeight(FaceRig::ListenKind::Smile,2200)==0,"a frown must show the frown alone, no flash, no smile");
    // a leak coming while the smile shows silences it
    f.update(4000);
    f.startListen(FaceRig::ListenKind::Smile,4000,4000);
    f.update(4500);
    require(f.listenWeight(FaceRig::ListenKind::Smile,4500)>0.9f,"the smile must show before the leak");
    f.startLeak("afraid",false,4500,4500);
    f.update(4700);
    require(f.listenWeight(4700)==0 && f.weights()[CheekSquint]==0,"a leak must silence the face of the one listening");
  }

  // without the section: no layer
  FaceRig none;
  none.setup(&head,&plain);
  none.startListen(FaceRig::ListenKind::Smile,0,0);
  require(none.update(600)==FaceRig::Result::Rest && none.listenWeight(600)==0,"a profile without listen_face must have no face of listening");

  // rejections
  const auto rejected = [&](const std::string& section, std::string_view reason) {
    try {
      (void)FaceProfile::parse(withListen(section),head);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,
              "the rejection must say why: want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"the profile must be rejected: "+std::string(reason));
    };
  auto with = [](std::string from, const std::string& what, const std::string& to) {
    from.replace(from.find(what),what.size(),to);
    return from;
    };
  const std::string sec = ListenSection;
  rejected(with(sec,"\"about\"","\"x\": 1, \"about\""),"listen_face: unknown field \"x\"");
  rejected(with(sec,"\"cue\": \"frown\"","\"cue\": \"wink\""),"listen_face: frown: no cue \"wink\"");
  rejected(with(sec,"\"weight\": 0.5, \"time\"","\"weight\": 0, \"time\""),"listen_face: flash: weight and time must be above 0");
  rejected(with(sec,"\"chance\": 0.3, \"gap","\"chance\": 2, \"gap"),"listen_face: chance must be a number 0..1");
  rejected(with(sec,"\"gap_ms\": 3000,",""),"listen_face: no gap_ms");
  rejected(with(sec,"\"off_ms\": 400","\"off_ms\": 0"),"listen_face: smile: on_ms and off_ms must be above 0");
  rejected(with(sec,"\"flash\": 0.4}","\"flash\": 0.4, \"y\": 1}"),"listen_face: smile: unknown field \"y\"");
  }

// The face from the voice (speech_face, SpeechFace): a track of ARKit weights played by the time of the voice
std::string base64(const std::vector<uint8_t>& in) {
  static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for(size_t i=0; i<in.size(); i+=3) {
    const uint32_t n = uint32_t(in[i])<<16 | (i+1<in.size() ? uint32_t(in[i+1])<<8 : 0u) | (i+2<in.size() ? uint32_t(in[i+2]) : 0u);
    out += abc[n>>18&63];
    out += abc[n>>12&63];
    out += i+1<in.size() ? abc[n>>6&63] : '=';
    out += i+2<in.size() ? abc[n&63] : '=';
    }
  return out;
  }

const char* SpeechSection = R"(, "speech_face": {"about": "synthetic", "fade_in_ms": 100, "fade_out_ms": 200,
  "cap": {"mouthClose": "jawOpen"}, "scale": {"mouthRoll*": 0.5}, "drop": ["eyeLook*"], "mood_faces": ["S_ANGRY", "S_FRIENDLY"]})";

// the shapes of the test track: jawOpen opens over the second (frame i: i/30), the rest stay
const std::vector<std::string> TrackShapes = {"jawOpen","mouthClose","mouthRollLower","eyeLookUpLeft","eyeBlink","browDown","noSuchShape"};

std::string trackJson(size_t frames, const std::string& emotion = "", std::string data = "") {
  if(data.empty()) {
    std::vector<uint8_t> bytes;
    for(size_t i=0; i<frames; ++i)
      for(auto b:{uint8_t(std::lround(255.0*double(i)/30.0)),uint8_t(153),uint8_t(204),uint8_t(255),uint8_t(51),uint8_t(26),uint8_t(255)})
        bytes.push_back(b);
    data = base64(bytes);
    }
  std::string shapes;
  for(auto& n:TrackShapes)
    shapes += (shapes.empty() ? "\"" : ",\"")+n+"\"";
  return R"({"type":"say_face","npc":"BAU_952_VINO","line":"AIB_7","fps":30,"shapes":[)"+shapes+R"(],"data":")"+data+"\""+
         (emotion.empty() ? std::string() : R"(,"emotion":")"+emotion+"\"")+"}";
  }

void speechFaceCases(const GltfHeadMesh& plainHead) {
  GltfHeadMesh head = plainHead;
  const uint32_t MouthClose = uint32_t(head.targets.size()), MouthRoll = MouthClose+1, EyeLook = MouthClose+2;
  head.targets.push_back({"mouthClose",    {{0,{0,1,0},{}}}});
  head.targets.push_back({"mouthRollLower",{{0,{0,0,-1},{}}}});
  head.targets.push_back({"eyeLookUpLeft", {{2,{0,1,0},{}}}});
  std::string json = Profile;
  json.insert(json.rfind('}'),std::string(BrowsSection)+SpeechSection);
  const auto p = FaceProfile::parse(json,head);
  require(p->speech.has_value() && p->speech->caps.size()==1 && p->speech->scales.size()==1 && p->speech->drop.size()==1 &&
          p->speech->moodFaces.size()==2 && !FaceProfile::parse(Profile,head)->speech.has_value(),
          "speech_face must be read; a profile without it has none");
  const auto track   = SpeechFace::Track::parse(trackJson(31));
  const auto angry   = SpeechFace::Track::parse(trackJson(31,"anger:0.8,disgust:0"));
  require(track->frames==31 && close(track->durationMs(),1000.f) && !track->emotional && angry->emotional &&
          angry->emotion=="anger:0.8,disgust:0","the track must be read: frames, time, its emotion");

  // a voice of 1.5 s named as the line, the track of that line given before it starts
  const auto voiceOf = [](const char* name, uint64_t ms) {
    auto v = voice(ms,{{0,ms}});
    v->setName(name);
    return v;
    };
  FaceRig    rig;
  SpeechFace sf;
  rig.setup(&head,p.get());
  sf.setTrack(track,"AIB_7");
  require(sf.play(rig,0)==0.f && !sf.playing(),"no voice, no face from it");
  sf.start(voiceOf("AIB_7",1500),"",1000);
  const auto at = [&](uint64_t now) {
    sf.play(rig,now);
    rig.update(now);
    return rig.weights();
    };
  // the rules: mouthClose under jawOpen, mouthRoll* halved, eyeLook* dropped, a shape the head lacks left out
  auto w = at(1100);
  require(close(rig.voiceFace(),1.f) && close(w[MouthClose],w[JawOpen]) && w[MouthClose]<0.15f,
          "mouthClose must stay under jawOpen: "+std::to_string(w[MouthClose])+" over "+std::to_string(w[JawOpen]));
  require(close(w[MouthRoll],0.4f) && w[EyeLook]==0.f,"mouthRoll* must be halved, eyeLook* dropped: "+
          std::to_string(w[MouthRoll])+" "+std::to_string(w[EyeLook]));
  // the lids: the track's eyeBlink is their level
  require(close(w[Target::EyeBlink],0.2f),"the track's eyeBlink must stay as the level of the lids: "+std::to_string(w[Target::EyeBlink]));
  // the frame by the time of the voice, linear between frames: 500 ms is frame 15, 517 ms 15.51
  w = at(1500);
  require(close(w[JawOpen],128.f/255.f) && close(rig.voiceFace(),1.f),"500 ms into the voice must show frame 15: "+
          std::to_string(w[JawOpen]));
  w = at(1517);
  require(close(w[JawOpen],(128.f*0.49f+136.f*0.51f)/255.f),"517 ms must lie between frames 15 and 16: "+std::to_string(w[JawOpen]));
  // fading in from the start of the voice
  {
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    s2.setTrack(track,"AIB_7");
    s2.start(voiceOf("AIB_7",1500),"",0);
    require(close(s2.play(r2,50),0.5f) && close(s2.play(r2,100),1.f),"the face must come in over fade_in_ms");
  }
  // a blink goes over the level of the lids, the sum at most 1
  rig.startAnim("R_EYESBLINK",0.5f,1800,1600);
  w = at(1660);
  require(close(w[Target::EyeBlink],0.7f),"a half blink must go over the level of the lids: "+std::to_string(w[Target::EyeBlink]));
  rig.startAnim("R_EYESBLINK",1.f,1900,1660);
  w = at(1710);
  require(close(w[Target::EyeBlink],1.f),"a blink over the level must reach 1 and no more");

  // VISEME and the brows of the voice are silent under the track
  rig.startBrows(false,1,1720,1720);
  for(uint64_t now=1740; now<=1820; now+=20) {
    rig.setShape("VISEME",2,2,0,1,now);
    w = at(now);
    }
  require(w[VisemeAa]==0.f && w[BrowUpLeft]==0.f && rig.browWeight(1820)==0.f,
          "VISEME and the brows of the voice must be silent under the track: "+std::to_string(w[VisemeAa])+" "+
          std::to_string(w[BrowUpLeft]));
  // the track ends at 1 s, the voice goes on: the face fades over fade_out_ms holding its last frame, VISEME comes back
  for(uint64_t now=1840; now<=2100; now+=20) {
    rig.setShape("VISEME",2,2,0,1,now);
    w = at(now);
    if(now==2100)
      require(rig.voiceFace()>0.2f && rig.voiceFace()<0.8f && w[VisemeAa]>0.2f && w[VisemeAa]<0.8f &&
              close(w[JawOpen]/rig.voiceFace(),1.f,0.02f),
              "past its end the track must fade holding its last frame as VISEME comes back: "+
              std::to_string(rig.voiceFace())+" "+std::to_string(w[VisemeAa]));
    }
  for(uint64_t now=2120; now<=2300; now+=20) {
    rig.setShape("VISEME",2,2,0,1,now);
    w = at(now);
    }
  require(!sf.playing() && rig.voiceFace()==0.f && close(w[VisemeAa],1.f),"after its fade the mouth must be the voice's again");
  // a voice shorter than its track: the face holds the frame at the end of the sound and fades
  {
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    s2.setTrack(track,"AIB_7");
    s2.start(voiceOf("AIB_7",600),"",0);
    s2.play(r2,700);
    const float k = s2.play(r2,800);
    r2.update(800);
    require(k>0.f && k<1.f && close(r2.weights()[JawOpen]/k,0.6f,0.01f),
            "the face must end with the sound, holding the frame there: "+std::to_string(k));
  }

  // the moods: S_* gives way to a track made with an emotion, shows over one without; S_EXTRA is not a mood face
  for(bool emo:{false,true}) {
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    r2.startAnim("S_ANGRY",1,uint64_t(-1),0);
    r2.startAnim("S_EXTRA",1,uint64_t(-1),0);
    s2.start(voiceOf("AIB_7",1500),"",1000);
    s2.setTrack(emo ? angry : track,"AIB_7");  // after its voice started: it plays all the same
    s2.play(r2,1500);
    r2.update(1500);
    const auto& ww = r2.weights();
    // browDown: the track 0.1, S_ANGRY 0.8, S_EXTRA 0.6
    require(emo ? close(ww[BrowDown],0.7f,0.01f) && close(r2.weight("S_ANGRY",1500),0.f) && close(r2.weight("S_EXTRA",1500),1.f)
                : close(ww[BrowDown],1.f) && close(r2.weight("S_ANGRY",1500),1.f),
            std::string(emo ? "S_ANGRY must give way to a track with an emotion: " : "S_ANGRY must show over a neutral track: ")+
            std::to_string(ww[BrowDown]));
  }
  // the order: a track for another line waits for its voice; a sound file of the line ("<id>.WAV") plays it
  {
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    s2.setTrack(track,"AIB_9");
    s2.start(voiceOf("AIB_8",1500),"",0);
    require(s2.play(r2,500)==0.f,"a track must not play on the voice of another line");
    s2.start(voiceOf("AIB_9.WAV",1500),"",600);
    require(s2.play(r2,1100)==1.f && s2.current()==track.get(),"the voice of its line must play the track waiting for it");
    s2.stop(1200);
    require(s2.play(r2,1300)<1.f && s2.play(r2,1450)==0.f && !s2.playing(),"a line cut off must take its face over fade_out_ms");
  }
  // without the section the head is as before; switched off, too
  {
    const auto plain = FaceProfile::parse(Profile,head);
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,plain.get());
    s2.setTrack(track,"AIB_7");
    s2.start(voiceOf("AIB_7",1500),"",0);
    r2.setShape("VISEME",2,2,0,1,500);
    require(s2.play(r2,500)==0.f && r2.update(500)!=FaceRig::Result::Same && close(r2.weights()[VisemeAa],1.f) &&
            r2.weights()[MouthClose]==0.f,"a profile without speech_face must show no track");
    FaceRig r3; SpeechFace s3;
    r3.setup(&head,p.get());
    SpeechFace::setEnabled(false);
    s3.setTrack(track,"AIB_7");
    s3.start(voiceOf("AIB_7",1500),"",0);
    const float off = s3.play(r3,500);
    SpeechFace::setEnabled(true);
    require(off==0.f && r3.voiceFace()==0.f,"OPENGOTHIC_SPEECH_FACE=0 must leave the face as without a track");
  }
  // a track of a file next to the head for a voice of the game, the name's case aside
  {
    const auto dir = std::filesystem::temp_directory_path()/"facerig-test-tracks";
    std::filesystem::create_directories(dir);
    std::ofstream(dir/"dia_test_01.face.json") << trackJson(31);
    std::ofstream(dir/"DIA_TEST_02.face.json") << "{\"fps\":30}";
    require(SpeechFace::fromFile("DIA_Test_01.WAV",dir.string())!=nullptr && SpeechFace::fromFile("DIA_TEST_03.WAV",dir.string())==nullptr &&
            SpeechFace::fromFile("DIA_TEST_02.WAV",dir.string())==nullptr,"a voice of the game must find its track by name, a broken one none");
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    s2.start(voiceOf("DIA_TEST_01.WAV",1500),dir.string(),0);
    require(s2.play(r2,500)==1.f,"a voice of the game must play the track of its file");
    std::filesystem::remove_all(dir);
  }
  // a streamed voice: the frame goes by what the audio thread has played, not by the time since it started
  {
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    auto v = voiceOf("AIB_7",1500);
    s2.setTrack(track,"AIB_7");
    s2.start(v,"",0);
    v->setPlayed(500);
    s2.play(r2,1000);
    r2.update(1000);
    require(close(r2.weights()[JawOpen],128.f/255.f),"a streamed voice must show the frame of the time it has played: "+
            std::to_string(r2.weights()[JawOpen]));
  }
  // a stream still arriving: shorter than the track so far, the face goes on and does not fade
  {
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    auto v = voice(600,{{0,600}},false);
    v->setName("AIB_7");
    s2.setTrack(track,"AIB_7");
    s2.start(v,"",0);
    s2.play(r2,700);
    const float k = s2.play(r2,900);
    r2.update(900);
    require(close(k,1.f) && close(r2.weights()[JawOpen],230.f/255.f),"a voice still arriving must not end the track at "
            "what has come: "+std::to_string(k)+" "+std::to_string(r2.weights()[JawOpen]));
  }
  // a label with no strength is no emotion: the mood faces stay
  {
    const auto none = SpeechFace::Track::parse(trackJson(31,"anger:0"));
    FaceRig r2; SpeechFace s2;
    r2.setup(&head,p.get());
    r2.startAnim("S_ANGRY",1,uint64_t(-1),0);
    s2.start(voiceOf("AIB_7",1500),"",1000);
    s2.setTrack(none,"AIB_7");
    s2.play(r2,1500);
    r2.update(1500);
    require(!none->emotional && close(r2.weight("S_ANGRY",1500),1.f),"anger:0 must be no emotion, S_ANGRY must stay");
  }
  // a track from the directory of OPENGOTHIC_FACE_TRACKS, the head's directory aside
  {
    const auto dir = std::filesystem::temp_directory_path()/"facerig-test-env-tracks";
    std::filesystem::create_directories(dir);
    std::ofstream(dir/"DIA_TEST_04.face.json") << trackJson(31);
    const char*       was = std::getenv("OPENGOTHIC_FACE_TRACKS");
    const std::string old = was!=nullptr ? was : "";
    setenv("OPENGOTHIC_FACE_TRACKS",dir.string().c_str(),1);
    const auto found = SpeechFace::fromFile("DIA_TEST_04.WAV","");
    if(was!=nullptr)
      setenv("OPENGOTHIC_FACE_TRACKS",old.c_str(),1); else
      unsetenv("OPENGOTHIC_FACE_TRACKS");
    std::filesystem::remove_all(dir);
    require(found!=nullptr,"a track in OPENGOTHIC_FACE_TRACKS must be found");
  }
  // rejections: the track, its rules, the bridge's say_face
  const auto rejected = [&](const std::string& j, std::string_view reason) {
    try {
      (void)SpeechFace::Track::parse(j);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,"want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"the track must be rejected: "+std::string(reason));
    };
  rejected(trackJson(31,"","AAA!"),"data: not base64");
  rejected(trackJson(31,"",base64({1,2,3,4,5,6,7,8})),"not whole frames of 7 shapes");
  rejected(trackJson(31,"rage:0.5"),"unknown emotion \"rage\"");
  rejected(trackJson(31,"anger:2"),"must be a number 0..1");
  rejected(R"({"fps":0,"shapes":["a"],"data":"AA=="})","fps must be");
  rejected(R"({"fps":30,"shapes":["a","a"],"data":"AAA="})","listed twice");
  const auto rules = [&](const std::string& section, std::string_view reason) {
    std::string j = Profile;
    j.insert(j.rfind('}'),", \"speech_face\": "+section);
    try {
      (void)FaceProfile::parse(j,head);
      }
    catch(const std::exception& e) {
      require(std::string_view(e.what()).find(reason)!=std::string_view::npos,"want '"+std::string(reason)+"', got '"+e.what()+"'");
      return;
      }
    require(false,"speech_face must be rejected: "+std::string(reason));
    };
  rules(R"({"fade_in_ms":100,"fade_out_ms":200,"extra":1})","speech_face: unknown field \"extra\"");
  rules(R"({"fade_in_ms":100})","fade_out_ms must be");
  rules(R"({"fade_in_ms":100,"fade_out_ms":200,"mood_faces":["S_NONE"]})","mood_faces: no anim \"S_NONE\"");
  rules(R"({"fade_in_ms":100,"fade_out_ms":200,"scale":{"*":0.5}})","is not a shape or a prefix");
  rules(R"({"fade_in_ms":100,"fade_out_ms":200,"cap":{"mouthClose":1}})","cap: \"mouthClose\" must name");
  rapidjson::Document doc;
  doc.Parse(trackJson(31,"","AAA!").c_str());
  require(LineCues::readFace(doc)==nullptr,"a broken say_face must leave the line without a track");
  doc.Parse(R"({"type":"say_face","npc":"BAU_952_VINO","fps":30,"shapes":["jawOpen"],"data":"AA=="})");
  require(LineCues::readFace(doc)==nullptr,"a say_face without its line must leave no track");
  doc.Parse(trackJson(31,"anger:0.8").c_str());
  const auto read = LineCues::readFace(doc);
  require(read!=nullptr && read->frames==31 && read->emotional,"say_face must be read by the bridge");

  // the price on the CPU: a frame of the track into the face (the blend itself is FaceRig's, as before)
  FaceRig r2; SpeechFace s2;
  r2.setup(&head,p.get());
  s2.setTrack(track,"AIB_7");
  s2.start(voiceOf("AIB_7",1500),"",0);
  const auto t0 = std::chrono::steady_clock::now();
  for(uint64_t now=0; now<1000; ++now)
    s2.play(r2,now);
  const double us = std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/1000.0;
  std::cout << "speech face: " << us << " us a frame to sample a track of " << track->shapes.size() << " shapes\n";
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
    toneCases();
    playerVoiceCases();
    browCases(head,*profile);
    listenCases(head,*profile);
    speechFaceCases(head);
    const auto eyeHead    = makeEyeHead();
    const auto eyeProfile = FaceProfile::parse(EyeProfile,eyeHead);
    eyeProfileCases(eyeHead,*eyeProfile);
    eyeRigCases(eyeHead,*eyeProfile);
    eyeMotionCases(*eyeProfile);
    eyeCueCases(eyeHead);
    lidShadeCases();
    }
  catch(const std::exception& e) {
    std::cerr << "facerig test: FAIL: " << e.what() << " (after " << assertions << " checks)\n";
    return 1;
    }
  std::cout << "facerig test: " << assertions << " checks passed\n";
  return 0;
  }
