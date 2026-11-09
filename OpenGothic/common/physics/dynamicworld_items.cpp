#include "dynamicworld_internal.h"

#include "loadprobe.h"
#include "sha256.h"
#include "graphics/mesh/protomesh.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/UnorderedMap.h>

#include <cctype>

// The placed-item census key hashes this function's inputs, not its hull:
// any change to the hull it builds bumps PlacedCensusCache::RulesVersion.
const DynamicWorld::ItemHull& DynamicWorld::itemHullShape(const ProtoMesh& proto, std::string_view visual) {
  std::string key(visual);
  std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  auto [cached,inserted]=itemHulls->shapes.try_emplace(key);
  auto& hull=cached->second;
  if(!inserted)
    return hull;
  const auto extent=CollisionWorld::toMeters(proto.bbox()[1]-proto.bbox()[0]);
  hull.boundsVolume=extent.GetX()*extent.GetY()*extent.GetZ();
  proto.appendItemVertices(hull.vertices);
  std::vector<JPH::Vec3> points;
  points.reserve(hull.vertices.size());
  for(const auto& point:hull.vertices) {
    if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      hull.fallbackReason="nonfinite mesh vertex";
      return hull;
      }
    points.push_back(CollisionWorld::toMeters(point));
    }
  std::sort(points.begin(),points.end(),[](const auto& a,const auto& b) {
    if(a.GetX()!=b.GetX()) return a.GetX()<b.GetX();
    if(a.GetY()!=b.GetY()) return a.GetY()<b.GetY();
    return a.GetZ()<b.GetZ();
    });
  points.erase(std::unique(points.begin(),points.end()),points.end());
  if(points.size()<4) {
    hull.fallbackReason="fewer than four mesh vertices";
    return hull;
    }
  // Find the mesh's broad plane, including tilted leaves and scrolls. A flat
  // surface gets 4 mm of symmetric thickness, while a line stays a fallback.
  const auto origin=points.front();
  auto longest=JPH::Vec3::sZero();
  for(const auto& point:points)
    if((point-origin).LengthSq()>longest.LengthSq())
      longest=point-origin;
  auto normal=JPH::Vec3::sZero();
  for(const auto& point:points) {
    const auto candidate=longest.Cross(point-origin);
    if(candidate.LengthSq()>normal.LengthSq())
      normal=candidate;
    }
  if(longest.LengthSq()<=1.e-12f || normal.LengthSq()<=1.e-12f*longest.LengthSq()) {
    hull.fallbackReason="collinear mesh vertices";
    return hull;
    }
  normal=normal.Normalized();
  float low=std::numeric_limits<float>::infinity(),high=-low;
  for(const auto& point:points) {
    const float projection=normal.Dot(point-origin);
    low=std::min(low,projection);
    high=std::max(high,projection);
    }
  constexpr float minimumThickness=.004f;
  if(high-low<minimumThickness) {
    const auto padding=normal*((minimumThickness-(high-low))*.5f);
    const auto size=points.size();
    points.reserve(2*size);
    for(size_t i=0;i<size;++i) {
      points.push_back(points[i]+padding);
      points[i]-=padding;
      }
    }
  Physics::initialize();
  // Jolt subtracts this radius internally; it does not inflate the envelope.
  // A small hull tolerance preserves millimetre-scale detail on coins/leaves.
  JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),.00025f);
  settings.mHullTolerance=.0001f;
  settings.mMaxErrorConvexRadius=.0001f;
  const auto result=settings.Create();
  if(result.HasError()) {
    hull.fallbackReason="Jolt: "+std::string(result.GetError().c_str());
    return hull;
    }
  const float volume=result.Get()->GetVolume();
  if(!std::isfinite(volume) || volume<=0.f) {
    hull.fallbackReason="zero or nonfinite hull volume";
    return hull;
    }
  const auto& shape=static_cast<const JPH::ConvexHullShape&>(*result.Get());
  std::vector<JPH::Vec3> directions;
  directions.reserve(14);
  for(int x=-1;x<=1;++x)
    for(int y=-1;y<=1;++y)
      for(int z=-1;z<=1;++z) {
        const int nonzero=int(x!=0)+int(y!=0)+int(z!=0);
        if(nonzero==1 || nonzero==3)
          directions.push_back(JPH::Vec3(float(x),float(y),float(z)).Normalized());
        }
  std::vector<JPH::Vec3> inflated;
  inflated.reserve(shape.GetNumPoints()*14);
  const auto inflate=[&](float padding) {
    inflated.clear();
    for(JPH::uint i=0;i<shape.GetNumPoints();++i) {
      const auto point=shape.GetPoint(i)+shape.GetCenterOfMass();
      for(const auto& direction:directions)
        inflated.push_back(point+direction*padding);
      }
    JPH::ConvexHullShapeSettings inflatedSettings(inflated.data(),int(inflated.size()),.00025f);
    inflatedSettings.mHullTolerance=.0001f;
    inflatedSettings.mMaxErrorConvexRadius=.0001f;
    return inflatedSettings.Create();
    };
  // One centimetre centres the visible envelope in Jolt's unchanged 2 cm slop.
  // Keep the source envelope for visible support measurements, mass and water.
  float padding=.01f;
  auto inflatedResult=inflate(padding);
  if(inflatedResult.HasError()) {
    hull.fallbackReason="Jolt inflated hull: "+std::string(inflatedResult.GetError().c_str());
    return hull;
    }
  // Inner radius is the shortest COM-to-support-plane distance, including
  // tilted thin meshes. Rebuilding also accounts for centroid movement.
  constexpr float minimumInnerRadius=.03f;
  if(inflatedResult.Get()->GetInnerRadius()<minimumInnerRadius) {
    float lower=padding,upper=.04f;
    auto upperResult=inflate(upper);
    for(int i=0;i<4 && !upperResult.HasError() && upperResult.Get()->GetInnerRadius()<minimumInnerRadius;++i) {
      lower=upper;upper*=2.f;upperResult=inflate(upper);
      }
    if(upperResult.HasError()) {
      hull.fallbackReason="Jolt inflated hull: "+std::string(upperResult.GetError().c_str());
      return hull;
      }
    if(upperResult.Get()->GetInnerRadius()<minimumInnerRadius) {
      hull.fallbackReason="inflated hull cannot reach the minimum inner radius";
      return hull;
      }
    for(int i=0;i<16 && upper-lower>.000001f;++i) {
      const float middle=(lower+upper)*.5f;
      auto middleResult=inflate(middle);
      if(middleResult.HasError()) {
        hull.fallbackReason="Jolt inflated hull: "+std::string(middleResult.GetError().c_str());
        return hull;
        }
      if(middleResult.Get()->GetInnerRadius()>=minimumInnerRadius) {
        upper=middle;upperResult=std::move(middleResult);
        }
      else lower=middle;
      }
    padding=upper;inflatedResult=std::move(upperResult);
    }
  hull.originalShape=result.Get();
  hull.shape=inflatedResult.Get();
  hull.volume=volume;
  hull.extraPaddingCm=(padding-.01f)*100.f;
  hull.vertices.clear();
  hull.vertices.reserve(shape.GetNumPoints());
  for(JPH::uint i=0;i<shape.GetNumPoints();++i)
    hull.vertices.push_back(CollisionWorld::toCentimeters(shape.GetPoint(i)+shape.GetCenterOfMass()));
  return hull;
  }

