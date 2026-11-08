#include "testbench_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include "physics/fracture.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

namespace {

// Acceptance guards of the destruction system (reports/physics/2026-11-destruction-system):
// the pieces of one broken prop 5, 13 and 15 seconds of game time after its break.
constexpr uint64_t SampleMs[]={5000,13000,15000};

const char* groupName(asset::MaterialGroup group) {
  switch(group) {
    case asset::MaterialGroup::WOOD: return "WOOD";
    case asset::MaterialGroup::METAL: return "METAL";
    case asset::MaterialGroup::STONE: return "STONE";
    case asset::MaterialGroup::CLAY: return "CLAY";
    case asset::MaterialGroup::EARTH: return "EARTH";
    default: return "UNDEF";
    }
  }

// Eigenvectors of a symmetric 3x3 matrix by Jacobi rotations; columns of v.
void eigen(double m[3][3],double v[3][3],double value[3]) {
  for(int i=0;i<3;++i)for(int j=0;j<3;++j)v[i][j]=i==j?1:0;
  for(int sweep=0;sweep<50;++sweep) {
    const double off=m[0][1]*m[0][1]+m[0][2]*m[0][2]+m[1][2]*m[1][2];
    if(off<1e-18)break;
    for(int p=0;p<2;++p)for(int q=p+1;q<3;++q) {
      if(std::abs(m[p][q])<1e-30)continue;
      const double theta=(m[q][q]-m[p][p])/(2*m[p][q]);
      const double t=(theta>=0?1.:-1.)/(std::abs(theta)+std::sqrt(theta*theta+1));
      const double c=1/std::sqrt(t*t+1),s=t*c;
      for(int k=0;k<3;++k) {
        const double mkp=m[k][p],mkq=m[k][q];
        m[k][p]=c*mkp-s*mkq;m[k][q]=s*mkp+c*mkq;
        }
      for(int k=0;k<3;++k) {
        const double mpk=m[p][k],mqk=m[q][k];
        m[p][k]=c*mpk-s*mqk;m[q][k]=s*mpk+c*mqk;
        }
      for(int k=0;k<3;++k) {
        const double vkp=v[k][p],vkq=v[k][q];
        v[k][p]=c*vkp-s*vkq;v[k][q]=s*vkp+c*vkq;
        }
      }
    }
  for(int i=0;i<3;++i)value[i]=m[i][i];
  }

Vec3 apply(const Matrix4x4& matrix,Vec3 point) {matrix.project(point);return point;}

Vec3 rotate(const Matrix4x4& matrix,const Vec3& direction) {
  return apply(matrix,direction)-apply(matrix,Vec3());
  }

JPH::Mat44 joltPose(const Matrix4x4& pose) {
  return JPH::Mat44(JPH::Vec4(pose.at(0,0),pose.at(0,1),pose.at(0,2),0),
                    JPH::Vec4(pose.at(1,0),pose.at(1,1),pose.at(1,2),0),
                    JPH::Vec4(pose.at(2,0),pose.at(2,1),pose.at(2,2),0),
                    JPH::Vec4(pose.at(3,0)*.01f,pose.at(3,1)*.01f,pose.at(3,2)*.01f,1));
  }

// Deepest native contact of two convex (or compound) shapes, centimetres; 0 without one.
float penetration(const JPH::Shape* a,const JPH::Mat44& poseA,const JPH::Shape* b,const JPH::Mat44& poseB) {
  JPH::CollideShapeSettings settings;settings.mMaxSeparationDistance=0;
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> contacts;
  JPH::CollisionDispatch::sCollideShapeVsShape(a,b,JPH::Vec3::sOne(),JPH::Vec3::sOne(),
    poseA.PreTranslated(a->GetCenterOfMass()),poseB.PreTranslated(b->GetCenterOfMass()),{},{},settings,contacts);
  float depth=0;
  for(const auto& contact:contacts.mHits)depth=std::max(depth,contact.mPenetrationDepth*100.f);
  return depth;
  }

float median(std::vector<float> values) {
  if(values.empty())return 0;
  std::sort(values.begin(),values.end());
  const size_t n=values.size();
  return n%2 ? values[n/2] : (values[n/2-1]+values[n/2])*.5f;
  }

}

