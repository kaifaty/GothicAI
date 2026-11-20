// The grass system (reports/engine/2026-11-14-grass-system): the `grass` field of a record and its bits for the
// ground's shader, the switch, the placement of the blades on synthetic ground (the same blades every run and for
// any split into chunks, none off the grass materials, over the slope limit or under water, the count by area,
// density and rank, the band at an open edge and the low blades past it, the low grass round a thing to pick up)
// and the processor's mirror of the wind's travelling front against the scene's wind. CPU only: no device, no game
// data.
#include "graphics/foliagefront.h"
#include "graphics/grass/grassdiag.h"
#include "graphics/grass/grassfield.h"
#include "graphics/grass/grasspack.h"
#include "graphics/grass/grassparams.h"
#include "graphics/grass/grassplace.h"
#include "graphics/grass/grassplots.h"
#include "graphics/grass/grasswaste.h"

#include <Tempest/Log>
#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// the meadow's own ground: grass_cover_test.cpp
size_t grassCoverTests();

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

using Words = std::array<uint32_t,4>;

// ---- the record ----

bool parsed(const char* json, Grass::Params& out) {
  rapidjson::Document d;
  d.Parse(json);
  require(!d.HasParseError(), std::string("the test's own json: ")+json);
  return Grass::parse(d,out);
  }

void records() {
  Grass::Params p;
  require(parsed("{}",p) && p.on && p.density==1.f && p.height==20.f && p.clumps==1.f && p.slope==35.f &&
          p.edge==150.f && p.wind==1.f && p.translucency==0.6f, "an empty grass object must take the defaults");
  require(parsed(R"({"density":0.5,"height":32,"clumps":0.25,"slope":20,"edge":80,"wind":1.5,"translucency":0.4})",p) &&
          p.on && p.density==0.5f && p.height==32.f && p.clumps==0.25f && p.slope==20.f && p.edge==80.f &&
          p.wind==1.5f && p.translucency==0.4f, "grass must carry the record's numbers");
  require(parsed(R"({"density":0,"height":5,"clumps":0,"slope":0,"edge":0,"wind":0,"translucency":0})",p) &&
          p.on && p.density==0.f && p.height==5.f && p.edge==0.f, "the low ends of the ranges are allowed");
  require(parsed(R"({"density":2,"height":60,"clumps":1,"slope":60,"edge":400,"wind":4,"translucency":1})",p) &&
          p.on && p.density==2.f && p.height==60.f && p.slope==60.f && p.edge==400.f && p.wind==4.f,
          "the high ends of the ranges are allowed");

  const Grass::Params kept = p;
  for(const char* bad:{"1", "true", "[]", "\"on\"", "null",
                       R"({"density":"1"})", R"({"density":2.1})", R"({"density":-0.1})",
                       R"({"height":4})", R"({"height":61})", R"({"height":[20]})",
                       R"({"clumps":1.5})", R"({"clumps":-1})",
                       R"({"slope":61})", R"({"slope":-5})",
                       R"({"edge":401})", R"({"edge":true})",
                       R"({"wind":4.5})", R"({"wind":-0.1})",
                       R"({"translucency":1.01})", R"({"translucency":-1})"}) {
    Grass::Params out = kept;
    require(!parsed(bad,out), std::string("grass of a wrong type or with a number out of its range must be refused: ")+bad);
    require(out==kept, std::string("a refused grass leaves the record's numbers alone: ")+bad);
    }
  }

// grassPacked* of shader/grass/grass_common.glsl
struct Unpacked final {
  float density, height, slope, wind, through;
  };

Unpacked unpack(uint32_t bits) {
  Unpacked u;
  u.density = float( bits        & 0x3Fu)*(2.f/63.f);
  u.height  = float((bits >> 6)  & 0x3Fu);
  u.slope   = float((bits >> 12) & 0x3Fu);
  u.wind    = float((bits >> 18) & 0x3Fu)*(4.f/63.f);
  u.through = float( bits >> 24)/255.f;
  return u;
  }

void bucketBits() {
  require(Grass::pack(Grass::Params())==0, "a material without grass gives its bucket no bits");
  Grass::Params p;
  p.on = true;
  require(Grass::pack(p)!=0, "the default grass gives its bucket bits");
  for(const auto& set:{std::array<float,5>{1.f,20.f,35.f,1.f,0.6f},  std::array<float,5>{0.f,5.f,0.f,0.f,0.f},
                       std::array<float,5>{2.f,60.f,60.f,4.f,1.f},   std::array<float,5>{0.37f,43.f,17.f,2.6f,0.31f}}) {
    p.density = set[0]; p.height = set[1]; p.slope = set[2]; p.wind = set[3]; p.translucency = set[4];
    const Unpacked u = unpack(Grass::pack(p));
    require(std::abs(u.density-p.density)<=1.f/63.f+1e-4f && u.height==p.height && u.slope==p.slope &&
            std::abs(u.wind-p.wind)<=2.f/63.f+1e-4f && std::abs(u.through-p.translucency)<=0.5f/255.f+1e-4f,
            "the shader must read back the record's numbers from the bucket's bits");
    }
  }

void switches() {
  unsetenv("OPENGOTHIC_GRASS");
  Grass::setSwitch(0);
  require(Grass::enabled().mode==Grass::Off && !Grass::enabled().blades, "grass=0 is off");
  Grass::setSwitch(1);
  require(Grass::enabled().mode==Grass::Low && Grass::enabled().blades, "grass=1 is the smaller ring");
  Grass::setSwitch(2);
  require(Grass::enabled().mode==Grass::Full && Grass::enabled().blades, "grass=2 is the whole ring");
  setenv("OPENGOTHIC_GRASS","0",1);
  Grass::setSwitch(2);
  require(Grass::enabled().mode==Grass::Off && !Grass::enabled().blades, "OPENGOTHIC_GRASS=0 is over the ini");
  setenv("OPENGOTHIC_GRASS","ground",1);
  Grass::setSwitch(2);
  require(Grass::enabled().mode==Grass::Full && !Grass::enabled().blades, "`ground` keeps the ground's look without blades");
  setenv("OPENGOTHIC_GRASS","1",1);
  Grass::setSwitch(0);
  require(Grass::enabled().mode==Grass::Low && Grass::enabled().blades, "OPENGOTHIC_GRASS=1 is over the ini");
  unsetenv("OPENGOTHIC_GRASS");
  Grass::setSwitch(2);

  const Grass::Ring full = Grass::ring(Grass::Full), low = Grass::ring(Grass::Low);
  require(full.full<full.fadeFrom && full.fadeFrom<full.radius && low.full<low.fadeFrom && low.fadeFrom<low.radius &&
          low.radius<full.radius && low.density<full.density && full.density==1.f,
          "the ring of the low quality is smaller and thinner");

  setenv("OPENGOTHIC_GRASS_LINKS","1000,2500,4500",1);
  const Grass::WasteRule waste;
  require(Grass::broadLinks(999.f)==4 && Grass::broadLinks(1000.f)==3 &&
          Grass::broadLinks(2500.f)==2 && Grass::broadLinks(4500.f)==1 && waste.links(2500.f)==2,
          "drawing and waste counting use the configured grass link thresholds");
  unsetenv("OPENGOTHIC_GRASS_LINKS");
  require(Grass::broadLinks(999.f)==4, "grass link thresholds are read once");

  require(Grass::wasteBand(999.f)==0 && Grass::wasteBand(1000.f)==1 && Grass::wasteBand(2399.f)==1 &&
          Grass::wasteBand(2400.f)==2 && Grass::wasteBand(4800.f)==3 && Grass::wasteBand(9000.f)==3,
          "the waste count's bands of distance are 0-10, 10-24, 24-48 m and beyond");
  require(Grass::wasteLinks(0,999.f)==4 && Grass::wasteLinks(0,2400.f)==3 && Grass::wasteLinks(0,4500.f)==1 &&
          Grass::wasteLinks(1,1100.f)==3 && Grass::wasteLinks(1,2400.f)==1 && Grass::wasteLinks(2,900.f)==2,
          "the waste count weighs each set of link bounds of step 1b");
  }

