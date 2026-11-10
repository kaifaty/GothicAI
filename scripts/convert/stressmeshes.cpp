#include "stressmeshes.h"

#include "basemesh.h"
#include "gothicglb.h"

#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

namespace StressBase::Meshes {

namespace {

void require(bool ok, const std::string& what) {
  if(!ok)
    throw std::runtime_error(what);
  }

uint64_t pairKey(uint32_t a, uint32_t b) {
  if(a>b)
    std::swap(a,b);
  return (uint64_t(a)<<32) | b;
  }

rapidjson::Document parseJson(const fs::path& file) {
  const auto bytes = gothicglb::readFile(file);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError() && doc.IsObject(),"invalid JSON: "+file.string());
  return doc;
  }

// meshes/meshes.json of a base: mesh name -> file name inside meshes/.
std::map<std::string,std::string> meshIndex(const fs::path& base) {
  std::map<std::string,std::string> index;
  const auto file = base/"meshes"/"meshes.json";
  if(!fs::is_regular_file(file))
    return index;
  const auto doc = parseJson(file);
  require(doc.HasMember("meshes") && doc["meshes"].IsObject(),"meshes.json without a meshes object: "+file.string());
  for(const auto& m:doc["meshes"].GetObject()) {
    require(m.value.IsString(),"meshes.json: the file of "+std::string(m.name.GetString())+" is not a string");
    const fs::path name(m.value.GetString());
    require(name.has_filename() && name.filename()==name && name.extension()==".glb",
            "meshes.json: unexpected file "+name.string());
    index[m.name.GetString()] = m.value.GetString();
    }
  return index;
  }

uint64_t triangles(const asset::StaticMesh& m) {
  uint64_t n = 0;
  for(const auto& sm:m.sub_meshes)
    n += sm.triangles.size();
  return n;
  }

D3 corner(const asset::StaticMesh& m, const asset::SubMesh& sm, size_t t, unsigned c) {
  const auto& v = m.positions[sm.wedges[sm.triangles[t].wedges[c]].index];
  return {v.x,v.y,v.z};
  }

// Twice the area, oriented by the corner order.
D3 normal2(const asset::StaticMesh& m, const asset::SubMesh& sm, size_t t) {
  const auto p0 = corner(m,sm,t,0);
  return cross(sub(corner(m,sm,t,1),p0),sub(corner(m,sm,t,2),p0));
  }

bool sameBits(const asset::Vec3& a, const asset::Vec3& b) {
  using gothicglb::bits;
  return bits(a.x)==bits(b.x) && bits(a.y)==bits(b.y) && bits(a.z)==bits(b.z);
  }

struct Check {
  uint64_t zero = 0, slivers = 0, sliverChildren = 0;
  double   sliverS = 0, sliverC = 0;
  double   worst = 0, worstShare = 0;
  size_t   byRounding = 0, submeshes = 0;
  };

// One split mesh against its source; `where` names it in a failure.
void verifyMesh(const asset::StaticMesh& s, const asset::StaticMesh& c, int level, const std::string& where, Check& k) {
  require(s.alpha_test==c.alpha_test,where+"alpha_test differs");
  require(sameBits(s.bbox.min,c.bbox.min) && sameBits(s.bbox.max,c.bbox.max),where+"bbox differs");
  require(s.sub_meshes.size()==c.sub_meshes.size(),where+"submesh count "+std::to_string(c.sub_meshes.size())+
          ", source "+std::to_string(s.sub_meshes.size()));
  const unsigned shift = unsigned(2*level);
  const double ulp = std::ldexp(1.0,-23);
  for(size_t i=0; i<s.sub_meshes.size(); ++i) {
    const auto& x = s.sub_meshes[i];
    const auto& y = c.sub_meshes[i];
    const auto at = where+"submesh "+std::to_string(i)+" ("+x.mat.name+"): ";
    // Material, its order and every field of it: the submesh is the source's.
    require(gothicglb::sameMaterial(x.mat,y.mat),at+"material differs");
    const size_t ns = x.wedges.empty() ? 0 : x.triangles.size(), nc = y.wedges.empty() ? 0 : y.triangles.size();
    require(nc==(uint64_t(ns)<<shift),at+std::to_string(nc)+" triangles, expected "+std::to_string(ns)+" x 4^"+
            std::to_string(level));
    if(ns==0)
      continue;
    ++k.submeshes;
    // Source slivers thinner than the float rounding of the midpoints have no plane at float
    // precision; their children may be degenerate or face either way: counted, not judged.
    std::vector<char> degenerate(ns,0);
    double areaS = 0, areaC = 0, rounding = 0;
    std::vector<double> scales(ns,1.0);
    for(size_t t=0; t<ns; ++t) {
      const double a2 = length(normal2(s,x,t));
      double edge = 0, perimeter = 0, scale = 1;
      for(unsigned e=0; e<3; ++e) {
        const double l = length(sub(corner(s,x,t,(e+1)%3),corner(s,x,t,e)));
        edge = std::max(edge,l);
        perimeter += l;
        for(const auto v:corner(s,x,t,e))
          scale = std::max(scale,std::abs(v));
        }
      scales[t] = scale;
      const bool sliver = a2!=0 && a2/edge<std::ldexp(scale*ulp,level+1);
      k.zero    += a2==0 ? 1 : 0;
      k.slivers += sliver ? 1 : 0;
      degenerate[t] = a2==0 || sliver ? 1 : 0;
      if(degenerate[t]) {
        k.sliverS += a2*0.5;
        continue;
        }
      // A rounded midpoint leaves its edge by at most sqrt(3)/2 float steps per split.
      rounding += 0.5*perimeter*std::sqrt(3.0)*0.5*scale*ulp*level;
      areaS += a2*0.5;
      }
    uint64_t newDegenerate = 0, flipped = 0, offPlane = 0;
    std::string first;
    for(size_t t=0; t<nc; ++t) {
      const size_t parent = t>>shift;
      const auto n = normal2(c,y,t);
      const double a2 = length(n);
      if(degenerate[parent]) {
        k.sliverC += a2*0.5;
        k.sliverChildren += a2==0 || dot(n,normal2(s,x,parent))<=0 ? 1 : 0;
        continue;
        }
      areaC += a2*0.5;
      if(a2==0) {
        if(newDegenerate++==0) first = "triangle "+std::to_string(t)+" is degenerate";
        continue;
        }
      const auto np = normal2(s,x,parent);
      if(dot(n,np)<=0 && flipped++==0 && first.empty())
        first = "triangle "+std::to_string(t)+" faces the other way";
      const double lp = length(np);
      const D3 unit = {np[0]/lp,np[1]/lp,np[2]/lp};
      const auto p0 = corner(s,x,parent,0);
      const double tolerance = 2.0*(level+1)*scales[parent]*ulp;
      for(unsigned e=0; e<3; ++e)
        if(std::abs(dot(unit,sub(corner(c,y,t,e),p0)))>tolerance) {
          if(offPlane++==0 && first.empty()) first = "triangle "+std::to_string(t)+" leaves its source plane";
          break;
          }
      }
    require(newDegenerate==0 && flipped==0 && offPlane==0,at+first+" (new degenerate "+std::to_string(newDegenerate)+
            ", flipped "+std::to_string(flipped)+", off plane "+std::to_string(offPlane)+")");
    // The world's rule: 1e-5, or within the float rounding of the midpoints.
    const double e = relative(areaS,areaC), d = std::abs(areaC-areaS);
    require(e<=1e-5 || d<=rounding,at+"area differs by "+std::to_string(e)+" ("+std::to_string(d)+
            " cm2, rounding bound "+std::to_string(rounding)+")");
    k.byRounding += e>1e-5 ? 1 : 0;
    k.worst = std::max(k.worst,e);
    if(rounding>0)
      k.worstShare = std::max(k.worstShare,d/rounding);
    }
  }

}

