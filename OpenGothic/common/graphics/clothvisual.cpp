#include "clothvisual.h"
#include "asset/modeltypes.h"

#include <Tempest/Log>
#include "asset/staticmesh.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include "clothdata.h"
#include "gothic.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/cloth.h"
#include "physics/clothgeometry.h"
#include "physics/physicmesh.h"
#include "physics/worldwind.h"
#include "utils/fileext.h"
#include "world/world.h"

using namespace Tempest;

namespace {
struct Definition {
  std::string visual, pins, material;
  float stiffness = .8f;
  Vec3 attachmentFrom, attachmentTo;
  float attachmentRadius = 0.f;
  };

std::string canonical(std::string_view name) {
  const auto slash = name.find_last_of("/\\");
  if(slash!=std::string_view::npos)
    name.remove_prefix(slash+1);
  auto out = std::string(name.substr(0,name.find_last_of('.')));
  for(auto& c:out)
    if(c>='a' && c<='z')
      c = char(c-'a'+'A');
  return out;
  }

bool matches(std::string_view pattern, std::string_view value) {
  size_t p=0, v=0, star=std::string_view::npos, retry=0;
  while(v<value.size()) {
    if(p<pattern.size() && (pattern[p]=='?' || pattern[p]==value[v])) {
      ++p;
      ++v;
      }
    else if(p<pattern.size() && pattern[p]=='*') {
      star = p++;
      retry = v;
      }
    else if(star!=std::string_view::npos) {
      p = star+1;
      v = ++retry;
      }
    else
      return false;
    }
  while(p<pattern.size() && pattern[p]=='*')
    ++p;
  return p==pattern.size();
  }

const Definition* definition(std::string_view visual) {
  static const auto definitions = [] {
    std::vector<Definition> out;
    std::istringstream input(ClothVobData);
    for(std::string line;std::getline(input,line);) {
      if(line.empty() || line[0]=='#')
        continue;
      std::istringstream row(line);
      Definition def;
      std::string stiffness;
      if(!std::getline(row,def.visual,',') || !std::getline(row,def.pins,',') ||
         !std::getline(row,stiffness,','))
        continue;
      std::getline(row,def.material,',');
      try {
        def.stiffness = std::clamp(std::stof(stiffness),0.f,1.f);
        std::string attachment;
        if(std::getline(row,attachment,',') && !attachment.empty()) {
          for(auto& c:attachment)
            if(c==':' || c==';') c=' ';
          std::istringstream capsule(attachment);
          if(!(capsule>>def.attachmentFrom.x>>def.attachmentFrom.y>>def.attachmentFrom.z>>
                        def.attachmentTo.x>>def.attachmentTo.y>>def.attachmentTo.z>>def.attachmentRadius) ||
             !std::isfinite(def.attachmentRadius) || def.attachmentRadius<=0.f)
            throw std::runtime_error("invalid attachment capsule");
          }
        }
      catch(const std::exception&) {
        Log::e("invalid cloth definition: ",def.visual);
        continue;
        }
      def.visual = canonical(def.visual);
      if(def.material.empty())
        def.material = "*";
      out.push_back(std::move(def));
      }
    return out;
    }();
  const auto name = canonical(visual);
  for(const auto& def:definitions)
    if(matches(def.visual,name))
      return &def;
  return nullptr;
  }

struct Geometry {
  std::vector<Vec3> positions, normals, physicsPositions;
  std::vector<uint32_t> triangles, physicsTriangles, packedToPhysics, pins;
  std::vector<Vec3> sourcePositions;
  std::vector<uint32_t> sourceVertexIds;
  std::vector<ClothGeometry::SourceWeights> sourceWeights;
  std::vector<asset::MorphAnimation> animations;
  std::vector<std::vector<int32_t>> morphIndices;
  bool coarsePhysics = false;
  bool explicitAlphaTest = false;
  const ProtoMesh* fullProto = nullptr;
  std::unique_ptr<ProtoMesh> ownedFullProto, rigidProto, clothProto;
  };

// Low-poly fabric uses the subdivided surface for both Jolt and rendering.
// Furled sails already have a sufficiently resolved authored cross-section;
// their dense visible surface interpolates that smaller simulation instead.
// Packed vertices retain the dense index across meshlets and UV seams.
void tessellate(Geometry& out, const std::vector<ClothGeometry::Vertex>& vertices,
                const std::vector<uint32_t>& indices, const std::vector<uint32_t>& pins,
                const std::vector<asset::Material>& materials,
                const std::vector<uint32_t>& triangleMaterials, std::string_view visual,
                bool coarsePhysics = false) {
  const auto dense = ClothGeometry::subdivide(vertices,indices,pins);
  if(dense.vertices.size()>uint16_t(-1))
    throw std::runtime_error("cloth exceeds the wedge index range");
  out.sourcePositions.reserve(vertices.size());
  for(const auto& v:vertices)
    out.sourcePositions.emplace_back(v.pos[0],v.pos[1],v.pos[2]);
  out.sourceWeights = dense.sourceWeights;
  out.coarsePhysics = coarsePhysics;
  out.pins = coarsePhysics ? pins : dense.pins;
  out.physicsTriangles = coarsePhysics ? indices : dense.indices;
  out.triangles = dense.indices;

  asset::StaticMesh mesh;
  mesh.sub_meshes.resize(materials.size());
  for(size_t i=0;i<materials.size();++i) {
    auto& sub = mesh.sub_meshes[i];
    sub.mat = materials[i];
    // Active cloth gets wind from Jolt. A native material wave must not add a
    // second displacement to the already simulated vertices.
    sub.mat.wave_mode = asset::WaveMode::NONE;
    sub.mat.wave_max_amplitude = 0;
    sub.wedges.reserve(dense.vertices.size());
    for(uint32_t id=0;id<dense.vertices.size();++id) {
      const auto& v = dense.vertices[id];
      sub.wedges.push_back({{v.norm[0],v.norm[1],v.norm[2]}, {v.uv[0],v.uv[1]}, uint16_t(id)});
      }
    }
  for(const auto& v:dense.vertices) {
    out.positions.emplace_back(v.pos[0],v.pos[1],v.pos[2]);
    out.normals.emplace_back(v.norm[0],v.norm[1],v.norm[2]);
    mesh.positions.push_back({v.pos[0],v.pos[1],v.pos[2]});
    }
  out.physicsPositions = coarsePhysics ? out.sourcePositions : out.positions;
  const size_t childrenPerTriangle = size_t(1)<<(dense.subdivisionDepth*2);
  for(size_t at=0;at<dense.indices.size();at+=3) {
    const auto material = triangleMaterials[(at/3)/childrenPerTriangle];
    mesh.sub_meshes[material].triangles.push_back({{uint16_t(dense.indices[at]),
                                                  uint16_t(dense.indices[at+1]),
                                                  uint16_t(dense.indices[at+2])}});
    }
  auto lo = out.positions.front(), hi = lo;
  for(const auto& p:out.positions) {
    lo = Vec3(std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z));
    hi = Vec3(std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z));
    }
  mesh.bbox.min = {lo.x,lo.y,lo.z};
  mesh.bbox.max = {hi.x,hi.y,hi.z};
  PackedMesh packed(mesh,PackedMesh::PK_VisualMorph,out.explicitAlphaTest);
  out.packedToPhysics = packed.verticesId;
  out.clothProto = std::make_unique<ProtoMesh>(std::move(packed),visual);
  for(const auto& animation:out.animations) {
    auto& remap = out.morphIndices.emplace_back(out.sourceVertexIds.size(),-1);
    for(size_t i=0;i<out.sourceVertexIds.size();++i) {
      const auto id = std::find(animation.vertices.begin(),animation.vertices.end(),out.sourceVertexIds[i]);
      if(id!=animation.vertices.end())
        remap[i] = int32_t(id-animation.vertices.begin());
      }
    }
  const auto sourcePositions = std::set<uint32_t>(out.sourceVertexIds.begin(),out.sourceVertexIds.end()).size();
  std::set<std::tuple<int64_t,int64_t,int64_t>> uniqueSource;
  for(const auto& v:vertices)
    uniqueSource.emplace(std::llround(double(v.pos[0])*100.0),std::llround(double(v.pos[1])*100.0),
                         std::llround(double(v.pos[2])*100.0));
  Log::i("cloth tessellation: ",visual," source=",vertices.size()," source_positions=",sourcePositions,
         " source_unique=",uniqueSource.size()," dense=",dense.vertices.size(),
         " particles=",coarsePhysics ? uniqueSource.size() : dense.uniquePositions," edge_cm=",dense.maxEdgeCm,
         " depth=",dense.subdivisionDepth," capped=",dense.budgetLimited,
         " simulation=",coarsePhysics ? "source" : "dense");
  }

