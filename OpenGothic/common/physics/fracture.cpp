#include "fracture.h"
#include "fracturedata.h"
#include "resources.h"
#include "utils/userhome.h"

#include <Jolt/Geometry/ConvexSupport.h>
#include <Jolt/Geometry/GJKClosestPoint.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/ConvexShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Tempest/Log>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_set>

namespace {
std::string basename(std::string_view visual) {
  const auto slash=visual.find_last_of("/\\");
  if(slash!=std::string_view::npos) visual.remove_prefix(slash+1);
  const auto dot=visual.find_last_of('.');
  if(dot!=std::string_view::npos) visual=visual.substr(0,dot);
  std::string value(visual);
  std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  return value;
  }

bool matches(std::string_view pattern,std::string_view text) {
  size_t p=0,t=0,star=std::string_view::npos,retry=0;
  while(t<text.size()) {
    if(p<pattern.size() && (pattern[p]=='?' || pattern[p]==text[t])) { ++p; ++t; }
    else if(p<pattern.size() && pattern[p]=='*') { star=p++; retry=t; }
    else if(star!=std::string_view::npos) { p=star+1; t=++retry; }
    else return false;
    }
  while(p<pattern.size() && pattern[p]=='*') ++p;
  return p==pattern.size();
  }

bool finite(const FractureMesh::Vertex& v, bool legacyBounds) {
  return std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z) &&
         std::isfinite(v.normal.x) && std::isfinite(v.normal.y) && std::isfinite(v.normal.z) &&
         std::isfinite(v.uv.x) && std::isfinite(v.uv.y) && (!legacyBounds || v.position.quadLength()<1.e12f);
  }

bool materialGroup(std::string_view name, asset::MaterialGroup& group) {
  if(name=="UNDEF") group=asset::MaterialGroup::UNDEFINED;
  else if(name=="WOOD") group=asset::MaterialGroup::WOOD;
  else if(name=="METAL") group=asset::MaterialGroup::METAL;
  else if(name=="STONE") group=asset::MaterialGroup::STONE;
  else if(name=="CLAY") group=asset::MaterialGroup::CLAY;
  else if(name=="EARTH") group=asset::MaterialGroup::EARTH;
  else if(name=="WATER") group=asset::MaterialGroup::WATER;
  else if(name=="SNOW") group=asset::MaterialGroup::SNOW;
  else return false;
  return true;
  }

bool bondKind(std::string_view name, FractureMesh::BondKind& kind) {
  using K=FractureMesh::BondKind;
  if(name=="NAIL") kind=K::Nail;
  else if(name=="HOOP") kind=K::Hoop;
  else if(name=="GROOVE") kind=K::Groove;
  else if(name=="DOWEL") kind=K::Dowel;
  else if(name=="ALONG") kind=K::Along;
  else if(name=="ACROSS") kind=K::Across;
  else return false;
  return true;
  }

// The optional format 3 block after the pieces: `bonds N`, then N lines `bond A B KIND x y z`.
bool readBonds(std::istream& file, FractureMesh& mesh, std::string& error) {
  constexpr size_t MaxBonds=1024;
  size_t count=0;
  if(!(file>>count)) { error="expected 'bonds N'"; return false; }
  if(count>MaxBonds) { error="bonds "+std::to_string(count)+" exceed "+std::to_string(MaxBonds); return false; }
  std::set<std::pair<uint32_t,uint32_t>> pairs;
  const size_t pieces=mesh.pieces.size();
  for(size_t i=0;i<count;++i) {
    std::string tag,kind;
    long long a=-1,b=-1;
    float x=0,y=0,z=0;
    const auto where="bond "+std::to_string(i)+": ";
    if(!(file>>tag>>a>>b>>kind>>x>>y>>z) || tag!="bond") {
      error=where+"expected 'bond A B KIND x y z'";
      return false;
      }
    if(a<0 || b<0 || size_t(a)>=pieces || size_t(b)>=pieces) {
      error=where+"piece "+std::to_string(a)+" or "+std::to_string(b)+" outside 0.."+std::to_string(pieces-1);
      return false;
      }
    if(a>=b) { error=where+"A "+std::to_string(a)+" must be less than B "+std::to_string(b); return false; }
    FractureMesh::Bond bond;
    if(!bondKind(kind,bond.kind)) { error=where+"unknown kind '"+kind+"'"; return false; }
    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) { error=where+"point is not finite"; return false; }
    bond.a=uint32_t(a);
    bond.b=uint32_t(b);
    bond.point=Tempest::Vec3(x,y,z);
    if(!pairs.emplace(bond.a,bond.b).second) {
      error=where+"pair "+std::to_string(a)+" "+std::to_string(b)+" repeats";
      return false;
      }
    mesh.bonds.push_back(bond);
    }
  return true;
  }