// ---- the ground of the tests ----

// A field of squares of 2 m, each of two triangles: y = x*tanX
void addField(Grass::Summary& out, float x0, float z0, float x1, float z1, float tanX, uint16_t slot) {
  const float step = 200.f;
  const float len  = std::sqrt(1.f+tanX*tanX);
  for(float z=z0; z<z1; z+=step)
    for(float x=x0; x<x1; x+=step) {
      const float corner[4][2] = {{x,z},{x+step,z},{x+step,z+step},{x,z+step}};
      const size_t order[2][3] = {{0,1,2},{0,2,3}};
      for(const auto& tri:order) {
        float pos[3][3], uv[3][2], normal[3][3];
        for(size_t i=0; i<3; ++i) {
          const float px = corner[tri[i]][0], pz = corner[tri[i]][1];
          pos[i][0] = px;          pos[i][1] = px*tanX;       pos[i][2] = pz;
          uv[i][0]  = px/400.f;    uv[i][1]  = pz/400.f;
          normal[i][0] = -tanX/len; normal[i][1] = 1.f/len;   normal[i][2] = 0.f;
          }
        Grass::addGrass(out,pos,uv,normal,slot);
        }
      }
  }

// Ground of another material, level at `y`, in the same squares
void addRoad(Grass::Summary& out, float x0, float z0, float x1, float z1, float y) {
  const float up[3][3] = {{0,1,0},{0,1,0},{0,1,0}};
  for(float z=z0; z<z1; z+=200.f)
    for(float x=x0; x<x1; x+=200.f) {
      const float a[3][3] = {{x,y,z},{x+200.f,y,z},{x+200.f,y,z+200.f}};
      const float b[3][3] = {{x,y,z},{x+200.f,y,z+200.f},{x,y,z+200.f}};
      Grass::addBeside(out,a,up);
      Grass::addBeside(out,b,up);
      }
  }

Grass::Params meadow(float edge = 0.f) {
  Grass::Params p;
  p.on   = true;
  p.edge = edge;
  return p;
  }

struct Build final {
  uint32_t                     rankLimit = 256;
  float                        density   = 1.f;
  std::vector<Grass::Disc>     items;
  bool                         squares   = true;
  };

Grass::CellBlades build(std::span<const Grass::Summary*const> ground, int32_t cx, int32_t cz,
                        std::span<const Grass::Params> params, const Build& b = Build()) {
  Grass::CellInput in;
  in.cx        = cx;
  in.cz        = cz;
  in.summaries = ground;
  in.params    = params;
  in.items     = b.items;
  in.rankLimit = b.rankLimit;
  in.density   = b.density;
  in.squares   = b.squares;
  return Grass::buildCell(in);
  }

Grass::CellBlades build(const Grass::Summary& ground, int32_t cx, int32_t cz, const Grass::Params& params,
                        const Build& b = Build()) {
  const Grass::Summary* list[1] = {&ground};
  return build(list,cx,cz,std::span<const Grass::Params>(&params,1),b);
  }

std::vector<Words> words(const Grass::CellBlades& c) {
  std::vector<Words> r(c.blades.size());
  for(size_t i=0; i<r.size(); ++i)
    r[i] = {c.blades[i].w[0],c.blades[i].w[1],c.blades[i].w[2],c.blades[i].w[3]};
  return r;
  }

std::vector<Words> sorted(std::vector<Words> w) {
  std::sort(w.begin(),w.end());
  return w;
  }

bool same(const Grass::CellBlades& a, const Grass::CellBlades& b) {
  if(a.minY!=b.minY || a.maxY!=b.maxY || a.batches.size()!=b.batches.size() || words(a)!=words(b))
    return false;
  for(size_t i=0; i<a.batches.size(); ++i)
    if(a.batches[i].slot!=b.batches[i].slot || a.batches[i].first!=b.batches[i].first || a.batches[i].count!=b.batches[i].count)
      return false;
  return true;
  }

uint32_t rankOf(const Words& w)   { return w[1] >> 24; }
// the root and its height over the cell's lowest, without what dresses the blade
std::pair<uint32_t,uint32_t> rootOf(const Words& w) { return {w[0] & 0x0FFFFFFFu, w[1] & 0xFFFFu}; }

std::string at(int32_t cx, int32_t cz) {
  return " (cell "+std::to_string(cx)+","+std::to_string(cz)+")";
  }

constexpr int32_t U = Grass::Unit;

void drawOrder(bool disabled) {
  struct Cell final {
    float dist;
    };
  struct Draw final {
    const Cell* cell;
    uint32_t table;
    bool operator==(const Draw&) const = default;
    };
  const Cell farther{2500.f}, closer{300.f}, tied{300.f};
  std::vector<Draw> draws = {{&farther,0}, {&farther,32}, {&closer,64}, {&closer,96}};
  // Forty draws at the same distance exercise ties beyond std::sort's small-range insertion sort.
  for(uint32_t i=0; i<38; ++i)
    draws.push_back({&tied,(i+4)*32});
  auto expected = draws;
  Grass::orderDraws(draws);
  if(disabled) {
    require(draws==expected, "OPENGOTHIC_GRASS_ORDER=0 must keep cell traversal order");
    } else {
    std::rotate(expected.begin(), expected.begin()+2, expected.end());
    require(draws==expected, "grass draws must be near first, preserving batch order and span offsets");
    }
  }

void packSpans() {
  for(const uint32_t pack:Grass::PackSizes) {
    std::vector<uint32_t> table = {17, 29};
    Grass::PackSpans spans(table, pack);
    spans.reset();
    spans.add(1, pack);
    spans.add(pack+1, 1); // contiguous with the first range
    const uint32_t next = pack+3;
    spans.add(next, pack); // one blade skipped in the cell buffer
    require(spans.finish()==3, "only a discontinuity in the blade buffer starts a new packed span");

    std::vector<uint32_t> expected = {17, 29};
    if(pack==1) {
      expected.insert(expected.end(), {2, 1, 3, next-2});
      expected.resize(2+16*2, UINT32_MAX);
      } else {
      // The last instance of the first span has one blade. The next instance starts at the next span's first blade.
      expected.insert(expected.end(), {2, 1, pack+2, 3, next-2*pack, next+pack});
      expected.resize(2+16*3, UINT32_MAX);
      }
    require(table==expected, "grass span boundaries and physical blade ends must match the shader's table");

    const size_t   drawOffset = expected.size();
    const uint32_t first = next+pack; // adjacent in the blade buffer, but in a new draw
    spans.reset();
    spans.add(first, 1);
    require(spans.finish()==1, "each grass draw must count its instances from zero");
    expected.insert(expected.end(), {1, first});
    if(pack!=1)
      expected.push_back(first+1);
    expected.resize(drawOffset+16*(pack==1 ? 2u : 3u), UINT32_MAX);
    require(table==expected, "a new grass draw must append its own spans after the previous draw's table");
    }
  }