std::optional<asset::StaticMesh> subdivide(const asset::StaticMesh& mesh, std::string* reason) {
  struct Wedge { asset::Vec3 normal; asset::Vec2 texture; uint32_t index; };
  struct Sub   { std::vector<Wedge> wedges; std::vector<std::array<uint32_t,3>> triangles; };

  std::vector<asset::Vec3> positions = mesh.positions;
  std::unordered_map<uint64_t,uint32_t> positionMid;
  const auto positionOf = [&](uint32_t a, uint32_t b) {
    const auto [it,inserted] = positionMid.emplace(pairKey(a,b),uint32_t(positions.size()));
    if(inserted) {
      const auto& x = mesh.positions[a];
      const auto& y = mesh.positions[b];
      positions.push_back({mid(x.x,y.x),mid(x.y,y.y),mid(x.z,y.z)});
      }
    return it->second;
    };

  std::vector<Sub> subs(mesh.sub_meshes.size());
  std::string over;
  for(size_t s=0; s<mesh.sub_meshes.size(); ++s) {
    const auto& src = mesh.sub_meshes[s];
    auto& dst = subs[s];
    for(const auto& w:src.wedges)
      dst.wedges.push_back({w.normal,w.texture,w.index});
    if(src.triangles.empty() || src.wedges.empty()) {
      // No geometry (the writer leaves such a submesh out): the material stays, as it was.
      dst.wedges.clear();
      continue;
      }
    std::unordered_map<uint64_t,uint32_t> wedgeMid;
    const auto wedgeOf = [&](uint32_t a, uint32_t b) {
      const auto [it,inserted] = wedgeMid.emplace(pairKey(a,b),uint32_t(dst.wedges.size()));
      if(inserted) {
        const auto& x = src.wedges[a];
        const auto& y = src.wedges[b];
        Wedge w;
        w.normal  = {mid(x.normal.x,y.normal.x),mid(x.normal.y,y.normal.y),mid(x.normal.z,y.normal.z)};
        w.texture = {mid(x.texture.x,y.texture.x),mid(x.texture.y,y.texture.y)};
        w.index   = positionOf(x.index,y.index);
        dst.wedges.push_back(w);
        }
      return it->second;
      };
    dst.triangles.reserve(src.triangles.size()*4);
    for(const auto& tri:src.triangles) {
      std::array<uint32_t,6> w; // corners, then midpoints of edges 01, 12, 20
      for(unsigned c=0; c<3; ++c)
        w[c] = tri.wedges[c];
      for(unsigned c=0; c<3; ++c)
        w[3+c] = wedgeOf(w[c],w[(c+1)%3]);
      // Same winding as the source: three corner triangles, then the middle one (as for the world).
      static constexpr unsigned children[4][3] = {{0,3,5},{3,1,4},{5,4,2},{3,4,5}};
      for(const auto& child:children)
        dst.triangles.push_back({w[child[0]],w[child[1]],w[child[2]]});
      }
    if(dst.wedges.size()>maxWedges)
      over += std::string(over.empty() ? "" : "; ")+"submesh "+std::to_string(s)+" ("+src.mat.name+") "+
              std::to_string(dst.wedges.size())+" wedges > "+std::to_string(maxWedges);
    }
  if(positions.size()>maxPositions)
    over = std::to_string(positions.size())+" positions > "+std::to_string(maxPositions)+(over.empty() ? "" : "; ")+over;
  if(!over.empty()) {
    if(reason!=nullptr)
      *reason = over;
    return std::nullopt;
    }

  asset::StaticMesh out;
  out.positions  = std::move(positions);
  out.normals    = mesh.normals;
  out.materials  = mesh.materials;
  out.alpha_test = mesh.alpha_test;
  out.bbox       = mesh.bbox;
  out.obbox      = mesh.obbox;
  out.sub_meshes.resize(mesh.sub_meshes.size());
  for(size_t s=0; s<subs.size(); ++s) {
    auto& dst = out.sub_meshes[s];
    dst.mat = mesh.sub_meshes[s].mat;
    if(subs[s].wedges.empty()) {
      dst.wedges    = mesh.sub_meshes[s].wedges;
      dst.triangles = mesh.sub_meshes[s].triangles;
      continue;
      }
    for(const auto& w:subs[s].wedges)
      dst.wedges.push_back({w.normal,w.texture,uint16_t(w.index)});
    for(const auto& t:subs[s].triangles)
      dst.triangles.push_back({{uint16_t(t[0]),uint16_t(t[1]),uint16_t(t[2])}});
    }
  return out;
  }