// Distance from a point to the nearest convex hull of a piece, centimetres; 0 inside.
float hullDistanceCm(const FractureMesh::Piece& piece, JPH::Vec3Arg point) {
  float best=FLT_MAX;
  for(const auto& hull:piece.hulls) {
    // Format 3 hulls are ConvexHullShapes (Jolt is built without RTTI).
    const auto& convex=static_cast<const JPH::ConvexShape&>(*hull);
    JPH::ConvexShape::SupportBuffer buffer;
    const auto* support=convex.GetSupportFunction(JPH::ConvexShape::ESupportMode::IncludeConvexRadius,buffer,JPH::Vec3::sOne());
    // Support points are relative to the hull's centre of mass.
    const JPH::PointConvexSupport target{point-convex.GetCenterOfMass()};
    JPH::GJKClosestPoint gjk;
    JPH::Vec3 axis=JPH::Vec3::sAxisX(),onHull,onPoint;
    const float squared=gjk.GetClosestPoints(*support,target,1.e-4f,FLT_MAX,axis,onHull,onPoint);
    best=std::min(best,std::sqrt(std::max(squared,0.f))*100.f);
    }
  return best;
  }
}

bool fractureMaterialsCollide(asset::MaterialGroup a, asset::MaterialGroup b) {
  using G=asset::MaterialGroup;
  return !((a==G::METAL && b==G::WOOD) || (a==G::WOOD && b==G::METAL));
  }

JPH::Ref<JPH::GroupFilter> fractureGroupFilter(const FractureMesh& mesh) {
  std::vector<uint32_t> single(mesh.pieces.size());
  std::iota(single.begin(),single.end(),0u);
  return fractureGroupFilter(mesh,single,uint32_t(single.size()));
  }

JPH::Ref<JPH::GroupFilter> fractureGroupFilter(const FractureMesh& mesh, const std::vector<uint32_t>& pieceGroup,
                                               uint32_t groups) {
  using G=asset::MaterialGroup;
  // A native table (Jolt is built without RTTI, so its filter cannot be subclassed here).
  // Other breaks and ragdolls carry other group IDs and collide with every piece.
  std::vector<uint8_t> metalOnly(groups,1),wood(groups,0);
  for(size_t i=0;i<mesh.pieces.size() && i<pieceGroup.size();++i) {
    const auto g=pieceGroup[i];
    if(g>=groups) continue;
    if(mesh.pieces[i].group!=G::METAL) metalOnly[g]=0;
    if(mesh.pieces[i].group==G::WOOD) wood[g]=1;
    }
  JPH::Ref<JPH::GroupFilterTable> table=new JPH::GroupFilterTable(groups);
  for(uint32_t i=0;i<groups;++i)
    for(uint32_t j=i+1;j<groups;++j)
      if((metalOnly[i] && wood[j]) || (metalOnly[j] && wood[i]))
        table->DisableCollision(i,j);
  return JPH::Ref<JPH::GroupFilter>(table.GetPtr());
  }

FractureBonds FractureBonds::load() {
  FractureBonds rule;
  const auto* env=std::getenv("OPENGOTHIC_FRACTURE_BONDS");
  if(env==nullptr || env[0]=='\0') return rule;
  const auto invalid=[](const std::string& entry, const char* reason) {
    Tempest::Log::e("Invalid OPENGOTHIC_FRACTURE_BONDS entry '",entry,"': ",reason,"; using defaults");
    return FractureBonds{};
    };
  std::istringstream entries(env);
  for(std::string entry;std::getline(entries,entry,',');) {
    std::istringstream field(entry);
    field.imbue(std::locale::classic());
    std::string key;
    float value=0;
    if(!std::getline(field,key,'=') || !(field>>value) || !(field>>std::ws).eof() ||
       !std::isfinite(value) || value<0.f)
      return invalid(entry,"expected key=value with a finite non-negative number");
    const auto first=key.find_first_not_of(" \t\r\n\f\v");
    if(first==std::string::npos) return invalid(entry,"missing key");
    key=key.substr(first,key.find_last_not_of(" \t\r\n\f\v")-first+1);
    static constexpr const char* radius[]={"fist","one","two","arrow","impact"};
    static constexpr const char* strength[]={"nail","hoop","groove","dowel","along","across"};
    bool known=false;
    for(size_t i=0;i<std::size(radius);++i)
      if(key==radius[i]) { rule.radius[i]=value; known=true; }
    for(size_t i=0;i<std::size(strength);++i)
      if(key==strength[i]) {
        if(value==0.f) return invalid(entry,"a bond strength must be greater than zero");
        rule.strength[i]=value;
        known=true;
        }
    if(key=="ring") {
      if(value>1.f) return invalid(entry,"ring is a share from 0 to 1");
      rule.ring=value;
      known=true;
      }
    if(key=="remnant") {
      rule.remnant=value;
      known=true;
      }
    if(!known) return invalid(entry,"unknown key");
    }
  return rule;
  }

