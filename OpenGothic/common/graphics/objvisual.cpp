#include "objvisual.h"

#include <Tempest/Log>

#include "world/world.h"
#include "game/serialize.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/clothvisual.h"
#include "utils/fileext.h"
#include "physics/fracture.h"

#include <map>
#include <unordered_map>

namespace {
Tempest::Matrix4x4 hiddenFractureMatrix() {
  auto matrix = Tempest::Matrix4x4::mkIdentity();
  matrix.translate(0,-10000000.f,0);
  return matrix;
  }
}

struct ObjVisual::FractureView {
  struct Geometry {
    struct Batch {
      size_t piece = 0;
      std::unique_ptr<ProtoMesh> mesh;
      };
    std::shared_ptr<const FractureMesh> source;
    std::vector<Batch> batches;
    };

  ObjVisual* owner = nullptr;
  std::shared_ptr<Geometry> geometry;
  std::vector<MeshObjects::Mesh> views;
  bool broken = false;

  static std::shared_ptr<Geometry> load(std::shared_ptr<const FractureMesh> mesh) {
    static std::unordered_map<const FractureMesh*,std::weak_ptr<Geometry>> cache;
    if(auto existing = cache[mesh.get()].lock())
      return existing;
    auto geometry = std::make_shared<Geometry>();
    geometry->source = std::move(mesh);
    for(size_t piece=0;piece<geometry->source->pieces.size();++piece) {
      std::map<std::string,std::vector<const FractureMesh::Triangle*>> materials;
      for(const auto& triangle:geometry->source->pieces[piece].triangles)
        materials[triangle.texture].push_back(&triangle);
      for(const auto& [texture,triangles]:materials) {
        Material material;
        material.setAlbedo(Resources::loadMaterialAlbedo(texture));
        // The small-mesh constructor accepts one 64-vertex meshlet. Keep flat
        // face normals and independent UVs by batching at most 20 triangles.
        for(size_t first=0;first<triangles.size();first+=20) {
          std::vector<Resources::Vertex> vertices;
          std::vector<uint32_t> indices;
          const size_t end = std::min(first+20,triangles.size());
          for(size_t t=first;t<end;++t)
            // Gothic draw meshes wind clockwise around their outward normal;
            // the cache winds outward counterclockwise for hull validation.
            for(const size_t corner:{size_t(0),size_t(2),size_t(1)}) {
              const auto& vertex = triangles[t]->vertices[corner];
              Resources::Vertex out = {};
              out.pos[0] = vertex.position.x;
              out.pos[1] = vertex.position.y;
              out.pos[2] = vertex.position.z;
              out.norm[0] = vertex.normal.x;
              out.norm[1] = vertex.normal.y;
              out.norm[2] = vertex.normal.z;
              out.uv[0] = vertex.uv.x;
              out.uv[1] = vertex.uv.y;
              out.color = 0xFFFFFFFFu;
              indices.push_back(uint32_t(vertices.size()));
              vertices.push_back(out);
              }
          geometry->batches.push_back({piece,std::make_unique<ProtoMesh>(material,std::move(vertices),std::move(indices))});
          }
        }
      }
    cache[geometry->source.get()] = geometry;
    return geometry;
    }

  void update(bool value, const std::vector<Tempest::Matrix4x4>& matrices) {
    const bool changed = broken!=value;
    broken = value;
    if(changed)
      owner->setObjMatrix(owner->renderMatrix);
    for(size_t i=0;i<views.size();++i) {
      const size_t piece = geometry->batches[i].piece;
      views[i].setObjMatrix(broken && piece<matrices.size() ? matrices[piece] : hiddenFractureMatrix());
      }
    }
  };

ObjVisual::ObjVisual() {
  }

ObjVisual::ObjVisual(ObjVisual&& other) {
  renderMatrix = other.renderMatrix;
  setType(other.type);
  other.type = M_None;

  switch(type) {
    case M_None:
      break;
    case M_Mdl:
      mdl = std::move(other.mdl);
      break;
    case M_Mesh:
      mesh = std::move(other.mesh);
      break;
    case M_Pfx:
      pfx = std::move(other.pfx);
      break;
    case M_Bundle:
      bundle = std::move(other.bundle);
      break;
    }
  fracture = std::move(other.fracture);
  if(fracture)
    fracture->owner = this;
  cloth = std::move(other.cloth);
  if(cloth)
    cloth->setView(mesh.view);
  }

ObjVisual& ObjVisual::operator =(ObjVisual&& other) {
  if(this==&other)
    return *this;
  renderMatrix = other.renderMatrix;
  fracture.reset();
  cloth.reset();
  setType(other.type);
  other.type = M_None;

  switch(type) {
    case M_None:
      break;
    case M_Mdl:
      mdl = std::move(other.mdl);
      break;
    case M_Mesh:
      mesh = std::move(other.mesh);
      break;
    case M_Pfx:
      pfx = std::move(other.pfx);
      break;
    case M_Bundle:
      bundle = std::move(other.bundle);
      break;
    }
  fracture = std::move(other.fracture);
  if(fracture)
    fracture->owner = this;
  cloth = std::move(other.cloth);
  if(cloth)
    cloth->setView(mesh.view);
  return *this;
  }