std::vector<uint32_t> pinned(const Definition& def, const std::vector<Vec3>& vertices) {
  // Rules select an edge of the cloth material's own bounds in centimetres.
  // Multiple rules (for example both sail corners) are separated by ';'.
  std::vector<uint32_t> out;
  Vec3 lo = vertices.front(), hi = lo;
  const auto component = [](const Vec3& p, size_t axis) {
    return axis==0 ? p.x : axis==1 ? p.y : p.z;
    };
  for(const auto& p:vertices)
    {
    lo = Vec3(std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z));
    hi = Vec3(std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z));
    }
  std::istringstream rules(def.pins);
  for(std::string rule;std::getline(rules,rule,';');) {
    std::istringstream fields(rule);
    std::string axisText, side, toleranceText;
    if(!std::getline(fields,axisText,':') || !std::getline(fields,side,':') ||
       !std::getline(fields,toleranceText,':'))
      continue;
    const size_t axis = axisText=="x" ? 0 : axisText=="y" ? 1 : axisText=="z" ? 2 : 3;
    if(axis==3 || (side!="min" && side!="max"))
      continue;
    try {
      const float tolerance = std::max(0.f,std::stof(toleranceText));
      const float edge = component(side=="min" ? lo : hi,axis);
      for(uint32_t i=0;i<vertices.size();++i)
        if(std::abs(component(vertices[i],axis)-edge)<=tolerance)
          out.push_back(i);
      }
    catch(const std::exception&) {
      }
    }
  std::sort(out.begin(),out.end());
  out.erase(std::unique(out.begin(),out.end()),out.end());
  return out;
  }