void packIndices() {
  const std::vector<uint16_t> legacy = {
    0,1,2, 1,3,2, 2,3,4, 3,5,4, 4,5,6, 5,7,6, 6,7,8,
    0,1,2, 1,3,2, 2,3,4, 3,5,4, 4,5,6,
    0,1,2, 1,3,2, 2,3,4,
    0,1,2,
    };
  require(Grass::packIndices(1)==legacy, "single-blade indices must keep the original topology and order");
  for(const uint32_t pack:Grass::PackSizes) {
    const auto indices = Grass::packIndices(pack);
    require(indices.size()==legacy.size()*pack, "the index buffer must have one strip per blade in each pack");
    size_t base = 0;
    for(uint8_t links=Grass::MaxSegments; links>0; --links) {
      const size_t vertices = 2u*links+1;
      const size_t count    = (2u*links-1)*3;
      const size_t start    = Grass::packIndexOffset(links, pack);
      require(start==base*pack,
              "packed strips must follow the same link order as the original index buffer");
      for(uint32_t copy=0; copy<pack; ++copy) {
        const size_t offset = copy*vertices;
        const auto   first  = indices.begin()+static_cast<std::ptrdiff_t>(start+copy*count);
        const auto   last   = first+static_cast<std::ptrdiff_t>(count);
        const auto   source = legacy.begin()+static_cast<std::ptrdiff_t>(base);
        require(offset+vertices-1<=UINT16_MAX &&
                std::equal(first, last, source, [&](uint16_t index, uint16_t original) {
                  return index==original+offset;
                  }), "each copy must keep the blade topology with its own uint16 vertex indices");
        }
      base += count;
      }
    }
  }

void environmentFallbacks() {
  // Each child sets its environment before the first use of the cached switches.
  for(const char* links:std::array<const char*,3>{nullptr, "3000,1750,750", "750,1750,3000x"}) {
    const auto child = fork();
    require(child>=0, "cannot fork grass environment test");
    if(child==0) {
      try {
        require(setenv("OPENGOTHIC_GRASS_ORDER","0",1)==0, "cannot set grass order switch");
        drawOrder(true);
        const int result = links==nullptr ? unsetenv("OPENGOTHIC_GRASS_LINKS") : setenv("OPENGOTHIC_GRASS_LINKS",links,1);
        require(result==0, "cannot configure grass link thresholds");
        size_t errors = 0;
        Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode, const char* text) {
          if(mode==Tempest::Log::Error && std::string_view(text).find("OPENGOTHIC_GRASS_LINKS")!=std::string_view::npos)
            ++errors;
          });
        require(Grass::broadLinks(799.f)==4 && Grass::broadLinks(800.f)==3 &&
                Grass::broadLinks(1199.f)==3 && Grass::broadLinks(1200.f)==2 &&
                Grass::broadLinks(2399.f)==2 && Grass::broadLinks(2400.f)==1,
                "unset or invalid grass link thresholds must use defaults");
        require(errors==size_t(links!=nullptr), "only invalid grass link thresholds must be logged once");
        _exit(0);
        }
      catch(const std::exception& e) {
        std::cerr << "grass environment test failed: " << e.what() << '\n';
        _exit(1);
        }
      }
    int status = 0;
    require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
            std::string("grass environment test failed for ")+(links==nullptr ? "unset thresholds" : links));
    }
  }

// ---- the placement ----

// A flat meadow of 64 m with 25 cells of 8 m in it, some on the far side of the world's zero
void placementRepeats() {
  Grass::Summary flat;
  addField(flat,-1600.f,-1600.f,4800.f,4800.f,0.f,0);
  require(flat.grass.size()==32u*32u*2u && flat.water.empty(), "every triangle of the flat meadow is ground");
  const Grass::Params par = meadow();

  // the same triangles as three chunks would give them, in another order
  Grass::Summary part[3];
  for(size_t i=flat.grass.size(); i>0; --i)
    part[(i*7)%3].grass.push_back(flat.grass[i-1]);
  const Grass::Summary* chunks[5] = {&part[2],nullptr,&part[0],&part[1],nullptr};

  size_t total = 0, low = 0, halfTotal = 0;
  for(int32_t cz=-1; cz<4; ++cz)
    for(int32_t cx=-1; cx<4; ++cx) {
      const auto cell = build(flat,cx,cz,par);
      const auto w    = words(cell);
      total += w.size();
      require(same(cell,build(flat,cx,cz,par)), "two builds of a cell must give the same blades"+at(cx,cz));
      require(same(cell,build(chunks,cx,cz,std::span<const Grass::Params>(&par,1))),
              "the blades must not depend on the chunks the ground came in or on their order"+at(cx,cz));
      if(w.empty())
        continue;
      require(cell.minY==0 && cell.maxY==0, "blades of a flat ground stand at its height"+at(cx,cz));
      // one material: a batch per square of 2 m with a blade in it, one after another; a root of the margin is of
      // the nearest square
      size_t  next = 0;
      int32_t last = -1;
      for(const auto& bt:cell.batches) {
        require(bt.slot==0 && bt.kind==0 && bt.first==next && bt.count>0 && int32_t(bt.square)>last &&
                bt.square<Grass::CellSquares*Grass::CellSquares, "one material: a batch per square, in their order"+at(cx,cz));
        next += bt.count;
        last  = int32_t(bt.square);
        const int32_t qx = bt.square/Grass::CellSquares, qz = bt.square%Grass::CellSquares;
        const int32_t x0 = cx*Grass::CellSize + qx*Grass::SquareSize, z0 = cz*Grass::CellSize + qz*Grass::SquareSize;
        for(uint32_t k=0; k<bt.count; ++k) {
          const auto b = Grass::decode(cell.blades[bt.first+k],cx,cz,cell.minY);
          require((b.x>=x0 || qx==0) && (b.x<x0+Grass::SquareSize || qx==Grass::CellSquares-1) &&
                  (b.z>=z0 || qz==0) && (b.z<z0+Grass::SquareSize || qz==Grass::CellSquares-1),
                  "a root lies in the square of its batch"+at(cx,cz));
          require(k==0 || rankOf(w[bt.first+k-1])<=rankOf(w[bt.first+k]),
                  "the blades of a batch are in the order of their ranks"+at(cx,cz));
          }
        }
      require(next==w.size(), "the batches hold all the blades"+at(cx,cz));
      Build uncut;
      uncut.squares = false;
      const auto one = build(flat,cx,cz,par,uncut);
      require(one.batches.size()==1 && one.batches[0].count==w.size() && sorted(words(one))==sorted(w),
              "without the squares: one batch of the same blades"+at(cx,cz));
      const auto ow = words(one);
      for(size_t i=1; i<ow.size(); ++i)
        require(rankOf(ow[i-1])<=rankOf(ow[i]), "and in the order of their ranks"+at(cx,cz));
      for(size_t i=0; i<w.size(); ++i) {
        const auto b = Grass::decode(cell.blades[i],cx,cz,cell.minY);
        require(b.x>=cx*Grass::CellSize-Grass::CellMargin && b.x<(cx+1)*Grass::CellSize+Grass::CellMargin &&
                b.z>=cz*Grass::CellSize-Grass::CellMargin && b.z<(cz+1)*Grass::CellSize+Grass::CellMargin,
                "a root lies in its cell or on its margin"+at(cx,cz));
        require(b.height>=2.f && b.height<=63.f && b.y==0, "a blade is 2 to 63 cm and stands on the ground"+at(cx,cz));
        if(rankOf(w[i])<64)
          ++low;
        }

      // a cell far from the camera is built of the low ranks alone: the start of the whole cell
      Build far;
      far.rankLimit = 90;
      std::vector<Words> start;
      for(const auto& i:w)
        if(rankOf(i)<90)
          start.push_back(i);
      require(words(build(flat,cx,cz,par,far))==start, "a cell of the ranks under 90 is the start of the whole one"+at(cx,cz));

      // half the density: every second blade of the same ones
      Build thin;
      thin.density = 0.5f;
      const auto half = build(flat,cx,cz,par,thin);
      const std::set<Words> whole(w.begin(),w.end());
      for(const auto& i:words(half))
        require(whole.count(i)==1, "a thinner meadow is made of the same blades"+at(cx,cz));
      halfTotal += half.blades.size();
      Grass::Params record = par;
      record.density = 0.5f;
      require(same(half,build(flat,cx,cz,record)), "the record's density and the quality's thin alike"+at(cx,cz));
      }

  // 25 cells of 64 m2: the short sward has 400 blades per m2, the main grass 320, the clumps up to 640; the band
  // is wide, since 40 m hold only a few of the patches
  const double area = 25.0*64.0;
  require(double(total)>=0.8*320.0*area && double(total)<=1.6*320.0*area,
          "the count must follow the area and the density: "+std::to_string(total)+" blades on 1600 m2");
  // and against the patches' own densities and the spots of bare earth, read every 20 cm: a node takes the patch
  // at its tuft's centre, up to a metre away, and the tuft's own count, and no more blades than nodes stand on a
  // metre
  double expected = 0;
  for(int32_t z=-790; z<3200; z+=20)
    for(int32_t x=-790; x<3200; x+=20) {
      const int32_t n    = Grass::patchNoise(x*U,z*U);
      const double  bare = double(Grass::patchBare(x*U,z*U))/256.0;
      expected += 0.04*(1.0-bare)*std::min(double(Grass::NodesPerM2), double(Grass::patchDensity(n))*double(Grass::BladesPerM2)/256.0);
      }
  require(double(total)>=0.93*expected && double(total)<=1.07*expected,
          "the count must be the patches' density over the area: "+std::to_string(total)+" blades, "+
          std::to_string(expected)+" by the patches");
  const double lowShare = double(low)/double(total);
  require(lowShare>=0.22 && lowShare<=0.28, "a quarter of the ranks holds a quarter of the blades: "+std::to_string(lowShare));
  const double halfShare = double(halfTotal)/double(total);
  require(halfShare>=0.47 && halfShare<=0.53, "half the density is half the blades: "+std::to_string(halfShare));

  Build none;
  none.density = 0.f;
  require(build(flat,1,1,par,none).blades.empty(), "no density: no blades");
  none.density   = 1.f;
  none.rankLimit = 0;
  require(build(flat,1,1,par,none).blades.empty(), "no ranks: no blades");
  require(build(flat,40,40,par).blades.empty() && build(flat,-9,1,par).blades.empty(), "no ground under a cell: no blades");
  }