void fractureCut(const FractureMesh& mesh, const FractureBonds& rule, FractureHit hit, const Tempest::Vec3& point,
                 const std::vector<uint32_t>& members, std::vector<bool>& broken) {
  const size_t count=mesh.pieces.size();
  broken.resize(mesh.bonds.size(),false);
  std::vector<uint8_t> member(count,0);
  for(const auto i:members)
    if(i<count) member[i]=1;
  // Only bonds still intact inside this remnant take the blow.
  const auto inside=[&](const FractureMesh::Bond& bond, size_t k) { return !broken[k] && member[bond.a] && member[bond.b]; };
  if(hit==FractureHit::Spell) {
    for(size_t k=0;k<mesh.bonds.size();++k)
      if(inside(mesh.bonds[k],k)) broken[k]=true;
    return;
    }
  if(members.empty() || mesh.bonds.empty()) return;
  const float radius=rule.radius[size_t(hit)];
  const JPH::Vec3 at(point.x*.01f,point.y*.01f,point.z*.01f);
  const auto metal=[&](uint32_t i) { return mesh.pieces[i].group==asset::MaterialGroup::METAL; };
  std::vector<float> distance(count,FLT_MAX);
  // The struck piece is the nearest one that is not METAL: a blade on a hoop hits the stave
  // under it. A remnant of metal only falls back to its nearest piece.
  size_t struck=count,nearest=count;
  for(const auto i:members) {
    if(i>=count) continue;
    distance[i]=hullDistanceCm(mesh.pieces[i],at);
    if(nearest==count || distance[i]<distance[nearest]) nearest=i;
    if(!metal(i) && (struck==count || distance[i]<distance[struck])) struck=i;
    }
  if(struck==count) struck=nearest;
  // A bond breaks within R / strength of its nearer piece; the struck piece loses every bond.
  // A HOOP bond is measured at its stave only: the hoop holds while its ring is whole.
  for(size_t k=0;k<mesh.bonds.size();++k) {
    const auto& bond=mesh.bonds[k];
    if(!inside(bond,k)) continue;
    const float reach=radius/rule.strength[size_t(bond.kind)];
    float nearCm=std::min(distance[bond.a],distance[bond.b]);
    if(bond.kind==FractureMesh::BondKind::Hoop && metal(bond.a)!=metal(bond.b))
      nearCm=metal(bond.a) ? distance[bond.b] : distance[bond.a];
    broken[k]=bond.a==struck || bond.b==struck || nearCm<=reach;
    }
  // The ring: a hoop that lost more than `ring` of its HOOP bonds, by this blow or before, has
  // nothing to hold and falls.
  for(const auto p:members) {
    if(p>=count || !metal(p)) continue;
    size_t total=0,lost=0;
    for(size_t k=0;k<mesh.bonds.size();++k) {
      const auto& bond=mesh.bonds[k];
      if(bond.kind!=FractureMesh::BondKind::Hoop || (bond.a!=p && bond.b!=p)) continue;
      ++total;
      lost+=broken[k];
      }
    if(total==0 || !(float(lost)>rule.ring*float(total))) continue;
    for(size_t k=0;k<mesh.bonds.size();++k) {
      const auto& bond=mesh.bonds[k];
      if(bond.kind==FractureMesh::BondKind::Hoop && (bond.a==p || bond.b==p)) broken[k]=true;
      }
    }
  }

