#include "ragdoll.h"

#include "collisionworld.h"
#include "dynamicworld.h"
#include "game/constants.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/skeleton.h"
#include "utils/versioninfo.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {
  constexpr size_t noBone = size_t(-1);
  constexpr float pi = 3.14159265358979323846f;

  bool finite(const Tempest::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

  bool finite(const Tempest::Vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    }

  float ulp(float value) {
    return std::abs(std::nextafter(value,std::numeric_limits<float>::infinity())-value);
    }

  bool nativePrecision(const Ragdoll::PartState& p) {
    if(!finite(p.centerOfMassMeters) || !finite(p.rotation) || !finite(p.linearVelocityMeters) ||
       std::abs(p.centerOfMassMeters.x)>1.e6f || std::abs(p.centerOfMassMeters.y)>1.e6f || std::abs(p.centerOfMassMeters.z)>1.e6f) return false;
    const auto rotation=JPH::Quat(p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w);
    if(!rotation.IsNormalized(1.e-4f)) return false;
    const auto basis=JPH::Mat44::sRotation(rotation);
    for(JPH::uint axis=0;axis<3;++axis)
      for(JPH::uint component=0;component<3;++component)
        if(std::abs(basis.GetColumn3(axis)[component]-p.transform.data()[axis*4+component])>1.e-5f) return false;
    const float native[] {p.linearVelocityMeters.x,p.linearVelocityMeters.y,p.linearVelocityMeters.z};
    const float publicCm[] {p.linearVelocity.x,p.linearVelocity.y,p.linearVelocity.z};
    for(size_t component=0;component<3;++component)
      if(std::abs(native[component]*100.f-publicCm[component])>std::max(1.e-5f,ulp(publicCm[component]))) return false;
    return true;
    }

  bool samePublicPosition(const Tempest::Matrix4x4& a,const Tempest::Matrix4x4& b) {
    for(size_t component=12;component<15;++component)
      if(std::abs(a.data()[component]-b.data()[component])>std::max(1.e-6f,2.f*ulp(b.data()[component]))) return false;
    return true;
    }

  // Capsule shapes require a positive uniform scale. Reject shear, reflection
  // and nonuniform scaling rather than build joints with different distances.
  bool rigidTransform(const Tempest::Matrix4x4& matrix, JPH::Mat44& out, float& scale, const char** reason=nullptr) {
    if(reason!=nullptr) *reason="accepted";
    auto reject=[&](const char* why) { if(reason!=nullptr) *reason=why; return false; };
    const auto* m = matrix.data();
    for(size_t i=0;i<16;++i)
      if(!std::isfinite(m[i]))
        return reject("nonfinite");
    if(std::abs(m[3])>1.e-5f || std::abs(m[7])>1.e-5f || std::abs(m[11])>1.e-5f || std::abs(m[15]-1.f)>1.e-5f)
      return reject("nonaffine");
    if(std::abs(m[12])>1.e8f || std::abs(m[13])>1.e8f || std::abs(m[14])>1.e8f)
      return reject("world_coordinate_bound");
    const auto x = JPH::Vec3(m[0],m[1],m[2]);
    const auto y = JPH::Vec3(m[4],m[5],m[6]);
    const auto z = JPH::Vec3(m[8],m[9],m[10]);
    scale = x.Length();
    if(scale<1.e-4f || scale>100.f || std::abs(y.Length()-scale)>scale*1.e-3f ||
       std::abs(z.Length()-scale)>scale*1.e-3f)
      return reject("scale_bound_or_nonuniform");
    const auto nx=x/scale, ny=y/scale, nz=z/scale;
    if(std::abs(nx.Dot(ny))>1.e-3f || std::abs(nx.Dot(nz))>1.e-3f || std::abs(ny.Dot(nz))>1.e-3f || nx.Cross(ny).Dot(nz)<0.999f)
      return reject("shear_or_reflection");
    const auto q = JPH::Mat44(JPH::Vec4(nx,0),JPH::Vec4(ny,0),JPH::Vec4(nz,0),JPH::Vec3::sZero()).GetQuaternion().Normalized();
    out = JPH::Mat44::sRotationTranslation(q,JPH::Vec3(m[12]*.01f,m[13]*.01f,m[14]*.01f));
    return true;
    }

  struct ObjectFrame {
    JPH::Mat44 rigid;
    JPH::Vec3 scale=JPH::Vec3::sOne();
    float volumeScale=1.f;
    bool uniform=true;
    };

  bool objectFrame(const Tempest::Matrix4x4& matrix, ObjectFrame& frame) {
    const auto* m=matrix.data();
    for(size_t i=0;i<16;++i) if(!std::isfinite(m[i])) return false;
    if(std::abs(m[3])>1.e-5f || std::abs(m[7])>1.e-5f || std::abs(m[11])>1.e-5f || std::abs(m[15]-1.f)>1.e-5f ||
       std::abs(m[12])>1.e8f || std::abs(m[13])>1.e8f || std::abs(m[14])>1.e8f) return false;
    JPH::Vec3 axes[3] {JPH::Vec3(m[0],m[1],m[2]),JPH::Vec3(m[4],m[5],m[6]),JPH::Vec3(m[8],m[9],m[10])};
    for(JPH::uint i=0;i<3;++i) {
      const float s=axes[i].Length();
      if(s<1.e-4f || s>100.f) return false;
      frame.scale.SetComponent(i,std::abs(s-1.f)<1.e-4f ? 1.f : s);
      axes[i]/=s;
      }
    if(std::abs(axes[0].Dot(axes[1]))>1.e-3f || std::abs(axes[0].Dot(axes[2]))>1.e-3f ||
       std::abs(axes[1].Dot(axes[2]))>1.e-3f || axes[0].Cross(axes[1]).Dot(axes[2])<.999f) return false;
    frame.volumeScale=float(std::abs(double(m[0])*(double(m[5])*m[10]-double(m[9])*m[6])-
                                     double(m[4])*(double(m[1])*m[10]-double(m[9])*m[2])+
                                     double(m[8])*(double(m[1])*m[6]-double(m[5])*m[2])));
    frame.uniform=std::abs(frame.scale.GetY()-frame.scale.GetX())<=frame.scale.GetX()*1.e-3f &&
                  std::abs(frame.scale.GetZ()-frame.scale.GetX())<=frame.scale.GetX()*1.e-3f;
    const auto q=JPH::Mat44(JPH::Vec4(axes[0],0),JPH::Vec4(axes[1],0),JPH::Vec4(axes[2],0),JPH::Vec3::sZero()).GetQuaternion().Normalized();
    frame.rigid=JPH::Mat44::sRotationTranslation(q,JPH::Vec3(m[12]*.01f,m[13]*.01f,m[14]*.01f));
    return true;
    }

  using Matrix3=std::array<std::array<double,3>,3>;

  double determinant(const Matrix3& a) {
    return a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-
           a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+
           a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    }

  bool inverse(const Matrix3& a, Matrix3& out) {
    const double det=determinant(a);
    if(!std::isfinite(det) || std::abs(det)<1.e-30) return false;
    for(size_t r=0;r<3;++r)
      for(size_t c=0;c<3;++c) {
        const size_t c1=(c+1)%3,c2=(c+2)%3,r1=(r+1)%3,r2=(r+2)%3;
        out[r][c]=(a[c1][r1]*a[c2][r2]-a[c1][r2]*a[c2][r1])/det;
        }
    return true;
    }

  std::array<double,3> singularValues(const Matrix3& a) {
    Matrix3 gram {};
    for(size_t r=0;r<3;++r)
      for(size_t c=0;c<3;++c)
        for(size_t i=0;i<3;++i) gram[r][c]+=a[i][r]*a[i][c];
    // Jacobi rotations on the symmetric Gram matrix avoid a fragile column
    // normalization: a rotated anisotropic bone legitimately contains shear.
    for(size_t step=0;step<32;++step) {
      size_t p=0,q=1;
      for(auto pair: {std::pair<size_t,size_t>{0,2}, {1,2}})
        if(std::abs(gram[pair.first][pair.second])>std::abs(gram[p][q])) { p=pair.first; q=pair.second; }
      const double largest=std::max({std::abs(gram[0][0]),std::abs(gram[1][1]),std::abs(gram[2][2])});
      if(std::abs(gram[p][q])<=largest*1.e-15) break;
      const double angle=.5*std::atan2(2*gram[p][q],gram[q][q]-gram[p][p]);
      const double c=std::cos(angle),s=std::sin(angle),pp=gram[p][p],qq=gram[q][q],pq=gram[p][q];
      gram[p][p]=c*c*pp-2*c*s*pq+s*s*qq;
      gram[q][q]=s*s*pp+2*c*s*pq+c*c*qq;
      gram[p][q]=gram[q][p]=0;
      for(size_t i=0;i<3;++i) if(i!=p && i!=q) {
        const double ip=gram[i][p],iq=gram[i][q];
        gram[i][p]=gram[p][i]=c*ip-s*iq;
        gram[i][q]=gram[q][i]=s*ip+c*iq;
        }
      }
    std::array<double,3> result {std::sqrt(std::max(0.,gram[0][0])),std::sqrt(std::max(0.,gram[1][1])),std::sqrt(std::max(0.,gram[2][2]))};
    std::sort(result.begin(),result.end());
    return result;
    }

  bool posedFrame(const Tempest::Matrix4x4& matrix, const ObjectFrame& object, JPH::Mat44& frame) {
    if(object.uniform) {
      float s=0;
      return rigidTransform(matrix,frame,s) && std::abs(s-object.scale.GetX())<=object.scale.GetX()*1.e-3f;
      }
    const auto* m=matrix.data();
    for(size_t i=0;i<16;++i) if(!std::isfinite(m[i])) return false;
    if(std::abs(m[3])>1.e-5f || std::abs(m[7])>1.e-5f || std::abs(m[11])>1.e-5f || std::abs(m[15]-1.f)>1.e-5f ||
       std::abs(m[12])>1.e8f || std::abs(m[13])>1.e8f || std::abs(m[14])>1.e8f) return false;
    Matrix3 a {};
    for(size_t r=0;r<3;++r) for(size_t c=0;c<3;++c) a[r][c]=m[c*4+r];
    if(determinant(a)<=0.) return false;
    const auto actual=singularValues(a);
    std::array<double,3> expected {object.scale.GetX(),object.scale.GetY(),object.scale.GetZ()};
    std::sort(expected.begin(),expected.end());
    for(size_t i=0;i<3;++i)
      if(!std::isfinite(actual[i]) || std::abs(actual[i]-expected[i])>expected[i]*.001) return false;
    // Newton's polar decomposition extracts the proper rotation while retaining
    // the complete affine stretch separately for skinning and save restoration.
    Matrix3 rotation=a;
    for(auto& row:rotation) for(double& v:row) v/=expected[2];
    for(size_t step=0;step<32;++step) {
      Matrix3 inv,next;
      if(!inverse(rotation,inv)) return false;
      double error=0;
      for(size_t r=0;r<3;++r) for(size_t c=0;c<3;++c) {
        next[r][c]=.5*(rotation[r][c]+inv[c][r]);
        error=std::max(error,std::abs(next[r][c]-rotation[r][c]));
        }
      rotation=next;
      if(error<1.e-12) break;
      }
    if(std::abs(determinant(rotation)-1.)>1.e-8) return false;
    const auto column=[&](size_t c) { return JPH::Vec4(float(rotation[0][c]),float(rotation[1][c]),float(rotation[2][c]),0); };
    const auto q=JPH::Mat44(column(0),column(1),column(2),JPH::Vec3::sZero()).GetQuaternion().Normalized();
    frame=JPH::Mat44::sRotationTranslation(q,JPH::Vec3(m[12]*.01f,m[13]*.01f,m[14]*.01f));
    return true;
    }

  Tempest::Matrix4x4 toMatrix(JPH::RMat44Arg transform, float scale) {
    float m[16];
    for(JPH::uint i=0;i<4;++i) {
      const auto c=transform.GetColumn4(i);
      const float k=i<3 ? scale : 1.f;
      m[4*i+0]=c.GetX()*k;
      m[4*i+1]=c.GetY()*k;
      m[4*i+2]=c.GetZ()*k;
      m[4*i+3]=c.GetW();
      }
    m[12]*=100.f; m[13]*=100.f; m[14]*=100.f;
    Tempest::Matrix4x4 result;
    result.setData(m);
    return result;
    }

  JPH::Mat44 affineMeters(const Tempest::Matrix4x4& matrix) {
    const auto* m=matrix.data();
    return {JPH::Vec4(m[0],m[1],m[2],0),JPH::Vec4(m[4],m[5],m[6],0),JPH::Vec4(m[8],m[9],m[10],0),
            JPH::Vec3(m[12]*.01f,m[13]*.01f,m[14]*.01f)};
    }

  JPH::Vec3 sourcePoint(const Skeleton& sk, size_t bone, const Tempest::Vec3& at) {
    auto p=at;
    sk.tr[bone].project(p);
    return CollisionWorld::toMeters(p);
    }

  bool named(std::string_view name, std::string_view part) {
    return name.find(part)!=std::string_view::npos;
    }

  float partBuoyancyFraction(std::string_view name) {
    if(named(name,"HEAD") || named(name,"NECK") || named(name,"JAW") || named(name,"KIEFER")) return .95f;
    if(named(name,"CLAVICLE") || named(name,"ARM") || named(name,"HAND") || named(name,"FINGER") ||
       named(name,"THIGH") || named(name,"CALF") || named(name,"HORSELINK") || named(name,"FOOT") || named(name,"TOE") ||
       named(name,"LEG") || named(name,"TAIL") || named(name,"CLAW") || named(name,"ZANGE") ||
       named(name,"PINCER") || named(name,"FUEHLER") || named(name,"WING")) return 1.f;
    return .9f;
    }

  JPH::Vec3 perpendicular(JPH::Vec3Arg axis, JPH::Vec3Arg preferred) {
    auto result=preferred-axis*preferred.Dot(axis);
    if(result.IsNearZero())
      result=axis.GetNormalizedPerpendicular();
    return result.Normalized();
    }
  }

