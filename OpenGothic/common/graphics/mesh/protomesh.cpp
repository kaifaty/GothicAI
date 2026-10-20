#include "protomesh.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/mesh/skeleton.h"
#include "physics/physicmeshshape.h"

using namespace Tempest;

ProtoMesh::Attach::~Attach() {
  }

// points of a mesh for the hit zones, MaxPoints at most (evenly picked); the empty vertices the meshlets
// are padded with (at the origin) are not
static constexpr size_t MaxPoints = 400;

static void thin(std::vector<Tempest::Vec3>& pt, size_t maxN) {
  if(pt.size()<=maxN)
    return;
  std::vector<Tempest::Vec3> out(maxN);
  for(size_t i=0; i<maxN; ++i)
    out[i] = pt[i*pt.size()/maxN];
  pt = std::move(out);
  }

static std::vector<Tempest::Vec3> pointsOf(const PackedMesh& pm) {
  std::vector<Tempest::Vec3> pt;
  for(auto& v:pm.vertices)
    if(v.pos[0]!=0.f || v.pos[1]!=0.f || v.pos[2]!=0.f)
      pt.emplace_back(v.pos[0],v.pos[1],v.pos[2]);
  thin(pt,MaxPoints);
  return pt;
  }

ProtoMesh::ProtoMesh(PackedMesh&& pm, std::string_view fname)
  :fname(fname) {
  attach.emplace_back(pm);
  submeshId.resize(attach[0].sub.size());
  points = pointsOf(pm);
  auto&  att   = attach[0];
  att.shape.reset(PhysicMeshShape::load(std::move(pm)));

  size_t count = 0;
  for(size_t r=0;r<att.sub.size();++r) {
    if(att.sub[r].material.tex==nullptr && !Resources::isHeadless()) {
      if(!att.sub[r].texName.empty())
        Tempest::Log::e("no texture: ",att.sub[r].texName);
      continue;
      }
    submeshId[count].id    = 0;
    submeshId[count].subId = r;
    count++;
    }
  submeshId.resize(count);

  nodes.emplace_back();
  nodes.back().attachId   = 0;
  nodes.back().submeshIdB = 0;
  nodes.back().submeshIdE = submeshId.size();
  nodes.back().transform.identity();

  bboxMesh[0] = pm.bbox().first;
  bboxMesh[1] = pm.bbox().second;
  setupScheme(fname);
  }

ProtoMesh::ProtoMesh(PackedMesh&& pm, const std::vector<zenkit::MorphAnimation>& aniList, std::string_view fname)
  : ProtoMesh(std::move(pm),fname) {
  if(attach.size()!=1) {
    Log::d("skip animations for: ",fname);
    return;
    }

  const size_t indexSz    = sizeof(int32_t[4])*((pm.verticesId.size()+3)/4);
  size_t       samplesCnt = 0;

  for(auto& i:aniList) {
    samplesCnt += i.samples.size();
    }

  if(Resources::hasGraphicsDevice()) {
    morphIndex   = Resources::ssbo(Tempest::Uninitialized, indexSz*aniList.size());
    morphSamples = Resources::ssbo(Tempest::Uninitialized, samplesCnt*sizeof(Vec4));
    }

  std::vector<int32_t> remapId;
  std::vector<Vec4>    samples;

  samplesCnt = 0;
  morph.resize(aniList.size());
  for(size_t i=0; i<aniList.size(); ++i) {
    remap(aniList[i],pm.verticesId,remapId,samples,samplesCnt);

    if(Resources::hasGraphicsDevice()) {
      morphIndex  .update(remapId.data(), i*indexSz,               remapId.size()*sizeof(remapId[0]));
      morphSamples.update(samples.data(), samplesCnt*sizeof(Vec4), samples.size()*sizeof(Vec4)      );
      }

    morph[i] = mkAnimation(aniList[i]);
    morph[i].index = (i*indexSz)/sizeof(int32_t);

    samplesCnt += samples.size();
    }

  if(morph.size()>0) {
    for(auto& a:attach) {
      a.morph.anim    = &morph;
      a.morph.index   = &morphIndex;
      a.morph.samples = &morphSamples;
      }
    }
  }

