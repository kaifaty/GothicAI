#include "stressbase.h"
#include "stressmeshes.h"

#include "baseimport.h"
#include "basestore.h"
#include "gothicglb.h"
#include "worldchunksruntime.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

namespace StressBase {

const char* const usage =
  "       content-convert stress-base <ready base> <out dir> --subdivide 0..3 [--meshes 0..3]\n"
  "       content-convert verify-stress-base <source base> <stress copy>\n";

namespace {

// Chunks carry source indices as floats, exact below 2^24 (common/worldchunks.cpp).
constexpr uint64_t formatLimit  = uint64_t(1)<<24;
constexpr int      maxSubdivide = 3;
constexpr const char* stressName = "stress.json";

void require(bool ok, const std::string& what) {
  if(!ok)
    throw std::runtime_error(what);
  }

uint64_t pairKey(uint32_t a, uint32_t b) {
  if(a>b)
    std::swap(a,b);
  return (uint64_t(a)<<32) | b;
  }

uint32_t midLight(uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for(unsigned s=0; s<32; s+=8)
    r |= ((((a>>s)&0xFFu)+((b>>s)&0xFFu)+1u)/2u) << s;
  return r;
  }

D3 corner(const asset::WorldMesh& m, size_t t, unsigned c) {
  const auto& v = m.vertices[m.polygons.vertex_indices[t*3+c]];
  return {v.x,v.y,v.z};
  }

// Twice the area, oriented by the corner order.
D3 normal2(const asset::WorldMesh& m, size_t t) {
  const auto p0 = corner(m,t,0);
  return cross(sub(corner(m,t,1),p0),sub(corner(m,t,2),p0));
  }

fs::path fullPath(const fs::path& p) {
  return fs::weakly_canonical(fs::absolute(p));
  }

bool inside(const fs::path& path, const fs::path& root) {
  auto p = path.begin();
  for(auto r=root.begin(); r!=root.end(); ++r, ++p) {
    if(r->empty())
      continue; // trailing separator
    if(p==path.end() || *p!=*r)
      return false;
    }
  return true;
  }

rapidjson::Document parseJson(const fs::path& file) {
  const auto bytes = gothicglb::readFile(file);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError() && doc.IsObject(),"invalid JSON: "+file.string());
  return doc;
  }

// World directories of a base, sorted by name.
std::vector<std::string> worldNames(const fs::path& base) {
  std::vector<std::string> names;
  if(fs::is_directory(base/"worlds"))
    for(const auto& entry:fs::directory_iterator(base/"worlds"))
      if(entry.is_directory() && fs::is_regular_file(entry.path()/"world.json"))
        names.push_back(entry.path().filename().string());
  std::sort(names.begin(),names.end());
  return names;
  }

// Files of a world the chunk writer produces; everything else of the base is linked.
bool writtenWorldFile(const fs::path& relative, const std::set<std::string>& worlds) {
  auto it = relative.begin();
  if(it==relative.end() || *it!="worlds" || ++it==relative.end() || worlds.count(it->string())==0 ||
     ++it==relative.end())
    return false;
  return *it=="chunks" || *it=="world.json" || *it=="unreferenced.bin";
  }

// writeWorldChunks leaves out the archive index the import adds after it (baseimport.cpp,
// addWorldArchives); the game's loader requires it (worldbase.cpp). The source's goes over unchanged.
void keepArchives(const fs::path& from, const fs::path& to) {
  const auto source = parseJson(from/"world.json");
  require(source.HasMember("archives") && source["archives"].IsArray(),"world.json without archives: "+from.string());
  rapidjson::StringBuffer archives;
  rapidjson::Writer<rapidjson::StringBuffer> writer(archives);
  source["archives"].Accept(writer);
  const auto bytes = gothicglb::readFile(to/"world.json");
  std::string text(bytes.begin(),bytes.end());
  require(!text.empty() && text[0]=='{' && text.find("\"archives\"")==std::string::npos,"unexpected world.json: "+to.string());
  text.insert(1,std::string("\n  \"archives\": ")+archives.GetString()+",");
  gothicglb::writeFile(to/"world.json",text.data(),text.size());
  }

struct WorldCount {
  std::string name;
  uint64_t    before = 0, after = 0;
  size_t      chunksBefore = 0, chunksAfter = 0;
  };

void checkSource(const fs::path& base, BaseImport::Identity& identity) {
  require(fs::is_regular_file(base/"base.json"),"no ready base at "+base.string()+": base.json is missing");
  require(!fs::exists(base/stressName),"the source is already a stress copy: "+base.string());
  identity = BaseImport::readIdentity(base);
  std::string reason;
  require(BaseImport::matches(base,identity,false,&reason),"the source base is not ready: "+reason);
  }

int build(const fs::path& baseArg, const fs::path& outArg, int subdivide, int meshLevel) {
  const auto started = std::chrono::steady_clock::now();
  require(subdivide>=0 && subdivide<=maxSubdivide,"--subdivide must be 0..3");
  require(meshLevel>=0 && meshLevel<=maxSubdivide,"--meshes must be 0..3");
  require(fs::is_directory(baseArg),"no ready base at "+baseArg.string());
  const auto base = fullPath(baseArg);
  const auto out  = fullPath(outArg);
  BaseImport::Identity identity;
  checkSource(base,identity);

  // Never write into a live base location or the source itself.
  std::vector<fs::path> forbidden = {base};
  if(const auto* home=std::getenv("HOME"); home && *home) {
    forbidden.push_back(fullPath(fs::path(home)/".cache/gothic-ai/base"));
    forbidden.push_back(fullPath(fs::path(home)/".local/share/gothic-ai"));
    }
  try { forbidden.push_back(BaseStore::cacheRoot()); }
  catch(const std::exception&) {}
  for(const auto& root:forbidden)
    require(!inside(out,root),"refusing to write "+out.string()+": it is inside "+root.string());
  require(!inside(base,out),"refusing to write "+out.string()+": it contains the source base");
  const bool existed = fs::exists(out);
  if(existed)
    require(fs::is_directory(out) && fs::is_empty(out),"output exists and is not empty: "+out.string());

  const auto names = worldNames(base);
  require(!names.empty(),"the source base has no worlds: "+base.string());
  const uint64_t factor = uint64_t(1)<<(2*subdivide);
  std::vector<WorldCount> counts;
  std::string over;
  for(const auto& name:names) {
    const auto manifest = readWorldChunkManifest(base/"worlds"/name);
    counts.push_back({name,manifest.triangles,manifest.triangles*factor,manifest.chunks.size(),0});
    if(counts.back().after>=formatLimit)
      over += " "+name+" ("+std::to_string(counts.back().after)+")";
    }
  require(over.empty(),"triangles after --subdivide "+std::to_string(subdivide)+" exceed the chunk format limit of "
          "2^24 = 16777216 (exact float indices):"+over);

  fs::create_directories(out);
  size_t linked = 0, copied = 0;
  try {
    for(auto& c:counts) {
      const auto source = base/"worlds"/c.name;
      const auto manifest = readWorldChunkManifest(source);
      auto mesh = readWorldChunks(source);
      require(mesh.polygons.material_indices.size()==c.before,"world "+c.name+": triangle count differs from world.json");
      for(int level=0; level<subdivide; ++level)
        mesh = StressBase::subdivide(mesh);
      // The importer's writer: the same contract for ids, order, flags and lightmaps.
      const auto written = writeWorldChunks(mesh,manifest.source,out/"worlds"/c.name,manifest.chunkSize);
      keepArchives(source,out/"worlds"/c.name);
      require(written.triangles==c.after,"world "+c.name+": written triangle count differs");
      c.chunksAfter = written.chunks.size();
      std::cout << c.name << ": " << c.before << " -> " << c.after << " triangles (x" << factor << "), "
                << c.chunksBefore << " -> " << c.chunksAfter << " chunks" << std::endl;
      }

    // Static meshes that fit the format after the split; the rest is linked below.
    const auto meshes = Meshes::build(base,out,meshLevel);

    // Every other family as it is: hard links, or copies across file systems.
    const std::set<std::string> worlds(names.begin(),names.end());
    for(auto it=fs::recursive_directory_iterator(base); it!=fs::recursive_directory_iterator(); ++it) {
      const auto relative = it->path().lexically_relative(base);
      const auto name = relative.generic_string();
      require(!it->is_symlink(),"the source base contains a symbolic link: "+it->path().string());
      if(name=="base.json" || name=="base-files.json" || meshes.written.count(name)!=0)
        continue;
      if(writtenWorldFile(relative,worlds)) {
        if(it->is_directory())
          it.disable_recursion_pending();
        continue;
        }
      const auto target = out/relative;
      if(it->is_directory()) {
        fs::create_directories(target);
        continue;
        }
      require(it->is_regular_file(),"unexpected entry in the source base: "+it->path().string());
      std::error_code error;
      fs::create_hard_link(it->path(),target,error);
      if(!error) {
        ++linked;
        continue;
        }
      fs::copy_file(it->path(),target);
      ++copied;
      }

    std::string json = "{\n  \"format\": \"gothic-stress-base\",\n  \"version\": 1,\n  \"subdivide\": " +
                       std::to_string(subdivide) + ",\n  \"source\": {\"fingerprint\": \"" + identity.fingerprint +
                       "\", \"manifest_sha256\": \"" + BaseImport::sha256File(base/"base-files.json") +
                       "\"},\n  \"worlds\": [";
    for(size_t i=0; i<counts.size(); ++i)
      json += std::string(i ? "," : "") + "\n    {\"name\": " + gothicglb::jsonString(counts[i].name) +
              ", \"triangles_before\": " + std::to_string(counts[i].before) +
              ", \"triangles_after\": " + std::to_string(counts[i].after) +
              ", \"chunks_before\": " + std::to_string(counts[i].chunksBefore) +
              ", \"chunks_after\": " + std::to_string(counts[i].chunksAfter) + "}";
    json += "\n  ],\n  " + Meshes::json(meshes) + "\n}\n";
    gothicglb::writeFile(out/stressName,json.data(),json.size());
    // The identity stays; only base-files.json and its binding are rebuilt.
    BaseImport::writeBaseMetadata(out,identity);
    }
  catch(...) {
    std::error_code ignored;
    if(existed) {
      for(const auto& entry:fs::directory_iterator(out,ignored))
        fs::remove_all(entry.path(),ignored);
      }
    else
      fs::remove_all(out,ignored);
    throw;
    }

  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  std::cout << "stress base: " << out.string() << " (subdivide " << subdivide << ", meshes " << meshLevel << ", " << linked << " files linked, "
            << copied << " copied, " << std::fixed << std::setprecision(1) << seconds << " s)\n"
            << "run the game with:\n"
            << "OPENGOTHIC_BASE=" << out.string() << "\n"
            << "OPENGOTHIC_BASE_CACHE=" << out.string() << ".cache\n";
  return 0;
  }

// ---- verification ----

std::map<std::string,std::string> manifestFiles(const fs::path& base) {
  const auto doc = parseJson(base/"base-files.json");
  require(doc.HasMember("files") && doc["files"].IsArray(),"invalid base-files.json in "+base.string());
  std::map<std::string,std::string> files;
  for(const auto& f:doc["files"].GetArray())
    files[f["path"].GetString()] = f["sha256"].GetString();
  return files;
  }

uint64_t mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h<<6) + (h>>2);
  h ^= h>>31;
  h *= 0xBF58476D1CE4E5B9ull;
  return h ^ (h>>29);
  }