std::vector<uint32_t> fractureComponents(const FractureMesh& mesh, const std::vector<bool>& broken) {
  const size_t count=mesh.pieces.size();
  std::vector<uint32_t> root(count);
  std::iota(root.begin(),root.end(),0u);
  const auto find=[&](uint32_t i) {
    while(root[i]!=i) i=root[i]=root[root[i]];
    return i;
    };
  for(size_t k=0;k<mesh.bonds.size();++k)
    if(k<broken.size() && !broken[k]) {
      const auto a=find(mesh.bonds[k].a),b=find(mesh.bonds[k].b);
      root[std::max(a,b)]=std::min(a,b);
      }
  std::vector<uint32_t> group(count),label(count,uint32_t(-1));
  uint32_t next=0;
  for(uint32_t i=0;i<count;++i) {
    const auto r=find(i);
    if(label[r]==uint32_t(-1)) label[r]=next++;
    group[i]=label[r];
    }
  return group;
  }

std::vector<uint32_t> fractureGroups(const FractureMesh& mesh, const FractureBonds& rule, FractureHit hit,
                                     const Tempest::Vec3& point, std::vector<bool>* broken) {
  // The intact prop is one remnant of every piece with every bond whole.
  std::vector<uint32_t> all(mesh.pieces.size());
  std::iota(all.begin(),all.end(),0u);
  std::vector<bool> cut(mesh.bonds.size(),false);
  fractureCut(mesh,rule,hit,point,all,cut);
  auto group=fractureComponents(mesh,cut);
  if(broken!=nullptr) *broken=std::move(cut);
  return group;
  }

FractureHit FractureRule::hit(Melee kind) {
  switch(kind) {
    case Melee::Fist:    return FractureHit::Fist;
    case Melee::OneHand: return FractureHit::OneHand;
    case Melee::TwoHand: return FractureHit::TwoHand;
    }
  return FractureHit::OneHand;
  }

const FractureRule* FractureRule::classify(std::string_view visual) {
  static const auto rules=[] {
    std::vector<FractureRule> value;
    std::istringstream csv(destructibleVobsCsv);
    for(std::string line;std::getline(csv,line);) {
      if(line.empty() || line.front()=='#') continue;
      std::replace(line.begin(),line.end(),',',' ');
      std::istringstream row(line);
      row.imbue(std::locale::classic());
      FractureRule rule;
      if(row>>rule.visualPattern>>rule.className>>rule.health>>rule.threshold>>rule.impactDamage>>rule.arrowDamage>>
         rule.meleeWeapon>>rule.meleeStrength>>rule.fist>>rule.oneHand>>rule.twoHand)
        if(rule.health>0 && rule.threshold>0 && rule.impactDamage>0 && rule.arrowDamage>0 &&
           rule.meleeWeapon>0 && rule.meleeStrength>0 && rule.fist>0 && rule.oneHand>0 && rule.twoHand>0)
          value.push_back(std::move(rule));
      }
    return value;
    }();
  const auto name=basename(visual);
  for(const auto& rule:rules)
    if(matches(rule.visualPattern,name)) return &rule;
  return nullptr;
  }

float FractureRule::meleeDamage(Melee kind,float weaponDamage,float strength) const {
  if(!std::isfinite(weaponDamage) || !std::isfinite(strength)) return 0.f;
  const float power=std::max(weaponDamage,0.f)*meleeWeapon+std::max(strength,0.f)*meleeStrength;
  switch(kind) {
    case Melee::Fist:    return power*fist;
    case Melee::OneHand: return power*oneHand;
    case Melee::TwoHand: return power*twoHand;
    }
  return 0.f;
  }

