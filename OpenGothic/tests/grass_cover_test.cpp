// The meadow's own ground (reports/engine/2026-11-14-meadow-ground): the records' numbers, the rule of the cover,
// the border with a verge, the blades on them. Synthetic ground, no device and no game data. Part of
// gothic-grass-test (grass_test.cpp calls grassCoverTests).

#include "graphics/grass/grassborder.h"
#include "graphics/grass/grasscover.h"
#include "graphics/grass/grassground.h"
#include "graphics/grass/grassparams.h"
#include "graphics/grass/grasspatch.h"
#include "graphics/grass/grassplace.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

constexpr int32_t U = Grass::Unit;

template<class T>
bool parsed(const char* json, T& out) {
  rapidjson::Document d;
  d.Parse(json);
  require(!d.HasParseError(), std::string("the test's own json: ")+json);
  return Grass::parse(d,out);
  }

// ---- the records ----

void records() {
  Grass::Ground g;
  require(parsed(R"({"sward":{"albedo":"a.dds"},"earth":{}})",g) && g.on && g.swardTile==4.f && g.earthTile==2.f && g.far==5.3f,
          "a ground with its two layers takes the defaults");
  require(parsed(R"({"sward":{"tile_m":6},"earth":{"tile_m":1.5},"far":0})",g) && g.swardTile==6.f && g.earthTile==1.5f && g.far==0.f,
          "a ground carries the record's numbers");
  const Grass::Ground kept = g;
  for(const char* bad:{"1", "[]", "{}", R"({"sward":{}})", R"({"earth":{}})", R"({"sward":1,"earth":{}})",
                       R"({"sward":{"tile_m":0.1},"earth":{}})", R"({"sward":{},"earth":{"tile_m":65}})",
                       R"({"sward":{"tile_m":"4"},"earth":{}})", R"({"sward":{},"earth":{},"far":1})",
                       R"({"sward":{},"earth":{},"far":17})", R"({"sward":{},"earth":{},"far":"5"})"}) {
    Grass::Ground out = kept;
    require(!parsed(bad,out) && out==kept, std::string("a ground of a wrong shape is refused and leaves the numbers alone: ")+bad);
    }

  Grass::Verge v;
  require(parsed("{}",v) && v.on && v.reach==90.f && v.margin==50.f, "a verge takes the defaults");
  require(parsed(R"({"of":"X.TGA","reach":120,"margin":0})",v) && v.reach==120.f && v.margin==0.f, "a verge carries the record's numbers");
  const Grass::Verge keptV = v;
  for(const char* bad:{"1", "true", R"({"reach":121})", R"({"reach":-1})", R"({"margin":101})", R"({"margin":"50"})"}) {
    Grass::Verge out = keptV;
    require(!parsed(bad,out) && out==keptV, std::string("a verge of a wrong shape is refused: ")+bad);
    }

  Grass::Params p;
  require(parsed("{}",p) && !p.fine && !p.cover && p.tile==0.f, "grass without `fine` has the blades it had");
  require(p.broad==Grass::BroadShare && parsed(R"({"broad":0.25})",p) && p.broad==0.25f && parsed(R"({"broad":0})",p) && p.broad==0.f &&
          parsed(R"({"broad":1})",p) && p.broad==1.f, "`broad` is a share, the default where the record has none");
  for(const char* bad:{R"({"broad":1.5})", R"({"broad":-0.1})", R"({"broad":"1"})", R"({"broad":true})"})
    require(!parsed(bad,p) && p.broad==1.f, std::string("`broad` of another value is refused: ")+bad);
  require(parsed(R"({"fine":1})",p) && p.fine && parsed(R"({"fine":0})",p) && !p.fine, "`fine` is 0 or 1");
  for(const char* bad:{R"({"fine":2})", R"({"fine":true})", R"({"fine":0.5})"})
    require(!parsed(bad,p), std::string("`fine` of another value is refused: ")+bad);
  require(Grass::coverBeyond(90)==145 && Grass::MaxVergeCm==175 && float(Grass::MaxVergeCm)<=Grass::BorderPad,
          "the ground's shader finds a border from as far as the sward comes over it");
  }

