#include "renderrig.h"

#include "skeleton.h"
#include "resources.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>

using namespace Tempest;
using IkMath::Mat3;

namespace {

constexpr float Pi     = 3.14159265358979f;  // M_PI is not standard (MSVC)
constexpr float Degree = Pi/180.f;

// knees (cm at the rig's bind size, scaled with the npc): a foot is on the ground while its ball is within
// ContactNear of the lowest ball and leaves it by ContactFar; a leg carries the body from CarryFrom to CarryTo of its
// hip-ankle line pointing down; the pelvis follows the lower ankles (Sigma), a leg on the ground but not carrying
// gives way by Slack, the pelvis moves by MaxShift at most; a foot turns about its ball by PitchMax with the heel up
constexpr float ContactNear = 2.f,  ContactFar = 8.f;
constexpr float CarryFrom   = 0.35f, CarryTo   = 0.6f;
constexpr float Sigma       = 3.f,  Slack      = 20.f, MaxShift = 10.f;
constexpr float PitchMax    = 50.f*Degree;
constexpr int   BandSteps   = 9;
// heel: a leg stands on its foot from ThighFrom to ThighTo of its thigh pointing down and from ShinFrom to ShinTo of
// its shin; the foot rolls over its own ball up to RollFrom of Gothic's heel lift and has left the ground by RollTo;
// its own heel lift fades out from ToesFrom to ToesTo of toes up; the root's lift follows over LiftTau seconds, a
// leg's share of the heel over PlantTau
constexpr float ThighFrom   = 0.5f, ThighTo   = 0.8f;
constexpr float ShinFrom    = 0.2f, ShinTo    = 0.5f;
const     float RollFrom    = std::cos(25.f*Degree), RollTo = std::cos(45.f*Degree);
constexpr float ToesFrom    = 10.f*Degree, ToesTo = 25.f*Degree;
constexpr float LiftTau     = 0.08f, PlantTau = 0.3f;

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error(what);
  }

std::string quote(std::string_view s) {
  return "\""+std::string(s)+"\"";
  }

std::string_view str(const rapidjson::Value& v) {
  return std::string_view(v.GetString(),v.GetStringLength());
  }

void onlyFields(const rapidjson::Value& obj, std::initializer_list<std::string_view> known, const std::string& where) {
  for(const auto& m:obj.GetObject())
    if(std::find(known.begin(),known.end(),str(m.name))==known.end())
      fail(where+"unknown field "+quote(str(m.name)));
  }

std::string name(const rapidjson::Value& obj, const char* key, const std::string& where, bool required = true) {
  if(!obj.HasMember(key)) {
    if(required)
      fail(where+"no "+key);
    return {};
    }
  const auto& v = obj[key];
  if(!v.IsString() || v.GetStringLength()==0 || str(v).find('\0')!=std::string_view::npos)
    fail(where+key+" must be a name");
  return std::string(str(v));
  }

std::vector<float> numbers(const rapidjson::Value& v, size_t count, const std::string& what) {
  if(!v.IsArray() || (count!=0 && v.Size()!=count) || v.Empty())
    fail(what+(count!=0 ? " must be "+std::to_string(count)+" numbers" : " must be a list of numbers"));
  std::vector<float> ret;
  for(const auto& x:v.GetArray()) {
    if(!x.IsNumber() || !std::isfinite(x.GetDouble()))
      fail(what+" must be finite numbers");
    ret.push_back(float(x.GetDouble()));
    }
  return ret;
  }

// a number field within [lo, hi] (`range` names it in the message); 0 when it is left out
float number(const rapidjson::Value& obj, const char* key, double lo, double hi, const char* range,
             const std::string& where) {
  if(!obj.HasMember(key))
    return 0;
  const auto& v = obj[key];
  if(!v.IsNumber() || !std::isfinite(v.GetDouble()) || v.GetDouble()<lo || v.GetDouble()>hi)
    fail(where+key+" must be a number from "+range);
  return float(v.GetDouble());
  }

const rapidjson::Value* list(const rapidjson::Value& obj, const char* key, const std::string& where) {
  if(!obj.HasMember(key))
    return nullptr;
  if(!obj[key].IsArray())
    fail(where+key+" must be a list");
  return &obj[key];
  }

RenderRigSpec parseRig(std::string_view rigName, const rapidjson::Value& v) {
  if(!v.IsObject())
    fail("the rig must be an object");
  onlyFields(v,{"skeleton","source","bones","root","legs","fingers","twists","shoulders","arms","sockets"},"");
  RenderRigSpec s;
  s.name     = std::string(rigName);
  s.skeleton = name(v,"skeleton","");
  s.source   = name(v,"source","");

  if(!v.HasMember("bones") || !v["bones"].IsArray() || v["bones"].Empty())
    fail("bones must be a list of bones");
  for(const auto& b:v["bones"].GetArray()) {
    const std::string where = "bones["+std::to_string(s.bones.size())+"]: ";
    if(!b.IsObject())
      fail(where+"must be an object");
    onlyFields(b,{"name","source","align"},where);
    RenderRigSpec::Bone bone;
    bone.name   = name(b,"name",where);
    bone.source = name(b,"source",where,false);
    if(b.HasMember("align")) {
      const auto q   = numbers(b["align"],4,where+"align");
      const float len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
      if(len<1e-6f)
        fail(where+"align must be a turn, not zero");
      bone.align = {q[0]/len, q[1]/len, q[2]/len, q[3]/len};
      }
    s.bones.push_back(std::move(bone));
    }

  if(!v.HasMember("root") || !v["root"].IsObject())
    fail("root must be an object");
  onlyFields(v["root"],{"bone","source","lift","knees","heel"},"root: ");
  s.rootBone   = name(v["root"],"bone","root: ");
  s.rootSource = name(v["root"],"source","root: ");
  if(v["root"].HasMember("lift")) {
    const auto& lift = v["root"]["lift"];
    if(!lift.IsNumber() || !std::isfinite(lift.GetDouble()) || lift.GetDouble()<0 || lift.GetDouble()>0.5)
      fail("root: lift must be a number from 0 to 0.5");
    s.rootLift = float(lift.GetDouble());
    }
  if(v["root"].HasMember("knees")) {
    if(!v["root"]["knees"].IsBool())
      fail("root: knees must be true or false");
    s.rootKnees = v["root"]["knees"].GetBool();
    if(s.rootKnees && s.rootLift>0)
      fail("root: lift and knees exclude each other");
    }
  s.rootHeel = s.rootKnees;
  if(v["root"].HasMember("heel")) {
    if(!v["root"]["heel"].IsBool())
      fail("root: heel must be true or false");
    s.rootHeel = v["root"]["heel"].GetBool();
    if(s.rootHeel && !s.rootKnees)
      fail("root: heel needs knees");
    }

  if(auto legs = list(v,"legs","")) {
    for(const auto& l:legs->GetArray()) {
      const std::string where = "legs["+std::to_string(s.legs.size())+"]: ";
      if(!l.IsObject())
        fail(where+"must be an object");
      onlyFields(l,{"thigh","calf","foot","ball","source_foot","source_knee","ankle","soft"},where);
      RenderRigSpec::Leg leg;
      leg.thigh      = name(l,"thigh",where);
      leg.calf       = name(l,"calf",where);
      leg.foot       = name(l,"foot",where);
      leg.ball       = name(l,"ball",where);
      leg.sourceFoot = name(l,"source_foot",where);
      leg.sourceKnee = name(l,"source_knee",where);
      if(!l.HasMember("ankle"))
        fail(where+"no ankle");
      const auto a = numbers(l["ankle"],3,where+"ankle");
      leg.ankle = {a[0],a[1],a[2]};
      if(l.HasMember("soft")) {
        const auto& soft = l["soft"];
        if(!soft.IsNumber() || !std::isfinite(soft.GetDouble()) || soft.GetDouble()<0 || soft.GetDouble()>20)
          fail(where+"soft must be a number from 0 to 20");
        leg.soft = float(soft.GetDouble());
        }
      s.legs.push_back(std::move(leg));
      }
    }

  if(auto fingers = list(v,"fingers","")) {
    for(const auto& f:fingers->GetArray()) {
      const std::string where = "fingers["+std::to_string(s.fingers.size())+"]: ";
      if(!f.IsObject())
        fail(where+"must be an object");
      onlyFields(f,{"source_hand","source_finger","chains","shares"},where);
      RenderRigSpec::Fingers fs;
      fs.sourceHand   = name(f,"source_hand",where);
      fs.sourceFinger = name(f,"source_finger",where);
      if(!f.HasMember("shares"))
        fail(where+"no shares");
      fs.shares = numbers(f["shares"],0,where+"shares");
      if(!f.HasMember("chains") || !f["chains"].IsArray() || f["chains"].Empty())
        fail(where+"chains must be a list of bone lists");
      for(const auto& c:f["chains"].GetArray()) {
        if(!c.IsArray() || c.Empty() || c.Size()>fs.shares.size())
          fail(where+"each chain must be a list of 1 to "+std::to_string(fs.shares.size())+" bones, one per share");
        std::vector<std::string> chain;
        for(const auto& b:c.GetArray()) {
          if(!b.IsString() || b.GetStringLength()==0)
            fail(where+"chains must hold bone names");
          chain.emplace_back(str(b));
          }
        fs.chains.push_back(std::move(chain));
        }
      s.fingers.push_back(std::move(fs));
      }
    }

  if(auto twists = list(v,"twists","")) {
    for(const auto& t:twists->GetArray()) {
      const std::string where = "twists["+std::to_string(s.twists.size())+"]: ";
      if(!t.IsObject())
        fail(where+"must be an object");
      onlyFields(t,{"bone","of","against","axis","share"},where);
      RenderRigSpec::Twist tw;
      tw.bone    = name(t,"bone",where);
      tw.of      = name(t,"of",where);
      tw.against = name(t,"against",where);
      tw.axis    = name(t,"axis",where);
      if(!t.HasMember("share"))
        fail(where+"no share");
      tw.share   = number(t,"share",-2,2,"-2 to 2",where);
      s.twists.push_back(std::move(tw));
      }
    }

  if(auto shoulders = list(v,"shoulders","")) {
    for(const auto& sh:shoulders->GetArray()) {
      const std::string where = "shoulders["+std::to_string(s.shoulders.size())+"]: ";
      if(!sh.IsObject())
        fail(where+"must be an object");
      onlyFields(sh,{"clavicle","arm","elbow","chest","share","from","max"},where);
      RenderRigSpec::Shoulder r;
      r.clavicle = name(sh,"clavicle",where);
      r.arm      = name(sh,"arm",where);
      r.elbow    = name(sh,"elbow",where);
      r.chest    = name(sh,"chest",where);
      r.share    = number(sh,"share",0,1,"0 to 1",where);
      r.from     = number(sh,"from",0,180,"0 to 180",where);
      r.max      = number(sh,"max",0,90,"0 to 90",where);
      s.shoulders.push_back(std::move(r));
      }
    }

  if(auto arms = list(v,"arms","")) {
    for(const auto& a:arms->GetArray()) {
      const std::string where = "arms["+std::to_string(s.arms.size())+"]: ";
      if(!a.IsObject())
        fail(where+"must be an object");
      onlyFields(a,{"upper","lower","hand","source_hand","holds","soft"},where);
      RenderRigSpec::Arm arm;
      arm.upper      = name(a,"upper",where);
      arm.lower      = name(a,"lower",where);
      arm.hand       = name(a,"hand",where);
      arm.sourceHand = name(a,"source_hand",where);
      arm.holds      = name(a,"holds",where,false);
      arm.soft       = number(a,"soft",0,20,"0 to 20",where);
      s.arms.push_back(std::move(arm));
      }
    }

  if(v.HasMember("sockets")) {
    if(!v["sockets"].IsObject())
      fail("sockets must be an object: source node -> {bone, offset}");
    for(const auto& m:v["sockets"].GetObject()) {
      const std::string node(str(m.name));
      const std::string where = "sockets["+quote(node)+"]: ";
      if(node.empty() || !m.value.IsObject())
        fail(where+"expected a source node name and an object");
      onlyFields(m.value,{"bone","offset"},where);
      RenderRigSpec::Socket socket;
      socket.source = node;
      socket.bone   = name(m.value,"bone",where);
      if(!m.value.HasMember("offset"))
        fail(where+"no offset");
      const auto o = numbers(m.value["offset"],16,where+"offset");
      socket.offset = Matrix4x4(o.data());
      s.sockets.push_back(std::move(socket));
      }
    }
  return s;
  }