std::shared_ptr<const FractureMesh> FractureMesh::load(std::string_view visual) {
  Resources::MeshReplacementSource source;
  const bool replacement=Resources::meshReplacementLoaded(visual,&source);
  std::filesystem::path path;
  if(replacement) {
    path=source.file;
    path.replace_extension(".fracture");
    }
  else {
    std::filesystem::path root;
    if(const auto* dir=std::getenv("OPENGOTHIC_FRACTURE")) {
      if(dir[0]=='\0') return {};
      root=dir;
      }
    else if(const auto home=userHome(); !home.empty()) root=home/".local/share/gothic-ai/fracture";
    else return {};
    path=root/(basename(visual)+".fracture");
    }
  const auto read=[&]() -> std::shared_ptr<const FractureMesh> {
    std::ifstream file(path);
    file.imbue(std::locale::classic());
    std::string tag;
    size_t version=0,count=0;
    if(!(file>>tag>>version) || tag!="GOTHIC_FRACTURE" ||
       (replacement ? version!=2 && version!=3 : version!=1)) return {};
    if(replacement) {
      size_t size=0;
      uint32_t crc=0;
      if(!(file>>tag>>size>>std::hex>>crc>>std::dec) || tag!="source" || size!=source.size || crc!=source.crc32) return {};
      }
    const bool compoundPieces=version==3;
    if(!(file>>tag>>count) || tag!="pieces" || count<(compoundPieces ? 2u : 8u) || count>(compoundPieces ? 64u : 20u)) return {};
    auto mesh=std::make_shared<FractureMesh>();
    mesh->version=version;
    Physics::initialize();
    for(size_t p=0;p<count;++p) {
      Piece piece;
      size_t triangles=0,hulls=0;
      if(!(file>>tag>>piece.volume>>piece.center.x>>piece.center.y>>piece.center.z) ||
         tag!="piece" || !std::isfinite(piece.volume) || piece.volume<=0) return {};
      if(compoundPieces) {
        std::string group;
        if(!(file>>group>>hulls>>triangles) || !materialGroup(group,piece.group) ||
           hulls<1 || hulls>16 || triangles<1 || triangles>20000 ||
           !std::isfinite(piece.center.x) || !std::isfinite(piece.center.y) || !std::isfinite(piece.center.z)) return {};
        JPH::StaticCompoundShapeSettings compound;
        for(size_t h=0;h<hulls;++h) {
          size_t pointCount=0;
          if(!(file>>tag>>pointCount) || tag!="hull" || pointCount<4 || pointCount>256) return {};
          std::vector<JPH::Vec3> points;
          points.reserve(pointCount);
          for(size_t v=0;v<pointCount;++v) {
            float x=0,y=0,z=0;
            if(!(file>>x>>y>>z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return {};
            points.emplace_back(x*.01f,y*.01f,z*.01f);
            }
          JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),.001f);
          const auto shape=settings.Create();
          if(shape.HasError() || !std::isfinite(shape.Get()->GetVolume()) || shape.Get()->GetVolume()<=0.f) return {};
          piece.hulls.push_back(shape.Get());
          if(hulls==1) piece.hull=shape.Get();
          else compound.AddShape(JPH::Vec3::sZero(),JPH::Quat::sIdentity(),shape.Get());
          }
        if(hulls>1) {
          const auto shape=compound.Create();
          if(shape.HasError()) return {};
          piece.hull=shape.Get();
          }
        }
      else if(!(file>>triangles) || triangles<4 || triangles>100000) return {};
      piece.triangles.resize(triangles);
      std::vector<JPH::Vec3> points;
      if(!compoundPieces) points.reserve(triangles*3);
      for(auto& triangle:piece.triangles) {
        if(!(file>>tag>>triangle.texture) || tag!="tri" || triangle.texture.size()>255) return {};
        for(auto& v:triangle.vertices) {
          if(!(file>>v.position.x>>v.position.y>>v.position.z>>v.normal.x>>v.normal.y>>v.normal.z>>v.uv.x>>v.uv.y) || !finite(v,!compoundPieces)) return {};
          if(!compoundPieces) points.emplace_back(v.position.x*.01f,v.position.y*.01f,v.position.z*.01f);
          }
        }
      if(!compoundPieces) {
        JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),.001f);
        const auto shape=settings.Create();
        if(shape.HasError() || shape.Get()->GetVolume()<=0.f) return {};
        piece.hull=shape.Get();
        }
      mesh->volume+=piece.volume;
      if(compoundPieces && !std::isfinite(mesh->volume)) return {};
      mesh->pieces.push_back(std::move(piece));
      }
    if(file>>tag) {
      // Format 3 may end with its bond block; anything else is a truncated or newer format.
      if(!compoundPieces || tag!="bonds") return {};
      std::string error;
      if(!readBonds(file,*mesh,error)) {
        Tempest::Log::e("fracture: ",path.string(),": invalid bonds block, ",error);
        return {};
        }
      if(file>>tag) return {};
      }
    return mesh;
  };
  auto mesh=read();
  if(replacement && !mesh) {
    static std::mutex mutex;
    static std::unordered_set<std::string> logged;
    const auto name=basename(visual);
    std::lock_guard<std::mutex> guard(mutex);
    if(logged.insert(name).second)
      Tempest::Log::i("fracture: disabled for loaded mesh replacement \"",name,".MRM\": missing, invalid or stale .fracture sidecar");
    }
  return mesh;
  }
