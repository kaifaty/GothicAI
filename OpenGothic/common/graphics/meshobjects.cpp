#include "meshobjects.h"

#include <Tempest/Log>

#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/attachbinder.h"
#include "graphics/mesh/protomesh.h"

#include "visualobjects.h"

MeshObjects::MeshObjects(VisualObjects& parent)
  :parent(&parent) {
  }

MeshObjects::~MeshObjects() {
  }

MeshObjects::Item MeshObjects::implGet(const StaticMesh &mesh, const StaticMesh::SubMesh& s,
                                       int32_t texVar, int32_t teethTex, int32_t bodyColor,
                                       bool staticDraw) {
  Material mat = s.material;
  if(teethTex!=0 || bodyColor!=0 || texVar!=0){
    if(s.texName=="HUM_TEETH_V0.TGA"){
      mat.setAlbedo(solveTex(s.material.tex,s.texName,teethTex,bodyColor));
      }
    else if(s.texName=="HUM_MOUTH_V0.TGA"){
      mat.setAlbedo(solveTex(s.material.tex,s.texName,teethTex,bodyColor));
      }
    else if(Resources::hasTextureVariant(s.texName)){
      mat.setAlbedo(solveTex(s.material.tex,s.texName,texVar,bodyColor));
      }
    }

  if(mat.tex==nullptr) {
    if(!s.texName.empty())
      Tempest::Log::e("texture not found: \"",s.texName,"\"");
    return MeshObjects::Item();
    }
  return parent->get(mesh,mat,s.iboOffset,s.iboLength,staticDraw);
  }

const Tempest::Texture2d *MeshObjects::solveTex(const Tempest::Texture2d *def, std::string_view format, int32_t v, int32_t c) {
  if(Resources::hasTextureVariant(format)){
    auto ntex = Resources::loadTexture(format,v,c);
    if(ntex!=nullptr)
      return ntex;
    }
  return def;
  }

void MeshObjects::Mesh::setSkeleton(const Skeleton *sk) {
  skeleton = sk;
  if(proto!=nullptr && skeleton!=nullptr)
    binder=Resources::bindMesh(*proto,*skeleton);
  }

void MeshObjects::Mesh::setPose(const Tempest::Matrix4x4& obj, const Pose &p) {
  setPose(obj,p.transform());
  }

void MeshObjects::Mesh::setPose(const Tempest::Matrix4x4& obj, const Tempest::Matrix4x4* bones) {
  if(anim!=nullptr)
    anim->set(bones);
  implSetObjMatrix(obj,bones);
  }

void MeshObjects::Mesh::setAsGhost(bool g) {
  for(size_t i=0;i<subCount;++i)
    sub[i].setAsGhost(g);
  }

void MeshObjects::Mesh::setFatness(float f) {
  for(size_t i=0;i<subCount;++i)
    sub[i].setFatness(f);
  }

void MeshObjects::Mesh::setWind(asset::AnimationType m, float intensity) {
  for(size_t i=0;i<subCount;++i)
    sub[i].setWind(m,intensity);
  }

void MeshObjects::Mesh::setVisible(bool visible) {
  for(size_t i=0;i<subCount;++i)
    sub[i].setVisible(visible);
  }

void MeshObjects::Mesh::setDeformation(const std::vector<Tempest::Vec4>& vertices) {
  for(size_t i=0;i<subCount;++i)
    sub[i].setDeformation(vertices);
  }

void MeshObjects::Mesh::startMMAnim(std::string_view anim, float intensity, uint64_t timeUntil, bool freeOnly) {
  for(size_t i=0;i<subCount;++i)
    sub[i].startMMAnim(anim,intensity,timeUntil,freeOnly);
  }

void MeshObjects::Mesh::setMMShape(std::string_view anim, uint32_t frame0, uint32_t frame1, float alpha, float intensity) {
  for(size_t i=0;i<subCount;++i)
    sub[i].setMMShape(anim,frame0,frame1,alpha,intensity);
  }

float MeshObjects::Mesh::morphWeight(std::string_view anim) const {
  float w = 0;
  for(size_t i=0;i<subCount;++i)
    w = std::max(w, sub[i].morphWeight(anim));
  return w;
  }