std::array<uint32_t,3> pointBits(const asset::Vec3& v) {
  return {gothicglb::bits(v.x),gothicglb::bits(v.y),gothicglb::bits(v.z)};
  }

// An edge by the exact bits of its end points, in either direction.
uint64_t edgeHash(const asset::Vec3& a, const asset::Vec3& b) {
  auto x = pointBits(a), y = pointBits(b);
  if(y<x)
    std::swap(x,y);
  uint64_t h = 0;
  for(const auto v:x) h = mix(h,v);
  for(const auto v:y) h = mix(h,v);
  return h;
  }

asset::Vec3 vertex(const asset::WorldMesh& m, size_t t, unsigned c) {
  return m.vertices[m.polygons.vertex_indices[t*3+c%3]];
  }

// Every triangle edge with its chunk, sorted by edge.
std::vector<std::pair<uint64_t,uint32_t>> edgeList(const asset::WorldMesh& m, const std::vector<uint32_t>& owner) {
  const size_t n = m.polygons.material_indices.size();
  std::vector<std::pair<uint64_t,uint32_t>> edges;
  edges.reserve(n*3);
  for(size_t t=0; t<n; ++t)
    for(unsigned c=0; c<3; ++c)
      edges.emplace_back(edgeHash(vertex(m,t,c),vertex(m,t,c+1)),owner[t]);
  std::sort(edges.begin(),edges.end());
  return edges;
  }