ProtoMesh::ProtoMesh(const zenkit::Model& library, std::unique_ptr<Skeleton>&& sk, std::string_view fname, bool explicitAlphaTest)
  :skeleton(std::move(sk)), fname(fname) {
  std::vector<std::vector<Tempest::Vec3>> attachPoints;
  for(auto& m:library.mesh.attachments) {
    PackedMesh pack(m.second,PackedMesh::PK_Visual,explicitAlphaTest);
    attachPoints.push_back(pointsOf(pack));
    attach.emplace_back(pack);
    auto& att = attach.back();
    att.name = m.first;
    att.shape.reset(PhysicMeshShape::load(std::move(pack)));
    }

  nodes.resize(skeleton==nullptr ? 0 : skeleton->nodes.size());
  for(size_t i=0;i<nodes.size();++i) {
    Node& n   = nodes[i];
    auto& src = skeleton->nodes[i];
    for(size_t r=0;r<attach.size();++r)
      if(attach[r].name==src.name){
        n.attachId = r;
        break;
        }
    n.parentId  = src.parent;
    n.transform = src.tr;
    }

  for(auto& i:nodes)
    if(i.parentId<nodes.size())
      nodes[i.parentId].hasChild=true;

  size_t subCount=0;
  for(auto& i:nodes)
    if(i.attachId<attach.size()) {
      attach[i.attachId].hasNode = true;
      subCount+=attach[i.attachId].sub.size();
      }
  for(auto& a:attach)
    if(!a.hasNode)
      subCount+=a.sub.size();
  submeshId.resize(subCount);

  subCount=0;
  for(auto& i:nodes) {
    i.submeshIdB = subCount;
    if(i.attachId<attach.size()) {
      auto& att = attach[i.attachId];
      for(size_t r=0;r<att.sub.size();++r){
        if(att.sub[r].material.tex==nullptr && !Resources::isHeadless()) {
          if(!att.sub[r].texName.empty())
            Tempest::Log::e("no texture: ",att.sub[r].texName);
          continue;
          }
        submeshId[subCount].id    = i.attachId;
        submeshId[subCount].subId = r;
        subCount++;
        }
      }
    i.submeshIdE = subCount;
    }
  submeshId.resize(subCount);

  shapeOfAttach(attachPoints);
  for(size_t i=0;i<library.mesh.meshes.size();++i){
    auto& mesh = library.mesh.meshes[i];
    PackedMesh pkg(mesh);
    shapeOfSkin(pkg);
    skined.emplace_back(pkg);
    }
  for(auto& n:nodePoints)
    thin(n,MaxPoints);

  if(skeleton!=nullptr) {
    for(size_t i=0;i<skeleton->nodes.size();++i) {
      auto& n=skeleton->nodes[i];
      if(n.name.find("ZS_POS")==0){
        Pos p;
        p.name      = n.name;
        p.node      = i;
        p.transform = n.tr;
        pos.push_back(p);
        }
      }
    }
  setupScheme(fname);
  }

ProtoMesh::ProtoMesh(const zenkit::ModelHierarchy& library, std::unique_ptr<Skeleton>&& sk, std::string_view fname)
      :skeleton(std::move(sk)), fname(fname) {
  nodes.resize(skeleton == nullptr ? 0 : skeleton->nodes.size());
  for(size_t i = 0; i < nodes.size(); ++i) {
    Node& n = nodes[i];
    auto& src = skeleton->nodes[i];
    n.parentId = src.parent;
    n.transform = src.tr;
    }

  for(auto& i : nodes)
    if(i.parentId < nodes.size())
      nodes[i.parentId].hasChild = true;

  size_t subCount = 0;
  for(auto& i : nodes) {
    i.submeshIdB = subCount;
    i.submeshIdE = subCount;
    }
  submeshId.resize(subCount);

  if(skeleton != nullptr) {
    for(size_t i = 0; i < skeleton->nodes.size(); ++i) {
      auto& n = skeleton->nodes[i];
      if(n.name.find("ZS_POS") == 0) {
        Pos p;
        p.name = n.name;
        p.node = i;
        p.transform = n.tr;
        pos.push_back(p);
        }
      }
    }
  setupScheme(fname);
  }

