// Offline vob inventory. Only resource names and aggregate numbers leave the archives.
// Geometry and traversal are copied from content/world-textures.cpp deliberately:
// this tool stays independent of the texture inventory and never exports mesh data.
#include <zenkit/Logger.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/VirtualObject.hh>
#include <zenkit/world/WayNet.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr std::array worlds = {"NEWWORLD.ZEN", "OLDWORLD.ZEN", "ADDONWORLD.ZEN", "DRAGONISLAND.ZEN"};
constexpr std::array<uint64_t,4> textureVobs = {11546,4438,4718,499};
constexpr std::array textureAreas = {2533385.016,1781506.359,1316101.720,13138.775};
const std::set<std::string> modifiers = {
  "BIG","SMALL","LARGE","HIGH","LOW","LONG","SHORT","HALF","BROKEN","DEFECT",
  "NORMAL","POOR","RICH","OPEN","CLOSED","LEFT","RIGHT","L","R","NEW","OLD",
  "IN","OUT","TOP","MIDDLE","CORNER","PART","GROUND","INDOOR","M","MS","ML","BL","BR","TL","TR",
  "DESTROYED","FILLED","EMPTY","BENT","BEND","HUGE","STRAIGHT","ROUND","RED","WHITE","NICE",
  "HEAVY","ALONE","VERYHIGH","BIGHIGH","BIGFLAT","XL","MED","MEDIUM","PEASANT","WAR","MIS"
};
// First matching visual glob wins. These names need context beyond a trailing token.
const std::vector<std::pair<std::string,std::string>> familyOverrides = {
  {"NW_CITY_WINDOW_*","WINDOW"},
  {"NW_CITY_BEAMS_*","BEAMS"},
  {"NW_CITY_SIGN_*","SIGN"},
  {"NW_CITY_MARKETSTALL_*","MARKETSTALL"},
  {"NW_NATURE_STONE_CULT_TOP_*","STONE"},
  {"NW_NATURE_STONE_CULT_STAND_*","STONE"},
  {"NW_NATURE_STONE_CULT_PART_*","STONE"},
  {"NW_NATURE_STONE_CULT_SCENE_*","STONE"},
  {"OC_DECORATE_V?","DECORATION"},
  {"NC_LOB_NICE*","DECORATION"},
  {"ORC_E_DEKO_*","DECORATION"},
  {"NW_MISC_*_STANDY","DISPLAY"},
  {"OC_BARRELS_REIHE","BARRELROW"},
  {"OC_BARRELS_STRUBBELIG","BARRELPILE"},
  {"NW_MISC_STRAW_GROUP_*","STRAWGROUP"},
  {"NW_NATURE_GREAT_WEED_XGROUP","WEEDGROUP"},
  {"OW_LOB_BUSH_WATER_*","BUSH"},
  {"ADDON_STONES_CRYSTAL_ROSE_*","CRYSTAL"},
  {"OW_SHIPWRECK_*","SHIPWRECK"},
  {"NW_DRAGONISLE_TORTURE_*","TORTUREDEVICE"},
  {"EVT_SPAWN_*","SPAWNPOINT"},
  {"NW_MISC_INNOS_GROUND_*","STATUE"},
  {"NW_MISC_DECO_INNOS_*","STATUE"},
  {"NW_HARBOUR_IRONROPEHOLDER_WROPE_*","IRONROPEHOLDER"},
  {"DT_SKELETON_V??_HEAD","SKULL"},
  {"NW_DRAGONISLE_BIGDOOR_HEAD_*","BIGDOOR"},
  {"OC_LOB_BOWTRAIN","TRAININGTARGET"}
};
// Only explicit equivalent nouns, never automatic singularization or compound splitting.
const std::map<std::string,std::string> familyAliases = {
  {"BUSHES","BUSH"},{"CRATEBROKEN","CRATE"},{"HOHETANNEN","HOHETANNE"},
  {"CACTUSBIG","CACTUS"},{"CACTUSLONG","CACTUS"},
  {"FISHINGNETBIG","FISHINGNET"},{"FISHINGNETSMALL","FISHINGNET"}
};
const std::set<std::string> natureFamilies = {
  "BAUMSTAMM","BAUMSTUMPF","BIGTREE","BLAETTERDACH","BROKENSTONE","BUSH","CACTUS","CAVEWEBS",
  "COUPLESTONES","CRYSTAL","DEADTREE","DUCKWEED","FARN","FARNTEPPICH","GRASSGROUP","HANGPLANTS",
  "HOHETANNE","JUNGLETREE","LIANA","MANYSTONES","MUSHROOM","OLDTREE","PALM","PLANT","REED",
  "ROOT","ROOTS","SIDEPLANT","SMALLPALM","SMALLTREE","SMALLWEED","SPIDERWEBS","STALAG","STONE",
  "TANNE","THINTREE","TREE","TREEGROUP","WALLFERN","WATERGRASS","WATERLILI","WEEDGROUP","WOODPIECE"
};

std::string kind(const std::string& family) { return natureFamilies.contains(family) ? "nature" : "object"; }

void require(bool condition,const std::string& message) {
  if(!condition) throw std::runtime_error(message);
}

std::string upper(std::string s) {
  for(auto& c:s) c=char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string basename(std::string s) {
  std::replace(s.begin(),s.end(),'\\','/');
  return upper(fs::path(s).filename().string());
}

std::vector<std::string> tokens(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream input(s);
  std::string token;
  while(std::getline(input,token,'_')) if(!token.empty()) out.push_back(token);
  return out;
}

std::string join(const std::vector<std::string>& values,const std::string& sep) {
  std::string out;
  for(const auto& s:values) { if(!out.empty()) out+=sep; out+=s; }
  return out;
}

bool digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(),s.end(),[](unsigned char c) { return std::isdigit(c); });
}

bool variant(const std::string& s) {
  if(digits(s) || (s.size()==1 && std::isalpha(static_cast<unsigned char>(s[0])))) return true;
  if(s.size()>1 && s[0]=='V' && digits(s.substr(1))) return true;
  return s.size()>1 && std::isalpha(static_cast<unsigned char>(s.back())) && digits(s.substr(0,s.size()-1));
}

bool glob(std::string pattern,std::string value) {
  pattern=upper(pattern); value=upper(value);
  size_t p=0,v=0,star=std::string::npos,retry=0;
  while(v<value.size()) {
    if(p<pattern.size() && (pattern[p]=='?' || pattern[p]==value[v])) { ++p; ++v; }
    else if(p<pattern.size() && pattern[p]=='*') { star=p++; retry=v; }
    else if(star!=std::string::npos) { p=star+1; v=++retry; }
    else return false;
  }
  while(p<pattern.size() && pattern[p]=='*') ++p;
  return p==pattern.size();
}

bool sizeOrPolygons(const std::string& token) {
  static const std::regex code("([0-9]+X[0-9]+(X[0-9]+)?M?)|([0-9]+PP?)");
  return std::regex_match(token,code);
}

std::string family(const std::string& visual) {
  const auto name=fs::path(basename(visual)).stem().string();
  for(const auto& [pattern,result]:familyOverrides) if(glob(pattern,name)) return result;
  auto parts=tokens(name);
  while(!parts.empty()) {
    auto& token=parts.back();
    if(variant(token) || sizeOrPolygons(token) || modifiers.contains(token)) { parts.pop_back(); continue; }
    // Attached suffixes can expose another modifier: M02 -> M, HUGE3 -> HUGE.
    while(!token.empty() && std::isdigit(static_cast<unsigned char>(token.back()))) token.pop_back();
    if(variant(token) || modifiers.contains(token)) { parts.pop_back(); continue; }
    const auto alias=familyAliases.find(token);
    return alias==familyAliases.end() ? token : alias->second;
  }
  return name;
}