struct Ragdoll::Impl final {
  struct Capsule {
    JPH::Vec3 a,b;
    float radius;
    size_t aBone=noBone,bBone=noBone;
    Tempest::Vec3 aLocal,bLocal;
    };
  struct Part {
    size_t bone=noBone;
    std::string name;
    int parent=-1;
    JPH::Mat44 bind;
    std::vector<Capsule> capsules;
    float volume=0;
    };
  struct Schema {
    struct Ancestor { size_t bone,anchorPart; };
    std::string name;
    size_t numBones=0;
    float totalMass=0;
    std::vector<Part> parts;
    std::vector<size_t> boneIndices;
    std::vector<Ancestor> ancestors;
    std::map<float,JPH::Ref<JPH::RagdollSettings>> scaled;
    };

  static std::shared_ptr<Schema> schema(const Skeleton& sk) {
    // Resources owns skeletons throughout the game. Geometry is derived from
    // their canonical HitZones, so clothing and head meshes do not change rigs.
    static std::map<const Skeleton*,std::shared_ptr<Schema>> cache;
    auto found=cache.find(&sk);
    if(found!=cache.end() && found->second->name==sk.name() && found->second->numBones==sk.nodes.size())
      return found->second;
    HitZones zones;
    zones.setSkeleton(&sk);
    auto result=std::make_shared<Schema>();
    result->name=std::string(sk.name());
    result->numBones=sk.nodes.size();
    result->totalMass=Ragdoll::speciesMass(sk.name());
    std::vector<bool> owners(sk.nodes.size(),false);
    for(const auto& cap:zones.caps)
      if(cap.owner<owners.size())
        owners[cap.owner]=true;
    // HitZones never give the skeleton root flesh: for a human it is the floor
    // point under the pelvis. An insect's root is its thorax (Bloodfly: BIP01
    // CENTER carries legs, wings, pincers and tail). Without a part there, all
    // limbs hang off whichever leg segment became part 0 and the chain explodes
    // on landing. Give such a root a trunk to the owners directly under it.
    std::vector<size_t> rootChildren;
    size_t root=noBone;
    for(size_t i=0;i<owners.size();++i) {
      if(!owners[i]) continue;
      size_t top=i;
      bool nested=false;
      for(size_t p=sk.nodes[i].parent,limit=0;p<sk.nodes.size() && limit<sk.nodes.size();p=sk.nodes[p].parent,++limit) {
        if(owners[p]) { nested=true; break; }
        top=p;
        }
      if(nested || top==i) continue;
      if(root!=noBone && root!=top) { rootChildren.clear(); break; }
      root=top;
      size_t child=i;
      while(sk.nodes[child].parent!=root) child=sk.nodes[child].parent;
      if(child==i) rootChildren.push_back(i);
      }
    if(rootChildren.size()<2) root=noBone;
    std::vector<size_t> pending;
    if(root!=noBone) pending.push_back(root);
    for(size_t i=0;i<owners.size();++i)
      if(owners[i]) pending.push_back(i);
    auto depth=[&](size_t bone) {
      size_t d=0;
      for(size_t p=sk.nodes[bone].parent;p<sk.nodes.size() && d<sk.nodes.size();p=sk.nodes[p].parent) ++d;
      return d;
      };
    std::stable_sort(pending.begin(),pending.end(),[&](size_t a,size_t b) { return depth(a)<depth(b); });
    std::vector<int> boneToPart(sk.nodes.size(),-1);
    for(size_t bone:pending) {
      Part part;
      part.bone=bone;
      part.name=sk.nodes[bone].name;
      float bindScale=1;
      if(!rigidTransform(sk.tr[bone],part.bind,bindScale) || std::abs(bindScale-1.f)>1.e-3f)
        throw std::runtime_error("Unsupported ragdoll bind transform");
      size_t parent=sk.nodes[bone].parent;
      for(size_t limit=0;parent<sk.nodes.size() && boneToPart[parent]<0 && limit<sk.nodes.size();++limit)
        parent=sk.nodes[parent].parent;
      part.parent=parent<sk.nodes.size() ? boneToPart[parent] : (result->parts.empty() ? -1 : 0);
      // Gothic hangs the clavicles from the neck bone. A physical shoulder hung
      // there follows every nod of the head; it belongs to the chest below it.
      if(named(part.name,"CLAVICLE") && part.parent>0 && named(result->parts[size_t(part.parent)].name,"NECK"))
        part.parent=result->parts[size_t(part.parent)].parent;
      const auto inverse=part.bind.InversedRotationTranslation();
      for(const auto& cap:zones.caps) {
        if(cap.owner!=bone || cap.a.bone>=sk.tr.size() || cap.b.bone>=sk.tr.size()) continue;
        Capsule c {inverse*sourcePoint(sk,cap.a.bone,cap.a.at),inverse*sourcePoint(sk,cap.b.bone,cap.b.at),cap.r*.01f};
        c.aBone=cap.a.bone; c.bBone=cap.b.bone; c.aLocal=cap.a.at; c.bLocal=cap.b.at;
        if(!std::isfinite(c.radius) || c.radius<=0.f) continue;
        part.volume+=pi*c.radius*c.radius*(c.b-c.a).Length()+4.f/3.f*pi*c.radius*c.radius*c.radius;
        part.capsules.push_back(c);
        }
      if(bone==root) {
        // HitZones' trunk thickness (12 cm for a 160 cm human) at this creature's size.
        const float radius=12.f/160.f*zones.size*.01f;
        for(size_t child:rootChildren) {
          Capsule c {inverse*sourcePoint(sk,bone,{}),inverse*sourcePoint(sk,child,{}),radius};
          c.aBone=bone; c.bBone=child; c.aLocal=c.bLocal=Tempest::Vec3();
          part.volume+=pi*c.radius*c.radius*(c.b-c.a).Length()+4.f/3.f*pi*c.radius*c.radius*c.radius;
          part.capsules.push_back(c);
          }
        }
      if(part.capsules.empty()) continue;
      boneToPart[bone]=int(result->parts.size());
      result->boneIndices.push_back(bone);
      result->parts.push_back(std::move(part));
      }
    std::vector<bool> captured(sk.nodes.size(),false);
    for(size_t part=0;part<result->parts.size();++part) {
      size_t parent=sk.nodes[result->parts[part].bone].parent;
      for(size_t limit=0;parent<sk.nodes.size() && limit<sk.nodes.size();++limit) {
        if(boneToPart[parent]>=0) break;
        if(!captured[parent]) {
          result->ancestors.push_back({parent,part});
          captured[parent]=true;
          }
        parent=sk.nodes[parent].parent;
        }
      }
    std::sort(result->ancestors.begin(),result->ancestors.end(),[](const Schema::Ancestor& a,const Schema::Ancestor& b) { return a.bone<b.bone; });
    cache[&sk]=result;
    return result;
    }

  // Model-space directions of a biped, from its bind pose: the thighs give its
  // left, the toes its front. Other creatures keep the generic joint table.
  struct BodyAxes { bool valid=false; JPH::Vec3 left,up,forward; };

  static BodyAxes bodyAxes(const Schema& schema) {
    const auto find=[&](std::string_view name) -> const Part* {
      for(const auto& p:schema.parts) if(named(p.name,name)) return &p;
      return nullptr;
      };
    const auto at=[](const Part* p) { return p->bind.GetTranslation(); };
    BodyAxes result;
    const Part* lt=find(" L THIGH"), *rt=find(" R THIGH"), *lf=find(" L FOOT"), *rf=find(" R FOOT");
    const Part* ltoe=find(" L TOE"), *rtoe=find(" R TOE"), *head=find("HEAD");
    if(!lt || !rt || !lf || !rf || !ltoe || !rtoe || !head || !find("PELVIS") || !find(" L UPPERARM") || !find(" R UPPERARM"))
      return result;
    const auto left=at(lt)-at(rt);
    if(left.LengthSq()<1.e-6f || at(head).GetY()<=at(lt).GetY()) return result;
    result.left=left.Normalized();
    result.up=perpendicular(result.left,JPH::Vec3::sAxisY());
    result.forward=result.left.Cross(result.up);
    if(result.forward.Dot(at(ltoe)-at(lf)+at(rtoe)-at(rf))<0.f) result.forward=-result.forward;
    result.valid=true;
    return result;
    }

  // Degrees: flexion moves the bone towards 'front', abduction towards 'side',
  // a positive twist turns 'front' towards 'medial'. Passive human ranges,
  // somewhat inside the extremes a living body reaches only with effort.
  struct JointRange { float flexMin,flexMax,sideMin,sideMax,twistMin,twistMax; };

  // An elliptic swing cone centred inside an asymmetric range. The parent frame
  // carries the centre; the child frame is the bind bone reached from it by the
  // shortest swing, so the bind pose has zero twist and lies inside the cone.
  static void anatomicalCone(JPH::SwingTwistConstraintSettings& joint, JPH::Vec3Arg bone, JPH::Vec3Arg neutral,
                             JPH::Vec3Arg front, JPH::Vec3Arg side, JPH::Vec3Arg medial, const JointRange& r) {
    constexpr float rad=pi/180.f;
    const auto f=perpendicular(neutral,front);
    const auto s=perpendicular(f,perpendicular(neutral,side));
    const auto offset=f*((r.flexMin+r.flexMax)*.5f*rad)+s*((r.sideMin+r.sideMax)*.5f*rad);
    auto centre=JPH::Quat::sIdentity();
    if(offset.LengthSq()>1.e-8f)
      centre=JPH::Quat::sRotation(neutral.Cross(offset).Normalized(),offset.Length());
    const auto c=(centre*neutral).Normalized(), cf=(centre*f).Normalized();
    joint.mTwistAxis1=c;
    joint.mPlaneAxis1=cf;
    joint.mTwistAxis2=bone;
    joint.mPlaneAxis2=perpendicular(bone,JPH::Quat::sFromTo(c,bone)*cf);
    // Jolt: the plane angle bounds the swing towards the plane axis, the normal angle across it.
    joint.mPlaneHalfConeAngle=(r.flexMax-r.flexMin)*.5f*rad;
    joint.mNormalHalfConeAngle=(r.sideMax-r.sideMin)*.5f*rad;
    // A positive native twist turns the plane axis towards twist x plane.
    const bool positive=c.Cross(cf).Dot(medial)>=0.f;
    joint.mTwistMinAngle=(positive ? r.twistMin : -r.twistMax)*rad;
    joint.mTwistMaxAngle=(positive ? r.twistMax : -r.twistMin)*rad;
    }

  // Joints of a biped other than the knee and elbow hinges. Returns false
  // for a part the table does not describe (a jaw, a tail).
  static bool humanJoint(JPH::SwingTwistConstraintSettings& joint, const Schema& schema, const Part& src,
                         const BodyAxes& axes) {
    const auto& name=src.name;
    const auto here=src.bind.GetTranslation();
    // The bone runs to the child that continues its chain; an end part
    // (a toe, a finger, the head) continues its parent.
    JPH::Vec3 bone=(here-schema.parts[size_t(src.parent)].bind.GetTranslation()).NormalizedOr(axes.up);
    for(const auto& child:schema.parts)
      if(child.parent>=0 && &schema.parts[size_t(child.parent)]==&src &&
         !named(child.name,"CLAVICLE") && !named(child.name,"THIGH")) {
        const auto d=child.bind.GetTranslation()-here;
        if(d.LengthSq()>1.e-8f) { bone=d.Normalized(); break; }
        }
    const float side=named(name," R ") ? -1.f : 1.f;
    const auto lateral=axes.left*side;
    if(named(name,"THIGH"))
      anatomicalCone(joint,bone,-axes.up,axes.forward,lateral,-lateral,{-15,110,-20,40,-40,30});
    else if(named(name,"FOOT")) // dorsiflexion up (a buckling knee needs a squat's), toe out, inversion
      anatomicalCone(joint,bone,bone,axes.up,lateral,lateral,{-45,35,-15,15,-15,25});
    else if(named(name,"TOE"))
      anatomicalCone(joint,bone,bone,axes.up,lateral,lateral,{-25,45,-10,10,-5,5});
    else if(named(name,"SPINE"))
      anatomicalCone(joint,bone,bone,axes.forward,axes.left,axes.left,{-8,25,-10,10,-12,12});
    else if(named(name,"NECK") || named(name,"HEAD"))
      anatomicalCone(joint,bone,bone,axes.forward,axes.left,axes.left,{-25,30,-20,20,-35,35});
    else if(named(name,"CLAVICLE")) // forward and up, from the bind shoulder
      anatomicalCone(joint,bone,bone,axes.forward,axes.up,axes.up,{-12,12,-8,15,-10,10});
    else if(named(name,"UPPERARM"))
      anatomicalCone(joint,bone,-axes.up,axes.forward,lateral,-lateral,{-40,135,-30,120,-70,70});
    else if(named(name,"HAND"))
      anatomicalCone(joint,bone,bone,axes.forward,axes.up,axes.up,{-45,45,-45,45,-80,80});
    else if(named(name,"FINGER"))
      anatomicalCone(joint,bone,bone,axes.forward,axes.up,axes.up,{-30,30,-30,30,-10,10});
    else
      return false;
    return true;
    }