ProtoMesh::ProtoMesh(const zenkit::ModelMesh& library, std::unique_ptr<Skeleton>&& sk, std::string_view fname, bool explicitAlphaTest)
  :skeleton(std::move(sk)), fname(fname){
  std::vector<std::vector<Tempest::Vec3>> attachPoints;
  for(auto& m:library.attachments) {
    PackedMesh pack(m.second,PackedMesh::PK_Visual,explicitAlphaTest);
    attachPoints.push_back(pointsOf(pack));
    attach.emplace_back(pack);
    auto& att = attach.back();
    att.name = m.first;
    att.shape.reset(PhysicMeshShape::load(std::move(pack)));
    }

  nodes.resize(skeleton==nullptr ? 0 : skeleton->nodes.size());
  for(size_t i=0;i<nodes.size();++i) {
    Node& n   = nodes[i];
    auto& src = skeleton->nodes[i];
    for(size_t r=0;r<attach.size();++r)
      if(attach[r].name==src.name){
        n.attachId = r;
        break;
        }
    n.parentId  = src.parent;
    n.transform = src.tr;
    }

  for(auto& i:nodes)
    if(i.parentId<nodes.size())
      nodes[i.parentId].hasChild=true;

  size_t subCount=0;
  for(auto& i:nodes)
    if(i.attachId<attach.size()) {
      attach[i.attachId].hasNode = true;
      subCount+=attach[i.attachId].sub.size();
      }
  for(auto& a:attach)
    if(!a.hasNode)
      subCount+=a.sub.size();
  submeshId.resize(subCount);

  subCount=0;
  for(auto& i:nodes) {
    i.submeshIdB = subCount;
    if(i.attachId<attach.size()) {
      auto& att = attach[i.attachId];
      for(size_t r=0;r<att.sub.size();++r){
        if(att.sub[r].material.tex==nullptr && !Resources::isHeadless()) {
          if(!att.sub[r].texName.empty())
            Tempest::Log::e("no texture: ",att.sub[r].texName);
          continue;
          }
        submeshId[subCount].id    = i.attachId;
        submeshId[subCount].subId = r;
        subCount++;
        }
      }
    i.submeshIdE = subCount;
    }
  submeshId.resize(subCount);

  shapeOfAttach(attachPoints);
  for(const auto &i : library.meshes) {
    if(i.weights.size()<i.mesh.positions.size()){
      Log::e("Vertex stream for skin-mesh \"", fname, "\" is too small");
      }
    PackedMesh pkg(i);
    shapeOfSkin(pkg);
    skined.emplace_back(pkg);
    }
  for(auto& n:nodePoints)
    thin(n,MaxPoints);

  if(skeleton!=nullptr) {
    for(size_t i=0;i<skeleton->nodes.size();++i) {
      auto& n=skeleton->nodes[i];
      if(n.name.find("ZS_POS")==0){
        Pos p;
        p.name      = n.name;
        p.node      = i;
        p.transform = n.tr;
        pos.push_back(p);
        }
      }
    }
  setupScheme(fname);
  }

