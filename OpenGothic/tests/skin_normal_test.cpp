// Skinned normals that follow the bones (graphics/mesh/submesh/packedmesh): a synthetic soft skin with known rest
// frames - the rest rotation of each node recovered from the skin itself, the normal stored in each bone's rest
// frame as octahedral snorm16x2 (decoded as the shader does), blended vertices whose model position is the blend of
// their nodes' rest positions (our MDM writer) not spoiling the fit, and a node that fits no rigid frame keeping the
// rest normal. The random normals cover both hemispheres, so the octahedral fold is checked too. CPU only: no
// device, no game data.
#include "graphics/mesh/submesh/packedmesh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

struct M3 {
  float m[3][3];
  asset::Vec3 operator*(const asset::Vec3& v) const {
    return {m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z,
            m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z,
            m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z};
    }
  asset::Vec3 transposed(const asset::Vec3& v) const {
    return {m[0][0]*v.x + m[1][0]*v.y + m[2][0]*v.z,
            m[0][1]*v.x + m[1][1]*v.y + m[2][1]*v.z,
            m[0][2]*v.x + m[1][2]*v.y + m[2][2]*v.z};
    }
  };

M3 axisAngle(float x, float y, float z, float degrees) {
  const float len = std::sqrt(x*x + y*y + z*z);
  x /= len; y /= len; z /= len;
  const float a = degrees*3.14159265f/180.f, c = std::cos(a), s = std::sin(a), t = 1.f - c;
  return {{{t*x*x + c,   t*x*y - s*z, t*x*z + s*y},
           {t*x*y + s*z, t*y*y + c,   t*y*z - s*x},
           {t*x*z - s*y, t*y*z + s*x, t*z*z + c}}};
  }

asset::Vec3 add(asset::Vec3 a, asset::Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
asset::Vec3 scale(asset::Vec3 a, float k) { return {a.x*k, a.y*k, a.z*k}; }

// vertex_process.glsl, octNormal
asset::Vec3 octNormal(uint32_t bits) {
  const float ex = std::max(float(int16_t(bits & 0xFFFFu))/32767.f, -1.f);
  const float ey = std::max(float(int16_t(bits >> 16))/32767.f, -1.f);
  float x = ex, y = ey;
  const float z = 1.f - std::abs(ex) - std::abs(ey);
  if(z<0.f) {
    x = (1.f - std::abs(ey))*(ex>=0.f ? 1.f : -1.f);
    y = (1.f - std::abs(ex))*(ey>=0.f ? 1.f : -1.f);
    }
  const float len = std::sqrt(x*x + y*y + z*z);
  return {x/len, y/len, z/len};
  }

bool close(asset::Vec3 a, asset::Vec3 b, float eps) {
  return std::abs(a.x-b.x)<=eps && std::abs(a.y-b.y)<=eps && std::abs(a.z-b.z)<=eps;
  }

using Key = std::tuple<float,float,float>;
}