std::shared_ptr<Geometry> loadGeometry(std::string_view visual, const Definition& def) {
  static std::unordered_map<std::string,std::weak_ptr<Geometry>> cache;
  const auto key = std::string(visual);
  if(auto old=cache[key].lock())
    return old;
  std::string resource(visual);
  const bool morph = FileExt::hasExt(resource,"MMS") || FileExt::hasExt(resource,"MMB");
  FileExt::exchangeExt(resource,"3DS","MRM");
  FileExt::exchangeExt(resource,"MMS","MMB");
  asset::StaticMesh source;
  bool replaced = false;
  std::vector<asset::MorphAnimation> animations;
  if(morph) {
    auto mesh = Resources::loadMorphData(resource);
    source = std::move(mesh.mesh);
    animations = std::move(mesh.animations);
    }
  else
    source = Resources::loadVisualMesh(resource,&replaced);
  PackedMesh packed(source,PackedMesh::PK_VisualMorph);
  auto out = std::make_shared<Geometry>();
  out->explicitAlphaTest = replaced;
  out->animations = std::move(animations);
  std::vector<ClothGeometry::Vertex> vertices;
  std::vector<uint32_t> indices, triangleMaterials;
  std::vector<asset::Material> materials;
  std::vector<uint32_t> compact(packed.vertices.size(),uint32_t(-1));
  for(const auto& sub:packed.subMeshes) {
    if(!matches(def.material,sub.material.name))
      continue;
    const auto material = uint32_t(materials.size());
    materials.push_back(sub.material);
    for(size_t at=sub.iboOffset;at+2<sub.iboOffset+sub.iboLength;at+=3) {
      const uint32_t a=packed.indices[at], b=packed.indices[at+1], c=packed.indices[at+2];
      if(a==b || a==c || b==c)
        continue;
      const auto position = [&](uint32_t i) {
        const auto& v=packed.vertices[i];
        return Vec3(v.pos[0],v.pos[1],v.pos[2]);
        };
      if(Vec3::crossProduct(position(b)-position(a),position(c)-position(a)).quadLength()<1e-6f)
        continue;
      for(const uint32_t id:{a,b,c}) {
        if(compact[id]==uint32_t(-1)) {
          compact[id] = uint32_t(vertices.size());
          const auto& v = packed.vertices[id];
          ClothGeometry::Vertex vertex;
          std::copy(v.pos,v.pos+3,vertex.pos);
          std::copy(v.norm,v.norm+3,vertex.norm);
          std::copy(v.uv,v.uv+2,vertex.uv);
          vertex.color = v.color;
          vertices.push_back(vertex);
          out->sourceVertexIds.push_back(packed.verticesId[id]);
          }
        indices.push_back(compact[id]);
        }
      triangleMaterials.push_back(material);
      }
    }
  if(vertices.empty())
    return nullptr;
  std::vector<Vec3> points;
  for(const auto& v:vertices)
    points.emplace_back(v.pos[0],v.pos[1],v.pos[2]);
  const auto pins = pinned(def,points);
  if(pins.empty() || pins.size()==vertices.size()) {
    Log::e("cloth needs both fixed and free vertices: ",visual);
    return nullptr;
    }
  tessellate(*out,vertices,indices,pins,materials,triangleMaterials,visual);
  out->fullProto = Resources::loadMesh(visual);
  // A standard retains its rigid pole and skull in rendering and collision.
  auto rigid = source;
  rigid.sub_meshes.erase(std::remove_if(rigid.sub_meshes.begin(),rigid.sub_meshes.end(),[&](const auto& sub) {
    return matches(def.material,sub.mat.name);
    }),rigid.sub_meshes.end());
  if(!rigid.sub_meshes.empty()) {
    PackedMesh rigidPacked(rigid,PackedMesh::PK_VisualMorph,replaced);
    if(out->animations.empty())
      out->rigidProto = std::make_unique<ProtoMesh>(std::move(rigidPacked),visual);
    else
      out->rigidProto = std::make_unique<ProtoMesh>(std::move(rigidPacked),out->animations,visual);
    }
  cache[key] = out;
  return out;
  }
}