Built build(const fs::path& base, const fs::path& out, int level) {
  Built b;
  b.level = level;
  const auto index = meshIndex(base);
  b.meshes = index.size();
  for(const auto& [name,file]:index) {
    // The game's reader: a source mesh it cannot read is an error, not a skip.
    const auto source = readBaseMesh(base/"meshes"/file);
    const uint64_t before = triangles(source);
    b.trianglesBefore += before;
    if(level==0) {
      b.trianglesAfter += before;
      continue;
      }
    std::optional<asset::StaticMesh> mesh = source;
    std::string reason;
    for(int l=0; l<level && mesh; ++l) {
      mesh = subdivide(*mesh,&reason);
      if(!mesh)
        reason = "level "+std::to_string(l+1)+": "+reason;
      }
    if(!mesh) {
      b.skipped.emplace_back(name,reason);
      b.trianglesAfter += before;
      continue;
      }
    const uint64_t after = triangles(*mesh);
    require(after==(before<<(2*level)),"mesh "+name+": "+std::to_string(after)+" triangles after the split, expected "+
            std::to_string(before)+" x 4^"+std::to_string(level));
    fs::create_directories(out/"meshes");
    // The importer's writer, under the importer's name for the mesh.
    writeBaseMesh(*mesh,name,out/"meshes"/file);
    b.written.insert("meshes/"+file);
    b.trianglesAfter += after;
    ++b.split;
    }
  std::cout << "meshes: " << b.split << " of " << b.meshes << " split (level " << level << ", x" << (uint64_t(1)<<(2*level))
            << " each), " << b.skipped.size() << " skipped, " << b.trianglesBefore << " -> " << b.trianglesAfter
            << " triangles over all meshes\n";
  for(const auto& [name,reason]:b.skipped)
    std::cout << "  skipped " << name << ": " << reason << "\n";
  return b;
  }