VisualObjects::MorphSample MeshObjects::Mesh::morphSample(std::string_view anim, uint64_t tick) const {
  for(size_t i=0;i<subCount;++i) {
    const auto sample = sub[i].morphSample(anim,tick);
    if(sample.intensity>0.f)
      return sample;
    }
  return {};
  }

Bounds MeshObjects::Mesh::bounds() const {
  if(nodesCount()==0)
    return Bounds();
  auto b = node(0).bounds();
  for(size_t i=1; i<nodesCount(); ++i)
    b.assign(b,node(i).bounds());
  return b;
  }

const PfxEmitterMesh* MeshObjects::Mesh::toMeshEmitter() const {
  if(auto p = proto)
    return Resources::loadEmiterMesh(p->fname);
  return nullptr;
  }

MeshObjects::Mesh::Mesh(MeshObjects::Mesh &&other) {
  *this = std::move(other);
  }

MeshObjects::Mesh::~Mesh() {
  }

MeshObjects::Mesh::Mesh(MeshObjects& owner, const StaticMesh& mesh, int32_t version, bool staticDraw) {
  cpuOnly = owner.parent==nullptr;
  if(cpuOnly) {
    for(auto& s:mesh.sub) {
      if(s.texName.find_first_of("VC")==std::string::npos && version!=0)
        continue;
      CpuNode n;
      n.bounds = mesh.bbox;
      n.material = s.material;
      n.mesh = &mesh;
      n.offset = uint32_t(s.iboOffset);
      n.length = uint32_t(s.iboLength);
      n.position.identity();
      cpuSub.push_back(std::move(n));
      }
    return;
    }
  sub.reset(new Item[mesh.sub.size()]);
  subCount = 0;
  for(size_t i=0;i<mesh.sub.size();++i) {
    // NOTE: light dragon hunter armour
    if(mesh.sub[i].texName.find_first_of("VC")==std::string::npos && version!=0)
      continue;
    Item it = owner.implGet(mesh,mesh.sub[i],0,0,version,staticDraw);
    if(it.isEmpty())
      continue;
    sub[subCount] = std::move(it);
    ++subCount;
    }
  }

MeshObjects::Mesh::Mesh(MeshObjects& owner, const StaticMesh& mesh,
                        int32_t texVar, int32_t teethTex, int32_t bodyColor) {
  cpuOnly = owner.parent==nullptr;
  if(cpuOnly) {
    for(auto& s:mesh.sub) {
      CpuNode n;
      n.bounds = mesh.bbox;
      n.material = s.material;
      n.mesh = &mesh;
      n.offset = uint32_t(s.iboOffset);
      n.length = uint32_t(s.iboLength);
      n.position.identity();
      cpuSub.push_back(std::move(n));
      }
    return;
    }
  sub.reset(new Item[mesh.sub.size()]);
  subCount = 0;
  for(size_t i=0;i<mesh.sub.size();++i){
    Item it = owner.implGet(mesh,mesh.sub[i],texVar,teethTex,bodyColor,false);
    if(it.isEmpty())
      continue;
    sub[subCount] = std::move(it);
    ++subCount;
    }
  }

