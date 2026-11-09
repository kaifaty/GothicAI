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
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

namespace {

// Acceptance guards of the destruction system (reports/physics/2026-11-destruction-system):
// the pieces of one broken prop 0.5-15 seconds of game time after its break. The early
// samples catch pieces through the ground; jitter is the travel from 13 to 15 s.
constexpr uint64_t SampleMs[]={500,1000,2000,5000,13000,15000};

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
  // A later break of the same prop is a remnant's: its own label and samples from its moment.
  const auto remnant=size_t(std::count_if(state.breaks.begin(),state.breaks.end(),[parent](const auto& b) {return b.parent==parent;}));
  item.parent=parent;item.label=std::string(label)+(remnant>0?"-remnant-"+std::to_string(remnant):"");
  item.visual=body->visual;item.version=mesh->version;
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
    piece.index=i;piece.volume=source.volume;piece.group=groupName(source.group);piece.material=source.group;
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
  // Where each piece starts: centre over the ground under it and native hull depth into it.
  std::string start="[";
  float deepest=0;
  size_t under=0;
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(part.parentId!=parent || part.piece>=item.pieces.size())continue;
    item.pieces[part.piece].bondGroup=part.bondGroup;
    item.groups=std::max(item.groups,size_t(part.bondGroup)+1);
    const auto center=apply(part.transform,item.pieces[part.piece].center);
    const auto floor=world.physic()->landRay(center+Vec3(0,100,0),400);
    const float height=floor.hasCol?Vec3::dotProduct(center-floor.v,floor.n):0.f;
    const float native=world.physic()->fragmentGroundPenetration(parent,part.piece);
    deepest=std::max(deepest,native);
    under+=floor.hasCol && height<-1.f;
    if(start.size()>1)start+=',';
    start+="["+std::to_string(part.piece)+","+ragdollNumber(height)+","+ragdollNumber(native)+"]";
    }
  event("destruction2_start","\"case\":"+jsonStr(item.label)+",\"remnant\":"+std::to_string(remnant)+",\"visual\":"+jsonStr(item.visual)+",\"pieces\":"+
    std::to_string(item.pieces.size())+",\"groups\":"+std::to_string(item.groups)+",\"below_ground\":"+std::to_string(under)+",\"max_native_ground_penetration_cm\":"+
    ragdollNumber(deepest)+",\"piece_height_native\":"+start+"]");
  state.breaks.push_back(std::move(item));
  }

// Frame by frame for 5 s: the first frame a piece's centre is under the ground, with the frame before it.
void fracturePiecesTrack(FracturePieces::Break& item,World& world) {
  const uint64_t now=world.tickCount();
  if(now-item.breakTime>5000)return;
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(part.parentId!=item.parent || part.piece>=item.pieces.size())continue;
    auto& piece=item.pieces[part.piece];
    if(piece.fellReported)continue;
    const auto center=apply(part.transform,piece.center);
    const auto floor=world.physic()->landRay(center+Vec3(0,100,0),400);
    const float height=floor.hasCol?Vec3::dotProduct(center-floor.v,floor.n):-1000.f;
    if(height<-1.f) {
      piece.fellReported=true;
      event("destruction2_fell","\"case\":"+jsonStr(item.label)+",\"visual\":"+jsonStr(item.visual)+",\"piece\":"+
        std::to_string(part.piece)+",\"group\":"+jsonStr(piece.group)+",\"extent_cm\":["+ragdollNumber(piece.extent[0])+","+
        ragdollNumber(piece.extent[1])+","+ragdollNumber(piece.extent[2])+"],\"after_break_ms\":"+std::to_string(now-item.breakTime)+
        ",\"frame_ms\":"+std::to_string(now-piece.lastTime)+",\"floor_hit\":"+(floor.hasCol?"true":"false")+
        ",\"floor_normal\":"+ragdollVector(floor.n)+",\"height_cm\":"+ragdollNumber(height)+
        ",\"center_cm\":"+ragdollVector(center)+",\"velocity_cm_s\":"+ragdollVector(part.velocity)+
        ",\"omega\":"+ragdollVector(part.angularVelocity)+",\"before\":{\"height_cm\":"+ragdollNumber(piece.lastHeight)+
        ",\"native_ground_penetration_cm\":"+ragdollNumber(piece.lastNative)+",\"center_cm\":"+ragdollVector(piece.lastCenter)+
        ",\"velocity_cm_s\":"+ragdollVector(piece.lastVelocity)+",\"omega\":"+ragdollVector(piece.lastOmega)+"}");
      continue;
      }
    piece.lastTime=now;piece.lastCenter=center;piece.lastVelocity=part.velocity;piece.lastOmega=part.angularVelocity;
    piece.lastHeight=height;piece.lastNative=world.physic()->fragmentGroundPenetration(item.parent,part.piece);
    }
  }