std::vector<DynamicWorld::PlacedItemCensusEntry> DynamicWorld::placedItemCensus(
    const std::vector<PlacedItemCandidate>& candidates) const {
  LoadProbe::Startup::Scope probe(LoadProbe::Startup::PlacementQueries,false,"placed-items");
  constexpr float supportLimitCm = 4.f, penetrationLimitCm = 2.f;
  constexpr uint32_t mask = (1u<<C_Landscape)|(1u<<C_Object)|(1u<<C_MovableVob)|(1u<<C_Item);
  std::vector<PlacedItemCensusEntry> result(candidates.size());
  std::vector<bool> groundBelow(candidates.size(),true);
  std::unordered_map<const CollisionWorld::CollisionBody*,size_t> identities;
  for(size_t i=0;i<candidates.size();++i) {
    const auto* handle = candidates[i].body;
    if(handle!=nullptr && handle->obj!=nullptr && handle->obj->body!=nullptr)
      identities.emplace(handle->obj->body.get(),i);
    }
  for(size_t i=0;i<candidates.size();++i) {
    const auto* handle = candidates[i].body;
    if(handle==nullptr || handle->obj==nullptr || handle->obj->body==nullptr)
      continue;
    const auto& body = *handle->obj->body;
    JPH::Vec3 min, max;
    body.getAabb(min,max);
    const auto hullMin = CollisionWorld::toCentimeters(min);
    const auto hullMax = CollisionWorld::toCentimeters(max);
    const float height = std::max(hullMax.y-hullMin.y,1.f);
    auto& entry = result[i];
    const CollisionWorld::CollisionBody* support = nullptr;
    for(const auto& hit:world->overlaps(body,supportLimitCm,mask)) {
      const auto other = identities.find(hit.body);
      if(hit.category==C_Item && other==identities.end())
        continue;
      // A downward contact near the bottom of the actual collision envelope
      // is support. Convert normal separation to its vertical distance.
      if(hit.axis.y<=-.6f && hit.point1.y<=hullMin.y+.3f*height) {
        const float gap = std::max(0.f,hit.depth/hit.axis.y);
        if(gap<entry.supportGapCm) {
          entry.supportGapCm = gap;
          entry.supportCategory = hit.category;
          entry.supportNormal = hit.axis;
          entry.supportLabel = hit.body!=nullptr ? hit.body->label() : "";
          entry.supportZenId = hit.body!=nullptr && hit.body->ptrType()==P_MovableVob ?
              static_cast<const BodyData*>(hit.body->payload())->pose->zenId : uint32_t(-1);
          entry.supportItemIndex = other!=identities.end() ? other->second : size_t(-1);
          support = hit.body;
          }
        }
      if(hit.category==C_Landscape || hit.category==C_Object)
        entry.staticDepthCm = std::max(entry.staticDepthCm,hit.depth);
      else if(hit.category==C_MovableVob)
        entry.furnitureDepthCm = std::max(entry.furnitureDepthCm,hit.depth);
      else if(hit.category==C_Item) {
        entry.itemDepthCm = std::max(entry.itemDepthCm,hit.depth);
        // Keep the pair symmetric even if the native narrow phase reports
        // its deepest contact from only one side.
        auto& neighbour = result[other->second];
        neighbour.itemDepthCm = std::max(neighbour.itemDepthCm,hit.depth);
        }
      }
    entry.hullLowPointCm = body.lowestPoint();
    if(entry.supportCategory==C_Landscape || entry.supportCategory==C_Object) {
      const auto from = entry.hullLowPointCm+Tempest::Vec3(0,supportLimitCm,0);
      const auto to = entry.hullLowPointCm-Tempest::Vec3(0,supportLimitCm,0);
      const auto face = world->rayCast(from,to,(1u<<C_Landscape)|(1u<<C_Object));
      if(face.hasHit) {
        entry.supportFaceNormal = face.normal;
        entry.supportSlopeDeg = std::acos(std::clamp(face.normal.y,-1.f,1.f))*180.f/float(M_PI);
        }
      }
    if(entry.supportCategory==C_Object || entry.supportCategory==C_MovableVob) {
      const auto from = body.centerOfMass();
      const auto to = Tempest::Vec3(from.x,entry.hullLowPointCm.y-500.f,from.z);
      const std::vector<const CollisionWorld::CollisionBody*> excluded = {support};
      groundBelow[i] = world->rayCast(from,to,(1u<<C_Landscape)|(1u<<C_Object),false,nullptr,&excluded).hasHit;
      }
    }
  for(size_t i=0;i<candidates.size();++i) {
    auto& entry = result[i];
    entry.disposition = candidates[i].mission ? "quest" : entry.supportGapCm>supportLimitCm ? "hangs" :
                        entry.staticDepthCm>penetrationLimitCm ? "static" : entry.furnitureDepthCm>penetrationLimitCm ? "furniture" :
                        entry.itemDepthCm>penetrationLimitCm ? "neighbour" : "ok";
    if(entry.disposition=="ok" && (entry.supportCategory==C_Landscape || entry.supportCategory==C_Object)) {
      constexpr std::string_view rootedPrefix = "ITPL_";
      const auto visual = candidates[i].visual;
      const bool rooted = visual.size()>=rootedPrefix.size() &&
          std::equal(rootedPrefix.begin(),rootedPrefix.end(),visual.begin(),[](char upper,char value) {
            return upper==char(std::toupper(static_cast<unsigned char>(value)));
            });
      if(rooted)
        entry.disposition = "rooted";
      else if(entry.supportSlopeDeg>15.f)
        entry.disposition = "slope";
      else if(!std::isfinite(entry.supportSlopeDeg))
        entry.disposition = "irregular";
      }
    if(entry.disposition=="ok" && !groundBelow[i])
      entry.disposition = "no_ground";
    entry.enabled = entry.disposition=="ok";
    }
  // A rejected supporting Item has no collision body after the census. Follow
  // that dependency through whole stacks, independent of their array order.
  bool changed = true;
  while(changed) {
    changed = false;
    for(auto& entry:result)
      if(entry.enabled && entry.supportItemIndex!=size_t(-1) && !result[entry.supportItemIndex].enabled) {
        entry.disposition = "on_excluded";
        entry.enabled = false;
        changed = true;
        }
    }
  return result;
  }

