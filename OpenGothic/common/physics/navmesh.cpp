#include "navmesh.h"

#include <Recast.h>
#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace {
using Clock = std::chrono::steady_clock;
using Point = NavMesh::Point;
constexpr uint32_t cacheVersion = 2;
constexpr uint64_t hashSeed = 14695981039346656037ull;
constexpr int maxPath = 32768;
constexpr unsigned short groundOnlyFlag = 2;

uint64_t hashBytes(uint64_t h, const void* data, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for(size_t i=0; i<size; ++i) {
    h ^= bytes[i];
    h *= 1099511628211ull;
    }
  return h;
  }

template<class T> uint64_t hashValue(uint64_t h, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  return hashBytes(h,&value,sizeof(value));
  }

template<class T> uint64_t hashVector(uint64_t h, const std::vector<T>& values) {
  h = hashValue(h,uint64_t(values.size()));
  return hashBytes(h,values.data(),values.size()*sizeof(T));
  }

uint64_t geometryHash(const NavMesh::Geometry& g, const NavMesh::Params& p, const std::string& world) {
  uint64_t h = hashValue(hashSeed,cacheVersion);
  h = hashBytes(h,world.data(),world.size());
  h = hashValue(h,uint32_t(sizeof(dtPolyRef)));
  h = hashVector(h,g.vertices);
  h = hashVector(h,g.triangles);
  h = hashVector(h,g.waterVertices);
  h = hashVector(h,g.waterTriangles);
  for(float value:{p.radius,p.height,p.climb,p.slope,p.cellSize,p.cellHeight,p.waterDepth,
                   p.nearestHorizontal,p.nearestVertical})
    h = hashValue(h,value);
  h = hashValue(h,p.tileSize);
  h = hashValue(h,uint64_t(p.regions.size()));
  for(const auto& region:p.regions) {
    for(float value:region.min) h = hashValue(h,value);
    for(float value:region.max) h = hashValue(h,value);
    }
  // Keep the stage 2a cache usable when there are no proven transitions.
  if(!p.links.empty()) {
    h = hashValue(h,uint64_t(p.links.size()));
    for(const auto& link:p.links) {
      for(float value:link.start) h = hashValue(h,value);
      for(float value:link.end) h = hashValue(h,value);
      h = hashValue(h,link.radius);
      h = hashValue(h,uint8_t(link.type));
      h = hashValue(h,uint8_t(link.bidirectional));
      h = hashValue(h,link.id);
      }
    }
  return h;
  }

float distance(const Point& a, const Point& b) {
  float d = 0;
  for(size_t i=0; i<3; ++i) d += (a[i]-b[i])*(a[i]-b[i]);
  return std::sqrt(d);
  }

double elapsed(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now()-start).count();
  }

class Context final : public rcContext {
  public:
    Context():rcContext(true){}
    std::string error;
  protected:
    void doLog(const rcLogCategory category, const char* msg, const int len) override {
      if(category==RC_LOG_ERROR) error.assign(msg,size_t(len));
      }
  };

void require(bool ok, const char* operation, const Context& context) {
  if(!ok) throw std::runtime_error(std::string("navmesh ")+operation+": "+context.error);
  }

template<class T> bool readValue(std::istream& in, T& value) {
  return bool(in.read(reinterpret_cast<char*>(&value),sizeof(value)));
  }

template<class T> void writeValue(std::ostream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value),sizeof(value));
  }

void validateGeometry(const std::vector<float>& vertices, const std::vector<int>& triangles) {
  if(vertices.size()%3!=0 || triangles.size()%3!=0 || vertices.size()/3>size_t(std::numeric_limits<int>::max()))
    throw std::runtime_error("navmesh invalid triangle arrays");
  for(float v:vertices)
    if(!std::isfinite(v)) throw std::runtime_error("navmesh non-finite vertex");
  for(int index:triangles)
    if(index<0 || size_t(index)>=vertices.size()/3) throw std::runtime_error("navmesh vertex index out of bounds");
  }

// Block submerged compact spans, including the lake/sea bottom. Merely tagging
// water surface triangles unwalkable would leave a second, walkable floor below.
void excludeDeepWater(rcHeightfield& field, const NavMesh::Geometry& geometry,
                      const std::vector<int>& triangles, float depth) {
  for(int triangle:triangles) {
    const float* a = &geometry.waterVertices[size_t(geometry.waterTriangles[size_t(triangle)*3+0])*3];
    const float* b = &geometry.waterVertices[size_t(geometry.waterTriangles[size_t(triangle)*3+1])*3];
    const float* c = &geometry.waterVertices[size_t(geometry.waterTriangles[size_t(triangle)*3+2])*3];
    const float determinant = (b[2]-c[2])*(a[0]-c[0])+(c[0]-b[0])*(a[2]-c[2]);
    if(std::abs(determinant)<1.e-8f) continue;
    const int minX = std::max(0,int(std::floor((std::min({a[0],b[0],c[0]})-field.bmin[0])/field.cs)));
    const int maxX = std::min(field.width-1,int(std::floor((std::max({a[0],b[0],c[0]})-field.bmin[0])/field.cs)));
    const int minZ = std::max(0,int(std::floor((std::min({a[2],b[2],c[2]})-field.bmin[2])/field.cs)));
    const int maxZ = std::min(field.height-1,int(std::floor((std::max({a[2],b[2],c[2]})-field.bmin[2])/field.cs)));
    for(int z=minZ; z<=maxZ; ++z) {
      const float pz = field.bmin[2]+(float(z)+0.5f)*field.cs;
      for(int x=minX; x<=maxX; ++x) {
        const float px = field.bmin[0]+(float(x)+0.5f)*field.cs;
        const float u = ((b[2]-c[2])*(px-c[0])+(c[0]-b[0])*(pz-c[2]))/determinant;
        const float v = ((c[2]-a[2])*(px-c[0])+(a[0]-c[0])*(pz-c[2]))/determinant;
        if(u < -0.0001f || v < -0.0001f || u+v>1.0001f) continue;
        const float waterY = u*a[1]+v*b[1]+(1.f-u-v)*c[1];
        for(rcSpan* span=field.spans[x+z*field.width]; span!=nullptr; span=span->next) {
          const float floorY = field.bmin[1]+float(span->smax)*field.ch;
          // A solid ceiling between the floor and water is a cave, not a lake.
          const float ceilingY = span->next==nullptr ? std::numeric_limits<float>::max() :
                                 field.bmin[1]+float(span->next->smin)*field.ch;
          if(waterY-floorY>depth && waterY<ceilingY+field.ch) span->area = RC_NULL_AREA;
          }
        }
      }
    }
  }
}

struct NavMesh::Impl {
  std::unique_ptr<dtNavMesh,decltype(&dtFreeNavMesh)> mesh{nullptr,dtFreeNavMesh};
  std::unique_ptr<dtNavMeshQuery,decltype(&dtFreeNavMeshQuery)> query{nullptr,dtFreeNavMeshQuery};
  Params params;
  Stats stats;
  std::unordered_map<dtPolyRef,uint64_t> groundComponents;
  std::unordered_map<dtPolyRef,Point> groundCenters;
  // One current support's ground distances only. Never retain one world-sized
  // table per visited support polygon in a long oracle campaign.
  mutable dtPolyRef rasterSupport = 0;
  mutable std::unordered_map<dtPolyRef,float> rasterGroundDistance;

  void initializeQuery() {
    groundComponents.clear();groundCenters.clear();
    rasterSupport=0;rasterGroundDistance.clear();
    std::vector<dtPolyRef> groundPolygons;
    // Cache v2 predates the ground-only query bit. Apply it in memory on both
    // freshly built and cached ground polygons without changing existing routes.
    for(int i=0; i<mesh->getMaxTiles(); ++i) {
      const auto* tile=static_cast<const dtNavMesh&>(*mesh).getTile(i);
      if(!tile || !tile->header) continue;
      for(int j=0; j<tile->header->polyCount; ++j)
        if(tile->polys[j].getType()==DT_POLYTYPE_GROUND) {
          const auto ref=mesh->getPolyRefBase(tile)|dtPolyRef(j);
          mesh->setPolyFlags(ref,tile->polys[j].flags|groundOnlyFlag);
          if(tile->polys[j].flags&1) {
            groundPolygons.push_back(ref);
            Point center{};
            const auto& poly=tile->polys[j];
            for(unsigned int vertex=0;vertex<poly.vertCount;++vertex)
              for(size_t axis=0;axis<3;++axis)
                center[axis]+=tile->verts[size_t(poly.verts[vertex])*3+axis]/float(poly.vertCount);
            groundCenters.emplace(ref,center);
            }
          }
      }
    query.reset(dtAllocNavMeshQuery());
    if(!query || dtStatusFailed(query->init(mesh.get(),65535)))
      throw std::runtime_error("navmesh cannot initialize Detour query");
    // Eager, ground-only BFS once per build/load. The minimum ground polygon
    // reference names its component deterministically, including after cache load.
    // Frame telemetry only reads this table; off-mesh connections are excluded.
    std::sort(groundPolygons.begin(),groundPolygons.end());
    groundComponents.reserve(groundPolygons.size());
    for(const auto root:groundPolygons) {
      if(groundComponents.find(root)!=groundComponents.end()) continue;
      std::vector<dtPolyRef> pending{root};groundComponents[root]=uint64_t(root);
      for(size_t at=0; at<pending.size(); ++at) {
        const dtMeshTile* tile=nullptr;const dtPoly* poly=nullptr;
        if(dtStatusFailed(mesh->getTileAndPolyByRef(pending[at],&tile,&poly))) continue;
        for(auto link=poly->firstLink; link!=DT_NULL_LINK; link=tile->links[link].next) {
          const auto ref=tile->links[link].ref;
          if(!ref || groundComponents.find(ref)!=groundComponents.end()) continue;
          const dtMeshTile* otherTile=nullptr;const dtPoly* other=nullptr;
          if(dtStatusSucceed(mesh->getTileAndPolyByRef(ref,&otherTile,&other)) &&
             other->getType()==DT_POLYTYPE_GROUND && (other->flags&1)) {
            groundComponents[ref]=uint64_t(root);pending.push_back(ref);
            }
          }
        }
      }
    }

