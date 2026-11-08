#include "renderrig.h"

#include "skeleton.h"
#include "resources.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>

using namespace Tempest;
using IkMath::Mat3;

namespace {

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
  onlyFields(v,{"skeleton","source","bones","root","legs","fingers","sockets"},"");
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
  onlyFields(v["root"],{"bone","source"},"root: ");
  s.rootBone   = name(v["root"],"bone","root: ");
  s.rootSource = name(v["root"],"source","root: ");

  if(auto legs = list(v,"legs","")) {
    for(const auto& l:legs->GetArray()) {
      const std::string where = "legs["+std::to_string(s.legs.size())+"]: ";
      if(!l.IsObject())
        fail(where+"must be an object");
      onlyFields(l,{"thigh","calf","foot","ball","source_foot","source_knee","ankle"},where);
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

bool RenderRig::solve(const Matrix4x4* source, size_t sourceCount, Matrix4x4* out) const {
  if(source==nullptr || out==nullptr || sourceCount<sourceNeeded)
    return false;
  for(const size_t i:order) {
    const Role role = bone[i].role;
    if(role==Role::Calf || role==Role::Foot)
      continue;  // with their thigh
    out[i] = place(i,source,out);
    if(role==Role::Thigh)
      solveLeg(legs[bone[i].part],source,out);
    }
  return true;
  }

// A bone by its parent: the place from the parent as in the bind pose, the turn of its source node from the bind
// pose (R_source(t) * R_source_bind^T * align * R_bind) or the parent's; the root by its source node; a phalanx
// turned further by its share of the finger's bend.
Matrix4x4 RenderRig::place(size_t i, const Matrix4x4* source, const Matrix4x4* out) const {
  const Bone& b = bone[i];
  Matrix4x4 m = (b.parent!=None ? out[b.parent] : source[sourceRoot])*b.local;
  if(b.source!=None)
    setLinear(m,IkMath::mul(linear(source[b.source]),b.turn));
  if(i==rootBone)
    setPosition(m,apply(source[rootSource],rootLocal));
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

// Two-bone IK onto the ankle of the source foot, the knee towards the source knee. The axis of a bone is the way to
// its child in the bind pose; the thigh and the calf take the shortest turn onto the solved directions, the foot keeps
// its turn and stands on the target. A target out of reach: the leg straight towards it.
void RenderRig::solveLeg(const Leg& leg, const Matrix4x4* source, Matrix4x4* out) const {
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
  const Vec3  toTarget = apply(source[leg.sourceFoot],leg.ankle) - hip;
  const float dist     = length(toTarget);
  if(l1<IkMath::Eps || l2<IkMath::Eps || dist<IkMath::Eps) {
    out[leg.foot] = place(leg.foot,source,out);  // nowhere to bend to: the leg as it stands
    return;
    }

  // the triangle in double: a straight leg cancels most of l1^2 - along^2
  const Vec3   u     = toTarget*(1.f/dist);
  const double d1 = l1, d2 = l2;
  const double d     = std::clamp(double(dist),std::abs(d1-d2)+1e-6,d1+d2);
  const double along = (d1*d1 - d2*d2 + d*d)/(2.0*d);
  const double up    = std::sqrt(std::max(d1*d1 - along*along,0.0));
  const Vec3   end   = hip + u*float(d);

  const Vec3 pole = IkMath::position(source[leg.sourceKnee]) - hip;
  Vec3 side = pole - u*IkMath::dot(pole,u);
  if(length(side)<IkMath::Eps)
    side = a1 - u*IkMath::dot(a1,u);  // the pole on the line: the knee keeps its side
  if(length(side)<IkMath::Eps)
    side = IkMath::cross(u,std::abs(u.x)<0.9f ? Vec3(1,0,0) : Vec3(0,1,0));
  side = normalize(side);
  const Vec3 knee = hip + u*float(along) + side*float(up);

  setLinear(thigh,IkMath::mul(IkMath::rotationBetween(normalize(a1),normalize(knee-hip)),linear(thigh)));
  Matrix4x4 calf = place(leg.calf,source,out);  // without a source it follows the turned thigh
  setPosition(calf,knee);
  const Vec3 axis = IkMath::mul(linear(calf),toFoot);
  setLinear(calf,IkMath::mul(IkMath::rotationBetween(normalize(axis),normalize(end-knee)),linear(calf)));
  out[leg.calf] = calf;
  Matrix4x4 foot = place(leg.foot,source,out);
  setPosition(foot,end);
  out[leg.foot] = foot;
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

RenderRigIndex::RenderRigIndex(Lookup lookup, Report report)
  :lookup(std::move(lookup)), report(std::move(report)) {
  }

RenderRigIndex::~RenderRigIndex() = default;

void RenderRigIndex::read(const std::filesystem::path& file) {
  specs.clear();
  meshes.clear();
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
  built.clear();
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(),json.size());
  try {
    if(doc.HasParseError())
      fail("invalid JSON at offset "+std::to_string(doc.GetErrorOffset())+": "+
           rapidjson::GetParseError_En(doc.GetParseError()));
    if(!doc.IsObject())
      fail("the file must be a JSON object");
    onlyFields(doc,{"format","version","about","rigs","meshes"},"");
    if(!doc.HasMember("format") || !doc["format"].IsString() || str(doc["format"])!="gothic-render-rigs")
      fail("format must be \"gothic-render-rigs\"");
    if(!doc.HasMember("version") || !doc["version"].IsUint() || doc["version"].GetUint()!=1)
      fail("version must be 1");
    if(!doc.HasMember("rigs") || !doc["rigs"].IsObject())
      fail("rigs must be an object");
    if(doc.HasMember("meshes") && !doc["meshes"].IsObject())
      fail("meshes must be an object: mesh -> rig");
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