std::string json(const Built& b) {
  std::string s = "\"meshes\": {\"subdivide\": "+std::to_string(b.level)+", \"count\": "+std::to_string(b.meshes)+
                  ", \"split\": "+std::to_string(b.split)+", \"triangles_before\": "+std::to_string(b.trianglesBefore)+
                  ", \"triangles_after\": "+std::to_string(b.trianglesAfter)+"},\n  \"meshes_skipped\": [";
  for(size_t i=0; i<b.skipped.size(); ++i)
    s += std::string(i ? "," : "")+"\n    {\"name\": "+gothicglb::jsonString(b.skipped[i].first)+", \"reason\": "+
         gothicglb::jsonString(b.skipped[i].second)+"}";
  s += b.skipped.empty() ? "]" : "\n  ]";
  return s;
  }

Built recorded(const rapidjson::Document& stress, const fs::path& base) {
  Built b;
  const auto index = meshIndex(base);
  b.meshes = index.size();
  if(!stress.HasMember("meshes"))
    return b;
  const auto& m = stress["meshes"];
  require(m.IsObject() && m.HasMember("subdivide") && m["subdivide"].IsInt() && m.HasMember("count") &&
          m["count"].IsUint64() && m.HasMember("split") && m["split"].IsUint64() && m.HasMember("triangles_before") &&
          m["triangles_before"].IsUint64() && m.HasMember("triangles_after") && m["triangles_after"].IsUint64() &&
          stress.HasMember("meshes_skipped") && stress["meshes_skipped"].IsArray(),"stress.json: invalid meshes");
  b.level = m["subdivide"].GetInt();
  require(b.level>=0 && b.level<=3,"stress.json: meshes subdivide out of range");
  require(m["count"].GetUint64()==b.meshes,"stress.json: "+std::to_string(m["count"].GetUint64())+
          " meshes, the source has "+std::to_string(b.meshes));
  b.split           = m["split"].GetUint64();
  b.trianglesBefore = m["triangles_before"].GetUint64();
  b.trianglesAfter  = m["triangles_after"].GetUint64();
  std::set<std::string> skipped;
  for(const auto& e:stress["meshes_skipped"].GetArray()) {
    require(e.IsObject() && e.HasMember("name") && e["name"].IsString() && e.HasMember("reason") &&
            e["reason"].IsString() && e["reason"].GetStringLength()>0,"stress.json: invalid meshes_skipped entry");
    const std::string name = e["name"].GetString();
    require(index.count(name)!=0,"stress.json: skipped mesh "+name+" is not in the source's meshes.json");
    require(skipped.insert(name).second,"stress.json: mesh "+name+" skipped twice");
    b.skipped.emplace_back(name,e["reason"].GetString());
    }
  require(b.level>0 || skipped.empty(),"stress.json: meshes skipped at level 0");
  if(b.level>0)
    for(const auto& [name,file]:index)
      if(skipped.count(name)==0)
        b.written.insert("meshes/"+file);
  require(b.split==b.written.size(),"stress.json: "+std::to_string(b.split)+" meshes split, the source has "+
          std::to_string(b.written.size())+" not skipped");
  return b;
  }