std::vector<std::string> readGlobs(const fs::path& path) {
  std::ifstream input(path);
  require(bool(input),"cannot open "+path.string());
  std::vector<std::string> out;
  std::string line;
  while(std::getline(input,line)) {
    const auto first=line.find_first_not_of(" \t\r");
    if(first==std::string::npos || line[first]=='#') continue;
    auto pattern=line.substr(first,line.find(',',first)-first);
    pattern.erase(pattern.find_last_not_of(" \t\r")+1);
    out.push_back(upper(pattern));
  }
  require(input.eof(),"cannot read "+path.string());
  return out;
}

bool matches(const std::vector<std::string>& patterns,const std::string& name) {
  return std::any_of(patterns.begin(),patterns.end(),[&](const auto& p) { return glob(p,name); });
}

struct Point {
  double x=0,y=0,z=0;
  Point()=default;
  Point(double x,double y,double z):x(x),y(y),z(z) {}
  explicit Point(zenkit::Vec3 p):x(p.x),y(p.y),z(p.z) {}
  Point operator-(Point b) const { return {x-b.x,y-b.y,z-b.z}; }
};

Point transform(Point p,const zenkit::VirtualObject* v) {
  if(!v) return p;
  const auto& r=v->rotation.columns;
  // Nested ZEN vobs already store object-to-world transforms; do not compose parents.
  return {r[0].x*p.x+r[1].x*p.y+r[2].x*p.z+v->position.x,
          r[0].y*p.x+r[1].y*p.y+r[2].y*p.z+v->position.y,
          r[0].z*p.x+r[1].z*p.y+r[2].z*p.z+v->position.z};
}

double area(Point a,Point b,Point c) {
  const auto u=b-a,v=c-a;
  return 0.5*std::hypot(u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x)/10000.0;
}

using Triangle=std::array<Point,3>;
using Geometry=std::vector<Triangle>;

Triangle meshTriangle(const zenkit::Mesh& m,size_t a,size_t b,size_t c,
                      const std::vector<uint32_t>& vertices) {
  return {Point(m.vertices.at(vertices.at(a))),Point(m.vertices.at(vertices.at(b))),
          Point(m.vertices.at(vertices.at(c)))};
}

Geometry basicGeometry(const zenkit::Mesh& m) {
  Geometry out;
  // Mesh::load without a BSP list does not populate m.polygons. Triangulate raw fans.
  for(const auto& p:m.geometry) {
    if(p.flags.is_portal || p.flags.is_ghost_occluder || p.flags.is_outdoor) continue;
    for(size_t k=2;k<p.index_count;++k)
      out.push_back(meshTriangle(m,p.index_offset,p.index_offset+k-1,p.index_offset+k,m.polygon_vertex_indices));
  }
  return out;
}

Point bounds(const std::vector<zenkit::Vec3>& positions) {
  require(!positions.empty(),"mesh has no positions");
  Point lo(positions.front()),hi=lo;
  for(auto p:positions) {
    require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),"non-finite mesh position");
    lo.x=std::min(lo.x,double(p.x)); lo.y=std::min(lo.y,double(p.y)); lo.z=std::min(lo.z,double(p.z));
    hi.x=std::max(hi.x,double(p.x)); hi.y=std::max(hi.y,double(p.y)); hi.z=std::max(hi.z,double(p.z));
  }
  return {(hi.x-lo.x)/100,(hi.y-lo.y)/100,(hi.z-lo.z)/100};
}

double geometryArea(const Geometry& g,const zenkit::VirtualObject* v=nullptr) {
  double sum=0;
  for(const auto& t:g) {
    const double a=area(transform(t[0],v),transform(t[1],v),transform(t[2],v));
    require(std::isfinite(a),"non-finite surface area");
    sum+=a;
  }
  return sum;
}

struct Mesh {
  Geometry geometry;
  Point bbox;
  double localArea=0;
  std::set<std::string> textures;
  std::string format,family;
  bool movable=false,destructible=false;
};
using Meshes=std::map<std::string,Mesh>;

Mesh staticGeometry(const zenkit::VfsNode& file,bool msh) {
  auto input=file.open_read();
  Mesh out;
  out.format=msh ? "MSH" : "MRM";
  if(msh) {
    zenkit::Mesh m;
    m.load(input.get(),false);
    out.geometry=basicGeometry(m); out.bbox=bounds(m.vertices);
    for(const auto& mat:m.materials) if(!mat.texture.empty()) out.textures.insert(basename(mat.texture));
  } else {
    zenkit::MultiResolutionMesh m;
    m.load(input.get());
    out.bbox=bounds(m.positions);
    for(const auto& mat:m.materials) if(!mat.texture.empty()) out.textures.insert(basename(mat.texture));
    for(const auto& sub:m.sub_meshes) {
      for(const auto& face:sub.triangles) {
        Triangle t;
        for(size_t k=0;k<3;++k) t[k]=Point(m.positions.at(sub.wedges.at(face.wedges[k]).index));
        out.geometry.push_back(t);
      }
    }
  }
  out.localArea=geometryArea(out.geometry);
  return out;
}

std::string areaName(const std::string& waypoint) {
  auto parts=tokens(upper(waypoint));
  while(!parts.empty() && digits(parts.back())) parts.pop_back();
  if(parts.size()>3) parts.resize(3);
  return parts.empty() ? "<NONE>" : join(parts,"_");
}

std::string nearestArea(const zenkit::WayNet& net,zenkit::Vec3 position) {
  double best=3000.0*3000.0; // 30 m, using full 3D distance from the vob origin.
  std::string name;
  for(const auto& p:net.points) {
    if(!p || p->free_point) continue;
    const auto d=Point(p->position)-Point(position);
    const double distance=d.x*d.x+d.y*d.y+d.z*d.z;
    if(distance<best || (distance==best && (name.empty() || upper(p->name)<name))) {
      best=distance; name=upper(p->name);
    }
  }
  return name.empty() ? "<NONE>" : areaName(name);
}

struct Stats {
  std::array<uint64_t,4> counts{};
  uint64_t items=0;
  double area=0;
  uint64_t instances() const { return std::accumulate(counts.begin(),counts.end(),uint64_t(0)); }
  void merge(const Stats& s) {
    for(size_t i=0;i<4;++i) counts[i]+=s.counts[i];
    items+=s.items; area+=s.area;
  }
};
using Visuals=std::map<std::string,Stats>;
using Rows=std::vector<std::pair<std::string,const Stats*>>;

Rows ranked(const Visuals& stats,bool byArea=false) {
  Rows rows;
  for(const auto& [name,s]:stats) rows.push_back({name,&s});
  std::sort(rows.begin(),rows.end(),[=](const auto& a,const auto& b) {
    if(byArea && a.second->area!=b.second->area) return a.second->area>b.second->area;
    if(a.second->instances()!=b.second->instances()) return a.second->instances()>b.second->instances();
    if(a.second->area!=b.second->area) return a.second->area>b.second->area;
    return a.first<b.first;
  });
  return rows;
}

double fraction(double value,double total) { return total==0 ? 0 : value/total; }