struct ClothVisual::State {
  MeshObjects::Mesh* view = nullptr;
  World* world = nullptr;
  std::shared_ptr<Geometry> geometry;
  MeshObjects::Mesh clothView, rigidView, fallbackView;
  std::shared_ptr<Cloth::Instance> physics;
  std::unique_ptr<PhysicMesh> collider;
  bool surface = false;
  bool nativeCollision = false, colliderInitialized = false, colliderClothEnabled = false;
  bool nativeMovable = false;
  Matrix4x4 matrix = Matrix4x4::mkIdentity();
  Matrix4x4 referenceTransform = Matrix4x4::mkIdentity();
  uint64_t referenceTick = uint64_t(-1);
  asset::AnimationType mode = asset::AnimationType::NONE;
  float strength = 0;
  std::vector<Vec4> deformation;
  std::vector<Vec3> references, normals, interpolatedPositions;

  Vec3 reference(uint32_t index, const Vec3& local) {
    const auto tick = world->tickCount();
    if(referenceTick!=tick) {
      auto source = geometry->sourcePositions;
      // Read the native animator rather than owning a second clock. The same
      // frames and 16-bit alpha/intensity are interpolated onto the dense mesh.
      if(view!=nullptr)
        for(size_t a=0;a<geometry->animations.size();++a) {
          const auto& animation = geometry->animations[a];
          const auto sample = view->morphSample(animation.name,tick);
          if(sample.intensity<=0.f || animation.frame_count==0)
            continue;
          const size_t stride = animation.samples.size()/animation.frame_count;
          for(size_t i=0;i<source.size();++i) {
            const int32_t id = geometry->morphIndices[a][i];
            if(id<0 || size_t(id)>=stride)
              continue;
            const size_t f0 = sample.frame0*stride+size_t(id), f1 = sample.frame1*stride+size_t(id);
            if(f0>=animation.samples.size() || f1>=animation.samples.size())
              continue;
            const auto& p0 = animation.samples[f0];
            const auto& p1 = animation.samples[f1];
            source[i] += (Vec3(p0.x,p0.y,p0.z)*(1.f-sample.alpha)+Vec3(p1.x,p1.y,p1.z)*sample.alpha)*sample.intensity;
            }
          }
      const float cycle = Gothic::settingsGetF("ENGINE","zWindCycleTime")+
                          Gothic::settingsGetF("ENGINE","zWindCycleTimeVar");
      const auto period = std::isfinite(cycle) && cycle>0.f ? uint64_t(cycle*1000.f) : uint64_t(0);
      const auto wind = WorldWind::shaderMatrix(matrix,tick,period,
                                                Gothic::settingsGetI("ENGINE","zWindEnabled")!=0,
                                                mode,strength,world->physic()->cloth().windScale());
      auto inverse = matrix;
      inverse.inverse();
      referenceTransform = inverse*wind;
      if(geometry->coarsePhysics) {
        references = std::move(source);
        for(auto& point:references)
          referenceTransform.project(point);
        }
      else {
        references.resize(geometry->sourceWeights.size());
        for(size_t i=0;i<references.size();++i) {
          const auto& weights = geometry->sourceWeights[i];
          Vec3 point;
          for(size_t j=0;j<weights.vertices.size();++j)
            if(weights.weights[j]!=0.f)
              point += source[weights.vertices[j]]*weights.weights[j];
          referenceTransform.project(point);
          references[i] = point;
          }
        }
      referenceTick = tick;
      }
    return index<references.size() ? references[index] : local;
    }