// ---- the rule of the cover ----

// The spots of bare earth over a hectare: about a hundredth of the ground, outlines no circle
void spots() {
  constexpr int32_t N = 2500, Step = 4*U;
  auto cell = [](int32_t x, int32_t z) { return size_t(z)*size_t(N) + size_t(x); };
  for(const auto& from:{std::pair<int32_t,int32_t>{40000,-3000}, std::pair<int32_t,int32_t>{-20000,10000}}) {
    std::vector<uint8_t> bare(size_t(N)*size_t(N));
    size_t count = 0, thin = 0, gaps = 0, isles = 0, rim = 0;
    for(int32_t j=0; j<N; ++j)
      for(int32_t i=0; i<N; ++i) {
        const int32_t x = from.first*U + i*Step, z = from.second*U + j*Step;
        const int32_t b = Grass::coverBare(x,z);
        require(b>=0 && b<=256, "the bare earth is a share of 256");
        if((i+j)%7==0) {
          const int32_t clear = Grass::coverClear(x,z), level = Grass::coverLevel(x,z,256);
          require(clear>=0 && clear<=256 && (b==0 || clear==0), "no place where a spot's sward thins is clear of the spot");
          require(b==0 ? level==256 : (b>=256 ? level==0 : std::abs(level-(256-b))<=Grass::CoverTuft+Grass::CoverIsles),
                  "the sward is whole away from a spot, none on it, broken up between the two");
          if(b>0 && b<256) {
            ++thin;
            if(b<Grass::CoverSward && level<=Grass::CoverSward)
              ++gaps;
            if(b>Grass::CoverSward && level>Grass::CoverSward)
              ++isles;
            }
          if(clear>0 && clear<256)
            ++rim;
          }
        bare[cell(i,j)] = b>Grass::CoverSward ? 1 : 0;
        count += bare[cell(i,j)];
        }
    const double share = double(count)/double(bare.size());
    require(share>0.003 && share<0.015, "the bare earth is a quiet accent, about a hundredth of the meadow: "+std::to_string(share*100.0));
    // towards a spot the sward breaks up: earth shows in its half, isles of sward stand in the earth's half
    require(thin>1000 && gaps*30>thin && isles*30>thin && rim>200, "a spot has no line for an edge: "+std::to_string(thin)+" places of thinning sward, "+
            std::to_string(gaps)+" of earth in the sward's half, "+std::to_string(isles)+" of sward in the earth's, "+std::to_string(rim)+" beside it");

    // each spot's outline against the circle of its area; cells' sides count as 4/pi of a line's length
    std::vector<int32_t> stack;
    double sum = 0;
    size_t spots = 0;
    for(size_t s=0; s<bare.size(); ++s) {
      if(bare[s]!=1)
        continue;
      size_t area = 0, sides = 0;
      stack.push_back(int32_t(s));
      bare[s] = 2;
      while(!stack.empty()) {
        const int32_t at = stack.back();
        stack.pop_back();
        ++area;
        const int32_t x = at%N, z = at/N;
        const int32_t next[4][2] = {{x-1,z},{x+1,z},{x,z-1},{x,z+1}};
        for(const auto& n:next) {
          if(n[0]<0 || n[1]<0 || n[0]>=N || n[1]>=N || bare[cell(n[0],n[1])]==0) {
            ++sides;
            continue;
            }
          uint8_t& b = bare[cell(n[0],n[1])];
          if(b==1) {
            b = 2;
            stack.push_back(n[1]*N + n[0]);
            }
          }
        }
      const double a = double(area)*16.0; // cm2
      if(a<2000.0)
        continue;
      sum += double(sides)*4.0*(M_PI/4.0)/(2.0*std::sqrt(M_PI*a));
      ++spots;
      }
    require(spots>20, "there are spots of bare earth in a hectare: "+std::to_string(spots));
    require(sum/double(spots)>=1.4, "a spot's outline is about half as long again as a circle's: "+std::to_string(sum/double(spots)));
    }
  }