// the mesh as asked for: upper case, without folder and extension
std::string meshKey(std::string_view mesh) {
  const auto slash = mesh.find_last_of("/\\");
  if(slash!=std::string_view::npos)
    mesh.remove_prefix(slash+1);
  const auto dot = mesh.find_last_of('.');
  if(dot!=std::string_view::npos)
    mesh = mesh.substr(0,dot);
  std::string key(mesh);
  for(auto& c:key)
    c = char(std::toupper(uint8_t(c)));
  return key;
  }

// the linear part of a bone matrix as it is (a model scale stays in it)
Mat3 linear(const Matrix4x4& m) {
  Mat3 r = {};
  for(int row=0; row<3; ++row)
    for(int col=0; col<3; ++col)
      r.m[row][col] = m.at(col,row);
  return r;
  }

void setLinear(Matrix4x4& m, const Mat3& r) {
  for(int row=0; row<3; ++row)
    for(int col=0; col<3; ++col)
      m.set(col,row,r.m[row][col]);
  }

void setPosition(Matrix4x4& m, const Vec3& p) {
  m.set(3,0,p.x);
  m.set(3,1,p.y);
  m.set(3,2,p.z);
  }

Vec3 apply(const Matrix4x4& m, const Vec3& p) {
  return {m.at(0,0)*p.x + m.at(1,0)*p.y + m.at(2,0)*p.z + m.at(3,0),
          m.at(0,1)*p.x + m.at(1,1)*p.y + m.at(2,1)*p.z + m.at(3,1),
          m.at(0,2)*p.x + m.at(1,2)*p.y + m.at(2,2)*p.z + m.at(3,2)};
  }

// axis and angle (0..pi) of a turn
float turnOf(const Mat3& r, Vec3& axis) {
  auto q = IkMath::toQuat(r);
  if(q.w<0)
    q = {-q.w,-q.x,-q.y,-q.z};
  const float s = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z);
  if(!(s>1e-7f)) {
    axis = {1,0,0};
    return 0;
    }
  axis = {q.x/s, q.y/s, q.z/s};
  return 2.f*std::atan2(s,q.w);
  }

}

RenderRig::~RenderRig() = default;