  void syncCollider() {
    if(!nativeCollision)
      return;
    const bool enabled = world->physic()->cloth().enabled();
    if(colliderInitialized && colliderClothEnabled==enabled)
      return;
    collider.reset();
    colliderInitialized = true;
    colliderClothEnabled = enabled;
    const auto* source = enabled ? geometry->rigidProto.get() : geometry->fullProto;
    if(source!=nullptr) {
      collider = std::make_unique<PhysicMesh>(*source,*world->physic(),nativeMovable);
      collider->setObjMatrix(matrix);
      }
    }

  void setupViews() {
    clothView = world->addStaticView(geometry->clothProto.get(),false);
    clothView.setVisible(false);
    if(geometry->rigidProto) {
      rigidView = world->addStaticView(geometry->rigidProto.get(),false);
      rigidView.setWind(mode,strength);
      rigidView.setVisible(false);
      }
    }

  void bindPhysics(float stiffness, std::string visual, uint32_t id) {
    physics = world->physic()->cloth().add(geometry->physicsPositions,geometry->physicsTriangles,
                                          geometry->pins,matrix,stiffness,std::move(visual),id);
    }

  void setupCallbacks(const std::shared_ptr<State>& self) {
    const auto weak = std::weak_ptr<State>(self);
    physics->setShaderReference([weak](uint32_t index, const Vec3& local, const Vec3&) {
      if(auto state=weak.lock())
        return state->reference(index,local);
      return local;
      });
    physics->setUpdateCallback([weak](const std::vector<Vec3>& positions, float weight) {
      if(auto state=weak.lock())
        state->update(positions,weight);
      });
    }

  void update(const std::vector<Vec3>& positions, float weight) {
    syncCollider();
    const bool enabled = world->physic()->cloth().enabled();
    const bool simulated = enabled && weight>0.f;
    if(view!=nullptr) {
      view->setVisible(!simulated && (!surface || enabled));
      view->setObjMatrix(matrix);
      }
    clothView.setVisible(simulated);
    rigidView.setVisible(simulated);
    rigidView.setObjMatrix(matrix);
    if(!simulated) {
      clothView.setDeformation({});
      return;
      }
    if(geometry->coarsePhysics) {
      // The callback has already blended each coarse particle with its exact
      // native fallback. Barycentric interpolation commutes with that blend,
      // so every dense visible vertex keeps the same seamless transition.
      interpolatedPositions.assign(geometry->sourceWeights.size(),Vec3());
      for(size_t i=0;i<interpolatedPositions.size();++i) {
        const auto& weights = geometry->sourceWeights[i];
        for(size_t j=0;j<weights.vertices.size();++j) {
          const auto source = weights.vertices[j];
          const auto& point = source<positions.size() ? positions[source] : geometry->sourcePositions[source];
          interpolatedPositions[i] += point*weights.weights[j];
          }
        }
      }
    const auto& visiblePositions = geometry->coarsePhysics ? interpolatedPositions : positions;
    normals.assign(geometry->positions.size(),Vec3());
    const auto position = [&](uint32_t id) {
      return id<visiblePositions.size() ? visiblePositions[id] : geometry->positions[id];
      };
    for(size_t i=0;i+2<geometry->triangles.size();i+=3) {
      const uint32_t a=geometry->triangles[i], b=geometry->triangles[i+1], c=geometry->triangles[i+2];
      Vec3 normal = Vec3::crossProduct(position(b)-position(a),position(c)-position(a));
      const auto rest = Vec3::crossProduct(geometry->positions[b]-geometry->positions[a],geometry->positions[c]-geometry->positions[a]);
      if(Vec3::dotProduct(rest,geometry->normals[a])<0)
        normal = -normal;
      for(const uint32_t id:{a,b,c})
        normals[id] += normal;
      }
    for(size_t i=0;i<normals.size();++i) {
      Vec3 native = geometry->normals[i];
      float w = 0;
      referenceTransform.project(native.x,native.y,native.z,w);
      const Vec3 simulatedNormal = normals[i].quadLength()>1e-8f ? Vec3::normalize(normals[i]) : geometry->normals[i];
      normals[i] = native*(1.f-weight)+simulatedNormal*weight;
      if(normals[i].quadLength()>1e-8f)
        normals[i] = Vec3::normalize(normals[i]);
      }
    deformation.assign(geometry->packedToPhysics.size()*2,Vec4());
    for(size_t i=0;i<geometry->packedToPhysics.size();++i) {
      const auto id = geometry->packedToPhysics[i];
      if(id>=geometry->positions.size())
        continue; // padding of the packed meshlet
      const auto p = position(id), n = normals[id];
      deformation[i*2] = Vec4(p.x,p.y,p.z,1.f);
      deformation[i*2+1] = Vec4(n.x,n.y,n.z,0);
      }
    clothView.setObjMatrix(matrix);
    clothView.setDeformation(deformation);
    }
  };

