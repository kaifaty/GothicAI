// Offline repro: item hull (as DynamicWorld::itemHullShape) on one static triangle in plain Jolt.
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/VirtualObject.hh>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>


using namespace JPH;

static Vec3 m(const zenkit::Vec3& v) { return Vec3(v.x*.01f,v.y*.01f,v.z*.01f); }

namespace Layers { constexpr ObjectLayer STATIC=0, MOVING=1, NUM=2; }
class BPL final : public BroadPhaseLayerInterface {
public:
  uint GetNumBroadPhaseLayers() const override { return 2; }
  BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override { return BroadPhaseLayer(BroadPhaseLayer::Type(l)); }
};
class OVB final : public ObjectVsBroadPhaseLayerFilter { public: bool ShouldCollide(ObjectLayer, BroadPhaseLayer) const override { return true; } };
class OPF final : public ObjectLayerPairFilter { public: bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override { return a==Layers::MOVING || b==Layers::MOVING; } };

struct Listener final : ContactListener {
  int step=0; int verbose=0; Vec3 com; float planeY=0;
  int contacts=0;
  void report(const Body& a,const Body& b,const ContactManifold& mf,bool persisted) {
    ++contacts;
    if(step>verbose) return;
    const bool aDyn=a.IsDynamic();
    Vec3 n=aDyn ? mf.mWorldSpaceNormal : -mf.mWorldSpaceNormal; // normal from item into support -> flip for push-out
    std::printf("   step %d %s depth %.3fcm n(%.3f %.3f %.3f) pts %u\n",step,persisted?"P":"A",mf.mPenetrationDepth*100,-n.GetX(),-n.GetY(),-n.GetZ(),mf.mRelativeContactPointsOn1.size());
    const auto& pts=aDyn ? mf.mRelativeContactPointsOn1 : mf.mRelativeContactPointsOn2;
    for(uint i=0;i<pts.size();++i) {
      Vec3 p=Vec3(mf.mBaseOffset)+pts[i];
      std::printf("      body pt rel COM (%.1f %.2f %.1f)cm  below plane %.2fcm\n",(p.GetX()-com.GetX())*100,(p.GetY()-com.GetY())*100,(p.GetZ()-com.GetZ())*100,(planeY-p.GetY())*100);
    }
  }
  void OnContactAdded(const Body& a,const Body& b,const ContactManifold& mf,ContactSettings&) override { report(a,b,mf,false); }
  void OnContactPersisted(const Body& a,const Body& b,const ContactManifold& mf,ContactSettings&) override { report(a,b,mf,true); }
};

struct Hull { RefConst<Shape> original, shape; float padding=0; };