void fracturePiecesBreak(FracturePieces& state,World& world,uintptr_t parent,std::string_view label) {
  std::optional<DynamicWorld::MovableVobSnapshot> body;
  for(auto& entry:world.physic()->movableVobSnapshot())
    if(entry.id==parent) {body=entry;break;}
  if(!body)return;
  const auto mesh=FractureMesh::load(body->visual);
  if(!mesh || mesh->pieces.empty())return;
  FracturePieces::Break item;
  item.parent=parent;item.label=std::string(label);item.visual=body->visual;item.version=mesh->version;
  item.breakTime=world.tickCount();
  // The barrel's axis: the volume-weighted centre of all pieces, in the visual's frame.
  Vec3 axis;double total=0;
  for(const auto& piece:mesh->pieces) {axis+=piece.center*float(piece.volume);total+=piece.volume;}
  if(total>0)axis/=float(total);
  std::string upper=body->visual;
  for(auto& c:upper)c=char(std::toupper(static_cast<unsigned char>(c)));
  const bool barrel=upper.find("BARREL")!=std::string::npos;
  for(size_t i=0;i<mesh->pieces.size();++i) {
    const auto& source=mesh->pieces[i];
    FracturePieces::Piece piece;
    piece.index=i;piece.volume=source.volume;piece.group=groupName(source.group);
    piece.center=source.center;piece.hull=source.hull;
    for(const auto& triangle:source.triangles)
      for(const auto& vertex:triangle.vertices)piece.vertices.push_back(vertex.position);
    std::sort(piece.vertices.begin(),piece.vertices.end(),[](const Vec3& a,const Vec3& b) {
      return std::tie(a.x,a.y,a.z)<std::tie(b.x,b.y,b.z);
      });
    piece.vertices.erase(std::unique(piece.vertices.begin(),piece.vertices.end(),[](const Vec3& a,const Vec3& b) {
      return a.x==b.x && a.y==b.y && a.z==b.z;
      }),piece.vertices.end());
    Vec3 mean;
    for(const auto& v:piece.vertices)mean+=v;
    if(!piece.vertices.empty())mean/=float(piece.vertices.size());
    double m[3][3]={},v[3][3],value[3];
    for(const auto& p:piece.vertices) {
      const double d[3]={p.x-mean.x,p.y-mean.y,p.z-mean.z};
      for(int r=0;r<3;++r)for(int c=0;c<3;++c)m[r][c]+=d[r]*d[c];
      }
    eigen(m,v,value);
    int order[3]={0,1,2};
    std::sort(order,order+3,[&](int a,int b){return value[a]>value[b];});
    Vec3 axes[3];
    for(int k=0;k<3;++k) {
      axes[k]=Vec3(float(v[0][order[k]]),float(v[1][order[k]]),float(v[2][order[k]]));
      float lo=std::numeric_limits<float>::infinity(),hi=-lo;
      for(const auto& p:piece.vertices) {const float s=Vec3::dotProduct(p,axes[k]);lo=std::min(lo,s);hi=std::max(hi,s);}
      piece.extent[k]=piece.vertices.empty()?0.f:hi-lo;
      }
    piece.longAxis=axes[0];piece.thin=axes[2];
    // A stave's own bulge rivals its width, so its thin direction is radial from the barrel axis.
    piece.stave=barrel && mesh->version==3 && std::abs(axes[0].y)>.85f && piece.extent[0]>3.f*piece.extent[1];
    if(piece.stave) {
      const Vec3 radial(piece.center.x-axis.x,0,piece.center.z-axis.z);
      if(radial.length()>1.f)piece.thin=Vec3::normalize(radial);
      }
    piece.flat=piece.stave || piece.extent[2]<=.35f*piece.extent[1];
    piece.start=apply(body->transform,piece.center);
    item.pieces.push_back(std::move(piece));
    }
  state.breaks.push_back(std::move(item));
  }