ProtoMesh::ProtoMesh(const Material& mat, std::vector<Resources::Vertex> vbo, std::vector<uint32_t> ibo) {
  attach.emplace_back(mat,std::move(vbo),std::move(ibo));
  submeshId.resize(attach[0].sub.size());
  auto& att = attach[0];

  size_t count = 0;
  for(size_t r=0;r<att.sub.size();++r) {
    if(att.sub[r].material.tex==nullptr && !Resources::isHeadless()) {
      if(!att.sub[r].texName.empty())
        Tempest::Log::e("no texture: ",att.sub[r].texName);
      continue;
      }
    submeshId[count].id    = 0;
    submeshId[count].subId = r;
    count++;
    }
  submeshId.resize(count);

  nodes.emplace_back();
  nodes.back().attachId   = 0;
  nodes.back().submeshIdB = 0;
  nodes.back().submeshIdE = submeshId.size();
  nodes.back().transform.identity();

  setupScheme("");
  }

ProtoMesh::~ProtoMesh() {
  }

void ProtoMesh::shapeOfAttach(const std::vector<std::vector<Tempest::Vec3>>& pt) {
  // the shape for the hit zones: a rigid part at its bone...
  attachPoints = pt;
  nodePoints.resize(nodes.size());
  for(size_t i=0; i<nodes.size(); ++i)
    if(nodes[i].attachId<attachPoints.size())
      nodePoints[i] = attachPoints[nodes[i].attachId];
  }

void ProtoMesh::shapeOfSkin(const PackedMesh& pkg) {
  // Meshlet buffers contain padding. Retain only vertices referenced by the
  // actual index buffer, including seam normals used by the fatness shader.
  std::vector<bool> used(pkg.verticesA.size(),false);
  for(const auto index:pkg.indices)
    if(index<pkg.verticesA.size() && !used[index]) {
      used[index]=true;
      skinVertices.push_back(pkg.verticesA[index]);
      }
  // ...a skinned vertex at the bone it follows the most, in that bone's frame
  for(auto& v:pkg.verticesA) {
    int best = -1;
    for(int r=0; r<4; ++r)
      if(v.weights[r]>0.f && (best<0 || v.weights[r]>v.weights[best]))
        best = r;
    if(best<0)
      continue;
    // the bones of the skeleton it is put on (a body mesh comes without its own)
    if(v.boneId[best]>=nodePoints.size())
      nodePoints.resize(v.boneId[best]+1u);
    nodePoints[v.boneId[best]].emplace_back(v.pos[best][0],v.pos[best][1],v.pos[best][2]);
    }
  }

bool ProtoMesh::minSkinnedVertex(const Matrix4x4* worldBones,size_t count,const Matrix4x4& object,
                                float fatness,Vec3& point) const {
  if(worldBones==nullptr || count==0 || skinVertices.empty() || !std::isfinite(fatness))
    return false;
  Vec3 minimum;
  bool found=false;
  for(const auto& vertex:skinVertices) {
    Vec3 posed;
    float weight=0.f;
    for(size_t r=0;r<4;++r) {
      if(!std::isfinite(vertex.weights[r]) || vertex.weights[r]<0.f)
        return false;
      if(vertex.weights[r]==0.f)
        continue;
      if(vertex.boneId[r]>=count)
        return false;
      Vec3 part(vertex.pos[r][0],vertex.pos[r][1],vertex.pos[r][2]);
      worldBones[vertex.boneId[r]].project(part);
      posed+=part*vertex.weights[r];
      weight+=vertex.weights[r];
      }
    if(weight<=0.f)
      continue;
    Vec3 normal;
    normal.x=object.at(0,0)*vertex.norm[0]+object.at(1,0)*vertex.norm[1]+object.at(2,0)*vertex.norm[2];
    normal.y=object.at(0,1)*vertex.norm[0]+object.at(1,1)*vertex.norm[1]+object.at(2,1)*vertex.norm[2];
    normal.z=object.at(0,2)*vertex.norm[0]+object.at(1,2)*vertex.norm[1]+object.at(2,2)*vertex.norm[2];
    posed+=normal*(fatness*.5f);
    if(!std::isfinite(posed.x) || !std::isfinite(posed.y) || !std::isfinite(posed.z))
      return false;
    if(!found || posed.y<minimum.y) {
      minimum=posed;
      found=true;
      }
    }
  if(found)
    point=minimum;
  return found;
  }

