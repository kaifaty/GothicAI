// The render skeleton of our own under a Gothic one (graphics/mesh/renderrig): synthetic source and rig skeletons
// with turned node frames, render-rigs.json from a string. A rig equal to its source repeats it in random poses; the
// legs' IK puts the foot on the target with the knee towards the pole and the bone lengths kept, and stretches the
// leg towards a target out of reach; the fingers' bend is spread by the shares; a socket rides its bone; each broken
// rig is left out with its reason, the others load. CPU only: no device, no game data.
#include "graphics/mesh/renderrig.h"
#include "graphics/mesh/skeleton.h"
#include "resources.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Tempest;

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

bool contains(const std::string& s, const std::string& part) {
  return s.find(part)!=std::string::npos;
  }

Vec3 position(const Matrix4x4& m) {
  return {m.at(3,0), m.at(3,1), m.at(3,2)};
  }

double distance(const Vec3& a, const Vec3& b) {
  const double x = double(a.x)-b.x, y = double(a.y)-b.y, z = double(a.z)-b.z;
  return std::sqrt(x*x + y*y + z*z);
  }

double length(const Vec3& a) {
  return distance(a,Vec3());
  }

// degrees between two directions
double angle(const Vec3& a, const Vec3& b) {
  const double c = (double(a.x)*b.x + double(a.y)*b.y + double(a.z)*b.z)/(length(a)*length(b));
  return std::acos(std::max(-1.0,std::min(1.0,c)))*180.0/M_PI;
  }

// degrees of the turn from a to b (their linear parts)
double turnAngle(const Matrix4x4& a, const Matrix4x4& b) {
  double trace = 0;
  for(int k=0; k<3; ++k)
    for(int r=0; r<3; ++r)
      trace += double(a.at(k,r))*b.at(k,r);  // trace of a^T b
  return std::acos(std::max(-1.0,std::min(1.0,(trace-1.0)/2.0)))*180.0/M_PI;
  }

Matrix4x4 linearOnly(Matrix4x4 m) {
  m.set(3,0,0);
  m.set(3,1,0);
  m.set(3,2,0);
  return m;
  }

// parent^-1 * child of the linear parts: the turn of a child against its parent
Matrix4x4 relative(const Matrix4x4& parent, const Matrix4x4& child) {
  Matrix4x4 p = linearOnly(parent);
  p.transpose();
  return p*linearOnly(child);
  }

double maxDiff(const Matrix4x4& a, const Matrix4x4& b) {
  double d = 0;
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      d = std::max(d,std::abs(double(a.at(c,r))-b.at(c,r)));
  return d;
  }

bool finite(const Matrix4x4& m) {
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      if(!std::isfinite(m.at(c,r)))
        return false;
  return true;
  }

Vec3 apply(const Matrix4x4& m, const Vec3& p) {
  Vec3 v = p;
  m.project(v);
  return v;
  }

Matrix4x4 inverse(Matrix4x4 m) {
  m.inverse();
  return m;
  }

Matrix4x4 turn(const Vec3& axis, float degrees) {
  Matrix4x4 m;
  m.identity();
  m.rotate(degrees,axis.x,axis.y,axis.z);
  return m;
  }

Matrix4x4 placed(const Vec3& at, const Matrix4x4& frame) {
  Matrix4x4 m = frame;
  m.set(3,0,at.x);
  m.set(3,1,at.y);
  m.set(3,2,at.z);
  return m;
  }

std::mt19937 rng(11);

float uniform(float a, float b) {
  return std::uniform_real_distribution<float>(a,b)(rng);
  }

Vec3 randomAxis() {
  for(;;) {
    const Vec3 v(uniform(-1,1),uniform(-1,1),uniform(-1,1));
    const double l = length(v);
    if(l>0.2 && l<=1.0)
      return v*float(1.0/l);
    }
  }