// The bond groups of the broken fixture prop, the largest by volume first: pieces, body and
// share of the prop's volume (equal volumes by group number).
struct RemnantGroup {
  uint32_t group=0;
  std::vector<size_t> pieces;
  Matrix4x4 transform;
  double volumeShare=0;
  Vec3 low,high; // world bounds of its drawn vertices, the largest group only
  };

std::vector<RemnantGroup> fracturePiecesGroups(World& world,Destruction& destruction) {
  std::vector<RemnantGroup> result;
  const auto snapshot=world.physic()->fragmentSnapshot();
  std::shared_ptr<const FractureMesh> mesh;
  for(const auto& part:snapshot) {
    if(!destructionOwns(destruction,part.parentId))continue;
    if(mesh==nullptr) {
      for(const auto& body:world.physic()->movableVobSnapshot())
        if(body.id==part.parentId) {mesh=FractureMesh::load(body.visual);break;}
      if(mesh==nullptr || !(mesh->volume>0))return {};
      }
    auto found=std::find_if(result.begin(),result.end(),[&](const auto& g){return g.group==part.bondGroup;});
    if(found==result.end()) {result.push_back({part.bondGroup,{},part.transform,0,{},{}});found=result.end()-1;}
    found->pieces.push_back(part.piece);
    if(part.piece<mesh->pieces.size())found->volumeShare+=mesh->pieces[part.piece].volume/mesh->volume;
    }
  std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.volumeShare>b.volumeShare;});
  if(!result.empty() && mesh!=nullptr) {
    // The largest group's world bounds by its drawn vertices: where the actor stands to strike it.
    auto& largest=result.front();
    largest.low=Vec3(1e9f,1e9f,1e9f);largest.high=Vec3(-1e9f,-1e9f,-1e9f);
    for(const auto piece:largest.pieces)
      for(const auto& triangle:mesh->pieces[piece].triangles)
        for(const auto& vertex:triangle.vertices) {
          const auto p=apply(largest.transform,vertex.position);
          largest.low=Vec3(std::min(largest.low.x,p.x),std::min(largest.low.y,p.y),std::min(largest.low.z,p.z));
          largest.high=Vec3(std::max(largest.high.x,p.x),std::max(largest.high.y,p.y),std::max(largest.high.z,p.z));
          }
    }
  return result;
  }

std::string remnantGroupRows(const std::vector<RemnantGroup>& groups) {
  std::string rows="[";
  for(const auto& group:groups) {
    if(rows.size()>1)rows+=',';
    rows+="{\"group\":"+std::to_string(group.group)+",\"volume_fraction\":"+ragdollNumber(float(group.volumeShare))+",\"pieces\":[";
    for(size_t i=0;i<group.pieces.size();++i)rows+=(i?",":"")+std::to_string(group.pieces[i]);
    rows+="]}";
    }
  return rows+"]";
  }