// Copy of DynamicWorld::itemHullShape (main d73a55aa7), metres.
static Hull buildHull(std::vector<Vec3> points, int variant) {
  Hull hull;
  std::sort(points.begin(),points.end(),[](const auto& a,const auto& b) {
    if(a.GetX()!=b.GetX()) return a.GetX()<b.GetX();
    if(a.GetY()!=b.GetY()) return a.GetY()<b.GetY();
    return a.GetZ()<b.GetZ();
  });
  points.erase(std::unique(points.begin(),points.end()),points.end());
  const auto origin=points.front();
  auto longest=Vec3::sZero();
  for(const auto& p:points) if((p-origin).LengthSq()>longest.LengthSq()) longest=p-origin;
  auto normal=Vec3::sZero();
  for(const auto& p:points) { auto c=longest.Cross(p-origin); if(c.LengthSq()>normal.LengthSq()) normal=c; }
  normal=normal.Normalized();
  float low=1e30f,high=-1e30f;
  for(const auto& p:points) { float pr=normal.Dot(p-origin); low=std::min(low,pr); high=std::max(high,pr); }
  constexpr float minimumThickness=.004f;
  if(high-low<minimumThickness) {
    const auto padding=normal*((minimumThickness-(high-low))*.5f);
    const auto size=points.size();
    for(size_t i=0;i<size;++i) { points.push_back(points[i]+padding); points[i]-=padding; }
  }
  ConvexHullShapeSettings settings(points.data(),int(points.size()),.00025f);
  settings.mHullTolerance=.0001f; settings.mMaxErrorConvexRadius=.0001f;
  auto result=settings.Create();
  if(result.HasError()) { std::printf("hull error %s\n",result.GetError().c_str()); std::exit(1); }
  const auto& shape=static_cast<const ConvexHullShape&>(*result.Get());
  std::vector<Vec3> dirs;
  for(int x=-1;x<=1;++x) for(int y=-1;y<=1;++y) for(int z=-1;z<=1;++z) {
    int nz=int(x!=0)+int(y!=0)+int(z!=0);
    if(nz==1||nz==3) dirs.push_back(Vec3(float(x),float(y),float(z)).Normalized());
  }
  std::vector<Vec3> inflated;
  auto inflate=[&](float padding) {
    inflated.clear();
    for(uint i=0;i<shape.GetNumPoints();++i) {
      auto p=shape.GetPoint(i)+shape.GetCenterOfMass();
      for(auto& d:dirs) inflated.push_back(p+d*padding);
    }
    ConvexHullShapeSettings s(inflated.data(),int(inflated.size()),.00025f);
    s.mHullTolerance=.0001f; s.mMaxErrorConvexRadius=.0001f;
    if(variant==1) s.mHullTolerance=.001f; // coarser merge
    return s.Create();
  };
  float padding=.01f;
  auto res=inflate(padding);
  constexpr float minIR=.03f;
  if(res.Get()->GetInnerRadius()<minIR) {
    float lower=padding,upper=.04f;
    auto up=inflate(upper);
    for(int i=0;i<4 && up.Get()->GetInnerRadius()<minIR;++i) { lower=upper; upper*=2; up=inflate(upper); }
    for(int i=0;i<16 && upper-lower>.000001f;++i) {
      float mid=(lower+upper)*.5f; auto r=inflate(mid);
      if(r.Get()->GetInnerRadius()>=minIR) { upper=mid; up=r; } else lower=mid;
    }
    padding=upper; res=up;
  }
  hull.original=result.Get(); hull.shape=res.Get(); hull.padding=padding;
  return hull;
}

static const zenkit::VirtualObject* findVob(const std::vector<std::shared_ptr<zenkit::VirtualObject>>& vobs, Vec3 target) {
  for(auto& v:vobs) {
    if(!v) continue;
    Vec3 p(v->position.x,v->position.y,v->position.z);
    if(v->type==zenkit::VirtualObjectType::oCItem && (p-target).Length()<0.05f) return v.get();
    if(auto* r=findVob(v->children,target)) return r;
  }
  return nullptr;
}