std::unique_ptr<RenderRig> RenderRig::build(const RenderRigSpec& spec, const asset::ModelHierarchy& rigH,
                                            const asset::ModelHierarchy& sourceH) {
  const std::string prefix = "render rig "+spec.name+": ";
  auto error = [&](const std::string& what) { fail(prefix+what); };
  auto checkTree = [&](const asset::ModelHierarchy& h, const std::string& file) {
    if(h.nodes.empty())
      error("skeleton "+quote(file)+" has no nodes");
    if(h.nodes.size()>Resources::MAX_NUM_SKELETAL_NODES)
      error(std::to_string(h.nodes.size())+" bones in "+quote(file)+", more than "+
            std::to_string(Resources::MAX_NUM_SKELETAL_NODES));
    for(const auto& n:h.nodes)
      if(n.parent_index<-1 || n.parent_index>=int(h.nodes.size()))
        error("skeleton "+quote(file)+": node "+quote(n.name)+" has no parent "+std::to_string(n.parent_index));
    };
  checkTree(rigH,spec.skeleton);
  checkTree(sourceH,spec.source);

  std::unique_ptr<RenderRig> r(new RenderRig());
  r->rigName = spec.name;
  r->own     = std::make_unique<Skeleton>(rigH,nullptr,spec.skeleton);
  const Skeleton  src(sourceH,nullptr,spec.source);
  const Skeleton& rig = *r->own;
  for(const auto& n:src.nodes)
    r->sourceNames.push_back(n.name);

  const size_t count = rig.nodes.size();
  std::vector<uint8_t> placed(count,0);
  while(r->order.size()<count) {
    const size_t before = r->order.size();
    for(size_t i=0; i<count; ++i) {
      const size_t p = rig.nodes[i].parent;
      if(!placed[i] && (p==None || placed[p])) {
        placed[i] = 1;
        r->order.push_back(i);
        }
      }
    if(r->order.size()==before)
      error("skeleton "+quote(spec.skeleton)+" has a loop of parents");
    }

  auto rigNode = [&](const std::string& n, const char* what) {
    const size_t i = rig.findNode(n);
    if(i==None)
      error(std::string(what)+" "+quote(n)+" is not in skeleton "+quote(spec.skeleton));
    return i;
    };
  auto sourceNode = [&](const std::string& n) {
    const size_t i = src.findNode(n);
    if(i==None)
      error("source "+quote(n)+" is not in skeleton "+quote(spec.source));
    return i;
    };
  auto inverse = [](Matrix4x4 m) {
    m.inverse();
    return m;
    };

  // the bind poses, once; a bone without a parent goes under the source's root node
  r->sourceRoot = src.findRootNode();
  if(r->sourceRoot==None)
    error("skeleton "+quote(spec.source)+" has no root node");
  r->bone.resize(count);
  for(size_t i=0; i<count; ++i) {
    auto& b = r->bone[i];
    b.parent = rig.nodes[i].parent;
    b.local  = (b.parent!=None ? inverse(rig.tr[b.parent]) : inverse(src.tr[r->sourceRoot]))*rig.tr[i];
    }
  std::vector<uint8_t> listed(count,0);
  for(const auto& sb:spec.bones) {
    const size_t i = rigNode(sb.name,"bone");
    if(listed[i])
      error("bone "+quote(sb.name)+" is listed twice");
    listed[i] = 1;
    if(sb.source.empty())
      continue;
    auto& b = r->bone[i];
    b.source = sourceNode(sb.source);
    b.turn   = IkMath::mul(IkMath::mul(IkMath::transpose(IkMath::rotationOf(src.tr[b.source])),IkMath::toMat3(sb.align)),
                           linear(rig.tr[i]));
    }

  r->rootBone   = rigNode(spec.rootBone,"root bone");
  r->rootSource = sourceNode(spec.rootSource);
  r->rootLocal  = apply(inverse(src.tr[r->rootSource]),IkMath::position(rig.tr[r->rootBone]));
  r->lift       = spec.rootLift;
  r->knees      = spec.rootKnees;
  r->heel       = spec.rootHeel;
  if(r->knees && r->bone[r->rootBone].source==None)
    error("root: knees need the root bone with a source");
  if(r->knees && spec.legs.size()>MaxLegs)
    error("root: knees take at most "+std::to_string(MaxLegs)+" legs");

  auto take = [&](size_t i, Role role, size_t part) {
    auto& b = r->bone[i];
    if(b.role!=Role::Plain)
      error("bone "+quote(rig.nodes[i].name)+" is in two legs or fingers");
    b.role = role;
    b.part = part;
    };
  for(const auto& l:spec.legs) {
    const std::string where = "legs["+std::to_string(r->legs.size())+"]: ";
    Leg leg;
    leg.thigh = rigNode(l.thigh,"bone");
    leg.calf  = rigNode(l.calf,"bone");
    leg.foot  = rigNode(l.foot,"bone");
    const size_t ball = rigNode(l.ball,"bone");
    if(r->bone[leg.calf].parent!=leg.thigh || r->bone[leg.foot].parent!=leg.calf || r->bone[ball].parent!=leg.foot)
      error(where+"not a chain "+l.thigh+" -> "+l.calf+" -> "+l.foot+" -> "+l.ball);
    if(IkMath::length(IkMath::position(r->bone[leg.calf].local))<IkMath::Eps ||
       IkMath::length(IkMath::position(r->bone[leg.foot].local))<IkMath::Eps)
      error(where+"a bone of zero length");
    if(r->rootBone==leg.calf || r->rootBone==leg.foot)
      error(where+"the root bone is below the thigh");
    leg.sourceFoot = sourceNode(l.sourceFoot);
    leg.sourceKnee = sourceNode(l.sourceKnee);
    leg.ankle      = l.ankle;
    leg.front      = IkMath::mul(IkMath::transpose(IkMath::rotationOf(rig.tr[leg.thigh])),Vec3(0,0,1));
    leg.soft       = l.soft;
    leg.length     = IkMath::length(IkMath::position(r->bone[leg.calf].local)) +
                     IkMath::length(IkMath::position(r->bone[leg.foot].local));
    leg.ball       = ball;
    leg.ballFoot   = apply(inverse(src.tr[leg.sourceFoot]),IkMath::position(rig.tr[ball]));
    leg.footBind   = IkMath::transpose(IkMath::rotationOf(src.tr[leg.sourceFoot]));
    leg.heelBack   = IkMath::position(rig.tr[leg.foot]).z - IkMath::position(src.tr[leg.sourceFoot]).z;
    leg.under      = IkMath::position(rig.tr[ball]).y - IkMath::position(src.tr[leg.sourceFoot]).y;
    if(r->knees) {
      if(r->bone[leg.thigh].parent!=r->rootBone)
        error(where+"knees need the thigh "+quote(l.thigh)+" right under the root bone");
      if(r->bone[leg.thigh].source==None || r->bone[leg.calf].source==None || r->bone[leg.foot].source==None)
        error(where+"knees need the thigh, the calf and the foot with a source");
      }
    take(leg.thigh,Role::Thigh,r->legs.size());
    take(leg.calf, Role::Calf, r->legs.size());
    take(leg.foot, Role::Foot, r->legs.size());
    r->legs.push_back(leg);
    }
  for(const auto& f:spec.fingers) {
    Fingers fs;
    fs.hand   = sourceNode(f.sourceHand);
    fs.finger = sourceNode(f.sourceFinger);
    fs.bind   = IkMath::mul(IkMath::transpose(IkMath::rotationOf(src.tr[fs.finger])),IkMath::rotationOf(src.tr[fs.hand]));
    for(const auto& chain:f.chains)
      for(size_t j=0; j<chain.size(); ++j) {
        const size_t i = rigNode(chain[j],"bone");
        take(i,Role::Phalanx,r->fingers.size());
        r->bone[i].share = f.shares[j];
        }
    r->fingers.push_back(fs);
    }

  auto bindAt = [&](size_t i) { return IkMath::position(rig.tr[i]); };
  auto inFrame = [&](size_t i, const Vec3& dir) {  // a world direction of the bind pose in bone i's frame
    return IkMath::mul(IkMath::transpose(IkMath::rotationOf(rig.tr[i])),dir);
    };
  for(const auto& t:spec.twists) {
    const std::string where = "twists["+std::to_string(r->twists.size())+"]: ";
    Twist tw;
    tw.bone    = rigNode(t.bone,"bone");
    tw.of      = rigNode(t.of,"bone");
    tw.against = rigNode(t.against,"bone");
    tw.parent  = rig.nodes[tw.bone].parent;
    const size_t axis = rigNode(t.axis,"bone");
    if(tw.parent==None)
      error(where+"the twist bone "+quote(t.bone)+" has no parent");
    if(r->bone[tw.bone].source!=None)
      error(where+"the twist bone "+quote(t.bone)+" has a source");
    for(size_t i=0; i<count; ++i)
      if(rig.nodes[i].parent==tw.bone)
        error(where+"the twist bone "+quote(t.bone)+" has children");
    if(tw.of==tw.bone || tw.against==tw.bone || tw.of==tw.against)
      error(where+"of, against and the bone must differ");
    const Vec3 dir = bindAt(axis)-bindAt(tw.parent);
    if(IkMath::length(dir)<IkMath::Eps)
      error(where+"the axis "+quote(t.axis)+" sits on the parent");
    tw.axisAgainst = inFrame(tw.against,IkMath::normalize(dir));
    tw.axisParent  = inFrame(tw.parent,IkMath::normalize(dir));
    tw.bindRel     = IkMath::mul(IkMath::transpose(IkMath::rotationOf(rig.tr[tw.against])),IkMath::rotationOf(rig.tr[tw.of]));
    tw.share       = t.share;
    take(tw.bone,Role::Twist,r->twists.size());
    r->twists.push_back(tw);
    }
  for(const auto& t:r->twists)
    if(r->bone[t.of].role==Role::Twist || r->bone[t.against].role==Role::Twist)
      error("twists: a twist bone cannot be another one's of or against");

  for(const auto& sh:spec.shoulders) {
    const std::string where = "shoulders["+std::to_string(r->shoulders.size())+"]: ";
    Shoulder rh;
    rh.clavicle = rigNode(sh.clavicle,"bone");
    rh.arm      = rigNode(sh.arm,"bone");
    rh.chest    = rigNode(sh.chest,"bone");
    const size_t elbow = rigNode(sh.elbow,"bone");
    if(r->bone[rh.arm].source==None)
      error(where+"the arm "+quote(sh.arm)+" has no source");
    bool above = false;
    for(size_t p=rig.nodes[rh.clavicle].parent; p!=None && !above; p=rig.nodes[p].parent)
      above = p==rh.chest;
    if(!above)
      error(where+"the chest "+quote(sh.chest)+" is not above the clavicle "+quote(sh.clavicle));
    if(r->bone[rh.clavicle].shoulder!=None)
      error(where+"the clavicle "+quote(sh.clavicle)+" has two rhythms");
    const Vec3 clav = bindAt(rh.arm)-bindAt(rh.clavicle), arm = bindAt(elbow)-bindAt(rh.arm);
    if(IkMath::length(clav)<IkMath::Eps || IkMath::length(arm)<IkMath::Eps)
      error(where+"a bone of zero length");
    rh.clavAxis  = inFrame(rh.clavicle,IkMath::normalize(clav));
    rh.armAxis   = inFrame(rh.arm,IkMath::normalize(arm));
    rh.chestDown = inFrame(rh.chest,Vec3(0,-1,0));
    rh.share     = sh.share;
    rh.from      = sh.from*Degree;
    rh.max       = sh.max*Degree;
    r->bone[rh.clavicle].shoulder = r->shoulders.size();
    r->shoulders.push_back(rh);
    }

  {
    // the arms that hold another hand go after the ones they hold
    std::vector<Arm>         held;
    std::vector<std::string> holdsName;
    for(const auto& a:spec.arms) {
      const std::string where = "arms["+std::to_string(r->arms.size()+held.size())+"]: ";
      Arm arm;
      arm.upper      = rigNode(a.upper,"bone");
      arm.lower      = rigNode(a.lower,"bone");
      arm.hand       = rigNode(a.hand,"bone");
      arm.sourceHand = sourceNode(a.sourceHand);
      if(r->bone[arm.lower].parent!=arm.upper || r->bone[arm.hand].parent!=arm.lower)
        error(where+"not a chain "+a.upper+" -> "+a.lower+" -> "+a.hand);
      for(const size_t i:{arm.upper,arm.lower,arm.hand})
        if(r->bone[i].source==None)
          error(where+"the bone "+quote(rig.nodes[i].name)+" has no source");
      if(IkMath::length(IkMath::position(r->bone[arm.lower].local))<IkMath::Eps ||
         IkMath::length(IkMath::position(r->bone[arm.hand].local))<IkMath::Eps)
        error(where+"a bone of zero length");
      arm.back   = inFrame(arm.upper,Vec3(0,0,-1));
      arm.soft   = a.soft;
      arm.length = IkMath::length(IkMath::position(r->bone[arm.lower].local)) +
                   IkMath::length(IkMath::position(r->bone[arm.hand].local));
      const size_t part = r->arms.size()+held.size();
      take(arm.upper,Role::ArmUpper,part);
      take(arm.lower,Role::ArmLower,part);
      take(arm.hand, Role::ArmHand, part);
      if(a.holds.empty()) {
        r->arms.push_back(arm);
        } else {
        held.push_back(arm);
        holdsName.push_back(a.holds);
        }
      }
    const size_t free = r->arms.size();
    for(size_t n=0; n<held.size(); ++n) {
      const size_t hand = rigNode(holdsName[n],"held hand");
      for(size_t m=0; m<free; ++m)
        if(r->arms[m].hand==hand)
          held[n].holds = m;
      if(held[n].holds==None)
        error("arms: "+quote(holdsName[n])+" is not the hand of an arm that holds nothing");
      r->arms.push_back(held[n]);
      }
    for(size_t m=0; m<r->arms.size(); ++m)
      for(const size_t i:{r->arms[m].upper,r->arms[m].lower,r->arms[m].hand})
        r->bone[i].part = m;
    // the bones below an arm's forearm wait for the arm (the fingers); the twists come last anyway
    for(const size_t i:r->order) {
      const Role role = r->bone[i].role;
      if(role==Role::ArmLower || role==Role::ArmHand || role==Role::Twist)
        continue;
      bool below = false;
      for(size_t p=r->bone[i].parent; p!=None && !below; p=r->bone[p].parent)
        below = r->bone[p].role==Role::ArmLower || r->bone[p].role==Role::ArmHand;
      if(below) {
        r->bone[i].afterArms = true;
        r->afterArms.push_back(i);
        }
      }
  }

  for(const auto& s:spec.sockets) {
    Socket socket;
    socket.name   = s.source;
    socket.source = sourceNode(s.source);
    socket.bone   = rigNode(s.bone,"bone");
    socket.offset = s.offset;
    r->sockets.push_back(std::move(socket));
    }

  // the source matrices solve() reads
  auto need = [&](size_t i) { r->sourceNeeded = std::max(r->sourceNeeded,i+1); };
  need(r->sourceRoot);
  need(r->rootSource);
  for(const auto& b:r->bone)
    if(b.source!=None)
      need(b.source);
  for(const auto& l:r->legs) {
    need(l.sourceFoot);
    need(l.sourceKnee);
    }
  for(const auto& f:r->fingers) {
    need(f.hand);
    need(f.finger);
    }
  for(const auto& a:r->arms)
    need(a.sourceHand);
  return r;
  }