// `destruction2 remnant <n>`: re-aim the actor at the largest group before each swing (the same
// side and stand-off as the first blow: 75 cm before its -z bound, facing +z) and swing the
// native attack until the prop has more bodies, or n swings were spent.
void fracturePiecesRemnantTick(FracturePieces& state,Destruction& destruction,const DestructionContext& context,World& world) {
  auto& attack=state.remnant;
  if(!attack.running)return;
  auto* actor=destruction.destructionMeleeActor;
  const auto parts=fracturePiecesGroups(world,destruction);
  const auto complete=[&](bool broken) {
    attack.running=false;context.waitUntil=context.time;
    event("destruction2_remnant_complete","\"case\":"+jsonStr(destruction.destructionCase)+",\"ordinal\":"+std::to_string(attack.ordinal)+
      ",\"broken\":"+(broken?"true":"false")+",\"swings\":"+std::to_string(attack.swings)+",\"limit\":"+std::to_string(attack.limit)+
      ",\"bodies_before\":"+std::to_string(attack.bodies)+",\"bodies_after\":"+std::to_string(parts.size())+
      ",\"largest_pieces_before\":"+std::to_string(attack.pieces)+",\"largest_fraction_before\":"+ragdollNumber(float(attack.largest))+
      ",\"largest_pieces_after\":"+std::to_string(parts.empty()?0:parts.front().pieces.size())+
      ",\"largest_fraction_after\":"+ragdollNumber(parts.empty()?0.f:float(parts.front().volumeShare))+
      ",\"actor_cm\":"+(actor?ragdollVector(actor->position()):std::string("null"))+",\"groups\":"+remnantGroupRows(parts));
    };
  if(parts.size()>attack.bodies) {complete(true);return;}
  if(actor==nullptr || parts.empty() || world.tickCount()>=attack.deadline) {
    attack.running=false;context.waitUntil=0;arenaAbort("destruction remnant lost its actor or prop");return;
    }
  if(world.tickCount()<attack.next)return;
  if(attack.swings>=attack.limit) {complete(false);return;}
  const auto& largest=parts.front();
  const auto center=(largest.low+largest.high)*.5f;
  Vec3 stand(center.x,center.y,largest.low.z-75.f);
  const auto floor=world.physic()->landRay(Vec3(stand.x,center.y+100,stand.z),300);
  if(floor.hasCol)stand.y=floor.v.y;
  actor->stopWalking();actor->clearSpeed();actor->setPosition(stand);
  actor->setDirection(destruction.destructionMeleeFacing);actor->updateTransform();
  const bool accepted=actor->doAttack(Npc::Anim::Attack,BS_HIT);
  if(accepted)++attack.swings;
  attack.next=world.tickCount()+(accepted?2200:100);
  event("destruction2_remnant_attempt","\"case\":"+jsonStr(destruction.destructionCase)+",\"ordinal\":"+std::to_string(attack.ordinal)+
    ",\"accepted\":"+(accepted?"true":"false")+",\"swing\":"+std::to_string(attack.swings)+",\"actor_cm\":"+ragdollVector(stand)+
    ",\"target_center_cm\":"+ragdollVector(center)+",\"target_pieces\":"+std::to_string(largest.pieces.size())+
    ",\"bodies\":"+std::to_string(parts.size()));
  }