  // Passive joint tissue: a dead limb still resists turning, in proportion to
  // its weight, so it settles short of its limits instead of flopping onto them.
  static float jointFriction(std::string_view name, float mass) {
    // Light in the legs: knees that resist give a corpse standing on locked legs.
    if(named(name,"NECK") || named(name,"HEAD")) return .3f*mass;
    if(named(name,"THIGH") || named(name,"CALF") || named(name,"FOOT") || named(name,"TOE")) return .2f*mass;
    if(named(name,"CLAVICLE") || named(name,"ARM") || named(name,"HAND") || named(name,"FINGER")) return .4f*mass;
    return .6f*mass;
    }

  static JPH::Ref<JPH::RagdollSettings> settings(Schema& schema, float scale) {
    auto found=schema.scaled.find(scale);
    if(found!=schema.scaled.end()) return found->second;
    JPH::Ref<JPH::RagdollSettings> result=new JPH::RagdollSettings();
    result->mSkeleton=new JPH::Skeleton();
    const float totalVolume=std::accumulate(schema.parts.begin(),schema.parts.end(),0.f,[](float v,const Part& p) { return v+p.volume; });
    const float totalMass=schema.totalMass;
    const auto axes=bodyAxes(schema);
    std::vector<JPH::Mat44> bind;
    for(const auto& src:schema.parts) {
      result->mSkeleton->AddJoint(src.name,src.parent);
      JPH::StaticCompoundShapeSettings compound;
      for(const auto& cap:src.capsules) {
        const auto d=(cap.b-cap.a)*scale;
        JPH::ShapeRefC shape;
        if(d.LengthSq()<1.e-10f)
          shape=new JPH::SphereShape(cap.radius*scale);
        else
          shape=new JPH::CapsuleShape(d.Length()*.5f,cap.radius*scale);
        const auto q=d.NormalizedOr(JPH::Vec3::sAxisY());
        compound.AddShape((cap.a+cap.b)*(.5f*scale),JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),q),shape.GetPtr());
        }
      auto shape=compound.Create();
      if(shape.HasError()) throw std::runtime_error("Jolt ragdoll capsule: "+std::string(shape.GetError().c_str()));
      JPH::RagdollSettings::Part part;
      part.SetShape(shape.Get().GetPtr());
      part.mPosition=src.bind.GetTranslation()*scale;
      part.mRotation=src.bind.GetQuaternion();
      part.mMotionType=JPH::EMotionType::Dynamic;
      part.mObjectLayer=DynamicWorld::C_Ragdoll;
      part.mFriction=.55f;
      part.mRestitution=0.f;
      part.mLinearDamping=.12f;
      part.mAngularDamping=.3f;
      part.mMotionQuality=JPH::EMotionQuality::LinearCast;
      part.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;
      part.mMassPropertiesOverride.mMass=totalMass*src.volume/totalVolume;
      part.mInertiaMultiplier=2.f;
      part.mNumVelocityStepsOverride=12;
      // CCD can stop a foot during a step while its parent still advances.
      // More position iterations keep the connected bone anchors together at
      // that first ground impact without redistributing the anatomical masses.
      part.mNumPositionStepsOverride=10;
      if(src.parent>=0) {
        JPH::Ref<JPH::SwingTwistConstraintSettings> joint=new JPH::SwingTwistConstraintSettings();
        joint->mNumVelocityStepsOverride=12;
        joint->mNumPositionStepsOverride=10;
        joint->mPosition1=joint->mPosition2=part.mPosition;
        const auto& name=src.name;
        const auto direction=src.bind.Multiply3x3((src.capsules.front().b-src.capsules.front().a).NormalizedOr(JPH::Vec3::sAxisY()));
        auto twist=direction;
        auto plane=perpendicular(twist,src.bind.GetAxisZ());
        bool anatomical=false;
        if(named(name,"CALF") || named(name,"FOREARM")) {
          // Gothic's limb bone X runs along the capsule: projecting it onto the
          // lateral plane would normalize a few float ULPs into an arbitrary
          // hinge axis. The two anatomical segments define the flexion plane.
          const auto& parent=schema.parts[size_t(src.parent)];
          const auto upper=(src.bind.GetTranslation()-parent.bind.GetTranslation()).NormalizedOr(direction);
          const auto cross=upper.Cross(direction);
          const float dot=std::clamp(upper.Dot(direction),-1.f,1.f);
          float bend=0.f;
          if(cross.LengthSq()>1.e-4f) {
            twist=cross.Normalized();
            bend=std::atan2(cross.Length(),dot);
            }
          else {
            if(dot<=0.f) throw std::runtime_error("Unsupported reversed ragdoll hinge");
            // A straight knee flexes behind model-forward; an elbow in front.
            // These are model-space references, carried with the bind frames,
            // so rotating the actor cannot change the hinge or its handedness.
            const auto flexion=named(name,"CALF") ? -JPH::Vec3::sAxisZ() : JPH::Vec3::sAxisZ();
            twist=direction.Cross(flexion);
            if(twist.LengthSq()<1.e-4f) {
              const auto y=src.bind.GetAxisY()-direction*src.bind.GetAxisY().Dot(direction);
              const auto z=src.bind.GetAxisZ()-direction*src.bind.GetAxisZ().Dot(direction);
              twist=y.LengthSq()>=z.LengthSq() ? y : z;
              }
            twist=twist.Normalized();
            bend=std::atan2(twist.Dot(cross),dot);
            }
          plane=direction;
          joint->mNormalHalfConeAngle=0.f;
          joint->mPlaneHalfConeAngle=0.f;
          // Constraint zero remains the canonical bind pose. Account for the
          // bind's existing flexion so the anatomical range is still 0..150
          // degrees (an elbow 0..145): straightening and bending are permitted.
          joint->mTwistMinAngle=-bend;
          joint->mTwistMaxAngle=(named(name,"CALF") ? 150.f : 145.f)*pi/180.f-bend;
          }
        else if(axes.valid && humanJoint(*joint,schema,src,axes)) {
          anatomical=true; // asymmetric limits carry their own frames
          }
        else if(named(name,"SPINE") || named(name,"NECK") || named(name,"PELVIS")) {
          joint->mNormalHalfConeAngle=25.f*pi/180.f;
          joint->mPlaneHalfConeAngle=20.f*pi/180.f;
          joint->mTwistMinAngle=-20.f*pi/180.f;
          joint->mTwistMaxAngle=20.f*pi/180.f;
          }
        else {
          const bool shoulder=named(name,"UPPERARM") || named(name,"CLAVICLE");
          joint->mNormalHalfConeAngle=(shoulder ? 80.f : 50.f)*pi/180.f;
          joint->mPlaneHalfConeAngle=(shoulder ? 75.f : 40.f)*pi/180.f;
          joint->mTwistMinAngle=-40.f*pi/180.f;
          joint->mTwistMaxAngle=40.f*pi/180.f;
          }
        if(!anatomical) {
          joint->mTwistAxis1=joint->mTwistAxis2=twist;
          joint->mPlaneAxis1=joint->mPlaneAxis2=plane;
          }
        joint->mMaxFrictionTorque=axes.valid ? jointFriction(name,part.mMassPropertiesOverride.mMass) : .15f;
        part.mToParent=joint;
        }
      bind.push_back(JPH::Mat44::sRotationTranslation(part.mRotation,JPH::Vec3(part.mPosition)));
      result->mParts.push_back(std::move(part));
      }
    // The filter is immutable and shared by instances, with a unique group per
    // corpse. Adjacent and bind-overlapping parts cannot launch each other.
    result->DisableParentChildCollisions(bind.data(),.001f);
    result->CalculateConstraintPriorities();
    result->CalculateBodyIndexToConstraintIndex();
    result->CalculateConstraintIndexToBodyIdxPair();
    schema.scaled.emplace(scale,result);
    return result;
    }

  static JPH::Ref<JPH::RagdollSettings> bakedSettings(Schema& schema, const ObjectFrame& object,
                                                     const std::vector<Tempest::Matrix4x4>& references,
                                                     const std::vector<JPH::Mat44>& frames) {
    auto canonical=settings(schema,1.f);
    JPH::Ref<JPH::RagdollSettings> result=new JPH::RagdollSettings();
    result->mSkeleton=canonical->mSkeleton;
    std::vector<int> boneToPart(schema.numBones,-1);
    for(size_t i=0;i<schema.parts.size();++i) boneToPart[schema.parts[i].bone]=int(i);
    std::vector<float> volume;
    for(size_t i=0;i<schema.parts.size();++i) {
      const auto& src=schema.parts[i];
      const auto& ref=references[i];
      const Tempest::Vec3 origin {ref.data()[12],ref.data()[13],ref.data()[14]};
      const auto rotation=frames[i].GetQuaternion();
      auto point=[&](size_t bone, Tempest::Vec3 value) {
        if(bone>=boneToPart.size() || boneToPart[bone]<0) throw std::runtime_error("Missing ragdoll reference bone");
        references[size_t(boneToPart[bone])].project(value);
        return rotation.InverseRotate(CollisionWorld::toMeters(value-origin));
        };
      JPH::StaticCompoundShapeSettings compound;
      float v=0;
      for(const auto& cap:src.capsules) {
        const auto a=point(cap.aBone,cap.aLocal),b=point(cap.bBone,cap.bLocal),d=b-a;
        // This is HitZones' existing capsule convention, not an ellipsoid:
        // posed endpoints use the complete affine bone, radius uses object X.
        const float r=cap.radius*object.scale.GetX();
        JPH::ShapeRefC shape;
        if(d.LengthSq()<1.e-10f) shape=new JPH::SphereShape(r);
        else shape=new JPH::CapsuleShape(d.Length()*.5f,r);
        compound.AddShape((a+b)*.5f,JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),d.NormalizedOr(JPH::Vec3::sAxisY())),shape.GetPtr());
        v+=pi*r*r*d.Length()+4.f/3.f*pi*r*r*r;
        }
      auto shape=compound.Create();
      if(shape.HasError()) throw std::runtime_error("Jolt scaled ragdoll capsule: "+std::string(shape.GetError().c_str()));
      auto part=canonical->mParts[i];
      part.SetShape(shape.Get().GetPtr());
      part.mPosition=frames[i].GetTranslation();
      part.mRotation=rotation;
      if(src.parent>=0) {
        const auto* base=static_cast<const JPH::SwingTwistConstraintSettings*>(canonical->mParts[i].mToParent.GetPtr());
        JPH::Ref<JPH::SwingTwistConstraintSettings> joint=new JPH::SwingTwistConstraintSettings(*base);
        const size_t parent=size_t(src.parent);
        // Only the translational anchor follows the posed, scaled geometry.
        // Angular reference frames remain canonical bind-local frames, so a
        // bent initial pose is not silently redefined as the neutral pose.
        joint->mPosition1=joint->mPosition2=part.mPosition;
        const auto parentBind=schema.parts[parent].bind.GetQuaternion();
        const auto childBind=src.bind.GetQuaternion();
        joint->mTwistAxis1=frames[parent].Multiply3x3(parentBind.InverseRotate(base->mTwistAxis1));
        joint->mPlaneAxis1=frames[parent].Multiply3x3(parentBind.InverseRotate(base->mPlaneAxis1));
        joint->mTwistAxis2=frames[i].Multiply3x3(childBind.InverseRotate(base->mTwistAxis2));
        joint->mPlaneAxis2=frames[i].Multiply3x3(childBind.InverseRotate(base->mPlaneAxis2));
        part.mToParent=joint;
        }
      result->mParts.push_back(std::move(part));
      volume.push_back(v);
      }
    const float total=std::accumulate(volume.begin(),volume.end(),0.f);
    for(size_t i=0;i<volume.size();++i) result->mParts[i].mMassPropertiesOverride.mMass=schema.totalMass*volume[i]/total;
    result->DisableParentChildCollisions(frames.data(),.001f);
    result->CalculateConstraintPriorities();
    result->CalculateBodyIndexToConstraintIndex();
    result->CalculateConstraintIndexToBodyIdxPair();
    return result;
    }

  static JPH::Mat44 visualCorrection(const Tempest::Matrix4x4& reference, JPH::Mat44Arg frame) {
    const auto* m=reference.data();
    const JPH::Mat44 basis {JPH::Vec4(m[0],m[1],m[2],0),JPH::Vec4(m[4],m[5],m[6],0),JPH::Vec4(m[8],m[9],m[10],0),JPH::Vec3::sZero()};
    return frame.GetRotation().Transposed3x3()*basis;
    }

  static JPH::Ref<JPH::RagdollSettings> massSettings(JPH::Ref<JPH::RagdollSettings> source, float volumeScale) {
    if(volumeScale==1.f) return source;
    // Cached geometry/anatomy is shared. Mass belongs to the instance's object
    // scale, including nonuniform scale, and must also reach the motor source.
    JPH::Ref<JPH::RagdollSettings> result=new JPH::RagdollSettings();
    result->mSkeleton=source->mSkeleton;
    result->mParts=source->mParts;
    result->mAdditionalConstraints=source->mAdditionalConstraints;
    for(auto& part:result->mParts) part.mMassPropertiesOverride.mMass*=volumeScale;
    // CalculateInertia uses this mass with the already scaled capsule shape.
    result->CalculateBodyIndexToConstraintIndex();
    result->CalculateConstraintIndexToBodyIdxPair();
    return result;
    }

  static JPH::Ref<JPH::RagdollSettings> kinematicSettings(const JPH::RagdollSettings& source) {
    JPH::Ref<JPH::RagdollSettings> result=new JPH::RagdollSettings();
    result->mSkeleton=source.mSkeleton;
    result->mParts=source.mParts;
    for(auto& part:result->mParts) {
      part.mMotionType=JPH::EMotionType::Kinematic;
      part.mObjectLayer=DynamicWorld::C_LivingBody;
      part.mGravityFactor=0.f;
      part.mMotionQuality=JPH::EMotionQuality::Discrete;
      part.mToParent=nullptr;
      part.mNumVelocityStepsOverride=0;
      part.mNumPositionStepsOverride=0;
      }
    result->CalculateBodyIndexToConstraintIndex();
    result->CalculateConstraintIndexToBodyIdxPair();
    return result;
    }

  bool ensureMotors(JPH::PhysicsSystem& system) {
    if(motorReady) return true;
    // Build outside the broad phase. Reusing the original settings keeps the
    // tested capsule geometry, anatomical limits and COM-local joint anchors.
    auto& interface=system.GetBodyInterface();
    std::vector<JPH::Mat44> pose;
    for(auto id:native->GetBodyIDs()) pose.push_back(interface.GetWorldTransform(id));
    JPH::Ref<JPH::Ragdoll> replacement=motorSource->CreateRagdoll(group,0,&system);
    if(replacement==nullptr) return false;
    replacement->SetPose(JPH::RVec3::sZero(),pose.data());
    for(size_t i=0;i<pose.size();++i) {
      const auto old=native->GetBodyID(int(i)),id=replacement->GetBodyID(int(i));
      interface.SetMotionType(id,JPH::EMotionType::Kinematic,JPH::EActivation::DontActivate);
      interface.SetObjectLayer(id,DynamicWorld::C_LivingBody);
      interface.SetMotionQuality(id,JPH::EMotionQuality::Discrete);
      interface.SetLinearAndAngularVelocity(id,interface.GetLinearVelocity(old),interface.GetAngularVelocity(old));
      }
    for(size_t i=0;i<replacement->GetConstraintCount();++i) replacement->GetConstraint(int(i))->SetEnabled(false);
    native->RemoveFromPhysicsSystem();
    native=nullptr; // Destroy every retired part before returning to the caller.
    native=std::move(replacement);
    native->AddToPhysicsSystem(JPH::EActivation::DontActivate);
    motorReady=true;
    return true;
    }

  size_t reactionRoot(size_t part) const {
    const auto& name=rig->parts[part].name;
    const auto arm=[](std::string_view n) { return named(n,"CLAVICLE") || named(n,"UPPERARM") || named(n,"FOREARM") || named(n,"HAND") || named(n,"FINGER"); };
    const auto leg=[](std::string_view n) { return named(n,"THIGH") || named(n,"CALF") || named(n,"FOOT") || named(n,"TOE"); };
    const auto head=[](std::string_view n) { return named(n,"NECK") || named(n,"HEAD") || named(n,"JAW") || named(n,"KIEFER"); };
    while(rig->parts[part].parent>0) {
      const auto parent=size_t(rig->parts[part].parent);
      const auto& parentName=rig->parts[parent].name;
      if((arm(name) && arm(parentName)) || (leg(name) && leg(parentName)) || (head(name) && head(parentName))) part=parent;
      else break;
      }
    return part;
    }

  size_t startReaction(size_t hitPart, JPH::Vec3Arg point, float strength, float stiffness, JPH::PhysicsSystem& system) {
    auto& interface=system.GetBodyInterface();
    if(hitPart==0) {
      // The gameplay root is never dynamic. A living pelvis transfers its hit
      // to the closest spine; limp bodies retain the exact addressed bone.
      float distance=std::numeric_limits<float>::max();
      for(size_t i=1;i<rig->parts.size();++i) {
        if(!named(rig->parts[i].name,"SPINE")) continue;
        const float d=(JPH::Vec3(interface.GetPosition(native->GetBodyID(int(i))))-point).LengthSq();
        if(d<distance) { distance=d; hitPart=i; }
        }
      if(hitPart==0 && rig->parts.size()>1) hitPart=1;
      }
    const size_t root=reactionRoot(hitPart);
    if(root==0) return noBone;
    const bool torso=named(rig->parts[hitPart].name,"SPINE");
    for(size_t i=1;i<rig->parts.size();++i) {
      size_t ancestor=i;
      while(ancestor>root && rig->parts[ancestor].parent>=0) ancestor=size_t(rig->parts[ancestor].parent);
      // A torso impact bends the spine without dropping the whole head/arm
      // subtree under gravity. Direct head and limb hits retain their chains.
      if(ancestor!=root || activeParts[i] || (torso && !named(rig->parts[i].name,"SPINE"))) continue;
      activeParts[i]=true;
      const auto id=native->GetBodyID(int(i));
      interface.SetObjectLayer(id,DynamicWorld::C_ActiveBody);
      interface.SetMotionType(id,JPH::EMotionType::Dynamic,JPH::EActivation::Activate);
      interface.SetMotionQuality(id,JPH::EMotionQuality::LinearCast);
      }
    reactionBones.clear();
    for(size_t i=1;i<activeParts.size();++i)
      if(activeParts[i]) reactionBones.push_back(rig->boneIndices[i]);
    for(size_t i=1;i<rig->parts.size();++i) {
      const int index=native->GetRagdollSettings()->GetConstraintIndexForBodyIndex(int(i));
      if(index<0) continue;
      // A kinematic child follows animation independently; constraining it to
      // a dynamic spine would pin the spine to the head and both shoulders.
      native->GetConstraint(index)->SetEnabled(activeParts[i]);
      }
    native->ResetWarmStart();
    // a fresh reaction takes the new hit's stiffness; one still running keeps the stiffer
    reactionStiffness=reactionWeight>0.f ? std::max(reactionStiffness,stiffness) : stiffness;
    reactionAge=0.f;
    reactionStrength=std::max(reactionWeight,strength);
    reactionWeight=reactionStrength;
    return hitPart;
    }

  void driveMotors() {
    // Same body-relative orientation convention as DriveToPoseUsingMotors;
    // the already decomposed animation targets avoid another SkeletonPose solve.
    const float recovery=benchActive ? 1.f : std::clamp(reactionAge/.35f,0.f,1.f);
    const float torqueScale=1.f-.9f*reactionStrength*(1.f-recovery);
    for(size_t i=1;i<activeParts.size();++i) {
      if(!activeParts[i]) continue;
      const int index=native->GetRagdollSettings()->GetConstraintIndexForBodyIndex(int(i));
      const int parent=rig->parts[i].parent;
      if(index<0 || parent<0) continue;
      auto* joint=static_cast<JPH::SwingTwistConstraint*>(native->GetConstraint(index));
      const float partMass=motorSource->mParts[i].mMassPropertiesOverride.mMass;
      for(auto* motor:{&joint->GetSwingMotorSettings(),&joint->GetTwistMotorSettings()}) {
        motor->mSpringSettings=JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping,(4.f+2.f*recovery)*reactionStiffness,1.f);
        motor->SetTorqueLimit(partMass*8.f*torqueScale*reactionStiffness*reactionStiffness);
        }
      joint->SetSwingMotorState(JPH::EMotorState::Position);
      joint->SetTwistMotorState(JPH::EMotorState::Position);
      joint->SetTargetOrientationBS(targets[size_t(parent)].GetQuaternion().Conjugated()*targets[i].GetQuaternion());
      }
    if(benchHand!=nullptr) {
      benchHand->SetTargetPositionCS(targets[0].GetTranslation()-benchHandOrigin);
      benchHand->SetTargetOrientationBS(targets[0].GetQuaternion());
      }
    }

  void endReaction(JPH::PhysicsSystem& system) {
    auto& interface=system.GetBodyInterface();
    for(size_t i=0;i<native->GetConstraintCount();++i) {
      auto* joint=static_cast<JPH::SwingTwistConstraint*>(native->GetConstraint(int(i)));
      joint->SetSwingMotorState(JPH::EMotorState::Off);
      joint->SetTwistMotorState(JPH::EMotorState::Off);
      joint->SetEnabled(false);
      }
    for(size_t i=1;i<activeParts.size();++i) {
      if(!activeParts[i]) continue;
      const auto id=native->GetBodyID(int(i));
      interface.SetMotionType(id,JPH::EMotionType::Kinematic,JPH::EActivation::DontActivate);
      interface.SetObjectLayer(id,DynamicWorld::C_LivingBody);
      interface.SetMotionQuality(id,JPH::EMotionQuality::Discrete);
      interface.SetPositionAndRotation(id,targets[i].GetTranslation(),targets[i].GetQuaternion(),JPH::EActivation::DontActivate);
      interface.SetLinearAndAngularVelocity(id,JPH::Vec3::sZero(),JPH::Vec3::sZero());
      activeParts[i]=false;
      }
    reactionBones.clear();
    reactionStrength=reactionWeight=reactionAge=0.f;
    reactionStiffness=1.f;
    }

  // A body that dies keeps its living shape for a moment and loses it part by
  // part: the legs give first, then the arms, the trunk and the neck last.
  // The motors hold the joint angles of the moment of death, never a new pose.
  // Knocked off the feet, the legs keep their stance while the trunk tips over them
  // and give on the way down, and the trunk stays whole longer: a fall, not a faint.
  static void fadeTiming(std::string_view name, bool knockedOff, float& hold, float& fade) {
    if(named(name,"THIGH") || named(name,"CALF") || named(name,"FOOT") || named(name,"TOE") || named(name,"LEG")) {
      hold=knockedOff ? .12f : 0.f; fade=knockedOff ? .5f : .15f;
      }
    else if(named(name,"SPINE")) { hold=knockedOff ? .25f : .1f; fade=knockedOff ? .8f : .6f; }
    else if(named(name,"NECK") || named(name,"HEAD")) { hold=.15f; fade=.7f; }
    else { hold=.05f; fade=.45f; }
    }

  static float fadeStrength(std::string_view name) {
    if(named(name,"THIGH") || named(name,"CALF") || named(name,"FOOT") || named(name,"TOE") || named(name,"LEG")) return 4.f;
    if(named(name,"SPINE")) return 2.f;
    return 1.f;
    }

  void startFade(JPH::PhysicsSystem& system) {
    auto& interface=system.GetBodyInterface();
    fadeTargets.clear();
    for(size_t i=0;i<size_t(native->GetBodyCount());++i)
      fadeTargets.push_back(interface.GetRotation(native->GetBodyID(int(i))));
    fadeAge=0.f;
    stepFade(0.f);
    }

  void stepFade(float deltaSeconds) {
    if(fadeAge<0.f) return;
    fadeAge+=deltaSeconds;
    bool active=false;
    const auto* settings=native->GetRagdollSettings();
    for(size_t i=1;i<rig->parts.size();++i) {
      const int index=settings->GetConstraintIndexForBodyIndex(int(i));
      const int parent=rig->parts[i].parent;
      if(index<0 || parent<0) continue;
      auto* joint=static_cast<JPH::SwingTwistConstraint*>(native->GetConstraint(index));
      float hold=0,fade=1;
      fadeTiming(rig->parts[i].name,knockedOff,hold,fade);
      // a living stance carries the body: thrown, the legs bear it while the trunk tips
      const float strength=knockedOff ? fadeStrength(rig->parts[i].name) : 1.f;
      const float t=std::clamp((fadeAge-hold)/fade,0.f,1.f);
      const float weight=1.f-t*t*(3.f-2.f*t);
      if(weight<=0.f) {
        joint->SetSwingMotorState(JPH::EMotorState::Off);
        joint->SetTwistMotorState(JPH::EMotorState::Off);
        continue;
        }
      active=true;
      // The living reaction's strength: enough to keep a limb's shape for an
      // instant, too weak to carry the body, so the knees buckle under it.
      const float partMass=settings->mParts[i].mMassPropertiesOverride.mMass;
      for(auto* motor:{&joint->GetSwingMotorSettings(),&joint->GetTwistMotorSettings()}) {
        motor->mSpringSettings=JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping,6.f,1.f);
        motor->SetTorqueLimit(partMass*8.f*weight*strength);
        }
      joint->SetSwingMotorState(JPH::EMotorState::Position);
      joint->SetTwistMotorState(JPH::EMotorState::Position);
      joint->SetTargetOrientationBS(fadeTargets[size_t(parent)].Conjugated()*fadeTargets[i]);
      }
    if(!active) {
      fadeAge=-1.f;
      fadeTargets.clear();
      }
    }

  static std::mutex& cacheMutex() { static std::mutex value; return value; }
  CollisionWorld* world=nullptr;
  std::shared_ptr<Schema> rig;
  JPH::Ref<JPH::Ragdoll> native;
  Tempest::Matrix4x4 initialObject;
  JPH::Vec3 initialPelvis=JPH::Vec3::sZero();
  ObjectFrame object;
  float actualMass=0.f;
  std::vector<Tempest::Matrix4x4> references;
  std::vector<JPH::Mat44> corrections;
  std::vector<AncestorState> ancestors;
  std::vector<JPH::Mat44> ancestorCorrections;
  std::vector<size_t> poseBones;
  std::vector<BuoyancyPart> waterParts;
  bool legacyAncestors=false;
  uint32_t group=0;
  std::weak_ptr<zenkit::INpc> npc,culprit;
  uint64_t culpritUntilStep=0;
  uint64_t nextNoiseStep=0;
  bool kinematic=false;
  struct Hit { size_t part; Tempest::Vec3 pointCm,impulseNs; float strength,stiffness; };
  std::vector<Hit> hits;
  JPH::Ref<JPH::RagdollSettings> motorSource;
  bool motorReady=false;
  bool benchActive=false;
  JPH::Ref<JPH::SixDOFConstraint> benchHand;
  JPH::Vec3 benchHandOrigin=JPH::Vec3::sZero();
  std::vector<bool> activeParts;
  std::vector<size_t> reactionBones;
  float reactionStrength=0.f,reactionWeight=0.f,reactionAge=0.f,reactionStiffness=1.f,stepSeconds=0.f;
  float fadeAge=-1.f;
  bool  knockedOff=false;
  std::vector<JPH::Quat> fadeTargets;
  std::vector<JPH::Mat44> targets, scratch;
  std::vector<bool> dirty;
  std::vector<size_t> changed, moving;
  };