struct Coverage { size_t count=0; double value=0; };
Coverage coverage(const Visuals& stats,double target,bool byArea=false) {
  double total=0;
  for(const auto& [name,s]:stats) total+=byArea ? s.area : double(s.instances());
  Coverage result;
  if(total==0) return result;
  for(const auto& [name,s]:ranked(stats,byArea)) {
    result.value+=byArea ? s->area : double(s->instances()); ++result.count;
    if(result.value>=target*total) break;
  }
  return result;
}

std::vector<std::array<double,2>> cumulative(const Visuals& stats) {
  double count=0,area=0,c=0,a=0;
  for(const auto& [name,s]:stats) { count+=s.instances(); area+=s.area; }
  std::vector<std::array<double,2>> out;
  for(const auto& [name,s]:ranked(stats)) {
    c+=s->instances(); a+=s->area;
    out.push_back({std::clamp(fraction(c,count),0.0,1.0),std::clamp(fraction(a,area),0.0,1.0)});
  }
  return out;
}

struct Inventory {
  std::string world;
  Visuals visuals;
  std::map<std::string,Visuals> areas;
  uint64_t includedVobs=0,hiddenStaticVobs=0,excludedVisuals=0;
  double triangleArea=0;

  void add(const std::string& name,const Mesh& mesh,const zenkit::VirtualObject& v,size_t index,
           const std::string& district) {
    // Independent triangle sum, before assigning the instance to visual or area buckets.
    for(const auto& t:mesh.geometry) triangleArea+=area(transform(t[0],&v),transform(t[1],&v),transform(t[2],&v));
    ++includedVobs;
    Stats s; s.counts[index]=1; s.items=v.type==zenkit::VirtualObjectType::oCItem;
    s.area=geometryArea(mesh.geometry,&v);
    visuals[name].merge(s); areas[district][name].merge(s);
  }

  Stats total() const {
    Stats out;
    for(const auto& [name,s]:visuals) out.merge(s);
    return out;
  }

  double errorPct() const { return 100*fraction(std::abs(total().area-triangleArea),triangleArea); }

  void validate() const {
    require(total().instances()==includedVobs,world+": visual instance total differs from included vobs");
    require(std::isfinite(triangleArea) && triangleArea>0 && errorPct()<=0.1,
            world+": visual areas differ from independent triangle sum by more than 0.1%");
    Stats districts;
    for(const auto& [name,visuals]:areas) for(const auto& [visual,s]:visuals) districts.merge(s);
    require(districts.instances()==includedVobs && std::abs(districts.area-triangleArea)<=0.001*triangleArea,
            world+": area buckets differ from included vobs");
  }
};

Visuals families(const Visuals& visuals,const Meshes& meshes) {
  Visuals out;
  for(const auto& [name,s]:visuals) out[meshes.at(name).family].merge(s);
  return out;
}

Visuals filterKind(const Visuals& visuals,const Meshes& meshes,const std::string& selectedKind) {
  Visuals out;
  for(const auto& [name,s]:visuals) if(kind(meshes.at(name).family)==selectedKind) out.emplace(name,s);
  return out;
}

Stats visualTotal(const Visuals& visuals) {
  Stats out;
  for(const auto& [name,s]:visuals) out.merge(s);
  return out;
}

Visuals coverageVisuals(const Visuals& visuals,double target) {
  Visuals out;
  const auto count=coverage(visuals,target).count;
  const auto rows=ranked(visuals);
  for(size_t i=0;i<count;++i) out.emplace(rows[i].first,*rows[i].second);
  return out;
}

size_t movableVisuals(const Visuals& visuals,const Meshes& meshes) {
  return std::count_if(visuals.begin(),visuals.end(),[&](const auto& row) { return meshes.at(row.first).movable; });
}

std::vector<std::string> familyVisuals(const std::string& family,const Visuals& visuals,const Meshes& meshes) {
  std::vector<std::string> out;
  for(const auto& [name,s]:ranked(visuals)) if(meshes.at(name).family==family) out.push_back(name);
  return out;
}

std::ofstream output(const fs::path& path) {
  std::ofstream f(path);
  f.exceptions(std::ios::failbit|std::ios::badbit); f.imbue(std::locale::classic());
  f << std::fixed << std::setprecision(9);
  return f;
}

void csvString(std::ostream& f,const std::string& value) {
  f << '"';
  for(char c:value) { if(c=='"') f << '"'; f << c; }
  f << '"';
}

std::string textureNames(const Mesh& mesh) { return join({mesh.textures.begin(),mesh.textures.end()},";"); }

void writeCsv(const fs::path& path,const Inventory& inv,const Inventory& all,const Meshes& meshes) {
  auto f=output(path);
  f << "world,visual,format,instances,instances_newworld,instances_oldworld,instances_addonworld,"
       "instances_dragonisland,instances_total,item_instances,triangles,bbox_x_m,bbox_y_m,bbox_z_m,"
       "area_per_instance_m2,area_total_m2,textures,movable,destructible,family,kind,cum_instances_fraction,cum_area_fraction\n";
  const auto fractions=cumulative(inv.visuals);
  size_t row=0;
  for(const auto& [name,s]:ranked(inv.visuals)) {
    const auto& m=meshes.at(name); const auto& global=all.visuals.at(name);
    csvString(f,inv.world); f << ','; csvString(f,name); f << ',' << m.format << ',' << s->instances();
    for(auto count:global.counts) f << ',' << count;
    f << ',' << global.instances() << ',' << s->items << ',' << m.geometry.size()
      << ',' << m.bbox.x << ',' << m.bbox.y << ',' << m.bbox.z << ',' << m.localArea << ',' << s->area << ',';
    csvString(f,textureNames(m)); f << ',' << m.movable << ',' << m.destructible << ',';
    csvString(f,m.family); f << ',' << kind(m.family) << ',' << fractions[row][0] << ',' << fractions[row][1] << '\n'; ++row;
  }
}

void writeFamilies(const fs::path& path,const Inventory& all,const Meshes& meshes) {
  auto f=output(path);
  f << "family,kind,visuals,instances_newworld,instances_oldworld,instances_addonworld,instances_dragonisland,"
       "instances_total,area_total_m2,movable_visuals,example_visuals\n";
  const auto grouped=families(all.visuals,meshes);
  for(const auto& [name,s]:ranked(grouped)) {
    auto names=familyVisuals(name,all.visuals,meshes);
    const auto movable=std::count_if(names.begin(),names.end(),[&](const auto& n) { return meshes.at(n).movable; });
    csvString(f,name); f << ',' << kind(name) << ',' << names.size();
    for(auto count:s->counts) f << ',' << count;
    f << ',' << s->instances() << ',' << s->area << ',' << movable << ',';
    if(names.size()>5) names.resize(5);
    csvString(f,join(names,";")); f << '\n';
  }
}

std::string topVisuals(const Visuals& visuals) {
  std::vector<std::string> out;
  for(const auto& [name,s]:ranked(visuals)) {
    out.push_back(name+":"+std::to_string(s->instances()));
    if(out.size()==5) break;
  }
  return join(out,";");
}

Rows areaRows(const Inventory& inv,Visuals& totals) {
  for(const auto& [name,visuals]:inv.areas) for(const auto& [visual,s]:visuals) totals[name].merge(s);
  return ranked(totals);
}

uint64_t movableInstances(const Visuals& visuals,const Meshes& meshes) {
  uint64_t out=0;
  for(const auto& [name,s]:visuals) if(meshes.at(name).movable) out+=s.instances();
  return out;
}