void fracturePiecesTick(FracturePieces& state,Destruction& destruction,const DestructionContext& context,World& world) {
  fracturePiecesRemnantTick(state,destruction,context,world);
  for(auto& item:state.breaks) {
    fracturePiecesTrack(item,world);
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
    // Bodies of the bond groups: present, asleep, through the ground (any of its pieces).
    std::vector<uint8_t> groupPresent(item.groups,0),groupAsleep(item.groups,0),groupFell(item.groups,0);
    std::vector<double> groupVolume(item.groups,0.);
    std::vector<size_t> groupPieces(item.groups,0);
    for(const auto& piece:item.pieces)
      if(piece.bondGroup<item.groups) {groupVolume[piece.bondGroup]+=piece.volume;++groupPieces[piece.bondGroup];}
    size_t missing=0,flat=0,onEdge=0,upright=0,sleeping=0,restingAsleep=0,fell=0,metal=0,metalOnGround=0;
    float maxJitter=0,maxFloor=0,maxNative=0,maxVisible=0,maxTerrain=0,maxPieces=0,maxSpeed=0;
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
      if(piece.bondGroup<item.groups) {
        groupPresent[piece.bondGroup]=1;groupAsleep[piece.bondGroup]=part.sleeping;groupFell[piece.bondGroup]|=below;
        }
      std::string where;
      if(below) {
        // A fallen piece: the static surface straight above or below it within 20 m, and water at it.
        const auto above=world.physic()->rayStatic(center+Vec3(0,2000,0),center);
        const auto under=world.physic()->rayStatic(center+Vec3(0,5,0),center-Vec3(0,2000,0));
        const auto water=world.physic()->waterRay(center+Vec3(0,50,0),50);
        where=",\"static_above_cm\":"+(above.hasCol?ragdollNumber(above.v.y-center.y):std::string("null"))+
          ",\"static_under_cm\":"+(under.hasCol?ragdollNumber(center.y-under.v.y):std::string("null"))+
          ",\"water\":"+(water.hasCol?"true":"false")+",\"water_depth_cm\":"+ragdollNumber(water.wdepth);
        }
      const auto thin=Vec3::normalize(rotate(part.transform,piece.thin));
      const auto along=Vec3::normalize(rotate(part.transform,piece.longAxis));
      // Angle of the broad face from vertical, and of the long axis from vertical, degrees.
      const float face=std::asin(std::min(1.f,std::abs(thin.y)))*180.f/float(M_PI);
      const float length=std::acos(std::min(1.f,std::abs(along.y)))*180.f/float(M_PI);
      // Orientation counts only pieces that lie on the ground, not those falling through it.
      if(piece.flat && !below) {++flat;if(face<30.f)++onEdge;if(length<30.f)++upright;}
      if(part.sleeping)++sleeping;
      if(part.sleeping && !below)++restingAsleep;
      const float speed=part.velocity.length();
      maxSpeed=std::max(maxSpeed,speed);
      std::string extra;
      if(at==13000)piece.at13=center;
      if(last) {
        const float jitter=(center-piece.at13).length();
        if(!below)maxJitter=std::max(maxJitter,jitter);
        float visible=0,ground=0,lowest=std::numeric_limits<float>::infinity(),terrain=lowest;
        poses[i]=joltPose(part.transform);
        low[i]=Vec3(1e9f,1e9f,1e9f);high[i]=Vec3(-1e9f,-1e9f,-1e9f);
        for(const auto& v:piece.vertices) {
          const auto p=apply(part.transform,v);
          low[i]=Vec3(std::min(low[i].x,p.x),std::min(low[i].y,p.y),std::min(low[i].z,p.z));
          high[i]=Vec3(std::max(high[i].x,p.x),std::max(high[i].y,p.y),std::max(high[i].z,p.z));
          if(floor.hasCol)lowest=std::min(lowest,Vec3::dotProduct(p-floor.v,floor.n));
          // Drawn vertices near the ground against the ground right under them: the plane
          // under the centre bends away from curved terrain under a 1 m board.
          if(floor.hasCol && !below && Vec3::dotProduct(p-floor.v,floor.n)<5.f) {
            const auto under=world.physic()->landRay(p+Vec3(0,30,0),60);
            if(under.hasCol)terrain=std::min(terrain,p.y-under.v.y);
            }
          }
        if(floor.hasCol)visible=std::max(0.f,-lowest);
        // A hoop lies on the ground when its lowest drawn point is at most 1 cm over the floor, not on a stave.
        if(piece.material==asset::MaterialGroup::METAL) {++metal;if(floor.hasCol && !below && terrain<=1.f)++metalOnGround;}
        const float sunk=std::isfinite(terrain)?std::max(0.f,-terrain):0.f;
        if(floor.hasCol && !below) {
          // The ground as a native box whose top face is the floor plane under the piece.
          const auto normal=JPH::Vec3(floor.n.x,floor.n.y,floor.n.z).Normalized();
          const JPH::ShapeRefC box=new JPH::BoxShape(JPH::Vec3(2,.5f,2),0);
          const auto top=JPH::Vec3(floor.v.x,floor.v.y,floor.v.z)*.01f;
          const auto pose=JPH::Mat44::sRotationTranslation(JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),normal),top-normal*.5f);
          if(piece.hull!=nullptr)ground=penetration(piece.hull.GetPtr(),poses[i],box.GetPtr(),pose);
          }
        // The plane under the centre misses curved ground under a 1.1 m stave: also the native contact itself.
        const float native=below?0.f:world.physic()->fragmentGroundPenetration(item.parent,i);
        maxFloor=std::max(maxFloor,ground);maxNative=std::max(maxNative,native);maxVisible=std::max(maxVisible,visible);
        maxTerrain=std::max(maxTerrain,sunk);
        extra=",\"jitter_cm\":"+ragdollNumber(jitter)+",\"floor_penetration_cm\":"+ragdollNumber(ground)+
          ",\"native_floor_penetration_cm\":"+ragdollNumber(native)+
          ",\"visible_floor_penetration_cm\":"+ragdollNumber(visible)+
          ",\"visible_terrain_penetration_cm\":"+ragdollNumber(sunk)+
          ",\"lowest_over_terrain_cm\":"+ragdollNumber(std::isfinite(terrain)?terrain:0.f);
        }
      if(rows.size()>1)rows+=',';
      rows+="{\"piece\":"+std::to_string(i)+",\"group\":"+jsonStr(piece.group)+",\"bond_group\":"+std::to_string(piece.bondGroup)+",\"volume_cm3\":"+
        ragdollNumber(float(piece.volume))+",\"extent_cm\":["+ragdollNumber(piece.extent[0])+","+
        ragdollNumber(piece.extent[1])+","+ragdollNumber(piece.extent[2])+"],\"flat\":"+(piece.flat?"true":"false")+
        ",\"stave\":"+(piece.stave?"true":"false")+",\"center_cm\":"+ragdollVector(center)+
        ",\"displacement_cm\":"+ragdollNumber(moved)+",\"face_from_vertical_deg\":"+ragdollNumber(face)+
        ",\"long_from_vertical_deg\":"+ragdollNumber(length)+",\"sleeping\":"+(part.sleeping?"true":"false")+
        ",\"speed_cm_s\":"+ragdollNumber(speed)+",\"fell\":"+(below?"true":"false")+where+extra+"}";
      }
    rows+="]";
    std::string pairs;
    if(last) {
      size_t worstA=0,worstB=0;
      for(size_t i=0;i<parts.size();++i)
        for(size_t j=i+1;j<parts.size();++j) {
          if(parts[i]==nullptr || parts[j]==nullptr || item.pieces[i].hull==nullptr || item.pieces[j].hull==nullptr)continue;
          // Format 3 hoops pass through the staves of their own break by design; joined pieces are one body.
          if(item.version==3 && !fractureMaterialsCollide(item.pieces[i].material,item.pieces[j].material))continue;
          if(item.pieces[i].bondGroup==item.pieces[j].bondGroup)continue;
          if(low[i].x>high[j].x+3 || low[j].x>high[i].x+3 || low[i].y>high[j].y+3 || low[j].y>high[i].y+3 ||
             low[i].z>high[j].z+3 || low[j].z>high[i].z+3)continue;
          const float depth=penetration(item.pieces[i].hull.GetPtr(),poses[i],item.pieces[j].hull.GetPtr(),poses[j]);
          if(depth>maxPieces) {maxPieces=depth;worstA=i;worstB=j;}
          }
      pairs=",\"max_floor_penetration_cm\":"+ragdollNumber(maxFloor)+",\"max_native_floor_penetration_cm\":"+ragdollNumber(maxNative)+",\"max_visible_floor_penetration_cm\":"+
        ragdollNumber(maxVisible)+",\"max_visible_terrain_penetration_cm\":"+ragdollNumber(maxTerrain)+",\"max_piece_penetration_cm\":"+ragdollNumber(maxPieces)+
        ",\"worst_piece_pair\":["+std::to_string(worstA)+","+std::to_string(worstB)+"],\"max_jitter_cm\":"+
        ragdollNumber(maxJitter)+",\"metal\":"+std::to_string(metal)+",\"metal_on_ground\":"+std::to_string(metalOnGround);
      }
    const float largest=volumes.empty()?0.f:*std::max_element(volumes.begin(),volumes.end());
    const float middle=median(volumes);
    double total=0,largestGroup=0;
    size_t singles=0,bodies=0,bodiesAsleep=0,bodiesFell=0;
    for(size_t g=0;g<item.groups;++g) {
      total+=groupVolume[g];largestGroup=std::max(largestGroup,groupVolume[g]);singles+=groupPieces[g]==1;
      bodies+=groupPresent[g];bodiesAsleep+=groupPresent[g] && groupAsleep[g];bodiesFell+=groupPresent[g] && groupFell[g];
      }
    const std::string groups=",\"groups\":"+std::to_string(item.groups)+",\"largest_group_volume_fraction\":"+
      ragdollNumber(total>0?float(largestGroup/total):0.f)+",\"single_pieces\":"+std::to_string(singles)+
      ",\"bodies\":"+std::to_string(bodies)+",\"bodies_sleeping\":"+std::to_string(bodiesAsleep)+
      ",\"bodies_fell\":"+std::to_string(bodiesFell);
    event("destruction2_pieces","\"case\":"+jsonStr(item.label)+",\"visual\":"+jsonStr(item.visual)+
      ",\"version\":"+std::to_string(item.version)+",\"after_break_ms\":"+std::to_string(at)+
      ",\"pieces\":"+std::to_string(item.pieces.size())+",\"missing\":"+std::to_string(missing)+
      ",\"fell\":"+std::to_string(fell)+",\"median_displacement_cm\":"+ragdollNumber(median(resting))+
      ",\"median_displacement_all_cm\":"+ragdollNumber(median(displacement))+
      ",\"flat\":"+std::to_string(flat)+",\"flat_face_within_30_of_vertical\":"+std::to_string(onEdge)+
      ",\"flat_long_axis_within_30_of_vertical\":"+std::to_string(upright)+
      ",\"sleeping\":"+std::to_string(sleeping)+",\"resting_sleeping\":"+std::to_string(restingAsleep)+",\"max_speed_cm_s\":"+ragdollNumber(maxSpeed)+
      ",\"largest_volume_cm3\":"+ragdollNumber(largest)+",\"median_volume_cm3\":"+ragdollNumber(middle)+
      ",\"largest_to_median\":"+ragdollNumber(middle>0?largest/middle:0.f)+groups+pairs+",\"rows\":"+rows);
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