size_t uses(const std::vector<std::pair<uint64_t,uint32_t>>& edges, uint64_t h) {
  const auto lo = std::lower_bound(edges.begin(),edges.end(),std::make_pair(h,uint32_t(0)));
  const auto hi = std::upper_bound(lo,edges.end(),std::make_pair(h,UINT32_MAX));
  return size_t(hi-lo);
  }

struct EdgeStats {
  uint64_t open  = 0; // edges used an odd number of times
  uint64_t cross = 0; // edges used by triangles of two or more chunks
  };

EdgeStats edgeStats(const std::vector<std::pair<uint64_t,uint32_t>>& edges) {
  EdgeStats s;
  for(size_t i=0; i<edges.size();) {
    size_t j = i;
    while(j<edges.size() && edges[j].first==edges[i].first)
      ++j;
    s.open  += (j-i)%2;
    s.cross += edges[j-1].second!=edges[i].second ? 1 : 0;
    i = j;
    }
  return s;
  }

asset::Vec3 midPoint(const asset::Vec3& a, const asset::Vec3& b) {
  return {mid(a.x,b.x),mid(a.y,b.y),mid(a.z,b.z)};
  }

// The 2^levels pieces of a source edge, as the split computes them, each used at least `need` times.
bool piecesShared(const std::vector<std::pair<uint64_t,uint32_t>>& copy, const asset::Vec3& a, const asset::Vec3& b,
                  int levels, size_t need) {
  if(levels==0)
    return uses(copy,edgeHash(a,b))>=need;
  const auto m = midPoint(a,b);
  return piecesShared(copy,a,m,levels-1,need) && piecesShared(copy,m,b,levels-1,need);
  }