void writeAreas(const fs::path& path,const std::vector<Inventory>& inventories,const Meshes& meshes) {
  auto f=output(path);
  f << "world,area,instances,area_m2,visuals,families,visuals_for_80pct_instances,movable_instances,"
       "object_instances,object_visuals,object_visuals_for_80pct,top_visuals\n";
  std::vector<const Inventory*> sorted;
  for(const auto& inv:inventories) sorted.push_back(&inv);
  std::sort(sorted.begin(),sorted.end(),[](auto a,auto b) { return a->world<b->world; });
  for(const auto* inv:sorted) {
    Visuals totals;
    for(const auto& [name,s]:areaRows(*inv,totals)) {
      const auto& visuals=inv->areas.at(name);
      const auto objects=filterKind(visuals,meshes,"object");
      csvString(f,inv->world); f << ','; csvString(f,name);
      f << ',' << s->instances() << ',' << s->area << ',' << visuals.size() << ',' << families(visuals,meshes).size()
        << ',' << coverage(visuals,0.8).count << ',' << movableInstances(visuals,meshes)
        << ',' << visualTotal(objects).instances() << ',' << objects.size() << ',' << coverage(objects,0.8).count << ',';
      csvString(f,topVisuals(visuals)); f << '\n';
    }
  }
}

void familyTable(std::ostream& f,const Inventory& inv,const Meshes& meshes,bool byArea,bool objectsOnly=false) {
  f << "| Семейство | Визуалов | Экземпляров | Площадь, м² | Подвижных визуалов | Примеры |\n"
       "|---|---:|---:|---:|---:|---|\n";
  const auto visuals=objectsOnly ? filterKind(inv.visuals,meshes,"object") : inv.visuals;
  const auto grouped=families(visuals,meshes);
  const auto rows=ranked(grouped,byArea);
  for(size_t i=0;i<std::min(size_t(20),rows.size());++i) {
    const auto& [name,s]=rows[i]; auto names=familyVisuals(name,visuals,meshes);
    const auto movable=std::count_if(names.begin(),names.end(),[&](const auto& n) { return meshes.at(n).movable; });
    f << "| " << name << " | " << names.size() << " | " << s->instances() << " | " << s->area << " | " << movable << " | ";
    if(names.size()>5) names.resize(5);
    f << join(names,"; ") << " |\n";
  }
}