bool RenderRig::fits(const Skeleton& source) const {
  if(source.nodes.size()!=sourceNames.size())
    return false;
  for(size_t i=0; i<sourceNames.size(); ++i)
    if(source.nodes[i].name!=sourceNames[i])
      return false;
  return true;
  }

bool RenderRig::solve(const Matrix4x4* source, size_t sourceCount, Matrix4x4* out, const Hands& hands, Gait* gait,
                      float dt) const {
  if(source==nullptr || out==nullptr || sourceCount<sourceNeeded)
    return false;
  float rise = 0;
  float carry[MaxLegs] = {}, plant[MaxLegs] = {};
  float lowBall = 0, scale = 1;
  if(knees && !legs.empty()) {
    rise = heel ? heelLift(source,carry,plant,lowBall,scale,gait,dt) : kneeLift(source,carry,lowBall,scale);
    }
  else if(lift>0 && !legs.empty()) {
    const float root = apply(source[rootSource],rootLocal).y;
    float       low  = root;
    for(const auto& l:legs)
      low = std::min(low,apply(source[l.sourceFoot],l.ankle).y);
    rise = lift*(root-low);
    }
  for(const size_t i:order) {
    const Role role = bone[i].role;
    if(role==Role::Calf || role==Role::Foot || role==Role::Twist || role==Role::ArmLower || role==Role::ArmHand ||
       bone[i].afterArms)
      continue;  // with their thigh or arm; the twists last
    out[i] = place(i,source,out,rise);
    if(role==Role::Thigh) {
      const size_t n = bone[i].part;
      solveLeg(legs[n],source,out,knees && n<MaxLegs ? carry[n] : 0.f,lowBall,scale,n<MaxLegs ? plant[n] : 0.f);
      }
    }
  for(const auto& a:arms)
    solveArm(a,source,out,hands);
  for(const size_t i:afterArms)
    out[i] = place(i,source,out,rise);
  for(const auto& t:twists)
    solveTwist(t,out);
  return true;
  }

// A bone by its parent: the place from the parent as in the bind pose, the turn of its source node from the bind
// pose (R_source(t) * R_source_bind^T * align * R_bind) or the parent's; the root by its source node, raised by
// `lift` cm; a phalanx turned further by its share of the finger's bend.
Matrix4x4 RenderRig::place(size_t i, const Matrix4x4* source, const Matrix4x4* out, float lift) const {
  const Bone& b = bone[i];
  Matrix4x4 m = (b.parent!=None ? out[b.parent] : source[sourceRoot])*b.local;
  if(b.source!=None)
    setLinear(m,IkMath::mul(linear(source[b.source]),b.turn));
  if(b.shoulder!=None)
    setLinear(m,IkMath::mul(rhythm(shoulders[b.shoulder],m,source,out),linear(m)));
  if(i==rootBone)
    setPosition(m,apply(source[rootSource],rootLocal)+Vec3(0,lift,0));
  if(b.role==Role::Phalanx) {
    const auto& f = fingers[b.part];
    // the finger's turn against the hand from the bind pose, in world: R_f(t) * R_f_bind^T * R_h_bind * R_h(t)^T
    const Mat3 bend = IkMath::mul(IkMath::mul(IkMath::rotationOf(source[f.finger]),f.bind),
                                  IkMath::transpose(IkMath::rotationOf(source[f.hand])));
    Vec3        axis;
    const float angle = turnOf(bend,axis);
    if(angle>0)
      setLinear(m,IkMath::mul(IkMath::axisAngle(axis,angle*b.share),linear(m)));
    }
  return m;
  }

// The lowest of the foot's two ends on Gothic's foot: our ball and, with heel, the point under Gothic's ankle.
float RenderRig::footLow(const Leg& leg, const Matrix4x4* source, float scale) const {
  const Matrix4x4& mf   = source[leg.sourceFoot];
  const float      ball = apply(mf,leg.ballFoot).y;
  if(!heel)
    return ball;
  const Mat3 rot = IkMath::mul(IkMath::rotationOf(mf),leg.footBind);
  return std::min(ball,(IkMath::position(mf) + IkMath::mul(rot,Vec3(0,leg.under*scale,0))).y);
  }