Ragdoll::Ragdoll(CollisionWorld& world, const Skeleton& skeleton, const HitZones& zones,
                 const Tempest::Matrix4x4* worldPose, size_t numBones,
                 const Tempest::Matrix4x4& objectMatrix, const Tempest::Vec3& initialVelocity, bool kinematic)
  :impl(std::make_unique<Impl>()) {
  // Opt-in input capture for reproducing creation failures with the exact game
  // skeleton and rendered pose. It neither queries nor mutates the physics world.
  if(const char* path=std::getenv("OPENGOTHIC_RAGDOLL_CREATION_DUMP");!kinematic && path!=nullptr && path[0]!=0) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    auto number=[&](float value) {
      if(std::isfinite(value)) w.Double(double(value));
      else w.String(std::isnan(value) ? "nan" : value<0.f ? "-inf" : "+inf");
      };
    auto vector=[&](const Tempest::Vec3& value) { w.StartArray(); number(value.x); number(value.y); number(value.z); w.EndArray(); };
    auto matrix=[&](const Tempest::Matrix4x4& value) {
      w.StartArray(); for(size_t i=0;i<16;++i) number(value.data()[i]); w.EndArray();
      };
    auto predicate=[&](const Tempest::Matrix4x4& value) {
      JPH::Mat44 rigid;
      float scale=0;
      const char* reason=nullptr;
      const bool accepted=rigidTransform(value,rigid,scale,&reason);
      w.StartObject(); w.Key("accepted"); w.Bool(accepted); w.Key("reason"); w.String(reason);
      w.Key("scale"); number(scale);
      w.Key("axis_lengths"); w.StartArray();
      for(size_t i=0;i<3;++i) {
        const auto* m=value.data()+i*4;
        number(JPH::Vec3(m[0],m[1],m[2]).Length());
        }
      w.EndArray();
      JPH::Vec3 axes[3];
      for(size_t i=0;i<3;++i) {
        const auto* m=value.data()+i*4;
        axes[i]=JPH::Vec3(m[0],m[1],m[2]).NormalizedOr(JPH::Vec3::sZero());
        }
      w.Key("normalized_dots"); w.StartArray(); number(axes[0].Dot(axes[1])); number(axes[0].Dot(axes[2])); number(axes[1].Dot(axes[2])); w.EndArray();
      w.Key("determinant"); number(axes[0].Cross(axes[1]).Dot(axes[2]));
      w.EndObject();
      };
    auto capsules=[&](const HitZones& value) {
      w.StartArray();
      for(const auto& c:value.caps) {
        w.StartObject(); w.Key("owner"); w.Uint64(c.owner);
        w.Key("a_bone"); w.Uint64(c.a.bone); w.Key("a"); vector(c.a.at);
        w.Key("b_bone"); w.Uint64(c.b.bone); w.Key("b"); vector(c.b.at);
        w.Key("radius"); number(c.r); w.Key("guess"); number(c.guess);
        w.Key("zone"); w.Uint(c.zone); w.Key("fitted"); w.Bool(c.fitted); w.EndObject();
        }
      w.EndArray();
      };
    HitZones canonical;
    canonical.setSkeleton(&skeleton);
    std::vector<size_t> owners;
    for(const auto& c:canonical.caps)
      if(c.owner<skeleton.nodes.size() && std::find(owners.begin(),owners.end(),c.owner)==owners.end()) owners.push_back(c.owner);
    auto depth=[&](size_t bone) {
      size_t n=0;
      for(size_t p=skeleton.nodes[bone].parent;p<skeleton.nodes.size() && n<skeleton.nodes.size();p=skeleton.nodes[p].parent) ++n;
      return n;
      };
    std::stable_sort(owners.begin(),owners.end(),[&](size_t a,size_t b) { return depth(a)<depth(b) || (depth(a)==depth(b) && a<b); });
    std::string first;
    if(worldPose==nullptr) first="null_world_pose";
    else if(numBones!=skeleton.nodes.size()) first="pose_count";
    else if(!zones.isValid()) first="invalid_hit_zones";
    else if(!finite(initialVelocity)) first="nonfinite_initial_velocity";
    else if(skeleton.tr.size()!=skeleton.nodes.size() || skeleton.nodes.empty()) first="invalid_skeleton_size";
    JPH::Mat44 rigid;
    float objectScale=0;
    const char* reason=nullptr;
    if(first.empty() && !rigidTransform(objectMatrix,rigid,objectScale,&reason)) first="object:"+std::string(reason);
    if(std::abs(objectScale-1.f)<1.e-4f) objectScale=1.f;
    if(first.empty() && owners.empty()) first="empty_canonical_schema";
    for(size_t bone:owners) {
      float scale=0;
      if(first.empty()) {
        const bool accepted=rigidTransform(skeleton.tr[bone],rigid,scale,&reason);
        if(!accepted || std::abs(scale-1.f)>1.e-3f)
          first="bind:"+std::to_string(bone)+":"+(accepted ? "bind_scale" : std::string(reason));
        }
      }
    for(size_t bone:owners) {
      float scale=0;
      if(first.empty()) {
        const bool accepted=rigidTransform(worldPose[bone],rigid,scale,&reason);
        if(!accepted || std::abs(scale-objectScale)>objectScale*1.e-3f)
          first="pose:"+std::to_string(bone)+":"+(accepted ? "object_scale_mismatch" : std::string(reason));
        }
      }
    w.StartObject(); w.Key("version"); w.Uint(1);
    w.Key("skeleton"); w.String(skeleton.name().data(),rapidjson::SizeType(skeleton.name().size()));
    w.Key("bone_count"); w.Uint64(numBones); w.Key("root_translation"); vector(skeleton.rootTr);
    w.Key("bbox"); w.StartArray(); vector(skeleton.bbox[0]); vector(skeleton.bbox[1]); w.EndArray();
    w.Key("collision_bbox"); w.StartArray(); vector(skeleton.bboxCol[0]); vector(skeleton.bboxCol[1]); w.EndArray();
    w.Key("object"); matrix(objectMatrix); w.Key("object_predicate"); predicate(objectMatrix);
    w.Key("velocity"); vector(initialVelocity); w.Key("first_rejection"); w.String(first.empty() ? "accepted_matrix_guards" : first.c_str());
    w.Key("physical_owners"); w.StartArray(); for(size_t i:owners) w.Uint64(i); w.EndArray();
    w.Key("provided_capsules"); capsules(zones); w.Key("canonical_capsules"); capsules(canonical);
    w.Key("nodes"); w.StartArray();
    for(size_t i=0;i<skeleton.nodes.size();++i) {
      const auto& node=skeleton.nodes[i];
      w.StartObject(); w.Key("name"); w.String(node.name.c_str());
      w.Key("parent"); w.Int64(node.parent==noBone ? -1 : int64_t(node.parent));
      w.Key("local"); matrix(node.tr);
      if(i<skeleton.tr.size()) { w.Key("bind"); matrix(skeleton.tr[i]); w.Key("bind_predicate"); predicate(skeleton.tr[i]); }
      if(worldPose!=nullptr && i<numBones) { w.Key("world"); matrix(worldPose[i]); w.Key("world_predicate"); predicate(worldPose[i]); }
      w.EndObject();
      }
    w.EndArray(); w.EndObject();
    static std::mutex captureMutex;
    std::lock_guard<std::mutex> lock(captureMutex);
    std::ofstream output(path,std::ios::app);
    if(output) output.write(buffer.GetString(),std::streamsize(buffer.GetSize())).put('\n');
    }
  if(worldPose==nullptr || numBones!=skeleton.nodes.size() || !zones.isValid() || !finite(initialVelocity)) return;
  if(skeleton.tr.size()!=skeleton.nodes.size() || skeleton.nodes.empty()) return;
  if(!objectFrame(objectMatrix,impl->object)) return;
  {
    std::lock_guard<std::mutex> lock(Impl::cacheMutex());
    impl->rig=Impl::schema(skeleton);
    if(impl->rig->parts.empty()) return;
    }
  std::vector<JPH::Mat44> pose;
  for(size_t bone:impl->rig->boneIndices) {
    JPH::Mat44 matrix;
    if(!posedFrame(worldPose[bone],impl->object,matrix)) return;
    pose.push_back(matrix);
    impl->references.push_back(worldPose[bone]);
    impl->corrections.push_back(Impl::visualCorrection(worldPose[bone],matrix));
    }
  impl->poseBones=impl->rig->boneIndices;
  for(const auto& ancestor:impl->rig->ancestors) {
    JPH::Mat44 rigid;
    if(!posedFrame(worldPose[ancestor.bone],impl->object,rigid)) return;
    AncestorState reference;
    reference.bone=uint32_t(ancestor.bone);
    reference.anchorBone=uint32_t(impl->rig->parts[ancestor.anchorPart].bone);
    reference.referenceTransform=worldPose[ancestor.bone];
    impl->ancestors.push_back(reference);
    impl->ancestorCorrections.push_back(pose[ancestor.anchorPart].InversedRotationTranslation()*affineMeters(reference.referenceTransform));
    impl->poseBones.push_back(ancestor.bone);
    }
  JPH::Ref<JPH::RagdollSettings> settings;
  {
    std::lock_guard<std::mutex> lock(Impl::cacheMutex());
    settings=impl->object.uniform ? Impl::settings(*impl->rig,impl->object.scale.GetX()) :
                                   Impl::bakedSettings(*impl->rig,impl->object,impl->references,pose);
    }
  settings=Impl::massSettings(std::move(settings),impl->object.volumeScale);
  impl->actualMass=impl->rig->totalMass*impl->object.volumeScale;
  impl->kinematic=kinematic;
  impl->hits.reserve(4);
  if(kinematic) {
    impl->motorSource=settings;
    settings=Impl::kinematicSettings(*settings);
    impl->targets=pose;
    impl->scratch.resize(pose.size());
    impl->dirty.resize(pose.size(),false);
    impl->changed.reserve(pose.size());
    impl->moving.reserve(pose.size());
    impl->activeParts.resize(pose.size(),false);
    impl->reactionBones.reserve(pose.size());
    }
  impl->group=world.registerRagdoll(*this);
  impl->world=&world;
  try {
    impl->native=settings->CreateRagdoll(impl->group,0,&world.physicsSystem());
    }
  catch(...) {
    world.unregisterRagdoll(*this);
    impl->world=nullptr;
    throw;
    }
  if(impl->native==nullptr) { world.unregisterRagdoll(*this); impl->world=nullptr; return; }
  impl->native->SetPose(JPH::RVec3::sZero(),pose.data());
  impl->native->SetLinearVelocity(CollisionWorld::toMeters(initialVelocity));
  impl->initialObject=objectMatrix;
  impl->initialPelvis=JPH::Vec3(world.physicsSystem().GetBodyInterface().GetPosition(impl->native->GetBodyID(0)));
  impl->native->AddToPhysicsSystem(kinematic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
  if(!kinematic) impl->startFade(world.physicsSystem());
  resetBuoyancy();
  }