// A skeleton given in world terms: each node's bind place and a turned frame; parents before children.
struct Joint final {
  std::string name;
  int         parent;
  Vec3        at;
  };

struct Skel final {
  std::vector<Joint>     joints;
  std::vector<Matrix4x4> world, local;

  explicit Skel(std::vector<Joint> j) : joints(std::move(j)) {
    for(size_t i=0; i<joints.size(); ++i) {
      world.push_back(placed(joints[i].at,turn(randomAxis(),uniform(-180,180))));
      local.push_back(joints[i].parent<0 ? world[i] : inverse(world[size_t(joints[i].parent)])*world[i]);
      }
    }

  size_t find(const std::string& name) const {
    for(size_t i=0; i<joints.size(); ++i)
      if(joints[i].name==name)
        return i;
    throw std::runtime_error("no joint "+name);
    }

  asset::ModelHierarchy hierarchy() const {
    asset::ModelHierarchy h {};
    for(size_t i=0; i<joints.size(); ++i) {
      asset::ModelHierarchyNode n {};
      n.parent_index = int16_t(joints[i].parent);
      n.name         = joints[i].name;
      std::memcpy(static_cast<void*>(&n.transform),static_cast<const void*>(&local[i]),sizeof(n.transform));
      h.nodes.push_back(n);
      }
    return h;
    }

  // world matrices of a pose: each node turned in its own frame (the bone lengths kept), the whole under `object`
  std::vector<Matrix4x4> pose(const std::vector<Matrix4x4>& turns, const Matrix4x4& object) const {
    std::vector<Matrix4x4> w(joints.size());
    for(size_t i=0; i<joints.size(); ++i) {
      const Matrix4x4 l = local[i]*turns[i];
      w[i] = joints[i].parent<0 ? object*l : w[size_t(joints[i].parent)]*l;
      }
    return w;
    }

  std::vector<Matrix4x4> randomPose(float maxDegrees, Matrix4x4 object) const {
    std::vector<Matrix4x4> turns;
    for(size_t i=0; i<joints.size(); ++i)
      turns.push_back(turn(randomAxis(),uniform(-maxDegrees,maxDegrees)));
    return pose(turns,object);
    }

  std::vector<Matrix4x4> bindTurns() const {
    return std::vector<Matrix4x4>(joints.size(),Matrix4x4::mkIdentity());
    }
  };

Matrix4x4 randomObject() {
  return placed(Vec3(uniform(-50,50),uniform(-10,10),uniform(-50,50)),turn(randomAxis(),uniform(-180,180)));
  }

std::string floats(const Matrix4x4& m) {
  std::string s = "[";
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      s += (s.size()>1 ? "," : "")+std::to_string(m.at(c,r));
  return s+"]";
  }

std::string vec(const Vec3& v) {
  return "["+std::to_string(v.x)+","+std::to_string(v.y)+","+std::to_string(v.z)+"]";
  }
}

