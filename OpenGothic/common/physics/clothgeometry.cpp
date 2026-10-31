#include "clothgeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {
  using namespace ClothGeometry;
  using Edge = std::pair<uint32_t,uint32_t>;

  Edge edge(uint32_t a, uint32_t b) {
    return {std::min(a,b),std::max(a,b)};
    }

  size_t uniquePositions(std::span<const Vertex> vertices) {
    std::set<std::tuple<int64_t,int64_t,int64_t>> positions;
    for(const auto& v:vertices)
      positions.emplace(std::llround(double(v.pos[0])*100.0),
                        std::llround(double(v.pos[1])*100.0),
                        std::llround(double(v.pos[2])*100.0));
    return positions.size();
    }

  float maxEdge(std::span<const Vertex> vertices, std::span<const uint32_t> indices) {
    double longestSquared = 0;
    for(size_t at=0;at<indices.size();at+=3)
      for(size_t side=0;side<3;++side) {
        const auto& a = vertices[indices[at+side]];
        const auto& b = vertices[indices[at+(side+1)%3]];
        double lengthSquared = 0;
        for(size_t k=0;k<3;++k) {
          const double delta = double(a.pos[k])-double(b.pos[k]);
          lengthSquared += delta*delta;
          }
        longestSquared = std::max(longestSquared,lengthSquared);
        }
    return float(std::sqrt(longestSquared));
    }

  SourceWeights midpointWeights(const SourceWeights& a, const SourceWeights& b) {
    std::map<uint32_t,float> weights;
    for(const auto* source:{&a,&b})
      for(size_t k=0;k<3;++k)
        if(source->weights[k]>0.f)
          weights[source->vertices[k]] += source->weights[k]*.5f;
    // An edge always belongs to one original triangle, even after refinement.
    if(weights.size()>3)
      throw std::logic_error("cloth subdivision lost its source triangle");
    SourceWeights out;
    size_t at = 0;
    for(const auto& [vertex,weight]:weights) {
      out.vertices[at] = vertex;
      out.weights[at++] = weight;
      }
    return out;
    }

  Vertex interpolate(std::span<const Vertex> original, const SourceWeights& source) {
    Vertex out;
    double colors[4] = {};
    for(size_t at=0;at<3;++at) {
      const auto weight = source.weights[at];
      if(weight<=0.f)
        continue;
      const auto& v = original[source.vertices[at]];
      for(size_t k=0;k<3;++k) {
        out.pos[k] += v.pos[k]*weight;
        out.norm[k] += v.norm[k]*weight;
        }
      for(size_t k=0;k<2;++k)
        out.uv[k] += v.uv[k]*weight;
      for(size_t k=0;k<4;++k)
        colors[k] += double((v.color>>(k*8))&0xffu)*double(weight);
      }
    const auto length = std::sqrt(double(out.norm[0])*out.norm[0]+
                                  double(out.norm[1])*out.norm[1]+
                                  double(out.norm[2])*out.norm[2]);
    if(length>1.e-12)
      for(auto& value:out.norm)
        value = float(double(value)/length);
    out.color = 0;
    for(size_t k=0;k<4;++k)
      out.color |= uint32_t(std::clamp(std::lround(colors[k]),0l,255l))<<(k*8);
    return out;
    }

  bool pinned(const SourceWeights& source, const std::vector<bool>& originalPins,
              const std::set<Edge>& pinnedEdges) {
    uint32_t vertices[3] = {};
    size_t count = 0;
    for(size_t at=0;at<3;++at)
      if(source.weights[at]>0.f)
        vertices[count++] = source.vertices[at];
    if(count==1)
      return originalPins[vertices[0]];
    if(count==2)
      return pinnedEdges.count(edge(vertices[0],vertices[1]))!=0;
    // Even three fixed corners do not turn a triangle's interior into a pin.
    return false;
    }

  Mesh split(const Mesh& source, std::span<const Vertex> original,
             const std::vector<bool>& originalPins, const std::set<Edge>& pinnedEdges) {
    Mesh out;
    out.vertices = source.vertices;
    out.sourceWeights = source.sourceWeights;
    out.pins = source.pins;
    out.indices.reserve(source.indices.size()*4);
    std::map<Edge,uint32_t> midpoints;
    const auto midpoint = [&](uint32_t a, uint32_t b) {
      const auto key = edge(a,b);
      const auto old = midpoints.find(key);
      if(old!=midpoints.end())
        return old->second;
      if(out.vertices.size()>=std::numeric_limits<uint32_t>::max())
        throw std::length_error("cloth subdivision exceeds the index range");
      const auto id = uint32_t(out.vertices.size());
      const auto weights = midpointWeights(source.sourceWeights[a],source.sourceWeights[b]);
      out.vertices.push_back(interpolate(original,weights));
      out.sourceWeights.push_back(weights);
      if(pinned(weights,originalPins,pinnedEdges))
        out.pins.push_back(id);
      midpoints.emplace(key,id);
      return id;
      };
    for(size_t at=0;at<source.indices.size();at+=3) {
      const auto a=source.indices[at], b=source.indices[at+1], c=source.indices[at+2];
      const auto ab=midpoint(a,b), bc=midpoint(b,c), ca=midpoint(c,a);
      out.indices.insert(out.indices.end(),{a,ab,ca, ab,b,bc, ca,bc,c, ab,bc,ca});
      }
    out.uniquePositions = uniquePositions(out.vertices);
    out.maxEdgeCm = source.maxEdgeCm*.5f;
    out.subdivisionDepth = source.subdivisionDepth+1;
    return out;
    }
  }

