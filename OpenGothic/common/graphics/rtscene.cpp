#include "rtscene.h"

#include <Tempest/Log>

#include "graphics/mesh/submesh/staticmesh.h"

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <unordered_set>
#endif

using namespace Tempest;

static bool isSupported(Material::AlphaFunc alpha, RtScene::Category cat) {
  if(cat!=RtScene::Landscape && cat!=RtScene::Static)
    return false; // not supported
  if(alpha!=Material::Solid && alpha!=Material::AlphaTest && alpha!=Material::Water)
    return false; // not supported
  return true;
  }

RtScene::RtScene() {
  }

void RtScene::notifyTlas(const Material& mat, Category cat) const {
  if(!isSupported(mat.alpha, cat))
    return;
  needToUpdate = true;
  }

bool RtScene::isUpdateRequired() const {
  return needToUpdate;
  }

uint32_t RtScene::aquireBucketId(const Material& mat, const StaticMesh& mesh) {
  for(size_t i=build.tex.size(); i>0; ) {
    --i;
    if(mat.tex!=build.tex[i] || &mesh.vbo!=build.vbo[i] || &mesh.ibo!=build.ibo[i])
      continue;
    return uint32_t(i);
    }
  build.tex.push_back(mat.tex);
  build.vbo.push_back(&mesh.vbo);
  build.ibo.push_back(&mesh.ibo);
  return uint32_t(build.tex.size()-1);
  }

void RtScene::addInstance(const Matrix4x4& pos, const AccelerationStructure& blas,
                          const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength,
                          Category cat) {
  if(!isSupported(mat.alpha, cat))
    return;

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  build.triangles += iboLength/3;
#endif
  const uint32_t bucketId       = aquireBucketId(mat,mesh);
  const uint32_t firstPrimitive = uint32_t(firstIndex/3);

  RtObjectDesc desc = {};
  desc.instanceId     = bucketId;
  desc.firstPrimitive = firstPrimitive & 0x00FFFFFF; // 24 bit for primmitive + 8 for utility
  if(mat.alpha==Material::Solid)
    desc.bits |= 0x1;
  if(mat.alpha==Material::Water)
    desc.bits |= 0x2;

  RtInstance ix;
  ix.mat  = pos;
  ix.id   = uint32_t(build.rtDesc.size());
  ix.blas = &blas;
  if(mat.alpha==Material::Water)
    ix.flags = RtInstanceFlags::Opaque;
  else if(mat.alpha!=Material::Solid)
    ix.flags = RtInstanceFlags::NonOpaque;
  else
    ix.flags = RtInstanceFlags::Opaque;
  ix.flags = ix.flags | RtInstanceFlags::CullFlip;

  if(mat.alpha==Material::Solid)
    ix.mask = CullMask::CM_Opaque;
  else if(mat.alpha==Material::AlphaTest)
    ix.mask = CullMask::CM_Transparent;
  else if(mat.alpha==Material::Water)
    ix.mask = CullMask::CM_Water;
  else
    ix.mask = CullMask::CM_Unknown;

  if(mat.alpha==Material::Solid && cat==Landscape) {
    build.staticOpaque.geom  .push_back({mesh.vbo, mesh.ibo, firstIndex, iboLength});
    build.staticOpaque.rtDesc.push_back(desc);
    return;
    }
  if(mat.alpha==Material::AlphaTest && cat==Landscape) {
    build.staticAt.geom  .push_back({mesh.vbo, mesh.ibo, firstIndex, iboLength});
    build.staticAt.rtDesc.push_back(desc);
    return;
    }

  build.inst.push_back(ix);
  build.rtDesc.push_back(desc);
  }

void RtScene::addInstance(const BuildBlas& ctx, Tempest::AccelerationStructure& blas, RtInstanceFlags flags) {
  auto& device = Resources::device();
  blas = device.blas(ctx.geom);

  Tempest::RtInstance ix;
  ix.mat   = Matrix4x4::mkIdentity();
  ix.id    = uint32_t(build.rtDesc.size());
  ix.flags = ix.flags | flags;
  ix.flags = ix.flags | RtInstanceFlags::CullFlip;
  ix.blas  = &blas;
  build.inst.push_back(ix);

  build.rtDesc.insert(build.rtDesc.end(), ctx.rtDesc.begin(), ctx.rtDesc.end());
  }

void RtScene::buildTlas() {
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  const auto start = std::chrono::steady_clock::now();
#endif
  needToUpdate = false;

  Resources::recycle(std::move(blasStaticOpaque));
  Resources::recycle(std::move(blasStaticAt));

  addInstance(build.staticOpaque, blasStaticOpaque, Tempest::RtInstanceFlags::Opaque);
  addInstance(build.staticAt, blasStaticAt, Tempest::RtInstanceFlags::NonOpaque);

  Resources::recycle(std::move(tex));
  Resources::recycle(std::move(vbo));
  Resources::recycle(std::move(ibo));
  Resources::recycle(std::move(rtDesc));
  Resources::recycle(std::move(tlas));

  auto& device = Resources::device();
  tex    = device.descriptors(build.tex);
  textureIdentities=build.tex;
  textureGeneration=Resources::textureGeneration();
  vbo    = device.descriptors(build.vbo);
  ibo    = device.descriptors(build.ibo);
  rtDesc = device.ssbo(build.rtDesc);
  tlas   = device.tlas(build.inst);

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  logBuild(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
#endif
  build = Build();
  }

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
void RtScene::logBuild(double buildMs) const {
  // Size of the scene each rebuild: instances and unique BLAS in the TLAS, triangles summed over
  // the instances; bytes are -1 when the backend keeps no size (Metal).
  std::unordered_set<const AccelerationStructure*> unique;
  int64_t blasBytes = 0;
  for(auto& i:build.inst) {
    if(i.blas==nullptr || !unique.insert(i.blas).second)
      continue;
    const int64_t bytes = profileByteSize(*i.blas);
    blasBytes = (bytes<0 || blasBytes<0) ? -1 : blasBytes+bytes;
    }
  char line[256] = {};
  std::snprintf(line, sizeof(line),
                "render: rt scene instances=%zu blas=%zu triangles=%" PRIu64 " tlas_bytes=%" PRId64
                " blas_bytes=%" PRId64 " build_ms=%.3f",
                build.inst.size(), unique.size(), build.triangles, profileByteSize(tlas), blasBytes, buildMs);
  Log::i(line);
  }
#endif

bool RtScene::refreshTextures() {
  const auto generation=Resources::textureGeneration();
  if(textureGeneration==generation)
    return false;
  Resources::recycle(std::move(tex));
  tex=Resources::device().descriptors(textureIdentities);
  textureGeneration=generation;
  return true;
  }