MeshObjects::Mesh::Mesh(MeshObjects& owner, const ProtoMesh& mesh,
                        int32_t texVar, int32_t teethTex, int32_t bodyColor, bool staticDraw)
  :proto(&mesh) {
  cpuOnly = owner.parent==nullptr;
  if(cpuOnly) {
    for(auto& m:mesh.submeshId) {
      auto& att = mesh.attach[m.id];
      auto& s = att.sub[m.subId];
      CpuNode n;
      n.bounds = att.bbox;
      n.material = s.material;
      n.mesh = &att;
      n.offset = uint32_t(s.iboOffset);
      n.length = uint32_t(s.iboLength);
      n.position.identity();
      cpuSub.push_back(std::move(n));
      }
    for(auto& skin:mesh.skined)
      for(auto& s:skin.sub) {
        CpuNode n;
        n.bounds = skin.bbox;
        n.material = s.material;
        n.offset = uint32_t(s.iboOffset);
        n.length = uint32_t(s.iboSize);
        n.position.identity();
        cpuSub.push_back(std::move(n));
        }
    return;
    }
  const size_t skinnedCount = mesh.skinedNodesCount();
  sub.reset(new Item[mesh.submeshId.size()+skinnedCount]);
  subCount = 0;

  for(auto& m:mesh.submeshId) {
    auto& att = mesh.attach[m.id];
    auto& sub = att.sub[m.subId];
    Item  it  = owner.implGet(att,sub,texVar,teethTex,bodyColor,staticDraw);
    if(it.isEmpty())
      continue;
    this->sub[subCount] = std::move(it);
    subCount++;
    }

  size_t bonesCount = 0;
  for(size_t i=0; i<mesh.skined.size(); ++i)
    bonesCount = std::max(bonesCount,mesh.skined[i].bonesCount);

  if(bonesCount>0)
    anim.reset(new InstanceStorage::Id(owner.parent->alloc(bonesCount*sizeof(Tempest::Matrix4x4))));

  for(size_t i=0; i<mesh.skined.size(); ++i) {
    auto& skin = mesh.skined[i];
    for(auto& m:skin.sub) {
      Material mat = m.material;
      mat.setAlbedo(owner.solveTex(mat.tex,m.texName,texVar,bodyColor));
      if(mat.tex!=nullptr) {
        sub[subCount] = owner.parent->get(skin,mat,m.iboOffset,m.iboSize,*anim);
        ++subCount;
        } else {
        if(!m.texName.empty())
          Tempest::Log::e("texture not found: \"",m.texName,"\"");
        }
      }
    }

  if(mesh.morph.size()>0)
    startMMAnim(mesh.morph[0].name,1,uint64_t(-1));
  }

MeshObjects::Mesh &MeshObjects::Mesh::operator =(MeshObjects::Mesh &&other) {
  std::swap(sub,      other.sub);
  std::swap(subCount, other.subCount);
  std::swap(anim,     other.anim);
  std::swap(proto,    other.proto);
  std::swap(skeleton, other.skeleton);
  std::swap(binder,   other.binder);
  std::swap(cpuOnly,  other.cpuOnly);
  std::swap(cpuSub,   other.cpuSub);
  return *this;
  }

void MeshObjects::Mesh::setObjMatrix(const Tempest::Matrix4x4 &mt) {
  implSetObjMatrix(mt,skeleton==nullptr ? nullptr : skeleton->tr.data());
  }

void MeshObjects::Mesh::implSetObjMatrix(const Tempest::Matrix4x4& mt, const Tempest::Matrix4x4* tr) {
  const size_t binds = (binder==nullptr ? 0 : binder->bind.size());
  if(cpuOnly) {
    for(size_t i=0; i<cpuSub.size(); ++i) {
      const size_t id = i<binds ? binder->bind[i] : size_t(-1);
      cpuSub[i].position = tr!=nullptr && skeleton!=nullptr && id<skeleton->nodes.size() ? tr[id] : mt;
      }
    return;
    }
  for(size_t i=0; i<binds; ++i) {
    auto id = binder->bind[i];
    sub[i].setObjMatrix(tr[id]);
    }
  for(size_t i=binds; i<subCount; ++i)
    sub[i].setObjMatrix(mt);
  }

const Bounds& MeshObjects::Node::bounds() const {
  return cpu!=nullptr ? cpu->bounds : it->bounds();
  }

const Material& MeshObjects::Node::material() const {
  return cpu!=nullptr ? cpu->material : it->material();
  }

Tempest::Matrix4x4 MeshObjects::Node::position() const {
  return cpu!=nullptr ? cpu->position : it->position();
  }

const StaticMesh* MeshObjects::Node::mesh() const {
  return cpu!=nullptr ? cpu->mesh : it->mesh();
  }

std::pair<uint32_t, uint32_t> MeshObjects::Node::meshSlice() const {
  return cpu!=nullptr ? std::make_pair(cpu->offset,cpu->length) : it->meshSlice();
  }