// Where the ground is of another material, too steep or under water
void placementGround() {
  Grass::Summary flat, half, two;
  addField(flat,-1600.f,-1600.f,4800.f,3200.f,0.f,0);
  addField(half,-1600.f,-1600.f,1400.f,3200.f,0.f,0);
  addField(two, -1600.f,-1600.f,1400.f,3200.f,0.f,0);
  addField(two,  1400.f,-1600.f,4800.f,3200.f,0.f,1);
  const int32_t      border = 1400*U;
  const Grass::Params par = meadow();
  const Grass::Params both[2] = {meadow(),meadow()};
  const Grass::Params onOff[2] = {meadow(),Grass::Params()};
  const Grass::Params off[2]  = {Grass::Params(),Grass::Params()};

  size_t onHalf = 0, onSide = 0;
  for(int32_t cz=0; cz<2; ++cz)
    for(int32_t cx=0; cx<4; ++cx) {
      const auto whole = build(flat,cx,cz,par);
      const auto part  = build(half,cx,cz,par);
      const auto wWhole = words(whole);
      const std::set<Words> set(wWhole.begin(),wWhole.end());
      for(const auto& w:words(part))
        require(set.count(w)==1, "the ground beside a meadow does not move its blades"+at(cx,cz));
      for(const auto& b:part.blades)
        require(Grass::decode(b,cx,cz,part.minY).x<=border, "no blade off the grass material"+at(cx,cz));
      onHalf += part.blades.size();
      for(const auto& b:whole.blades)
        if(Grass::decode(b,cx,cz,whole.minY).x<=border)
          ++onSide;

      // the same ground of two materials: the same blades, in a batch each
      const Grass::Summary* list[1] = {&two};
      const auto pair = build(list,cx,cz,both);
      require(sorted(words(pair))==sorted(wWhole), "the material of the ground does not move a blade"+at(cx,cz));
      uint32_t next = 0;
      for(size_t i=0; i<pair.batches.size(); ++i) {
        const auto& batch = pair.batches[i];
        require(batch.first==next && batch.count>0 && (i==0 || pair.batches[i-1].slot<batch.slot ||
                (pair.batches[i-1].slot==batch.slot && pair.batches[i-1].square<batch.square)),
                "the batches follow one another by material, then by square"+at(cx,cz));
        for(uint32_t k=batch.first; k<batch.first+batch.count; ++k) {
          const int32_t x = Grass::decode(pair.blades[k],cx,cz,pair.minY).x;
          require(batch.slot==0 ? x<=border : x>=border, "a blade is of the material it stands on"+at(cx,cz));
          }
        next = batch.first+batch.count;
        }
      require(next==pair.blades.size(), "the batches hold every blade"+at(cx,cz));
      // a material without the field among those with it
      require(same(build(list,cx,cz,onOff),part), "a ground material without grass is as no ground"+at(cx,cz));
      require(build(list,cx,cz,off).blades.empty(), "no grass material: no blades"+at(cx,cz));
      }
  require(onHalf>0 && onHalf<=onSide && double(onHalf)>=0.9*double(onSide),
          "the count must follow the area of the grass material: "+std::to_string(onHalf)+" of "+std::to_string(onSide));

  // the slope: none over the limit, thinner on its last 10 degrees, the whole count under them
  const auto tanOf = [](double degrees) { return float(std::tan(degrees*M_PI/180.0)); };
  Grass::Summary level, gentle, steep, wall;
  addField(level, -400.f,-400.f,1200.f,1200.f,0.f,0);
  addField(gentle,-400.f,-400.f,1200.f,1200.f,tanOf(20),0);
  addField(steep, -400.f,-400.f,1200.f,1200.f,tanOf(30),0);
  addField(wall,  -400.f,-400.f,1200.f,1200.f,tanOf(50),0);
  const size_t onLevel = build(level,0,0,par).blades.size();
  require(onLevel>1000, "the test's cell has grass: "+std::to_string(onLevel));
  const auto onGentle = build(gentle,0,0,par);
  require(onGentle.blades.size()==onLevel, "a slope 10 degrees under the limit keeps every blade");
  for(const auto& b:onGentle.blades) {
    const auto i = Grass::decode(b,0,0,onGentle.minY);
    require(std::abs(double(i.y)-double(i.x)*double(tanOf(20)))<=8.0, "a root stands on the slope");
    }
  // by the share of the way to the limit left: 0.525 at 30 degrees of 25 to 35. The level meadow keeps every node
  // the patches ask for, so the share is the only place where the count shows the node's final roll
  const size_t onSteep  = build(steep,0,0,par).blades.size();
  const double thinning = double(tanOf(par.slope)-tanOf(30))/double(tanOf(par.slope)-tanOf(par.slope-10.f));
  require(double(onSteep)>=0.95*thinning*double(onLevel) && double(onSteep)<=1.05*thinning*double(onLevel),
          "the last 10 degrees under the limit thin the grass by the slope: "+std::to_string(onSteep)+" of "+
          std::to_string(onLevel)+", "+std::to_string(thinning)+" of it expected");
  require(build(wall,0,0,par).blades.empty(), "no grass over the slope limit");
  Grass::Params climber = par;
  climber.slope = 60.f;
  const size_t onWall = build(wall,0,0,climber).blades.size();
  require(onWall<=onLevel && double(onWall)>=0.9*double(onLevel), "the limit is the record's: "+std::to_string(onWall));

  // water over the ground
  Grass::Summary pond = level, puddle = level;
  const float over[2][3][3]  = {{{0,50,0},{400,50,0},{400,50,800}},  {{0,50,0},{400,50,800},{0,50,800}}};
  const float under[2][3][3] = {{{0,-50,0},{400,-50,0},{400,-50,800}}, {{0,-50,0},{400,-50,800},{0,-50,800}}};
  for(int i=0; i<2; ++i) {
    Grass::addWater(pond,over[i]);
    Grass::addWater(puddle,under[i]);
    }
  require(pond.water.size()==2 && pond.grass.size()==level.grass.size(), "water is kept beside the ground");
  const auto wet = build(pond,0,0,par);
  size_t dry = 0;
  for(const auto& b:wet.blades) {
    const auto i = Grass::decode(b,0,0,wet.minY);
    require(!(i.x>0 && i.x<400*U && i.z>0 && i.z<800*U), "no grass under water");
    ++dry;
    }
  require(dry>0 && dry<onLevel, "the water takes the blades under it and no others");
  require(same(build(puddle,0,0,par),build(level,0,0,par)), "water under the ground drowns nothing");

  // what is not ground
  Grass::Summary nothing;
  const float uv[3][2]       = {{0,0},{1,0},{0,1}};
  const float up[3][3]       = {{0,1,0},{0,1,0},{0,1,0}};
  const float down[3][3]     = {{0,-1,0},{0,-1,0},{0,-1,0}};
  const float side[3][3]     = {{1,0,0},{1,0,0},{1,0,0}};
  const float lying[3][3]    = {{0,0,0},{200,0,0},{0,0,200}};
  const float upright[3][3]  = {{0,0,0},{200,0,0},{100,200,0}};
  const float huge[3][3]     = {{0,0,0},{20000,0,0},{0,0,200}};
  Grass::addGrass(nothing,lying,uv,down,0);
  Grass::addGrass(nothing,lying,uv,side,0);
  Grass::addGrass(nothing,upright,uv,up,0);
  Grass::addGrass(nothing,huge,uv,up,0);
  require(nothing.empty(), "a ceiling, a wall, a triangle on edge and one longer than 164 m are not ground");
  Grass::addGrass(nothing,lying,uv,up,3);
  require(nothing.grass.size()==1 && nothing.grass[0].slot==3, "a floor is ground");
  }