size_t ProtoMesh::skinedNodesCount() const {
  size_t ret=0;
  for(auto& i:skined)
    ret+=i.sub.size();
  return ret;
  }

size_t ProtoMesh::findNode(std::string_view name, size_t def) const {
  if(skeleton==nullptr)
    return def;
  return skeleton->findNode(name,def);
  }

const Vec3* ProtoMesh::bbox() const {
  if(skeleton==nullptr)
    return bboxMesh;
  return skeleton->bbox;
  }

void ProtoMesh::setupScheme(std::string_view s) {
  auto sep = s.find("_");
  if(sep!=std::string::npos) {
    scheme = s.substr(0,sep);
    return;
    }
  sep = s.rfind(".");
  if(sep!=std::string::npos) {
    scheme = s.substr(0,sep);
    return;
    }
  scheme = s;
  }

void ProtoMesh::remap(const zenkit::MorphAnimation& a,
                      const std::vector<uint32_t>& vertId,
                      std::vector<int32_t>&        remap,
                      std::vector<Tempest::Vec4>&  samples,
                      size_t idOffset) {
  size_t indexSz = ((vertId.size()+3)/4)*4;
  remap.resize(indexSz);
  for(auto& i:remap)
    i = -1;
  for(size_t i=0; i<vertId.size(); ++i) {
    const uint32_t id = vertId[i];
    for(size_t r=0; r<a.vertices.size(); ++r)
      if(a.vertices[r]==id) {
        remap[i] = int(r+idOffset);
        break;
      }
  }

  samples.resize(a.samples.size());
  for(size_t i=0; i<a.samples.size(); ++i) {
    auto& s = a.samples[i];
    samples[i] = Tempest::Vec4(s.x,s.y,s.z,0);
  }
}

ProtoMesh::Morph ProtoMesh::mkAnimation(const zenkit::MorphAnimation& a) {
  Morph ret;
  ret.name            = a.name;
  ret.numFrames       = a.frame_count;
  ret.samplesPerFrame = a.samples.size()/a.frame_count;
  ret.layer           = a.layer;
  ret.duration        = uint64_t(a.duration>0 ? a.duration : 0);
  ret.blendIn         = std::abs(a.blend_in);
  ret.blendOut        = std::abs(a.blend_out);  // stored negative: the weight goes down

  if(a.flags&0x2 || a.duration<=0)
    ret.tickPerFrame = size_t(1.f/a.speed); else
    ret.tickPerFrame = size_t(a.duration/float(a.frame_count));

  if(ret.tickPerFrame==0)
    ret.tickPerFrame = 1;

  // how far each frame moves the vertices on average (lip sync picks mouth shapes by it)
  float maxStrength = 0;
  ret.strength.resize(ret.numFrames);
  for(size_t f=0; f<ret.numFrames && ret.samplesPerFrame>0; ++f) {
    float sum = 0;
    for(size_t i=0; i<ret.samplesPerFrame; ++i) {
      auto& s = a.samples[f*ret.samplesPerFrame+i];
      const float d = std::sqrt(s.x*s.x + s.y*s.y + s.z*s.z);
      sum     += d;
      ret.peak = std::max(ret.peak,d);
      ret.moveMin.x = std::min(ret.moveMin.x,s.x);
      ret.moveMin.y = std::min(ret.moveMin.y,s.y);
      ret.moveMin.z = std::min(ret.moveMin.z,s.z);
      ret.moveMax.x = std::max(ret.moveMax.x,s.x);
      ret.moveMax.y = std::max(ret.moveMax.y,s.y);
      ret.moveMax.z = std::max(ret.moveMax.z,s.z);
      }
    ret.strength[f] = sum/float(ret.samplesPerFrame);
    maxStrength     = std::max(maxStrength, ret.strength[f]);
    }
  for(auto& i:ret.strength)
    i = maxStrength>0 ? i/maxStrength : 0;
  return ret;
  }