ObjVisual::~ObjVisual() {
  cleanup();
  }

void ObjVisual::save(Serialize& fout, const Interactive& mob) const {
  if(type==M_Mdl)
    return mdl.view.save(fout,mob);
  }

void ObjVisual::load(Serialize& fin, Interactive& mob) {
  if(type==M_Mdl)
    return mdl.view.load(fin,mob);
  }

void ObjVisual::cleanup() {
  cloth.reset();
  fracture.reset();
  switch(type) {
    case M_None:
      break;
    case M_Mdl:
      mdl.~Mdl();
      break;
    case M_Mesh:
      mesh.~Mesh();
      break;
    case M_Pfx:
      pfx.~PfxEmitter();
      break;
    case M_Bundle:
      bundle.~VobBundle();
      break;
    }
  type = M_None;
  }

void ObjVisual::setType(Type t) {
  if(t==type)
    return;
  cleanup();
  switch(t) {
    case M_None:
      break;
    case M_Mdl:
      new(&mdl) Mdl();
      break;
    case M_Mesh:
      new (&mesh) Mesh();
      break;
    case M_Pfx:
      new (&pfx) PfxEmitter();
      break;
    case M_Bundle:
      new(&bundle) VobBundle();
      break;
    }
  type = t;
  }

void ObjVisual::setVisual(const zenkit::IItem& hitem, World& world, bool staticDraw) {
  cleanup();

  if(FileExt::hasExt(hitem.visual,"ZEN")) {
    setType(M_Bundle);
    bundle = VobBundle(world,hitem.visual,(staticDraw ? Vob::Static : Vob::None));
    } else {
    setType(M_Mesh);
    mesh.view = world.addView(hitem);
    }
  }

void ObjVisual::setVisual(const asset::VirtualObject& vob, World& world, bool staticDraw, bool enablePhysics) {
  cleanup();

  const bool       enableCollision = vob.cd_dynamic && enablePhysics; // collide with player
  std::string_view visName         = vob.visual->name;  // *.PFX; *.TGA; *.3DS; *.MDS; *.ASC; *.MMS

  if(visName.empty())
    return;

  // NOTE: .ZEN / M_Bundle is unused only by items - just assert it for now
  assert(!FileExt::hasExt(visName,"ZEN"));

  switch (vob.visual->type) {
    case asset::VisualType::MESH:
    case asset::VisualType::MULTI_RESOLUTION_MESH: {
      auto view = Resources::loadMesh(visName);
      if(!view)
        return;
      setType(M_Mesh);
      mesh.proto = view;
      const bool windy = (vob.anim_mode!=asset::AnimationType::NONE && vob.anim_strength>0);
      if(vob.show_visual) {
        mesh.view = world.addStaticView(view,staticDraw);
        mesh.view.setWind(vob.anim_mode,vob.anim_strength);
        cloth = ClothVisual::create(mesh.view,world,vob,enableCollision && !windy);
        }

      if(vob.show_visual && enableCollision && !windy && !cloth) {
        mesh.physic = PhysicMesh(*view,*world.physic(),false);
        }
      break;
      }
    case asset::VisualType::MODEL:
    case asset::VisualType::MORPH_MESH:{
      // *.MDS; *.ASC; *.MMS
      auto visual = std::string(visName);
      FileExt::exchangeExt(visual,"ASC","MDL");

      auto view = Resources::loadMesh(visual);
      if(!view)
        return;
      // Authored cloth morph meshes have no skeleton. Keep their normal mesh
      // morph animation as the distant fallback while binding nearby Jolt cloth.
      if(vob.visual->type==asset::VisualType::MORPH_MESH && ClothVisual::supports(visName)) {
        setType(M_Mesh);
        mesh.proto = view;
        if(vob.show_visual) {
          mesh.view = world.addStaticView(view,staticDraw);
          mesh.view.setWind(vob.anim_mode,vob.anim_strength);
          cloth = ClothVisual::create(mesh.view,world,vob,enableCollision);
          // If a resource fails to bind, retain the native complete collider.
          if(!cloth && enableCollision)
            mesh.physic = PhysicMesh(*view,*world.physic(),true);
          }
        break;
        }
      setType(M_Mdl);
      mdl.proto = view;
      mdl.view.setYTranslationEnable(false);
      mdl.view.setVisual(view->skeleton.get());

      if(vob.show_visual) {
        if((view->skeleton==nullptr || view->skeleton->animation()==nullptr) && enableCollision)
          mdl.view.setVisualBody(world,world.addStaticView(view,true)); else
          mdl.view.setVisualBody(world,world.addView(view));
        }

      if(vob.show_visual && enableCollision) {
        mdl.physic = PhysicMesh(*view,*world.physic(),true);
        mdl.physic.setSkeleton(view->skeleton.get());
        }
      break;
      }
    case asset::VisualType::DECAL: {
      // *.TGA
      if(vob.sprite_camera_facing_mode!=asset::SpriteAlignment::NONE) {
        setType(M_Pfx);
        pfx = PfxEmitter(world,vob);
        pfx.setActive(true);
        pfx.setLooped(true);
        }
      else if(auto decal = dynamic_cast<const asset::VisualDecal*>(vob.visual.get())) {
        setType(M_Mesh);
        mesh.view = world.addDecalView(*decal);
        }
      break;
      }
    case asset::VisualType::PARTICLE_EFFECT: {
      // *.PFX
      setType(M_Pfx);
      pfx = PfxEmitter(world,vob);
      pfx.setActive(true);
      pfx.setLooped(true);
      break;
      }
    case asset::VisualType::AI_CAMERA:
    case asset::VisualType::UNKNOWN:
      break;
    }
  }