void verify(const fs::path& base, const fs::path& copy, const Built& r) {
  const auto index = meshIndex(base);
  require(index==meshIndex(copy),"meshes.json of the copy differs from the source");
  std::set<std::string> skipped;
  for(const auto& s:r.skipped)
    skipped.insert(s.first);
  uint64_t before = 0, after = 0;
  size_t split = 0;
  Check k;
  for(const auto& [name,file]:index) {
    const auto s = readBaseMesh(base/"meshes"/file);
    before += triangles(s);
    if(r.level==0 || skipped.count(name)!=0) {
      // Byte-equal to the source: the caller's manifest comparison checks it.
      after += triangles(s);
      continue;
      }
    // The game's reader (resources.cpp, Resources::loadMesh path for base meshes).
    const auto c = readBaseMesh(copy/"meshes"/file);
    verifyMesh(s,c,r.level,"mesh "+name+": ",k);
    after += triangles(c);
    ++split;
    }
  require(before==r.trianglesBefore && after==r.trianglesAfter && split==r.split,
          "stress.json: mesh counts differ from the meshes ("+std::to_string(split)+" split, "+std::to_string(before)+
          " -> "+std::to_string(after)+" triangles)");
  std::cout << "meshes: " << split << " of " << index.size() << " split (level " << r.level << "), " << before << " -> "
            << after << " triangles, " << skipped.size() << " skipped and byte-equal to the source; "
            << k.submeshes << " submeshes: material, alpha_test and bbox equal, area error worst "
            << std::scientific << std::setprecision(2) << k.worst << " (" << k.byRounding
            << " over 1e-5 within the rounding bound, worst " << std::fixed << k.worstShare*100 << "% of it)"
            << std::defaultfloat << ", source degenerate " << k.zero << " and slivers " << k.slivers
            << " (degenerate or turned children " << k.sliverChildren << ", area " << std::fixed << std::setprecision(2)
            << k.sliverS << " -> " << k.sliverC << " cm2, not in the area sums)" << std::defaultfloat << std::endl;
  }