ClothVisual::ClothVisual() = default;
ClothVisual::~ClothVisual() = default;

bool ClothVisual::supports(std::string_view visual) {
  return definition(visual)!=nullptr;
  }

std::unique_ptr<ClothVisual> ClothVisual::create(MeshObjects::Mesh& view, World& world,
                                               const asset::VirtualObject& vob, bool enableCollision) {
  const auto* def = definition(vob.visual->name);
  if(def==nullptr || view.isEmpty())
    return nullptr;
  try {
    auto geometry = loadGeometry(vob.visual->name,*def);
    if(geometry==nullptr)
      return nullptr;
    auto out = std::unique_ptr<ClothVisual>(new ClothVisual());
    out->state = std::make_shared<State>();
    auto& state = *out->state;
    state.view = &view;
    state.world = &world;
    state.geometry = std::move(geometry);
    state.nativeCollision = enableCollision;
    state.nativeMovable = vob.visual->type==asset::VisualType::MORPH_MESH;
    state.mode = vob.anim_mode;
    state.strength = vob.anim_strength;
    if(state.strength!=0 && state.mode==asset::AnimationType::NONE)
      state.mode = asset::AnimationType::WIND_ALT;
    if(state.strength==0)
      state.mode = asset::AnimationType::NONE;
    state.bindPhysics(def->stiffness,vob.visual->name,vob.id);
    if(!state.physics)
      return nullptr;
    if(def->attachmentRadius>0.f)
      state.physics->setAttachmentCapsule(def->attachmentFrom,def->attachmentTo,def->attachmentRadius);
    state.setupViews();
    state.setupCallbacks(out->state);
    state.syncCollider();
    return out;
    }
  catch(const std::exception& error) {
    Log::e("cloth mesh unavailable: ",vob.visual->name,": ",error.what());
    return nullptr;
    }
  }