// knees: a foot on the ground pivots about Gothic's ball. The axis is the source foot's side turned as the foot is
// (+ lifts the heel); the heel may go up by PitchMax beyond Gothic's own lift, or down to Gothic's flat foot.
// heel: the pivot goes back from Gothic's ball by the share `plant` of the way to its heel - along the ground with the
// heel up (our foot rolls over its own ball), along the foot with the toes up (it turns about Gothic's ankle, without
// a heel lift of its own); a heel beyond RollFrom..RollTo takes a standing foot off the ground.
RenderRig::Pivot RenderRig::pivot(const Leg& leg, const Matrix4x4* source, float lowBall, float scale,
                                  float plant) const {
  const Matrix4x4& mf = source[leg.sourceFoot];
  const Bone&      fb = bone[leg.foot];
  const Mat3 rot = IkMath::mul(IkMath::rotationOf(mf),leg.footBind);  // the foot's turn from its bind pose
  const Vec3 fwd = IkMath::mul(rot,Vec3(0,0,1));
  Pivot p;
  p.ball = apply(mf,leg.ballFoot);
  float off = 1, toes = 0;
  if(heel) {
    Vec3 way = fwd;
    if(fwd.y<0) {
      const Vec3  flat(fwd.x,0,fwd.z);
      const float n = IkMath::length(flat);
      const float s = std::clamp((n-0.2f)/0.3f,0.f,1.f);  // a foot on end has no way along the ground
      way = fwd*(1.f-s) + flat*(s/std::max(n,IkMath::Eps));
      off = std::clamp((n-RollTo)/(RollFrom-RollTo),0.f,1.f);
      } else {
      toes = std::asin(std::clamp(fwd.y,-1.f,1.f));
      }
    p.ball = p.ball - way*(leg.heelBack*scale*plant);
    off    = 1.f - plant*(1.f-off);                       // a heel up on a leg that does not stand: kneeling
    }
  // our ankle from our ball, the foot turned as Gothic's: the ball lands on the pivot exactly
  p.r    = IkMath::mul(IkMath::mul(linear(source[fb.source]),fb.turn),IkMath::position(bone[leg.ball].local))*-1.f;
  p.contact = off*std::clamp(1.f - (footLow(leg,source,scale) - lowBall - ContactNear*scale)/
                                   ((ContactFar-ContactNear)*scale),0.f,1.f);
  p.k = IkMath::normalize(IkMath::mul(rot,Vec3(1,0,0)));
  if(IkMath::mul(IkMath::axisAngle(p.k,0.01f),p.r).y<p.r.y)
    p.k = p.k*-1.f;
  const float lifted = std::max(0.f,std::asin(std::clamp(-fwd.y,-1.f,1.f)));  // Gothic's own heel lift
  const float own    = PitchMax*std::clamp(1.f - (toes-ToesFrom)/(ToesTo-ToesFrom),0.f,1.f);
  p.lo = -lifted;
  p.hi = own - lifted;
  if(heel && lifted>0) {
    // Gothic's ankle rises on a longer foot: ours turns further for the same rise, as far as it may
    const float lever = IkMath::length(apply(mf,leg.ballFoot)-IkMath::position(mf))/
                        std::max(IkMath::length(p.r),IkMath::Eps);
    const float want  = std::asin(std::min(1.f,lever*std::sin(lifted))) - lifted;
    p.match = std::clamp(want,0.f,std::max(p.hi,0.f));
    }
  return p;
  }

// knees: how far the root goes up (world +y) for the knees to bend as Gothic's. A preferred shift: each leg's shift at
// which its hip-ankle distance is our leg's at Gothic's knee angle (its reach), weighted by how low its ankle is and
// how much it stands under the hip; a weak total shrinks it towards 0 (lying, sitting). Each leg on the ground then
// allows a band of shifts - its foot turning about the ball through its range - and the root takes the preferred
// shift moved into the bands' overlap, or the middle of the gap. carry[n]: how much leg n is on the ground and
// carries; lowBall: the lowest ball; scale: the npc's size against the rig's bind.
float RenderRig::kneeLift(const Matrix4x4* source, float* carry, float& lowBall, float& scale) const {
  using IkMath::length;
  const Bone& root = bone[rootBone];
  const Mat3  rp   = IkMath::mul(linear(source[root.source]),root.turn);
  const Vec3  pos  = apply(source[rootSource],rootLocal);
  Vec3  hip[MaxLegs], ankle[MaxLegs];
  float reach[MaxLegs] = {};
  float lowAnkle = std::numeric_limits<float>::max();
  lowBall = lowAnkle;
  scale   = 1;
  for(size_t n=0; n<legs.size(); ++n) {
    const Leg& l  = legs[n];
    const Bone& t = bone[l.thigh], &c = bone[l.calf];
    hip[n] = pos + IkMath::mul(rp,IkMath::position(t.local));
    const Vec3 a1 = IkMath::mul(IkMath::mul(linear(source[t.source]),t.turn),IkMath::position(c.local));
    const Vec3 a2 = IkMath::mul(IkMath::mul(linear(source[c.source]),c.turn),IkMath::position(bone[l.foot].local));
    reach[n] = length(a1+a2);
    if(n==0 && l.length>IkMath::Eps)
      scale = (length(a1)+length(a2))/l.length;
    lowBall  = std::min(lowBall,apply(source[l.sourceFoot],l.ballFoot).y);
    }
  for(size_t n=0; n<legs.size(); ++n) {
    const Pivot p = pivot(legs[n],source,lowBall,scale,0);
    ankle[n] = p.ball+p.r;
    lowAnkle = std::min(lowAnkle,ankle[n].y);
    }
  auto stands = [&](const Vec3& v) {  // the leg's line points down: it carries the body
    const float down = -v.y/std::max(length(v),IkMath::Eps);
    return std::clamp((down-CarryFrom)/(CarryTo-CarryFrom),0.f,1.f);
    };
  // the shift that puts the hip at `reach` from a point v below it (v = point - hip), the hip above; false - none
  auto shiftTo = [](const Vec3& v, float reach, float& dy) {
    const float h2 = reach*reach - v.x*v.x - v.z*v.z;
    if(h2<0)
      return false;
    dy = v.y + std::sqrt(h2);
    return true;
    };

  float num = 0, den = 0;
  for(size_t n=0; n<legs.size(); ++n) {
    const Vec3  v    = ankle[n]-hip[n];
    const float disc = v.y*v.y - IkMath::dot(v,v) + reach[n]*reach[n];
    if(disc<0)
      continue;
    const float r  = std::sqrt(disc);
    const float dy = std::abs(v.y-r)<std::abs(v.y+r) ? v.y-r : v.y+r;
    const float w  = std::exp(-(ankle[n].y-lowAnkle)/(Sigma*scale))*stands(v);
    num += w*dy;
    den += w;
    }
  const float prefer = num/std::max(den,1.f);

  float lo = -std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::max();
  for(size_t n=0; n<legs.size(); ++n) {
    const Pivot p = pivot(legs[n],source,lowBall,scale,0);
    carry[n] = p.contact*stands(ankle[n]-hip[n]);
    if(carry[n]<=0)
      continue;
    float bandLo = std::numeric_limits<float>::max(), bandHi = -bandLo;
    for(int j=0; j<BandSteps; ++j) {
      const float th = p.lo + (p.hi-p.lo)*float(j)/float(BandSteps-1);
      float dy = 0;
      if(shiftTo(p.ball + IkMath::mul(IkMath::axisAngle(p.k,th),p.r) - hip[n],reach[n],dy)) {
        bandLo = std::min(bandLo,dy);
        bandHi = std::max(bandHi,dy);
        }
      }
    if(bandLo>bandHi)
      continue;
    const float give = (1.f-carry[n])*Slack*scale;
    lo = std::max(lo,bandLo-give);
    hi = std::min(hi,bandHi+give);
    }
  const float dy = lo<=hi ? std::clamp(prefer,lo,hi) : 0.5f*(lo+hi);
  return std::clamp(dy,-MaxShift*scale,MaxShift*scale);
  }