// The band at an open edge: lower and thinner grass, the same roots
void placementEdge() {
  Grass::Summary field;
  addField(field,-600.f,-600.f,600.f,2400.f,0.f,0);
  const int32_t border = 600*U;
  size_t nearPlain = 0, nearBand = 0, farPlain = 0;
  for(int32_t cz=0; cz<2; ++cz) {
    const auto plain = build(field,0,cz,meadow(0.f));
    const auto band  = build(field,0,cz,meadow(150.f));
    const auto wPlain = words(plain), wBand = words(band);
    std::map<std::pair<uint32_t,uint32_t>,float> roots;
    std::set<Words> inner;
    for(size_t i=0; i<plain.blades.size(); ++i) {
      const auto& w = wPlain[i];
      const auto b = Grass::decode(plain.blades[i],0,cz,plain.minY);
      float& tallest = roots[rootOf(w)]; // two blades may share a root: 1/16 cm
      tallest = std::max(tallest,b.height);
      if(b.x>=border-50*U)
        ++nearPlain;
      if(b.x<200*U) {
        inner.insert(w);
        ++farPlain;
        }
      }
    size_t farBand = 0;
    for(size_t i=0; i<band.blades.size(); ++i) {
      const auto& w = wBand[i];
      const auto b = Grass::decode(band.blades[i],0,cz,band.minY);
      const auto it = roots.find(rootOf(w));
      require(it!=roots.end(), "the band at an edge moves no root"+at(0,cz));
      require(b.height<=it->second, "the grass at an edge is not taller"+at(0,cz));
      if(b.x>=border-50*U)
        ++nearBand;
      if(b.x<200*U) {
        require(inner.count(w)==1, "the grass 4 m from an edge is as without the band"+at(0,cz));
        ++farBand;
        }
      }
    require(farBand==inner.size(), "the grass 4 m from an edge is all there"+at(0,cz));
    }
  require(farPlain>0 && nearPlain>0 && double(nearBand)<0.9*double(nearPlain),
          "the grass is thinner on the last 50 cm before an open edge: "+std::to_string(nearBand)+" of "+
          std::to_string(nearPlain));

  // The exception to "a root stands on grass": where ground of another material goes on from the grass past an
  // open edge, sparse low blades root up to 35 cm past it, so the mesh's straight line does not read
  Grass::Summary road = field, ledge = field, wall = field;
  addRoad(road, 600.f,-600.f,1000.f,2400.f,0.f);
  addRoad(ledge,600.f,-600.f,1000.f,2400.f,-30.f);
  const float side[3][3]  = {{1,0,0},{1,0,0},{1,0,0}};
  const float stand[3][3] = {{600,0,-600},{600,0,2400},{600,300,2400}};
  Grass::addBeside(wall,stand,side);
  require(road.beside.size()==2u*2u*15u && wall.beside.empty(), "level ground beside the grass is kept, a wall is not");
  size_t past = 0, last = 0;
  for(int32_t cz=0; cz<2; ++cz) {
    const auto band  = build(field,0,cz,meadow(150.f));
    const auto wBand = words(band);
    const std::set<Words> onGrass(wBand.begin(),wBand.end());
    const auto with  = build(road,0,cz,meadow(150.f));
    const auto wWith = words(with);
    size_t on = 0;
    for(size_t i=0; i<with.blades.size(); ++i) {
      const auto b = Grass::decode(with.blades[i],0,cz,with.minY);
      if(b.x<=border) {
        require(onGrass.count(wWith[i])==1, "the ground beside the grass moves no blade on it"+at(0,cz));
        ++on;
        if(b.x>=border-Grass::PastEdge*U)
          ++last;
        continue;
        }
      require(b.x<=border+Grass::PastEdge*U, "a root past an open edge is no further than 35 cm from it"+at(0,cz));
      require(b.height<=Grass::PastHeight && b.y==0, "a blade past an open edge is low and stands on the ground there"+at(0,cz));
      ++past;
      }
    require(on==band.blades.size(), "the blades on the grass are all there"+at(0,cz));
    require(same(build(ledge,0,cz,meadow(150.f)),band), "no blade past an edge where the ground steps down"+at(0,cz));
    require(same(build(road,0,cz,meadow(0.f)),build(field,0,cz,meadow(0.f))), "no band: no blade past an edge"+at(0,cz));
    }
  require(past>0 && 2*past<last, "the blades past an open edge are sparse: "+std::to_string(past)+" past it, "+
          std::to_string(last)+" on the last 35 cm before it");
  }

// A thing to pick up: the grass round it is low, the blades are the same
void placementItems() {
  Grass::Summary flat;
  addField(flat,-400.f,-400.f,2000.f,2000.f,0.f,0);
  const Grass::Params par = meadow();
  const auto plain = build(flat,1,1,par);
  require(!plain.blades.empty(), "the test's cell has grass");
  Grass::BladeInfo tall;
  for(const auto& b:plain.blades) {
    const auto i = Grass::decode(b,1,1,plain.minY);
    if(i.height>tall.height)
      tall = i;
    }
  require(tall.height>8.f, "the test's cell has grass over 8 cm");

  Build with;
  with.items.push_back(Grass::Disc{tall.x,tall.z});
  with.items.push_back(Grass::Disc{-100000,-100000});
  const auto low = build(flat,1,1,par,with);
  require(low.blades.size()==plain.blades.size(), "a thing to pick up takes no blade");
  const auto wPlain = words(plain), wLow = words(low);
  size_t lowered = 0;
  for(size_t i=0; i<low.blades.size(); ++i) {
    const auto a = Grass::decode(plain.blades[i],1,1,plain.minY), b = Grass::decode(low.blades[i],1,1,low.minY);
    require(a.x==b.x && a.z==b.z && a.rank==b.rank, "a thing to pick up moves no blade");
    const double r = std::hypot(double(b.x-tall.x),double(b.z-tall.z))/double(U);
    if(r<=35.0) {
      require(b.height<=8.f, "the grass within 35 cm of a thing to pick up is no taller than 8 cm");
      if(a.height>8.f)
        ++lowered;
      }
    require(b.height<=a.height, "the grass round a thing to pick up is not taller");
    if(r>81.0)
      require(wPlain[i]==wLow[i], "the grass 80 cm from a thing to pick up is as without it");
    }
  require(lowered>0, "the tall blade at the thing is lowered");
  }