Ragdoll::~Ragdoll() { detach(); }

void Ragdoll::detach() {
  impl->waterParts.clear();
  if(impl->benchHand!=nullptr) {
    impl->world->physicsSystem().RemoveConstraint(impl->benchHand.GetPtr());
    impl->benchHand=nullptr;
    }
  if(impl->native!=nullptr) {
    impl->native->RemoveFromPhysicsSystem();
    impl->native=nullptr;
    }
  if(impl->world!=nullptr) impl->world->unregisterRagdoll(*this);
  impl->world=nullptr;
  }

bool Ragdoll::valid() const { return impl->world!=nullptr && impl->native!=nullptr; }
bool Ragdoll::isKinematic() const { return impl->kinematic && impl->reactionBones.empty(); }

bool Ragdoll::queueHit(size_t bone, const Tempest::Vec3& pointCm,
                       const Tempest::Vec3& impulseNs, float strength, Npc* culprit, float stiffness) {
  if(!valid() || !finite(pointCm) || !finite(impulseNs) || !std::isfinite(strength) || strength<=0.f || strength>1.f ||
     std::abs(pointCm.x)>1.e8f || std::abs(pointCm.y)>1.e8f || std::abs(pointCm.z)>1.e8f ||
     impulseNs.quadLength()<=0.f || impulseNs.quadLength()>1.e8f) return false;
  const auto found=std::find(impl->rig->boneIndices.begin(),impl->rig->boneIndices.end(),bone);
  if(found==impl->rig->boneIndices.end()) return false;
  if(!std::isfinite(stiffness)) return false;
  impl->hits.push_back({size_t(found-impl->rig->boneIndices.begin()),pointCm,impulseNs,strength,std::clamp(stiffness,1.f,4.f)});
  setNoiseCulprit(culprit);
  return true;
  }