// heel: the root's lift over solves. Each leg near the ground and under the hip asks for the lift at which it reaches
// its foot with Gothic's knee, the foot turned up as far as Gothic's longer foot lifts the ankle (Pivot::match); the
// legs are weighed by how low the ankle is, how much the leg stands and how much the foot is on the ground. With no
// leg on the ground the last lift holds while a leg points down (a jump, the flight of a run) and goes to 0 otherwise
// (lying). The lift follows its goal over LiftTau and is never under what a carrying leg needs with its foot flat: a
// pelvis too low bends a straight knee by 17 degrees a cm, one too high only lifts a foot off the ground (solveLeg).
// plant[n]: the leg's share of standing heel on heel - it points down, its thigh and its shin do; it follows the
// pose over PlantTau and only while the foot is near the ground, so a lunge that levels the thigh for a moment does
// not drag the foot and a leg swung through the air comes down as it left. Without a valid gait: the goal and the
// pose's shares at once.
float RenderRig::heelLift(const Matrix4x4* source, float* carry, float* plant, float& lowFoot, float& scale,
                          Gait* gait, float dt) const {
  using IkMath::length;
  const Bone& root = bone[rootBone];
  const Mat3  rp   = IkMath::mul(linear(source[root.source]),root.turn);
  const Vec3  pos  = apply(source[rootSource],rootLocal);
  const bool  kept = gait!=nullptr && gait->valid;
  const float step = std::max(dt,0.f);
  Vec3  hip[MaxLegs];
  float reach[MaxLegs] = {}, low[MaxLegs] = {};
  lowFoot = std::numeric_limits<float>::max();
  scale   = 1;
  for(size_t n=0; n<legs.size(); ++n) {
    const Leg& l  = legs[n];
    const Bone& t = bone[l.thigh], &c = bone[l.calf];
    hip[n] = pos + IkMath::mul(rp,IkMath::position(t.local));
    const Vec3 a1 = IkMath::mul(IkMath::mul(linear(source[t.source]),t.turn),IkMath::position(c.local));
    const Vec3 a2 = IkMath::mul(IkMath::mul(linear(source[c.source]),c.turn),IkMath::position(bone[l.foot].local));
    reach[n] = length(a1+a2);
    if(n==0 && l.length>IkMath::Eps)
      scale = (length(a1)+length(a2))/l.length;
    low[n]  = footLow(l,source,scale);
    lowFoot = std::min(lowFoot,low[n]);
    }
  auto down = [](const Vec3& v, float from, float to) {  // how much v points down, from..to -> 0..1
    return std::clamp((-v.y/std::max(length(v),IkMath::Eps)-from)/(to-from),0.f,1.f);
    };
  for(size_t n=0; n<legs.size(); ++n) {
    const Leg& l    = legs[n];
    const Vec3 hipG = IkMath::position(source[bone[l.thigh].source]);
    const Vec3 knee = IkMath::position(source[bone[l.calf].source]);
    const Vec3 foot = IkMath::position(source[l.sourceFoot]);
    float share = std::min({down(foot-hipG,CarryFrom,CarryTo),down(knee-hipG,ThighFrom,ThighTo),
                            down(foot-knee,ShinFrom,ShinTo)});
    if(kept) {
      const float near = std::clamp(1.f - (low[n]-lowFoot-ContactNear*scale)/((ContactFar-ContactNear)*scale),0.f,1.f);
      share = gait->plant[n] + (share-gait->plant[n])*(1.f-std::exp(-step/PlantTau))*near;
      }
    plant[n] = share;
    }

  // the shift that puts the hip at `reach` from a point v below it (v = point - hip), the hip above; false - none
  auto shiftTo = [](const Vec3& v, float reach, float& dy) {
    const float h2 = reach*reach - v.x*v.x - v.z*v.z;
    if(h2<0)
      return false;
    dy = v.y + std::sqrt(h2);
    return true;
    };
  float ankleY[MaxLegs] = {}, wish[MaxLegs] = {}, stand[MaxLegs] = {}, bandLo[MaxLegs] = {};
  bool  wishes[MaxLegs] = {}, bands[MaxLegs] = {};
  float lowAnkle = std::numeric_limits<float>::max(), standing = 0;
  for(size_t n=0; n<legs.size(); ++n) {
    const Pivot p = pivot(legs[n],source,lowFoot,scale,plant[n]);
    const Vec3  v = p.ball + IkMath::mul(IkMath::axisAngle(p.k,p.match),p.r) - hip[n];
    ankleY[n] = v.y + hip[n].y;
    lowAnkle  = std::min(lowAnkle,ankleY[n]);
    stand[n]  = down(v,CarryFrom,CarryTo);
    standing  = std::max(standing,stand[n]);
    carry[n]  = p.contact*stand[n];
    wishes[n] = shiftTo(v,reach[n],wish[n]);
    bandLo[n] = std::numeric_limits<float>::max();
    for(int j=0; j<BandSteps; ++j) {
      const float th = p.lo + (p.hi-p.lo)*float(j)/float(BandSteps-1);
      float dy = 0;
      if(shiftTo(p.ball + IkMath::mul(IkMath::axisAngle(p.k,th),p.r) - hip[n],reach[n],dy)) {
        bandLo[n] = std::min(bandLo[n],dy);
        bands[n]  = true;
        }
      }
    }
  float num = 0, den = 0;
  for(size_t n=0; n<legs.size(); ++n) {
    if(!wishes[n])
      continue;
    const float w = std::exp(-(ankleY[n]-lowAnkle)/(Sigma*scale))*carry[n];
    num += w*wish[n];
    den += w;
    }
  const float trust = std::min(den,1.f);
  float       goal  = trust*num/std::max(den,1e-6f);
  float       dy    = goal;
  if(kept) {
    goal += (1.f-trust)*standing*gait->lift;
    dy    = gait->lift + (goal-gait->lift)*(1.f-std::exp(-step/LiftTau));
    }
  for(size_t n=0; n<legs.size(); ++n)
    if(carry[n]>0 && bands[n])
      dy = std::max(dy,bandLo[n]-(1.f-carry[n])*Slack*scale);
  dy = std::clamp(dy,-MaxShift*scale,MaxShift*scale);
  if(gait!=nullptr) {
    gait->lift = dy;
    for(size_t n=0; n<MaxLegs; ++n)
      gait->plant[n] = n<legs.size() ? plant[n] : 0.f;
    gait->valid = true;
    }
  return dy;
  }