std::pair<asset::Vec3,asset::Vec3> bounds(const asset::WorldMesh& m) {
  asset::Vec3 lo(INFINITY), hi(-INFINITY);
  for(const auto v:m.polygons.vertex_indices)
    for(unsigned ax=0; ax<3; ++ax) {
      lo[ax] = std::min(lo[ax],m.vertices[v][ax]);
      hi[ax] = std::max(hi[ax],m.vertices[v][ax]);
      }
  return {lo,hi};
  }

// expected: triangles and chunks before and after, as stress.json records them.
void verifyWorld(const fs::path& sourceDir, const fs::path& copyDir, const std::string& name, int subdivide,
                 const std::array<uint64_t,4>& expected) {
  const auto where = "world "+name+": ";
  const auto ms = readWorldChunkManifest(sourceDir);
  const auto mc = readWorldChunkManifest(copyDir);
  require(mc.source.world==ms.source.world && mc.source.size==ms.source.size && mc.source.crc32==ms.source.crc32 &&
          mc.chunkSize==ms.chunkSize && mc.materials.size()==ms.materials.size(),where+"world.json identity differs");
  // Every world.json field but the counts and the chunk list is the source's, the archive index included.
  {
  const auto js = parseJson(sourceDir/"world.json"), jc = parseJson(copyDir/"world.json");
  const std::set<std::string> recounted = {"triangles","vertices","features","chunks"};
  require(js.MemberCount()==jc.MemberCount(),where+"world.json fields differ from the source");
  for(const auto& m:js.GetObject()) {
    const std::string key = m.name.GetString();
    require(jc.HasMember(m.name) && (recounted.count(key)!=0 || jc[m.name]==m.value),where+"world.json "+key+" differs from the source");
    }
  }
  ChunkTable ts, tc;
  const auto s = readWorldChunks(sourceDir,{},&ts);
  // The game's reader: every chunk of the copy is parsed and stitched here.
  const auto c = readWorldChunks(copyDir,{},&tc);
  const size_t ns = s.polygons.material_indices.size(), nc = c.polygons.material_indices.size();
  const unsigned shift = unsigned(2*subdivide);
  require(ns==expected[0] && nc==expected[1] && ts.entries.size()==expected[2] && tc.entries.size()==expected[3],
          where+"stress.json triangle or chunk counts differ from the chunks");
  require(nc==(uint64_t(ns)<<shift),where+std::to_string(nc)+" triangles, expected "+std::to_string(ns)+" x 4^"+
          std::to_string(subdivide));
  require(s.materials.size()==c.materials.size(),where+"material table differs");

  // Each chunk again through the streaming reader the game uses for one chunk.
  const auto lists = chunkTriangleLists(c,tc);
  size_t reads = 0;
  for(size_t i=0; i<lists.size(); ++i) {
    std::vector<uint32_t> sources;
    for(const auto t:lists[i])
      sources.push_back(tc.triangleSources[t]);
    const auto chunk = tc.source->read(i,sources);
    require(chunk.mesh.polygons.material_indices.size()==lists[i].size() && lists[i].size()==tc.entries[i].triangleCount,
            where+"chunk "+tc.entries[i].file+" reads back a different triangle count");
    for(size_t k=0; k<lists[i].size(); ++k)
      for(unsigned v=0; v<3; ++v) {
        const auto& a = chunk.mesh.vertices[chunk.mesh.polygons.vertex_indices[k*3+v]];
        const auto& b = c.vertices[c.polygons.vertex_indices[size_t(lists[i][k])*3+v]];
        require(a==b,where+"chunk "+tc.entries[i].file+" reads back other positions");
        }
    ++reads;
    }

  const auto bs = bounds(s), bc = bounds(c);
  require(bs.first==bc.first && bs.second==bc.second,where+"bounding box differs");

  std::vector<double> areaS(s.materials.size(),0.0), areaC(c.materials.size(),0.0);
  double totalS = 0, totalC = 0;
  // A source sliver thinner than the float rounding of N midpoints has no plane at float
  // precision: a rounded midpoint may land on either side of it. Its children may be
  // degenerate or face either way; they are counted, not judged.
  const double ulp = std::ldexp(1.0,-23);
  std::vector<char> degenerate(ns,0);
  uint64_t zero = 0, slivers = 0;
  double sliverS = 0, sliverC = 0;
  std::vector<double> rounding(s.materials.size(),0.0);
  for(size_t t=0; t<ns; ++t) {
    const double a2 = length(normal2(s,t));
    double edge = 0, scale = 1;
    for(unsigned k=0; k<3; ++k) {
      edge = std::max(edge,length(sub(corner(s,t,(k+1)%3),corner(s,t,k))));
      for(const auto v:corner(s,t,k))
        scale = std::max(scale,std::abs(v));
      }
    const bool sliver = a2!=0 && a2/edge<std::ldexp(scale*ulp,subdivide+1);
    zero    += a2==0 ? 1 : 0;
    slivers += sliver ? 1 : 0;
    degenerate[t] = a2==0 || sliver ? 1 : 0;
    if(degenerate[t]) {
      sliverS += a2*0.5;
      continue;
      }
    // A rounded midpoint leaves its edge by at most sqrt(3)/2 float steps per split:
    // the children gain or lose at most that times half the perimeter.
    double perimeter = 0;
    for(unsigned k=0; k<3; ++k)
      perimeter += length(sub(corner(s,t,(k+1)%3),corner(s,t,k)));
    rounding[s.polygons.material_indices[t]] += 0.5*perimeter*std::sqrt(3.0)*0.5*scale*ulp*subdivide;
    areaS[s.polygons.material_indices[t]] += a2*0.5;
    totalS += a2*0.5;
    }
  uint64_t moved = 0, newDegenerate = 0, offPlane = 0, flipped = 0, attributes = 0, sliverChildren = 0;
  std::string firstProblem;
  for(size_t t=0; t<nc; ++t) {
    const size_t parent = t>>shift;
    const auto n = normal2(c,t);
    const double a2 = length(n);
    (degenerate[parent] ? sliverC : areaC[c.polygons.material_indices[t]]) += a2*0.5;
    totalC += degenerate[parent] ? 0.0 : a2*0.5;
    if(tc.entries[tc.triangleChunks[t]].id!=ts.entries[ts.triangleChunks[parent]].id)
      ++moved;
    // The triangle the copy's order number points to keeps its source's record and plane.
    if(c.polygons.material_indices[t]!=s.polygons.material_indices[parent] ||
       c.polygons.lightmap_indices[t]!=s.polygons.lightmap_indices[parent] ||
       !(c.polygons.flags[t]==s.polygons.flags[parent])) {
      if(attributes++==0) firstProblem = "triangle "+std::to_string(t)+" has another material, lightmap or flags";
      }
    if(degenerate[parent]) {
      sliverChildren += a2==0 || dot(n,normal2(s,parent))<=0 ? 1 : 0;
      continue;
      }
    if(a2==0) {
      if(newDegenerate++==0 && firstProblem.empty()) firstProblem = "triangle "+std::to_string(t)+" is degenerate";
      continue;
      }
    const auto np = normal2(s,parent);
    const auto unit = D3{np[0]/length(np),np[1]/length(np),np[2]/length(np)};
    const auto p0 = corner(s,parent,0);
    double scale = 1;
    for(unsigned k=0; k<3; ++k)
      for(const auto v:corner(s,parent,k))
        scale = std::max(scale,std::abs(v));
    const double tolerance = 2.0*(subdivide+1)*scale*ulp;
    for(unsigned k=0; k<3; ++k)
      if(std::abs(dot(unit,sub(corner(c,t,k),p0)))>tolerance) {
        if(offPlane++==0 && firstProblem.empty()) firstProblem = "triangle "+std::to_string(t)+" leaves its source plane";
        break;
        }
    if(dot(n,np)<=0 && flipped++==0 && firstProblem.empty())
      firstProblem = "triangle "+std::to_string(t)+" faces the other way";
    }
  require(attributes==0 && newDegenerate==0 && offPlane==0 && flipped==0,
          where+firstProblem+" (records "+std::to_string(attributes)+", new degenerate "+std::to_string(newDegenerate)+
          ", off plane "+std::to_string(offPlane)+", flipped "+std::to_string(flipped)+")");
  double worst = relative(totalS,totalC);
  require(worst<=1e-5,where+"total area differs by "+std::to_string(worst));
  // Per material: 1e-5, or within the float rounding of the midpoints (small materials of long thin
  // triangles: the rounding is a fixed number of square centimetres, not a share of the area).
  double worstMaterial = 0, worstShare = 0;
  size_t byRounding = 0;
  for(size_t m=0; m<areaS.size(); ++m) {
    const double e = relative(areaS[m],areaC[m]), d = std::abs(areaC[m]-areaS[m]);
    require(e<=1e-5 || d<=rounding[m],where+"area of material "+std::to_string(m)+" ("+s.materials[m].name+
            ") differs by "+std::to_string(e)+" ("+std::to_string(d)+" cm2, rounding bound "+std::to_string(rounding[m])+")");
    byRounding += e>1e-5 ? 1 : 0;
    worstMaterial = std::max(worstMaterial,e);
    if(rounding[m]>0)
      worstShare = std::max(worstShare,d/rounding[m]);
    }

  // No cracks or T-junctions: an edge k source triangles share (across chunks too) is split
  // into 2^N pieces, and each piece is again an edge of at least k triangles of the copy.
  // The open edge count may fall: a midpoint can land on a T-junction vertex of the source.
  const auto edgesS = edgeList(s,ts.triangleChunks), edgesC = edgeList(c,tc.triangleChunks);
  const auto es = edgeStats(edgesS), ec = edgeStats(edgesC);
  uint64_t shared = 0;
  for(size_t t=0; t<ns; ++t)
    for(unsigned k=0; k<3; ++k) {
      const auto a = vertex(s,t,k), b = vertex(s,t,k+1);
      const auto need = uses(edgesS,edgeHash(a,b));
      shared += need>1 ? 1 : 0;
      require(piecesShared(edgesC,a,b,subdivide,need),where+"triangle "+std::to_string(t)+" edge "+std::to_string(k)+
              ": its pieces are not shared as the source edge was (a crack or a chunk border that does not match)");
      }
  require(ec.open<=(es.open<<subdivide),where+"open edges "+std::to_string(ec.open)+", more than "+
          std::to_string(es.open)+" x 2^"+std::to_string(subdivide));

  std::cout << name << ": " << ns << " -> " << nc << " triangles, chunks " << ts.entries.size() << " -> "
            << tc.entries.size() << " (" << reads << " read back one by one), bbox equal, area error "
            << std::scientific << std::setprecision(2) << worst << " (worst material " << worstMaterial << "; "
            << byRounding << " materials over 1e-5 within the rounding bound, worst " << std::fixed << worstShare*100
            << "% of it)"
            << std::defaultfloat << ", shared edge uses kept " << shared << ", open edges " << es.open << " -> "
            << ec.open << ", cross-chunk edges "
            << es.cross << " -> " << ec.cross << ", in a neighbour chunk of their source " << moved
            << ", source degenerate " << zero << " and slivers " << slivers << " (degenerate or turned children "
            << sliverChildren << ", area " << std::fixed << std::setprecision(2) << sliverS << " -> " << sliverC
            << " cm2, not in the area sums)" << std::defaultfloat << std::endl;
  }