void writeReadme(const fs::path& path,const std::vector<Inventory>& inventories,const Inventory& all,const Meshes& meshes) {
  auto f=output(path); f << std::setprecision(3);
  f << "# Статичные меши vob четырёх миров Gothic II + Ночь Ворона\n\n"
       "Офлайн-перепись для этапа 2 [плана своего контента](../../../docs/own-content.md), 2026-10-20. "
       "В репозитории только имена ресурсов и агрегированные числа; вершины, UV, пиксели и меши не экспортируются.\n\n"
       "Повторить на Mac из корня репозитория:\n\n```sh\nscripts/content-meshes.sh --self-test\nscripts/content-meshes.sh\n"
       "scripts/content-meshes.sh reports/content/2026-10-world-meshes\n```\n\n"
       "`GOTHIC_DIR` задаёт установленную игру (по умолчанию `/Volumes/DevSSD/Dev/games/Gothic II`); "
       "необязательный аргумент — каталог отчёта. Скрипт использует готовую библиотеку ZenKit или собирает только цель "
       "`zenkit`; бинарник — `OpenGothic/build/meshes/world-meshes`. Игра, GPU и модели ИИ не запускаются.\n\n"
       "## Состав и метод\n\n"
       "- Смонтированы `Meshes.vdf`, `Meshes_Addon.vdf`, `Worlds.vdf`, `Worlds_Addon.vdf` именно в этом порядке "
       "с `VfsOverwriteBehavior::ALL`: аддон перекрывает базу независимо от временных меток. Loose-файлы и моды не подключаются.\n"
       "- Обход и геометрия повторяют `world-textures.cpp`: все видимые vob с `.3DS` → `.MRM`, `.MRM` или `.MSH`, "
       "полная детализация. Флаг `vob_static` не фильтрует объекты: входят и лежащие предметы `oCItem`. "
       "Дочерние vob обходятся независимо от родителя. Ключ визуала — basename в верхнем регистре без расширения, как в `meshes.json`.\n"
       "- Меш мира не входит. Исключены NPC, предметы в руках и инвентарях NPC, скрытые визуалы, декали, частицы, "
       "`.MDS`/`.MDL` и другие анимированные/морф-модели; сундуки и двери на `.MDS` сюда не попадают. "
       "Отсутствующий меш, коллизия ключей разных форматов или ошибка ZenKit прерывает прогон.\n"
       "- Площадь — сумма площадей треугольников (см² / 10 000), для каждого экземпляра со своим мировым трансформом. "
       "Вложенные vob уже имеют object-to-world трансформ; родитель повторно не применяется. "
       "Это площадь всех поверхностей, не земли и не видимых пикселей; прозрачность не вычитается. "
       "`.MSH` триангулируется веером с тем же исключением portal/ghost occluder/outdoor, что в переписи текстур.\n"
       "- Локальный bbox вычислен по позициям вершин исходного меша; размеры переведены из см в м. "
       "Локальная площадь и число треугольников относятся к одному экземпляру без трансформа. "
       "Текстуры — непустые имена материалов, basename в верхнем регистре, без повторов, по алфавиту.\n"
       "- `movable` и `destructible` — совпадение ключа визуала с глобом первого столбца "
       "`OpenGothic/data/movable-vobs.csv` и `destructible-vobs.csv` (без регистра, `*` — любая строка, `?` — один символ). "
       "Это признаки таблиц движка, не вывод из имени семейства или типа vob.\n\n"
       "## Семейства и районы\n\n"
       "Сначала применяется первая подходящая строка явной таблицы визуал-глоб → семейство ниже. "
       "Иначе имя без расширения разбивается по `_` и просматривается с хвоста: пропускаются числа и варианты "
       "(`01`, `3`, `01B`, `V1`, `V2`, одиночная буква), коды размера `\\d+X\\d+(X\\d+)?M?`, "
       "коды полигонов `\\d+P`/`\\d+PP` и перечисленные модификаторы. После каждого снятого хвоста правило "
       "повторяется: `STONE_02_BIG_124P` → `STONE`. У токена отрезаются хвостовые цифры, затем снова "
       "проверяется модификатор (`M02` → `M` снимается, `BARREL2` → `BARREL` остаётся). "
       "Первое оставшееся существительное становится семейством с явной нормализацией из второй таблицы; "
       "если ничего не осталось, используется полное имя.\n\n"
       "Полный список модификаторов: ";
  f << '`' << join({modifiers.begin(),modifiers.end()},"`, `") << "`.\n\n"
       "Явная таблица (регистр не учитывается, `*` и `?` — глоб):\n\n"
       "| Визуал-глоб | Семейство |\n|---|---|\n";
  for(const auto& [pattern,result]:familyOverrides) f << "| `" << pattern << "` | " << result << " |\n";
  f << "\nЯвные эквивалентные существительные; автоматического выделения частей слитного слова и "
       "единственного числа нет:\n\n| Остаток имени | Семейство |\n|---|---|\n";
  for(const auto& [name,result]:familyAliases) f << "| " << name << " | " << result << " |\n";
  f << "\n`CRATEPILE` и `BARRELGROUP` сохраняются отдельно: это группы, другая модель. "
       "`STRAWGROUP`, `WEEDGROUP`, `BARRELROW` и `BARRELPILE` также обозначают целые группы. "
       "Для непрозрачных имён `DECORATE`, `NICE`, `DEKO` выбрано общее существительное `DECORATION`, "
       "без предположения о точном виде украшения. По именам просмотрены все " << all.visuals.size()
    << " визуала и все " << families(all.visuals,meshes).size()
    << " получившихся семейств: чисел, размерных кодов, прилагательных и позиций среди семейств нет.\n\n"
       "`kind` определяется **только явным списком семейств nature**: `"
    << join({natureFamilies.begin(),natureFamilies.end()},"`, `")
    << "`. Всё остальное — `object`, в том числе кости, черепа, факелы и заборы; "
       "`NW_NATURE_FENCE_01` имеет `kind=object`. Слово `NATURE` в имени визуала не используется для классификации.\n\n"
       "Каждому учтённому vob назначается ближайший waypoint своего `world.way_net->points`, "
       "исключая `free_point`, в радиусе **30 м** по 3D-расстоянию от позиции vob. На равном расстоянии выбирается "
       "первое имя по алфавиту. При отсутствии точки — `<NONE>`. У имени снимаются хвостовые числовые токены, "
       "после чего остаются **первые три токена** по `_`: гавань `NW_CITY_HABOUR_*` (написание оригинала), "
       "рынок `NW_CITY_MERCHANT_*`, верхний квартал `NW_CITY_UPTOWN_*` и казармы `NW_CITY_KASERN_*` "
       "остаются разными районами. `NW_CITY_TO` объединяет переходы от города. Это приближение по навигационным именам, "
       "не географические границы: большие объекты приписываются по началу координат, не по покрываемой территории.\n\n"
       "## Файлы и столбцы\n\n"
       "[Все миры](all-worlds.csv), [Хоринис](newworld.csv), [Долина рудников](oldworld.csv), "
       "[Яркендар](addonworld.csv), [Остров драконов](dragonisland.csv): строка на визуал, "
       "сортировка по экземплярам в области файла, затем по площади (оба по убыванию), затем по имени.\n\n"
       "- `world`: имя мира или `ALL`; `visual`, `format`: ключ и `MRM`/`MSH` (`.3DS` считается `MRM`).\n"
       "- `instances`, `item_instances`, `area_total_m2`: экземпляры, из них `oCItem`, сумма мировых площадей в области файла. "
       "`instances_newworld`, `instances_oldworld`, `instances_addonworld`, `instances_dragonisland`, `instances_total` "
       "в каждом файле дают глобальные числа этого визуала по четырём мирам.\n"
       "- `triangles`, `bbox_x_m`, `bbox_y_m`, `bbox_z_m`, `area_per_instance_m2`: полная локальная геометрия одного экземпляра. "
       "`textures`: имена через `;`; `movable`, `destructible`: 0/1; `family`: семейство; `kind`: `nature` или `object`.\n"
       "- `cum_instances_fraction`, `cum_area_fraction`: накопленные доли 0–1 в порядке строк, с итогами области файла в знаменателе.\n\n"
       "[families.csv](families.csv): семейство, его `kind`, число разных визуалов, экземпляры по мирам и всего, суммарная площадь, "
       "число подвижных визуалов, до пяти самых частых примеров; сортировка по экземплярам, площади и имени.\n\n"
       "[areas.csv](areas.csv): `world`, `area`, `instances`, `area_m2`, число разных `visuals` и `families`, "
       "`visuals_for_80pct_instances` (минимум визуалов для ≥80 % экземпляров района при сортировке по экземплярам), "
       "`movable_instances`, `object_instances`, `object_visuals`, `object_visuals_for_80pct` (экземпляры и разные визуалы "
       "только `object`, затем минимум этих визуалов для ≥80 % экземпляров `object` района), "
       "`top_visuals` (до пяти `ИМЯ:число` через `;`). Сортировка по имени мира, затем экземплярам, площади и имени района.\n\n"
       "## Сверка сумм\n\n"
       "Независимая сумма по всем треугольникам считается до разнесения по визуалам. Прогон падает, если "
       "сумма экземпляров не равна числу учтённых vob или площадь расходится больше чем на 0,1 %. "
       "Районные суммы проверяются отдельно. Для сравнения взяты опубликованные числа "
       "[переписи текстур](../2026-10-world-textures/README.md), округлённые до 0,001 м².\n\n"
       "| Мир | Учтено vob | Визуалов | Семейств | Сумма CSV, м² | По треугольникам, м² | Ошибка, % | Vob / площадь отчёта текстур | Совпало |\n"
       "|---|---:|---:|---:|---:|---:|---:|---|---|\n";
  bool matched=true;
  for(size_t i=0;i<inventories.size();++i) {
    const auto& inv=inventories[i];
    const bool match=inv.includedVobs==textureVobs[i] && std::abs(inv.triangleArea-textureAreas[i])<0.0005;
    matched=matched && match;
    f << "| " << inv.world << " | " << inv.includedVobs << " | " << inv.visuals.size() << " | "
      << families(inv.visuals,meshes).size() << " | " << inv.total().area << " | " << inv.triangleArea << " | "
      << std::setprecision(9) << inv.errorPct() << std::setprecision(3) << " | " << textureVobs[i] << " / "
      << textureAreas[i] << " | " << (match ? "да" : "нет") << " |\n";
  }
  f << "| Все миры | " << all.includedVobs << " | " << all.visuals.size() << " | " << families(all.visuals,meshes).size()
    << " | " << all.total().area << " | " << all.triangleArea << " | " << std::setprecision(9) << all.errorPct()
    << std::setprecision(3) << " | — | — |\n\n";
  f << (matched ? "Число vob и площадь совпали во всех четырёх мирах в точности опубликованного округления.\n\n" :
                  "Есть расхождение с опубликованным отчётом текстур; требуется выяснение причины.\n\n");
  f << "| Область | Вид | Экземпляров | Доля экземпляров, % | Площадь, м² | Доля площади, % |\n"
       "|---|---|---:|---:|---:|---:|\n";
  for(const auto* inv:{&all,&inventories.front()}) for(const auto& selectedKind:{"nature","object"}) {
    const auto s=visualTotal(filterKind(inv->visuals,meshes,selectedKind));
    f << "| " << (inv==&all ? "Все миры" : "NEWWORLD.ZEN") << " | " << selectedKind << " | " << s.instances()
      << " | " << 100*fraction(s.instances(),inv->includedVobs) << " | " << s.area << " | "
      << 100*fraction(s.area,inv->total().area) << " |\n";
  }
  f << '\n';
  size_t mrm=0,msh=0;
  for(const auto& [name,s]:all.visuals) { if(meshes.at(name).format=="MRM") ++mrm; else ++msh; }
  f << "В этих vob встречаются " << mrm << " визуалов MRM и " << msh << " MSH. Это использованные визуалы четырёх миров, "
       "а не все 1 355 MRM в архивах из плана замены: неиспользованные ресурсы и другие классы визуалов не перечисляются. "
       "Учтённых `oCItem`: " << all.total().items << "; `item_instances` считает только явно заданные подходящие визуалы "
       "vob из ZEN, без восстановления визуалов предметов по скриптам.\n\n";
  f << "| Мир | Скрытых статичных визуалов | Прочих исключённых непустых визуалов |\n|---|---:|---:|\n";
  for(const auto& inv:inventories) f << "| " << inv.world << " | " << inv.hiddenStaticVobs << " | " << inv.excludedVisuals << " |\n";
  f << "\n## Топ-50 визуалов всех миров по экземплярам\n\n"
       "NW / OW / AW / DI — экземпляры в Хоринисе, Долине рудников, Яркендаре и на Острове драконов. "
       "M / D — movable / destructible; bbox и площадь одного экземпляра локальные.\n\n"
       "| Визуал | Семейство | NW | OW | AW | DI | Всего | Треугольников | Bbox X × Y × Z, м | Площадь одного, м² | Всего, м² | M / D |\n"
       "|---|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---|\n";
  const auto rows=ranked(all.visuals);
  for(size_t i=0;i<std::min(size_t(50),rows.size());++i) {
    const auto& [name,s]=rows[i]; const auto& m=meshes.at(name);
    f << "| `" << name << "` | " << m.family;
    for(auto count:s->counts) f << " | " << count;
    f << " | " << s->instances() << " | " << m.geometry.size() << " | " << m.bbox.x << " × " << m.bbox.y << " × " << m.bbox.z
      << " | " << m.localArea << " | " << s->area << " | " << m.movable << " / " << m.destructible << " |\n";
  }
  f << "\n## Топ-20 семейств по экземплярам\n\n"; familyTable(f,all,meshes,false);
  f << "\n## Топ-20 семейств по площади\n\n"; familyTable(f,all,meshes,true);
  f << "\n## Топ-20 семейств object по экземплярам\n\n"; familyTable(f,all,meshes,false,true);
  f << "\n## Топ-20 семейств object по площади\n\n"; familyTable(f,all,meshes,true,true);
  f << "\n## Покрытие\n\n"
       "Число визуалов и семейств до первого достижения ≥50/80/90 % выбранной меры. "
       "Для каждой меры своя сортировка по убыванию; семейства ранжируются по сумме всех своих визуалов.\n\n"
       "| Область | Мера | 50 %: визуалов | 50 %: семейств | 80 %: визуалов | 80 %: семейств | 90 %: визуалов | 90 %: семейств |\n"
       "|---|---|---:|---:|---:|---:|---:|---:|\n";
  for(bool objectsOnly:{false,true}) for(const auto* inv:{&all,&inventories.front()}) for(bool byArea:{false,true}) {
    f << "| " << (inv==&all ? "Все миры" : "NEWWORLD.ZEN") << (objectsOnly ? ", object" : ", все виды")
      << " | " << (byArea ? "Площадь" : "Экземпляры");
    const auto visuals=objectsOnly ? filterKind(inv->visuals,meshes,"object") : inv->visuals;
    const auto grouped=families(visuals,meshes);
    for(double target:{0.5,0.8,0.9}) f << " | " << coverage(visuals,target,byArea).count << " | " << coverage(grouped,target,byArea).count;
    f << " |\n";
  }
  f << "\n## Топ-15 районов NEWWORLD.ZEN по экземплярам\n\n"
       "| Район | Экземпляров | Площадь, м² | Визуалов | Семейств | Визуалов для 80 % | Подвижных экземпляров | Топ визуалов |\n"
       "|---|---:|---:|---:|---:|---:|---:|---|\n";
  const auto& nw=inventories.front(); Visuals totals;
  const auto districts=areaRows(nw,totals);
  for(size_t i=0;i<std::min(size_t(15),districts.size());++i) {
    const auto& [name,s]=districts[i]; const auto& visuals=nw.areas.at(name);
    f << "| `" << name << "` | " << s->instances() << " | " << s->area << " | " << visuals.size() << " | "
      << families(visuals,meshes).size() << " | " << coverage(visuals,0.8).count << " | " << movableInstances(visuals,meshes)
      << " | " << topVisuals(visuals) << " |\n";
  }
  Visuals objectDistrictTotals;
  for(const auto& [name,visuals]:nw.areas) objectDistrictTotals[name]=visualTotal(filterKind(visuals,meshes,"object"));
  const auto objectDistricts=ranked(objectDistrictTotals);
  f << "\n## Топ-15 районов NEWWORLD.ZEN по object_instances\n\n"
       "| Район | Экземпляров object | Площадь object, м² | Визуалов object | Визуалов object для 80 % | Подвижных экземпляров object | Топ визуалов object |\n"
       "|---|---:|---:|---:|---:|---:|---|\n";
  for(size_t i=0;i<std::min(size_t(15),objectDistricts.size());++i) {
    const auto& [name,s]=objectDistricts[i]; const auto objects=filterKind(nw.areas.at(name),meshes,"object");
    f << "| `" << name << "` | " << s->instances() << " | " << s->area << " | " << objects.size()
      << " | " << coverage(objects,0.8).count << " | " << movableInstances(objects,meshes)
      << " | " << topVisuals(objects) << " |\n";
  }
  const auto objects=filterKind(all.visuals,meshes,"object");
  const auto grouped=families(objects,meshes);
  const auto familyRows=ranked(grouped); const auto& first=familyRows.front();
  const auto firstNames=familyVisuals(first.first,objects,meshes);
  const std::set<std::string> nonVegetation={
    "BROKENSTONE","CAVEWEBS","COUPLESTONES","CRYSTAL","MANYSTONES","SPIDERWEBS","STALAG","STONE"
  };
  Stats vegetation;
  for(const auto& [name,s]:filterKind(all.visuals,meshes,"nature"))
    if(!nonVegetation.contains(meshes.at(name).family)) vegetation.merge(s);
  f << "\n## С чего начать\n\n"
    << "Растительность, деревья и грибы дают " << vegetation.instances() << " экземпляров ("
    << 100*fraction(vegetation.instances(),all.includedVobs) << " %) и " << vegetation.area << " м² ("
    << 100*fraction(vegetation.area,all.total().area) << " %) во всех мирах. Сюда включены пни и стволы; "
       "это `nature` без семейств `" << join({nonVegetation.begin(),nonVegetation.end()},"`, `")
    << "`. Полная категория `nature` в сверке включает также камни, кристаллы и паутину. "
       "Растительность требует отдельного вида ручной работы; для первого набора утвари и построек ниже используются только `object`.\n\n"
    << "Первое семейство `object` по экземплярам — **" << first.first << "**: " << first.second->instances()
    << " экземпляров, " << firstNames.size() << " разных визуалов и " << first.second->area << " м². Его визуалы: `"
    << join(firstNames,"`, `") << "`.\n\n"
       "Для первого цельного городского набора сравниваются четыре района. В 80 %-набор входят самые частые визуалы "
       "`object` до первого достижения ≥80 % экземпляров `object` района; доля показана по фактически закрытым экземплярам. "
       "Критерий выбора — наибольшее число закрытых экземпляров на один визуал этого набора.\n\n"
       "| Район | Экземпляров object | Разных визуалов object | Визуалов для 80 % | Закрыто экземпляров | Доля, % | Из выбранных в movable-vobs.csv |\n"
       "|---|---:|---:|---:|---:|---:|---:|\n";
  std::string district; double best=-1;
  for(const auto& name:std::array{"NW_CITY_HABOUR","NW_CITY_MERCHANT","NW_CITY_KANAL","NW_CITY_UPTOWN"}) {
    require(nw.areas.contains(name),std::string("missing city district: ")+name);
    const auto areaObjects=filterKind(nw.areas.at(name),meshes,"object");
    const auto selected=coverageVisuals(areaObjects,0.8); const auto covered=visualTotal(selected).instances();
    const auto count=visualTotal(areaObjects).instances();
    const double efficiency=fraction(covered,double(selected.size()));
    if(efficiency>best || (efficiency==best && name<district)) { best=efficiency; district=name; }
    f << "| `" << name << "` | " << count << " | " << areaObjects.size() << " | " << selected.size()
      << " | " << covered << " | " << 100*fraction(covered,count) << " | " << movableVisuals(selected,meshes) << " |\n";
  }
  const auto areaObjects=filterKind(nw.areas.at(district),meshes,"object");
  const auto selected=coverageVisuals(areaObjects,0.8); const auto covered=visualTotal(selected).instances();
  f << "\nПо этому критерию начинать стоит с **" << district << "**: полный набор содержит " << areaObjects.size()
    << " разных визуалов `object`, а сокращённый — **" << selected.size() << "** и закрывает " << covered
    << " из " << visualTotal(areaObjects).instances() << " экземпляров `object` ("
    << 100*fraction(covered,visualTotal(areaObjects).instances()) << " %), " << best << " экземпляров на визуал. "
    << "Из этих " << selected.size() << " визуалов **" << movableVisuals(selected,meshes)
    << "** уже перечислены в `movable-vobs.csv`. Это вывод из таблиц частот, без решений о геймплее.\n";
}