// The line where the sward ends at a border: wanders over more than 60 cm, no stretch of it straight
void borderLine() {
  for(const int32_t x:{41000, 43000, 47000, -5000}) {
    std::vector<int32_t> line;
    for(int32_t z=300; z<2300; ++z) {
      // the furthest of the sward: coverBorder falls with the distance past the border
      int32_t at = -Grass::CoverInside, prev = 256;
      for(int32_t s=-Grass::CoverInside; s<=Grass::coverBeyond(90); ++s) {
        const int32_t c = Grass::coverBorder(x*U,z*U,s*U,90);
        require(c>=0 && c<=256 && c<=prev, "the sward at a border thins with the way past it");
        prev = c;
        if(c>=Grass::CoverSward)
          at = s;
        }
      line.push_back(at);
      }
    const auto [lo,hi] = std::minmax_element(line.begin(),line.end());
    require(*hi-*lo>=60, "the border's line wanders over 60 cm and more: "+std::to_string(*hi-*lo));
    require(*lo<0 && *hi>30 && *hi<=Grass::coverBeyond(90), "bays of earth in the meadow, tongues of sward over the road");
    size_t run = 0;
    for(size_t i=0; i<line.size();) {
      size_t j = i;
      while(j+1<line.size() && std::abs(line[j+1]-line[i])<4)
        ++j;
      run = std::max(run,j-i);
      i   = std::max(j,i+1);
      }
    require(run<150, "no stretch of the border's line over 1.5 m is straight: "+std::to_string(run));
    }
  require(Grass::coverBorder(100,200,-Grass::CoverInside*U,90)==256 && Grass::coverBorder(100,200,Grass::coverBeyond(90)*U,90)==0 &&
          Grass::coverBorder(100,200,Grass::coverBeyond(120)*U,120)==0,
          "the border is not felt deep in the meadow, no sward past the reach");
  require(Grass::coverLevel(100,200,0)==0 && Grass::coverBlades(Grass::CoverBlades)==0 && Grass::coverBlades(256)==256 &&
          Grass::coverBlades(Grass::CoverSward)>0, "blades stand from under the level where the ground turns sward");
  require(Grass::coverFine(0,256)==Grass::CoverSparse && Grass::coverFine(0,0)==0 && Grass::coverFine(256,256)==256 &&
          Grass::coverFine(0,Grass::CoverBlades)==0 && Grass::coverFine(200,256)==Grass::coverBlades(200),
          "a few fine blades stand on a spot of bare earth, none on the earth of a border");
  }

// ---- the ground of the tests ----

// A level field of squares of 2 m
void addField(Grass::Summary& out, float x0, float z0, float x1, float z1, uint16_t slot) {
  const float up[3][3] = {{0,1,0},{0,1,0},{0,1,0}};
  for(float z=z0; z<z1; z+=200.f)
    for(float x=x0; x<x1; x+=200.f) {
      const float a[3][3] = {{x,0,z},{x+200.f,0,z},{x+200.f,0,z+200.f}};
      const float b[3][3] = {{x,0,z},{x+200.f,0,z+200.f},{x,0,z+200.f}};
      const float ua[3][2] = {{x/400.f,z/400.f},{(x+200.f)/400.f,z/400.f},{(x+200.f)/400.f,(z+200.f)/400.f}};
      const float ub[3][2] = {{x/400.f,z/400.f},{(x+200.f)/400.f,(z+200.f)/400.f},{x/400.f,(z+200.f)/400.f}};
      Grass::addGrass(out,a,ua,up,slot);
      Grass::addGrass(out,b,ub,up,slot);
      }
  }

// A road of squares of 4 m; verge: of the grass `slot`
void addRoad(Grass::Summary& out, float x0, float z0, float x1, float z1, bool verge, uint16_t slot = 0) {
  const float up[3][3] = {{0,1,0},{0,1,0},{0,1,0}};
  for(float z=z0; z<z1; z+=400.f)
    for(float x=x0; x<x1; x+=400.f) {
      const float a[3][3] = {{x,0,z},{x+400.f,0,z},{x+400.f,0,z+400.f}};
      const float b[3][3] = {{x,0,z},{x+400.f,0,z+400.f},{x,0,z+400.f}};
      Grass::addBeside(out,a,up);
      Grass::addBeside(out,b,up);
      if(verge) {
        Grass::addVerge(out,a,up,slot,90.f,50.f);
        Grass::addVerge(out,b,up,slot,90.f,50.f);
        }
      }
  }