ClothGeometry::Mesh ClothGeometry::subdivide(std::span<const Vertex> vertices,
                                            std::span<const uint32_t> indices,
                                            std::span<const uint32_t> pins,
                                            const Options& options) {
  if(vertices.empty() && indices.empty() && pins.empty())
    return {};
  if(vertices.empty() || indices.empty() || indices.size()%3!=0 ||
     vertices.size()>std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("cloth needs indexed triangles");
  if(!std::isfinite(options.targetEdgeCm) || options.targetEdgeCm<=0.f || options.maxParticles==0)
    throw std::invalid_argument("invalid cloth tessellation options");
  for(const auto& v:vertices) {
    for(const auto value:v.pos)
      if(!std::isfinite(value) || std::abs(double(value))>double(std::numeric_limits<int64_t>::max())/200.0)
        throw std::invalid_argument("cloth position is outside the finite weld range");
    for(const auto value:v.norm)
      if(!std::isfinite(value))
        throw std::invalid_argument("cloth normal is not finite");
    for(const auto value:v.uv)
      if(!std::isfinite(value))
        throw std::invalid_argument("cloth UV is not finite");
    }
  for(const auto id:indices)
    if(id>=vertices.size())
      throw std::invalid_argument("cloth triangle index is outside the vertex array");
  std::vector<bool> originalPins(vertices.size(),false);
  for(const auto id:pins) {
    if(id>=vertices.size())
      throw std::invalid_argument("cloth pin index is outside the vertex array");
    originalPins[id] = true;
    }
  std::set<Edge> pinnedEdges;
  for(size_t at=0;at<indices.size();at+=3)
    for(size_t side=0;side<3;++side) {
      const auto a = indices[at+side], b = indices[at+(side+1)%3];
      if(originalPins[a] && originalPins[b])
        pinnedEdges.insert(edge(a,b));
      }
  Mesh out;
  out.vertices.assign(vertices.begin(),vertices.end());
  out.indices.assign(indices.begin(),indices.end());
  for(uint32_t i=0;i<vertices.size();++i) {
    out.sourceWeights.push_back({{i,0,0},{1.f,0.f,0.f}});
    if(originalPins[i])
      out.pins.push_back(i);
    }
  out.uniquePositions = uniquePositions(vertices);
  out.maxEdgeCm = maxEdge(vertices,indices);
  const auto minParticles = std::min(options.minParticles,options.maxParticles);
  while(out.subdivisionDepth<options.maxDepth &&
        (out.maxEdgeCm>options.targetEdgeCm || out.uniquePositions<minParticles)) {
    auto next = split(out,vertices,originalPins,pinnedEdges);
    if(next.uniquePositions>options.maxParticles) {
      out.budgetLimited = true;
      break;
      }
    out = std::move(next);
    }
  if(out.uniquePositions>options.maxParticles)
    out.budgetLimited = true;
  return out;
  }