std::unique_ptr<ClothVisual> ClothVisual::createSurface(World& world, const asset::WorldMesh& source,
                                                      const std::vector<uint32_t>& pinnedSourcePositionIds,
                                                      std::string visual, uint32_t id, float stiffness, float linearDamping) {
  try {
    if(source.vertices.empty() || source.polygons.vertex_indices.empty())
      return nullptr;
    auto local = source;
    Vec3 lo(local.vertices.front().x,local.vertices.front().y,local.vertices.front().z), hi=lo;
    for(const auto& p:local.vertices) {
      lo = Vec3(std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z));
      hi = Vec3(std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z));
      }
    const auto center = (lo+hi)*.5f;
    for(auto& p:local.vertices) {
      p.x -= center.x;
      p.y -= center.y;
      p.z -= center.z;
      }
    local.bbox.min = {lo.x-center.x,lo.y-center.y,lo.z-center.z};
    local.bbox.max = {hi.x-center.x,hi.y-center.y,hi.z-center.z};
    auto geometry = std::make_shared<Geometry>();
    std::vector<ClothGeometry::Vertex> vertices;
    std::vector<uint32_t> indices, pins, triangleMaterials;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> compact;
    for(size_t at=0;at<local.polygons.vertex_indices.size();at+=3) {
      if(at+2>=local.polygons.vertex_indices.size())
        throw std::runtime_error("cloth surface needs triangles");
      for(size_t j=0;j<3;++j) {
        const auto sourcePosition = local.polygons.vertex_indices[at+j];
        const auto sourceFeature = local.polygons.feature_indices[at+j];
        if(sourcePosition>=local.vertices.size() || sourceFeature>=local.features.size())
          throw std::runtime_error("cloth surface has an invalid vertex");
        const auto key = std::make_pair(sourcePosition,sourceFeature);
        auto found = compact.find(key);
        if(found==compact.end()) {
          const auto vertex = uint32_t(vertices.size());
          found = compact.emplace(key,vertex).first;
          const auto& p = local.vertices[sourcePosition];
          const auto& f = local.features[sourceFeature];
          vertices.push_back({{p.x,p.y,p.z},{f.normal.x,f.normal.y,f.normal.z},
                              {f.texture.x,f.texture.y},f.light});
          geometry->sourceVertexIds.push_back(sourcePosition);
          if(std::find(pinnedSourcePositionIds.begin(),pinnedSourcePositionIds.end(),sourcePosition)!=pinnedSourcePositionIds.end())
            pins.push_back(vertex);
          }
        indices.push_back(found->second);
        }
      const auto material = local.polygons.material_indices[at/3];
      if(material>=local.materials.size())
        throw std::runtime_error("cloth surface has an invalid material");
      triangleMaterials.push_back(material);
      }
    if(pins.empty() || pins.size()==vertices.size())
      throw std::runtime_error("cloth surface needs fixed and free vertices");
    const bool furledSail = local.materials.size()==1 && local.materials.front().name=="NW_SHIP_SAIL_02";
    const bool shipFlag = local.materials.size()==1 &&
                          (local.materials.front().name=="NW_SHIP_FLAG_01" || local.materials.front().name=="NW_SHIP_FLAG_02");
    if(shipFlag) {
      // The authored 5x5 flags need a finer simulation, but not every visible
      // subdivision needs a particle. An intermediate 9x9 grid drives the
      // unchanged dense mesh through barycentric weights, just like the sails.
      ClothGeometry::Options options;
      options.targetEdgeCm=100.f;
      options.minParticles=0;
      options.maxParticles=100;
      options.maxDepth=1;
      const auto simulation=ClothGeometry::subdivide(vertices,indices,pins,options);
      const size_t children=size_t(1)<<(simulation.subdivisionDepth*2);
      std::vector<uint32_t> simulationMaterials;
      for(auto material:triangleMaterials)
        simulationMaterials.insert(simulationMaterials.end(),children,material);
      tessellate(*geometry,simulation.vertices,simulation.indices,simulation.pins,
                 local.materials,simulationMaterials,visual,true);
      }
    else {
      tessellate(*geometry,vertices,indices,pins,local.materials,triangleMaterials,visual,furledSail);
      }
    PackedMesh original(local,PackedMesh::PK_Visual);
    geometry->ownedFullProto = std::make_unique<ProtoMesh>(std::move(original),visual);
    geometry->fullProto = geometry->ownedFullProto.get();
    auto out = std::unique_ptr<ClothVisual>(new ClothVisual());
    out->state = std::make_shared<State>();
    auto& state = *out->state;
    state.world = &world;
    state.geometry = std::move(geometry);
    state.surface = true;
    state.matrix.translate(center);
    state.fallbackView = world.addStaticView(state.geometry->fullProto,false);
    state.view = &state.fallbackView;
    state.view->setObjMatrix(state.matrix);
    state.view->setVisible(world.physic()->cloth().enabled());
    state.bindPhysics(stiffness,std::move(visual),id);
    if(!state.physics)
      return nullptr;
    state.physics->setLinearDamping(linearDamping);
    state.setupViews();
    state.setupCallbacks(out->state);
    return out;
    }
  catch(const std::exception& error) {
    Log::e("cloth surface unavailable: ",visual,": ",error.what());
    return nullptr;
    }
  }

void ClothVisual::setView(MeshObjects::Mesh& view) {
  state->view = &view;
  }

void ClothVisual::setMatrix(const Matrix4x4& matrix) {
  state->matrix = matrix;
  state->referenceTick = uint64_t(-1);
  state->physics->setMatrix(matrix);
  if(state->collider)
    state->collider->setObjMatrix(matrix);
  }