void fracturePiecesStand(Npc* actor,const Vec3& stand,const Vec3& facing) {
  if(actor==nullptr) {
    event("destruction2_stand","\"moved\":false");
    return;
    }
  const Vec3 from=actor->position();
  actor->setPosition(stand);actor->setDirection(facing);actor->updateTransform();
  event("destruction2_stand","\"moved\":true,\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(actor->position())+
        ",\"distance_cm\":"+ragdollNumber((actor->position()-from).length()));
  }

bool fracturePiecesCommand(FracturePieces& state,Destruction& destruction,const DestructionContext& context,bool& cutPending,const std::vector<std::string>& a,World& w) {
  const std::string& c = a[0];
  if(c=="destruction2" && a.size()==2 && (a[1]=="checkpoint" || a[1]=="stand"))
    a[1]=="checkpoint" ? destructionCheckpoint(w) : fracturePiecesStand(destruction.destructionMeleeActor,destruction.destructionMeleeStand,destruction.destructionMeleeFacing);
  else if(c=="destruction2" && a.size()==8 && a[1]=="view") {
    // view <camera xyz> <target xyz>: centimetres from the centre of the prop, held after the break
    const auto view=fracturePiecesView(w,destruction.destructionIds,Vec3(num(a,2,0),num(a,3,0),num(a,4,0)),Vec3(num(a,5,0),num(a,6,0),num(a,7,0)));
    if(!view) {arenaAbort("destruction view needs a fixture prop");return true;}
    context.cam=C_Hold;cutPending=true;lookFrom(view->first,view->second);
    }
  else if(c=="destruction2" && a.size()==3 && a[1]=="remnant") {
    // remnant <n>: strike the largest group of the broken fixture prop, at most n swings
    const auto parts=fracturePiecesGroups(w,destruction);
    if(!destruction.destructionMeleeActor || parts.empty()) {arenaAbort("destruction remnant needs a broken fixture prop");return true;}
    auto& attack=state.remnant;
    attack={};
    attack.running=true;attack.limit=std::max(1u,unsigned(num(a,2,6)));attack.ordinal=unsigned(std::count_if(state.breaks.begin(),state.breaks.end(),
      [&](const auto& b){return destructionOwns(destruction,b.parent);}));
    attack.bodies=parts.size();attack.pieces=parts.front().pieces.size();attack.largest=parts.front().volumeShare;
    attack.next=w.tickCount();attack.deadline=w.tickCount()+uint64_t(attack.limit)*2200+30000;
    context.waitUntil=context.time+uint64_t(attack.limit)*2200+30034;
    }
  else return false;
  return true;
  }

}