int main() {
  try {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    auto randomUnit = [&]() {
      for(;;) {
        asset::Vec3 v{u(rng), u(rng), u(rng)};
        const float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if(len>0.2f && len<1.f)
          return scale(v, 1.f/len);
        }
      };

    // rest frames (node -> model) of two good nodes; node 2 carries points that fit no rigid frame
    const M3 R[2] = {axisAngle(1, 2, 3, 40.f), axisAngle(0, 1, 0.3f, -110.f)};
    const asset::Vec3 T[2] = {{5, 100, -3}, {-10, 50, 20}};

    asset::SoftSkinMesh skin;
    auto& mesh = skin.mesh;
    std::vector<asset::Vec3> normals;      // model-space normal per vertex
    std::map<Key,size_t> byLocal;          // the first local position of a vertex -> its index
    std::vector<int> owner;                // node of a rigid vertex, -1 for a blended one, 2 for the bad node

    auto addVertex = [&](std::vector<asset::SkinWeight> weights, asset::Vec3 model) {
      byLocal[{weights[0].position.x, weights[0].position.y, weights[0].position.z}] = mesh.positions.size();
      mesh.positions.push_back(model);
      skin.weights.push_back(std::move(weights));
      normals.push_back(randomUnit());
      };
    for(int node=0; node<2; ++node)
      for(int i=0; i<12; ++i) {
        const asset::Vec3 p = scale(asset::Vec3{u(rng), u(rng), u(rng)}, 20.f);
        addVertex({{1.f, p, uint8_t(node)}}, add(R[node]*p, T[node]));
        owner.push_back(node);
        }
    for(int i=0; i<6; ++i) {
      // the writer's way: a local position per node from a posed skeleton, the model position the blend of the
      // rest positions - each node's rest transform alone does not give it
      const asset::Vec3 p0 = scale(asset::Vec3{u(rng), u(rng), u(rng)}, 20.f);
      const asset::Vec3 p1 = scale(asset::Vec3{u(rng), u(rng), u(rng)}, 20.f);
      const asset::Vec3 model = add(scale(add(R[0]*p0, T[0]), 0.5f), scale(add(R[1]*p1, T[1]), 0.5f));
      addVertex({{0.5f, p0, uint8_t(0)}, {0.5f, p1, uint8_t(1)}}, model);
      owner.push_back(-1);
      }
    for(int i=0; i<9; ++i) {
      const asset::Vec3 p = scale(asset::Vec3{u(rng), u(rng), u(rng)}, 20.f);
      const asset::Vec3 q = scale(asset::Vec3{u(rng), u(rng), u(rng)}, 20.f);   // unrelated to p
      addVertex({{1.f, p, uint8_t(2)}}, q);
      owner.push_back(2);
      }

    asset::SubMesh sub;
    sub.mat.name = "SKIN_TEST";
    sub.mat.texture = "SKIN_TEST.TGA";
    for(size_t i=0; i<mesh.positions.size(); ++i) {
      asset::MeshWedge w;
      w.normal  = normals[i];
      w.texture = {0.f, 0.f};
      w.index   = uint16_t(i);
      sub.wedges.push_back(w);
      }
    for(size_t i=0; i+2<mesh.positions.size(); i+=3)
      sub.triangles.push_back({{uint16_t(i), uint16_t(i+1), uint16_t(i+2)}});
    mesh.materials.push_back(sub.mat);
    mesh.sub_meshes.push_back(sub);
    mesh.bbox = {{-100, -100, -100}, {100, 200, 100}};

    PackedMesh packed(skin);
    size_t posed = 0, legacy = 0;
    for(auto& vx:packed.verticesA) {
      if(vx.weights[0]<=0.f)
        continue;                                            // meshlet padding
      const auto it = byLocal.find({vx.pos[0][0], vx.pos[0][1], vx.pos[0][2]});
      require(it!=byLocal.end(), "a packed vertex has no source");
      const size_t src = it->second;
      const asset::Vec3 n = normals[src];
      if(owner[src]==2) {
        require(vx.color==0xFFFFFFFFu, "a vertex of the node with no rigid frame must keep the rest normal");
        ++legacy;
        continue;
        }
      require(vx.color==0u, "a vertex of fitted nodes must carry its normal in the bones' frames");
      for(int r=0; r<4; ++r) {
        if(vx.weights[r]<=0.f)
          continue;
        const int node = vx.boneId[r];
        require(node==0 || node==1, "unexpected bone");
        const asset::Vec3 expect = R[node].transposed(n);
        const asset::Vec3 got    = octNormal(vx.normalLocal[r]);
        require(close(got, expect, 2e-3f), "the normal in bone " + std::to_string(node) + "'s frame is wrong");
        // turned back by the rest frame it is the model normal again: the shader's normal at the rest pose
        require(close(R[node]*got, n, 2e-3f), "the rest pose does not give the model normal back");
        }
      ++posed;
      }
    require(posed==30, "every vertex of the two fitted nodes is packed, got " + std::to_string(posed));
    require(legacy==9, "every vertex of the bad node is packed, got " + std::to_string(legacy));

    std::cout << "skin normals: " << assertions << " assertions passed\n";
    return 0;
    }
  catch(const std::exception& e) {
    std::cerr << "skin normals: FAILED after " << assertions << " assertions: " << e.what() << "\n";
    return 1;
    }
  }
