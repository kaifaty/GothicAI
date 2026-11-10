// The render skeleton of our own under a Gothic one (graphics/mesh/renderrig): synthetic source and rig skeletons
// with turned node frames, render-rigs.json from a string. A rig equal to its source repeats it in random poses; the
// legs' IK puts the foot on the target with the knee towards the pole and the bone lengths kept, and stretches the
// leg towards a target out of reach; the fingers' bend is spread by the shares; a socket rides its bone; each broken
// rig is left out with its reason, the others load. CPU only: no device, no game data.
#include "graphics/mesh/renderrig.h"
#include "graphics/mesh/skeleton.h"
#include "resources.h"

#include <algorithm>
#include <array>
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

constexpr double Pi = 3.14159265358979323846;
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
  return std::acos(std::max(-1.0,std::min(1.0,c)))*180.0/Pi;
  }

// degrees of the turn from a to b (their linear parts)
double turnAngle(const Matrix4x4& a, const Matrix4x4& b) {
  double trace = 0;
  for(int k=0; k<3; ++k)
    for(int r=0; r<3; ++r)
      trace += double(a.at(k,r))*b.at(k,r);  // trace of a^T b
  return std::acos(std::max(-1.0,std::min(1.0,(trace-1.0)/2.0)))*180.0/Pi;
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

// node i and all below it turned about the node's place by `degrees` around a world axis
void bend(const Skel& s, std::vector<Matrix4x4>& w, size_t i, const Vec3& axis, float degrees) {
  const Vec3 at = position(w[i]);
  const Matrix4x4 r = placed(at,Matrix4x4::mkIdentity())*turn(axis,degrees)*placed(at*-1.f,Matrix4x4::mkIdentity());
  for(size_t j=0; j<s.joints.size(); ++j) {
    bool below = j==i;
    for(int p=s.joints[j].parent; p>=0 && !below; p=s.joints[size_t(p)].parent)
      below = size_t(p)==i;
    if(below)
      w[j] = r*w[j];
    }
  }

// the same about a given point
void bendAbout(const Skel& s, std::vector<Matrix4x4>& w, size_t i, const Vec3& at, const Vec3& axis, float degrees) {
  const Matrix4x4 r = placed(at,Matrix4x4::mkIdentity())*turn(axis,degrees)*placed(at*-1.f,Matrix4x4::mkIdentity());
  for(size_t j=0; j<s.joints.size(); ++j) {
    bool below = j==i;
    for(int p=s.joints[j].parent; p>=0 && !below; p=s.joints[size_t(p)].parent)
      below = size_t(p)==i;
    if(below)
      w[j] = r*w[j];
    }
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
    // (h) an arm with twist bones and a clavicle that follows the arm's rise
    const Skel arm({
      {"chest",       -1, {0, 130, 0}},
      {"clav",         0, {5, 140, 1}},
      {"upper",        1, {18, 140, 0}},
      {"upper_twist",  2, {18, 140, 0}},
      {"fore",         2, {18, 114, 0.5f}},
      {"fore_twist",   4, {18, 101, 0.5f}},
      {"fore_full",    4, {18, 90, 0.5f}},
      {"hand",         4, {18, 88, 0}},
      });
    // (i, j) two legs and two arms: Gothic's and ours - longer legs, a longer trunk, other arms
    const Skel source2({
      {"BIP01",           -1, {0, 90, 0}},
      {"BIP01 PELVIS",     0, {0, 90, 0}},
      {"BIP01 SPINE",      1, {0, 102, 0}},
      {"BIP01 L THIGH",    1, {10, 85, 0}},
      {"BIP01 L CALF",     3, {10, 45, 2}},
      {"BIP01 L FOOT",     4, {10, 8, 0}},
      {"BIP01 L TOE0",     5, {10, 0, 12}},
      {"BIP01 R THIGH",    1, {-10, 85, 0}},
      {"BIP01 R CALF",     7, {-10, 45, 2}},
      {"BIP01 R FOOT",     8, {-10, 8, 0}},
      {"BIP01 R TOE0",     9, {-10, 0, 12}},
      {"BIP01 L UPPERARM", 2, {18, 140, 0}},
      {"BIP01 L FOREARM", 11, {18, 119, -1}},
      {"BIP01 L HAND",    12, {18, 98, 0}},
      {"BIP01 R UPPERARM", 2, {-18, 140, 0}},
      {"BIP01 R FOREARM", 14, {-18, 119, -1}},
      {"BIP01 R HAND",    15, {-18, 98, 0}},
      });
    const Skel body({
      {"root",     -1, {0, 0, 0}},
      {"pelvis",    0, {0, 93, 0}},
      {"thigh_l",   1, {9, 89, 1}},
      {"calf_l",    2, {9, 47, 3}},
      {"foot_l",    3, {9, 8, -1}},
      {"ball_l",    4, {9, 1, 12}},
      {"thigh_r",   1, {-9, 89, 1}},
      {"calf_r",    6, {-9, 47, 3}},
      {"foot_r",    7, {-9, 8, -1}},
      {"ball_r",    8, {-9, 1, 12}},
      {"spine",     1, {0, 106, -1}},
      {"upper_l",  10, {20, 146, 0}},
      {"lower_l",  11, {20, 121, -1}},
      {"hand_l",   12, {20, 96, 0}},
      {"finger_l", 13, {20, 88, 1}},
      {"upper_r",  10, {-20, 146, 0}},
      {"lower_r",  15, {-20, 121, -1}},
      {"hand_r",   16, {-20, 96, 0}},
      });
    // (k) the same body with a short foot: the ankle 4 cm ahead of Gothic's, 8 cm to the ball against Gothic's 12
    std::vector<Joint> shortFoot = body.joints;
    for(auto& j:shortFoot)
      if(j.name=="foot_l" || j.name=="foot_r")
        j.at.z = 4;
    const Skel body3(shortFoot);
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
      {"ARM.MDH",  arm.hierarchy()},
      {"SRC2.MDH", source2.hierarchy()},
      {"BODY.MDH", body.hierarchy()},
      {"BODY3.MDH", body3.hierarchy()},
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

    const Vec3 ankleL = apply(inverse(source2.world[source2.find("BIP01 L FOOT")]),
                              position(source2.world[source2.find("BIP01 L FOOT")])+Vec3(0,-2,4));
    const Vec3 ankleR = apply(inverse(source2.world[source2.find("BIP01 R FOOT")]),
                              position(source2.world[source2.find("BIP01 R FOOT")])+Vec3(0,-2,4));
    std::string bodyBones;
    for(const auto& [rig,src]:std::vector<std::pair<std::string,std::string>>{
          {"pelvis","BIP01 PELVIS"},{"spine","BIP01 SPINE"},{"thigh_l","BIP01 L THIGH"},{"calf_l","BIP01 L CALF"},
          {"foot_l","BIP01 L FOOT"},{"ball_l","BIP01 L TOE0"},{"thigh_r","BIP01 R THIGH"},{"calf_r","BIP01 R CALF"},
          {"foot_r","BIP01 R FOOT"},{"ball_r","BIP01 R TOE0"},{"upper_l","BIP01 L UPPERARM"},
          {"lower_l","BIP01 L FOREARM"},{"hand_l","BIP01 L HAND"},{"upper_r","BIP01 R UPPERARM"},
          {"lower_r","BIP01 R FOREARM"},{"hand_r","BIP01 R HAND"}})
      bodyBones += std::string(bodyBones.empty() ? "" : ",")+"{\"name\": \""+rig+"\", \"source\": \""+src+"\"}";
    const std::string bodyLegs = R"("legs": [{"thigh": "thigh_l", "calf": "calf_l", "foot": "foot_l", "ball": "ball_l",
                  "source_foot": "BIP01 L FOOT", "source_knee": "BIP01 L CALF", "ankle": )"+vec(ankleL)+R"(},
                 {"thigh": "thigh_r", "calf": "calf_r", "foot": "foot_r", "ball": "ball_r",
                  "source_foot": "BIP01 R FOOT", "source_knee": "BIP01 R CALF", "ankle": )"+vec(ankleR)+R"(}])";
    const std::string bodyArms = R"("arms": [{"upper": "upper_l", "lower": "lower_l", "hand": "hand_l",
                  "source_hand": "BIP01 L HAND", "holds": "hand_r"},
                 {"upper": "upper_r", "lower": "lower_r", "hand": "hand_r", "source_hand": "BIP01 R HAND"}])";
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
        "LIFT": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
                "bones": [{"name": "root"}, {"name": "pelvis", "source": "BIP01 PELVIS"},
                          {"name": "thigh_l", "source": "BIP01 L THIGH"}, {"name": "calf_l", "source": "BIP01 L CALF"},
                          {"name": "foot_l", "source": "BIP01 L FOOT"}, {"name": "ball_l", "source": "BIP01 L TOE0"}],
                "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "lift": 0.04},
                "legs": [{"thigh": "thigh_l", "calf": "calf_l", "foot": "foot_l", "ball": "ball_l",
                          "source_foot": "BIP01 L FOOT", "source_knee": "BIP01 L CALF", "ankle": )"+vec(ankle)+R"(,
                          "soft": 1.0}]},
        "HAND": {"skeleton": "HAND.MDH", "source": "SRC.MDH",
                 "bones": [{"name": "hand_l", "source": "BIP01 L HAND"}],
                 "root": {"bone": "hand_l", "source": "BIP01 L HAND"},
                 "fingers": [{"source_hand": "BIP01 L HAND", "source_finger": "BIP01 L FINGER0",
                              "chains": [["index_01_l", "index_02_l", "index_03_l"], ["middle_01_l", "middle_02_l", "middle_03_l"]],
                              "shares": [1.0, 0.9, 0.7]}]},
        "ARM":  {"skeleton": "ARM.MDH", "source": "SRC.MDH",
                 "bones": [{"name": "chest", "source": "BIP01 SPINE"}, {"name": "upper", "source": "BIP01 L UPPERARM"},
                           {"name": "hand", "source": "BIP01 L HAND"}],
                 "root": {"bone": "chest", "source": "BIP01 SPINE"},
                 "twists": [{"bone": "upper_twist", "of": "upper", "against": "clav", "axis": "fore", "share": -1.0},
                            {"bone": "fore_twist", "of": "hand", "against": "fore", "axis": "hand", "share": 0.5},
                            {"bone": "fore_full", "of": "hand", "against": "fore", "axis": "hand", "share": 1.0}],
                 "shoulders": [{"clavicle": "clav", "arm": "upper", "elbow": "fore", "chest": "chest",
                                "share": 0.5, "from": 30, "max": 40}]},
        "ARM_FLAT": {"skeleton": "ARM.MDH", "source": "SRC.MDH",
                 "bones": [{"name": "chest", "source": "BIP01 SPINE"}, {"name": "upper", "source": "BIP01 L UPPERARM"},
                           {"name": "hand", "source": "BIP01 L HAND"}],
                 "root": {"bone": "chest", "source": "BIP01 SPINE"}},
        "KNEES":  {"skeleton": "BODY.MDH", "source": "SRC2.MDH", "bones": [)"+bodyBones+R"(],
                   "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "knees": true, "heel": false}, )"+bodyLegs+R"(},
        "HEEL":   {"skeleton": "BODY3.MDH", "source": "SRC2.MDH", "bones": [)"+bodyBones+R"(],
                   "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "knees": true}, )"+bodyLegs+R"(},
        "PLAIN3": {"skeleton": "BODY3.MDH", "source": "SRC2.MDH", "bones": [)"+bodyBones+R"(],
                   "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}, )"+bodyLegs+R"(},
        "PLAIN2": {"skeleton": "BODY.MDH", "source": "SRC2.MDH", "bones": [)"+bodyBones+R"(],
                   "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}, )"+bodyLegs+R"(},
        "ARMS2":  {"skeleton": "BODY.MDH", "source": "SRC2.MDH", "bones": [)"+bodyBones+R"(],
                   "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}, )"+bodyLegs+R"(, )"+bodyArms+R"(},
        "BAD_KNEES":  {"skeleton": "LEG.MDH", "source": "SRC.MDH",
                       "bones": [{"name": "root", "source": "BIP01"}, {"name": "thigh_l", "source": "BIP01 L THIGH"},
                                 {"name": "calf_l", "source": "BIP01 L CALF"}],
                       "root": {"bone": "root", "source": "BIP01", "knees": true},
                       "legs": [{"thigh": "thigh_l", "calf": "calf_l", "foot": "foot_l", "ball": "ball_l",
                                 "source_foot": "BIP01 L FOOT", "source_knee": "BIP01 L CALF", "ankle": [0,0,0]}]},
        "BAD_HOLDS":  {"skeleton": "BODY.MDH", "source": "SRC2.MDH", "bones": [)"+bodyBones+R"(],
                       "root": {"bone": "pelvis", "source": "BIP01 PELVIS"},
                       "arms": [{"upper": "upper_l", "lower": "lower_l", "hand": "hand_l", "source_hand": "BIP01 L HAND",
                                 "holds": "finger_l"}]},
        "BAD_BONE":   {"skeleton": "LEG.MDH", "source": "SRC.MDH", "bones": [{"name": "knee_l"}],
                       "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}},
        "BAD_SOURCE": {"skeleton": "LEG.MDH", "source": "SRC.MDH", "bones": [{"name": "pelvis", "source": "BIP01 R THIGH"}],
                       "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}},
        "BAD_BIG":    {"skeleton": "BIG.MDH", "source": "SRC.MDH", "bones": [{"name": "node0"}],
                       "root": {"bone": "node0", "source": "BIP01"}},
        "BAD_RECORD": {"skeleton": "NONE.MDH", "source": "SRC.MDH", "bones": [{"name": "pelvis"}],
                       "root": {"bone": "pelvis", "source": "BIP01 PELVIS"}},
        "BAD_TWIST":  {"skeleton": "ARM.MDH", "source": "SRC.MDH", "bones": [{"name": "chest"}],
                       "root": {"bone": "chest", "source": "BIP01 SPINE"},
                       "twists": [{"bone": "fore", "of": "hand", "against": "upper", "axis": "hand", "share": 0.5}]},
        "BAD_RHYTHM": {"skeleton": "ARM.MDH", "source": "SRC.MDH", "bones": [{"name": "chest"}],
                       "root": {"bone": "chest", "source": "BIP01 SPINE"},
                       "shoulders": [{"clavicle": "clav", "arm": "upper", "elbow": "fore", "chest": "chest", "share": 0.5}]}
        },
      "meshes": {"HUM_BODY_TEST.MDM": "LEG"}
      })";
    index.parse(json);
    require(messages.empty(),"a good file is read without messages: "+(messages.empty() ? "" : messages[0]));
    require(index.size()==19,"nineteen rigs are read");

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
    require(index.meshTable().size()==1 && index.meshTable().count("HUM_BODY_TEST")==1 && index.rigNames().size()==19,
            "the table lists its rigs and meshes");
    // a bone's Gothic node: its source, else its parent's, else the source's root
    require(legRig->sourceOf(leg.find("ball_l"))==source.find("BIP01 L TOE0") &&
            legRig->sourceOf(leg.find("pelvis"))==source.find("BIP01 PELVIS") &&
            legRig->sourceOf(leg.find("root"))==source.find("BIP01") && legRig->sourceOf(99)==size_t(-1),
            "a leg bone belongs to its source node");
    const size_t th = leg.find("thigh_l"), ca = leg.find("calf_l"), fo = leg.find("foot_l"), ba = leg.find("ball_l");
    const double L1 = distance(leg.joints[ca].at,leg.joints[th].at), L2 = distance(leg.joints[fo].at,leg.joints[ca].at);
    const size_t sThigh = source.find("BIP01 L THIGH");
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
        // the knee's way: the model's front (+z) of the bind pose carried by Gothic's thigh turn (align 1)
        const Vec3 u = (T-H)*float(1.0/reach);
        const Vec3 front = apply(linearOnly(w[sThigh])*inverse(linearOnly(source.world[sThigh])),Vec3(0,0,1));
        const float along = u.x*front.x + u.y*front.y + u.z*front.z;
        const Vec3 side = front-u*along, knee = K-H;
        if(length(side)>0.2) {
          ++poled;
          const double off = length(knee-u*float(u.x*knee.x + u.y*knee.y + u.z*knee.z));
          require(side.x*knee.x + side.y*knee.y + side.z*knee.z>=0.999*length(side)*off,
                  "the knee bends towards the thigh's front"+at);
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

    // (g) lift: the pelvis rises by 0.04 of its height over the ankle target; soft: the last 1 cm of reach come slowly
    const RenderRig* liftRig = index.rig("LIFT");
    require(liftRig!=nullptr,"the lifted leg rig loads");
    {
      const size_t pe = leg.find("pelvis");
      std::vector<Matrix4x4> lifted(Resources::MAX_NUM_SKELETAL_NODES);
      int soft = 0, hard = 0;
      for(int n=0; n<400; ++n) {
        const auto w = source.randomPose(70,randomObject());
        require(legRig->solve(w.data(),w.size(),out.data()) && liftRig->solve(w.data(),w.size(),lifted.data()),
                "the lifted pose is solved");
        const std::string at = ", pose "+std::to_string(n);
        const Vec3   P = position(out[pe]), Q = position(lifted[pe]);
        const double low  = std::min(double(P.y),double(apply(w[sFoot],ankle).y));
        const double rise = 0.04*(P.y-low);
        require(std::abs(Q.y-P.y-rise)<=1e-3 && std::abs(Q.x-P.x)<=1e-4 && std::abs(Q.z-P.z)<=1e-4,
                "the pelvis rises by the share of its height over the ankle"+at+": "+std::to_string(Q.y-P.y)+" not "+
                std::to_string(rise));
        const Vec3   H = position(lifted[th]), K = position(lifted[ca]), F = position(lifted[fo]);
        const Vec3   T = apply(w[sFoot],ankle);
        const double reach = distance(T,H), near = L1+L2-1.0;
        require(std::abs(distance(K,H)-L1)<=1e-3 && std::abs(distance(F,K)-L2)<=1e-3,"soft IK keeps the lengths"+at);
        if(reach<near-0.01 && reach>std::abs(L1-L2)+0.5) {
          ++hard;
          require(distance(F,T)<=0.01,"short of the soft zone the foot stands on the target"+at);
          }
        else if(reach>near+0.01) {
          ++soft;
          const double want = near + 1.0*(1.0-std::exp(-(reach-near)/1.0));
          require(std::abs(distance(F,H)-want)<=2e-3 && angle(F-H,T-H)<=0.05,
                  "in the soft zone the foot falls short along the target's line"+at+": "+std::to_string(distance(F,H))+
                  " not "+std::to_string(want));
          }
        }
      require(soft>=20 && hard>=50,"the lifted poses cover both zones: "+std::to_string(soft)+" "+std::to_string(hard));
    }

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

    // (h) twist bones and the shoulder rhythm
    {
      const RenderRig* armRig  = index.rig("ARM");
      const RenderRig* flatRig = index.rig("ARM_FLAT");
      require(armRig!=nullptr && flatRig!=nullptr,"the arm rigs load");
      const size_t ch = arm.find("chest"), cl = arm.find("clav"), up = arm.find("upper"), ut = arm.find("upper_twist"),
                   fo = arm.find("fore"), ft = arm.find("fore_twist"), ff = arm.find("fore_full"), ha = arm.find("hand");
      auto rot = [](const Matrix4x4& m) { return IkMath::rotationOf(m); };
      // the twist of `rel` (a turn in some frame) about unit `axis` of that frame, degrees; and the angle of a turn
      auto twistAbout = [](const IkMath::Mat3& rel, const Vec3& axis) {
        IkMath::Quat q = IkMath::toQuat(rel);
        if(q.w<0)
          q = {-q.w,-q.x,-q.y,-q.z};
        return 2.0*std::atan2(double(q.x*axis.x + q.y*axis.y + q.z*axis.z),double(q.w))*180.0/Pi;
        };
      auto turnOf = [](const IkMath::Mat3& r) {
        return std::acos(std::clamp((double(r.m[0][0])+r.m[1][1]+r.m[2][2]-1.0)/2.0,-1.0,1.0))*180.0/Pi;
        };
      // a bone's turn against another from the bind pose, in the other's frame
      auto against = [&](const std::vector<Matrix4x4>& o, size_t of, size_t x) {
        return IkMath::mul(IkMath::mul(IkMath::transpose(rot(o[x])),rot(o[of])),
                           IkMath::transpose(IkMath::mul(IkMath::transpose(rot(arm.world[x])),rot(arm.world[of]))));
        };
      auto axisIn = [&](size_t frame, size_t from, size_t to) {
        const Vec3 d = position(arm.world[to])-position(arm.world[from]);
        return IkMath::normalize(IkMath::mul(IkMath::transpose(rot(arm.world[frame])),d));
        };
      std::vector<Matrix4x4> o(Resources::MAX_NUM_SKELETAL_NODES), f(Resources::MAX_NUM_SKELETAL_NODES);
      require(armRig->solve(source.world.data(),source.world.size(),o.data()),"the arm's bind pose is solved");
      for(size_t i:{ut,ft,ff})
        require(maxDiff(relative(o[size_t(arm.joints[i].parent)],o[i]),linearOnly(arm.local[i]))<=1e-4,
                "in the bind pose a twist bone sits as bound: "+arm.joints[i].name);
      int raised = 0;
      for(int n=0; n<400; ++n) {
        const auto w = source.randomPose(120,randomObject());
        require(armRig->solve(w.data(),w.size(),o.data()) && flatRig->solve(w.data(),w.size(),f.data()),
                "the arm pose is solved");
        const std::string at = ", pose "+std::to_string(n);
        // a twist bone turns against its parent only about the parent's axis
        for(size_t i:{ut,ft,ff}) {
          const size_t p = size_t(arm.joints[i].parent);
          const auto   r = against(o,i,p);
          const Vec3   a = axisIn(p,p,i==ut ? fo : ha);
          require(std::abs(std::abs(twistAbout(r,a))-turnOf(r))<=0.05,"a twist bone turns about its parent's axis"+at);
          }
        // share 1: the full twist of the hand against the forearm; 0.5: half of it; -1: none of the upper arm's
        const Vec3   aFore = axisIn(fo,fo,ha);
        const double hand  = twistAbout(against(o,ha,fo),aFore);
        require(std::abs(twistAbout(against(o,ff,fo),aFore)-hand)<=0.05,"share 1 takes the hand's whole twist"+at);
        require(std::abs(twistAbout(against(o,ft,fo),aFore)-0.5*hand)<=0.05,"share 0.5 takes half of it"+at);
        const Vec3 aUpper = axisIn(cl,up,fo);
        require(std::abs(twistAbout(against(o,ut,cl),aUpper))<=0.05,
                "share -1 takes the upper arm's twist off: "+std::to_string(twistAbout(against(o,ut,cl),aUpper))+at);
        // the rhythm: the clavicle turns further towards the arm by 0.5 x (rise - 30), at most 40 degrees
        const Vec3 armDir = IkMath::normalize(position(f[fo])-position(f[up]));
        const Vec3 down   = IkMath::normalize(IkMath::mul(rot(f[ch]),IkMath::mul(IkMath::transpose(rot(arm.world[ch])),Vec3(0,-1,0))));
        const Vec3 clavF  = IkMath::normalize(position(f[up])-position(f[cl]));
        const double rise = std::acos(std::clamp(double(IkMath::dot(armDir,down)),-1.0,1.0))*180.0/Pi;
        const double gap  = std::acos(std::clamp(double(IkMath::dot(clavF,armDir)),-1.0,1.0))*180.0/Pi;
        const double want = std::min({40.0,0.5*std::max(0.0,rise-30.0),gap});
        const double got  = turnOf(IkMath::mul(rot(o[cl]),IkMath::transpose(rot(f[cl]))));
        require(std::abs(got-want)<=0.05,"the clavicle turns by the rhythm: "+std::to_string(got)+" not "+
                std::to_string(want)+at);
        const Vec3 clavO = IkMath::normalize(position(o[up])-position(o[cl]));
        if(want>0.5) {
          ++raised;
          require(IkMath::dot(clavO,armDir)>IkMath::dot(clavF,armDir),"the clavicle turns towards the arm"+at);
          }
        require(maxDiff(linearOnly(o[up]),linearOnly(f[up]))<=1e-4,"the arm keeps its own turn"+at);
        require(distance(position(o[up]),apply(o[cl],position(arm.local[up])))<=1e-3,"the arm hangs on the clavicle"+at);
        }
      require(raised>=50,"the poses raise the arm: "+std::to_string(raised));
    }

    // (i) knees: the knees bend as Gothic's, a foot on the ground keeps its ball on Gothic's, one in the air takes
    // Gothic's turns, legs that do not carry leave the pelvis where it was
    {
      const RenderRig* kneesRig = index.rig("KNEES");
      const RenderRig* plainRig = index.rig("PLAIN2");
      require(kneesRig!=nullptr && plainRig!=nullptr,"the knee rigs load");
      std::vector<Matrix4x4> o(Resources::MAX_NUM_SKELETAL_NODES), p(Resources::MAX_NUM_SKELETAL_NODES);
      const Vec3 X(1,0,0);
      struct LegIds { size_t th, ca, fo, ba, sTh, sCa, sFo; };
      const LegIds legsIds[2] = {
        {body.find("thigh_l"),body.find("calf_l"),body.find("foot_l"),body.find("ball_l"),
         source2.find("BIP01 L THIGH"),source2.find("BIP01 L CALF"),source2.find("BIP01 L FOOT")},
        {body.find("thigh_r"),body.find("calf_r"),body.find("foot_r"),body.find("ball_r"),
         source2.find("BIP01 R THIGH"),source2.find("BIP01 R CALF"),source2.find("BIP01 R FOOT")}};
      auto relRot = [&](const std::vector<Matrix4x4>& w, size_t s) {
        return linearOnly(w[s])*inverse(linearOnly(source2.world[s]));
        };
      // our knee's flex with Gothic's turns (align 1) and as solved, degrees
      auto fkFlex = [&](const std::vector<Matrix4x4>& w, const LegIds& l) {
        const Vec3 a1 = apply(relRot(w,l.sTh),position(body.world[l.ca])-position(body.world[l.th]));
        const Vec3 a2 = apply(relRot(w,l.sCa),position(body.world[l.fo])-position(body.world[l.ca]));
        return angle(a1,a2);
        };
      auto flex = [&](const LegIds& l) {
        return angle(position(o[l.ca])-position(o[l.th]),position(o[l.fo])-position(o[l.ca]));
        };
      auto gothicBall = [&](const std::vector<Matrix4x4>& w, const LegIds& l) {
        return apply(w[l.sFo],apply(inverse(source2.world[l.sFo]),position(body.world[l.ba])));
        };
      auto check = [&](const std::vector<Matrix4x4>& w, const std::string& what, bool lAir, bool rAir) {
        require(kneesRig->solve(w.data(),w.size(),o.data()) && plainRig->solve(w.data(),w.size(),p.data()),what+" is solved");
        for(size_t n=0; n<2; ++n) {
          const auto& l = legsIds[n];
          const std::string side = n==0 ? " (left)" : " (right)";
          require(std::abs(flex(l)-fkFlex(w,l))<=0.2,what+": the knee bends as Gothic's"+side+": "+std::to_string(flex(l))+
                  " not "+std::to_string(fkFlex(w,l)));
          if(!(n==0 ? lAir : rAir))
            require(distance(position(o[l.ba]),gothicBall(w,l))<=0.05,what+": the ball stays on Gothic's"+side+": "+
                    std::to_string(distance(position(o[l.ba]),gothicBall(w,l))));
          else
            require(maxDiff(linearOnly(o[l.fo]),linearOnly(p[l.fo]))<=1e-4,what+": the foot in the air keeps Gothic's turn"+side);
          }
        const Vec3 a = position(o[body.find("pelvis")]), b = position(p[body.find("pelvis")]);
        require(std::abs(a.x-b.x)<=1e-3 && std::abs(a.z-b.z)<=1e-3 && std::abs(a.y-b.y)<=10.01,
                what+": the pelvis moves only up or down, by 10 cm at most");
        };
      const auto bindW = source2.pose(source2.bindTurns(),Matrix4x4::mkIdentity());
      // standing
      check(bindW,"standing",false,false);
      // a squat: the knees forward, the shins back, the feet flat again
      for(float deep:{20.f,45.f,70.f}) {
        auto w = bindW;
        for(const auto& l:legsIds) {
          bend(source2,w,l.sTh,X,-deep);
          bend(source2,w,l.sCa,X,2.f*deep);
          bend(source2,w,l.sFo,X,-deep);
          }
        check(w,"a squat of "+std::to_string(int(deep)),false,false);
        }
      // toe-off: the left heel up about the ball, the knee a little bent
      for(float heel:{10.f,25.f,40.f}) {
        auto w = bindW;
        const auto& l = legsIds[0];
        bend(source2,w,l.sTh,X,-10);
        bend(source2,w,l.sCa,X,15);
        const Vec3 ball = gothicBall(w,l);
        float sign = 1;
        {
          auto t = w;
          bendAbout(source2,t,l.sFo,ball,X,heel);
          sign = position(t[l.sFo]).y>position(w[l.sFo]).y ? 1.f : -1.f;
        }
        bendAbout(source2,w,l.sFo,ball,X,sign*heel);
        check(w,"toe-off, the heel up "+std::to_string(int(heel)),false,false);
        }
      // a step: the right leg up in the air, the left one standing
      {
        auto w = bindW;
        const auto& r = legsIds[1];
        bend(source2,w,r.sTh,X,-60);
        bend(source2,w,r.sCa,X,70);
        require(gothicBall(w,r).y-gothicBall(w,legsIds[0]).y>8.5f,"the step lifts the foot off the ground");
        check(w,"a step",false,true);
      }
      // lying: no leg carries, the pelvis stays where the plain rig puts it
      {
        const auto w = source2.pose(source2.bindTurns(),turn(X,-90));
        require(kneesRig->solve(w.data(),w.size(),o.data()) && plainRig->solve(w.data(),w.size(),p.data()),"lying is solved");
        require(distance(position(o[body.find("pelvis")]),position(p[body.find("pelvis")]))<=1e-3,
                "lying, the legs do not move the pelvis");
      }
      // any pose: finite, the legs keep their lengths
      for(int n=0; n<300; ++n) {
        const auto w = source2.randomPose(60,randomObject());
        require(kneesRig->solve(w.data(),w.size(),o.data()),"a random pose is solved");
        for(size_t i=0; i<body.joints.size(); ++i)
          require(finite(o[i]),"no NaN with knees, pose "+std::to_string(n));
        for(const auto& l:legsIds)
          require(std::abs(distance(position(o[l.ca]),position(o[l.th]))-distance(body.joints[l.ca].at,body.joints[l.th].at))<=1e-3 &&
                  std::abs(distance(position(o[l.fo]),position(o[l.ca]))-distance(body.joints[l.fo].at,body.joints[l.ca].at))<=1e-3,
                  "with knees the legs keep their lengths, pose "+std::to_string(n));
        }
    }

    // (k) heel: a leg that stands is heel on heel with Gothic's, rolls over its own ball, leaves the ground with a
    // high heel and is never straighter than Gothic's knee; the root's lift is kept between solves
    {
      const RenderRig* heelRig  = index.rig("HEEL");
      const RenderRig* plainRig = index.rig("PLAIN3");
      require(heelRig!=nullptr && plainRig!=nullptr,"the heel rigs load");
      std::vector<Matrix4x4> o(Resources::MAX_NUM_SKELETAL_NODES), p(Resources::MAX_NUM_SKELETAL_NODES);
      const Vec3 X(1,0,0);
      struct LegIds { size_t th, ca, fo, ba, sTh, sCa, sFo; };
      const LegIds legsIds[2] = {
        {body3.find("thigh_l"),body3.find("calf_l"),body3.find("foot_l"),body3.find("ball_l"),
         source2.find("BIP01 L THIGH"),source2.find("BIP01 L CALF"),source2.find("BIP01 L FOOT")},
        {body3.find("thigh_r"),body3.find("calf_r"),body3.find("foot_r"),body3.find("ball_r"),
         source2.find("BIP01 R THIGH"),source2.find("BIP01 R CALF"),source2.find("BIP01 R FOOT")}};
      const size_t pelvis = body3.find("pelvis");
      auto relRot = [&](const std::vector<Matrix4x4>& w, size_t s) {
        return linearOnly(w[s])*inverse(linearOnly(source2.world[s]));
        };
      auto fkFlex = [&](const std::vector<Matrix4x4>& w, const LegIds& l) {
        const Vec3 a1 = apply(relRot(w,l.sTh),position(body3.world[l.ca])-position(body3.world[l.th]));
        const Vec3 a2 = apply(relRot(w,l.sCa),position(body3.world[l.fo])-position(body3.world[l.ca]));
        return angle(a1,a2);
        };
      auto flex = [&](const LegIds& l) {
        return angle(position(o[l.ca])-position(o[l.th]),position(o[l.fo])-position(o[l.ca]));
        };
      // a bind point of ours carried by Gothic's foot
      auto carried = [&](const std::vector<Matrix4x4>& w, const LegIds& l, const Vec3& bindPoint) {
        return apply(w[l.sFo],apply(inverse(source2.world[l.sFo]),bindPoint));
        };
      const float back = position(body3.world[legsIds[0].fo]).z - position(source2.world[legsIds[0].sFo]).z;
      require(std::abs(back-4.f)<1e-4,"the short foot's ankle is 4 cm ahead of Gothic's");
      // our ankle and ball heel on heel with Gothic's foot as it is turned
      auto heelAnkle = [&](const std::vector<Matrix4x4>& w, const LegIds& l) {
        return carried(w,l,position(body3.world[l.fo])-Vec3(0,0,back));
        };
      auto heelBall = [&](const std::vector<Matrix4x4>& w, const LegIds& l) {
        return carried(w,l,position(body3.world[l.ba])-Vec3(0,0,back));
        };
      auto knees = [&](const std::vector<Matrix4x4>& w, const std::string& what) {
        for(const auto& l:legsIds)
          require(std::abs(flex(l)-fkFlex(w,l))<=0.2,what+": the knee bends as Gothic's: "+std::to_string(flex(l))+
                  " not "+std::to_string(fkFlex(w,l)));
        };
      // the foot's turn about its side axis, the sign that lifts the heel about the ball
      auto heelSign = [&](const std::vector<Matrix4x4>& w, const LegIds& l) {
        auto t = w;
        bendAbout(source2,t,l.sFo,carried(w,l,position(body3.world[l.ba])),X,5);
        return position(t[l.sFo]).y>position(w[l.sFo]).y ? 1.f : -1.f;
        };
      const auto bindW = source2.pose(source2.bindTurns(),Matrix4x4::mkIdentity());

      // standing: the ankle over Gothic's, the ball 4 cm short of where Gothic carries it
      require(heelRig->solve(bindW.data(),bindW.size(),o.data()),"standing on the heel is solved");
      knees(bindW,"standing on the heel");
      for(const auto& l:legsIds) {
        require(distance(position(o[l.fo]),heelAnkle(bindW,l))<=0.05,"standing: the ankle stands on Gothic's: "+
                std::to_string(distance(position(o[l.fo]),heelAnkle(bindW,l))));
        require(distance(position(o[l.ba]),heelBall(bindW,l))<=0.05,"standing: the ball is the foot's length from the heel");
        }
      const float standLift = position(o[pelvis]).y;

      // the toes up by 30 degrees on both feet: the foot turns about Gothic's ankle and takes no heel lift of its own
      {
        auto w = bindW;
        for(const auto& l:legsIds)
          bend(source2,w,l.sFo,X,-heelSign(bindW,l)*30);
        require(heelRig->solve(w.data(),w.size(),o.data()) && plainRig->solve(w.data(),w.size(),p.data()),"toes up is solved");
        knees(w,"toes up");
        for(const auto& l:legsIds) {
          require(distance(position(o[l.fo]),heelAnkle(w,l))<=0.05,"toes up: the ankle stays on Gothic's: "+
                  std::to_string(distance(position(o[l.fo]),heelAnkle(w,l))));
          require(maxDiff(linearOnly(o[l.fo]),linearOnly(p[l.fo]))<=1e-4,"toes up: the foot keeps Gothic's turn");
          }
      }
      // the heel up by 15 degrees about the ball on both feet: our ball stays where it stood, the foot turns further
      {
        auto w = bindW;
        for(const auto& l:legsIds)
          bendAbout(source2,w,l.sFo,carried(bindW,l,position(body3.world[l.ba])),X,heelSign(bindW,l)*15);
        require(heelRig->solve(w.data(),w.size(),o.data()) && plainRig->solve(w.data(),w.size(),p.data()),"heel up is solved");
        knees(w,"heel up");
        for(const auto& l:legsIds) {
          require(distance(position(o[l.ba]),heelBall(bindW,l))<=0.1,"heel up: the ball stays where it stood: "+
                  std::to_string(distance(position(o[l.ba]),heelBall(bindW,l))));
          const double lever = distance(position(w[l.sFo]),carried(w,l,position(body3.world[l.ba])))/
                               distance(position(body3.world[l.fo]),position(body3.world[l.ba]));
          const double want  = std::asin(lever*std::sin(15.0*Pi/180.0))*180.0/Pi - 15.0;
          require(want>2.0 && std::abs(turnAngle(o[l.fo],p[l.fo])-want)<=0.3,
                  "heel up: the shorter foot turns further for the same rise: "+std::to_string(turnAngle(o[l.fo],p[l.fo]))+
                  " not "+std::to_string(want));
          }
      }
      // the heel up by 60 degrees: the foot has left the ground, the leg takes Gothic's turns
      std::vector<Matrix4x4> highHeels;
      {
        auto w = bindW;
        for(const auto& l:legsIds)
          bendAbout(source2,w,l.sFo,carried(bindW,l,position(body3.world[l.ba])),X,heelSign(bindW,l)*60);
        highHeels = w;
        require(heelRig->solve(w.data(),w.size(),o.data()) && plainRig->solve(w.data(),w.size(),p.data()),"a high heel is solved");
        knees(w,"a high heel");
        for(const auto& l:legsIds)
          require(maxDiff(linearOnly(o[l.fo]),linearOnly(p[l.fo]))<=1e-4,"a high heel: the foot keeps Gothic's turn");
        require(std::abs(position(o[pelvis]).y-position(p[pelvis]).y)<=1e-3,"a high heel alone: no leg lifts the pelvis");
      }
      // a seat: with the thighs level the pelvis is held by something else - ball on ball, as without heel
      {
        auto w = bindW;
        for(const auto& l:legsIds) {
          bend(source2,w,l.sTh,X,-80);
          bend(source2,w,l.sCa,X,160);
          bend(source2,w,l.sFo,X,-80);
          }
        require(heelRig->solve(w.data(),w.size(),o.data()),"a seat is solved");
        knees(w,"a seat");
        for(const auto& l:legsIds)
          require(distance(position(o[l.ba]),carried(w,l,position(body3.world[l.ba])))<=0.05,
                  "a seat: the ball stays on Gothic's: "+
                  std::to_string(distance(position(o[l.ba]),carried(w,l,position(body3.world[l.ba])))));
      }
      // the memory: a solve without it and the first one with it are the same
      {
        RenderRig::Gait gait;
        require(heelRig->solve(bindW.data(),bindW.size(),o.data()) &&
                heelRig->solve(bindW.data(),bindW.size(),p.data(),RenderRig::Hands(),&gait,0.033f),"the first solve with a memory");
        for(size_t i=0; i<body3.joints.size(); ++i)
          require(maxDiff(o[i],p[i])<=1e-5,"the first solve with a memory is the solve without it");
        require(gait.valid && std::abs(gait.lift-(standLift-93.f))<=1e-3 && gait.plant[0]==1.f && gait.plant[1]==1.f,
                "the memory keeps the lift and the legs' shares: "+std::to_string(gait.lift));
        const float stand = gait.lift;
        // a lift above the legs' wish comes down over LiftTau; meanwhile the heels come up about the balls and the
        // knees are no straighter than Gothic's
        gait.lift = stand+6;
        require(heelRig->solve(bindW.data(),bindW.size(),o.data(),RenderRig::Hands(),&gait,0.08f),"a lift too high is solved");
        require(std::abs(gait.lift-(stand+6.f/std::exp(1.f)))<=0.05,"the lift follows over its time: "+std::to_string(gait.lift-stand));
        for(const auto& l:legsIds) {
          require(flex(l)>=fkFlex(bindW,l)-0.2,"a lift too high: the knee is no straighter than Gothic's: "+
                  std::to_string(flex(l))+" under "+std::to_string(fkFlex(bindW,l)));
          require(position(o[l.fo]).y-heelAnkle(bindW,l).y>1.0 && position(o[l.ba]).y-heelBall(bindW,l).y>-0.05,
                  "a lift too high: the heel comes up, the ball does not go under the ground");
          }
        // a lift under the legs' wish does not wait: a pelvis too low would bend the knees
        gait.lift = stand-6;
        require(heelRig->solve(bindW.data(),bindW.size(),o.data(),RenderRig::Hands(),&gait,0.001f),"a lift too low is solved");
        require(std::abs(gait.lift-stand)<=0.05,"a lift under what the standing legs need comes up at once: "+std::to_string(gait.lift-stand));
        // in the air with the legs down the lift holds; lying it goes to none
        gait.lift = stand+2;
        require(heelRig->solve(highHeels.data(),highHeels.size(),o.data(),RenderRig::Hands(),&gait,0.5f),"a flight is solved");
        require(std::abs(gait.lift-(stand+2))<=1e-3,"no foot on the ground, the legs down: the lift holds: "+std::to_string(gait.lift-stand));
        const auto lying = source2.pose(source2.bindTurns(),turn(X,-90));
        for(int n=0; n<30; ++n)
          require(heelRig->solve(lying.data(),lying.size(),o.data(),RenderRig::Hands(),&gait,0.033f),"lying is solved");
        require(std::abs(gait.lift)<=0.01,"lying, the lift goes to none: "+std::to_string(gait.lift));
        // a leg in the air keeps its share of the heel, one on the ground lets it go as the pose does
        require(gait.plant[0]<0.1f && gait.plant[1]<0.1f,"lying, the legs stop standing on their heels");
        // any pose: finite, the legs keep their lengths
        for(int n=0; n<300; ++n) {
          const auto w = source2.randomPose(60,randomObject());
          require(heelRig->solve(w.data(),w.size(),o.data(),RenderRig::Hands(),&gait,0.033f),"a random pose is solved");
          require(std::isfinite(gait.lift) && std::abs(gait.lift)<=10.01f,"the lift stays within 10 cm, pose "+std::to_string(n));
          for(size_t i=0; i<body3.joints.size(); ++i)
            require(finite(o[i]),"no NaN with the heel, pose "+std::to_string(n));
          for(const auto& l:legsIds)
            require(std::abs(distance(position(o[l.ca]),position(o[l.th]))-distance(body3.joints[l.ca].at,body3.joints[l.th].at))<=1e-3 &&
                    std::abs(distance(position(o[l.fo]),position(o[l.ca]))-distance(body3.joints[l.fo].at,body3.joints[l.ca].at))<=1e-3,
                    "with the heel the legs keep their lengths, pose "+std::to_string(n));
          }
      }
    }

    // (j) arms: without hands they are FK; `world` puts the wrists on Gothic's, `hold` the left one where Gothic holds
    // it against the right hand
    {
      const RenderRig* armsRig  = index.rig("ARMS2");
      const RenderRig* plainRig = index.rig("PLAIN2");
      require(armsRig!=nullptr,"the arms rig loads");
      std::vector<Matrix4x4> o(Resources::MAX_NUM_SKELETAL_NODES), p(Resources::MAX_NUM_SKELETAL_NODES);
      const size_t ul = body.find("upper_l"), ll = body.find("lower_l"), hl = body.find("hand_l"), fl = body.find("finger_l");
      const size_t ur = body.find("upper_r"), lr = body.find("lower_r"), hr = body.find("hand_r");
      const size_t gL = source2.find("BIP01 L HAND"), gR = source2.find("BIP01 R HAND");
      const double l1 = distance(body.joints[ll].at,body.joints[ul].at), l2 = distance(body.joints[hl].at,body.joints[ll].at);
      int worldReached = 0, held = 0;
      for(int n=0; n<300; ++n) {
        const auto w = source2.randomPose(50,randomObject());
        const std::string at = ", pose "+std::to_string(n);
        require(armsRig->solve(w.data(),w.size(),o.data()) && plainRig->solve(w.data(),w.size(),p.data()),"arms are solved"+at);
        for(size_t i=0; i<body.joints.size(); ++i)
          require(maxDiff(o[i],p[i])<=1e-4,"without hands the arms are as the other bones"+at);

        require(armsRig->solve(w.data(),w.size(),o.data(),RenderRig::Hands{0,1}),"world hands are solved"+at);
        for(const auto& [u,lo,h,g]:std::vector<std::array<size_t,4>>{{ul,ll,hl,gL},{ur,lr,hr,gR}}) {
          require(std::abs(distance(position(o[lo]),position(o[u]))-l1)<=1e-3 &&
                  std::abs(distance(position(o[h]),position(o[lo]))-l2)<=1e-3,"the arm keeps its lengths"+at);
          if(distance(position(w[g]),position(o[u]))<l1+l2-0.5) {
            ++worldReached;
            require(distance(position(o[h]),position(w[g]))<=0.01,"world: the wrist on Gothic's"+at);
            }
          require(maxDiff(linearOnly(o[h]),linearOnly(p[h]))<=1e-4,"the hand keeps its turn"+at);
          }
        require(distance(position(o[fl]),apply(o[hl],position(body.local[fl])))<=1e-3,"the finger follows the solved hand"+at);

        require(armsRig->solve(w.data(),w.size(),o.data(),RenderRig::Hands{1,0}),"held hands are solved"+at);
        for(size_t i:{ur,lr,hr})
          require(maxDiff(o[i],p[i])<=1e-4,"hold: the right arm as the other bones"+at);
        // the left wrist in the right hand's frame, ours turned back to Gothic's axes, against Gothic's
        const Matrix4x4 frame = linearOnly(o[hr])*inverse(linearOnly(body.world[hr]))*linearOnly(source2.world[gR]);
        const Vec3 ours   = apply(inverse(frame),position(o[hl])-position(o[hr]));
        const Vec3 gothic = apply(inverse(linearOnly(w[gR])),position(w[gL])-position(w[gR]));
        const Vec3 want   = position(o[hr]) + apply(frame,gothic);
        if(distance(want,position(o[ul]))<l1+l2-0.5) {
          ++held;
          require(distance(ours,gothic)<=0.01,"hold: the left wrist where Gothic holds it"+at+": "+std::to_string(distance(ours,gothic)));
          }
        }
      require(worldReached>=200 && held>=100,"the arm poses reach: "+std::to_string(worldReached)+" "+std::to_string(held));
    }

    // (f) each broken rig is left out with its reason, once
    const std::map<std::string,std::string> broken = {
      {"BAD_BONE",   "bone \"knee_l\" is not in skeleton \"LEG.MDH\""},
      {"BAD_SOURCE", "source \"BIP01 R THIGH\" is not in skeleton \"SRC.MDH\""},
      {"BAD_BIG",    "97 bones in \"BIG.MDH\", more than 96"},
      {"BAD_RECORD", "no skeleton record \"NONE.MDH\""},
      {"BAD_TWIST",  "the twist bone \"fore\" has children"},
      {"BAD_RHYTHM", "the arm \"upper\" has no source"},
      {"BAD_KNEES",  "legs[0]: knees need the thigh \"thigh_l\" right under the root bone"},
      {"BAD_HOLDS",  "arms: \"finger_l\" is not the hand of an arm that holds nothing"},
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
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS"},
           "twists": [{"bone": "calf_l", "of": "foot_l", "against": "thigh_l", "axis": "foot_l", "share": 3}]}}})",
                                                                            "render rig R: twists[0]: share must be a number from -2 to 2"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS"},
           "shoulders": [{"clavicle": "thigh_l", "arm": "calf_l", "elbow": "foot_l", "chest": "pelvis", "max": 100}]}}})",
                                                                            "render rig R: shoulders[0]: max must be a number from 0 to 90"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "lift": 2}}}})",
                                                                            "render rig R: root: lift must be a number from 0 to 0.5"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS"},
           "legs": [{"thigh": "thigh_l", "calf": "calf_l", "foot": "foot_l", "ball": "ball_l", "source_foot": "BIP01 L FOOT",
                     "source_knee": "BIP01 L CALF", "ankle": [0,0,0], "soft": -1}]}}})",
                                                                            "render rig R: legs[0]: soft must be a number from 0 to 20"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "lift": 0.04, "knees": true}}}})",
                                                                            "render rig R: root: lift and knees exclude each other"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "knees": 1}}}})",
                                                                            "render rig R: root: knees must be true or false"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "knees": true, "heel": 1}}}})",
                                                                            "render rig R: root: heel must be true or false"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS", "heel": true}}}})",
                                                                            "render rig R: root: heel needs knees"},
      {R"({"format": "gothic-render-rigs", "version": 1, "rigs": {"R": {"skeleton": "LEG.MDH", "source": "SRC.MDH",
           "bones": [{"name": "pelvis"}], "root": {"bone": "pelvis", "source": "BIP01 PELVIS"},
           "arms": [{"upper": "thigh_l", "lower": "calf_l", "hand": "foot_l", "source_hand": "BIP01 L FOOT", "soft": 30}]}}})",
                                                                            "render rig R: arms[0]: soft must be a number from 0 to 20"},
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