int main(int argc,char** argv) {
  if(argc<16) { std::fprintf(stderr,"usage: gothicdir world mrm x y z  ax ay az bx by bz cx cy cz [manifoldMode] [hullVariant] [verboseSteps] [steps] [friction]\n"); return 2; }
  std::string dir=argv[1], worldName=argv[2], mrmName=argv[3];
  Vec3 zpos(std::atof(argv[4]),std::atof(argv[5]),std::atof(argv[6]));
  Float3 tri[3];
  for(int i=0;i<3;++i) tri[i]=Float3(std::atof(argv[7+3*i])*.01f,std::atof(argv[8+3*i])*.01f,std::atof(argv[9+3*i])*.01f);
  int manifoldMode=argc>16?std::atoi(argv[16]):0;
  int hullVariant=argc>17?std::atoi(argv[17]):0;
  int verbose=argc>18?std::atoi(argv[18]):10;
  int steps=argc>19?std::atoi(argv[19]):300;
  float friction=argc>20?std::atof(argv[20]):0.5f;

  zenkit::Logger::set_default(zenkit::LogLevel::ERROR);
  zenkit::Vfs vfs;
  for(auto& e:std::filesystem::directory_iterator(dir+"/Data")) {
    auto n=e.path().filename().string();
    for(auto& c:n) c=char(std::toupper((unsigned char)c));
    if(n=="MESHES.VDF"||n=="MESHES_ADDON.VDF"||n=="WORLDS.VDF"||n=="WORLDS_ADDON.VDF") vfs.mount_disk(e.path(),zenkit::VfsOverwriteBehavior::ALL);
  }
  auto* mf=vfs.find(mrmName);
  if(!mf) { std::printf("no mesh %s\n",mrmName.c_str()); return 1; }
  zenkit::MultiResolutionMesh mrm; { auto in=mf->open_read(); mrm.load(in.get()); }
  std::vector<Vec3> pts;
  for(auto& sub:mrm.sub_meshes) for(auto& w:sub.wedges) pts.push_back(m(mrm.positions.at(w.index)));
  auto* wf=vfs.find(worldName);
  zenkit::World world; { auto in=wf->open_read(); world.load(in.get(),zenkit::GameVersion::GOTHIC_2); }
  auto* vob=findVob(world.world_vobs,zpos);
  if(!vob) { std::printf("vob not found\n"); return 1; }
  const auto& r=vob->rotation.columns;
  Mat44 rot(Vec4(r[0].x,r[0].y,r[0].z,0),Vec4(r[1].x,r[1].y,r[1].z,0),Vec4(r[2].x,r[2].y,r[2].z,0),Vec4(0,0,0,1));
  std::printf("vob visual=%s rot cols (%.4f %.4f %.4f) (%.4f %.4f %.4f) (%.4f %.4f %.4f)\n",vob->visual?vob->visual->name.c_str():"-",
    r[0].x,r[0].y,r[0].z,r[1].x,r[1].y,r[1].z,r[2].x,r[2].y,r[2].z);

  RegisterDefaultAllocator();
  Factory::sInstance=new Factory();
  RegisterTypes();
  Hull hull=buildHull(pts,hullVariant);
  auto* ch=static_cast<const ConvexHullShape*>(hull.shape.GetPtr());
  std::printf("mesh pts %zu, inflated hull pts %u faces %u innerR %.3fcm padding %.3fcm\n",pts.size(),ch->GetNumPoints(),ch->GetNumFaces(),ch->GetInnerRadius()*100,hull.padding*100);

  if(std::getenv("DUMP_HULL")) {
    Quat qq=rot.GetQuaternion().Normalized();
    std::printf("DUMP pos_m %.7f %.7f %.7f quat %.9f %.9f %.9f %.9f\nDUMP points_m (body frame):\n",zpos.GetX()*.01f,zpos.GetY()*.01f,zpos.GetZ()*.01f,qq.GetX(),qq.GetY(),qq.GetZ(),qq.GetW());
    for(uint i=0;i<ch->GetNumPoints();++i) { Vec3 p=ch->GetPoint(i)+hull.shape->GetCenterOfMass(); std::printf("{%.7ff,%.7ff,%.7ff},",p.GetX(),p.GetY(),p.GetZ()); if(i%3==2) std::printf("\n"); }
    std::printf("\nDUMP com_m %.7f %.7f %.7f original_volume_m3 %.9g\n",hull.shape->GetCenterOfMass().GetX(),hull.shape->GetCenterOfMass().GetY(),hull.shape->GetCenterOfMass().GetZ(),hull.original->GetVolume());
  }
  // World pose
  Quat q=rot.GetQuaternion().Normalized();
  Vec3 posM=zpos*.01f;
  Vec3 comW=posM+q*hull.shape->GetCenterOfMass();
  float planeY=tri[0].y;
  // List faces facing down in world
  std::printf("down-facing faces (world normal y<-0.8):\n");
  for(uint f=0;f<ch->GetNumFaces();++f) {
    Plane pl=ch->GetPlanes()[f];
    Vec3 nW=q*pl.GetNormal();
    if(nW.GetY()>-0.8f) continue;
    uint idx[64]; uint nv=ch->GetFaceVertices(f,64,idx);
    float minx=1e9,maxx=-1e9,minz=1e9,maxz=-1e9,miny=1e9,maxy=-1e9; Vec3 c=Vec3::sZero();
    for(uint i=0;i<nv;++i) { Vec3 p=posM+q*(ch->GetPoint(idx[i])+hull.shape->GetCenterOfMass()); c+=p;
      minx=std::min(minx,p.GetX());maxx=std::max(maxx,p.GetX());minz=std::min(minz,p.GetZ());maxz=std::max(maxz,p.GetZ());miny=std::min(miny,p.GetY());maxy=std::max(maxy,p.GetY()); }
    c/=float(nv);
    std::printf("  face %u tilt %.3fdeg nv %u extent x %.1f z %.1f cm, centroid rel COM (%.1f %.2f %.1f), y %.2f..%.2f below plane\n",f,std::acos(std::min(1.f,-nW.GetY()))*57.2958f,nv,
      (maxx-minx)*100,(maxz-minz)*100,(c.GetX()-comW.GetX())*100,(c.GetY()-comW.GetY())*100,(c.GetZ()-comW.GetZ())*100,(planeY-maxy)*100,(planeY-miny)*100);
  }

  TempAllocatorImpl temp(64*1024*1024);
  JobSystemThreadPool jobs(cMaxPhysicsJobs,cMaxPhysicsBarriers,1);
  BPL bpl; OVB ovb; OPF opf;
  PhysicsSystem sys; sys.Init(1024,0,1024,1024,bpl,ovb,opf);
  sys.SetGravity(Vec3(0,-9.81f,0));
  auto ps=sys.GetPhysicsSettings(); ps.mDeepestContactLayers=manifoldMode ? 1u<<Layers::MOVING : 0; ps.mTimeBeforeSleep=2.f; ps.mPointVelocitySleepThreshold=.8f; sys.SetPhysicsSettings(ps);
  Listener lis; lis.verbose=verbose; lis.planeY=planeY; sys.SetContactListener(&lis);
  sys.SetCombineFriction([](const Body& a,const SubShapeID&,const Body& b,const SubShapeID&){ return std::clamp(a.GetFriction()*b.GetFriction(),-10.f,10.f); });
  TriangleList tl; tl.push_back(Triangle(tri[0],tri[1],tri[2]));
  MeshShapeSettings ms(tl);
  auto msr=ms.Create();
  auto& bi=sys.GetBodyInterface();
  BodyCreationSettings gs(msr.Get(),RVec3::sZero(),Quat::sIdentity(),EMotionType::Static,Layers::STATIC);
  gs.mFriction=0.5f;
  bi.CreateAndAddBody(gs,EActivation::DontActivate);
  BodyCreationSettings bs(hull.shape,posM,q,EMotionType::Dynamic,Layers::MOVING);
  bs.mFriction=friction; bs.mOverrideMassProperties=EOverrideMassProperties::CalculateInertia;
  bs.mMassPropertiesOverride.mMass=std::max(.05f,7.8e3f*hull.original->GetVolume());
  bs.mMotionQuality=EMotionQuality::LinearCast; bs.mLinearDamping=0; bs.mAngularDamping=0;
  BodyID id=bi.CreateAndAddBody(bs,EActivation::Activate);
  AABox b0=bi.GetTransformedShape(id).GetWorldSpaceBounds();
  RVec3 p0=bi.GetPosition(id);
  std::printf("mass %.3fkg start: com above plane %.2fcm, lowest %.2fcm below plane\n",bs.mMassPropertiesOverride.mMass,(comW.GetY()-planeY)*100,(planeY-b0.mMin.GetY())*100);
  float maxDisp=0,maxPen=0; int tunnel=-1,sleepStep=-1;
  for(int s=1;s<=steps;++s) {
    lis.step=s; lis.com=Vec3(bi.GetCenterOfMassPosition(id));
    sys.Update(1.f/60.f,1,&temp,&jobs);
    Vec3 com(bi.GetCenterOfMassPosition(id));
    RMat44 tr=bi.GetCenterOfMassTransform(id);
    float lowY=1e30f; Vec3 lowP;
    for(uint i=0;i<ch->GetNumPoints();++i) { Vec3 p=Vec3(tr*ch->GetPoint(i)); if(p.GetY()<lowY) { lowY=p.GetY(); lowP=p; } }
    AABox bb; bb.mMin=Vec3(0,lowY,0);
    float disp=(Vec3(bi.GetPosition(id))-Vec3(p0)).Length()*100;
    float pen=(planeY-bb.mMin.GetY())*100;
    maxDisp=std::max(maxDisp,disp); maxPen=std::max(maxPen,pen);
    if(tunnel<0 && com.GetY()<planeY) tunnel=s;
    if(sleepStep<0 && !bi.IsActive(id)) sleepStep=s;
    if(s<=verbose || s%50==0) std::printf(" step %3d low@(%.1f %.1f)rel com-plane %.2fcm lowest-below %.2fcm disp %.2fcm vy %.2fcm/s active %d\n",s,(lowP.GetX()-com.GetX())*100,(lowP.GetZ()-com.GetZ())*100,(com.GetY()-planeY)*100,pen,disp,bi.GetLinearVelocity(id).GetY()*100,bi.IsActive(id));
  }
  std::printf("RESULT manifoldMode=%d hullVariant=%d maxDisp %.3fcm maxLowestBelow %.3fcm tunnelStep %d sleepStep %d\n",manifoldMode,hullVariant,maxDisp,maxPen,tunnel,sleepStep);
  return 0;
}