void selfTest(const fs::path& dir) {
  asset::StaticMesh m;
  asset::Material stone, wood;
  stone.name = "STRESS_STONE";
  stone.texture = "STONE.TGA";
  wood.name = "STRESS_WOOD";
  wood.texture = "WOOD.TGA";
  wood.alpha_func = asset::AlphaFunction::BLEND;
  m.positions = {{0,10,0},{200,10,0},{0,30,100},{200,30,100},{400,10,0},{400,30,100}};
  m.alpha_test = 0;
  m.bbox = {{0,10,0},{400,30,100}};
  // Submesh 0: two triangles over the shared edge 1-2, the same wedges on both sides.
  asset::SubMesh a;
  a.mat = stone;
  a.wedges = {{{0,1,0},{0,0},0},{{0,1,0},{1,0},1},{{0,0.5f,0.5f},{0,1},2},{{0,0.5f,0.5f},{1,1},3}};
  a.triangles = {{{0,1,2}},{{2,1,3}}};
  // Submesh 1: its own wedges, the edge 1-3 shares positions with submesh 0.
  asset::SubMesh b;
  b.mat = wood;
  b.wedges = {{{0,1,0},{0,0},1},{{0,1,0},{1,0},4},{{0,0,1},{0,1},3},{{0,0,1},{1,1},5}};
  b.triangles = {{{0,1,2}},{{2,1,3}}};
  m.sub_meshes = {a,b};
  m.materials = {stone,wood};

  std::string reason;
  const auto s = subdivide(m,&reason);
  require(s.has_value(),"stress mesh split: refused: "+reason);
  require(triangles(*s)==16 && s->sub_meshes.size()==2 && s->sub_meshes[0].triangles.size()==8 &&
          s->sub_meshes[1].triangles.size()==8,"stress mesh split: 4 triangles must give 16, 8 per submesh");
  // Positions: edges 01 12 20 13 32 of submesh 0, then 14 43 35 54 of submesh 1 (13 is shared): 6+9.
  require(s->positions.size()==15,"stress mesh split: "+std::to_string(s->positions.size())+
          " positions, the shared edges must have one midpoint each");
  // Submesh 0 wedges: 4 + 5 midpoints; triangle 0's child 1 (3,1,4) and triangle 1's child 0 (0,3,5)
  // both hold the midpoint of 1-2.
  const auto& q = s->sub_meshes[0];
  require(q.wedges.size()==9 && s->sub_meshes[1].wedges.size()==9,"stress mesh split: one midpoint wedge per shared edge");
  require(q.triangles[1].wedges[2]==q.triangles[4].wedges[1],"stress mesh split: both triangles must use one midpoint wedge");
  const auto& mw = q.wedges[q.triangles[1].wedges[2]];
  require(s->positions[mw.index]==asset::Vec3(100,20,50) && mw.texture==asset::Vec2(0.5f,0.5f) &&
          mw.normal==asset::Vec3(0,0.75f,0.25f),"stress mesh split: midpoint is not the average of the ends");
  // Submesh 1's midpoint of positions 1-3 is the same position as submesh 0's.
  const auto& r = s->sub_meshes[1];
  require(r.wedges[r.triangles[0].wedges[2]].index==q.wedges[q.triangles[5].wedges[2]].index,
          "stress mesh split: a position pair shared by two submeshes must have one midpoint");
  for(size_t i=0; i<2; ++i) {
    require(gothicglb::sameMaterial(s->sub_meshes[i].mat,m.sub_meshes[i].mat),"stress mesh split: submesh fields changed");
    for(size_t t=0; t<2; ++t) {
      double sum = 0;
      const auto np = normal2(m,m.sub_meshes[i],t);
      for(size_t c=0; c<4; ++c) {
        const auto n = normal2(*s,s->sub_meshes[i],t*4+c);
        require(dot(n,np)>0,"stress mesh split: a child faces the other way");
        sum += length(n);
        }
      require(relative(sum,length(np))<1e-12,"stress mesh split: area must be preserved");
      }
    }
  require(s->alpha_test==m.alpha_test && s->bbox.min==m.bbox.min && s->bbox.max==m.bbox.max,
          "stress mesh split: alpha_test and bbox must stay");

  // The importer's writer and the game's reader take it as it is; the verification accepts it.
  fs::create_directories(dir);
  const auto file = dir/"stress-mesh.glb";
  writeBaseMesh(*s,"STRESS_MESH",file);
  const auto back = readBaseMesh(file);
  const auto diff = compareBaseMeshes(*s,back);
  require(diff.empty(),"stress mesh round trip: "+diff);
  Check k;
  verifyMesh(m,back,1,"stress mesh: ",k);
  fs::remove(file);

  // Over the format: 65535 positions and one triangle give 65538.
  asset::StaticMesh big;
  big.positions.assign(maxPositions-1,asset::Vec3{0,0,0});
  big.positions[0] = {0,0,0};
  big.positions[1] = {1,0,0};
  big.positions[2] = {0,1,0};
  asset::SubMesh one;
  one.mat = stone;
  one.wedges = {{{0,0,1},{0,0},0},{{0,0,1},{1,0},1},{{0,0,1},{0,1},2}};
  one.triangles = {{{0,1,2}}};
  big.sub_meshes = {one};
  reason.clear();
  require(!subdivide(big,&reason) && reason.find("65538 positions > 65536")!=std::string::npos,
          "stress mesh split: a mesh over the position limit must be refused with its reason, got '"+reason+"'");
  }

}