namespace {
// Streams Jolt shape state straight into a digest. Vec3 writes omit W, so the
// bytes depend only on the shape data that native queries read.
class DigestStream final : public JPH::StreamOut {
  public:
    void WriteBytes(const void* data, size_t size) override { digest.update(data,size); }
    bool IsFailed() const override { return false; }
    void raw(const void* data, size_t size) { digest.update(data,size); }
    void u32(uint32_t value) { raw(&value,sizeof(value)); }
    void u64(uint64_t value) { raw(&value,sizeof(value)); }
    void text(std::string_view value) { u64(value.size()); raw(value.data(),value.size()); }
    void matrix(const Tempest::Matrix4x4& value) { raw(value.data(),16*sizeof(float)); }
    Sha256 digest;
  };
}

std::string DynamicWorld::placedItemCensusInputs(const std::vector<PlacedItemCandidate>& candidates) const {
  DigestStream out;
  JPH::Shape::ShapeToIDMap shapes;
  JPH::Shape::MaterialToIDMap materials;
  const auto body=[&](const CollisionWorld::CollisionBody* body) {
    if(body==nullptr || body->shape()==nullptr) {
      out.u32(0);
      return;
      }
    out.u32(1);
    out.u32(uint32_t(body->category()));
    out.matrix(body->matrix());
    JPH::Vec3 min,max;
    body->getAabb(min,max);
    out.Write(min);
    out.Write(max);
    // Shared hulls are written once; later references write only their ID.
    body->shape()->SaveWithChildren(out,shapes,materials);
    };
  // Candidates are hashed by what their body is built from, not by the body:
  // a cache hit must not create bodies or item hulls. Body construction is
  // deterministic in these inputs; changes to it bump RulesVersion.
  out.text("candidates-v2");
  out.u64(candidates.size());
  std::unordered_map<std::string,uint32_t> hullSources;
  std::vector<Tempest::Vec3> vertices;
  for(const auto& candidate:candidates) {
    out.u32(candidate.mission ? 1 : 0);
    out.u32(candidate.zenId);
    out.u64(uint64_t(candidate.instance));
    out.text(candidate.visual);
    const auto* proto=candidate.proto;
    if(proto==nullptr) {
      out.u32(0);
      continue;
      }
    out.u32(1);
    out.matrix(candidate.transform);
    out.u32(candidate.material);
    out.raw(&proto->bboxMesh[0],sizeof(proto->bboxMesh[0]));
    out.raw(&proto->bboxMesh[1],sizeof(proto->bboxMesh[1]));
    // itemHullShape keeps one hull per upper-case visual, built from the first
    // proto it sees. Hash that proto's source vertices once per visual.
    std::string key(candidate.visual);
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return char(std::toupper(c)); });
    const auto [source,inserted]=hullSources.try_emplace(std::move(key),uint32_t(hullSources.size()));
    out.u32(source->second);
    if(!inserted)
      continue;
    vertices.clear();
    proto->appendItemVertices(vertices);
    out.u64(vertices.size());
    out.raw(vertices.data(),vertices.size()*sizeof(Tempest::Vec3));
    }
  // Furniture and other movable vobs are supports and penetration targets.
  // Offline convex parts are outside the base fingerprint: hash their shapes.
  out.text("movable");
  const auto* movable=movableVobs.get();
  out.u64(movable!=nullptr ? movable->bodies.size() : 0);
  if(movable!=nullptr)
    for(const auto* vob:movable->bodies) {
      out.text(vob->visual);
      out.text(vob->className);
      out.text(vob->disposition);
      out.u32(vob->pose->zenId);
      out.matrix(vob->origin);
      body(vob->body.get());
      out.u64(vob->query.size());
      for(const auto& query:vob->query) {
        // Query meshes come from base meshes; their pose and layer decide hits.
        out.u32(uint32_t(query->category()));
        out.matrix(query->matrix());
        }
      }
  return out.digest.finish();
  }
