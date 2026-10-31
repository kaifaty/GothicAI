// Private geometry analysis used by world-textures --intent. This file never exports pixels.
// Include after the shared archive/geometry helpers, inside their anonymous namespace.
constexpr std::array<const char*,4> intentLayouts={"tiled","panel","strip","atlas"};
constexpr double intentPositionEpsilon=.01; // cm; shared-edge vertex matching
constexpr double intentUvEpsilon=.0001;
constexpr double intentNormalCos=.996194698; // five degrees
constexpr double intentPlaneEpsilon=.5; // cm
constexpr double intentUnitMin=.75, intentUnitMax=1.25;
constexpr double intentStripAspect=4;
constexpr double intentPlaceRadius=1000; // cm, a local ten-metre concentration

bool cityTexture(const std::string& name) {
  constexpr std::array<const char*,7> cityMiscTextures={
    "NW_MISC_STRONG_STONEWALL_01.TGA","NW_MISC_STRONG_STONEWALL_SIDE_01.TGA",
    "NW_MISC_STRONG_STONEWALL_ENTRANCE_01.TGA","NW_MISC_STRONG_STONEWALL_BLOCKBORDER_01.TGA",
    "NW_MISC_STRONG_STONEWALL_ROUNDBORDER_01.TGA","NW_MISC_OLDSTONEPATH_01.TGA",
    "NW_MISC_OLDSTONEPATH_WAY_01.TGA"};
  return name.rfind("NW_CITY_",0)==0 || name.rfind("NW_HARBOUR_",0)==0 ||
    std::find(cityMiscTextures.begin(),cityMiscTextures.end(),name)!=cityMiscTextures.end();
}
double dot(Point a,Point b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Point cross(Point a,Point b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double length(Point a) { return std::sqrt(dot(a,a)); }
Point normalized(Point a) { const double l=length(a); return l>0 ? a*(1/l) : Point{}; }
Point triangleCenter(const Triangle& t) { return (t.p[0]+t.p[1]+t.p[2])*(1.0/3); }
Point triangleNormal(const Triangle& t) {
  Point n=normalized(cross(t.p[1]-t.p[0],t.p[2]-t.p[0]));
  return dot(n,t.shadingNormal)<0 ? n*(-1) : n;
}
Point transformNormal(Point n,const zenkit::VirtualObject* v) {
  if(!v) return n;
  const auto& r=v->rotation.columns;
  const Point a(r[0]),b(r[1]),c(r[2]);
  const double det=dot(a,cross(b,c));
  if(std::abs(det)<1e-12) return {};
  return (cross(b,c)*n.x+cross(c,a)*n.y+cross(a,b)*n.z)*(1/det);
}

struct IntentTriangle {
  Triangle triangle;
  size_t source=0; // zero is world mesh; vob IDs cannot be welded to one another
  std::string visual;
  double area=0;
  Point center,normal;
};
struct IntentPatch {
  std::vector<size_t> triangles;
  double area=0,uvArea=0;
  Point center,normal,lo,hi;
  std::array<double,2> uvLo,uvHi,axisSpanCm{};
  size_t layout=3;
};
size_t intentLayout(double u,double v,double uvAreaValue,double uCm=100,double vCm=100) {
  // Absolute UV origin is irrelevant: a [7,8] by [-3,-2] panel remains one panel.
  // Degenerate UVs are retained in totals, but provide no evidence for repeat or panel mapping.
  if(uvAreaValue<=1e-10) return 3;
  const bool ur=u>intentUnitMax, vr=v>intentUnitMax;
  if(ur && vr) return 0;
  if(ur!=vr) {
    const double along=ur ? uCm : vCm, across=ur ? vCm : uCm;
    return across>0 && along/across>=intentStripAspect ? 2 : 0;
  }
  if(u>=intentUnitMin && v>=intentUnitMin && u<=intentUnitMax && v<=intentUnitMax) return 1;
  return 3; // partial subrectangles are atlas candidates, pending visual interpretation
}
struct IntentDsu {
  std::vector<size_t> p;
  explicit IntentDsu(size_t n):p(n) { std::iota(p.begin(),p.end(),0); }
  size_t find(size_t a) { while(a!=p[a]) { p[a]=p[p[a]]; a=p[a]; } return a; }
  void join(size_t a,size_t b) { a=find(a); b=find(b); if(a!=b) p[b]=a; }
};
using IntentVertexKey=std::array<int64_t,3>;
struct IntentEdgeKey {
  size_t source;
  IntentVertexKey a,b;
  bool operator<(const IntentEdgeKey& o) const { return std::tie(source,a,b)<std::tie(o.source,o.a,o.b); }
};
IntentVertexKey intentVertexKey(Point p) {
  return {int64_t(std::llround(p.x/intentPositionEpsilon)),int64_t(std::llround(p.y/intentPositionEpsilon)),
          int64_t(std::llround(p.z/intentPositionEpsilon))};
}
struct IntentEdge { size_t triangle,a,b; };
bool sameUv(zenkit::Vec2 a,zenkit::Vec2 b) {
  return std::abs(a.x-b.x)<=intentUvEpsilon && std::abs(a.y-b.y)<=intentUvEpsilon;
}
std::vector<IntentPatch> intentPatches(const std::vector<IntentTriangle>& ts) {
  IntentDsu dsu(ts.size());
  std::map<IntentEdgeKey,std::vector<IntentEdge>> edges;
  for(size_t i=0;i<ts.size();++i) {
    const auto& t=ts[i];
    for(size_t k=0;k<3;++k) {
      size_t a=k,b=(k+1)%3;
      auto ka=intentVertexKey(t.triangle.p[a]),kb=intentVertexKey(t.triangle.p[b]);
      if(kb<ka) { std::swap(ka,kb); std::swap(a,b); }
      auto& bucket=edges[{t.source,ka,kb}];
      for(const auto& e:bucket) {
        const auto& other=ts[e.triangle];
        if(dot(t.normal,other.normal)<intentNormalCos) continue;
        if(std::abs(dot(t.triangle.p[0]-other.triangle.p[0],other.normal))>intentPlaneEpsilon) continue;
        if(sameUv(t.triangle.uv[a],other.triangle.uv[e.a]) && sameUv(t.triangle.uv[b],other.triangle.uv[e.b]))
          dsu.join(i,e.triangle);
      }
      bucket.push_back({i,a,b});
    }
  }
  std::map<size_t,IntentPatch> groups;
  for(size_t i=0;i<ts.size();++i) groups[dsu.find(i)].triangles.push_back(i);
  std::vector<IntentPatch> patches;
  for(auto& [id,p]:groups) {
    p.lo=Point{INFINITY,INFINITY,INFINITY}; p.hi=Point{-INFINITY,-INFINITY,-INFINITY};
    p.uvLo={INFINITY,INFINITY}; p.uvHi={-INFINITY,-INFINITY};
    for(auto i:p.triangles) {
      const auto& t=ts[i]; p.area+=t.area; p.uvArea+=uvArea(t.triangle.uv);
      const auto& uv=t.triangle.uv;
      const double du1=uv[1].x-uv[0].x,dv1=uv[1].y-uv[0].y,du2=uv[2].x-uv[0].x,dv2=uv[2].y-uv[0].y;
      const double det=du1*dv2-du2*dv1;
      if(std::abs(det)>1e-12) {
        const auto e1=t.triangle.p[1]-t.triangle.p[0],e2=t.triangle.p[2]-t.triangle.p[0];
        p.axisSpanCm[0]+=length((e1*dv2-e2*dv1)*(1/det))*t.area;
        p.axisSpanCm[1]+=length((e2*du1-e1*du2)*(1/det))*t.area;
      }
      p.center=p.center+t.center*t.area; p.normal=p.normal+t.normal*t.area;
      for(auto v:t.triangle.p) {
        p.lo={std::min(p.lo.x,v.x),std::min(p.lo.y,v.y),std::min(p.lo.z,v.z)};
        p.hi={std::max(p.hi.x,v.x),std::max(p.hi.y,v.y),std::max(p.hi.z,v.z)};
      }
      for(auto uv:t.triangle.uv) for(size_t k=0;k<2;++k) {
        p.uvLo[k]=std::min(p.uvLo[k],double(uv[k])); p.uvHi[k]=std::max(p.uvHi[k],double(uv[k]));
      }
    }
    p.center=p.area>0 ? p.center*(1/p.area) : ts[p.triangles[0]].center;
    p.normal=normalized(p.normal);
    for(size_t axis=0;axis<2;++axis) p.axisSpanCm[axis]=p.area>0 ?
      p.axisSpanCm[axis]*(p.uvHi[axis]-p.uvLo[axis])/p.area : 0;
    p.layout=intentLayout(p.uvHi[0]-p.uvLo[0],p.uvHi[1]-p.uvLo[1],p.uvArea,p.axisSpanCm[0],p.axisSpanCm[1]);
    patches.push_back(std::move(p));
  }
  std::sort(patches.begin(),patches.end(),[](const auto& a,const auto& b) {return a.area>b.area;});
  return patches;
}

void jsonString(std::ostream& f,const std::string& s) {
  f << '"';
  for(unsigned char c:s) {
    if(c=='"' || c=='\\') f << '\\' << c;
    else if(c<' ') f << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec << std::setfill(' ');
    else f << c;
  }
  f << '"';
}
void jsonPoint(std::ostream& f,Point p) { f << '[' << p.x << ',' << p.y << ',' << p.z << ']'; }
void jsonBounds(std::ostream& f,Point lo,Point hi) { f << '['; jsonPoint(f,lo); f << ','; jsonPoint(f,hi); f << ']'; }
void jsonShares(std::ostream& f,const std::array<double,4>& shares,double areaValue) {
  f << '{';
  for(size_t k=0;k<4;++k) { if(k) f << ','; jsonString(f,intentLayouts[k]); f << ':' << fraction(shares[k],areaValue); }
  f << '}';
}

struct IntentPlace {
  Point center,normal,lo,hi;
  double area=0;
  std::array<double,4> shares{};
  size_t patch=0;
};
std::vector<IntentPlace> intentPlaces(const std::vector<IntentPatch>& ps) {
  // Greedy density peaks, then remove their members. Centers are weighted by actual surface area.
  // A seed is a patch centroid, not a grid corner, so patches never disappear at grid boundaries.
  std::vector<bool> used(ps.size(),false);
  using Cell=std::array<int64_t,3>;
  auto cell=[](Point p) { return Cell{int64_t(std::floor(p.x/intentPlaceRadius)),
    int64_t(std::floor(p.y/intentPlaceRadius)),int64_t(std::floor(p.z/intentPlaceRadius))}; };
  std::map<Cell,std::vector<size_t>> bins;
  for(size_t i=0;i<ps.size();++i) bins[cell(ps[i].center)].push_back(i);
  auto nearby=[&](size_t i,const auto& visit) {
    const auto c=cell(ps[i].center);
    for(int dx=-1;dx<=1;++dx) for(int dy=-1;dy<=1;++dy) for(int dz=-1;dz<=1;++dz) {
      const auto b=bins.find({c[0]+dx,c[1]+dy,c[2]+dz}); if(b==bins.end()) continue;
      for(auto j:b->second) if(!used[j] && length(ps[i].center-ps[j].center)<=intentPlaceRadius) visit(j);
    }
  };
  std::vector<IntentPlace> places;
  for(size_t pass=0;pass<3;++pass) {
    size_t seed=ps.size(); double best=0;
    for(size_t i=0;i<ps.size();++i) if(!used[i]) {
      double weight=0;
      nearby(i,[&](size_t j) { weight+=ps[j].area; });
      if(weight>best) { best=weight; seed=i; }
    }
    if(seed==ps.size()) break;
    IntentPlace place; place.lo={INFINITY,INFINITY,INFINITY}; place.hi={-INFINITY,-INFINITY,-INFINITY};
    place.patch=seed;
    nearby(seed,[&](size_t j) {
      used[j]=true; const auto& p=ps[j];
      if(p.area>ps[place.patch].area) place.patch=j;
      place.area+=p.area; place.center=place.center+p.center*p.area; place.normal=place.normal+p.normal*p.area;
      place.shares[p.layout]+=p.area;
      place.lo={std::min(place.lo.x,p.lo.x),std::min(place.lo.y,p.lo.y),std::min(place.lo.z,p.lo.z)};
      place.hi={std::max(place.hi.x,p.hi.x),std::max(place.hi.y,p.hi.y),std::max(place.hi.z,p.hi.z)};
    });
    place.center=place.center*(1/place.area); place.normal=normalized(place.normal);
    places.push_back(place);
  }
  return places;
}

int intentMain(const fs::path& game,const fs::path& destination) {
  size_t zenkitErrors=0;
  zenkit::Logger::set(zenkit::LogLevel::ERROR,[&](auto,const char* component,const char* message) {
    ++zenkitErrors; std::cerr << "ZenKit " << component << ": " << message << '\n';
  });
  std::map<std::string,fs::path> archives;
  for(const auto& e:fs::directory_iterator(game/"Data")) if(e.is_regular_file()) archives[upper(e.path().filename().string())]=e.path();
  zenkit::Vfs vfs;
  for(const auto* name:{"MESHES.VDF","MESHES_ADDON.VDF","WORLDS.VDF","WORLDS_ADDON.VDF"}) {
    const auto it=archives.find(name); if(it==archives.end()) throw std::runtime_error(std::string("missing archive ")+name);
    vfs.mount_disk(it->second,zenkit::VfsOverwriteBehavior::ALL);
  }
  const auto* worldFile=vfs.find("NEWWORLD.ZEN");
  if(!worldFile) throw std::runtime_error("missing NEWWORLD.ZEN");
  zenkit::World world; auto input=worldFile->open_read(); world.load(input.get(),zenkit::GameVersion::GOTHIC_2);
  std::map<std::string,std::vector<IntentTriangle>> textures;
  std::map<std::string,Geometry> cache;
  size_t nextVob=0;
  auto add=[&](const Geometry& g,const zenkit::VirtualObject* v,size_t id,const std::string& visual) {
    for(const auto& original:g) if(cityTexture(original.texture)) {
      Triangle t=original; for(auto& p:t.p) p=transform(p,v);
      t.shadingNormal=transformNormal(t.shadingNormal,v);
      textures[t.texture].push_back({t,id,visual,area(t.p[0],t.p[1],t.p[2]),triangleCenter(t),triangleNormal(t)});
    }
  };
  const auto worldTriangles=worldGeometry(world.world_mesh);
  fs::create_directories(destination.parent_path());
  auto context=output(destination.parent_path()/"context.json");
  context << "{\"waypoints\":[";
  size_t pointIndex=0;
  if(world.way_net) for(const auto& wp:world.way_net->points) if(wp) {
    if(pointIndex++) context << ',';
    context << "{\"name\":"; jsonString(context,wp->name);
    context << ",\"position\":"; jsonPoint(context,Point(wp->position)); context << '}';
  }
  context << "],\"occluders\":[";
  size_t occluderIndex=0;
  auto addOccluders=[&](const Geometry& g,const zenkit::VirtualObject* v) {
    for(const auto& t:g) {
      if(occluderIndex++) context << ',';
      context << '[';
      for(size_t k=0;k<3;++k) {if(k) context << ','; jsonPoint(context,transform(t.p[k],v));}
      context << ']';
    }
  };
  addOccluders(worldTriangles,nullptr);
  add(worldTriangles,nullptr,0,"");
  std::function<void(const zenkit::VirtualObject&)> visit=[&](const auto& v) {
    if(v.visual && !v.visual->name.empty()) {
      auto original=upper(v.visual->name); std::replace(original.begin(),original.end(),'\\','/');
      original=fs::path(original).filename().string(); const auto ext=fs::path(original).extension().string();
      if((ext==".3DS" || ext==".MRM" || ext==".MSH") && v.show_visual &&
         v.type!=zenkit::VirtualObjectType::oCNpc && v.visual->type!=zenkit::VisualType::DECAL &&
         v.visual->type!=zenkit::VisualType::PARTICLE_EFFECT) {
        auto visual=ext==".3DS" ? fs::path(original).replace_extension(".MRM").string() : original;
        auto found=cache.find(visual);
        if(found==cache.end()) {
          const auto* file=vfs.find(visual); if(!file) throw std::runtime_error("missing static mesh "+visual);
          found=cache.emplace(visual,staticGeometry(*file,ext==".MSH")).first;
        }
        add(found->second,&v,++nextVob,original);
        addOccluders(found->second,&v);
      }
    }
    for(const auto& c:v.children) if(c) visit(*c);
  };
  for(const auto& v:world.world_vobs) if(v) visit(*v);
  if(zenkitErrors) throw std::runtime_error("ZenKit reported "+std::to_string(zenkitErrors)+" errors");
  context << "]}\n"; context.close();
  fs::create_directories(destination.parent_path());
  auto f=output(destination);
  f << "{\"world\":\"NEWWORLD.ZEN\",\"coordinates\":\"Gothic world centimetres (Y up)\","
       "\"layout_status\":\"geometric candidates: sub-unit atlas candidates require semantic confirmation\",\"method\":{\"position_epsilon_cm\":" << intentPositionEpsilon << ",\"uv_epsilon\":" << intentUvEpsilon
    << ",\"normal_degrees\":5,\"plane_epsilon_cm\":" << intentPlaneEpsilon
    << ",\"unit_min\":" << intentUnitMin << ",\"unit_max\":" << intentUnitMax
    << ",\"strip_aspect\":" << intentStripAspect << ",\"place_radius_cm\":" << intentPlaceRadius << "},\"textures\":[";
  size_t textureIndex=0,totalTriangles=0,totalPatches=0;
  for(const auto& [name,ts]:textures) {
    if(textureIndex++) f << ',';
    const auto patches=intentPatches(ts); const auto places=intentPlaces(patches);
    double meshArea=0,vobArea=0; std::array<double,4> shares{};
    std::map<std::string,std::pair<std::set<size_t>,double>> visuals;
    for(const auto& t:ts) {
      if(t.source) { vobArea+=t.area; visuals[t.visual].first.insert(t.source); visuals[t.visual].second+=t.area; }
      else meshArea+=t.area;
    }
    for(const auto& p:patches) shares[p.layout]+=p.area;
    const double total=meshArea+vobArea;
    const size_t mainLayout=std::max_element(shares.begin(),shares.end())-shares.begin();
    f << "{\"texture\":"; jsonString(f,name);
    f << ",\"world_area_m2\":" << meshArea << ",\"vob_area_m2\":" << vobArea << ",\"area_m2\":" << total
      << ",\"triangle_count\":" << ts.size() << ",\"layout\":"; jsonString(f,intentLayouts[mainLayout]);
    f << ",\"layout_share\":"; jsonShares(f,shares,total);
    std::vector<std::string> visualNames; for(const auto& [visual,value]:visuals) visualNames.push_back(visual);
    std::sort(visualNames.begin(),visualNames.end(),[&](const auto& a,const auto& b) {
      const auto& va=visuals.at(a); const auto& vb=visuals.at(b);
      if(va.first.size()!=vb.first.size()) return va.first.size()>vb.first.size();
      if(va.second!=vb.second) return va.second>vb.second;
      return a<b;
    });
    f << ",\"visuals\":[";
    for(size_t k=0;k<std::min(size_t(5),visualNames.size());++k) {
      if(k) f << ','; const auto& visual=visualNames[k]; const auto& value=visuals.at(visual);
      f << "{\"name\":"; jsonString(f,visual); f << ",\"instances\":" << value.first.size() << ",\"area_m2\":" << value.second << '}';
    }
    f << "],\"places\":[";
    for(size_t k=0;k<places.size();++k) {
      if(k) f << ','; const auto& p=places[k];
      f << "{\"center\":"; jsonPoint(f,p.center); f << ",\"normal\":"; jsonPoint(f,p.normal);
      f << ",\"area_m2\":" << p.area << ",\"bounds\":"; jsonBounds(f,p.lo,p.hi);
      f << ",\"layout_share\":"; jsonShares(f,p.shares,p.area);
      f << ",\"representative_patch\":" << p.patch << '}';
    }
    f << "],\"patches\":[";
    std::vector<size_t> trianglePatches(ts.size());
    for(size_t k=0;k<patches.size();++k) {
      if(k) f << ','; const auto& p=patches[k];
      for(auto t:p.triangles) trianglePatches[t]=k;
      f << "{\"center\":"; jsonPoint(f,p.center); f << ",\"normal\":"; jsonPoint(f,p.normal);
      f << ",\"area_m2\":" << p.area << ",\"bounds\":"; jsonBounds(f,p.lo,p.hi);
      f << ",\"uv_min\":[" << p.uvLo[0] << ',' << p.uvLo[1] << "],\"uv_max\":[" << p.uvHi[0] << ',' << p.uvHi[1]
        << "],\"uv_axis_span_cm\":[" << p.axisSpanCm[0] << "," << p.axisSpanCm[1]
        << "],\"uv_area\":" << p.uvArea << ",\"layout\":"; jsonString(f,intentLayouts[p.layout]);
      f << ",\"triangle_count\":" << p.triangles.size() << ",\"source\":" << ts[p.triangles[0]].source << ",\"visual\":";
      jsonString(f,ts[p.triangles[0]].visual); f << '}';
    }
    f << "],\"triangles\":[";
    for(size_t k=0;k<ts.size();++k) {
      if(k) f << ','; const auto& t=ts[k];
      f << "{\"patch\":" << trianglePatches[k] << ",\"points\":[";
      for(size_t j=0;j<3;++j) {if(j) f << ','; jsonPoint(f,t.triangle.p[j]);}
      f << "],\"uv\":[";
      for(size_t j=0;j<3;++j) {if(j) f << ','; f << '[' << t.triangle.uv[j].x << ',' << t.triangle.uv[j].y << ']';}
      f << "],\"area_m2\":" << t.area << ",\"center\":"; jsonPoint(f,t.center);
      f << ",\"normal\":"; jsonPoint(f,t.normal); f << '}';
    }
    f << "]}";
    totalTriangles+=ts.size(); totalPatches+=patches.size();
    std::cerr << name << ": " << ts.size() << " triangles, " << patches.size() << " patches, " << intentLayouts[mainLayout] << '\n';
  }
  f << "]}\n";
  std::cerr << "Intent: " << textures.size() << " textures, " << totalTriangles << " triangles, " << totalPatches << " patches; " << destination << '\n';
  return 0;
}

void intentRequire(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void intentSelfTest() {
  intentRequire(intentLayout(2,3,6)==0,"intent two-axis repeat");
  intentRequire(intentLayout(4,1,4,400,100)==2 && intentLayout(1,4,4,100,400)==2,"intent one-axis strip");
  intentRequire(intentLayout(4,1,4,400,300)==0,"intent ordinary one-axis material repeat");
  intentRequire(intentLayout(1,1,1)==1,"intent one-unit panel");
  intentRequire(intentLayout(.5,1,.5)==3,"intent atlas subrectangle");
  intentRequire(intentLayout(1,1,0)==3,"intent degenerate UV");
  Triangle a {{{Point(0,0,0),Point(200,0,0),Point(200,200,0)}},{{{0,0},{1,0},{1,1}}},"WALL.TGA"};
  Triangle b {{{Point(0,0,0),Point(200,200,0),Point(0,200,0)}},{{{0,0},{1,1},{0,1}}},"WALL.TGA"};
  auto wrap=[](Triangle t,size_t id=0) { return IntentTriangle{t,id,"",area(t.p[0],t.p[1],t.p[2]),triangleCenter(t),triangleNormal(t)}; };
  auto oriented=a; oriented.shadingNormal={0,0,-1};
  intentRequire(triangleNormal(oriented).z<0,"intent archive normals orient camera surface");
  auto ps=intentPatches({wrap(a),wrap(b)});
  intentRequire(ps.size()==1 && ps[0].layout==1 && ps[0].area==4,"intent joins quad across fan edge");
  for(auto& uv:b.uv) uv.x+=1;
  intentRequire(intentPatches({wrap(a),wrap(b)}).size()==2,"intent UV seam separates panels");
  for(auto& uv:b.uv) uv.x-=1;
  intentRequire(intentPatches({wrap(a,1),wrap(b,2)}).size()==2,"intent separate instances do not weld");
  for(auto& uv:a.uv) {uv.x+=7; uv.y-=3;}
  for(auto& uv:b.uv) {uv.x+=7; uv.y-=3;}
  ps=intentPatches({wrap(a),wrap(b)});
  intentRequire(ps.size()==1 && ps[0].layout==1,"intent offset panel is not repetition");
  const auto places=intentPlaces(ps);
  intentRequire(places.size()==1 && places[0].area==4 && places[0].shares[1]==4,"intent concentration conserves area");
  std::cout << "intent self-test: 12 checks passed\n";
}
