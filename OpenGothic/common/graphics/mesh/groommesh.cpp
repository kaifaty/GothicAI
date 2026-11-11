#include "graphics/mesh/submesh/packedmesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

// Strands of a head as ribbons (reports/content/2026-11-npc-hair/strands.md). The file of
// scripts/content/npc/hair/groom_export.py holds points; here every point becomes two vertices at the same place,
// one for each edge of the ribbon, and the vertex shader moves them apart across the strand towards the eye
// (strandExpand, vertex_process.glsl). What a vertex carries:
//   pos    the point, cm, the head's frame
//   norm   the strand's direction, root to tip
//   uv     x: half of the hair's width in cm, its sign is the edge; y: the strand's random number, 0..1
//   color  the normal of the hair's surface (octahedral, two bytes), the light that reaches the point through the
//          hair over it, the strand's tint
// A strand never crosses a meshlet: as many whole strands as fit into 64 vertices go into one.

namespace {

struct Point {
  float    pos[3];
  float    tan[3];
  float    halfWidth;
  uint32_t bits;
  };

template<class T>
void read(std::ifstream& in, T* dst, size_t count) {
  in.read(reinterpret_cast<char*>(dst),std::streamsize(sizeof(T)*count));
  if(!in)
    throw std::runtime_error("unexpected end of the groom file");
  }

}

PackedMesh::PackedMesh(const std::filesystem::path& groom, float share) {
  std::ifstream in(groom,std::ios::binary);
  if(!in)
    throw std::runtime_error("unable to open the groom file");
  char     magic[4] = {};
  uint32_t groups   = 0;
  char     name[65] = {};
  read(in,magic,4);
  read(in,&groups,1);
  read(in,name,64);
  if(std::memcmp(magic,"GRM1",4)!=0 || groups==0 || groups>64)
    throw std::runtime_error("not a GRM1 groom file");
  share = std::clamp(share,0.01f,1.f);

  mBbox[0] = Tempest::Vec3( 1e9f, 1e9f, 1e9f);
  mBbox[1] = Tempest::Vec3(-1e9f,-1e9f,-1e9f);
  std::vector<Point>   points;
  std::vector<uint8_t> ids;
  for(uint32_t g=0; g<groups; ++g) {
    uint32_t k = 0, strands = 0;
    read(in,&k,1);
    read(in,&strands,1);
    if(k<2 || k*2>MaxVert || strands>(1u<<22))
      throw std::runtime_error("a strand must have 2..32 points");
    points.resize(size_t(k)*strands);
    ids.resize((size_t(strands)+3)/4*4);
    read(in,points.data(),points.size());
    read(in,ids.data(),ids.size());

    // the first share of a group is that share of the hair; what is left is wider, so it covers as much
    const uint32_t kept     = std::max<uint32_t>(1,uint32_t(std::ceil(float(strands)*share)));
    const float    wider    = float(strands)/float(kept);
    const uint32_t perMesh  = MaxVert/(k*2);
    const uint32_t strandTr = (k-1)*2;
    for(uint32_t first=0; first<kept; first+=perMesh) {
      const uint32_t count  = std::min(perMesh,kept-first);
      const size_t   vboSz  = vertices.size();
      const size_t   iboSz  = indices.size();
      const size_t   iboSz8 = indices8.size();
      vertices.resize(vboSz+MaxVert);
      indices .resize(iboSz+MaxInd);
      indices8.resize(iboSz8+MaxPrim*4);
      uint32_t prim = 0;
      for(uint32_t s=0; s<count; ++s) {
        const Point* pt = points.data()+size_t(first+s)*k;
        const float  id = float(ids[first+s])/255.f;
        for(uint32_t j=0; j<k; ++j)
          for(uint32_t side=0; side<2; ++side) {
            Vertex& v = vertices[vboSz+(s*k+j)*2+side];
            std::memcpy(v.pos, pt[j].pos,sizeof(v.pos));
            std::memcpy(v.norm,pt[j].tan,sizeof(v.norm));
            v.uv[0] = pt[j].halfWidth*wider*(side==0 ? -1.f : 1.f);
            v.uv[1] = id;
            v.color = pt[j].bits;
            }
        for(uint32_t j=0; j<k; ++j) {
          mBbox[0] = Tempest::Vec3(std::min(mBbox[0].x,pt[j].pos[0]),std::min(mBbox[0].y,pt[j].pos[1]),std::min(mBbox[0].z,pt[j].pos[2]));
          mBbox[1] = Tempest::Vec3(std::max(mBbox[1].x,pt[j].pos[0]),std::max(mBbox[1].y,pt[j].pos[1]),std::max(mBbox[1].z,pt[j].pos[2]));
          }
        for(uint32_t j=0; j+1<k; ++j) {
          const uint8_t a = uint8_t((s*k+j)*2);
          const uint8_t quad[2][3] = {{a,uint8_t(a+2),uint8_t(a+1)}, {uint8_t(a+1),uint8_t(a+2),uint8_t(a+3)}};
          for(const auto& tri:quad) {
            for(uint32_t c=0; c<3; ++c) {
              indices [iboSz +prim*3+c] = uint32_t(vboSz)+tri[c];
              indices8[iboSz8+prim*4+c] = tri[c];
              }
            ++prim;
            }
          }
        }
      // as Meshlet::flush: the rest are degenerate triangles, the last one holds the counts
      for(size_t i=size_t(prim)*3; i<MaxInd; ++i)
        indices[iboSz+i] = uint32_t(vboSz+prim);
      const size_t last = iboSz8+MaxPrim*4-4;
      indices8[last+0] = 0;
      indices8[last+1] = 0;
      indices8[last+2] = uint8_t(count*strandTr);
      indices8[last+3] = uint8_t(count*k*2);
      }
    }
  if(vertices.empty())
    throw std::runtime_error("the groom file holds no strands");

  isUsingAlphaTest = true;
  SubMesh sub;
  sub.material.name     = name;
  sub.material.texture  = name;
  sub.iboOffset         = 0;
  sub.iboLength         = indices.size();
  sub.alphaTest         = true;
  sub.alphaTestRequired = true; // the cover of a ribbon is cut by the hashed alpha test, as our glTF MASK is
  subMeshes.push_back(std::move(sub));
  }