  bool containsGround(dtPolyRef ref,const Point& requested,Point& surface) const {
    bool over=false;
    if(groundCenters.find(ref)==groundCenters.end() ||
       dtStatusFailed(query->closestPointOnPoly(ref,requested.data(),surface.data(),&over)) || !over ||
       std::hypot(surface[0]-requested[0],surface[2]-requested[2])>1.e-4f) return false;
    float height=0;
    if(dtStatusFailed(query->getPolyHeight(ref,requested.data(),&height)) || !std::isfinite(height)) return false;
    surface={requested[0],height,requested[2]};
    return true;
    }

  void supportDistances(dtPolyRef root) const {
    if(rasterSupport==root) return;
    rasterSupport=root;rasterGroundDistance.clear();
    using Entry=std::pair<float,dtPolyRef>;
    std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> pending;
    rasterGroundDistance.emplace(root,0.f);pending.push({0.f,root});
    while(!pending.empty()) {
      const auto item=pending.top();pending.pop();
      const auto found=rasterGroundDistance.find(item.second);
      if(found==rasterGroundDistance.end() || item.first!=found->second) continue;
      const dtMeshTile* tile=nullptr;const dtPoly* poly=nullptr;
      if(dtStatusFailed(mesh->getTileAndPolyByRef(item.second,&tile,&poly))) continue;
      const auto center=groundCenters.find(item.second);
      if(center==groundCenters.end()) continue;
      for(auto link=poly->firstLink;link!=DT_NULL_LINK;link=tile->links[link].next) {
        const auto next=tile->links[link].ref;
        const auto other=groundCenters.find(next);
        if(other==groundCenters.end()) continue; // Never cross off-mesh links.
        const float score=item.first+distance(center->second,other->second);
        const auto old=rasterGroundDistance.find(next);
        if(old!=rasterGroundDistance.end() && old->second<=score) continue;
        rasterGroundDistance[next]=score;pending.push({score,next});
        }
      }
    }

  bool exactGroundSegment(dtPolyRef from,const Point& a,dtPolyRef to,const Point& b) const {
    if(from==to) return true; // A covered segment of one convex ground polygon.
    dtQueryFilter filter;filter.setIncludeFlags(groundOnlyFlag);
    std::array<dtPolyRef,256> corridor{};
    float fraction=0;Point normal{};int count=0;
    const auto status=query->raycast(from,a.data(),b.data(),&filter,&fraction,normal.data(),
                                    corridor.data(),&count,int(corridor.size()));
    if(dtStatusFailed(status) || dtStatusDetail(status,DT_BUFFER_TOO_SMALL) || fraction<=1.f || count<=0) return false;
    const auto last=corridor[size_t(count-1)];
    if(last==to) return true;
    // An endpoint exactly on a tile/polygon edge may remain in the polygon
    // from which the ray approached it. Detour's point-in-poly is half-open,
    // so its reference need not equal the raster's selected neighbour. Accept
    // only a directly linked ground neighbour sharing that numerical endpoint
    // and the same height; never use the much larger support-snap tolerance.
    const dtMeshTile* tile=nullptr;const dtPoly* poly=nullptr;
    if(dtStatusFailed(mesh->getTileAndPolyByRef(last,&tile,&poly))) return false;
    bool linked=false;
    for(auto link=poly->firstLink;link!=DT_NULL_LINK;link=tile->links[link].next)
      linked|=tile->links[link].ref==to;
    if(!linked) return false;
    Point boundary{};bool over=false;
    if(dtStatusFailed(query->closestPointOnPoly(last,b.data(),boundary.data(),&over))) return false;
    return std::hypot(boundary[0]-b[0],boundary[2]-b[2])<=1.e-4f && std::abs(boundary[1]-b[1])<=1.e-3f;
    }

  bool load(uint64_t key) {
    const auto start = Clock::now();
    std::ifstream in(stats.cacheFile,std::ios::binary);
    std::array<char,8> magic{};
    uint32_t version=0, endian=0;
    uint64_t storedKey=0, tileCount=0;
    double buildSeconds=0;
    dtNavMeshParams meshParams{};
    if(!in.read(magic.data(),std::streamsize(magic.size())) || std::memcmp(magic.data(),"OGNAV001",8)!=0 ||
       !readValue(in,version) || version!=cacheVersion || !readValue(in,endian) || endian!=0x01020304u ||
       !readValue(in,storedKey) || storedKey!=key || !readValue(in,buildSeconds) ||
       !readValue(in,meshParams) || !readValue(in,tileCount)) return false;
    if(meshParams.maxTiles<=0 || meshParams.maxTiles>(1<<20) || meshParams.maxPolys<=0 ||
       meshParams.maxPolys>(1<<20) || tileCount>uint64_t(meshParams.maxTiles) ||
       !std::isfinite(meshParams.tileWidth) || meshParams.tileWidth<=0 ||
       !std::isfinite(meshParams.tileHeight) || meshParams.tileHeight<=0) return false;
    auto loaded = std::unique_ptr<dtNavMesh,decltype(&dtFreeNavMesh)>(dtAllocNavMesh(),dtFreeNavMesh);
    if(!loaded || dtStatusFailed(loaded->init(&meshParams))) return false;
    uint64_t polygons=0;
    for(uint64_t i=0; i<tileCount; ++i) {
      dtTileRef reference=0;
      uint32_t size=0;
      uint64_t checksum=0;
      if(!readValue(in,reference) || !readValue(in,size) || !readValue(in,checksum) ||
         size<sizeof(dtMeshHeader) || size>64u*1024u*1024u) return false;
      auto* data = static_cast<unsigned char*>(dtAlloc(size,DT_ALLOC_PERM));
      if(data==nullptr) return false;
      if(!in.read(reinterpret_cast<char*>(data),size) || hashBytes(hashSeed,data,size)!=checksum) {
        dtFree(data);
        return false;
        }
      const auto* header = reinterpret_cast<const dtMeshHeader*>(data);
      if(header->magic!=DT_NAVMESH_MAGIC || header->version!=DT_NAVMESH_VERSION || header->polyCount<0) {
        dtFree(data);
        return false;
        }
      const int polygonCount = header->polyCount;
      if(dtStatusFailed(loaded->addTile(data,int(size),DT_TILE_FREE_DATA,reference,nullptr))) {
        dtFree(data);
        return false;
        }
      polygons += uint64_t(polygonCount);
      }
    if(in.peek()!=std::char_traits<char>::eof()) return false;
    mesh = std::move(loaded);
    initializeQuery();
    stats.buildSeconds = buildSeconds;
    stats.loadSeconds = elapsed(start);
    stats.tiles = tileCount;
    stats.polygons = polygons;
    stats.complete = true;
    stats.fromCache = true;
    stats.cacheBytes = std::filesystem::file_size(stats.cacheFile);
    return true;
    }

  void save(uint64_t key) {
    std::filesystem::create_directories(stats.cacheFile.parent_path());
    const std::filesystem::path temporary = stats.cacheFile.string()+".tmp";
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
    if(!out) throw std::runtime_error("navmesh cannot open cache "+temporary.string());
    out.write("OGNAV001",8);
    writeValue(out,cacheVersion);
    writeValue(out,uint32_t(0x01020304u));
    writeValue(out,key);
    writeValue(out,stats.buildSeconds);
    writeValue(out,*mesh->getParams());
    writeValue(out,stats.tiles);
    for(int i=0; i<mesh->getMaxTiles(); ++i) {
      const dtMeshTile* tile = static_cast<const dtNavMesh&>(*mesh).getTile(i);
      if(tile==nullptr || tile->header==nullptr) continue;
      writeValue(out,mesh->getTileRef(tile));
      writeValue(out,uint32_t(tile->dataSize));
      writeValue(out,hashBytes(hashSeed,tile->data,size_t(tile->dataSize)));
      out.write(reinterpret_cast<const char*>(tile->data),tile->dataSize);
      }
    out.close();
    if(!out) throw std::runtime_error("navmesh cannot write cache "+temporary.string());
    std::filesystem::rename(temporary,stats.cacheFile);
    stats.cacheBytes = std::filesystem::file_size(stats.cacheFile);
    }