void selfTest() {
  size_t checks=0;
  auto check=[&](bool ok,const std::string& message) { require(ok,message); ++checks; };
  for(const auto& [name,expected]:std::vector<std::pair<std::string,std::string>>{
      {"NW_HARBOUR_BARREL_01","BARREL"},{"NW_CITY_WEAPON_BARREL_01","BARREL"},{"OC_BARREL_V1","BARREL"},
      {"OC_MOB_TABLE_NORMAL_DEFECT","TABLE"},{"NW_CITY_CUPBOARD_POOR_02","CUPBOARD"},{"MIN_LOB_HANDRAIL_3","HANDRAIL"},
      {"x/barrel2.mrm","BARREL"},{"OC_BARREL_01B","BARREL"},{"OC_BARREL_A","BARREL"},{"01_V2_A","01_V2_A"},
      {"OC_BARREL_V1_DESTROYED","BARREL"},{"NW_CITY_WINDOW_STONE_OUT_01","WINDOW"},
      {"ADDON_VALLEYPLANT_TREE_02_XL_191P","TREE"},{"NW_NATURE_STONE_BIGHIGH_01","STONE"},
      {"ADDON_STONES_STONE_02_BIG_124P","STONE"},{"ADDON_MISC_WOODPILLAR_M02","WOODPILLAR"},
      {"EVT_TPL_METALTHORNS_2X2X05M","METALTHORNS"},{"ADDON_PLANTS_WALLFERN_02_234PP","WALLFERN"},
      {"NW_CITY_TABLE_PEASANT_01","TABLE"},{"NW_CITY_MAP_WAR_OPEN_01","MAP"},
      {"OC_DECORATE_V1","DECORATION"},{"NW_MISC_STRAW_GROUP_01","STRAWGROUP"},
      {"ADDON_CANYONPLANT_BUSHES_02_240P","BUSH"},{"NW_HARBOUR_CRATEBROKEN_01","CRATE"},
      {"NW_NATURE_HOHETANNEN_02_531P","HOHETANNE"},{"NW_HARBOUR_CRATEPILE_01","CRATEPILE"},
      {"NW_HARBOUR_BARRELGROUP_01","BARRELGROUP"},{"NW_NATURE_SIDEPLANT_CORNER_01","SIDEPLANT"},
      {"EVT_MAINHALL_DOOR_LOCK_BL_01","LOCK"},{"OC_LOB_TORCHHOLDER_FILLED","TORCHHOLDER"}})
    check(family(name)==expected,"family: "+name);
  check(sizeOrPolygons("4X4X4M") && sizeOrPolygons("4X4M") && sizeOrPolygons("2X3M") &&
        sizeOrPolygons("234PP") && sizeOrPolygons("191P") && !sizeOrPolygons("SMALLPALM"),"size and polygon codes");
  check(kind(family("NW_NATURE_FENCE_01"))=="object" && kind("BUSH")=="nature" &&
        kind("CRYSTAL")=="nature" && kind("TORCH")=="object" && kind("SKULL")=="object",
        "kind uses explicit family set, not visual prefix");
  check(glob("OC_BARREL_V?","oc_barrel_v1") && !glob("OC_BARREL_V?","OC_BARREL_V12"),"glob question mark and case");
  check(glob("NW_CITY_TABLE_*","nw_city_table_01") && glob("NW_CITY_TABLE_*","NW_CITY_TABLE_") &&
        !glob("NW_CITY_TABLE_*","NW_CITY_CHAIR_01"),"glob star and mismatch");
  check(glob("A*B?C","AXBBYC") && !glob("A*B?C","AXBC"),"glob backtracking");
  zenkit::Mesh quad;
  quad.vertices={{0,0,0},{200,0,0},{200,200,0},{0,200,0}};
  quad.polygon_vertex_indices={0,1,2,3};
  zenkit::Polygon poly{}; poly.index_count=4; quad.geometry.push_back(poly);
  Mesh mesh; mesh.geometry=basicGeometry(quad); mesh.bbox=bounds(quad.vertices); mesh.localArea=geometryArea(mesh.geometry);
  check(mesh.geometry.size()==2 && mesh.localArea==4,"quad fan area and full triangles");
  check(mesh.bbox.x==2 && mesh.bbox.y==2 && mesh.bbox.z==0,"quad local bbox cm to m");
  zenkit::VirtualObject v;
  v.rotation=zenkit::Mat3({0,2,0},{-3,0,0},{0,0,1}); v.position={10000,-123,47};
  check(std::abs(geometryArea(mesh.geometry,&v)-24)<1e-9,"quad rotation translation nonuniform scale");
  Inventory inv; inv.world="TEST";
  inv.add("QUAD",mesh,v,0,"AREA"); inv.add("QUAD",mesh,v,0,"AREA"); inv.validate();
  check(inv.includedVobs==2 && inv.total().instances()==2 && inv.total().area==48 && inv.triangleArea==48,
        "two transformed instances and independent sum");
  quad.geometry[0].flags.is_portal=1;
  check(basicGeometry(quad).empty(),"MSH portal exclusion");
  Visuals small;
  small["A"].counts[0]=6; small["A"].area=1;
  small["B"].counts[0]=3; small["B"].area=3;
  small["C"].counts[0]=1; small["C"].area=6;
  const auto cum=cumulative(small);
  check(cum.size()==3 && cum[0][0]==0.6 && cum[0][1]==0.1 && cum[1][0]==0.9 &&
        cum[1][1]==0.4 && cum.back()[0]==1 && cum.back()[1]==1,"cumulative fractions in instance order");
  check(coverage(small,0.5).count==1 && coverage(small,0.8).count==2 && coverage(small,0.9).count==2,
        "instance coverage thresholds");
  check(coverage(small,0.5,true).count==1 && coverage(small,0.8,true).count==2 &&
        coverage(small,0.9,true).count==2 && ranked(small,true).front().first=="C","area coverage uses own order");
  check(coverage({},0.8).count==0 && cumulative({}).empty(),"empty coverage");
  check(areaName("NW_CITY_HABOUR_01_02")=="NW_CITY_HABOUR" &&
        areaName("NW_CITY_MERCHANT_STREET_03")=="NW_CITY_MERCHANT" &&
        areaName("NW_CITY_UPTOWN_01")=="NW_CITY_UPTOWN" &&
        areaName("NW_CITY_KASERN_01")=="NW_CITY_KASERN","district depth and trailing numeric tokens");
  zenkit::WayNet net;
  auto waypoint=[&](const std::string& name,float x,bool free=false) {
    auto p=std::make_shared<zenkit::WayPoint>(); p->name=name; p->position={x,0,0}; p->free_point=free; net.points.push_back(p);
  };
  waypoint("NW_CITY_MARKET_01",100); waypoint("NW_CITY_HARBOUR_01",10,true);
  check(nearestArea(net,{0,0,0})=="NW_CITY_MARKET","nearest excludes free points");
  check(nearestArea(net,{3100,0,0})=="NW_CITY_MARKET" && nearestArea(net,{3101,0,0})=="<NONE>","district radius inclusive 30m");
  waypoint("NW_CITY_BARRACK_01",100);
  check(nearestArea(net,{0,0,0})=="NW_CITY_BARRACK","nearest waypoint tie by name");
  Meshes testMeshes; testMeshes["A"].family="F"; testMeshes["B"].family="F"; testMeshes["C"].family="G";
  const auto grouped=families(small,testMeshes);
  check(grouped.at("F").instances()==9 && grouped.at("F").area==4 && coverage(grouped,0.8).count==1,
        "family aggregation and coverage");
  testMeshes["A"].family="BARREL"; testMeshes["A"].movable=true;
  testMeshes["B"].family="BUSH"; testMeshes["C"].family="TORCH";
  const auto objects=filterKind(small,testMeshes,"object");
  check(objects.size()==2 && visualTotal(objects).instances()==7 &&
        visualTotal(filterKind(small,testMeshes,"nature")).instances()==3,"kind partitions visuals and instances");
  const auto selected=coverageVisuals(objects,0.8);
  check(selected.size()==1 && visualTotal(selected).instances()==6 && movableVisuals(selected,testMeshes)==1,
        "object 80 percent set and movable visual count");
  std::cout << "self-test: " << checks << " checks passed\n";
}
}