// The patches: Grass::patchNoise and what the placement and the ground's shader read from it
void patches() {
  size_t sward = 0, clump = 0, bare = 0, all = 0;
  for(int32_t z=-100; z<100; ++z)
    for(int32_t x=-100; x<100; ++x) {
      const int32_t n = Grass::patchNoise(x*97*U,z*97*U);
      require(n>=0 && n<=65535, "the patches' noise is of 16 bits");
      require(n==Grass::patchNoise(x*97*U,z*97*U), "the patches stand where they stood");
      const int32_t b = Grass::patchBare(x*97*U,z*97*U);
      require(b>=0 && b<=256 && b==Grass::patchBare(x*97*U,z*97*U), "a spot of bare earth stands where it stood");
      if(Grass::patchSward(n))
        ++sward;
      if(Grass::patchHeight(n)>=1.5f)
        ++clump;
      if(b>=128)
        ++bare;
      ++all;
      }
  const double swardShare = double(sward)/double(all), clumpShare = double(clump)/double(all), bareShare = double(bare)/double(all);
  require(swardShare>=0.2 && swardShare<=0.4, "the short sward is a third of the meadow: "+std::to_string(swardShare));
  require(clumpShare>=0.05 && clumpShare<=0.22, "the tall clumps are a small part of the meadow: "+std::to_string(clumpShare));
  // bare earth is an accent: a few hundredths of the ground, no hole of the patches' size
  require(bareShare>=0.01 && bareShare<=0.06, "bare earth is a few hundredths of the meadow: "+std::to_string(bareShare));
  float height = 0.f;
  for(int32_t n=0; n<=65535; n+=7) {
    const int32_t d = Grass::patchDensity(n);
    const float   h = Grass::patchHeight(n);
    require(h>=height && h>=0.5f && h<=2.4f, "the higher the patches' noise, the taller: no step down");
    // every patch is grass: the sward makes up in blades what it lacks in height, no patch is thin or empty
    require(d>=256 && d<=512, "no patch of the meadow is thinner than the main grass");
    require(!Grass::patchSward(n) || (d>256 && h<=0.65f), "the sward is short and dense");
    height = h;
    }
  require(Grass::patchDensity(65535)==512, "the clumps are of twice the density");

  // a spot of bare earth is a metre or two across, and two may touch: no line through a bare place is bare for 5 m
  for(int32_t z=-60; z<60; ++z)
    for(int32_t x=-60; x<60; ++x) {
      const int32_t px = x*53*U, pz = z*53*U;
      if(Grass::patchBare(px,pz)<256)
        continue;
      int runX = 0, runZ = 0;
      for(int32_t i=-350; i<=350; i+=10) {
        runX += Grass::patchBare(px+i*U,pz)>=128 ? 1 : 0;
        runZ += Grass::patchBare(px,pz+i*U)>=128 ? 1 : 0;
        }
      require(runX<=50 && runZ<=50, "a spot of bare earth is no hole of the patches' size");
      }
  }

// The tufts: no two neighbours are copies
void tufts() {
  size_t pairs = 0, alike = 0, loose = 0, all = 0;
  float  reach[2] = {1e9f,0.f}, count[2] = {1e9f,0.f}, longest = 0.f;
  for(int32_t tz=-20; tz<20; ++tz)
    for(int32_t tx=-20; tx<20; ++tx) {
      const Grass::Tuft a = Grass::tuft(tx,tz,36000);
      const Grass::Tuft same = Grass::tuft(tx,tz,36000);
      require(a.x==same.x && a.z==same.z && a.reachQ8==same.reachQ8 && a.densityQ8==same.densityQ8 && a.phase==same.phase,
              "a tuft is the same every time");
      require(a.x>=tx*Grass::TuftStep && a.x<(tx+1)*Grass::TuftStep && a.z>=tz*Grass::TuftStep && a.z<(tz+1)*Grass::TuftStep,
              "a tuft's centre lies in its square");
      require(!a.sward && !a.clump && a.reachQ8>=160 && a.reachQ8<=415 && a.longQ8>=256 && a.longQ8<=511 &&
              a.pullQ8>=0 && a.pullQ8<=300 && a.fan>=0.f && a.fan<=1.f && a.lean>=0.599f && a.lean<=1.401f &&
              a.fall>=0.149f && a.fall<=0.551f && a.spread>=0.249f && a.spread<=0.801f &&
              std::abs(a.leanX*a.leanX+a.leanZ*a.leanZ-1.f)<1e-3f && a.axisX*a.axisX+a.axisZ*a.axisZ>1040000 &&
              a.axisX*a.axisX+a.axisZ*a.axisZ<1056000, "a tuft's own numbers are in their ranges");
      // its own centre is nearer to it by its measure than a point a square away
      require(Grass::tuftMeasure(a,a.x,a.z)==0 && Grass::tuftMeasure(a,a.x+Grass::TuftStep,a.z)>0, "the measure grows from the centre");
      reach[0] = std::min(reach[0],float(a.reachQ8)); reach[1] = std::max(reach[1],float(a.reachQ8));
      count[0] = std::min(count[0],float(a.densityQ8)); count[1] = std::max(count[1],float(a.densityQ8));
      longest  = std::max(longest,float(a.longQ8));
      if(a.pullQ8<40)
        ++loose;
      ++all;
      for(const auto& d:{std::pair<int32_t,int32_t>{1,0},std::pair<int32_t,int32_t>{0,1}}) {
        const Grass::Tuft b = Grass::tuft(tx+d.first,tz+d.second,36000);
        // a copy: about the same size, count, height and lean at once
        const bool copy = std::abs(a.reachQ8-b.reachQ8)<24 && std::abs(a.densityQ8-b.densityQ8)<24 &&
                          std::abs(a.height-b.height)<0.08f && std::abs(a.lean-b.lean)<0.1f &&
                          (a.leanX*b.leanX+a.leanZ*b.leanZ)>0.9f && std::abs(a.longQ8-b.longQ8)<32;
        ++pairs;
        if(copy)
          ++alike;
        }
      }
  require(alike*200<=pairs, "neighbouring tufts must differ: "+std::to_string(alike)+" pairs alike of "+std::to_string(pairs));
  require(reach[0]<180.f && reach[1]>395.f && count[0]<180.f && count[1]>330.f && longest>490.f,
          "the tufts differ in size, in outline and in the count of their blades");
  const double looseShare = double(loose)/double(all);
  require(looseShare>=0.1 && looseShare<=0.3, "one tuft in five is loose: "+std::to_string(looseShare));
  // the sward is even: its tufts differ by half as much, and hardly gather
  for(int32_t tx=0; tx<200; ++tx) {
    const Grass::Tuft s = Grass::tuft(tx,7,20000);
    require(s.sward && !s.clump && s.densityQ8>=320*207/256 && s.densityQ8<=320*305/256 && s.pullQ8<=135 &&
            s.fall<=0.28f && s.height>=0.55f*0.86f && s.height<=0.65f*1.19f, "a tuft of the sward is short, dense and even");
    }
  }

// ---- the wind's front ----