// Two-bone IK onto the ankle of the source foot, the knee towards the thigh's front. The axis of a bone is the way to
// its child in the bind pose; the thigh and the calf take the shortest turn onto the solved directions, the foot keeps
// its turn and stands on the target. A target out of reach: the leg straight towards it. With soft, the last soft cm
// of reach come as 1 - e^-x: the foot stops short of a far target and the knee never snaps straight.
// knees: on the ground the ankle goes where the foot, turned about Gothic's ball by the share `carry` of what our
// knee needs, puts it; in the air the leg's own Gothic turns decide (FK), the knee to Gothic's side.
void RenderRig::solveLeg(const Leg& leg, const Matrix4x4* source, Matrix4x4* out, float carry, float lowBall,
                         float scale, float plant) const {
  using IkMath::length;
  using IkMath::normalize;
  Matrix4x4& thigh = out[leg.thigh];
  const Vec3 hip    = IkMath::position(thigh);
  const Vec3 toCalf = IkMath::position(bone[leg.calf].local);
  const Vec3 toFoot = IkMath::position(bone[leg.foot].local);
  const Vec3 a1     = IkMath::mul(linear(thigh),toCalf);
  out[leg.calf] = place(leg.calf,source,out);
  const Vec3 a2     = IkMath::mul(linear(out[leg.calf]),toFoot);
  const float l1 = length(a1), l2 = length(a2);
  Vec3  target = apply(source[leg.sourceFoot],leg.ankle);
  float ground = 1;                 // knees: the foot on the ground (pivot) against in the air (FK)
  Mat3  footTurn = {{{1,0,0},{0,1,0},{0,0,1}}};
  if(knees) {
    const Pivot p = pivot(leg,source,lowBall,scale,plant);
    // the turn about the ball that puts the ankle at our reach from the hip: A cos + B sin = C on the circle
    const float reach = length(a1+a2);
    const Vec3  rpar  = p.k*IkMath::dot(p.k,p.r), rperp = p.r-rpar;
    const Vec3  q     = p.ball - hip + rpar;
    const float A = 2.f*IkMath::dot(q,rperp), B = 2.f*IkMath::dot(q,IkMath::cross(p.k,rperp));
    const float C = reach*reach - IkMath::dot(q,q) - IkMath::dot(rperp,rperp);
    const float R = std::sqrt(A*A + B*B), base = std::atan2(B,A);
    float th = 0;
    if(R>IkMath::Eps) {
      if(std::abs(C)<=R) {
        const float t1 = std::remainder(base+std::acos(C/R),2.f*Pi), t2 = std::remainder(base-std::acos(C/R),2.f*Pi);
        th = std::abs(t1)<std::abs(t2) ? t1 : t2;
        } else {
        th = std::remainder(C>R ? base : base+Pi,2.f*Pi);
        }
      }
    th = std::clamp(th,p.lo,p.hi)*carry;  // a foot that carries nothing stays as Gothic's
    ground   = p.contact;
    target   = (p.ball + IkMath::mul(IkMath::axisAngle(p.k,th),p.r))*ground + (hip+a1+a2)*(1.f-ground);
    footTurn = IkMath::axisAngle(p.k,ground*th);
    }
  const Vec3  toTarget = target - hip;
  const float dist     = length(toTarget);
  if(l1<IkMath::Eps || l2<IkMath::Eps || dist<IkMath::Eps) {
    out[leg.foot] = place(leg.foot,source,out);  // nowhere to bend to: the leg as it stands
    return;
    }

  // the triangle in double: a straight leg cancels most of l1^2 - along^2
  const Vec3   u     = toTarget*(1.f/dist);
  const double d1 = l1, d2 = l2;
  double       d     = std::clamp(double(dist),std::abs(d1-d2)+1e-6,d1+d2);
  double       far   = dist;
  if(knees && heel) {
    // no straighter than Gothic's knee: the foot stops short of a far target (a pelvis higher than the leg wants)
    far = std::min(far,std::max(double(length(a1+a2)),std::abs(d1-d2)+1e-6));
    d   = std::min(d,far);
    }
  if(leg.soft>0 && leg.length>IkMath::Eps) {
    const double s    = double(leg.soft)*(d1+d2)/double(leg.length);  // the npc's scale too
    const double near = d1+d2-s;
    if(far>near && near>std::abs(d1-d2))
      d = near + s*(1.0-std::exp(-(far-near)/s));
    }
  const double along = (d1*d1 - d2*d2 + d*d)/(2.0*d);
  const double up    = std::sqrt(std::max(d1*d1 - along*along,0.0));
  const Vec3   end   = hip + u*float(d);

  // the knee's way: the thigh's front, robust on a straight leg (Gothic's knee point lies on the line there and gave
  // a random side - the knees knocked inwards); along the leg (a thigh raised level) - towards Gothic's knee
  const Vec3 front = IkMath::mul(linear(thigh),leg.front);
  Vec3 side = front - u*IkMath::dot(front,u);
  if(length(side)<0.05f*length(front)) {
    const Vec3 pole = IkMath::position(source[leg.sourceKnee]) - hip;
    side = pole - u*IkMath::dot(pole,u);
    }
  if(length(side)<IkMath::Eps)
    side = a1 - u*IkMath::dot(a1,u);  // the pole on the line: the knee keeps its side
  if(length(side)<IkMath::Eps)
    side = IkMath::cross(u,std::abs(u.x)<0.9f ? Vec3(1,0,0) : Vec3(0,1,0));
  side = normalize(side);
  if(knees && ground<1) {             // in the air: the side Gothic's knee bends to
    const Vec3 own = a1 - u*IkMath::dot(a1,u);
    // heel: Gothic's knee bent backwards (its chain straighter than its bind) - ours still bends to the front
    if(length(own)>0.05f*l1 && !(heel && IkMath::dot(own,side)<0))
      side = normalize(side*ground + normalize(own)*(1.f-ground));
    }
  const Vec3 knee = hip + u*float(along) + side*float(up);

  setLinear(thigh,IkMath::mul(IkMath::rotationBetween(normalize(a1),normalize(knee-hip)),linear(thigh)));
  Matrix4x4 calf = place(leg.calf,source,out);  // without a source it follows the turned thigh
  setPosition(calf,knee);
  const Vec3 axis = IkMath::mul(linear(calf),toFoot);
  setLinear(calf,IkMath::mul(IkMath::rotationBetween(normalize(axis),normalize(end-knee)),linear(calf)));
  out[leg.calf] = calf;
  Matrix4x4 foot = place(leg.foot,source,out);
  if(knees)
    setLinear(foot,IkMath::mul(footTurn,linear(foot)));
  setPosition(foot,end);
  out[leg.foot] = foot;
  }

// Two-bone IK of an arm, as the leg's: the elbow to the side Gothic's arm bends it, a little back so a straight arm
// has one. The wrist: where the arm's own Gothic turns put it, moved by `hold` to where Gothic holds it against the
// held hand (that arm is solved first) and by `world` onto Gothic's wrist. Both 0: the arm as the other bones.
void RenderRig::solveArm(const Arm& a, const Matrix4x4* source, Matrix4x4* out, const Hands& hands) const {
  using IkMath::length;
  using IkMath::normalize;
  out[a.lower] = place(a.lower,source,out);
  const float hold  = a.holds!=None ? std::clamp(hands.hold,0.f,1.f) : 0.f;
  const float world = std::clamp(hands.world,0.f,1.f);
  if(!(hold>0) && !(world>0)) {
    out[a.hand] = place(a.hand,source,out);
    return;
    }
  Matrix4x4& upper   = out[a.upper];
  const Vec3 shoulder = IkMath::position(upper);
  const Vec3 toLower  = IkMath::position(bone[a.lower].local);
  const Vec3 toHand   = IkMath::position(bone[a.hand].local);
  const Vec3 a1 = IkMath::mul(linear(upper),toLower);
  const Vec3 a2 = IkMath::mul(linear(out[a.lower]),toHand);
  const float l1 = length(a1), l2 = length(a2);
  Vec3 target = shoulder + a1 + a2;
  if(hold>0) {
    const Arm&       m  = arms[a.holds];
    const Matrix4x4& gr = source[m.sourceHand];
    const Vec3 rel  = IkMath::mul(IkMath::transpose(IkMath::rotationOf(gr)),
                                  IkMath::position(source[a.sourceHand])-IkMath::position(gr));
    const Vec3 want = IkMath::position(out[m.hand]) +
                      IkMath::mul(IkMath::mul(IkMath::rotationOf(out[m.hand]),IkMath::transpose(bone[m.hand].turn)),rel);
    target = target + (want-target)*hold;
    }
  if(world>0)
    target = target + (IkMath::position(source[a.sourceHand])-target)*world;
  const Vec3  toTarget = target - shoulder;
  const float dist     = length(toTarget);
  if(l1<IkMath::Eps || l2<IkMath::Eps || dist<IkMath::Eps) {
    out[a.hand] = place(a.hand,source,out);
    return;
    }
  const Vec3   u  = toTarget*(1.f/dist);
  const double d1 = l1, d2 = l2;
  double       d  = std::clamp(double(dist),std::abs(d1-d2)+1e-6,d1+d2);
  if(a.soft>0 && a.length>IkMath::Eps) {
    const double s    = double(a.soft)*(d1+d2)/double(a.length);
    const double near = d1+d2-s;
    if(dist>near && near>std::abs(d1-d2))
      d = near + s*(1.0-std::exp(-(double(dist)-near)/s));
    }
  const double along = (d1*d1 - d2*d2 + d*d)/(2.0*d);
  const double up    = std::sqrt(std::max(d1*d1 - along*along,0.0));
  const Vec3   end   = shoulder + u*float(d);

  Vec3 side = a1 - u*IkMath::dot(a1,u);
  Vec3 back = IkMath::mul(linear(upper),a.back);
  back = back - u*IkMath::dot(back,u);
  if(length(back)>IkMath::Eps)
    side = side + normalize(back)*(0.05f*l1);
  if(length(side)<IkMath::Eps)
    side = IkMath::cross(u,std::abs(u.x)<0.9f ? Vec3(1,0,0) : Vec3(0,1,0));
  side = normalize(side);
  const Vec3 elbow = shoulder + u*float(along) + side*float(up);

  setLinear(upper,IkMath::mul(IkMath::rotationBetween(normalize(a1),normalize(elbow-shoulder)),linear(upper)));
  Matrix4x4 lower = place(a.lower,source,out);
  setPosition(lower,elbow);
  const Vec3 axis = IkMath::mul(linear(lower),toHand);
  setLinear(lower,IkMath::mul(IkMath::rotationBetween(normalize(axis),normalize(end-elbow)),linear(lower)));
  out[a.lower] = lower;
  Matrix4x4 hand = place(a.hand,source,out);
  setPosition(hand,end);
  out[a.hand] = hand;
  }

// The scapulohumeral rhythm: as the arm rises from the chest's down past `from`, the clavicle turns further towards
// it by share of the excess, at most `max` and never past the arm. The arm's turn comes from its source here: the
// clavicle is placed before it.
Mat3 RenderRig::rhythm(const Shoulder& s, const Matrix4x4& clavicle, const Matrix4x4* source, const Matrix4x4* out) const {
  const Mat3  none    = {{{1,0,0},{0,1,0},{0,0,1}}};
  const Bone& arm     = bone[s.arm];
  const Vec3  armDir  = IkMath::normalize(IkMath::mul(IkMath::mul(linear(source[arm.source]),arm.turn),s.armAxis));
  const Vec3  down    = IkMath::normalize(IkMath::mul(linear(out[s.chest]),s.chestDown));
  const Vec3  clavDir = IkMath::normalize(IkMath::mul(linear(clavicle),s.clavAxis));
  const float rise    = std::acos(std::clamp(IkMath::dot(armDir,down),-1.f,1.f));
  float       extra   = std::min(s.max,s.share*std::max(0.f,rise-s.from));
  const Vec3  axis    = IkMath::cross(clavDir,armDir);
  if(extra<=0 || IkMath::length(axis)<IkMath::Eps)
    return none;
  extra = std::min(extra,std::acos(std::clamp(IkMath::dot(clavDir,armDir),-1.f,1.f)));
  return IkMath::axisAngle(IkMath::normalize(axis),extra);
  }