int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--self-test") { selfTest(); return 0; }
    require(argc==4,"usage: world-meshes <game dir> <out dir> <repo root> | --self-test");
    size_t zenkitErrors=0;
    zenkit::Logger::set(zenkit::LogLevel::ERROR,[&](auto,const char* component,const char* message) {
      ++zenkitErrors; std::cerr << "ZenKit " << component << ": " << message << '\n';
    });
    const auto movable=readGlobs(fs::path(argv[3])/"OpenGothic/data/movable-vobs.csv");
    const auto destructible=readGlobs(fs::path(argv[3])/"OpenGothic/data/destructible-vobs.csv");
    std::map<std::string,fs::path> archives;
    for(const auto& e:fs::directory_iterator(fs::path(argv[1])/"Data"))
      if(e.is_regular_file()) archives[upper(e.path().filename().string())]=e.path();
    zenkit::Vfs vfs;
    for(const auto* name:{"MESHES.VDF","MESHES_ADDON.VDF","WORLDS.VDF","WORLDS_ADDON.VDF"}) {
      const auto found=archives.find(name);
      require(found!=archives.end(),std::string("missing archive ")+name);
      vfs.mount_disk(found->second,zenkit::VfsOverwriteBehavior::ALL);
    }
    Meshes meshes;
    std::vector<Inventory> inventories;
    for(size_t index=0;index<worlds.size();++index) {
      const auto* name=worlds[index]; const auto* file=vfs.find(name);
      require(file!=nullptr,std::string("missing world ")+name);
      std::cerr << "Reading " << name << "...\n";
      zenkit::World world;
      auto input=file->open_read(); world.load(input.get(),zenkit::GameVersion::GOTHIC_2);
      require(world.way_net!=nullptr,std::string(name)+": missing way net");
      Inventory inv; inv.world=name;
      std::function<void(const zenkit::VirtualObject&)> visit=[&](const auto& v) {
        if(v.visual && !v.visual->name.empty()) {
          auto visual=basename(v.visual->name);
          const auto ext=fs::path(visual).extension().string();
          const bool eligible=(ext==".3DS" || ext==".MRM" || ext==".MSH") &&
                              v.type!=zenkit::VirtualObjectType::oCNpc &&
                              v.visual->type!=zenkit::VisualType::DECAL &&
                              v.visual->type!=zenkit::VisualType::PARTICLE_EFFECT;
          if(eligible && v.show_visual) {
            if(ext==".3DS") visual=fs::path(visual).replace_extension(".MRM").string();
            const auto key=fs::path(visual).stem().string();
            try {
              auto found=meshes.find(key);
              if(found==meshes.end()) {
                const auto* mesh=vfs.find(visual);
                require(mesh!=nullptr,"missing static mesh "+visual);
                auto data=staticGeometry(*mesh,ext==".MSH");
                data.family=family(key); data.movable=matches(movable,key); data.destructible=matches(destructible,key);
                found=meshes.emplace(key,std::move(data)).first;
              }
              require(found->second.format==(ext==".MSH" ? "MSH" : "MRM"),"different formats share visual key "+key);
              inv.add(key,found->second,v,index,nearestArea(*world.way_net,v.position));
            } catch(const std::exception& e) { throw std::runtime_error(inv.world+": "+visual+": "+e.what()); }
          } else if(eligible) ++inv.hiddenStaticVobs;
          else ++inv.excludedVisuals;
        }
        for(const auto& child:v.children) if(child) visit(*child);
      };
      for(const auto& v:world.world_vobs) if(v) visit(*v);
      inv.validate();
      require(zenkitErrors==0,inv.world+": ZenKit reported "+std::to_string(zenkitErrors)+" errors");
      std::cerr << std::fixed << std::setprecision(6) << name << ": " << inv.includedVobs << " vobs, "
                << inv.visuals.size() << " visuals, " << families(inv.visuals,meshes).size() << " families, "
                << inv.triangleArea << " m2; area mismatch " << inv.errorPct() << " %\n";
      inventories.push_back(std::move(inv));
    }
    Inventory all; all.world="ALL";
    for(const auto& inv:inventories) {
      all.includedVobs+=inv.includedVobs; all.triangleArea+=inv.triangleArea;
      for(const auto& [name,s]:inv.visuals) all.visuals[name].merge(s);
      for(const auto& [district,visuals]:inv.areas)
        for(const auto& [name,s]:visuals) all.areas[inv.world+":"+district][name].merge(s);
    }
    all.validate();
    const fs::path out=argv[2]; fs::create_directories(out);
    for(size_t i=0;i<inventories.size();++i) {
      auto filename=fs::path(worlds[i]).stem().string();
      for(auto& c:filename) c=char(std::tolower(static_cast<unsigned char>(c)));
      writeCsv(out/(filename+".csv"),inventories[i],all,meshes);
    }
    writeCsv(out/"all-worlds.csv",all,all,meshes);
    writeFamilies(out/"families.csv",all,meshes); writeAreas(out/"areas.csv",inventories,meshes);
    writeReadme(out/"README.md",inventories,all,meshes);
    std::cerr << "Wrote 7 CSV files and README.md; " << all.includedVobs << " vobs, " << all.visuals.size()
              << " visuals; ZenKit errors: " << zenkitErrors << '\n';
  } catch(const std::exception& e) {
    std::cerr << "world-meshes: " << e.what() << '\n'; return 1;
  }
}