void ObjVisual::setObjMatrix(const Tempest::Matrix4x4& obj) {
  renderMatrix = obj;
  switch(type) {
    case M_None:
      break;
    case M_Mdl:
      mdl.view.setObjMatrix(obj,true);
      mdl.physic.setObjMatrix(obj);
      break;
    case M_Mesh:
      mesh.view.setObjMatrix(fracture && fracture->broken ? hiddenFractureMatrix() : obj);
      mesh.physic.setObjMatrix(obj);
      if(cloth)
        cloth->setMatrix(obj);
      break;
    case M_Pfx:
      pfx.setObjMatrix(obj);
      break;
    case M_Bundle:
      bundle.setObjMatrix(obj);
      break;
    }
  }

void ObjVisual::setupFracture(World& world, DynamicWorld::Item& body) {
  auto source = body.fractureMesh();
  if(source==nullptr || type!=M_Mesh)
    return;
  fracture = std::make_shared<FractureView>();
  fracture->owner = this;
  fracture->geometry = FractureView::load(std::move(source));
  for(const auto& batch:fracture->geometry->batches) {
    fracture->views.push_back(world.addView(batch.mesh.get()));
    fracture->views.back().setObjMatrix(hiddenFractureMatrix());
    }
  body.setFractureCallback([view=std::weak_ptr<FractureView>(fracture)](bool broken, const std::vector<Tempest::Matrix4x4>& matrices) {
    if(auto locked = view.lock())
      locked->update(broken,matrices);
    });
  }

bool ObjVisual::renderPosition(Tempest::Vec3& out) const {
  if(fracture && fracture->broken)
    return false;
  if(type==M_Pfx)
    return pfx.renderPosition(out);
  if(type==M_Mesh && !mesh.view.isEmpty()) {
    const auto m = mesh.view.node(0).position();
    out = Tempest::Vec3(m.at(3,0),m.at(3,1),m.at(3,2));
    return true;
    }
  return false;
  }

void ObjVisual::setSlotItem(MeshObjects::Mesh&& itm, std::string_view slot) {
  if(type==M_Mdl)
    mdl.view.setSlotItem(std::move(itm), slot);
  }

void ObjVisual::setInteractive(Interactive* it) {
  switch(type) {
    case M_None:
    case M_Pfx:
      break;
    case M_Mdl:
      mdl.physic.setInteractive(it);
      break;
    case M_Mesh:
      mesh.physic.setInteractive(it);
      break;
    case M_Bundle:
      //bundle.setObjMatrix(obj);
      break;
    }
  }

const Animation::Sequence* ObjVisual::startAnimAndGet(std::string_view name, uint64_t tickCount, bool force) {
  if(type==M_Mdl) {
    return mdl.view.startAnimAndGet(name,tickCount,force);
    }
  return nullptr;
  }

bool ObjVisual::hasAnim(std::string_view name) const {
  if(type==M_Mdl)
    return mdl.view.hasAnim(name);
  return false;
  }

bool ObjVisual::updateAnimation(Npc* npc, Interactive* mobsi, World& world, uint64_t dt, bool force) {
  if(type==M_Mdl) {
    bool ret = mdl.view.updateAnimation(npc,mobsi,world,dt,force);
    if(ret)
      mdl.view.syncAttaches();
    return ret;
    }
  return false;
  }

void ObjVisual::processLayers(World& world) {
  if(type==M_Mdl) {
    mdl.view.processLayers(world);
    }
  }

void ObjVisual::syncPhysics() {
  if(type==M_Mdl)
    mdl.physic.setPose(mdl.view.pose());
  if(type==M_Mesh)
    ;//mesh.physic.setObjMatrix(mesh.view.);
  }

const ProtoMesh* ObjVisual::protoMesh() const {
  if(type==M_Mdl)
    return mdl.proto;
  if(type==M_Mesh)
    return mesh.proto;
  return nullptr;
  }

std::string ObjVisual::animNames() const {
  return type==M_Mdl ? mdl.view.pose().animNames() : std::string();
  }

const Tempest::Matrix4x4& ObjVisual::bone(size_t i) const {
  if(type==M_Mdl)
    return mdl.view.pose().bone(i);
  static const Tempest::Matrix4x4 m;
  return m;
  }