Grass::Params meadow(bool cover, bool fine, float broad = 0.f) {
  Grass::Params p;
  p.on    = true;
  p.edge  = 150.f;
  p.fine  = fine;
  p.broad = broad;
  p.cover = cover;
  p.tile  = cover ? 400.f : 0.f;
  return p;
  }

Grass::CellBlades build(std::span<const Grass::Summary*const> ground, int32_t cx, int32_t cz, const Grass::Params& p,
                        uint32_t rankLimitFine = 256) {
  Grass::CellInput in;
  in.cx            = cx;
  in.cz            = cz;
  in.summaries     = ground;
  in.params        = std::span<const Grass::Params>(&p,1);
  in.rankLimitFine = rankLimitFine;
  return Grass::buildCell(in);
  }

bool same(const Grass::CellBlades& a, const Grass::CellBlades& b) {
  if(a.minY!=b.minY || a.maxY!=b.maxY || a.batches.size()!=b.batches.size() || a.blades.size()!=b.blades.size())
    return false;
  for(size_t i=0; i<a.blades.size(); ++i)
    if(!std::equal(a.blades[i].w,a.blades[i].w+4,b.blades[i].w))
      return false;
  for(size_t i=0; i<a.batches.size(); ++i)
    if(a.batches[i].slot!=b.batches[i].slot || a.batches[i].kind!=b.batches[i].kind ||
       a.batches[i].first!=b.batches[i].first || a.batches[i].count!=b.batches[i].count)
      return false;
  return true;
  }

// ---- the blades on the cover ----