int verify(const fs::path& baseArg, const fs::path& copyArg) {
  const auto started = std::chrono::steady_clock::now();
  const auto base = fullPath(baseArg), copy = fullPath(copyArg);
  require(fs::is_regular_file(copy/"base.json"),"no stress copy at "+copy.string());
  BaseImport::Identity identity;
  checkSource(base,identity);
  std::string reason;
  require(BaseImport::matches(copy,identity,true,&reason),"the copy does not match the source identity: "+reason);
  rapidjson::Document a, b;
  a.Parse(identity.json.c_str());
  b.Parse(BaseImport::readIdentity(copy).json.c_str());
  require(!a.HasParseError() && a==b,"base.json identity differs from the source");
  std::cout << "manifest: every file hashed, identity equal to the source (" << identity.fingerprint << ")\n";

  const auto stress = parseJson(copy/stressName);
  require(stress.HasMember("format") && stress["format"]=="gothic-stress-base" && stress.HasMember("subdivide") &&
          stress["subdivide"].IsInt() && stress.HasMember("source") && stress["source"].IsObject() &&
          stress["source"].HasMember("fingerprint") && stress["source"]["fingerprint"]==identity.fingerprint.c_str() &&
          stress.HasMember("worlds") && stress["worlds"].IsArray(),"invalid stress.json or another source fingerprint");
  const int subdivide = stress["subdivide"].GetInt();
  require(subdivide>=0 && subdivide<=maxSubdivide,"stress.json: subdivide out of range");
  std::map<std::string,std::array<uint64_t,4>> recorded;
  for(const auto& w:stress["worlds"].GetArray())
    recorded[w["name"].GetString()] = {w["triangles_before"].GetUint64(),w["triangles_after"].GetUint64(),
                                       w["chunks_before"].GetUint64(),w["chunks_after"].GetUint64()};

  const auto names = worldNames(base);
  require(names==worldNames(copy),"the copy has other worlds than the source");
  const std::set<std::string> worlds(names.begin(),names.end());
  const auto meshes = Meshes::recorded(stress,base);
  // Every other file, unreferenced.bin and the BSP, rooms and sectors included, is the source's.
  const auto filesS = manifestFiles(base), filesC = manifestFiles(copy);
  size_t same = 0;
  for(const auto& [path,sum]:filesC) {
    const fs::path relative(path);
    const bool rewritten = path==stressName || meshes.written.count(path)!=0 ||
                           (writtenWorldFile(relative,worlds) && relative.filename()!="unreferenced.bin");
    if(rewritten)
      continue;
    const auto found = filesS.find(path);
    require(found!=filesS.end() && found->second==sum,"file differs from the source: "+path);
    ++same;
    }
  for(const auto& [path,sum]:filesS) {
    const fs::path relative(path);
    require(filesC.count(path)!=0 || (writtenWorldFile(relative,worlds) && relative.filename()!="unreferenced.bin"),
            "file missing from the copy: "+path);
    }
  for(const auto& path:meshes.written)
    require(filesC.count(path)!=0,"split mesh missing from the copy: "+path);
  std::cout << "unchanged files: " << same << " equal to the source (unreferenced.bin, bsp.bin, rooms.bin, "
               "sectors.json and " << meshes.skipped.size() << " skipped meshes among them)\n";

  for(const auto& name:names) {
    const auto r = recorded.find(name);
    require(r!=recorded.end(),"stress.json does not list world "+name);
    verifyWorld(base/"worlds"/name,copy/"worlds"/name,name,subdivide,r->second);
    }
  Meshes::verify(base,copy,meshes);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  std::cout << "stress base verified: " << copy.string() << " (subdivide " << subdivide << ", meshes " << meshes.level << ", " << std::fixed
            << std::setprecision(1) << seconds << " s)\n";
  return 0;
  }

}