void windFront() {
  const float close = 2e-3f;
  for(const uint64_t period:{uint64_t(8000),uint64_t(20000)})
    for(const float scale:{1.f,0.6f}) {
      for(uint64_t tick=200000; tick<200000+12*period; tick+=137) {
        const auto front = FoliageFront::make(tick,period,true,scale,false);
        const auto scene = FoliageWind::scene(tick,period,true,scale,false);
        require(front.k.w==1.f, "the front runs with the world's wind");
        const auto here = FoliageFront::atPlace(front,scene,0.f);
        require(std::abs(here.pressure-scene.wind.z)<=close && std::abs(here.swing-scene.swing.y)<=close &&
                std::abs(here.flutter-scene.swing.z)<=close, "at z=0 the place has the scene's wind");
        // the speed the front counts is WorldWind::sample's
        const auto   v     = WorldWind::sample(tick,period,true,scale);
        const double speed = std::hypot(double(v.x),double(v.z));
        const double mine  = std::pow(double(here.flutter),1.0/double(FoliageWind::EddyCurve))*double(FoliageWind::GustSpeed);
        require(std::abs(speed-mine)<=0.01, "the front's speed of the wind is the world's: "+std::to_string(speed)+
                " and "+std::to_string(mine));
        // a place down the wind has what the scene had when the front left z=0: 5 m/s, 2 ms per cm
        for(const float z:{1000.f,6400.f,40000.f,-2500.f}) {
          const auto then  = FoliageWind::scene(uint64_t(int64_t(tick)-int64_t(z)*2),period,true,scale,false);
          const auto there = FoliageFront::atPlace(front,scene,z);
          require(std::abs(there.pressure-then.wind.z)<=close && std::abs(there.swing-then.swing.y)<=close &&
                  std::abs(there.flutter-then.swing.z)<=close,
                  "a place has the wind the scene had z/5 m/s ago: z "+std::to_string(z)+" tick "+std::to_string(tick));
          }
        }
      // the gust comes to a place when the bench waits for it
      for(const float z:{0.f,1700.f,-3000.f,25000.f})
        for(uint64_t tick=300000; tick<300000+3*period; tick+=977) {
          const uint64_t wait = FoliageFront::untilGust(tick,period,z);
          require(double(wait)<=double(period)/FoliageFront::RateGust, "the next gust is within one of its cycles");
          const auto front = FoliageFront::make(tick+wait,period,true,scale,false);
          const auto scene = FoliageWind::scene(tick+wait,period,true,scale,false);
          const auto peak  = FoliageFront::atPlace(front,scene,z);
          const auto v     = WorldWind::sample(uint64_t(int64_t(tick+wait)-int64_t(z)*2),period,true,scale);
          const double top = (2.2+2.8)*double(scale) - 0.65*double(scale);
          require(std::hypot(double(v.x),double(v.z))>=top-0.01, "the bench's wait ends on the gust's peak at the place");
          require(scale<1.f || peak.pressure>0.999f, "at the gust's peak the grass lies over");
          }
      }

  // a gust is seen crossing: the peak comes to a place 20 m down the wind 4 s later
  const uint64_t period = 8000;
  const uint64_t first  = 300000 + FoliageFront::untilGust(300000,period,0.f);
  const uint64_t second = 300000 + FoliageFront::untilGust(300000,period,2000.f);
  const uint64_t cycle  = uint64_t(double(period)/FoliageFront::RateGust);
  const int64_t  lag    = (int64_t(second)-int64_t(first)+int64_t(cycle)) % int64_t(cycle);
  require(std::abs(lag-4000)<=2, "the front runs at 5 m/s along +Z: "+std::to_string(lag)+" ms over 20 m");

  // no front without the world's wind, with the steady legacy one, or with no scale: the scene's numbers
  const auto scene = FoliageWind::scene(123456,period,true,1.f,true);
  for(const auto& front:{FoliageFront::make(123456,period,false,1.f,false), FoliageFront::make(123456,0,true,1.f,false),
                         FoliageFront::make(123456,period,true,1.f,true),   FoliageFront::make(123456,period,true,0.f,false)}) {
    require(front.k.w==0.f, "no front without a changing wind");
    const auto there = FoliageFront::atPlace(front,scene,5000.f);
    require(there.pressure==scene.wind.z && there.swing==scene.swing.y && there.flutter==scene.swing.z,
            "without a front every place has the scene's wind");
    }
  // the last frame's phases are the frame's own until the scene sets them
  const auto front = FoliageFront::make(123456,period,true,1.f,false);
  const auto live  = FoliageWind::scene(123456,period,true,1.f,false);
  const auto now   = FoliageFront::atPlace(front,live,3000.f,false), before = FoliageFront::atPlace(front,live,3000.f,true);
  require(now.pressure==before.pressure && now.swing==before.swing, "a new front has no motion behind it");
  }

// ---- stalks with an ear, and the ground the content gives (grassear.h, grassplots.h) ----

void earRecords() {
  Grass::Params p;
  require(parsed("{}",p) && p.kind==0, "a record without `kind` is of the grass's own blades");
  require(parsed(R"({"kind":"ear","height":120})",p) && p.on && p.kind==Grass::KindEar && p.height==120.f,
          "`kind` `ear` makes the record's blades stalks with an ear");
  require(parsed(R"({"kind":"ear","height":160})",p) && parsed(R"({"kind":"ear","height":5})",p),
          "a stalk with an ear is of 5 to 160 cm");
  const Grass::Params kept = p;
  for(const char* bad:{R"({"kind":"ear","height":161})", R"({"kind":"ear","height":4})", R"({"height":120})",
                       R"({"kind":"rye"})", R"({"kind":2})", R"({"kind":null})"}) {
    Grass::Params out = kept;
    require(!parsed(bad,out) && out==kept, std::string("a kind that is not `ear` or a height out of its kind's range must be refused: ")+bad);
    }
  }

// The meadow of placementRepeats stands as it stood before the stalks and the content's ground: the roots, the
// ranks and the batches of its 25 cells, whole numbers alone (the same on every machine), against their digest
// from origin/main
void placementKept() {
  Grass::Summary flat;
  addField(flat,-1600.f,-1600.f,4800.f,4800.f,0.f,0);
  const Grass::Params par = meadow();
  uint64_t digest = 1469598103934665603ull;
  size_t   count  = 0;
  const auto mix = [&](uint32_t v) { digest = (digest ^ v)*1099511628211ull; };
  for(int32_t cz=-1; cz<4; ++cz)
    for(int32_t cx=-1; cx<4; ++cx) {
      const auto cell = build(flat,cx,cz,par);
      count += cell.blades.size();
      for(const auto& b:cell.blades) {
        mix(b.w[0] & 0x0FFFFFFFu);
        mix(b.w[1] & 0xFF00FFFFu);
        }
      for(const auto& bt:cell.batches)
        for(uint32_t v:{uint32_t(bt.slot),uint32_t(bt.kind),uint32_t(bt.square),bt.first,bt.count})
          mix(v);
      }
  require(count==563245u && digest==0xb1cc833359aa51a7ull,
          "a meadow without `kind` and without the content's ground must keep its blades: "+std::to_string(count)+" "+std::to_string(digest));
  }