// A meadow of 32 m with a road along its side x = 16 m
void placement() {
  Grass::Summary field, plainRoad, vergeRoad;
  addField(field,-1600.f,-1600.f,1600.f,3200.f,0);
  plainRoad = field;
  vergeRoad = field;
  addRoad(plainRoad,1600.f,-1600.f,2400.f,3200.f,false);
  addRoad(vergeRoad,1600.f,-1600.f,2400.f,3200.f,true);
  require(vergeRoad.verge.size()==2u*2u*12u && plainRoad.verge.empty() && vergeRoad.beside.size()==plainRoad.beside.size(),
          "a road with `verge` is kept whole beside the meadow");
  const float side[3][3]  = {{1,0,0},{1,0,0},{1,0,0}};
  const float stand[3][3] = {{1600,0,-600},{1600,0,2400},{1600,300,2400}};
  Grass::Summary wall = field;
  Grass::addVerge(wall,stand,side,0,90.f,50.f);
  require(wall.verge.empty(), "a wall is no verge");

  const int32_t         border = 1600*U, deep = 1400*U;
  const Grass::Summary* plain[1] = {&plainRoad};
  const Grass::Summary* verge[1] = {&vergeRoad};
  const Grass::Summary* alone[1] = {&field};
  size_t onSpot = 0, onSpotFine = 0, onWholeFine = 0, sward = 0, fineCount = 0, coarse = 0, past = 0, bay = 0, bayPlain = 0;
  int32_t furthest = 0;
  for(int32_t cz=-1; cz<3; ++cz)
    for(int32_t cx=-1; cx<3; ++cx) {
      const std::string at = " (cell "+std::to_string(cx)+","+std::to_string(cz)+")";
      // without the cover a road with or without `verge` in the summary is the same ground beside the grass
      const auto old = build(plain,cx,cz,meadow(false,false));
      require(same(old,build(plain,cx,cz,meadow(false,true))), "`fine` without the ground's layers changes no blade"+at);
      for(const auto& b:old.batches)
        require(b.kind==0, "no fine blades without the ground's layers"+at);

      const auto cover = build(verge,cx,cz,meadow(true,false));
      const auto fine  = build(verge,cx,cz,meadow(true,true));
      require(same(cover,build(verge,cx,cz,meadow(true,false))) && same(fine,build(verge,cx,cz,meadow(true,true))),
              "the blades on the cover repeat"+at);
      for(const auto& b:cover.batches)
        require(b.kind==0, "no fine blades without `fine`"+at);
      for(const auto& batch:fine.batches)
        for(uint32_t i=0; i<batch.count; ++i) {
          const auto& w = fine.blades[batch.first+i];
          const auto  b = Grass::decode(w,cx,cz,fine.minY);
          // the cover at the root, as the ground's shader counts it
          const int32_t edge = Grass::coverBorder(b.x,b.z,b.x-border,90);
          const int32_t level = Grass::coverLevel(b.x,b.z,edge);
          if(batch.kind==1)
            require(Grass::coverFine(level,Grass::coverEdge(b.x,b.z,edge))>0, "a fine blade stands on sward, on its rim or on a spot of bare earth"+at); else
            require(level>Grass::CoverBlades, "a blade stands where the cover has sward or its rim"+at);
          if(Grass::coverBare(b.x,b.z)>=256) {
            if(batch.kind==0)
              ++onSpot;
            else if(b.x<deep)
              ++onSpotFine;
            }
          if(batch.kind==1 && b.x<deep && Grass::coverBare(b.x,b.z)==0 && Grass::patchSward(Grass::patchNoise(b.x,b.z)))
            ++onWholeFine;
          if(batch.kind==1) {
            ++fineCount;
            require(((w.w[3] >> 24) & 0xFu)<=7u, "a fine blade's width is of three bits"+at);
            } else {
            ++coarse;
            }
          if(b.x>border) {
            ++past;
            furthest = std::max(furthest,(b.x-border)/U);
            require(batch.kind==1 && b.height<=Grass::PastHeight && b.y==0, "a blade on the verge is fine, low and stands on the road"+at);
            } else if(level>=256) {
            ++sward;
            }
          if(b.x<=border && b.x>border-20*U)
            ++bay;
          }
      for(const auto& w:old.blades)
        if(const auto b = Grass::decode(w,cx,cz,old.minY); b.x<=border && b.x>border-20*U)
          ++bayPlain;

      // a cell far from the camera: fewer fine blades, the rest as it is
      const auto few = build(verge,cx,cz,meadow(true,true),96);
      size_t n = 0, all = 0;
      for(const auto& batch:few.batches)
        for(uint32_t i=0; i<batch.count; ++i)
          if(batch.kind==1) {
            require((few.blades[batch.first+i].w[1] >> 24)<96u, "a far cell keeps the fine blades of the low ranks"+at);
            ++n;
            }
      for(const auto& batch:fine.batches)
        if(batch.kind==1)
          all += batch.count;
      require(all==0 || (n>all/4 && n<all/2), "a far cell has 3/8 of the fine blades: "+std::to_string(n)+" of "+std::to_string(all)+at);
      const auto none = build(verge,cx,cz,meadow(true,true),0);
      for(const auto& b:none.batches)
        require(b.kind==0, "a cell past the fine blades' ring has none"+at);

      // deep in the meadow the road does not matter
      if(cx<1)
        require(same(fine,build(alone,cx,cz,meadow(true,true))), "the blades 8 m from the border do not know of it"+at);
      }
  require(onSpot==0, "no blade of the main grass in a spot of bare earth");
  // the fine blades per area of the spots against the whole short sward's, away from the border: CoverSparse of 256
  size_t spotArea = 0, wholeArea = 0;
  for(int32_t z=-800; z<2400; z+=4)
    for(int32_t x=-800; x<1400; x+=4) {
      const int32_t bare = Grass::coverBare(x*U,z*U);
      if(bare>=256)
        ++spotArea;
      else if(bare==0 && Grass::patchSward(Grass::patchNoise(x*U,z*U)))
        ++wholeArea;
      }
  require(spotArea>200 && wholeArea>50000 && onSpotFine>50 && onWholeFine>10000, "the meadow has spots and short sward to count on: "+
          std::to_string(spotArea)+", "+std::to_string(wholeArea)+", "+std::to_string(onSpotFine)+", "+std::to_string(onWholeFine));
  const double sparse = (double(onSpotFine)/double(spotArea))/(double(onWholeFine)/double(wholeArea));
  require(sparse>0.08 && sparse<0.45, "a spot of bare earth keeps about a fifth of the fine blades: "+std::to_string(sparse));
  require(sward>100000 && fineCount>20000 && coarse>50000, "the meadow has its grass: "+std::to_string(sward)+" blades, "+
          std::to_string(fineCount)+" fine");
  require(past>500 && furthest>=60 && furthest<=Grass::coverBeyond(90),
          "the sward comes onto the verge in tongues: "+std::to_string(past)+" blades, to "+std::to_string(furthest)+" cm");
  require(bay>0 && bayPlain>0, "the grass stands at the border");
  }