// A twist bone: rigid behind its parent, then turned about the parent's axis by share x the twist of `of` against
// `against` from the bind pose (the twist part of a swing-twist split about the bind axis)
void RenderRig::solveTwist(const Twist& t, Matrix4x4* out) const {
  Matrix4x4  m   = out[t.parent]*bone[t.bone].local;
  const Mat3 rel = IkMath::mul(IkMath::mul(IkMath::transpose(IkMath::rotationOf(out[t.against])),
                                           IkMath::rotationOf(out[t.of])),IkMath::transpose(t.bindRel));
  IkMath::Quat q = IkMath::toQuat(rel);
  if(q.w<0)
    q = {-q.w,-q.x,-q.y,-q.z};
  const float along = q.x*t.axisAgainst.x + q.y*t.axisAgainst.y + q.z*t.axisAgainst.z;
  const float twist = 2.f*std::atan2(along,q.w);
  const Vec3  axis  = IkMath::normalize(IkMath::mul(linear(out[t.parent]),t.axisParent));
  setLinear(m,IkMath::mul(IkMath::axisAngle(axis,t.share*twist),linear(m)));
  out[t.bone] = m;
  }

bool RenderRig::socket(std::string_view sourceNode, const Matrix4x4* palette, Matrix4x4& out) const {
  for(const auto& s:sockets)
    if(s.name==sourceNode) {
      out = palette[s.bone]*s.offset;
      return true;
      }
  return false;
  }

bool RenderRig::socket(size_t sourceNode, const Matrix4x4* palette, Matrix4x4& out) const {
  for(const auto& s:sockets)
    if(s.source==sourceNode) {
      out = palette[s.bone]*s.offset;
      return true;
      }
  return false;
  }

size_t RenderRig::sourceOf(size_t i) const {
  if(i>=bone.size())
    return None;
  for(size_t depth=0; i!=None && depth<bone.size(); ++depth) {
    if(bone[i].source!=None)
      return bone[i].source;
    i = bone[i].parent;
    }
  return sourceRoot;
  }

RenderRigIndex::RenderRigIndex(Lookup lookup, Report report)
  :lookup(std::move(lookup)), report(std::move(report)) {
  }

RenderRigIndex::~RenderRigIndex() = default;

void RenderRigIndex::read(const std::filesystem::path& file) {
  specs.clear();
  meshes.clear();
  bodies.clear();
  built.clear();
  std::error_code ec;
  if(!std::filesystem::exists(file,ec)) {
    if(ec)
      report("render rigs: cannot read "+quote(file.string())+": "+ec.message());
    return;
    }
  std::ifstream in(file,std::ios::binary);
  if(!in) {
    report("render rigs: cannot read "+quote(file.string()));
    return;
    }
  const std::string text{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
  parse(text);
  }

void RenderRigIndex::parse(std::string_view json) {
  specs.clear();
  meshes.clear();
  bodies.clear();
  built.clear();
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(),json.size());
  try {
    if(doc.HasParseError())
      fail("invalid JSON at offset "+std::to_string(doc.GetErrorOffset())+": "+
           rapidjson::GetParseError_En(doc.GetParseError()));
    if(!doc.IsObject())
      fail("the file must be a JSON object");
    onlyFields(doc,{"format","version","about","rigs","meshes","bodies"},"");
    if(!doc.HasMember("format") || !doc["format"].IsString() || str(doc["format"])!="gothic-render-rigs")
      fail("format must be \"gothic-render-rigs\"");
    if(!doc.HasMember("version") || !doc["version"].IsUint() || doc["version"].GetUint()!=1)
      fail("version must be 1");
    if(!doc.HasMember("rigs") || !doc["rigs"].IsObject())
      fail("rigs must be an object");
    if(doc.HasMember("meshes") && !doc["meshes"].IsObject())
      fail("meshes must be an object: mesh -> rig");
    if(doc.HasMember("bodies") && !doc["bodies"].IsObject())
      fail("bodies must be an object: body -> {texture variant -> mesh}");
    }
  catch(const std::exception& e) {
    report(std::string("render rigs: ")+e.what());
    return;
    }

  for(const auto& m:doc["rigs"].GetObject()) {
    const std::string rigName(str(m.name));
    try {
      if(rigName.empty())
        fail("the rig needs a name");
      if(specs.count(rigName))
        fail("listed twice");
      specs.emplace(rigName,parseRig(rigName,m.value));
      }
    catch(const std::exception& e) {
      report("render rig "+rigName+": "+e.what());
      }
    }
  if(!doc.HasMember("meshes"))
    return;
  for(const auto& m:doc["meshes"].GetObject()) {
    const std::string key = meshKey(str(m.name));
    if(key.empty() || !m.value.IsString()) {
      report("render rigs: mesh "+quote(str(m.name))+": expected a mesh name and a rig name");
      continue;
      }
    if(specs.find(str(m.value))==specs.end()) {
      report("render rigs: mesh "+quote(str(m.name))+": no rig "+quote(str(m.value)));
      continue;
      }
    meshes[key] = std::string(str(m.value));
    }
  if(!doc.HasMember("bodies"))
    return;
  for(const auto& b:doc["bodies"].GetObject()) {
    const std::string body = meshKey(str(b.name));
    if(body.empty() || !b.value.IsObject()) {
      report("render rigs: body "+quote(str(b.name))+": expected a body name and its {texture variant -> mesh}");
      continue;
      }
    for(const auto& v:b.value.GetObject()) {
      const std::string_view num = str(v.name);
      int32_t variant = 0;
      const auto parsed = std::from_chars(num.data(),num.data()+num.size(),variant);
      const std::string mesh = v.value.IsString() ? meshKey(str(v.value)) : std::string();
      if(num.empty() || parsed.ec!=std::errc() || parsed.ptr!=num.data()+num.size() || variant<0) {
        report("render rigs: body "+quote(str(b.name))+": "+quote(num)+" is not a texture variant number");
        continue;
        }
      if(meshes.find(mesh)==meshes.end()) {
        report("render rigs: body "+quote(str(b.name))+", variant "+std::string(num)+
               ": the mesh must be one of \"meshes\"");
        continue;
        }
      bodies[body][variant] = mesh;
      }
    }
  }

std::vector<std::string> RenderRigIndex::rigNames() const {
  std::vector<std::string> ret;
  for(const auto& s:specs)
    ret.push_back(s.first);
  return ret;
  }

const RenderRig* RenderRigIndex::rig(std::string_view rigName) {
  const auto done = built.find(std::string(rigName));
  if(done!=built.end())
    return done->second.get();
  const auto spec = specs.find(rigName);
  if(spec==specs.end())
    return nullptr;

  std::unique_ptr<RenderRig> ret;
  try {
    const auto& s = spec->second;
    auto record = [&](const std::string& file, asset::ModelHierarchy& out) {
      bool found = false;
      try {
        found = lookup && lookup(file,out);
        }
      catch(const std::exception& e) {
        fail("render rig "+s.name+": skeleton record "+quote(file)+": "+e.what());
        }
      if(!found)
        fail("render rig "+s.name+": no skeleton record "+quote(file));
      };
    asset::ModelHierarchy rigH {}, sourceH {};
    record(s.skeleton,rigH);
    record(s.source,sourceH);
    ret = RenderRig::build(s,rigH,sourceH);
    }
  catch(const std::exception& e) {
    report(e.what());
    }
  const RenderRig* p = ret.get();
  built.emplace(std::string(rigName),std::move(ret));
  return p;
  }

const RenderRig* RenderRigIndex::rigOfMesh(std::string_view mesh) {
  const auto it = meshes.find(meshKey(mesh));
  return it==meshes.end() ? nullptr : rig(it->second);
  }

std::string RenderRigIndex::bodyOf(std::string_view body, int32_t variant) {
  const auto b = bodies.find(meshKey(body));
  if(b==bodies.end())
    return {};
  const auto v = b->second.find(variant);
  if(v==b->second.end() || rigOfMesh(v->second)==nullptr)
    return {};
  return v->second;
  }