void Ragdoll::setNpc(Npc* npc) {
  impl->npc=npc!=nullptr ? npc->handlePtr() : std::weak_ptr<zenkit::INpc>();
  updateBuoyancyFractions();
  }

std::vector<Ragdoll::BuoyancyPart>& Ragdoll::buoyancyParts() { return impl->waterParts; }
const std::vector<Ragdoll::BuoyancyPart>& Ragdoll::buoyancyParts() const { return impl->waterParts; }

void Ragdoll::resetBuoyancy() {
  impl->waterParts.clear();
  // isKinematic() also becomes false during living hit reactions. The original
  // rig policy, rather than the current motion of one limb, excludes all living
  // bodies from corpse buoyancy.
  if(!valid() || impl->kinematic) return;
  impl->waterParts.resize(partCount());
  for(size_t i=0;i<partCount();++i) {
    impl->waterParts[i].body=impl->native->GetBodyID(int(i)).GetIndexAndSequenceNumber();
    // A new corpse can start crossing water on its first native step. Treat
    // that as entry; restoreRagdollBuoyancy resets this for loaded bodies.
    impl->waterParts[i].initialized=true;
    }
  updateBuoyancyFractions();
  }

void Ragdoll::updateBuoyancyFractions() {
  if(impl->waterParts.empty()) return;
  std::string name=impl->rig->name;
  for(char& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
  bool sinks=named(name,"SKELETON") || named(name,"GOLEM") || named(name,"STONEGUARDIAN");
  if(const auto* npc=noiseNpc()) {
    const auto guild=npc->trueGuild();
    if(npc->world().version().game==2) {
      sinks=sinks || guild==GIL_GOBBO_SKELETON || guild==GIL_SUMMONED_GOBBO_SKELETON ||
            guild==GIL_SKELETON || guild==GIL_SUMMONED_SKELETON || guild==GIL_SKELETON_MAGE ||
            guild==GIL_SHADOWBEAST_SKELETON || guild==GIL_STONEGOLEM || guild==GIL_SUMMONED_GOLEM ||
            guild==GIL_Stoneguardian || guild==GIL_SummonedGuardian;
      }
    else {
      sinks=sinks || guild==GIL_G1_SKELETON || guild==GIL_G1_GOLEM;
      }
    }
  // A fraction above one leaves weight even when fully immersed. Stone and
  // bones still receive water drag and entry splashes, but settle on the bed.
  for(size_t i=0;i<impl->waterParts.size();++i)
    impl->waterParts[i].fraction=sinks ? 1.5f : partBuoyancyFraction(impl->rig->parts[i].name);
  }

void Ragdoll::setNoiseCulprit(Npc* culprit) {
  impl->culprit=culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<zenkit::INpc>();
  impl->culpritUntilStep=impl->world!=nullptr ? impl->world->simulationStep()+300 : 0;
  }

Npc* Ragdoll::noiseNpc() const {
  const auto npc=impl->npc.lock();
  return npc!=nullptr ? static_cast<Npc*>(npc->user_ptr) : nullptr;
  }

Npc* Ragdoll::noiseCulprit() const {
  if(impl->world==nullptr || impl->world->simulationStep()>impl->culpritUntilStep) return nullptr;
  const auto npc=impl->culprit.lock();
  return npc!=nullptr ? static_cast<Npc*>(npc->user_ptr) : nullptr;
  }

bool Ragdoll::acceptNoise(uint64_t step) {
  if(step<impl->nextNoiseStep) return false;
  impl->nextNoiseStep=step+30;
  return true;
  }

uint64_t Ragdoll::noiseId() const {
  return (uint64_t(1)<<63)|impl->group;
  }

float Ragdoll::reactionWeight() const { return impl->reactionWeight; }

const std::vector<size_t>& Ragdoll::reactionBones() const { return impl->reactionBones; }

bool Ragdoll::configureBench(BenchMode mode, BenchPelvis pelvis) {
  const char* flag=std::getenv("OPENGOTHIC_MELEE_BENCH");
  if(flag==nullptr || flag[0]==0 || std::string_view(flag)=="0" || !valid() || !impl->kinematic || partCount()!=24) return false;
  auto& system=impl->world->physicsSystem();
  if(!impl->ensureMotors(system)) return false;
  clearBench();
  impl->endReaction(system);
  auto& interface=system.GetBodyInterface();
  if(pelvis==BenchPelvis::Dynamic) {
    JPH::SixDOFConstraintSettings hand;
    hand.mSpace=JPH::EConstraintSpace::LocalToBodyCOM;
    impl->benchHandOrigin=impl->targets[0].GetTranslation();
    hand.mPosition1=impl->benchHandOrigin;
    hand.mPosition2=-interface.GetShape(impl->native->GetBodyID(0))->GetCenterOfMass();
    hand.mNumVelocityStepsOverride=12;
    hand.mNumPositionStepsOverride=10;
    for(int axis=0;axis<JPH::SixDOFConstraint::EAxis::Num;++axis) {
      auto& motor=hand.mMotorSettings[axis];
      motor.mSpringSettings=JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping,6.f,1.f);
      if(axis<JPH::SixDOFConstraint::EAxis::NumTranslation) motor.SetForceLimit(impl->actualMass*50.f);
      else motor.SetTorqueLimit(impl->actualMass*10.f);
      }
    impl->benchHand=static_cast<JPH::SixDOFConstraint*>(interface.CreateConstraint(&hand,JPH::BodyID(),impl->native->GetBodyID(0)));
    for(int axis=0;axis<JPH::SixDOFConstraint::EAxis::Num;++axis)
      impl->benchHand->SetMotorState(JPH::SixDOFConstraint::EAxis(axis),JPH::EMotorState::Position);
    system.AddConstraint(impl->benchHand.GetPtr());
    }
  for(size_t i=0;i<partCount();++i) {
    const auto& name=impl->rig->parts[i].name;
    const bool arm=named(name," R ") && (named(name,"CLAVICLE") || named(name,"UPPERARM") || named(name,"FOREARM") || named(name,"HAND") || named(name,"FINGER"));
    const bool dynamic=i==0 ? pelvis==BenchPelvis::Dynamic : mode==BenchMode::Full || named(name,"SPINE") || arm;
    impl->activeParts[i]=dynamic;
    const auto id=impl->native->GetBodyID(int(i));
    interface.SetObjectLayer(id,dynamic ? DynamicWorld::C_ActiveBody : DynamicWorld::C_LivingBody);
    interface.SetMotionType(id,dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic,
                            dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    interface.SetMotionQuality(id,dynamic ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
    JPH::BodyLockWrite lock(system.GetBodyLockInterface(),id);
    if(lock.Succeeded()) lock.GetBody().SetAllowSleeping(!dynamic);
    }
  for(size_t i=1;i<partCount();++i) {
    const int index=impl->native->GetRagdollSettings()->GetConstraintIndexForBodyIndex(int(i));
    if(index>=0) impl->native->GetConstraint(index)->SetEnabled(impl->activeParts[i]);
    }
  impl->native->ResetWarmStart();
  impl->benchActive=true;
  impl->driveMotors();
  std::fprintf(stderr,"[melee-bench] rig parts=%zu dynamic=%zu mode=%s pelvis=%s\n",partCount(),
    size_t(std::count(impl->activeParts.begin(),impl->activeParts.end(),true)),
    mode==BenchMode::Full ? "full" : "chain",pelvis==BenchPelvis::Dynamic ? "dynamic" : "kinematic");
  return true;
  }

void Ragdoll::clearBench() {
  if(!valid() || !impl->benchActive) return;
  auto& system=impl->world->physicsSystem();
  auto& interface=system.GetBodyInterface();
  if(impl->benchHand!=nullptr) {
    system.RemoveConstraint(impl->benchHand.GetPtr());
    impl->benchHand=nullptr;
    }
  if(impl->activeParts[0]) {
    const auto id=impl->native->GetBodyID(0);
    interface.SetMotionType(id,JPH::EMotionType::Kinematic,JPH::EActivation::DontActivate);
    interface.SetObjectLayer(id,DynamicWorld::C_LivingBody);
    interface.SetMotionQuality(id,JPH::EMotionQuality::Discrete);
    interface.SetPositionAndRotation(id,impl->targets[0].GetTranslation(),impl->targets[0].GetQuaternion(),JPH::EActivation::DontActivate);
    interface.SetLinearAndAngularVelocity(id,JPH::Vec3::sZero(),JPH::Vec3::sZero());
    impl->activeParts[0]=false;
    }
  impl->endReaction(system);
  for(auto id:impl->native->GetBodyIDs()) {
    JPH::BodyLockWrite lock(system.GetBodyLockInterface(),id);
    if(lock.Succeeded()) lock.GetBody().SetAllowSleeping(true);
    }
  impl->benchActive=false;
  }

Ragdoll::BenchStats Ragdoll::benchStats() const {
  BenchStats result;
  if(!valid() || !impl->kinematic) return result;
  auto& system=impl->world->physicsSystem();
  double squaredError=0;
  std::vector<JPH::AABox> feet;
  feet.reserve(4);
  for(size_t i=0;i<partCount();++i) {
    JPH::BodyLockRead lock(system.GetBodyLockInterface(),impl->native->GetBodyID(int(i)));
    if(!lock.Succeeded()) { result.finite=false; continue; }
    const auto& body=lock.GetBody();
    const float linear=body.GetLinearVelocity().Length()*100.f;
    const float angular=body.GetAngularVelocity().Length();
    const float error=(JPH::Vec3(body.GetPosition())-impl->targets[i].GetTranslation()).Length()*100.f;
    result.finite=result.finite && std::isfinite(linear) && std::isfinite(angular) && std::isfinite(error);
    result.maxLinearCmS=std::max(result.maxLinearCmS,linear);
    result.maxAngularRadS=std::max(result.maxAngularRadS,angular);
    if(impl->activeParts[i]) {
      ++result.dynamicParts;
      squaredError+=double(error)*error;
      result.poseMaxCm=std::max(result.poseMaxCm,error);
      }
    const auto& name=impl->rig->parts[i].name;
    if(named(name,"FOOT") || named(name,"TOE")) {
      const auto bounds=body.GetWorldSpaceBounds();
      const float bottom=bounds.mMin.GetY()*100.f;
      const auto* shape=body.GetShape();
      const auto targetCom=impl->targets[i]*JPH::Mat44::sTranslation(shape->GetCenterOfMass());
      const float targetBottom=shape->GetWorldSpaceBounds(targetCom,JPH::Vec3::sOne()).mMin.GetY()*100.f;
      result.maxFootDropCm=std::max(result.maxFootDropCm,targetBottom-bottom);
      const bool validBounds=finite(CollisionWorld::toCentimeters(bounds.mMin)) && finite(CollisionWorld::toCentimeters(bounds.mMax));
      result.finite=result.finite && validBounds && std::isfinite(targetBottom);
      if(validBounds) feet.push_back(bounds);
      }
    }
  // World queries take native body locks too. All foot bounds are copied and
  // their read locks released before looking up the supporting static surface.
  constexpr uint32_t floorMask=(1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object);
  for(const auto& bounds:feet) {
    const auto center=CollisionWorld::toCentimeters(bounds.GetCenter());
    const float bottom=bounds.mMin.GetY()*100.f;
    const Tempest::Vec3 from {center.x,bounds.mMax.GetY()*100.f+100.f,center.z};
    const Tempest::Vec3 to {center.x,bottom-200.f,center.z};
    const auto floor=impl->world->rayCast(from,to,floorMask);
    if(!floor.hasHit) { ++result.footFloorMisses; continue; }
    ++result.footFloorSamples;
    result.maxFootPenetrationCm=std::max(result.maxFootPenetrationCm,floor.point.y-bottom);
    }
  if(result.dynamicParts!=0) result.poseRmsCm=float(std::sqrt(squaredError/double(result.dynamicParts)));
  return result;
  }

bool Ragdoll::moveKinematic(const Tempest::Matrix4x4* worldPose, size_t numBones,
                            const Tempest::Matrix4x4& objectMatrix, float deltaSeconds) {
  if(!valid() || !impl->kinematic || worldPose==nullptr || numBones!=boneCount() ||
     !std::isfinite(deltaSeconds) || deltaSeconds<=0.f) return false;
  ObjectFrame object;
  if(!objectFrame(objectMatrix,object)) return false;
  for(JPH::uint axis=0;axis<3;++axis)
    if(std::abs(object.scale[axis]-impl->object.scale[axis])>impl->object.scale[axis]*1.e-3f) return false;
  // Reuse the solved pose. Only changed native owners need matrix decomposition
  // or a later BodyInterface update; slots and animation-only bones are ignored.
  impl->changed.clear();
  for(size_t i=0;i<impl->references.size();++i) {
    const auto& frame=worldPose[impl->rig->boneIndices[i]];
    if(std::equal(frame.data(),frame.data()+16,impl->references[i].data())) continue;
    if(!posedFrame(frame,object,impl->scratch[i])) return false;
    impl->changed.push_back(i);
    }
  for(const auto& ancestor:impl->ancestors) {
    JPH::Mat44 frame;
    if(!posedFrame(worldPose[ancestor.bone],object,frame)) return false;
    }
  // Validation is complete before any target or public pose is changed.
  for(size_t i:impl->changed) {
    const auto& frame=worldPose[impl->rig->boneIndices[i]];
    impl->references[i]=frame;
    impl->targets[i]=impl->scratch[i];
    impl->corrections[i]=Impl::visualCorrection(frame,impl->targets[i]);
    impl->dirty[i]=true;
    }
  for(size_t i=0;i<impl->ancestors.size();++i) {
    auto& ancestor=impl->ancestors[i];
    const size_t part=impl->rig->ancestors[i].anchorPart;
    const auto& frame=worldPose[ancestor.bone];
    if(!impl->dirty[part] && std::equal(frame.data(),frame.data()+16,ancestor.referenceTransform.data())) continue;
    ancestor.referenceTransform=frame;
    impl->ancestorCorrections[i]=impl->targets[part].InversedRotationTranslation()*affineMeters(frame);
    }
  impl->initialObject=objectMatrix;
  return true;
  }

void Ragdoll::prepareKinematicStep(float deltaSeconds, float targetFraction) {
  if(!valid()) return;
  impl->stepSeconds=deltaSeconds;
  auto& system=impl->world->physicsSystem();
  auto& interface=system.GetBodyInterface();
  if(!impl->hits.empty()) {
    const bool ready=!impl->kinematic || impl->ensureMotors(system);
    if(ready) {
      for(const auto& hit:impl->hits) {
        const auto point=CollisionWorld::toMeters(hit.pointCm);
        const size_t part=impl->kinematic ? impl->startReaction(hit.part,point,hit.strength,hit.stiffness,system) : hit.part;
        if(part!=noBone)
          interface.AddImpulse(impl->native->GetBodyID(int(part)),JPH::Vec3(hit.impulseNs.x,hit.impulseNs.y,hit.impulseNs.z),point);
        }
      }
    impl->hits.clear();
    }
  if(!impl->kinematic) {
    impl->stepFade(deltaSeconds);
    return;
    }
  if(impl->benchActive || !impl->reactionBones.empty()) impl->driveMotors();
  for(size_t i=0;i<impl->dirty.size();++i) {
    if(!impl->dirty[i]) continue;
    impl->dirty[i]=targetFraction<1.f;
    if(impl->activeParts[i]) continue;
    const auto& target=impl->targets[i];
    const auto id=impl->native->GetBodyID(int(i));
    // Interaction attachment and script repositioning can change a ready pose
    // abruptly. Teleport instead of turning that discontinuity into contact
    // velocity; compare against the native pose even if targets were coalesced.
    if((JPH::Vec3(interface.GetPosition(id))-target.GetTranslation()).LengthSq()>.5f*.5f) {
      interface.SetPositionAndRotation(id,target.GetTranslation(),target.GetQuaternion(),JPH::EActivation::DontActivate);
      interface.SetLinearAndAngularVelocity(id,JPH::Vec3::sZero(),JPH::Vec3::sZero());
      }
    else {
      if(targetFraction<1.f) {
        const auto from=interface.GetPosition(id);
        interface.MoveKinematic(id,from+(JPH::RVec3(target.GetTranslation())-from)*targetFraction,
                                interface.GetRotation(id).SLERP(target.GetQuaternion(),targetFraction),deltaSeconds);
        }
      else
        interface.MoveKinematic(id,target.GetTranslation(),target.GetQuaternion(),deltaSeconds);
      impl->moving.push_back(i);
      }
    }
  }

void Ragdoll::finishKinematicStep() {
  if(!valid() || !impl->kinematic) return;
  auto& interface=impl->world->physicsSystem().GetBodyInterface();
  // MoveKinematic sets velocities rather than a persistent destination. Stop
  // once the fixed step reaches it, including when no new animation arrives.
  for(size_t i:impl->moving)
    interface.SetLinearAndAngularVelocity(impl->native->GetBodyID(int(i)),JPH::Vec3::sZero(),JPH::Vec3::sZero());
  impl->moving.clear();
  if(!impl->benchActive && !impl->reactionBones.empty()) {
    // The native rig may remain displaced against an obstacle; its visible
    // influence fades out before the bounded return to the ordinary kin pose.
    constexpr float hold=.12f, duration=1.1f;
    impl->reactionAge+=impl->stepSeconds;
    const float t=std::clamp((impl->reactionAge-hold)/(duration-hold),0.f,1.f);
    impl->reactionWeight=impl->reactionStrength*(1.f-t*t*(3.f-2.f*t));
    if(impl->reactionAge>=duration) impl->endReaction(impl->world->physicsSystem());
    }
  }

size_t Ragdoll::partCount() const { return valid() ? impl->native->GetBodyCount() : 0; }
size_t Ragdoll::boneForBodyId(uint32_t id) const {
  if(!valid()) return noBone;
  for(size_t i=0;i<partCount();++i)
    if(impl->native->GetBodyID(int(i)).GetIndexAndSequenceNumber()==id)
      return impl->rig->boneIndices[i];
  return noBone;
  }
std::vector<uint32_t> Ragdoll::diagnosticBodyIds() const {
  std::vector<uint32_t> result;
  result.reserve(partCount());
  for(size_t i=0;i<partCount();++i)
    result.push_back(impl->native->GetBodyID(int(i)).GetIndexAndSequenceNumber());
  return result;
  }
size_t Ragdoll::boneCount() const { return impl->rig!=nullptr ? impl->rig->numBones : 0; }
float Ragdoll::mass() const { return valid() ? impl->actualMass : 0.f; }

const std::vector<size_t>& Ragdoll::boneIndices() const {
  static const std::vector<size_t> empty;
  return impl->rig!=nullptr ? impl->rig->boneIndices : empty;
  }

const std::vector<size_t>& Ragdoll::poseIndices() const {
  return impl->poseBones;
  }

bool Ragdoll::getPose(std::vector<Tempest::Matrix4x4>& worldBones) const {
  if(!valid()) { worldBones.clear(); return false; }
  worldBones.resize(boneCount());
  const auto& interface=impl->world->physicsSystem().GetBodyInterface();
  for(size_t i=0;i<partCount();++i)
    worldBones[impl->rig->boneIndices[i]]=toMatrix(interface.GetWorldTransform(impl->native->GetBodyID(int(i)))*impl->corrections[i],1.f);
  for(size_t i=0;i<impl->ancestors.size();++i) {
    const size_t part=impl->rig->ancestors[i].anchorPart;
    worldBones[impl->ancestors[i].bone]=toMatrix(interface.GetWorldTransform(impl->native->GetBodyID(int(part)))*impl->ancestorCorrections[i],1.f);
    }
  return true;
  }

Tempest::Matrix4x4 Ragdoll::pelvisTransform() const {
  if(!valid()) return Tempest::Matrix4x4::mkIdentity();
  return toMatrix(impl->world->physicsSystem().GetBodyInterface().GetWorldTransform(impl->native->GetBodyID(0))*impl->corrections[0],1.f);
  }

Tempest::Matrix4x4 Ragdoll::rootTransform() const {
  if(!valid()) return Tempest::Matrix4x4::mkIdentity();
  if(impl->kinematic) return impl->initialObject;
  auto result=impl->initialObject;
  const auto at=impl->world->physicsSystem().GetBodyInterface().GetPosition(impl->native->GetBodyID(0));
  const auto delta=CollisionWorld::toCentimeters(JPH::Vec3(at)-impl->initialPelvis);
  float m[16]; std::copy_n(result.data(),16,m);
  m[12]+=delta.x; m[13]+=delta.y; m[14]+=delta.z;
  result.setData(m);
  return result;
  }

bool Ragdoll::isSleeping() const { return valid() && !impl->native->IsActive(); }

bool Ragdoll::settled() const {
  if(!valid()) return false;
  if(isSleeping()) return true;
  const auto& interface=impl->world->physicsSystem().GetBodyInterface();
  for(auto id:impl->native->GetBodyIDs())
    if(interface.GetLinearVelocity(id).LengthSq()>.0025f || interface.GetAngularVelocity(id).LengthSq()>.01f)
      return false;
  return true;
  }

bool Ragdoll::onBack() const {
  if(!valid()) return false;
  // Gothic bind bone axes differ between species. Carry the model's forward
  // reference through the bind frame instead of assuming a pelvis local axis.
  const auto front=impl->rig->parts.front().bind.InversedRotationTranslation().Multiply3x3(JPH::Vec3::sAxisZ());
  return (impl->world->physicsSystem().GetBodyInterface().GetRotation(impl->native->GetBodyID(0))*front).GetY()>0.f;
  }

void Ragdoll::activate() { if(valid()) impl->native->Activate(); }

void Ragdoll::setAllowSleeping(bool allow) {
  if(!valid()) return;
  auto& system=impl->world->physicsSystem();
  for(auto id:impl->native->GetBodyIDs()) {
    JPH::BodyLockWrite lock(system.GetBodyLockInterface(),id);
    if(lock.Succeeded()) lock.GetBody().SetAllowSleeping(allow);
    }
  // SetAllowSleeping alone does not awaken a sleeping native body. Activate
  // after all write locks are released so the policy also applies immediately.
  if(!allow) impl->native->Activate();
  }

void Ragdoll::addImpulse(const Tempest::Vec3& impulse, Npc* culprit) {
  if(!valid() || impl->kinematic || !finite(impulse)) return;
  setNoiseCulprit(culprit);
  auto& interface=impl->world->physicsSystem().GetBodyInterface();
  for(size_t i=0;i<partCount();++i) {
    const auto* settings=impl->native->GetRagdollSettings();
    const float ratio=settings->mParts[i].mMassPropertiesOverride.mMass/mass();
    interface.AddImpulse(impl->native->GetBodyID(int(i)),JPH::Vec3(impulse.x,impulse.y,impulse.z)*ratio);
    }
  }

void Ragdoll::knockOff(const Tempest::Vec3& direction, float impulseNs, Npc* culprit) {
  if(!valid() || impl->kinematic || !finite(direction) || !std::isfinite(impulseNs)) return;
  Tempest::Vec3 d(direction.x,0.f,direction.z);
  if(d.length()<1e-3f) return;
  d=Tempest::Vec3::normalize(d);
  setNoiseCulprit(culprit);
  impl->knockedOff=true;
  // The blow alone moves 75 kg by under a metre a second; a man thrown down moves
  // his trunk at about two. The heavier the blow, the harder he goes down.
  const float speed=std::clamp(1.4f+1.2f*impulseNs/60.f,1.4f,2.6f);
  auto& interface=impl->world->physicsSystem().GetBodyInterface();
  const auto* settings=impl->native->GetRagdollSettings();
  for(size_t i=0;i<partCount();++i) {
    const auto& name=impl->rig->parts[i].name;
    float share=1.f;
    if(named(name,"THIGH") || named(name,"CALF") || named(name,"FOOT") || named(name,"TOE") || named(name,"LEG"))
      share=0.f; else
    if(named(name,"PELVIS"))
      share=.4f;
    if(share<=0.f) continue;
    const float partMass=settings->mParts[i].mMassPropertiesOverride.mMass;
    interface.AddImpulse(impl->native->GetBodyID(int(i)),JPH::Vec3(d.x,0.f,d.z)*(speed*share*partMass));
    }
  }

Ragdoll::State Ragdoll::state() const {
  State result;
  if(!valid() || impl->kinematic) return result;
  result.skeleton=impl->rig->name;
  result.rootTransform=rootTransform();
  result.ancestors=impl->ancestors;
  result.legacyAncestors=impl->legacyAncestors;
  const auto& interface=impl->world->physicsSystem().GetBodyInterface();
  for(size_t i=0;i<partCount();++i) {
    const auto id=impl->native->GetBodyID(int(i));
    PartState p;
    p.bone=uint32_t(impl->rig->boneIndices[i]);
    p.transform=toMatrix(interface.GetWorldTransform(id),1.f);
    p.referenceTransform=impl->references[i];
    const auto com=interface.GetCenterOfMassPosition(id);
    p.centerOfMassMeters={float(com.GetX()),float(com.GetY()),float(com.GetZ())};
    const auto rotation=interface.GetRotation(id);
    p.rotation={rotation.GetX(),rotation.GetY(),rotation.GetZ(),rotation.GetW()};
    const auto linear=interface.GetLinearVelocity(id);
    p.linearVelocityMeters={linear.GetX(),linear.GetY(),linear.GetZ()};
    p.linearVelocity=CollisionWorld::toCentimeters(interface.GetLinearVelocity(id));
    const auto angular=interface.GetAngularVelocity(id);
    p.angularVelocity={angular.GetX(),angular.GetY(),angular.GetZ()};
    p.sleeping=!interface.IsActive(id);
    result.parts.push_back(p);
    }
  return result;
  }

bool Ragdoll::restore(const State& state) {
  if(!valid() || impl->kinematic || (state.version!=1 && state.version!=2 && state.version!=3) || state.skeleton!=impl->rig->name || state.parts.size()!=partCount()) return false;
  ObjectFrame object;
  if(!objectFrame(state.rootTransform,object) || (state.version==1 && !object.uniform)) return false;
  for(JPH::uint i=0;i<3;++i)
    if(std::abs(object.scale[i]-impl->object.scale[i])>impl->object.scale[i]*1.e-3f) return false;
  std::vector<JPH::Mat44> pose;
  std::vector<JPH::Mat44> referenceFrames;
  std::vector<Tempest::Matrix4x4> references;
  std::vector<JPH::Mat44> corrections;
  for(size_t i=0;i<state.parts.size();++i) {
    const auto& p=state.parts[i];
    JPH::Mat44 matrix;
    float scale=1;
    if(p.bone!=impl->rig->boneIndices[i] || !finite(p.linearVelocity) || !finite(p.angularVelocity) ||
       p.linearVelocity.x*p.linearVelocity.x+p.linearVelocity.y*p.linearVelocity.y+p.linearVelocity.z*p.linearVelocity.z>2.5e9f ||
       p.angularVelocity.x*p.angularVelocity.x+p.angularVelocity.y*p.angularVelocity.y+p.angularVelocity.z*p.angularVelocity.z>4.e4f ||
       !rigidTransform(p.transform,matrix,scale) ||
       std::abs(scale-(state.version==1 ? object.scale.GetX() : 1.f))>(state.version==1 ? object.scale.GetX() : 1.f)*1.e-3f) return false;
    if(state.version>=3 && !nativePrecision(p)) return false;
    pose.push_back(matrix);
    const auto& reference=state.version==1 ? p.transform : p.referenceTransform;
    JPH::Mat44 frame;
    if(!posedFrame(reference,object,frame)) return false;
    references.push_back(reference);
    referenceFrames.push_back(frame);
    corrections.push_back(Impl::visualCorrection(reference,frame));
    }
  std::vector<AncestorState> ancestors;
  std::vector<JPH::Mat44> ancestorCorrections;
  auto poseBones=impl->rig->boneIndices;
  const bool legacyAncestors=state.version<3 || state.legacyAncestors;
  if(state.version>=3) {
    if(legacyAncestors) {
      if(!state.ancestors.empty()) return false;
      }
    else {
      if(state.ancestors.size()!=impl->rig->ancestors.size()) return false;
      for(size_t i=0;i<state.ancestors.size();++i) {
        const auto& reference=state.ancestors[i];
        const auto& expected=impl->rig->ancestors[i];
        JPH::Mat44 rigid;
        if(reference.bone!=expected.bone || reference.anchorBone!=impl->rig->parts[expected.anchorPart].bone ||
           !posedFrame(reference.referenceTransform,object,rigid)) return false;
        ancestors.push_back(reference);
        ancestorCorrections.push_back(referenceFrames[expected.anchorPart].InversedRotationTranslation()*affineMeters(reference.referenceTransform));
        poseBones.push_back(reference.bone);
        }
      }
    }
  // Build a complete candidate outside the broad phase. Bad state, failed shape
  // creation and exhausted body capacity leave the old corpse untouched.
  JPH::Ref<JPH::Ragdoll> replacement;
  try {
    JPH::Ref<JPH::RagdollSettings> settings;
    {
      std::lock_guard<std::mutex> lock(Impl::cacheMutex());
      settings=object.uniform ? Impl::settings(*impl->rig,object.scale.GetX()) :
                                Impl::bakedSettings(*impl->rig,object,references,referenceFrames);
      }
    settings=Impl::massSettings(std::move(settings),object.volumeScale);
    replacement=settings->CreateRagdoll(impl->group,0,&impl->world->physicsSystem());
    if(replacement==nullptr) return false;
    replacement->SetPose(JPH::RVec3::sZero(),pose.data());
    if(state.version>=3) {
      auto& system=impl->world->physicsSystem();
      auto& interface=system.GetBodyInterface();
      for(size_t i=0;i<state.parts.size();++i) {
        const auto& p=state.parts[i];
        const auto id=replacement->GetBodyID(int(i));
        const auto rotation=JPH::Quat(p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w);
        // SetPose reconstructs a quaternion from a matrix. Use the saved native
        // quaternion unchanged, then restore the COM rather than round-tripping
        // it through centimetres and through the shape's bone-origin offset.
        interface.SetPositionAndRotation(id,pose[i].GetTranslation(),rotation,JPH::EActivation::DontActivate);
        JPH::BodyLockWrite lock(system.GetBodyLockInterface(),id);
        if(!lock.Succeeded()) return false;
        auto& body=lock.GetBody();
        if(body.IsInBroadPhase()) return false;
        const auto target=JPH::RVec3(p.centerOfMassMeters.x,p.centerOfMassMeters.y,p.centerOfMassMeters.z);
        const auto current=body.GetCenterOfMassPosition();
        const auto delta=JPH::Vec3(target-current);
        const auto offset=rotation*body.GetShape()->GetCenterOfMass();
        for(JPH::uint component=0;component<3;++component) {
          // This only repairs representational rounding: the public pose must
          // already place the candidate within a few float operations of COM.
          const float bound=std::max(1.e-7f,8.f*(ulp(float(current[component]))+ulp(pose[i].GetTranslation()[component])+ulp(offset[component])));
          if(std::abs(delta[component])>bound) return false;
          }
        body.AddPositionStep(delta);
        if(body.GetCenterOfMassPosition()!=target) return false;
        body.CalculateWorldSpaceBoundsInternal();
        const auto publicPose=toMatrix(body.GetWorldTransform(),1.f);
        if(!samePublicPosition(publicPose,p.transform)) return false;
        }
      }
    }
  catch(const std::exception&) { return false; }
  impl->native->RemoveFromPhysicsSystem();
  impl->native=replacement;
  impl->native->AddToPhysicsSystem(JPH::EActivation::DontActivate);
  // A restored body carries no muscles: the saved state is all there is.
  impl->fadeAge=-1.f;
  impl->fadeTargets.clear();
  auto& interface=impl->world->physicsSystem().GetBodyInterface();
  for(size_t i=0;i<state.parts.size();++i) {
    const auto& p=state.parts[i];
    const auto id=impl->native->GetBodyID(int(i));
    const auto linear=state.version>=3 ? JPH::Vec3(p.linearVelocityMeters.x,p.linearVelocityMeters.y,p.linearVelocityMeters.z) : CollisionWorld::toMeters(p.linearVelocity);
    interface.SetLinearAndAngularVelocity(id,linear,JPH::Vec3(p.angularVelocity.x,p.angularVelocity.y,p.angularVelocity.z));
    if(p.sleeping) interface.DeactivateBody(id); else interface.ActivateBody(id);
    }
  impl->initialObject=state.rootTransform;
  impl->initialPelvis=JPH::Vec3(interface.GetPosition(impl->native->GetBodyID(0)));
  impl->object=object;
  impl->actualMass=impl->rig->totalMass*object.volumeScale;
  impl->references=std::move(references);
  impl->corrections=std::move(corrections);
  impl->ancestors=std::move(ancestors);
  impl->ancestorCorrections=std::move(ancestorCorrections);
  impl->poseBones=std::move(poseBones);
  impl->legacyAncestors=legacyAncestors;
  resetBuoyancy();
  impl->world->restoreRagdollBuoyancy(*this);
  return true;
  }

bool Ragdoll::translateState(const Skeleton& skeleton, State& state, const Tempest::Vec3& displacement) {
  if(!finite(displacement) || state.parts.empty() || state.skeleton!=skeleton.name() ||
     (state.version!=1 && state.version!=2 && state.version!=3)) return false;
  const Tempest::Vec3 position {state.rootTransform.data()[12],state.rootTransform.data()[13],state.rootTransform.data()[14]};
  return relocateState(skeleton,state,position+displacement);
  }

bool Ragdoll::relocateState(const Skeleton& skeleton, State& state, const Tempest::Vec3& target) {
  if(!finite(target) || state.parts.empty() || (state.version!=1 && state.version!=2 && state.version!=3) || state.skeleton!=skeleton.name()) return false;
  ObjectFrame object;
  if(!objectFrame(state.rootTransform,object) || (state.version==1 && !object.uniform)) return false;
  auto translated=state;
  for(size_t component=0;component<3;++component) {
    const float value=component==0 ? target.x : component==1 ? target.y : target.z;
    translated.rootTransform.set(3,int(component),value);
    }
  ObjectFrame movedObject;
  if(!objectFrame(translated.rootTransform,movedObject)) return false;
  try {
    std::lock_guard<std::mutex> lock(Impl::cacheMutex());
    const auto rig=Impl::schema(skeleton);
    if(state.parts.size()!=rig->parts.size() || rig->parts.empty()) return false;
    std::vector<JPH::Mat44> frames;
    std::vector<Tempest::Matrix4x4> references;
    for(size_t i=0;i<state.parts.size();++i) {
      const auto& p=state.parts[i];
      JPH::Mat44 rigid;
      float scale=0;
      const float expected=state.version==1 ? object.scale.GetX() : 1.f;
      if(p.bone!=rig->parts[i].bone || !finite(p.linearVelocity) || !finite(p.angularVelocity) ||
         p.linearVelocity.x*p.linearVelocity.x+p.linearVelocity.y*p.linearVelocity.y+p.linearVelocity.z*p.linearVelocity.z>2.5e9f ||
         p.angularVelocity.x*p.angularVelocity.x+p.angularVelocity.y*p.angularVelocity.y+p.angularVelocity.z*p.angularVelocity.z>4.e4f ||
         !rigidTransform(p.transform,rigid,scale) || std::abs(scale-expected)>expected*1.e-3f ||
         (state.version>=3 && !nativePrecision(p))) return false;
      const auto reference=state.version==1 ? p.transform : p.referenceTransform;
      if(!posedFrame(reference,object,rigid)) return false;
      references.push_back(reference);
      frames.push_back(rigid);
      }
    if(state.version>=3) {
      if(state.legacyAncestors) {
        if(!state.ancestors.empty()) return false;
        }
      else {
        if(state.ancestors.size()!=rig->ancestors.size()) return false;
        for(size_t i=0;i<state.ancestors.size();++i) {
          const auto& reference=state.ancestors[i];
          const auto& expected=rig->ancestors[i];
          JPH::Mat44 rigid;
          if(reference.bone!=expected.bone || reference.anchorBone!=rig->parts[expected.anchorPart].bone ||
             !posedFrame(reference.referenceTransform,object,rigid)) return false;
          }
        }
      }
    JPH::Ref<JPH::RagdollSettings> settings;
    if(state.version>=3)
      settings=object.uniform ? Impl::settings(*rig,object.scale.GetX()) : Impl::bakedSettings(*rig,object,references,frames);
    for(size_t i=0;i<state.parts.size();++i) {
      const auto& original=state.parts[i];
      auto& part=translated.parts[i];
      // Preserve the public local offsets before moving to an absolute target.
      // Forming float(target - distantRoot) first would discard target bits.
      const auto relative=[&](size_t component,float value) {
        return float(double(original.transform.data()[12+component])-double(state.rootTransform.data()[12+component])+double(value));
        };
      const Tempest::Vec3 position {relative(0,target.x),relative(1,target.y),relative(2,target.z)};
      if(state.version>=3) {
        const auto rotation=JPH::Quat(original.rotation.x,original.rotation.y,original.rotation.z,original.rotation.w);
        const auto shapeCom=settings->mParts[i].GetShape()->GetCenterOfMass();
        const auto previous=JPH::Vec3(original.centerOfMassMeters.x,original.centerOfMassMeters.y,original.centerOfMassMeters.z);
        const auto oldPose=toMatrix(JPH::RMat44::sRotationTranslation(rotation,JPH::RVec3(previous)).PreTranslated(-shapeCom),1.f);
        if(!samePublicPosition(oldPose,original.transform)) return false;
        // The requested public origin defines this explicit teleport. Adding a
        // separately rounded world-space delta to COM would fail cancellation
        // when moving a distant body near zero, and lose its local shape offset.
        auto com=CollisionWorld::toMeters(position)+rotation*shapeCom;
        const float requested[] {position.x,position.y,position.z};
        for(JPH::uint component=0;component<3;++component)
          if(requested[component]==original.transform.data()[12+component]) com.SetComponent(component,previous[component]);
        part.centerOfMassMeters={com.GetX(),com.GetY(),com.GetZ()};
        part.transform=toMatrix(JPH::RMat44::sRotationTranslation(rotation,JPH::RVec3(com)).PreTranslated(-shapeCom),1.f);
        if(!nativePrecision(part)) return false;
        }
      else {
        part.transform.set(3,0,position.x);
        part.transform.set(3,1,position.y);
        part.transform.set(3,2,position.z);
        }
      JPH::Mat44 rigid;
      float scale=0;
      if(!rigidTransform(part.transform,rigid,scale)) return false;
      }
    }
  catch(const std::exception&) { return false; }
  state=std::move(translated);
  return true;
  }

float Ragdoll::speciesMass(std::string_view name) {
  std::string normalized(name);
  for(char& c:normalized) c=char(std::toupper(static_cast<unsigned char>(c)));
  name=normalized;
  if(named(name,"TROLL")) return 600.f;
  if(named(name,"ORC")) return 150.f;
  if(named(name,"WARG")) return 60.f;
  if(named(name,"WOLF")) return 40.f;
  if(named(name,"SCAVENGER")) return 45.f;
  if(named(name,"MOLERAT")) return 30.f;
  if(named(name,"GOBBO") || named(name,"GOBLIN")) return 18.f;
  if(named(name,"SKELETON")) return 45.f;
  if(named(name,"SNAPPER")) return 90.f;
  if(named(name,"LURKER")) return 80.f;
  if(named(name,"SHADOWBEAST")) return 250.f;
  return 75.f;
  }