// The short sward's broad blades among its fine ones (Params::broad): in whole sward only, the fine ones stay
void broadBlades() {
  Grass::Summary ground;
  addField(ground,-1600.f,-1600.f,1600.f,3200.f,0);
  addRoad(ground,1600.f,-1600.f,2400.f,3200.f,true);
  const Grass::Summary* verge[1] = {&ground};
  const int32_t border = 1600*U;
  auto key = [](const Grass::Blade& b) { return std::array<uint32_t,4>{b.w[0],b.w[1],b.w[2],b.w[3]}; };
  size_t whole = 0, half = 0, without = 0, fineWithout = 0, fineWhole = 0, atBorder = 0;
  for(int32_t cz=-1; cz<3; ++cz)
    for(int32_t cx=-1; cx<3; ++cx) {
      const std::string at = " (cell "+std::to_string(cx)+","+std::to_string(cz)+")";
      const auto none = build(verge,cx,cz,meadow(true,true,0.f));
      const auto all  = build(verge,cx,cz,meadow(true,true,1.f));
      const auto some = build(verge,cx,cz,meadow(true,true,0.5f));
      const auto old  = build(verge,cx,cz,meadow(true,false,1.f));
      require(same(all,build(verge,cx,cz,meadow(true,true,1.f))), "the mixed sward repeats"+at);
      require(same(old,build(verge,cx,cz,meadow(true,false,0.f))) &&
              same(build(verge,cx,cz,meadow(false,true,1.f)),build(verge,cx,cz,meadow(false,false,0.f))),
              "`broad` changes no blade without the fine ones"+at);
      // the blades of the main grass and the clumps are the same in every one of them
      std::vector<std::array<uint32_t,4>> main;
      for(const auto& batch:none.batches)
        for(uint32_t i=0; i<batch.count; ++i) {
          if(batch.kind==0)
            main.push_back(key(none.blades[batch.first+i])); else
            ++fineWithout;
          }
      std::sort(main.begin(),main.end());
      auto count = [&](const Grass::CellBlades& cell, bool check) {
        size_t n = 0, mainSeen = 0;
        for(const auto& batch:cell.batches)
          for(uint32_t i=0; i<batch.count; ++i) {
            const auto& w = cell.blades[batch.first+i];
            if(batch.kind!=0)
              continue;
            if(std::binary_search(main.begin(),main.end(),key(w))) {
              ++mainSeen;
              continue;
              }
            ++n;
            if(!check)
              continue;
            const auto    b     = Grass::decode(w,cx,cz,cell.minY);
            const int32_t level = Grass::coverLevel(b.x,b.z,Grass::coverBorder(b.x,b.z,b.x-border,90));
            // its node is CoverBroadFrom from the border and more, its tuft gathers it by a part of the way
            require(b.x<=border-20*U && level>=256 && Grass::coverClear(b.x,b.z)>0 && Grass::coverBare(b.x,b.z)==0,
                    "a broad blade of the sward stands in whole sward, clear of the spots and of the border"+at);
            require(((w.w[3] >> 24) & 0xFu)>=3u, "a broad blade of the sward is as broad as it was"+at);
            if(b.x>border-Grass::CoverBroadFrom*U)
              ++atBorder;
            }
        require(mainSeen==main.size(), "the main grass does not know of the sward's mix"+at);
        return n;
        };
      whole += count(all,true);
      half  += count(some,true);
      for(const auto& batch:all.batches)
        if(batch.kind==1)
          fineWhole += batch.count;
      // the sward's blades without the fine ones: every broad one of the mix is one of them
      size_t was = 0;
      for(const auto& batch:old.batches)
        was += batch.kind==0 ? batch.count : 0;
      require(was>=main.size(), "the sward had its blades before the fine ones"+at);
      without += was-main.size();
      // a node is of one kind at any distance: a far cell has the same broad blades, fewer of the fine ones
      const auto far = build(verge,cx,cz,meadow(true,true,0.5f),96);
      require(count(far,false)==count(some,false), "the two kinds do not change places with the distance"+at);
      const auto past = build(verge,cx,cz,meadow(true,true,1.f),0);
      for(const auto& b:past.batches)
        require(b.kind==0, "a cell past the fine blades' ring has none"+at);
      require(count(past,false)==count(all,false), "past the fine blades' ring the sward keeps its broad ones"+at);
      }
  require(whole>5000 && whole<=without && whole*10>without*5, "with `broad` 1 the sward has the broad blades it had, but for the spots and "
          "the borders: "+std::to_string(whole)+" of "+std::to_string(without));
  require(half*10>whole*4 && half*10<whole*6, "`broad` is the share of them: "+std::to_string(half)+" of "+std::to_string(whole));
  require(fineWhole*10>fineWithout*2 && fineWhole<fineWithout, "the fine blade stays in the mix: "+std::to_string(fineWhole)+" of "+
          std::to_string(fineWithout));
  require(atBorder*50<whole, "hardly a broad blade of the sward within "+std::to_string(Grass::CoverBroadFrom)+" cm of the border: "+
          std::to_string(atBorder)+" of "+std::to_string(whole));
  }