asset::WorldMesh subdivide(const asset::WorldMesh& mesh) {
  const auto& p = mesh.polygons;
  const size_t n = p.material_indices.size();
  asset::WorldMesh out;
  out.name      = mesh.name;
  out.bbox      = mesh.bbox;
  out.materials = mesh.materials;
  out.vertices  = mesh.vertices;
  out.features  = mesh.features;
  std::unordered_map<uint64_t,uint32_t> vertexMid, featureMid;
  vertexMid.reserve(n*2);
  featureMid.reserve(n*2);
  const auto vertexOf = [&](uint32_t a, uint32_t b) {
    const auto [it,inserted] = vertexMid.emplace(pairKey(a,b),uint32_t(out.vertices.size()));
    if(inserted) {
      const auto& x = mesh.vertices[a];
      const auto& y = mesh.vertices[b];
      out.vertices.push_back({mid(x.x,y.x),mid(x.y,y.y),mid(x.z,y.z)});
      }
    return it->second;
    };
  const auto featureOf = [&](uint32_t a, uint32_t b) {
    const auto [it,inserted] = featureMid.emplace(pairKey(a,b),uint32_t(out.features.size()));
    if(inserted) {
      const auto& x = mesh.features[a];
      const auto& y = mesh.features[b];
      asset::VertexFeature f;
      f.texture = {mid(x.texture.x,y.texture.x),mid(x.texture.y,y.texture.y)};
      f.light   = midLight(x.light,y.light);
      f.normal  = {mid(x.normal.x,y.normal.x),mid(x.normal.y,y.normal.y),mid(x.normal.z,y.normal.z)};
      out.features.push_back(f);
      }
    return it->second;
    };

  auto& q = out.polygons;
  q.material_indices.reserve(n*4);
  q.lightmap_indices.reserve(n*4);
  q.flags.reserve(n*4);
  q.vertex_indices.reserve(n*12);
  q.feature_indices.reserve(n*12);
  for(size_t t=0; t<n; ++t) {
    std::array<uint32_t,6> v, f; // corners, then midpoints of edges 01, 12, 20
    for(unsigned c=0; c<3; ++c) {
      v[c] = p.vertex_indices[t*3+c];
      f[c] = p.feature_indices[t*3+c];
      }
    for(unsigned c=0; c<3; ++c) {
      v[3+c] = vertexOf(v[c],v[(c+1)%3]);
      f[3+c] = featureOf(f[c],f[(c+1)%3]);
      }
    // Same winding as the source: three corner triangles, then the middle one.
    static constexpr unsigned children[4][3] = {{0,3,5},{3,1,4},{5,4,2},{3,4,5}};
    for(const auto& child:children) {
      q.material_indices.push_back(p.material_indices[t]);
      q.lightmap_indices.push_back(p.lightmap_indices[t]);
      q.flags.push_back(p.flags[t]);
      for(const auto k:child) {
        q.vertex_indices.push_back(v[k]);
        q.feature_indices.push_back(f[k]);
        }
      }
    }
  return out;
  }