// 16 m of stalks (slot 1) beside 16 m of meadow (slot 0)
void placementEar() {
  Grass::Params par[2] = {meadow(),meadow()};
  par[1].kind    = Grass::KindEar;
  par[1].height  = 120.f;
  par[1].density = 1.25f;
  Grass::Summary ground;
  addField(ground,0.f,0.f,1600.f,1600.f,0.f,1);
  addField(ground,1600.f,0.f,3200.f,1600.f,0.f,0);
  const Grass::Summary* list[1] = {&ground};

  size_t stalks = 0, grass = 0;
  float  low = 1e9f, high = 0.f;
  for(int32_t cz=0; cz<2; ++cz)
    for(int32_t cx=0; cx<4; ++cx) {
      const auto cell = build(list,cx,cz,par);
      require(same(cell,build(list,cx,cz,par)), "two builds of a cell of stalks must give the same blades"+at(cx,cz));
      for(const auto& bt:cell.batches) {
        require(bt.kind==(bt.slot==1 ? Grass::KindEar : 0), "the stalks are a kind of their own, the meadow beside keeps its"+at(cx,cz));
        // an even stand: every square of 2 m has the record's count, 400 a m2
        require(bt.slot!=1 || cx>1 || (bt.count>1440 && bt.count<1760), "stalks stand evenly, without tufts or bare spots"+at(cx,cz));
        for(uint32_t i=bt.first; i<bt.first+bt.count; ++i) {
          const auto& b    = cell.blades[i];
          const auto  info = Grass::decode(b,cx,cz,cell.minY,bt.kind);
          // a root on the line between the two is of either
          require((bt.slot==1 ? info.x<=1600*U : info.x>=1600*U) && info.y==0, "each kind roots on its own ground"+at(cx,cz));
          if(bt.slot!=1) {
            ++grass;
            continue;
            }
          ++stalks;
          low  = std::min(low,info.height);
          high = std::max(high,info.height);
          require((b.w[0] >> 28)<=4u, "a stalk stands nearly upright: 8 to 18 degrees"+at(cx,cz));
          }
        }
      }
  // 0.9..1.1 of the record's 120 cm, and up to a fifth lower by spots: 86..132 cm, to a step of the byte
  require(low>=86.f && high<=132.4f && low<96.f && high>124.f,
          "stalks are of the record's height: "+std::to_string(low)+".."+std::to_string(high));
  require(stalks>100000 && stalks<104800 && grass>0, "256 m2 of stalks at 400 a m2: "+std::to_string(stalks));
  }

// A file of worlds/<WORLD>/grass.d
void plots() {
  const auto slotOf = [](std::string_view material) { return material=="OWN_CROP_STAND.TGA" ? uint16_t(1) : uint16_t(-1); };
  // a patch over the cell (1,2) of the chunk (0,0), y = 250 + (x-800)/10, its triangles wound either way; one of
  // the chunk (1,0)
  const char* file = R"({"format": 1, "patches": [
    {"material": "OWN_CROP_STAND.TGA",
     "positions": [800,250,1600, 1600,330,1600, 1600,330,2400, 800,250,2400],
     "uvs": [0,0, 1,0, 1,1, 0,1], "triangles": [0,1,2, 0,3,2]},
    {"material": "OWN_CROP_STAND.TGA",
     "positions": [6500,0,100, 7300,0,100, 7300,0,900], "uvs": [0,0, 1,0, 1,1], "triangles": [0,1,2]}]})";
  Grass::Plots plots;
  std::string  error;
  require(plots.empty(), "no file: no ground of the content");
  require(plots.parse(file,slotOf,error) && error.empty() && !plots.empty(), "a file of patches must be taken: "+error);
  Grass::Summary ground, next, none;
  plots.addTo(ground,ChunkId::fromPosition(1200.f,2000.f));
  plots.addTo(next,  ChunkId{1,0});
  plots.addTo(none,  ChunkId{0,1});
  require(ground.grass.size()==2 && next.grass.size()==1 && none.empty(), "a patch's triangle is of the chunk its centre is in");
  for(const auto& t:ground.grass)
    require(t.slot==1 && t.ny[0]>0.99f && t.nx[0]<0.f && t.nx[0]>-0.11f, "a patch's triangle faces up whatever its winding");

  // its stalks stand on it and nowhere else
  Grass::Params par[2] = {meadow(),meadow()};
  par[1].kind   = Grass::KindEar;
  par[1].height = 120.f;
  const Grass::Summary* list[1] = {&ground};
  size_t stalks = 0;
  for(int32_t cz=0; cz<5; ++cz)
    for(int32_t cx=-1; cx<4; ++cx) {
      const auto cell = build(list,cx,cz,par);
      stalks += cell.blades.size();
      for(const auto& bt:cell.batches)
        for(uint32_t i=bt.first; i<bt.first+bt.count; ++i) {
          const auto info = Grass::decode(cell.blades[i],cx,cz,cell.minY,bt.kind);
          require(bt.slot==1 && bt.kind==Grass::KindEar && info.x>=800*U && info.x<=1600*U && info.z>=1600*U && info.z<=2400*U &&
                  std::abs(info.y - (250*U + (info.x-800*U)/10))<=3, "a stalk stands on its patch, at the patch's height"+at(cx,cz));
          require(info.height>=86.f && info.height<=132.4f, "a stalk of a patch is of the record's height"+at(cx,cz));
          }
      }
  // 64 m2 at the record's 320 a m2
  require(stalks>19400 && stalks<21500, "stalks grow on the content's ground: "+std::to_string(stalks));

  // a broken file is refused whole
  for(const char* bad:{"", "{", "[]", R"({"format":2,"patches":[]})", R"({"format":1})", R"({"format":1,"patches":{}})",
        R"({"format":1,"patches":[{"positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,2]}]})",
        R"({"format":1,"patches":[{"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,2]}]})",
        R"({"format":1,"patches":[{"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0,"1"],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,2]}]})",
        R"({"format":1,"patches":[{"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0],"triangles":[0,1,2]}]})",
        R"({"format":1,"patches":[{"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1]}]})",
        R"({"format":1,"patches":[{"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,3]}]})",
        R"({"format":1,"patches":[{"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,2]},
                                 {"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,-2]}]})"}) {
    Grass::Plots broken;
    require(!broken.parse(bad,slotOf,error) && !error.empty() && broken.empty(), std::string("a broken file of patches must be refused whole: ")+bad);
    }

  // a patch of a material without `grass` is left out and named; the file's other patches are taken
  Grass::Plots   part;
  Grass::Summary taken;
  require(part.parse(R"({"format":1,"patches":[
      {"material":"NO_SUCH.TGA","positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,2]},
      {"material":"OWN_CROP_STAND.TGA","positions":[0,0,0, 100,0,0, 0,0,100],"uvs":[0,0, 1,0, 0,1],"triangles":[0,1,2]}]})",slotOf,error) &&
          error.find("NO_SUCH.TGA")!=std::string::npos, "a patch of an unknown material must be named");
  part.addTo(taken,ChunkId{0,0});
  require(taken.grass.size()==1 && taken.grass[0].slot==1, "a patch of an unknown material alone is left out");

  // the folder: its *.json, a broken one among them; none without the folder
  const auto dir = std::filesystem::temp_directory_path()/("gothic-grass-test-"+std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  Grass::Plots absent, read;
  absent.load(dir,slotOf);
  require(absent.empty(), "a world without grass.d has no ground of the content");
  std::filesystem::create_directories(dir/"grass.d");
  std::ofstream(dir/"grass.d"/"a-broken.json") << "{";
  std::ofstream(dir/"grass.d"/"b-field.json")  << file;
  std::ofstream(dir/"grass.d"/"c-notes.txt")   << file;
  read.load(dir,slotOf);
  std::filesystem::remove_all(dir);
  Grass::Summary fromDir;
  read.addTo(fromDir,ChunkId{0,0});
  require(fromDir.grass.size()==2, "the files of grass.d are read, a broken one among them is passed by");
  }

}

int main() {
  try {
    environmentFallbacks();
    records();
    bucketBits();
    switches();
    patches();
    tufts();
    require(unsetenv("OPENGOTHIC_GRASS_ORDER")==0, "cannot clear grass order switch");
    drawOrder(false);
    packSpans();
    packIndices();
    placementRepeats();
    placementGround();
    placementEdge();
    placementItems();
    earRecords();
    placementKept();
    placementEar();
    plots();
    windFront();
    assertions += grassCoverTests();
    std::cout << "{\"test\":\"grass\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& e) {
    std::cerr << "grass test failed: " << e.what() << '\n';
    return 1;
    }
  }