int main() {
  try {
    // Gothic's side: a hip, a leg, an arm with one finger and a socket (cm, y up, z ahead)
    const Skel source({
      {"BIP01",           -1, {0, 90, 0}},
      {"BIP01 PELVIS",     0, {0, 90, 0}},
      {"BIP01 SPINE",      1, {0, 102, 0}},
      {"BIP01 L THIGH",    1, {10, 85, 0}},
      {"BIP01 L CALF",     3, {10, 42, 2}},
      {"BIP01 L FOOT",     4, {10, 5, 0}},
      {"BIP01 L TOE0",     5, {10, 0, 12}},
      {"BIP01 L UPPERARM", 2, {18, 140, 0}},
      {"BIP01 L HAND",     7, {18, 88, 0}},
      {"BIP01 L FINGER0",  8, {18, 80, 0}},
      {"ZS_LEFTHAND",      8, {18, 84, 2}},
      });
    // (a) the same skeleton as the source
    const Skel& same = source;
    // (b, c) our leg: the thigh longer, the knee lower than Gothic's
    const Skel leg({
      {"root",    -1, {0, 0, 0}},
      {"pelvis",   0, {0, 92, 0}},
      {"thigh_l",  1, {9, 88, 1}},
      {"calf_l",   2, {9, 46, 3}},
      {"foot_l",   3, {9, 8, -1}},
      {"ball_l",   4, {9, 1, 12}},
      });
    // (d) a hand with two fingers of three phalanges
    const Skel hand({
      {"hand_l",      -1, {18, 88, 0}},
      {"index_01_l",   0, {19, 81, 1}},
      {"index_02_l",   1, {19, 77, 1.5f}},
      {"index_03_l",   2, {19, 74.5f, 1.8f}},
      {"middle_01_l",  0, {17, 81, 1}},
      {"middle_02_l",  4, {17, 76.5f, 1.4f}},
      {"middle_03_l",  5, {17, 73.8f, 1.6f}},
      });
    std::vector<Joint> many;
    for(int i=0; i<int(Resources::MAX_NUM_SKELETAL_NODES)+1; ++i)
      many.push_back({"node"+std::to_string(i),i-1,{0,float(i),0}});
    const Skel big(many);

    std::map<std::string,asset::ModelHierarchy> records = {
      {"SRC.MDH",  source.hierarchy()},
      {"SAME.MDH", same.hierarchy()},
      {"LEG.MDH",  leg.hierarchy()},
      {"HAND.MDH", hand.hierarchy()},
      {"BIG.MDH",  big.hierarchy()},
      };
    std::vector<std::string> messages;
    RenderRigIndex index([&](std::string_view name, asset::ModelHierarchy& out) {
                           const auto it = records.find(std::string(name));
                           if(it==records.end())
                             return false;
                           out = it->second;
                           return true;
                           },
                         [&](const std::string& m) { messages.push_back(m); });

    // the ankle: 2 cm under and 4 cm ahead of Gothic's foot node, in its frame
    const size_t sFoot = source.find("BIP01 L FOOT");
    const Vec3   ankle = apply(inverse(source.world[sFoot]),position(source.world[sFoot])+Vec3(0,-2,4));
    const Matrix4x4 offset = placed(Vec3(1,-2,3),turn(Vec3(0.3f,0.9f,0.1f),35));

    std::string sameBones;
    for(const auto& j:same.joints)
      sameBones += std::string(sameBones.empty() ? "" : ",")+"{\"name\": \""+j.name+"\", \"source\": \""+j.name+"\", \"align\": [1,0,0,0]}";
    const std::string json = R"({
      "format": "gothic-render-rigs", "version": 1,
      "rigs": {
        "SAME": {"skeleton": "SAME.MDH", "source": "SRC.MDH", "bones": [)"+sameBones+R"(],
                 "root": {"bone": "BIP01 PELVIS", "source": "BIP01 PELVIS"},
                 "sockets": {"ZS_LEFTHAND": {"bone": "BIP01 L HAND", "offset": )"+floats(offset)+R"(}}},
        "LEG": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
                "bones": [{"name": "root"}, {"name": "pelvis", "source": "BIP01 PELVIS"},
                          {"name": "thigh_l", "source": "BIP01 L THIGH"}, {"name": "calf_l", "source": "BIP01 L CALF"},
                          {"name": "foot_l", "source": "BIP01 L FOOT"}, {"name": "ball_l", "source": "BIP01 L TOE0"}],
                "root": {"bone": "pelvis", "source": "BIP01 PELVIS"},
                "legs": [{"thigh": "thigh_l", "calf": "calf_l", "foot": "foot_l", "ball": "ball_l",
                          "source_foot": "BIP01 L FOOT", "source_knee": "BIP01 L CALF", "ankle": )"+vec(ankle)+R"(}]},
        "HAND": {"skeleton": "HAND.MDH", "source": "SRC.MDH",
                 "bones": [{"name": "hand_l", "source": "BIP01 L HAND"}],
                 "root": {"bone": "hand_l", "source": "BIP01 L HAND"},
                 "fingers": [{"source_hand": "BIP01 L HAND", "source_finger": "BIP01 L FINGER0",
                              "chains": [["index_01_l", "index_02_l", "index_03_l"], ["middle_01_l", "middle_02_l", "middle_03_l"]],
                              "shares": [1.0, 0.9, 0.7]}]},
        "BAD_BONE":   {"skeleton": "LEG.MDH", "source": "SRC.MDH", "bones": [{"name": "knee_l"}],
                       "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}},
        "BAD_SOURCE": {"skeleton": "LEG.MDH", "source": "SRC.MDH", "bones": [{"name": "pelvis", "source": "BIP01 R THIGH"}],
                       "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}},
        "BAD_BIG":    {"skeleton": "BIG.MDH", "source": "SRC.MDH", "bones": [{"name": "node0"}],
                       "root": {"bone": "node0", "source": "BIP01"}},
        "BAD_RECORD": {"skeleton": "NONE.MDH", "source": "SRC.MDH", "bones": [{"name": "pelvis"}],
                       "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}}
        },
      "meshes": {"HUM_BODY_TEST.MDM": "LEG"}
      })";
    index.parse(json);
    require(messages.empty(),"a good file is read without messages: "+(messages.empty() ? "" : messages[0]));
    require(index.size()==7,"seven rigs are read");

    // (a) the same skeleton repeats the source
    const RenderRig* sameRig = index.rig("SAME");
    require(sameRig!=nullptr && sameRig->bones()==same.joints.size(),"the rig equal to the source loads");
    require(sameRig->fits(Skeleton(source.hierarchy(),nullptr,"SRC.MDH")),"the rig fits its source skeleton");
    require(!sameRig->fits(Skeleton(leg.hierarchy(),nullptr,"LEG.MDH")),"the rig does not fit another skeleton");
    std::vector<Matrix4x4> out(Resources::MAX_NUM_SKELETAL_NODES);
    require(!sameRig->solve(source.world.data(),source.world.size()-1,out.data()),"too few source matrices are refused");
    double worst = 0;
    for(int n=0; n<200; ++n) {
      const auto w = source.randomPose(180,randomObject());
      require(sameRig->solve(w.data(),w.size(),out.data()),"the pose is solved");
      for(size_t i=0; i<w.size(); ++i)
        worst = std::max(worst,maxDiff(out[i],w[i]));
      }
    require(worst<=1e-4,"the same skeleton repeats the source within 1e-4: "+std::to_string(worst));

    // (e) a socket rides its bone: M_bone(t) * offset
    {
      const auto w = source.randomPose(90,randomObject());
      sameRig->solve(w.data(),w.size(),out.data());
      Matrix4x4 m, k;
      const size_t handNode = sameRig->skeleton().findNode("BIP01 L HAND");
      require(sameRig->socket("ZS_LEFTHAND",out.data(),m) && maxDiff(m,out[handNode]*offset)<=1e-5,
              "the socket is M_bone * offset");
      require(sameRig->socket(source.find("ZS_LEFTHAND"),out.data(),k) && maxDiff(k,m)==0,
              "the socket is found by the source node's index");
      require(!sameRig->socket("ZS_RIGHTHAND",out.data(),m) && !sameRig->socket(size_t(0),out.data(),m),
              "a node without a socket has none");
    }

    // (b, c) the leg: IK onto the ankle, out of reach - straight
    const RenderRig* legRig = index.rig("LEG");
    require(legRig!=nullptr && index.rigOfMesh("models/hum_body_test.asc")==legRig,"the leg rig loads, by its mesh too");
    require(index.meshTable().size()==1 && index.meshTable().count("HUM_BODY_TEST")==1 && index.rigNames().size()==7,
            "the table lists its rigs and meshes");
    // a bone's Gothic node: its source, else its parent's, else the source's root
    require(legRig->sourceOf(leg.find("ball_l"))==source.find("BIP01 L TOE0") &&
            legRig->sourceOf(leg.find("pelvis"))==source.find("BIP01 PELVIS") &&
            legRig->sourceOf(leg.find("root"))==source.find("BIP01") && legRig->sourceOf(99)==size_t(-1),
            "a leg bone belongs to its source node");
    const size_t th = leg.find("thigh_l"), ca = leg.find("calf_l"), fo = leg.find("foot_l"), ba = leg.find("ball_l");
    const double L1 = distance(leg.joints[ca].at,leg.joints[th].at), L2 = distance(leg.joints[fo].at,leg.joints[ca].at);
    const size_t sCalf = source.find("BIP01 L CALF");
    // the foot's own turn: R_source(t) * R_source_bind^T * R_bind (align 1)
    const Matrix4x4 footTurn = relative(source.world[sFoot],leg.world[fo]);
    int reached = 0, stretched = 0, poled = 0;
    for(int n=0; n<400; ++n) {
      auto w = n==0 ? source.pose(source.bindTurns(),Matrix4x4::mkIdentity()) : source.randomPose(70,randomObject());
      require(legRig->solve(w.data(),w.size(),out.data()),"the leg pose is solved");
      for(size_t i=0; i<leg.joints.size(); ++i)
        require(finite(out[i]),"no NaN in the leg, pose "+std::to_string(n));
      const Vec3 H = position(out[th]), K = position(out[ca]), F = position(out[fo]);
      const Vec3 T = apply(w[sFoot],ankle);
      const double reach = distance(T,H);
      const std::string at = ", pose "+std::to_string(n);
      require(std::abs(distance(K,H)-L1)<=1e-3 && std::abs(distance(F,K)-L2)<=1e-3,
              "the thigh and the calf keep their lengths"+at+": "+std::to_string(distance(K,H))+" "+std::to_string(distance(F,K)));
      // the bones point at the solved joints: the skin bends with them
      require(distance(apply(out[th],position(leg.local[ca])),K)<=1e-3 && distance(apply(out[ca],position(leg.local[fo])),F)<=1e-3,
              "the thigh and the calf turn onto the knee and the ankle"+at);
      require(maxDiff(linearOnly(out[fo]),linearOnly(w[sFoot])*footTurn)<=1e-4,"the foot keeps its turn"+at);
      require(distance(position(out[ba]),apply(out[fo],position(leg.local[ba])))<=1e-3,"the ball follows the foot"+at);
      if(reach<L1+L2-0.5 && reach>std::abs(L1-L2)+0.5) {
        ++reached;
        require(distance(F,T)<=0.01,"the foot stands on the target within 0.01 cm"+at+": "+std::to_string(distance(F,T)));
        const Vec3 u = (T-H)*float(1.0/reach), toPole = position(w[sCalf])-H;
        const float along = u.x*toPole.x + u.y*toPole.y + u.z*toPole.z;
        const Vec3 side = toPole-u*along, knee = K-H;
        if(length(side)>1.0) {
          ++poled;
          require(side.x*knee.x + side.y*knee.y + side.z*knee.z>0,"the knee lies on the pole's side"+at);
          }
        }
      else if(reach>L1+L2) {
        ++stretched;
        require(angle(K-H,F-K)<=0.05,"out of reach the leg is straight"+at+": "+std::to_string(angle(K-H,F-K)));
        require(angle(F-H,T-H)<=0.05,"out of reach the leg points at the target"+at);
        }
      }
    require(reached>=50 && poled>=50 && stretched>=20,"the poses cover reach and stretch: "+std::to_string(reached)+" "+
            std::to_string(poled)+" "+std::to_string(stretched));
    require(distance(apply(source.world[sFoot],ankle),position(leg.world[th]))>L1+L2,"the bind pose is out of reach");

    // (d) the finger's bend over the phalanges: 60 degrees of FINGER0 against the hand -> 60, 54, 42
    const RenderRig* handRig = index.rig("HAND");
    require(handRig!=nullptr,"the hand rig loads");
    require(handRig->sourceOf(hand.find("middle_03_l"))==source.find("BIP01 L HAND"),
            "a phalanx without a source belongs to the hand's");
    for(int n=0; n<20; ++n) {
      auto turns = source.bindTurns();
      turns[source.find("BIP01 L HAND")]    = turn(randomAxis(),uniform(-90,90));
      turns[source.find("BIP01 L FINGER0")] = turn(randomAxis(),60);
      const auto w = source.pose(turns,randomObject());
      require(handRig->solve(w.data(),w.size(),out.data()),"the hand pose is solved");
      const double want[3] = {60, 54, 42};
      for(const char* f:{"index","middle"})
        for(size_t j=0; j<3; ++j) {
          const size_t c = hand.find(std::string(f)+"_0"+std::to_string(j+1)+"_l");
          const size_t p = size_t(hand.joints[c].parent);
          const double got = turnAngle(relative(out[p],out[c]),relative(hand.world[p],hand.world[c]));
          require(std::abs(got-want[j])<=0.01,std::string(f)+" phalanx "+std::to_string(j+1)+" turns "+
                  std::to_string(got)+" against its parent, not "+std::to_string(want[j]));
          }
      }

    // (f) each broken rig is left out with its reason, once
    const std::map<std::string,std::string> broken = {
      {"BAD_BONE",   "bone \"knee_l\" is not in skeleton \"LEG.MDH\""},
      {"BAD_SOURCE", "source \"BIP01 R THIGH\" is not in skeleton \"SRC.MDH\""},
      {"BAD_BIG",    "97 bones in \"BIG.MDH\", more than 96"},
      {"BAD_RECORD", "no skeleton record \"NONE.MDH\""},
      };
    for(const auto& [rig,why]:broken) {
      messages.clear();
      require(index.rig(rig)==nullptr && index.rig(rig)==nullptr,rig+" is refused");
      require(messages.size()==1 && contains(messages[0],"render rig "+rig+": ") && contains(messages[0],why),
              rig+" gives one message with its reason: "+(messages.empty() ? "none" : messages[0]));
      }
    require(index.rig("LEG")==legRig,"the good rigs stay");

    const std::map<std::string,std::string> files = {
      {R"({"format": "gothic-models", "version": 1, "rigs": {}})",          "render rigs: format must be \"gothic-render-rigs\""},
      {R"({"format": "gothic-render-rigs", "version": 2, "rigs": {}})",     "render rigs: version must be 1"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {)",       "render rigs: invalid JSON"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis", "turn": [1,0,0,0]}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}}}})",
                                                                            "render rig R: bones[0]: unknown field \"turn\""},
      };
    for(const auto& [text,why]:files) {
      messages.clear();
      index.parse(text);
      require(messages.size()==1 && contains(messages[0],why),"a broken file is refused: "+why+" / "+
              (messages.empty() ? "none" : messages[0]));
      require(index.size()==0 && index.rig("LEG")==nullptr,"a broken file loads no rig: "+why);
      }

    // without the file - as before
    messages.clear();
    index.read(std::filesystem::temp_directory_path()/"gothic-render-rig-test-none"/"render-rigs.json");
    require(messages.empty() && index.size()==0 && index.rig("LEG")==nullptr,"no file - no rigs and no messages");

    std::cout << "render rig test: " << assertions << " assertions passed (worst repeat " << worst << ", "
              << reached << " legs reached, " << stretched << " stretched)\n";
    return 0;
    }
  catch(const std::exception& e) {
    std::cerr << "render rig test failed: " << e.what() << "\n";
    return 1;
    }
  }