std::optional<int> run(const std::vector<std::string>& args) {
  if(args.empty())
    return std::nullopt;
  if(args[0]=="stress-base") {
    // --subdivide N is required, --meshes M is optional and defaults to N.
    std::map<std::string,int> levels;
    for(size_t i=3; i+1<args.size() && args.size()%2==1; i+=2) {
      const auto& flag = args[i];
      const auto& value = args[i+1];
      if((flag!="--subdivide" && flag!="--meshes") || levels.count(flag)!=0) {
        levels.clear();
        break;
        }
      if(value.size()!=1 || value[0]<'0' || value[0]>char('0'+maxSubdivide)) {
        std::cerr << "content-convert: " << flag << " must be 0..3, got '" << value << "'\n";
        return 2;
        }
      levels[flag] = value[0]-'0';
      }
    if(args.size()<5 || args.size()%2==0 || levels.count("--subdivide")==0) {
      std::cerr << "usage:\n" << usage;
      return 2;
      }
    const int n = levels["--subdivide"];
    return build(args[1],args[2],n,levels.count("--meshes")!=0 ? levels["--meshes"] : n);
    }
  if(args[0]=="verify-stress-base") {
    if(args.size()!=3) {
      std::cerr << "usage:\n" << usage;
      return 2;
      }
    return verify(args[1],args[2]);
    }
  return std::nullopt;
  }