void fracturePiecesTick(FracturePieces& state,World& world) {
  for(auto& item:state.breaks) {
    if(item.samples>=std::size(SampleMs) || world.tickCount()-item.breakTime<SampleMs[item.samples])continue;
    const uint64_t at=SampleMs[item.samples++];
    std::vector<const DynamicWorld::FragmentSnapshot*> parts(item.pieces.size(),nullptr);
    const auto all=world.physic()->fragmentSnapshot();
    for(const auto& part:all)
      if(part.parentId==item.parent && part.piece<parts.size())parts[part.piece]=&part;
    const bool last=at==SampleMs[std::size(SampleMs)-1];
    std::vector<float> displacement,resting,volumes;
    std::vector<JPH::Mat44> poses(parts.size());
    std::vector<Vec3> low(parts.size()),high(parts.size());
    size_t missing=0,flat=0,onEdge=0,upright=0,sleeping=0,fell=0;
    float maxJitter=0,maxFloor=0,maxVisible=0,maxPieces=0,maxSpeed=0;
    std::string rows="[";
    for(size_t i=0;i<parts.size();++i) {
      auto& piece=item.pieces[i];
      volumes.push_back(float(piece.volume));
      if(parts[i]==nullptr) {++missing;continue;}
      const auto& part=*parts[i];
      const auto center=apply(part.transform,piece.center);
      const float moved=(center-piece.start).length();
      displacement.push_back(moved);
      // Through the ground: no floor under the centre, or the centre more than 1 cm below it.
      const auto floor=world.physic()->landRay(center+Vec3(0,100,0),400);
      const bool below=!floor.hasCol || Vec3::dotProduct(center-floor.v,floor.n)<-1.f;
      if(below)++fell;else resting.push_back(moved);
      const auto thin=Vec3::normalize(rotate(part.transform,piece.thin));
      const auto along=Vec3::normalize(rotate(part.transform,piece.longAxis));
      // Angle of the broad face from vertical, and of the long axis from vertical, degrees.
      const float face=std::asin(std::min(1.f,std::abs(thin.y)))*180.f/float(M_PI);
      const float length=std::acos(std::min(1.f,std::abs(along.y)))*180.f/float(M_PI);
      if(piece.flat) {++flat;if(face<30.f)++onEdge;}
      if(piece.flat && length<30.f)++upright;
      if(part.sleeping)++sleeping;
      const float speed=part.velocity.length();
      maxSpeed=std::max(maxSpeed,speed);
      std::string extra;
      if(at==13000)piece.at13=center;
      if(last) {
        const float jitter=(center-piece.at13).length();
        maxJitter=std::max(maxJitter,jitter);
        float visible=0,ground=0;
        poses[i]=joltPose(part.transform);
        low[i]=Vec3(1e9f,1e9f,1e9f);high[i]=Vec3(-1e9f,-1e9f,-1e9f);
        for(const auto& v:piece.vertices) {
          const auto p=apply(part.transform,v);
          low[i]=Vec3(std::min(low[i].x,p.x),std::min(low[i].y,p.y),std::min(low[i].z,p.z));
          high[i]=Vec3(std::max(high[i].x,p.x),std::max(high[i].y,p.y),std::max(high[i].z,p.z));
          if(floor.hasCol)visible=std::max(visible,-Vec3::dotProduct(p-floor.v,floor.n));
          }
        if(floor.hasCol && !below) {
          // The ground as a native box whose top face is the floor plane under the piece.
          const auto normal=JPH::Vec3(floor.n.x,floor.n.y,floor.n.z).Normalized();
          const JPH::ShapeRefC box=new JPH::BoxShape(JPH::Vec3(2,.5f,2),0);
          const auto top=JPH::Vec3(floor.v.x,floor.v.y,floor.v.z)*.01f;
          const auto pose=JPH::Mat44::sRotationTranslation(JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),normal),top-normal*.5f);
          if(piece.hull!=nullptr)ground=penetration(piece.hull.GetPtr(),poses[i],box.GetPtr(),pose);
          }
        maxFloor=std::max(maxFloor,ground);maxVisible=std::max(maxVisible,visible);
        extra=",\"jitter_cm\":"+ragdollNumber(jitter)+",\"floor_penetration_cm\":"+ragdollNumber(ground)+
          ",\"visible_floor_penetration_cm\":"+ragdollNumber(visible);
        }
      if(rows.size()>1)rows+=',';
      rows+="{\"piece\":"+std::to_string(i)+",\"group\":"+jsonStr(piece.group)+",\"volume_cm3\":"+
        ragdollNumber(float(piece.volume))+",\"extent_cm\":["+ragdollNumber(piece.extent[0])+","+
        ragdollNumber(piece.extent[1])+","+ragdollNumber(piece.extent[2])+"],\"flat\":"+(piece.flat?"true":"false")+
        ",\"stave\":"+(piece.stave?"true":"false")+",\"center_cm\":"+ragdollVector(center)+
        ",\"displacement_cm\":"+ragdollNumber(moved)+",\"face_from_vertical_deg\":"+ragdollNumber(face)+
        ",\"long_from_vertical_deg\":"+ragdollNumber(length)+",\"sleeping\":"+(part.sleeping?"true":"false")+
        ",\"speed_cm_s\":"+ragdollNumber(speed)+",\"fell\":"+(below?"true":"false")+extra+"}";
      }
    rows+="]";
    std::string pairs;
    if(last) {
      size_t worstA=0,worstB=0;
      for(size_t i=0;i<parts.size();++i)
        for(size_t j=i+1;j<parts.size();++j) {
          if(parts[i]==nullptr || parts[j]==nullptr || item.pieces[i].hull==nullptr || item.pieces[j].hull==nullptr)continue;
          if(low[i].x>high[j].x+3 || low[j].x>high[i].x+3 || low[i].y>high[j].y+3 || low[j].y>high[i].y+3 ||
             low[i].z>high[j].z+3 || low[j].z>high[i].z+3)continue;
          const float depth=penetration(item.pieces[i].hull.GetPtr(),poses[i],item.pieces[j].hull.GetPtr(),poses[j]);
          if(depth>maxPieces) {maxPieces=depth;worstA=i;worstB=j;}
          }
      pairs=",\"max_floor_penetration_cm\":"+ragdollNumber(maxFloor)+",\"max_visible_floor_penetration_cm\":"+
        ragdollNumber(maxVisible)+",\"max_piece_penetration_cm\":"+ragdollNumber(maxPieces)+
        ",\"worst_piece_pair\":["+std::to_string(worstA)+","+std::to_string(worstB)+"],\"max_jitter_cm\":"+
        ragdollNumber(maxJitter);
      }
    const float largest=volumes.empty()?0.f:*std::max_element(volumes.begin(),volumes.end());
    const float middle=median(volumes);
    event("destruction2_pieces","\"case\":"+jsonStr(item.label)+",\"visual\":"+jsonStr(item.visual)+
      ",\"version\":"+std::to_string(item.version)+",\"after_break_ms\":"+std::to_string(at)+
      ",\"pieces\":"+std::to_string(item.pieces.size())+",\"missing\":"+std::to_string(missing)+
      ",\"fell\":"+std::to_string(fell)+",\"median_displacement_cm\":"+ragdollNumber(median(resting))+
      ",\"median_displacement_all_cm\":"+ragdollNumber(median(displacement))+
      ",\"flat\":"+std::to_string(flat)+",\"flat_face_within_30_of_vertical\":"+std::to_string(onEdge)+
      ",\"flat_long_axis_within_30_of_vertical\":"+std::to_string(upright)+
      ",\"sleeping\":"+std::to_string(sleeping)+",\"max_speed_cm_s\":"+ragdollNumber(maxSpeed)+
      ",\"largest_volume_cm3\":"+ragdollNumber(largest)+",\"median_volume_cm3\":"+ragdollNumber(middle)+
      ",\"largest_to_median\":"+ragdollNumber(middle>0?largest/middle:0.f)+pairs+",\"rows\":"+rows);
    }
  }

std::optional<std::pair<Vec3,Vec3>> fracturePiecesView(World& world,const std::vector<uintptr_t>& ids,
                                                       const Vec3& camera,const Vec3& target) {
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(std::find(ids.begin(),ids.end(),body.id)==ids.end())continue;
    auto center=(body.localBounds[0]+body.localBounds[1])*.5f;body.transform.project(center);
    return std::make_pair(center+camera,center+target);
    }
  return std::nullopt;
  }

}