  void build(const Geometry& geometry) {
    const auto start = Clock::now();
    Context context;
    Point bmin{}, bmax{};
    rcCalcBounds(geometry.vertices.data(),int(geometry.vertices.size()/3),bmin.data(),bmax.data());
    const float tileWidth = params.cellSize*float(params.tileSize);
    const int width = std::max(1,int(std::ceil((bmax[0]-bmin[0])/tileWidth)));
    const int height = std::max(1,int(std::ceil((bmax[2]-bmin[2])/tileWidth)));
    if(uint64_t(width)*uint64_t(height)>(1<<20)) throw std::runtime_error("navmesh world has too many tiles");
    const size_t tileCount = size_t(width)*size_t(height);
    std::vector<bool> selected(tileCount,params.regions.empty());
    for(int z=0; z<height; ++z) {
      for(int x=0; x<width; ++x) {
        const float x0=bmin[0]+float(x)*tileWidth, z0=bmin[2]+float(z)*tileWidth;
        for(const auto& region:params.regions)
          if(x0<=region.max[0] && x0+tileWidth>=region.min[0] &&
             z0<=region.max[2] && z0+tileWidth>=region.min[2]) selected[size_t(x+z*width)] = true;
        }
      }
    int selectedCount=0;
    for(bool value:selected) if(value) ++selectedCount;
    stats.tilesConsidered = uint64_t(selectedCount);
    dtNavMeshParams meshParams{};
    std::copy(bmin.begin(),bmin.end(),meshParams.orig);
    meshParams.tileWidth = meshParams.tileHeight = tileWidth;
    meshParams.maxTiles = std::max(selectedCount,1);
    meshParams.maxPolys = 32768;
    mesh.reset(dtAllocNavMesh());
    if(!mesh || dtStatusFailed(mesh->init(&meshParams))) throw std::runtime_error("navmesh cannot initialize Detour mesh");

    rcConfig config{};
    config.cs = params.cellSize;
    config.ch = params.cellHeight;
    config.walkableSlopeAngle = params.slope;
    config.walkableHeight = int(std::ceil(params.height/config.ch));
    // Preserve exact centimetre guild steps at a voxel boundary (60cm / 10cm
    // must be six cells despite the float conversion from engine centimetres).
    config.walkableClimb = int(std::floor(params.climb/config.ch+1.e-4f));
    config.walkableRadius = int(std::ceil(params.radius/config.cs));
    config.maxEdgeLen = int(12.f/config.cs);
    config.maxSimplificationError = 1.3f;
    config.minRegionArea = 8*8;
    config.mergeRegionArea = 20*20;
    config.maxVertsPerPoly = 6;
    config.tileSize = params.tileSize;
    config.borderSize = config.walkableRadius+3;
    config.width = config.height = config.tileSize+config.borderSize*2;
    config.detailSampleDist = 6.f*config.cs;
    config.detailSampleMaxError = config.ch;
    const float border = float(config.borderSize)*config.cs;

    std::vector<float> linkVertices, linkRadii;
    std::vector<unsigned short> linkFlags;
    std::vector<unsigned char> linkAreas, linkDirections;
    std::vector<unsigned int> linkIds;
    for(const auto& link:params.links) {
      linkVertices.insert(linkVertices.end(),link.start.begin(),link.start.end());
      linkVertices.insert(linkVertices.end(),link.end.begin(),link.end.end());
      linkRadii.push_back(link.radius);
      linkFlags.push_back(1);
      linkAreas.push_back(uint8_t(link.type));
      linkDirections.push_back(link.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
      linkIds.push_back(link.id);
      }

    // Index input triangles once. Tile building never scans the complete world.
    std::vector<std::vector<int>> solidBins(tileCount), waterBins(tileCount);
    auto bin = [&](const std::vector<float>& vertices, const std::vector<int>& triangles,
                   std::vector<std::vector<int>>& bins) {
      for(size_t i=0; i<triangles.size()/3; ++i) {
        const float* a=&vertices[size_t(triangles[i*3+0])*3];
        const float* b=&vertices[size_t(triangles[i*3+1])*3];
        const float* c=&vertices[size_t(triangles[i*3+2])*3];
        const int x0=std::max(0,int(std::floor((std::min({a[0],b[0],c[0]})-bmin[0]-border)/tileWidth)));
        const int x1=std::min(width-1,int(std::floor((std::max({a[0],b[0],c[0]})-bmin[0]+border)/tileWidth)));
        const int z0=std::max(0,int(std::floor((std::min({a[2],b[2],c[2]})-bmin[2]-border)/tileWidth)));
        const int z1=std::min(height-1,int(std::floor((std::max({a[2],b[2],c[2]})-bmin[2]+border)/tileWidth)));
        for(int z=z0; z<=z1; ++z)
          for(int x=x0; x<=x1; ++x)
            if(selected[size_t(x+z*width)]) bins[size_t(x+z*width)].push_back(int(i));
        }
      };
    bin(geometry.vertices,geometry.triangles,solidBins);
    bin(geometry.waterVertices,geometry.waterTriangles,waterBins);
    std::vector<unsigned char> walkable(geometry.triangles.size()/3,RC_NULL_AREA);
    rcMarkWalkableTriangles(&context,config.walkableSlopeAngle,geometry.vertices.data(),
                           int(geometry.vertices.size()/3),geometry.triangles.data(),
                           int(geometry.triangles.size()/3),walkable.data());
    stats.complete = true;
    for(int z=0; z<height; ++z) {
      for(int x=0; x<width; ++x) {
        const size_t index = size_t(x+z*width);
        if(!selected[index] || solidBins[index].empty()) continue;
        if(elapsed(start)>double(params.maxBuildSeconds)) {
          stats.complete = false;
          break;
          }
        rcConfig tile = config;
        tile.bmin[0] = bmin[0]+float(x)*tileWidth-border;
        tile.bmin[1] = bmin[1];
        tile.bmin[2] = bmin[2]+float(z)*tileWidth-border;
        tile.bmax[0] = bmin[0]+float(x+1)*tileWidth+border;
        tile.bmax[1] = bmax[1]+params.height+config.ch;
        tile.bmax[2] = bmin[2]+float(z+1)*tileWidth+border;
        std::unique_ptr<rcHeightfield,decltype(&rcFreeHeightField)> field(rcAllocHeightfield(),rcFreeHeightField);
        std::unique_ptr<rcCompactHeightfield,decltype(&rcFreeCompactHeightfield)> compact(rcAllocCompactHeightfield(),rcFreeCompactHeightfield);
        std::unique_ptr<rcContourSet,decltype(&rcFreeContourSet)> contours(rcAllocContourSet(),rcFreeContourSet);
        std::unique_ptr<rcPolyMesh,decltype(&rcFreePolyMesh)> polys(rcAllocPolyMesh(),rcFreePolyMesh);
        std::unique_ptr<rcPolyMeshDetail,decltype(&rcFreePolyMeshDetail)> detail(rcAllocPolyMeshDetail(),rcFreePolyMeshDetail);
        if(!field || !compact || !contours || !polys || !detail) throw std::bad_alloc();
        require(rcCreateHeightfield(&context,*field,tile.width,tile.height,tile.bmin,tile.bmax,tile.cs,tile.ch),"heightfield",context);
        std::vector<int> triangles;
        std::vector<unsigned char> areas;
        triangles.reserve(solidBins[index].size()*3);
        areas.reserve(solidBins[index].size());
        for(int triangle:solidBins[index]) {
          for(size_t i=0; i<3; ++i) triangles.push_back(geometry.triangles[size_t(triangle)*3+i]);
          areas.push_back(walkable[size_t(triangle)]);
          }
        require(rcRasterizeTriangles(&context,geometry.vertices.data(),int(geometry.vertices.size()/3),triangles.data(),
                                     areas.data(),int(areas.size()),*field,tile.walkableClimb),"rasterize",context);
        rcFilterLowHangingWalkableObstacles(&context,tile.walkableClimb,*field);
        rcFilterLedgeSpans(&context,tile.walkableHeight,tile.walkableClimb,*field);
        rcFilterWalkableLowHeightSpans(&context,tile.walkableHeight,*field);
        excludeDeepWater(*field,geometry,waterBins[index],params.waterDepth);
        require(rcBuildCompactHeightfield(&context,tile.walkableHeight,tile.walkableClimb,*field,*compact),"compact",context);
        field.reset();
        require(rcErodeWalkableArea(&context,tile.walkableRadius,*compact),"erode",context);
        require(rcBuildDistanceField(&context,*compact),"distance field",context);
        require(rcBuildRegions(&context,*compact,tile.borderSize,tile.minRegionArea,tile.mergeRegionArea),"regions",context);
        require(rcBuildContours(&context,*compact,tile.maxSimplificationError,tile.maxEdgeLen,*contours),"contours",context);
        require(rcBuildPolyMesh(&context,*contours,tile.maxVertsPerPoly,*polys),"polymesh",context);
        if(polys->npolys==0) continue;
        require(rcBuildPolyMeshDetail(&context,*polys,*compact,tile.detailSampleDist,tile.detailSampleMaxError,*detail),"detail",context);
        for(int i=0; i<polys->npolys; ++i) {
          polys->areas[i] = 0;
          polys->flags[i] = 1;
          }
        dtNavMeshCreateParams create{};
        create.verts=polys->verts; create.vertCount=polys->nverts;
        create.polys=polys->polys; create.polyAreas=polys->areas; create.polyFlags=polys->flags;
        create.polyCount=polys->npolys; create.nvp=polys->nvp;
        create.detailMeshes=detail->meshes; create.detailVerts=detail->verts;
        create.detailVertsCount=detail->nverts; create.detailTris=detail->tris;
        create.detailTriCount=detail->ntris;
        create.offMeshConVerts=linkVertices.data(); create.offMeshConRad=linkRadii.data();
        create.offMeshConFlags=linkFlags.data(); create.offMeshConAreas=linkAreas.data();
        create.offMeshConDir=linkDirections.data(); create.offMeshConUserID=linkIds.data();
        create.offMeshConCount=int(params.links.size());
        create.walkableHeight=params.height; create.walkableRadius=params.radius; create.walkableClimb=params.climb;
        create.tileX=x; create.tileY=z;
        rcVcopy(create.bmin,polys->bmin); rcVcopy(create.bmax,polys->bmax);
        create.cs=tile.cs; create.ch=tile.ch; create.buildBvTree=true;
        unsigned char* data=nullptr;
        int dataSize=0;
        if(!dtCreateNavMeshData(&create,&data,&dataSize)) throw std::runtime_error("navmesh cannot create Detour tile");
        if(dtStatusFailed(mesh->addTile(data,dataSize,DT_TILE_FREE_DATA,0,nullptr))) {
          dtFree(data);
          throw std::runtime_error("navmesh cannot add Detour tile");
          }
        ++stats.tiles;
        stats.polygons += uint64_t(reinterpret_cast<const dtMeshHeader*>(data)->polyCount);
        }
      if(!stats.complete) break;
      }
    stats.buildSeconds = elapsed(start);
    initializeQuery();
    }
  };

NavMesh::NavMesh():impl(std::make_unique<Impl>()){}

uint64_t NavMesh::cacheKey(const Geometry& geometry, const Params& params, const std::string& worldName) {
  return geometryHash(geometry,params,worldName);
  }
NavMesh::~NavMesh() = default;
NavMesh::NavMesh(NavMesh&&) noexcept = default;
NavMesh& NavMesh::operator=(NavMesh&&) noexcept = default;

void NavMesh::buildOrLoad(const Geometry& geometry, const Params& parameters,
                          const std::filesystem::path& cacheDirectory, const std::string& worldName) {
  validateGeometry(geometry.vertices,geometry.triangles);
  validateGeometry(geometry.waterVertices,geometry.waterTriangles);
  if(geometry.triangles.empty()) throw std::runtime_error("navmesh empty collision world");
  if(!(parameters.radius>0.f && parameters.height>0.f && parameters.climb>=0.f &&
       parameters.cellSize>0.f && parameters.cellHeight>0.f && parameters.waterDepth>=0.f &&
       parameters.slope>0.f && parameters.slope<90.f && parameters.tileSize>=16 &&
       parameters.tileSize<=1024 && parameters.maxBuildSeconds>0.f &&
       parameters.nearestHorizontal>0.f && parameters.nearestVertical>0.f))
    throw std::runtime_error("navmesh invalid agent/build parameters");
  if(parameters.links.size()>size_t(std::numeric_limits<int>::max()))
    throw std::runtime_error("navmesh too many off-mesh links");
  for(const auto& link:parameters.links) {
    for(float value:link.start)
      if(!std::isfinite(value)) throw std::runtime_error("navmesh non-finite link start");
    for(float value:link.end)
      if(!std::isfinite(value)) throw std::runtime_error("navmesh non-finite link end");
    if(!std::isfinite(link.radius) || link.radius<=0.f ||
       (link.type!=LinkType::Jump && link.type!=LinkType::Climb && link.type!=LinkType::Drop))
      throw std::runtime_error("navmesh invalid off-mesh link");
    }
  impl = std::make_unique<Impl>();
  impl->params = parameters;
  impl->stats.triangles = uint64_t(geometry.triangles.size()/3);
  impl->stats.waterTriangles = uint64_t(geometry.waterTriangles.size()/3);
  const auto hashStart = Clock::now();
  const uint64_t hash = cacheKey(geometry,parameters,worldName);
  const double hashSeconds = elapsed(hashStart);
  std::ostringstream key;
  key << std::hex << std::setfill('0') << std::setw(16) << hash;
  impl->stats.key = key.str();
  std::string safeName;
  for(char c:worldName)
    safeName += ((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-') ? c : '_';
  impl->stats.cacheFile = cacheDirectory/(safeName+"-"+impl->stats.key+".navmesh");
  if(impl->load(hash)) {
    impl->stats.loadSeconds += hashSeconds;
    return;
    }
  impl->build(geometry);
  impl->stats.buildSeconds += hashSeconds;
  if(impl->stats.complete) impl->save(hash);
  }

NavMesh::Path NavMesh::findPath(const Point& start, const Point& end, bool allowTransitions) const {
  Path result;
  if(!impl->query) { result.reason="mesh_not_built"; return result; }
  for(float value:start) if(!std::isfinite(value)) { result.reason="invalid_start"; return result; }
  for(float value:end) if(!std::isfinite(value)) { result.reason="invalid_end"; return result; }
  const float extents[3]={impl->params.nearestHorizontal,impl->params.nearestVertical,impl->params.nearestHorizontal};
  dtQueryFilter filter;
  filter.setIncludeFlags(allowTransitions?1:groundOnlyFlag);
  dtPolyRef startRef=0, endRef=0;
  Point snappedStart{}, snappedEnd{};
  if(dtStatusFailed(impl->query->findNearestPoly(start.data(),extents,&filter,&startRef,snappedStart.data())) || !startRef) {
    result.reason="start_off_mesh"; return result;
    }
  result.startSnapDistance=distance(start,snappedStart);
  if(dtStatusFailed(impl->query->findNearestPoly(end.data(),extents,&filter,&endRef,snappedEnd.data())) || !endRef) {
    result.reason="end_off_mesh"; return result;
    }
  result.endSnapDistance=distance(end,snappedEnd);
  std::vector<dtPolyRef> corridor(maxPath);
  int count=0;
  const dtStatus status=impl->query->findPath(startRef,endRef,snappedStart.data(),snappedEnd.data(),&filter,
                                            corridor.data(),&count,maxPath);
  if(dtStatusFailed(status) || dtStatusDetail(status,DT_OUT_OF_NODES) || dtStatusDetail(status,DT_BUFFER_TOO_SMALL)) {
    result.reason="query_capacity"; return result;
    }
  if(count==0 || corridor[size_t(count-1)]!=endRef || dtStatusDetail(status,DT_PARTIAL_RESULT)) {
    result.reason="disconnected"; return result;
    }
  std::vector<float> points(size_t(maxPath)*3);
  std::vector<unsigned char> flags(maxPath);
  // String-pull ground legs separately so an off-mesh connection can never be
  // mistaken for ordinary walking, even when its two endpoints are collinear.
  auto appendGroundLeg = [&](const Point& a, const Point& b, int begin, int length) {
    if(length<=0) return false;
    int pointCount=0;
    const dtStatus straight=impl->query->findStraightPath(a.data(),b.data(),corridor.data()+begin,length,
                                                         points.data(),flags.data(),nullptr,&pointCount,maxPath);
    if(dtStatusFailed(straight) || dtStatusDetail(straight,DT_BUFFER_TOO_SMALL) || pointCount==0 ||
       !(flags[size_t(pointCount-1)]&DT_STRAIGHTPATH_END)) return false;
    for(int i=0; i<pointCount; ++i) {
      const Point p={points[size_t(i)*3],points[size_t(i)*3+1],points[size_t(i)*3+2]};
      if(result.corners.empty() || distance(result.corners.back(),p)>1.e-5f) result.corners.push_back(p);
      }
    return true;
    };
  int legBegin=0;
  Point legStart=snappedStart;
  for(int i=1; i<count; ++i) {
    const auto* connection=impl->mesh->getOffMeshConnectionByRef(corridor[size_t(i)]);
    if(connection==nullptr) continue;
    Path::Transition transition;
    const dtMeshTile* tile=nullptr;
    const dtPoly* poly=nullptr;
    if(dtStatusFailed(impl->mesh->getTileAndPolyByRef(corridor[size_t(i)],&tile,&poly)) ||
       dtStatusFailed(impl->mesh->getOffMeshConnectionPolyEndPoints(corridor[size_t(i-1)],corridor[size_t(i)],
                                                                  transition.start.data(),transition.end.data())) ||
       !appendGroundLeg(legStart,transition.start,legBegin,i-legBegin)) {
      result.reason="string_pull_failed"; return result;
      }
    transition.type=LinkType(poly->getArea());
    transition.id=connection->userId;
    transition.cornerIndex=result.corners.size()-1;
    result.corners.back()=transition.start;
    // Retain even a coincident endpoint: cornerIndex+1 always means landing.
    result.corners.push_back(transition.end);
    result.transitions.push_back(transition);
    legStart=transition.end;
    legBegin=i+1;
    }
  if(!appendGroundLeg(legStart,snappedEnd,legBegin,count-legBegin)) {
    result.reason="string_pull_failed"; return result;
    }
  for(size_t i=1; i<result.corners.size(); ++i) result.length += distance(result.corners[i-1],result.corners[i]);
  result.complete=true;
  result.reason="ok";
  return result;
  }

NavMesh::LocalRaster NavMesh::rasterLocal(const Point& feet,float bodyYawDegrees) const {
  for(float value:feet) if(!std::isfinite(value)) throw std::invalid_argument("navmesh raster non-finite feet");
  if(!std::isfinite(bodyYawDegrees)) throw std::invalid_argument("navmesh raster non-finite yaw");
  LocalRaster result;
  result.near.size=64;result.near.cellSize=.25f;result.near.cells.resize(64*64);
  result.far.size=100;result.far.cellSize=2.f;result.far.cells.resize(100*100);
  if(!impl->query) return result;
  const float radians=bodyYawDegrees*(3.14159265358979323846f/180.f);
  const float cosine=std::cos(radians),sine=std::sin(radians);
  const auto world=[&](float right,float forward) -> Point {
    return {feet[0]+forward*cosine-right*sine,feet[1],feet[2]+forward*sine+right*cosine};
    };
  const auto local=[&](const float* point) -> std::array<float,2> {
    const float x=point[0]-feet[0],z=point[2]-feet[2];
    return {-x*sine+z*cosine,x*cosine+z*sine};
    };
  // One broad-phase query, then polygon bounding-box rasterization. No
  // per-cell nearest-poly queries and no horizontal snapping across cracks.
  const float extents[3]={142.f,100000.f,142.f};
  dtQueryFilter filter;filter.setIncludeFlags(groundOnlyFlag);
  std::vector<dtPolyRef> polygons(1024);int count=0;
  for(;;) {
    const auto status=impl->query->queryPolygons(feet.data(),extents,&filter,polygons.data(),&count,int(polygons.size()));
    if(dtStatusFailed(status)) throw std::runtime_error("navmesh raster polygon query failed");
    if(!dtStatusDetail(status,DT_BUFFER_TOO_SMALL)) break;
    if(polygons.size()>=(1u<<20)) throw std::runtime_error("navmesh raster polygon query capacity");
    polygons.resize(polygons.size()*2);
    }
  polygons.resize(size_t(count));std::sort(polygons.begin(),polygons.end());
  dtPolyRef support=0;float supportError=std::numeric_limits<float>::infinity();
  for(const auto ref:polygons) {
    if(impl->groundCenters.find(ref)==impl->groundCenters.end()) continue;
    Point surface{};bool over=false;
    if(dtStatusFailed(impl->query->closestPointOnPoly(ref,feet.data(),surface.data(),&over))) continue;
    const float horizontal=std::hypot(surface[0]-feet[0],surface[2]-feet[2]);
    const float vertical=surface[1]-feet[1];
    // Only support selection may snap. The raster below still demands exact
    // XZ containment for every native cell, including the body and real gaps.
    if(!std::isfinite(horizontal) || !std::isfinite(vertical) || horizontal>impl->params.nearestHorizontal ||
       std::abs(vertical)>impl->params.climb+2.f*impl->params.cellHeight) continue;
    const float error=horizontal*horizontal+vertical*vertical;
    if(error<supportError || (error==supportError && ref<support)) {
      support=ref;supportError=error;result.support=surface;
      result.supportExact=over && horizontal<=1.e-4f;
      result.supportSnapDistance=std::sqrt(error);result.supportHorizontalDistance=horizontal;
      result.supportVerticalOffset=vertical;
      }
    }
  if(!support) return result;
  result.supportValid=true;result.supportPolygon=uint64_t(support);
  impl->supportDistances(support);
  const auto fill=[&](RasterLayer& layer) {
    const int size=int(layer.size);const float step=layer.cellSize;
    std::vector<float> ranks(layer.cells.size(),std::numeric_limits<float>::infinity());
    for(const auto ref:polygons) {
      const dtMeshTile* tile=nullptr;const dtPoly* poly=nullptr;
      if(impl->groundCenters.find(ref)==impl->groundCenters.end() ||
         dtStatusFailed(impl->mesh->getTileAndPolyByRef(ref,&tile,&poly))) continue;
      float minRight=std::numeric_limits<float>::infinity(),maxRight=-minRight;
      float minForward=minRight,maxForward=-minRight;
      for(unsigned int vertex=0;vertex<poly->vertCount;++vertex) {
        const auto point=local(&tile->verts[size_t(poly->verts[vertex])*3]);
        minRight=std::min(minRight,point[0]);maxRight=std::max(maxRight,point[0]);
        minForward=std::min(minForward,point[1]);maxForward=std::max(maxForward,point[1]);
        }
      const int x0=std::max(0,int(std::ceil(minRight/step+float(size)*.5f-.5f)));
      const int x1=std::min(size-1,int(std::floor(maxRight/step+float(size)*.5f-.5f)));
      const int z0=std::max(0,int(std::ceil(minForward/step+float(size)*.5f-.5f)));
      const int z1=std::min(size-1,int(std::floor(maxForward/step+float(size)*.5f-.5f)));
      const auto reachable=impl->rasterGroundDistance.find(ref);
      const bool connected=reachable!=impl->rasterGroundDistance.end();
      const float rank=connected ? reachable->second : std::numeric_limits<float>::infinity();
      for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x) {
        const size_t index=size_t(z)*layer.size+size_t(x);
        const Point requested=world((float(x)+.5f-float(size)*.5f)*step,(float(z)+.5f-float(size)*.5f)*step);
        Point surface{};
        if(!impl->containsGround(ref,requested,surface)) continue;
        auto& cell=layer.cells[index];
        const float heightError=std::abs(surface[1]-feet[1]);
        const float oldHeightError=std::abs(cell.groundHeight-feet[1]);
        if(cell.heightValid && ((cell.traversable && !connected) ||
           (cell.traversable==connected && (ranks[index]<rank ||
             (ranks[index]==rank && (oldHeightError<heightError ||
               (oldHeightError==heightError && cell.polygon<=uint64_t(ref)))))))) continue;
        cell.heightValid=true;cell.traversable=connected;cell.groundHeight=surface[1];cell.polygon=uint64_t(ref);
        ranks[index]=rank;
        }
      }
    };
  fill(result.near);fill(result.far);
  // A single height per XZ cannot represent stacked floors. Keep the sheet
  // closest in ground-walk distance to support, and remove shortcuts at its
  // discontinuities rather than painting the lower floor green across a ledge.
  const auto position=[&](const RasterLayer& layer,size_t index) {
    const float right=(float(index%layer.size)+.5f-float(layer.size)*.5f)*layer.cellSize;
    const float forward=(float(index/layer.size)+.5f-float(layer.size)*.5f)*layer.cellSize;
    auto point=world(right,forward);point[1]=layer.cells[index].groundHeight;return point;
    };
  std::vector<uint8_t> nearBoundary(result.near.cells.size()),farBoundary(result.far.cells.size());
  const auto compare=[&](const RasterLayer& a,size_t ai,const RasterLayer& b,size_t bi,
                         std::vector<uint8_t>& amask,std::vector<uint8_t>& bmask) {
    const auto& first=a.cells[ai];const auto& second=b.cells[bi];
    if(!first.traversable || !second.traversable || first.polygon==second.polygon) return;
    if(!impl->exactGroundSegment(dtPolyRef(first.polygon),position(a,ai),dtPolyRef(second.polygon),position(b,bi)) ||
       !impl->exactGroundSegment(dtPolyRef(second.polygon),position(b,bi),dtPolyRef(first.polygon),position(a,ai))) {
      amask[ai]=1;bmask[bi]=1;
      }
    };
  const auto edges=[&](const RasterLayer& layer,std::vector<uint8_t>& mask,bool far) {
    for(size_t z=0;z<layer.size;++z)for(size_t x=0;x<layer.size;++x) {
      const size_t i=z*layer.size+x;
      const float right=(float(x)+.5f-float(layer.size)*.5f)*layer.cellSize;
      const float forward=(float(z)+.5f-float(layer.size)*.5f)*layer.cellSize;
      if(far && std::abs(right)<8.f && std::abs(forward)<8.f) continue;
      if(x+1<layer.size) {
        const float other=right+layer.cellSize;
        if(!far || !(std::abs(other)<8.f && std::abs(forward)<8.f)) compare(layer,i,layer,i+1,mask,mask);
        }
      if(z+1<layer.size) {
        const float other=forward+layer.cellSize;
        if(!far || !(std::abs(right)<8.f && std::abs(other)<8.f)) compare(layer,i,layer,i+layer.size,mask,mask);
        }
      }
    };
  edges(result.near,nearBoundary,false);edges(result.far,farBoundary,true);
  // Compare the nearest outside far cell at each fine-layer boundary. Far
  // cells hidden under the near layer never dictate fine-grid wall clearance.
  for(size_t z=0;z<64;++z)for(size_t x=0;x<64;++x) {
    if(x!=0 && x!=63 && z!=0 && z!=63) continue;
    const float right=(float(x)+.5f-32.f)*.25f,forward=(float(z)+.5f-32.f)*.25f;
    int fx=int(std::floor(right/2.f+50.f)),fz=int(std::floor(forward/2.f+50.f));
    if(x==0)fx=45;else if(x==63)fx=54;
    if(z==0)fz=45;else if(z==63)fz=54;
    compare(result.near,z*64+x,result.far,size_t(fz)*100+size_t(fx),nearBoundary,farBoundary);
    }
  const auto finish=[](RasterLayer& layer,const std::vector<uint8_t>& mask) {
    for(size_t i=0;i<layer.cells.size();++i)if(mask[i]) {
      layer.cells[i].heightBoundary=true;layer.cells[i].traversable=false;
      }
    };
  finish(result.near,nearBoundary);finish(result.far,farBoundary);
  return result;
  }

bool NavMesh::straightWalkable(const Point& start, const Point& end) const {
  if(!impl->query) return false;
  for(float value:start) if(!std::isfinite(value)) return false;
  for(float value:end) if(!std::isfinite(value)) return false;
  const auto& p=impl->params;
  const float extents[3]={p.nearestHorizontal,p.nearestVertical,p.nearestHorizontal};
  dtQueryFilter filter;
  filter.setIncludeFlags(1);
  dtPolyRef startRef=0, endRef=0;
  Point snappedStart{}, snappedEnd{};
  if(dtStatusFailed(impl->query->findNearestPoly(start.data(),extents,&filter,&startRef,snappedStart.data())) || !startRef ||
     dtStatusFailed(impl->query->findNearestPoly(end.data(),extents,&filter,&endRef,snappedEnd.data())) || !endRef)
    return false;
  // A generous nearest-poly search is useful for Detour routes, but direct steering
  // must not silently move an endpoint across a wall or onto another elevation.
  const auto onSurface=[&](const Point& requested,const Point& snapped) {
    return std::hypot(requested[0]-snapped[0],requested[2]-snapped[2])<=p.cellSize &&
           std::abs(requested[1]-snapped[1])<=p.climb+2.f*p.cellHeight;
    };
  if(!onSurface(start,snappedStart) || !onSurface(end,snappedEnd)) return false;
  std::vector<dtPolyRef> corridor(maxPath);
  float t=0;
  Point normal{};
  int count=0;
  const dtStatus status=impl->query->raycast(startRef,snappedStart.data(),snappedEnd.data(),&filter,
                                            &t,normal.data(),corridor.data(),&count,maxPath);
  return dtStatusSucceed(status) && !dtStatusDetail(status,DT_BUFFER_TOO_SMALL) &&
         t>1.f && count>0 && corridor[size_t(count-1)]==endRef;
  }

NavMesh::GroundLocation NavMesh::groundLocation(const Point& position) const {
  GroundLocation result;
  if(!impl->query) return result;
  for(float value:position) if(!std::isfinite(value)) return result;
  dtQueryFilter filter;filter.setIncludeFlags(groundOnlyFlag);
  const float extents[3]={impl->params.nearestHorizontal,impl->params.nearestVertical,impl->params.nearestHorizontal};
  std::vector<dtPolyRef> polygons(64);
  int count=0;
  for(;;) {
    const auto status=impl->query->queryPolygons(position.data(),extents,&filter,polygons.data(),&count,int(polygons.size()));
    if(dtStatusFailed(status)) return result;
    if(!dtStatusDetail(status,DT_BUFFER_TOO_SMALL)) break;
    polygons.resize(polygons.size()*2);
    }
  double best=std::numeric_limits<double>::max();
  // Detour's findNearestPoly subtracts walkableClimb from the vertical score
  // when XZ is over a polygon. Use the true 3D surface distance instead so that
  // a nearby bank or a stacked floor cannot win because of that scoring bias.
  for(int i=0; i<count; ++i) {
    Point snapped{};bool over=false;
    if(dtStatusFailed(impl->query->closestPointOnPoly(polygons[size_t(i)],position.data(),snapped.data(),&over))) continue;
    bool finite=true;for(float value:snapped) finite&=std::isfinite(value);
    if(!finite) continue;
    double squared=0;
    for(size_t axis=0; axis<3; ++axis) squared+=std::pow(double(snapped[axis])-double(position[axis]),2);
    if(squared>best || (squared==best && uint64_t(polygons[size_t(i)])>=result.polygon)) continue;
    const auto component=impl->groundComponents.find(polygons[size_t(i)]);
    if(component==impl->groundComponents.end()) continue;
    best=squared;result.known=true;result.snapped=snapped;
    result.polygon=uint64_t(polygons[size_t(i)]);result.component=component->second;
    result.snapDistance=float(std::sqrt(squared));
    result.horizontalDistance=std::hypot(snapped[0]-position[0],snapped[2]-position[2]);
    result.verticalOffset=snapped[1]-position[1];
    }
  return result;
  }

std::vector<NavMesh::GroundLocation> NavMesh::nearbyGroundLocations(const Point& position) const {
  std::vector<GroundLocation> result;
  if(!impl->query || !impl->mesh) return result;
  for(float value:position) if(!std::isfinite(value)) return result;
  // Tile bounds contain the native world height range. Search every local
  // storey rather than guessing ground Y from the actor's current pose.
  Point center=position;
  float low=std::numeric_limits<float>::infinity(),high=-std::numeric_limits<float>::infinity();
  for(int i=0;i<impl->mesh->getMaxTiles();++i) {
    const auto* tile=static_cast<const dtNavMesh&>(*impl->mesh).getTile(i);
    if(tile==nullptr || tile->header==nullptr) continue;
    low=std::min(low,tile->header->bmin[1]);high=std::max(high,tile->header->bmax[1]);
    }
  if(!std::isfinite(low) || !std::isfinite(high)) return result;
  center[1]=(low+high)*.5f;
  const float vertical=(high-low)*.5f+impl->params.cellHeight;
  dtQueryFilter filter;filter.setIncludeFlags(groundOnlyFlag);
  const float extents[3]={6.f,vertical,6.f};
  std::array<dtPolyRef,512> polygons={};int count=0;
  const auto status=impl->query->queryPolygons(center.data(),extents,&filter,polygons.data(),&count,int(polygons.size()));
  if(dtStatusFailed(status) || dtStatusDetail(status,DT_BUFFER_TOO_SMALL)) return result;
  const auto source=groundLocation(position);
  constexpr float Pi=3.14159265358979323846f;
  for(unsigned ring=2;ring<=6;++ring)for(unsigned angle=0;angle<16;++angle) {
    const float radians=float(angle)*(2.f*Pi/16.f);
    const Point sample={position[0]+float(ring)*std::cos(radians),position[1],position[2]+float(ring)*std::sin(radians)};
    GroundLocation closest;
    double best=std::numeric_limits<double>::max();bool bestComponent=false;
    for(int i=0;i<count;++i) {
      const auto ref=polygons[size_t(i)];
      const auto component=impl->groundComponents.find(ref);
      if(component==impl->groundComponents.end()) continue;
      Point snapped{};bool over=false;
      if(dtStatusFailed(impl->query->closestPointOnPoly(ref,sample.data(),snapped.data(),&over))) continue;
      bool finite=true;for(float value:snapped) finite&=std::isfinite(value);
      if(!finite) continue;
      const float horizontal=std::hypot(snapped[0]-position[0],snapped[2]-position[2]);
      if(horizontal<2.f || horizontal>6.f) continue;
      double squared=0;
      for(size_t axis=0;axis<3;++axis) squared+=std::pow(double(snapped[axis])-double(sample[axis]),2);
      const bool same=source.known && component->second==source.component;
      if(closest.known && ((bestComponent && !same) ||
         (bestComponent==same && (squared>best || (squared==best && uint64_t(ref)>=closest.polygon))))) continue;
      closest.known=true;closest.snapped=snapped;closest.polygon=uint64_t(ref);closest.component=component->second;
      closest.snapDistance=distance(snapped,position);closest.horizontalDistance=horizontal;closest.verticalOffset=snapped[1]-position[1];
      best=squared;bestComponent=same;
      }
    if(!closest.known) continue;
    const bool duplicate=std::any_of(result.begin(),result.end(),[&](const GroundLocation& value) {
      return distance(value.snapped,closest.snapped)<.01f;
      });
    if(!duplicate) result.push_back(closest);
    }
  std::sort(result.begin(),result.end(),[&](const GroundLocation& a,const GroundLocation& b) {
    const bool aSource=source.known && a.component==source.component,bSource=source.known && b.component==source.component;
    if(aSource!=bSource) return aSource;
    if(a.snapDistance!=b.snapDistance) return a.snapDistance<b.snapDistance;
    if(a.polygon!=b.polygon) return a.polygon<b.polygon;
    return a.snapped<b.snapped;
    });
  return result;
  }

NavMesh::TransitionSides NavMesh::transitionSides(const Point& source, const Point& end, const Point& current) const {
  TransitionSides sides;
  sides.source=groundLocation(source);sides.end=groundLocation(end);sides.current=groundLocation(current);
  if(!sides.source.known || !sides.end.known) return sides;
  if(sides.source.component!=sides.end.component) {
    sides.method="component";
    sides.takeoffSide=sides.current.known && sides.current.component==sides.source.component;
    sides.landingSide=sides.current.known && sides.current.component==sides.end.component;
    }
  else if(sides.source.polygon!=sides.end.polygon) {
    sides.method="polygon";
    sides.takeoffSide=sides.current.known && sides.current.polygon==sides.source.polygon;
    sides.landingSide=sides.current.known && sides.current.polygon==sides.end.polygon;
    }
  return sides;
  }

NavMesh::TakeoffSite NavMesh::takeoffSite(const Point& source, float insetDistance,
                                          float maxHorizontalSnap, float maxVerticalSnap) const {
  TakeoffSite result;result.snapped=source;result.inset=source;
  if(!impl->query) {result.reason="mesh_not_built";return result;}
  for(float value:source) if(!std::isfinite(value)) {result.reason="invalid_source";return result;}
  if(!std::isfinite(insetDistance) || !std::isfinite(maxHorizontalSnap) || !std::isfinite(maxVerticalSnap) ||
     insetDistance<0 || maxHorizontalSnap<=0 || maxVerticalSnap<=0) {
    result.reason="invalid_limits";return result;
    }
  dtQueryFilter filter;filter.setIncludeFlags(groundOnlyFlag);
  const float extents[3]={maxHorizontalSnap,maxVerticalSnap,maxHorizontalSnap};
  dtPolyRef ref=0;
  if(dtStatusFailed(impl->query->findNearestPoly(source.data(),extents,&filter,&ref,result.snapped.data())) || !ref) {
    result.reason="source_off_mesh";return result;
    }
  result.sourcePolygon=uint64_t(ref);
  result.sourceHorizontalDistance=std::hypot(source[0]-result.snapped[0],source[2]-result.snapped[2]);
  result.sourceVerticalDistance=std::abs(source[1]-result.snapped[1]);
  if(result.sourceHorizontalDistance>maxHorizontalSnap || result.sourceVerticalDistance>maxVerticalSnap) {
    result.reason="source_snap_too_far";return result;
    }
  const dtMeshTile* tile=nullptr;const dtPoly* poly=nullptr;
  if(dtStatusFailed(impl->mesh->getTileAndPolyByRef(ref,&tile,&poly)) || poly->getType()!=DT_POLYTYPE_GROUND) {
    result.reason="source_not_ground";return result;
    }
  Point center{};
  for(unsigned int vertex=0; vertex<poly->vertCount; ++vertex)
    for(size_t axis=0; axis<3; ++axis) center[axis]+=tile->verts[size_t(poly->verts[vertex])*3+axis]/poly->vertCount;
  Point wall{},normal{};
  if(dtStatusFailed(impl->query->findDistanceToWall(ref,result.snapped.data(),5.f,&filter,&result.sourceWallDistance,
                                                   wall.data(),normal.data())) || !std::isfinite(result.sourceWallDistance)) {
    result.sourceWallDistance=0;result.insetWallDistance=0;result.reason="source_wall_query_failed";return result;
    }
  // A tile polygon can be much longer along a boundary than across it. Its
  // centroid then gives a nearly tangential move with negligible wall clearance.
  // Maximize actual ground-wall clearance over bounded directions, always on
  // this same convex polygon. Do not use findDistanceToWall's normal: at a wall
  // its zero-length normalization can be non-finite.
  bool found=false;
  auto consider=[&](const Point& requested) {
    Point adjusted{};bool over=false;
    if(dtStatusFailed(impl->query->closestPointOnPoly(ref,requested.data(),adjusted.data(),&over)) || !over ||
       std::hypot(adjusted[0]-requested[0],adjusted[2]-requested[2])>1.e-4f ||
       std::abs(adjusted[1]-source[1])>maxVerticalSnap) return;
    const float amount=std::hypot(adjusted[0]-result.snapped[0],adjusted[2]-result.snapped[2]);
    if(amount>insetDistance+1.e-4f) return;
    float clearance=0;
    if(dtStatusFailed(impl->query->findDistanceToWall(ref,adjusted.data(),5.f,&filter,&clearance,wall.data(),normal.data())) ||
       !std::isfinite(clearance)) return;
    if(!found || clearance>result.insetWallDistance ||
       (clearance==result.insetWallDistance && adjusted<result.inset)) {
      found=true;result.inset=adjusted;result.insetDistance=amount;result.insetWallDistance=clearance;
      }
    };
  const float dx=center[0]-result.snapped[0],dz=center[2]-result.snapped[2],length=std::hypot(dx,dz);
  Point towardsCenter=result.snapped;
  if(length>1.e-5f) {
    const float amount=std::min(insetDistance,length*.5f);
    towardsCenter[0]+=dx/length*amount;towardsCenter[2]+=dz/length*amount;
    }
  consider(towardsCenter);
  for(unsigned i=0; i<32; ++i) {
    const float angle=float(i)*(2.f*3.14159265358979323846f/32.f);
    auto requested=result.snapped;
    requested[0]+=std::cos(angle)*insetDistance;requested[2]+=std::sin(angle)*insetDistance;
    consider(requested);
    }
  if(!found) {result.reason="inset_left_source_polygon";return result;}
  result.valid=true;result.reason="ok";
  return result;
  }

std::vector<NavMesh::GapCandidate> NavMesh::gapCandidates(const Point& start, const Point& end,
                                                        float maxHorizontal, float maxVertical,
                                                        size_t maxCandidates) const {
  std::vector<GapCandidate> candidates;
  if(!impl->query || !std::isfinite(maxHorizontal) || !std::isfinite(maxVertical) ||
     maxHorizontal<=0 || maxVertical<0 || maxCandidates==0) return candidates;
  for(float value:start) if(!std::isfinite(value)) return candidates;
  for(float value:end) if(!std::isfinite(value)) return candidates;
  dtQueryFilter filter;
  filter.setIncludeFlags(1);
  const float extents[3]={impl->params.nearestHorizontal,impl->params.nearestVertical,impl->params.nearestHorizontal};
  dtPolyRef startRef=0, endRef=0;
  Point snapped{};
  if(dtStatusFailed(impl->query->findNearestPoly(start.data(),extents,&filter,&startRef,snapped.data())) || !startRef ||
     dtStatusFailed(impl->query->findNearestPoly(end.data(),extents,&filter,&endRef,snapped.data())) || !endRef)
    return candidates;
  // Ground-only flood: a candidate joins walkable components; already proven
  // off-mesh actions must not silently change which native action is probed.
  auto component = [&](dtPolyRef root) {
    std::unordered_set<dtPolyRef> seen{root};
    std::vector<dtPolyRef> pending{root};
    for(size_t at=0; at<pending.size(); ++at) {
      const dtMeshTile* tile=nullptr;
      const dtPoly* poly=nullptr;
      if(dtStatusFailed(impl->mesh->getTileAndPolyByRef(pending[at],&tile,&poly))) continue;
      for(unsigned int link=poly->firstLink; link!=DT_NULL_LINK; link=tile->links[link].next) {
        const auto ref=tile->links[link].ref;
        const dtMeshTile* otherTile=nullptr;
        const dtPoly* other=nullptr;
        if(ref && seen.find(ref)==seen.end() &&
           dtStatusSucceed(impl->mesh->getTileAndPolyByRef(ref,&otherTile,&other)) &&
           other->getType()==DT_POLYTYPE_GROUND && (other->flags&1)) {
          seen.insert(ref);
          pending.push_back(ref);
          }
        }
      }
    return pending;
    };
  const auto from=component(startRef);
  if(std::find(from.begin(),from.end(),endRef)!=from.end()) return candidates;
  const auto to=component(endRef);
  struct Segment { Point a,b; };
  auto boundaries = [&](const std::vector<dtPolyRef>& polygons) {
    std::vector<Segment> result;
    for(const auto ref:polygons) {
      float vertices[64*6];
      dtPolyRef neighbors[64];
      int count=0;
      const auto status=impl->query->getPolyWallSegments(ref,&filter,vertices,neighbors,&count,64);
      if(dtStatusFailed(status) || dtStatusDetail(status,DT_BUFFER_TOO_SMALL))
        throw std::runtime_error("navmesh boundary query capacity");
      for(int i=0; i<count; ++i) {
        if(neighbors[i]) continue;
        const auto* p=vertices+i*6;
        result.push_back({{p[0],p[1],p[2]},{p[3],p[4],p[5]}});
        }
      }
    return result;
    };
  const auto a=boundaries(from), b=boundaries(to);
  auto cell = [&](float value) { return int(std::floor(value/maxHorizontal)); };
  auto cellKey = [](int x, int z) { return (uint64_t(uint32_t(x))<<32)|uint32_t(z); };
  std::unordered_map<uint64_t,std::vector<size_t>> bins;
  for(size_t i=0; i<b.size(); ++i) {
    for(int z=cell(std::min(b[i].a[2],b[i].b[2])); z<=cell(std::max(b[i].a[2],b[i].b[2])); ++z)
      for(int x=cell(std::min(b[i].a[0],b[i].b[0])); x<=cell(std::max(b[i].a[0],b[i].b[0])); ++x)
        bins[cellKey(x,z)].push_back(i);
    }
  auto dot = [](const Point& u, const Point& v) { return u[0]*v[0]+u[2]*v[2]; };
  auto subtract = [](const Point& u, const Point& v) { return Point{u[0]-v[0],u[1]-v[1],u[2]-v[2]}; };
  auto interpolate = [](const Segment& s, float t) {
    return Point{s.a[0]+(s.b[0]-s.a[0])*t,s.a[1]+(s.b[1]-s.a[1])*t,s.a[2]+(s.b[2]-s.a[2])*t};
    };
  for(const auto& first:a) {
    std::unordered_set<size_t> checked;
    for(int z=cell(std::min(first.a[2],first.b[2])-maxHorizontal); z<=cell(std::max(first.a[2],first.b[2])+maxHorizontal); ++z) {
      for(int x=cell(std::min(first.a[0],first.b[0])-maxHorizontal); x<=cell(std::max(first.a[0],first.b[0])+maxHorizontal); ++x) {
        const auto found=bins.find(cellKey(x,z));
        if(found==bins.end()) continue;
        for(const auto index:found->second) {
          if(!checked.insert(index).second) continue;
          const auto& second=b[index];
          const auto u=subtract(first.b,first.a), v=subtract(second.b,second.a), w=subtract(first.a,second.a);
          const float uu=dot(u,u), vv=dot(v,v), uv=dot(u,v), uw=dot(u,w), vw=dot(v,w);
          float s=0, t=0;
          if(uu<1.e-8f) t=vv>1.e-8f ? std::clamp(vw/vv,0.f,1.f) : 0.f;
          else if(vv<1.e-8f) s=std::clamp(-uw/uu,0.f,1.f);
          else {
            const float determinant=uu*vv-uv*uv;
            if(determinant>1.e-8f) s=std::clamp((uv*vw-uw*vv)/determinant,0.f,1.f);
            t=(uv*s+vw)/vv;
            if(t<0) { t=0; s=std::clamp(-uw/uu,0.f,1.f); }
            else if(t>1) { t=1; s=std::clamp((uv-uw)/uu,0.f,1.f); }
            }
          GapCandidate candidate;
          candidate.start=interpolate(first,s);
          candidate.end=interpolate(second,t);
          const float dx=candidate.end[0]-candidate.start[0], dz=candidate.end[2]-candidate.start[2];
          candidate.horizontalDistance=std::sqrt(dx*dx+dz*dz);
          candidate.verticalDistance=std::abs(candidate.end[1]-candidate.start[1]);
          if(candidate.horizontalDistance<=maxHorizontal && candidate.verticalDistance<=maxVertical)
            candidates.push_back(candidate);
          }
        }
      }
    }
  std::sort(candidates.begin(),candidates.end(),[&](const GapCandidate& a, const GapCandidate& b) {
    // Prefer nearby probe sites when multiple parallel boundary pairs tie.
    const float scoreA=a.horizontalDistance+0.25f*a.verticalDistance+
                       0.01f*(distance(a.start,start)+distance(a.end,end));
    const float scoreB=b.horizontalDistance+0.25f*b.verticalDistance+
                       0.01f*(distance(b.start,start)+distance(b.end,end));
    if(scoreA!=scoreB) return scoreA<scoreB;
    if(a.start!=b.start) return a.start<b.start;
    return a.end<b.end;
    });
  std::vector<GapCandidate> result;
  for(const auto& candidate:candidates) {
    bool duplicate=false;
    for(const auto& kept:result)
      if(distance(candidate.start,kept.start)<0.75f && distance(candidate.end,kept.end)<0.75f) duplicate=true;
    if(!duplicate) result.push_back(candidate);
    if(result.size()==maxCandidates) break;
    }
  return result;
  }

NavMesh::ShortcutSearch NavMesh::shortcutCandidates(size_t siteBudget, float maxHorizontal,
                                                    float maxVertical) const {
  ShortcutSearch result;
  if(!impl->query || !siteBudget || siteBudget>10000 || !std::isfinite(maxHorizontal) ||
     !std::isfinite(maxVertical) || maxHorizontal<=0 || maxVertical<0) return result;
  result.queryBudget=siteBudget*10;
  dtQueryFilter filter;
  filter.setIncludeFlags(groundOnlyFlag);
  struct Boundary { Point a,b,center; dtPolyRef ref=0; };
  std::vector<Boundary> boundaries;
  std::vector<dtPolyRef> polygons;
  for(int i=0; i<impl->mesh->getMaxTiles(); ++i) {
    const auto* tile=static_cast<const dtNavMesh&>(*impl->mesh).getTile(i);
    if(!tile || !tile->header) continue;
    for(int j=0; j<tile->header->polyCount; ++j) {
      const auto& poly=tile->polys[j];
      if(poly.getType()!=DT_POLYTYPE_GROUND || !(poly.flags&1)) continue;
      const auto ref=impl->mesh->getPolyRefBase(tile)|dtPolyRef(j);
      polygons.push_back(ref);
      Point center{};
      for(unsigned int k=0; k<poly.vertCount; ++k)
        for(size_t axis=0; axis<3; ++axis) center[axis]+=tile->verts[size_t(poly.verts[k])*3+axis]/poly.vertCount;
      float vertices[64*6];dtPolyRef neighbors[64];int count=0;
      const auto status=impl->query->getPolyWallSegments(ref,&filter,vertices,neighbors,&count,64);
      if(dtStatusFailed(status) || dtStatusDetail(status,DT_BUFFER_TOO_SMALL))
        throw std::runtime_error("navmesh shortcut boundary query capacity");
      for(int edge=0; edge<count; ++edge) {
        if(neighbors[edge]) continue;
        const auto* p=vertices+edge*6;
        boundaries.push_back({{p[0],p[1],p[2]},{p[3],p[4],p[5]},center,ref});
        }
      }
    }
  // Connectivity is ground-only, even when this mesh already contains accepted links.
  // Avoid a whole-component failed Detour search for every disconnected roof/island.
  std::unordered_map<dtPolyRef,size_t> components;
  size_t component=0;
  for(const auto root:polygons) {
    if(components.find(root)!=components.end()) continue;
    std::vector<dtPolyRef> pending{root};components[root]=component++;
    for(size_t at=0; at<pending.size(); ++at) {
      const dtMeshTile* tile=nullptr;const dtPoly* poly=nullptr;
      if(dtStatusFailed(impl->mesh->getTileAndPolyByRef(pending[at],&tile,&poly))) continue;
      for(auto link=poly->firstLink; link!=DT_NULL_LINK; link=tile->links[link].next) {
        const auto ref=tile->links[link].ref;
        if(!ref || components.find(ref)!=components.end()) continue;
        const dtMeshTile* otherTile=nullptr;const dtPoly* other=nullptr;
        if(dtStatusSucceed(impl->mesh->getTileAndPolyByRef(ref,&otherTile,&other)) &&
           other->getType()==DT_POLYTYPE_GROUND && (other->flags&1)) {
          components[ref]=components[root];pending.push_back(ref);
          }
        }
      }
    }
  auto cell=[&](float value){return int(std::floor(value/maxHorizontal));};
  auto key=[](int x,int z){return (uint64_t(uint32_t(x))<<32)|uint32_t(z);};
  std::unordered_map<uint64_t,std::vector<size_t>> bins;
  for(size_t i=0; i<boundaries.size(); ++i) {
    const auto& b=boundaries[i];
    for(int z=cell(std::min(b.a[2],b.b[2])); z<=cell(std::max(b.a[2],b.b[2])); ++z)
      for(int x=cell(std::min(b.a[0],b.b[0])); x<=cell(std::max(b.a[0],b.b[0])); ++x)
        bins[key(x,z)].push_back(i);
    }
  auto inward=[&](const Point& edge,const Boundary& boundary) {
    auto point=edge;
    const float dx=boundary.center[0]-edge[0],dz=boundary.center[2]-edge[2];
    const float length=std::hypot(dx,dz);
    if(length>1.e-5f) {
      const float amount=std::min(.15f,length*.5f);
      point[0]+=dx/length*amount;point[2]+=dz/length*amount;
      }
    Point snapped{};bool over=false;
    if(dtStatusSucceed(impl->query->closestPointOnPoly(boundary.ref,point.data(),snapped.data(),&over)) && over)
      return snapped;
    return edge;
    };
  struct Site { ShortcutCandidate candidate; dtPolyRef from=0,to=0; };
  constexpr size_t SitesPerCell=128;
  std::map<std::pair<int,int>,std::vector<Site>> areas;
  auto score=[](const Site& s){return s.candidate.horizontalDistance+.25f*s.candidate.verticalDistance;};
  for(size_t i=0; i<boundaries.size(); ++i) {
    const auto& source=boundaries[i];
    const int samples=std::max(1,int(std::ceil(std::hypot(source.b[0]-source.a[0],source.b[2]-source.a[2])/2.f)));
    for(int sample=0; sample<samples; ++sample) {
      ++result.boundarySamples;
      const float t=(float(sample)+.5f)/float(samples);
      const Point edge={source.a[0]+(source.b[0]-source.a[0])*t,
                        source.a[1]+(source.b[1]-source.a[1])*t,
                        source.a[2]+(source.b[2]-source.a[2])*t};
      std::unordered_set<size_t> checked;
      for(int z=cell(edge[2]-maxHorizontal); z<=cell(edge[2]+maxHorizontal); ++z)
        for(int x=cell(edge[0]-maxHorizontal); x<=cell(edge[0]+maxHorizontal); ++x) {
          const auto found=bins.find(key(x,z));
          if(found==bins.end()) continue;
          for(const auto j:found->second) {
            if(j==i || !checked.insert(j).second) continue;
            const auto& target=boundaries[j];
            if(source.ref==target.ref) continue;
            const float dx=target.b[0]-target.a[0],dz=target.b[2]-target.a[2],length=dx*dx+dz*dz;
            if(length<1.e-6f) continue;
            const float along=std::clamp(((edge[0]-target.a[0])*dx+(edge[2]-target.a[2])*dz)/length,0.f,1.f);
            const Point opposite={target.a[0]+dx*along,target.a[1]+(target.b[1]-target.a[1])*along,target.a[2]+dz*along};
            const float acrossX=opposite[0]-edge[0],acrossZ=opposite[2]-edge[2];
            const float horizontal=std::hypot(acrossX,acrossZ),vertical=std::abs(opposite[1]-edge[1]);
            if(horizontal>maxHorizontal || vertical>maxVertical || (horizontal<.35f && vertical<.4f)) continue;
            // Facing boundary interiors reject pairs running along the same wall,
            // and avoid placing takeoff on the outside of a polygon at a corner.
            if(acrossX*(source.center[0]-edge[0])+acrossZ*(source.center[2]-edge[2])>=-.01f ||
               acrossX*(target.center[0]-opposite[0])+acrossZ*(target.center[2]-opposite[2])<=.01f) continue;
            Site site;site.from=source.ref;site.to=target.ref;
            auto& candidate=site.candidate;
            candidate.start=inward(edge,source);candidate.end=inward(opposite,target);
            candidate.horizontalDistance=std::hypot(candidate.end[0]-candidate.start[0],candidate.end[2]-candidate.start[2]);
            candidate.verticalDistance=std::abs(candidate.end[1]-candidate.start[1]);
            if(candidate.horizontalDistance>maxHorizontal || candidate.verticalDistance>maxVertical) continue;
            auto& sites=areas[{int(std::floor(edge[0]/50.f)),int(std::floor(edge[2]/50.f))}];
            bool duplicate=false;
            for(const auto& previous:sites)
              if(distance(previous.candidate.start,candidate.start)<.75f && distance(previous.candidate.end,candidate.end)<.75f) {
                duplicate=true;break;
                }
            if(duplicate) continue;
            if(sites.size()<SitesPerCell) sites.push_back(site);
            else {
              auto worst=std::max_element(sites.begin(),sites.end(),[&](const Site& a,const Site& b){return score(a)<score(b);});
              if(score(site)<score(*worst)) *worst=site;
              }
            }
          }
      }
    }
  for(auto& area:areas) {
    auto& sites=area.second;
    result.geometricSites+=sites.size();
    std::sort(sites.begin(),sites.end(),[&](const Site& a,const Site& b) {
      if(score(a)!=score(b)) return score(a)<score(b);
      if(a.candidate.start!=b.candidate.start) return a.candidate.start<b.candidate.start;
      return a.candidate.end<b.candidate.end;
      });
    }
  size_t tested=0;
  for(size_t round=0; round<SitesPerCell; ++round) {
    for(const auto& area:areas) {
      if(round>=area.second.size()) continue;
      if(tested++>=result.queryBudget || result.candidates.size()==siteBudget) return result;
      const auto& site=area.second[round];auto candidate=site.candidate;
      if(components[site.from]==components[site.to]) {
        ++result.groundQueries;
        const auto path=findPath(candidate.start,candidate.end,false);
        if(!path.complete && path.reason!="disconnected") continue;
        candidate.groundComplete=path.complete;candidate.groundDistance=path.length;
        }
      const float direct=distance(candidate.start,candidate.end);
      if(candidate.groundComplete && (candidate.groundDistance<3.f*direct || candidate.groundDistance<direct+10.f)) continue;
      result.candidates.push_back(candidate);
      }
    }
  return result;
  }

void NavMesh::writePolygons(const std::filesystem::path& output) const {
  if(!impl->mesh) throw std::runtime_error("navmesh mesh not built");
  std::ofstream out(output);
  if(!out) throw std::runtime_error("navmesh cannot open polygon output "+output.string());
  out << std::setprecision(8);
  for(int i=0; i<impl->mesh->getMaxTiles(); ++i) {
    const dtMeshTile* tile=static_cast<const dtNavMesh&>(*impl->mesh).getTile(i);
    if(tile==nullptr || tile->header==nullptr) continue;
    for(int j=0; j<tile->header->polyCount; ++j) {
      const dtPoly& poly=tile->polys[j];
      if(poly.getType()!=DT_POLYTYPE_GROUND) continue;
      out << "{\"type\":\"polygon\",\"tile\":[" << tile->header->x << ',' << tile->header->y << "],\"vertices\":[";
      for(unsigned int k=0; k<poly.vertCount; ++k) {
        const float* v=&tile->verts[size_t(poly.verts[k])*3];
        if(k>0) out << ',';
        out << '[' << v[0] << ',' << v[1] << ',' << v[2] << ']';
        }
      out << "]}\n";
      }
    }
  if(!out) throw std::runtime_error("navmesh cannot write polygon output "+output.string());
  }

const NavMesh::Stats& NavMesh::stats() const { return impl->stats; }
const NavMesh::Params& NavMesh::params() const { return impl->params; }