void selfTest(const fs::path& dir) {
  asset::WorldMesh m;
  asset::Material stone, wood;
  stone.name = "STRESS_STONE";
  stone.texture = "STONE.TGA";
  wood.name = "STRESS_WOOD";
  wood.texture = "WOOD.TGA";
  m.materials = {stone,wood};
  // Two triangles sharing the edge 1-2 in both vertex and feature ids.
  m.vertices = {{0,10,0},{200,10,0},{0,30,100},{200,30,100}};
  m.features = {{{0,0},0xFF102030u,{0,1,0}},{{1,0},0xFF204060u,{0,1,0}},
                {{0,1},0x80306090u,{0,0.5f,0.5f}},{{1,1},0x804080C0u,{0,0.5f,0.5f}}};
  auto tri = [&](uint32_t mat, std::array<uint32_t,3> c, int32_t lightmap, int16_t sector) {
    m.polygons.material_indices.push_back(mat);
    m.polygons.lightmap_indices.push_back(lightmap);
    asset::PolygonFlagSet flags {};
    flags.is_sector = 1;
    flags.sector_index = sector;
    m.polygons.flags.push_back(flags);
    for(const auto i:c) {
      m.polygons.vertex_indices.push_back(i);
      m.polygons.feature_indices.push_back(i);
      }
    };
  tri(0,{0,1,2},3,5);
  tri(1,{2,1,3},-1,6);

  const auto s = subdivide(m);
  const auto& q = s.polygons;
  require(q.material_indices.size()==8,"stress split: 2 triangles must give 8");
  // Edges 01, 12, 20, 13, 32: the shared edge has one midpoint vertex and one midpoint feature.
  require(s.vertices.size()==9 && s.features.size()==9,"stress split: the shared edge must have one midpoint");
  require(q.vertex_indices[1*3+2]==q.vertex_indices[4*3+1] && q.feature_indices[1*3+2]==q.feature_indices[4*3+1],
          "stress split: both triangles must use the same midpoint of their shared edge");
  const auto midpoint = q.vertex_indices[1*3+2];
  require(s.vertices[midpoint]==asset::Vec3(100,20,50),"stress split: midpoint position is not the average");
  const auto& mf = s.features[q.feature_indices[1*3+2]];
  require(mf.texture==asset::Vec2(0.5f,0.5f) && mf.light==0xC0285078u && mf.normal==asset::Vec3(0,0.75f,0.25f),
          "stress split: midpoint attributes are not the average of the ends");
  for(size_t t=0; t<8; ++t)
    require(q.material_indices[t]==m.polygons.material_indices[t/4] && q.lightmap_indices[t]==m.polygons.lightmap_indices[t/4] &&
            q.flags[t]==m.polygons.flags[t/4],"stress split: children must keep material, lightmap and flags");
  for(size_t t=0; t<2; ++t) {
    double sum = 0;
    for(size_t k=0; k<4; ++k) {
      const auto n = normal2(s,t*4+k);
      require(dot(n,normal2(m,t))>0,"stress split: a child faces the other way");
      sum += length(n);
      }
    require(relative(sum,length(normal2(m,t)))<1e-12,"stress split: area must be preserved");
    }
  const auto b0 = bounds(m), b1 = bounds(s);
  require(b0.first==b1.first && b0.second==b1.second,"stress split: bounds must be preserved");

  // The importer's writer and the game's reader accept the split mesh as it is.
  const auto out = dir/"stress-split";
  writeWorldChunks(s,{"STRESS.ZEN",1,2},out);
  const auto diff = compareWorldMeshes(canonicalWorldMesh(s),readWorldChunks(out));
  require(diff.empty(),"stress split round trip: "+diff);
  fs::remove_all(out);

  Meshes::selfTest(dir/"stress-meshes");
  fs::remove_all(dir/"stress-meshes");
  }

}