// The pieces of the border: found from the chunks, packed for the shader, read back
void borders() {
  // two chunks of meadow side by side, a road along both
  Grass::Summary west, east, road;
  addField(west,-1600.f,0.f,0.f,1600.f,0);
  addField(east,0.f,0.f,1600.f,1600.f,0);
  addRoad(road,-1600.f,1600.f,1600.f,2400.f,true);
  Grass::Summary plain;
  addRoad(plain,-1600.f,1600.f,1600.f,2400.f,false);

  // the finder: a field alone has its four sides open, the one along the road is the verge's
  std::vector<const Grass::Tri*> tris;
  for(const auto& t:west.grass) tris.push_back(&t);
  for(const auto& t:east.grass) tris.push_back(&t);
  std::sort(tris.begin(),tris.end(),[](const Grass::Tri* l, const Grass::Tri* r) { return Grass::triLess(*l,*r); });
  std::vector<const Grass::VergeTri*> strip;
  for(const auto& t:road.verge) strip.push_back(&t);
  const int32_t lo[2] = {-1600*U,0}, hi[2] = {1600*U,1600*U};
  std::vector<Grass::OpenEdge> open, openVerge;
  Grass::openEdges(tris,std::span<const Grass::VergeTri*const>(),lo,hi,0,open);
  Grass::openEdges(tris,strip,lo,hi,0,openVerge);
  auto length = [](const std::vector<Grass::OpenEdge>& e, bool verge) {
    double sum = 0;
    for(const auto& s:e)
      if(s.verge==verge)
        sum += std::hypot(double(s.bx-s.ax),double(s.bz-s.az));
    return sum;
    };
  require(std::abs(length(open,false)-9600.0)<1.0 && length(open,true)==0.0, "a field's open edges are its outline");
  require(open.size()==openVerge.size() && std::abs(length(openVerge,true)-3200.0)<1.0 && std::abs(length(openVerge,false)-6400.0)<1.0,
          "the side along a road with `verge` is the border, the others are as they were");
  for(const auto& s:openVerge)
    if(s.verge)
      require(s.az==1600 && s.bz==1600 && s.reach==90 && s.margin==50 && s.outZ>0.99f, "the border lies on the mesh's edge");

  const Grass::Summary* all[3]   = {&west,&east,&road};
  const Grass::Summary* other[3] = {&road,&east,&west};
  const Grass::Summary* none[3]  = {&west,&east,&plain};
  const auto w = Grass::chunkBorder(west,all), e = Grass::chunkBorder(east,all);
  auto total = [](const std::vector<Grass::BorderSeg>& s) {
    double sum = 0;
    for(const auto& i:s)
      sum += std::hypot(double(i.bx-i.ax),double(i.bz-i.az));
    return sum;
    };
  require(std::abs(total(w)-1600.0)<1.0 && std::abs(total(e)-1600.0)<1.0, "each chunk has its own half of the border");
  const auto w2 = Grass::chunkBorder(west,other);
  require(w.size()==w2.size() && total(w)==total(w2), "the order of the chunks does not matter");
  require(Grass::chunkBorder(west,none).empty() && Grass::chunkBorder(road,all).empty() &&
          Grass::chunkBorder(west,std::span<const Grass::Summary*const>(all,2)).empty(),
          "no border without a verge, none of a chunk without grass");

  std::vector<Grass::BorderSeg> segs = w;
  segs.insert(segs.end(),e.begin(),e.end());
  const auto grid = Grass::packBorder(segs);
  require(grid.size()>4+size_t(Grass::BorderSide)*Grass::BorderSide && grid[0]==Grass::BorderSide && grid[1]==uint32_t(Grass::BorderBin),
          "the buffer has its head and its table");
  const auto empty = Grass::packBorder(std::span<const Grass::BorderSeg>());
  require(empty.size()==4 && empty[0]==0 && !Grass::nearestBorder(empty,0.f,1600.f,180.f).found, "no pieces: a head alone");
  for(float x=-1590.f; x<1600.f; x+=37.f)
    for(float d=-170.f; d<180.f; d+=11.f) {
      const auto hit = Grass::nearestBorder(grid,x,1600.f+d,180.f);
      require(hit.found && std::abs(hit.dist-std::abs(d))<0.01f && hit.reach==90.f && hit.margin==50.f,
              "the shader's lookup finds the way to the border: at "+std::to_string(x)+", "+std::to_string(d));
      }
  require(!Grass::nearestBorder(grid,0.f,1600.f+181.f,180.f).found && !Grass::nearestBorder(grid,0.f,1600.f-90.f,80.f).found &&
          !Grass::nearestBorder(grid,0.f,400.f,180.f).found, "no border further than asked for");
  require(Grass::nearestBorder(grid,1650.f,1650.f,180.f).found && std::abs(Grass::nearestBorder(grid,1650.f,1650.f,180.f).dist-std::hypot(50.f,50.f))<0.01f,
          "past the border's end the way is to that end");

  // the blades and the buffer see one line: a blade past the border is as far past it as the lookup says
  const auto p = meadow(true,true);
  size_t past = 0;
  for(int32_t cx=-2; cx<2; ++cx) {
    const auto cell = build(all,cx,2,p);
    for(const auto& b:cell.blades) {
      const auto i = Grass::decode(b,cx,2,cell.minY);
      if(i.z<=1600*U)
        continue;
      const auto hit = Grass::nearestBorder(grid,float(i.x)/float(U),float(i.z)/float(U),180.f);
      require(hit.found && std::abs(hit.dist-float(i.z-1600*U)/float(U))<0.07f, "a blade on the verge is by the buffer's border");
      const int32_t edge  = Grass::coverBorder(i.x,i.z,int32_t(hit.dist*float(U)),int32_t(hit.reach));
      const int32_t level = Grass::coverLevel(i.x,i.z,edge);
      // a spot of bare earth on a tongue keeps a few fine blades, as in the meadow
      require(level>Grass::CoverBlades-8 || (Grass::coverBare(i.x,i.z)>0 && Grass::coverEdge(i.x,i.z,edge)>Grass::CoverBlades-8),
              "the ground's shader has sward, its rim or a spot of bare earth under a blade on the verge");
      ++past;
      }
    }
  require(past>500, "blades stand on the verge: "+std::to_string(past));
  }

}

size_t grassCoverTests() {
  records();
  spots();
  borderLine();
  placement();
  broadBlades();
  borders();
  return assertions;
  }
