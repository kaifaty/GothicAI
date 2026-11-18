#pragma once

// A part of material_texture_test.cpp, included inside its unnamed namespace after `require`, `textFile`, `source`
// and `Fixture`.
// graphics/foliagetree.h: the processor's copy of the rule of shader/materials/foliage_tree.glsl, the tree's wind
// (reports/engine/2026-11-16-modern-trees/tree-wind.md). A spruce 12 m high stands in its mesh's axes, the root at
// the origin.
// graphics/crownlight.h: the flag of the crown's light, which materials it marks, and the processor's copy of the
// numbers of shader/lighting/crown_light.glsl (reports/engine/2026-11-16-modern-trees/crown-light.md)
void crownLightMaths() {
  using namespace CrownLight;
  using FoliageWind::Params;

  require(parse("",true)==On && parse("",false)==Off && parse("0",true)==Off && parse("1",false)==On &&
          parse("mask",false)==Mask && parse("mask",true)==Mask,
          "the crown's light is on by default; OPENGOTHIC_CROWN_LIGHT wins over [INTERNAL] crownLight, `mask` is the measure");

  Params bark, leaf, tuft;
  bark.on = true; bark.anchor = FoliageWind::AnchorTree; bark.translucency = 0.f;
  leaf.on = true; leaf.anchor = FoliageWind::AnchorTree;
  tuft.on = true;
  require(lit(bark,FoliageWind::Both,true) && lit(leaf,FoliageWind::Light,true),
          "the bark and the leaves of a tree are both of its crown, whatever they let through");
  require(!lit(tuft,FoliageWind::Both,true) && !lit(Params(),FoliageWind::Both,true),
          "a tuft and a material without the field are lit as before");
  require(!lit(leaf,FoliageWind::Both,false) && !lit(leaf,FoliageWind::Wind,true) && !lit(leaf,0,true),
          "the flag and the light half of the foliage switch the crown's light off");
  require(lit(leaf,FoliageWind::Both,true,0.f) && !lit(leaf,FoliageWind::Both,true,0.5f),
          "a glowing material is no crown: the glow and the crown's shade ride in the same alpha of the G-buffer");

  // the openness rides to the pixel in the normal's length; its top tenth reads as fully open
  for(uint32_t a=0; a<256; ++a) {
    const float len  = normalLength(uint8_t(a));
    const float open = std::min(float(a)/(255.f*OpenTop),1.f);
    require(len>=0.5f && len<=1.f && std::abs(openness(len)-open)<1e-5f,
            "the alpha of a vertex survives the ride in the normal's length");
    if(a>0)
      require(shade(openness(len))>=shade(openness(normalLength(uint8_t(a-1)))),"a more open vertex is not lit less");
    // the shade through the eight bits of the ORM alpha: it stays an own material's and comes back within a step
    const uint8_t code = shadeCode(shade(openness(len)));
    require(code>127 && std::abs(shadeOfCode(code)-shade(openness(len)))<=0.5f/(255.f*OrmCode)+1e-5f,
            "the shade of a crown survives the alpha of the G-buffer");
    }
  require(normalLength(255)==1.f && openness(1.f)==1.f && shade(1.f)==1.f && shadeCode(1.f)==255 && shadeOfCode(255)==1.f,
          "a mesh without COLOR_0 (alpha 1) keeps the normal's length, the alpha of any own material and all of its light");
  // a trunk of ten sides: the normal between two of its vertices is cos(18 deg) long
  require(openness(0.9511f)==1.f && openness(0.94f)<1.f,
          "a round mesh without the colour is lit as before although its normals come out shorter between the vertices");
  require(shade(0.f)==ShadeFloor && ShadeFloor>=0.1f && ShadeFloor<=0.3f,
          "the depth of a crown keeps 10-30 % of the light, as the needles by a spruce's trunk");
  require(openness(0.4f)==0.f && openness(1.2f)==1.f,"a short or a long normal stays in the range of the openness");

  require(skyThrough(0.f,1.f)==0.f && skyThrough(0.35f,1.f)==SkyLobe*0.35f && skyThrough(0.35f,-0.7f)==0.f &&
          skyThrough(0.35f,-0.3f)<skyThrough(0.35f,0.f) && skyThrough(1.f,1.f)<0.5f,
          "the sky comes through a leaf by its translucency, less than half of what lights its other side, and fades "
          "where the normal already faces that sky");
  }

void foliageTreeMaths() {
  crownLightMaths();
  using namespace FoliageTree;
  using FoliageWind::Both;
  using FoliageWind::Params;
  using FoliageWind::Scene;
  using FoliageWind::pack;
  using Tempest::Vec3;

  const float H = 1200.f;
  const Vec3  boxMin(-260,0,-250), boxMax(255,H,262);
  Params bark, leaf, tuft;
  bark.on = true; bark.anchor = FoliageWind::AnchorTree; bark.translucency = 0.f;
  leaf.on = true; leaf.anchor = FoliageWind::AnchorTree; leaf.flutter = 0.5f;
  tuft.on = true;
  const uint32_t barkBits = pack(bark,Both), leafBits = pack(leaf,Both), tuftBits = pack(tuft,Both);
  require(isTree(bark) && isTree(barkBits) && isTree(leafBits) && !isTree(tuft) && !isTree(tuftBits) && !isTree(Params()) &&
          FoliageWind::moves(barkBits),"the anchor of a tree is packed into the bucket's bits");
  require(reach(bark,boxMin,boxMax)==H && reach(leaf,boxMin,boxMax)==H &&
          reach(tuft,boxMin,boxMax)==FoliageWind::reach(boxMin,boxMax),
          "a tree's reach is the height of its whole mesh, the same for its bark and its leaves; a tuft's is its arm");

  const Vec3 upright[3] = {Vec3(1,0,0), Vec3(0,1,0), Vec3(0,0,1)};
  const Vec3 turned[3]  = {Vec3(0.6f,0,-0.8f), Vec3(0,1,0), Vec3(0.8f,0,0.6f)};
  const Vec3 place(34731,4015,1367);
  // the colours of a vertex: R the branch's weight, G its phase, B the leaf's weight, A the crown's openness
  const auto color = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g<<8) | (b<<16) | (a<<24); };
  // the wind along +Z at the peak of a gust and in the breeze, at a moment of the swing's period
  const auto wind = [](float pressure, float at) {
    Scene sc;
    sc.wind  = Tempest::Vec4(0,1,pressure,1);
    sc.phase = Tempest::Vec4(FoliageWind::fract(at*10.f),FoliageWind::fract(at*18.75f),FoliageWind::fract(at*77.9f),FoliageWind::fract(at*113.2f));
    sc.swing = Tempest::Vec4(at,0.f,std::pow(std::sqrt(pressure),FoliageWind::EddyCurve),0.f);
    return sc;
    };

  // the rates: the trunk's own by the formula of the reference, whole swings a period
  for(int i=0; i<1000; ++i) {
    const float h = float(i)/1000.f;
    const FoliageWind::SwingRates t12 = swayRates(h,1200.f), t30 = swayRates(h,3000.f);
    require(t12.first>=30.f && t12.first<=37.f && t30.first>=15.f && t30.first<=19.f,
            "a tree of 12 m sways 0.50..0.62 times a second, one of 30 m 0.25..0.32");
    for(const FoliageWind::SwingRates& r:{t12,t30})
      require(r.first==std::floor(r.first) && r.second==std::floor(r.second) && r.second-r.first>=TreeBeatMin &&
              r.second-r.first<TreeBeatMin+TreeBeatVar,"the second rate is 2..4 whole swings a period over the first");
    }
  require(swayRates(0.3f,100.f).first==swayRates(0.3f,TreeHeightMin).first &&
          swayRates(0.3f,9000.f).first==swayRates(0.3f,TreeHeightMax).first,"the height is taken between 3 and 60 m");

  // what the end of a branch 1.8 m long gives way at full pressure, without the gusts
  const float give18 = 180.f*BranchGive*std::sqrt(180.f/BranchLong);
  float topMin = 1e9f, topMax = -1e9f, sideMax = 0.f, sweepMin = 1e9f, sweepMax = -1e9f, tremble = 0.f, still = 1e9f, inner = 0.f, leader = 0.f;
  for(int i=0; i<240; ++i) {
    const float at   = float(i)/240.f;
    const Scene gust = wind(1.f,at);

    // the root stands
    for(const Vec3& root:{Vec3(0,0,0), Vec3(14,0,-9), Vec3(-30,-25,12), Vec3(200,0,0)})
      require(move(root,color(0,0,0,255),barkBits,H,upright,place,gust)==root,"the root and all under it stand");

    // the trunk keeps its length: every point of its axis stays as far from the root, and the axis is no longer
    Vec3  before(0,0,0);
    float length = 0.f;
    for(int y=50; y<=1200; y+=50) {
      const Vec3 p(0,float(y),0);
      const Vec3 r = move(p,color(0,0,0,255),barkBits,H,upright,place,gust);
      require(std::abs(r.length()-float(y))<1e-3f*float(y),"a point of the trunk keeps its distance from the root");
      length += (r-before).length();
      before  = r;
      }
    require(std::abs(length-H)<0.002f*H,"the trunk bends and does not stretch");

    const Vec3 top = move(Vec3(0,H,0),color(0,0,0,255),barkBits,H,upright,place,gust);
    const Vec3 mid = move(Vec3(0,H*0.5f,0),color(0,0,0,255),barkBits,H,upright,place,gust);
    topMin  = std::min(topMin,top.z);
    topMax  = std::max(topMax,top.z);
    sideMax = std::max(sideMax,std::abs(top.x));
    require(std::abs(mid.z-0.25f*top.z)<0.02f*top.z && top.y<H && top.y>H-2.f,
            "the lean grows as the square of the height: half way up, a quarter of the top's");

    // the bark and the leaves of one point move as one: the height is the mesh's and not the material's
    const Vec3 fork(35,640,-22);
    for(const uint32_t c:{color(0,0,0,255), color(140,77,0,90), color(255,255,0,0)})
      require(move(fork,c,barkBits,H,upright,place,gust)==move(fork,c,leafBits,H,upright,place,gust),
              "bark and leaf of the same point and wind move the same way");

    // without a branch's and a leaf's weight a vertex goes with the trunk only: no phase or openness moves it
    const Vec3 onTrunk = move(fork,color(0,0,0,255),leafBits,H,upright,place,gust);
    require(onTrunk==move(fork,color(0,213,0,7),leafBits,H,upright,place,gust) && std::abs(onTrunk.length()-fork.length())<1e-2f &&
            std::abs((onTrunk.z-fork.z) - top.z*(640.f/H)*(640.f/H))<0.5f && onTrunk!=move(fork,color(255,0,0,255),leafBits,H,upright,place,gust),
            "with R = 0 and B = 0 the vertex goes with the trunk alone");
    // the needles of the leader, which is no branch, tremble too
    leader = std::max(leader,(move(fork,color(0,0,255,255),leafBits,H,upright,place,gust)-onTrunk).length());

    // the branch across the wind: the end gives way down the wind, the base stands on the trunk
    const Vec3 twig(180,600,0);
    const Vec3 held = move(twig,color(0,90,0,255),barkBits,H,upright,place,gust);
    const Vec3 free = move(twig,color(255,90,0,255),barkBits,H,upright,place,gust);
    sweepMin = std::min(sweepMin,free.z-held.z);
    sweepMax = std::max(sweepMax,free.z-held.z);
    require(std::abs(free.y-held.y)<1e-3f && std::abs(free.x-held.x)<1e-3f,
            "a branch across the wind goes down the wind: not up, and not along itself");
    require((free-held).length()<=H*BranchReach*BranchGive*BranchTopK*(1.f+BranchGust)+1e-3f,"a branch's end goes no farther than its bound");
    // a branch that points down the wind is not pushed along itself: its end rises; one that points against it dips
    const Vec3 lee(0,600,180), fore(0,600,-180);
    const Vec3 leeMove  = move(lee,color(255,90,0,255),barkBits,H,upright,place,gust)  - move(lee,color(0,90,0,255),barkBits,H,upright,place,gust);
    const Vec3 foreMove = move(fore,color(255,90,0,255),barkBits,H,upright,place,gust) - move(fore,color(0,90,0,255),barkBits,H,upright,place,gust);
    require(leeMove.y>0.2f*give18 && std::abs(leeMove.z)<0.05f*leeMove.y && std::abs(leeMove.x)<1e-3f,
            "a branch keeps its length: the wind does not push it along itself, the lee branch's end rises");
    require(foreMove.y<-0.2f*give18 && std::abs(foreMove.z)<0.05f*-foreMove.y && std::abs(foreMove.x)<1e-3f,
            "the windward branch's end dips");
    // up and down is the same push as along the wind, not a motion of its own: their ratio does not change
    const Vec3  slant(127,600,127);
    const Vec3  slantMove = move(slant,color(255,90,0,255),barkBits,H,upright,place,gust) - move(slant,color(0,90,0,255),barkBits,H,upright,place,gust);
    const float firm      = 2.f*127.f*127.f/(2.f*127.f*127.f + H*BranchAxis*H*BranchAxis);
    require(slantMove.z>0.f && std::abs(slantMove.y - slantMove.z*BranchLift*firm*0.70710678f/(1.f-0.5f*firm))<2e-3f,
            "a branch goes up by the same push that takes it down the wind");

    // the needles: the free edge trembles by a centimetre in a gust's crest, the stalk stays on the branch
    const Vec3 card(150,700,40);
    const Vec3 stalk = move(card,color(200,90,0,255),leafBits,H,upright,place,gust);
    const Vec3 edge  = move(card,color(200,90,255,255),leafBits,H,upright,place,gust);
    tremble = std::max(tremble,(edge-stalk).length());
    still   = std::min(still,(edge-stalk).length());
    inner   = std::max(inner,(move(card,color(40,90,255,255),leafBits,H,upright,place,gust)-move(card,color(40,90,0,255),leafBits,H,upright,place,gust)).length());
    require((edge-stalk).length()<=0.5f*LeafMax*(LeafUp+LeafSide)+0.02f,"a needle trembles within its bound");
    }
  require(topMin>0.02f*H && topMax<0.04f*H && topMax-topMin>0.005f*H && sideMax>0.001f*H && sideMax<0.004f*H,
          "in a gust the top of a tree of 12 m leans 3 % of its height down the wind and sways about that");
  require(sweepMin>0.45f*give18 && sweepMax<1.55f*give18 && sweepMax-sweepMin>0.4f*give18,
          "in a gust a branch's end is held down the wind and swings about that with the gusts: it does not wave back");
  require(tremble>0.5f && tremble<1.05f && still<0.01f,"a needle's edge trembles by about a centimetre in a crest and stands between them");
  require(inner>0.1f && inner<=LeafInner*tremble+0.02f && leader>0.1f && leader<=LeafInner*tremble+0.02f,
          "the needles by the trunk and on the leader tremble less than those on a branch's end");

  // the breeze: the lean is the pressure's share of the gust's; the branches and the needles go by the pressure too
  {
  float lean = 0.f, sweep = 0.f, shake = 0.f;
  for(int i=0; i<240; ++i) {
    const Scene calm = wind(0.15f,float(i)/240.f);
    lean  = std::max(lean,move(Vec3(0,H,0),color(0,0,0,255),barkBits,H,upright,place,calm).z);
    sweep = std::max(sweep,(move(Vec3(180,600,0),color(255,90,0,255),barkBits,H,upright,place,calm) -
                            move(Vec3(180,600,0),color(0,90,0,255),barkBits,H,upright,place,calm)).length());
    shake = std::max(shake,(move(Vec3(150,700,40),color(200,90,255,255),leafBits,H,upright,place,calm) -
                            move(Vec3(150,700,40),color(200,90,0,255),leafBits,H,upright,place,calm)).length());
    }
  require(lean>0.003f*H && lean<0.008f*H,"between gusts the top leans a fraction of a percent of the height");
  require(sweep>0.f && sweep<=0.16f*sweepMax,"between gusts the branches nearly stand");
  require(shake<=0.16f*tremble+0.01f,"between gusts the needles nearly stand");
  }

  // the length: a short branch goes less than a long one, and quicker
  {
  const Vec3 longEnd(450,300,0), shortEnd(60,1100,0);
  float      farLong = 0.f, farShort = 0.f, wasLong = 0.f, wasShort = 0.f, upLong = 0.f, upShort = 0.f;
  int        turnsLong = 0, turnsShort = 0;
  for(int i=0; i<6000; ++i) {
    const Scene gust = wind(1.f,float(i)/6000.f);
    const float a = (move(longEnd,color(255,90,0,255),barkBits,H,upright,place,gust)  - move(longEnd,color(0,90,0,255),barkBits,H,upright,place,gust)).z;
    const float b = (move(shortEnd,color(255,90,0,255),barkBits,H,upright,place,gust) - move(shortEnd,color(0,90,0,255),barkBits,H,upright,place,gust)).z;
    farLong  = std::max(farLong,a);
    farShort = std::max(farShort,b);
    if(i>0) {
      if((a-wasLong)*upLong<0.f)
        ++turnsLong;
      if((b-wasShort)*upShort<0.f)
        ++turnsShort;
      if(a!=wasLong)
        upLong = a-wasLong;
      if(b!=wasShort)
        upShort = b-wasShort;
      }
    wasLong  = a;
    wasShort = b;
    }
  require(farLong>30.f && farLong<60.f && farShort>1.f && farShort<0.1f*farLong,
          "the end of a branch 4.5 m long goes tens of centimetres, that of one 0.6 m long a few: by the length, not by the tree's height");
  require(turnsLong>=30 && turnsLong<=80 && turnsShort>=2*turnsLong,
          "a long branch swings a few times in ten seconds, a short one at least twice as often");
  }

  // the world's gust through the crown: the front comes to the windward branch before the lee one, at 5 m/s; in the
  // lull between two gusts a branch's travel is a small share of the gust's
  {
  const uint64_t period = 6000, cycle = uint64_t(double(period)/FoliageFront::RateGust);
  const Vec3     foreEnd(300,600,-300), leeEnd(300,600,300);
  const float    lullShare = 0.35f;
  for(uint64_t from=400000; from<400000+3*cycle; from+=cycle) {
    const uint64_t peak = from + FoliageFront::untilGust(from,period,place.z);
    std::vector<float> fores, lees;
    float  gustFar = 0.f, lullFar = 0.f, gustShake = 0.f, lullShake = 0.f;
    for(int64_t dt=-int64_t(cycle)/2; dt<int64_t(cycle)/2; dt+=25) {
      const uint64_t tick  = uint64_t(int64_t(peak)+dt);
      const auto     front = FoliageFront::make(tick,period,true,1.f,false);
      Scene          sc    = FoliageWind::scene(tick,period,true,1.f,false);
      const auto     here  = FoliageFront::atPlace(front,sc,place.z);
      sc.wind.z  = here.pressure;
      sc.swing.y = here.swing;
      sc.swing.z = here.flutter;
      const float fore = (move(foreEnd,color(255,90,0,255),barkBits,H,upright,place,sc,front) - move(foreEnd,color(0,90,0,255),barkBits,H,upright,place,sc,front)).length();
      const float lee  = (move(leeEnd,color(255,90,0,255),barkBits,H,upright,place,sc,front)  - move(leeEnd,color(0,90,0,255),barkBits,H,upright,place,sc,front)).length();
      const float shake = (move(foreEnd,color(255,90,255,255),leafBits,H,upright,place,sc,front) - move(foreEnd,color(255,90,0,255),leafBits,H,upright,place,sc,front)).length();
      fores.push_back(fore);
      lees.push_back(lee);
      // the gust is over the tree within 4.5 s of its peak and lingers a second more; the rest of the cycle is the lull
      const bool lull = dt<-5000 || dt>6500;
      (lull ? lullFar   : gustFar)   = std::max(lull ? lullFar   : gustFar,   std::max(fore,lee));
      (lull ? lullShake : gustShake) = std::max(lull ? lullShake : gustShake, shake);
      // with the front the previous frame's vertex is that of the previous frame's wind
      if(dt==0) {
        auto before = FoliageFront::make(tick-33,period,true,1.f,false);
        auto both   = front;
        both.phase.z = before.phase.x;
        both.phase.w = before.phase.y;
        require(move(leeEnd,color(255,90,0,255),barkBits,H,upright,place,sc,both,true)==move(leeEnd,color(255,90,0,255),barkBits,H,upright,place,sc,before) &&
                move(leeEnd,color(255,90,0,255),barkBits,H,upright,place,sc,both,true)!=move(leeEnd,color(255,90,0,255),barkBits,H,upright,place,sc,both),
                "the previous frame's branch stands where the previous frame's front had it");
        }
      }
    // when a branch's gust is: the middle of its travel over what the lull has of it
    const auto middle = [&](const std::vector<float>& far) {
      double sum = 0, at = 0;
      for(size_t i=0; i<far.size(); ++i) {
        const double over = std::max(0.0, double(far[i]) - double(lullShare*gustFar));
        sum += over;
        at  += over*double(i)*25.0;
        }
      return at/sum;
      };
    const double lag = middle(lees) - middle(fores);
    require(lag>800.0 && lag<1600.0,
            ("the gust comes to a branch 6 m down the wind 1.2 s later: "+std::to_string(lag)).c_str());
    require(gustFar>10.f && lullFar<=lullShare*gustFar,
            ("in the lull a branch's travel is no more than a third of the gust's: "+std::to_string(lullFar)+" of "+std::to_string(gustFar)).c_str());
    require(gustShake>0.8f && lullShake<=lullShare*gustShake,("in the lull the needles nearly stand: "+std::to_string(lullShake)+" of "+std::to_string(gustShake)).c_str());
    }
  }

  // the bounds: no vertex of any colour leaves the travel in any wind, whichever way the tree is turned
  Params strong = leaf;
  strong.wind = 4.f; strong.flutter = 4.f;
  uint32_t seed = 12345u;
  const auto rnd = [&seed]() { seed = seed*1664525u + 1013904223u; return float(seed>>8)/16777216.f; };
  for(const Params& m:{bark,leaf,strong}) {
    const uint32_t bits = pack(m,Both);
    const float    grow = travel(bits,reach(m,boxMin,boxMax));
    require(grow==FoliageTree::travel(m,boxMin,boxMax) || FoliageWind::enabled()!=Both,"the travel of a material's mesh is that of its bucket");
    for(int i=0; i<4000; ++i) {
      const float a = rnd()*6.2831853f;
      Scene sc;
      sc.wind  = Tempest::Vec4(std::sin(a),std::cos(a),rnd(),1.f);
      sc.phase = Tempest::Vec4(rnd(),rnd(),rnd(),rnd());
      sc.swing = Tempest::Vec4(rnd(),0.065f*rnd(),2.f*rnd(),0.f);
      // with the front and without it, this frame's and the previous one's
      auto front = FoliageFront::make(123456,6000,true,1.f,false);
      front.phase = Tempest::Vec4(rnd(),rnd(),rnd(),rnd());
      front.k.w   = (i%3)==0 ? 0.f : 1.f;
      const Vec3 p(boxMin.x+(boxMax.x-boxMin.x)*rnd(), H*rnd(), boxMin.z+(boxMax.z-boxMin.z)*rnd());
      const uint32_t c = color(uint32_t(rnd()*255.99f),uint32_t(rnd()*255.99f),uint32_t(rnd()*255.99f),uint32_t(rnd()*255.99f));
      const Vec3 r = move(p,c,bits,H,(i%2)==0 ? upright : turned,place+Vec3(float(i)*37.f,0,float(i)*11.f),sc,front,(i%5)==0);
      require((r-p).length()<=grow,"the bounds' travel covers the vertex in any wind");
      }
    }
  require(travel(barkBits,H)>0.12f*H && travel(barkBits,H)<0.14f*H,"a tree's bounds grow by an eighth of its height");

  // a tree of 60 m with boughs longer than BranchTopK allows and wider than BranchReach of its height: what the
  // branch adds to the trunk's move stays within the branch's share of the travel, and comes near it
  {
  const float tall = 6000.f, wide = 0.6f*tall;
  for(const Params& m:{bark,strong}) {
    const uint32_t bits = pack(m,Both);
    const float    most = tall*FoliageWind::packedWind(bits)*BranchReach*BranchGive*BranchTopK*(1.f + BranchGust);
    float          far  = 0.f;
    for(int i=0; i<4000; ++i) {
      const float a = rnd()*6.2831853f;
      Scene sc;
      sc.wind  = Tempest::Vec4(std::sin(a),std::cos(a),rnd(),1.f);
      sc.phase = Tempest::Vec4(rnd(),rnd(),rnd(),rnd());
      sc.swing = Tempest::Vec4(rnd(),0.065f*rnd(),2.f*rnd(),0.f);
      auto front = FoliageFront::make(123456,6000,true,1.f,false);
      front.phase = Tempest::Vec4(rnd(),rnd(),rnd(),rnd());
      front.k.w   = (i%3)==0 ? 0.f : 1.f;
      const Vec3     p(wide*(2.f*rnd()-1.f), tall*rnd(), wide*(2.f*rnd()-1.f));
      const uint32_t R = (i%2)==0 ? 255u : uint32_t(rnd()*255.99f), G = uint32_t(rnd()*255.99f);
      const Vec3*    axes = (i%2)==0 ? upright : turned;
      const Vec3     at   = place+Vec3(float(i)*37.f,0,float(i)*11.f);
      const Vec3     onTrunk = move(p,color(0,G,0,255),bits,tall,axes,at,sc,front);
      const Vec3     r       = move(p,color(R,G,0,255),bits,tall,axes,at,sc,front);
      far = std::max(far,(r-onTrunk).length());
      require((r-onTrunk).length()<=most*1.001f && (r-p).length()<=travel(bits,tall),
              "a long branch of a wide crown stays within the branch's share of the bounds' travel");
      }
    require(far>0.8f*most,"the branch's share of the bounds' travel is what a long branch does reach");
    }
  }

  // equal ticks give equal answers; no wind and the wind half off leave the tree at rest
  const Scene same = FoliageWind::scene(4242,10000,true,1.f,false);
  const Vec3  tip(120,1010,-35);
  require(move(tip,color(180,31,255,255),leafBits,H,turned,place,same)==move(tip,color(180,31,255,255),leafBits,H,turned,place,same),
          "equal ticks: this frame's vertex is the previous frame's, no motion");
  Scene stopped = same;
  stopped.wind = Tempest::Vec4();
  require(move(tip,color(180,31,255,255),leafBits,H,turned,place,stopped)==tip,"no wind: the tree rests where the mesh has it");
  require(move(tip,color(180,31,255,255),pack(leaf,FoliageWind::Light),H,turned,place,same)==tip,"the wind half off: the tree rests");
  // the trees of a row are out of step
  require(FoliageWind::placeHash(place)!=FoliageWind::placeHash(place+Vec3(700,0,0)),"a tree's step is its place on the ground");

  // a turned tree leans down the world's wind all the same
  {
  const Scene gust = wind(1.f,0.31f);
  const Vec3  a = move(Vec3(0,H,0),0,barkBits,H,upright,place,gust), b = move(Vec3(0,H,0),0,barkBits,H,turned,place,gust);
  const Vec3  world = turned[0]*b.x + turned[1]*b.y + turned[2]*b.z;
  require((world-a).length()<1e-2f,"the lean follows the world's wind whichever way the mesh is turned");
  }

  // a tuft answers as before: the anchor chooses the rule
  {
  const Scene gust = wind(1.f,0.31f);
  const Vec3  p(10,-85,4);
  require(move(p,color(255,255,255,255),tuftBits,85.f,upright,place,gust)==FoliageWind::bend(p,tuftBits,85.f,upright,place,gust) &&
          travel(tuftBits,85.f)==FoliageWind::travel(tuftBits,85.f) && FoliageTree::travel(tuft,boxMin,boxMax)==FoliageWind::travel(tuft,boxMin,boxMax),
          "the line anchor moves and grows its bounds as FoliageWind does");
  }
  }

// materials.json "foliage": {"anchor": "tree"}
// "two_sided" of a record (reports/engine/2026-11-16-modern-trees/two-sided.md): the field's values, a bucket of
// its own, a pipeline without culling for an object's mesh in every pass and the old ones for everything else.
void twoSidedRecords(Tempest::Device& device, const Fixture& fixture) {
  const auto record = [](std::string_view name, std::string_view sides) {
    return "\"" + std::string(name) + R"(":{"albedo":"solid.dds","normal":"alpha.dds","orm":"alpha.dds")" +
           std::string(sides) + "}";
    };
  textFile(fixture.root/"materials/materials.json", "{" +
    record("SIDES_NONE.TGA", "") + "," +
    record("SIDES_OFF.TGA",  R"(,"two_sided":"off")") + "," +
    record("SIDES_SAME.TGA", R"(,"two_sided":"same")") + "," +
    record("SIDES_FLIP.TGA", R"(,"two_sided":"flip","foliage":{"anchor":"tree","translucency":0.25})") + "," +
    record("SIDES_BOOL.TGA", R"(,"two_sided":true)") + "," +
    record("SIDES_NUMBER.TGA", R"(,"two_sided":1)") + "," +
    record("SIDES_CASE.TGA", R"(,"two_sided":"Flip")") + "," +
    record("SIDES_BOTH.TGA", R"(,"two_sided":"both")") + "," +
    record("SIDES_EMPTY.TGA", R"(,"two_sided":"")") +
    "}");
  Resources resources(device);
  Shaders shaders;
  shaders.waitCompiler();
  using Sides = Material::TwoSided;
  require(Resources::loadMaterialMaps("SIDES_NONE.TGA").twoSided==Sides::Off &&
          Resources::loadMaterialMaps("SIDES_OFF.TGA").twoSided==Sides::Off &&
          Resources::loadMaterialMaps("SIDES_OFF.TGA").present &&
          Resources::loadMaterialMaps("SIDES_SAME.TGA").twoSided==Sides::Same &&
          Resources::loadMaterialMaps("SIDES_FLIP.TGA").twoSided==Sides::Flip &&
          FoliageTree::isTree(Resources::loadMaterialMaps("SIDES_FLIP.TGA").foliage),
          "\"two_sided\" is \"off\", \"same\" or \"flip\"; a record without it has one side; foliage stays beside it");
  for(const auto* name:{"SIDES_BOOL.TGA","SIDES_NUMBER.TGA","SIDES_CASE.TGA","SIDES_BOTH.TGA","SIDES_EMPTY.TGA"})
    require(!Resources::loadMaterialMaps(name).present,
            (std::string("\"two_sided\" that is not one of its three words must reject the record whole: ")+name).c_str());

  const Material none(source("SIDES_NONE.TGA"),true), off(source("SIDES_OFF.TGA"),true);
  const Material same(source("SIDES_SAME.TGA"),true), flip(source("SIDES_FLIP.TGA"),true);
  require(none.twoSided==Sides::Off && off.twoSided==Sides::Off && same.twoSided==Sides::Same && flip.twoSided==Sides::Flip &&
          flip.foliage.translucency==0.25f,"a material takes the sides of its record");
  Material sameOfNone = none, flipOfNone = none, sameAgain = none;
  sameOfNone.twoSided = Sides::Same;
  flipOfNone.twoSided = Sides::Flip;
  sameAgain.twoSided  = Sides::Same;
  require(none==Material(none) && !(none==sameOfNone) && !(none==flipOfNone) && !(sameOfNone==flipOfNone) && sameOfNone==sameAgain,
          "the sides are a part of the material: one, two alike and two turned do not share a bucket");

  for(const bool bindless:{false,true}) {
    if(bindless && !Shaders::options().doBindless)
      continue;
    for(const auto pass:{Shaders::T_Main,Shaders::T_Depth,Shaders::T_Shadow,Shaders::T_Vsm}) {
      const auto* one      = shaders.materialPipeline(none,DrawCommands::Static,pass,bindless);
      const auto* two      = shaders.materialPipeline(sameOfNone,DrawCommands::Static,pass,bindless);
      const auto* turned   = shaders.materialPipeline(flipOfNone,DrawCommands::Movable,pass,bindless);
      require(one!=nullptr && two!=nullptr && two!=one && turned==two &&
              shaders.materialPipeline(off,DrawCommands::Movable,pass,bindless)==one &&
              shaders.materialPipeline(none,DrawCommands::Static,pass,bindless)==one,
              "an object's two-sided material takes one more pipeline per pass, the same for both of its kinds; "
              "a one-sided material keeps its own");
      for(const auto type:{DrawCommands::Landscape,DrawCommands::Animated,DrawCommands::Morph})
        require(shaders.materialPipeline(sameOfNone,type,pass,bindless)==shaders.materialPipeline(none,type,pass,bindless),
                "the world mesh, a skin and a morph are drawn by the pipeline of a one-sided material");
      }
    }
  }

void foliageTreeRecords(Tempest::Device& device, const Fixture& fixture) {
  twoSidedRecords(device,fixture);
  const auto record = [](std::string_view name, std::string_view foliage) {
    return "\"" + std::string(name) + R"(":{"albedo":"solid.dds","normal":"alpha.dds","orm":"alpha.dds","foliage":)" +
           std::string(foliage) + "}";
    };
  textFile(fixture.root/"materials/materials.json", "{" +
    record("TREE_LEAF.TGA", R"({"anchor":"tree","wind":1.5,"flutter":0.5,"translucency":0.25})") + "," +
    record("TREE_BARK.TGA", R"({"anchor":"tree","wind":1.5,"translucency":0})") + "," +
    record("TREE_TUFT.TGA", R"({"wind":1.5,"flutter":0.5,"translucency":0.25})") + "," +
    record("TREE_CASE.TGA", R"({"anchor":"Tree"})") + "," +
    record("TREE_LIST.TGA", R"({"anchor":["tree"]})") + "," +
    record("TREE_EMPTY.TGA", R"({"anchor":""})") + "," +
    record("TREE_WIND.TGA", R"({"anchor":"tree","wind":4.5})") +
    "}");
  Resources resources(device);
  const auto leaf = Resources::loadMaterialMaps("TREE_LEAF.TGA").foliage;
  const auto bark = Resources::loadMaterialMaps("TREE_BARK.TGA").foliage;
  const auto tuft = Resources::loadMaterialMaps("TREE_TUFT.TGA").foliage;
  require(leaf.on && leaf.anchor==FoliageWind::AnchorTree && leaf.wind==1.5f && leaf.flutter==0.5f && leaf.translucency==0.25f &&
          FoliageTree::isTree(leaf) && FoliageTree::isTree(FoliageWind::pack(leaf,FoliageWind::Both)),
          "a record with the anchor of a tree must carry it and its numbers");
  require(bark.on && FoliageTree::isTree(bark) && bark.translucency==0.f && FoliageWind::moves(FoliageWind::pack(bark,FoliageWind::Both)),
          "bark without translucency is still a tree's material: its bucket moves");
  require(tuft.on && !FoliageTree::isTree(tuft) && !(tuft==leaf),"the anchor is a part of the material: a tree and a tuft do not share a bucket");
  const Material treeLeaf(source("TREE_LEAF.TGA"),true), treeBark(source("TREE_BARK.TGA"),true);
  const Tempest::Vec3 boxMin(-260,0,-250), boxMax(255,1200,262);
  require(treeLeaf.foliage==leaf && treeLeaf.hasMaterialMaps() && treeBark.hasMaterialMaps() &&
          FoliageTree::reach(treeLeaf.foliage,boxMin,boxMax)==FoliageTree::reach(treeBark.foliage,boxMin,boxMax),
          "the materials of one tree keep the anchor and take one height");
  for(const auto* name:{"TREE_CASE.TGA","TREE_LIST.TGA","TREE_EMPTY.TGA","TREE_WIND.TGA"})
    require(!Resources::loadMaterialMaps(name).present,
            (std::string("an anchor that is not \"line\" or \"tree\", or a tree with a number outside its range, must "
                        "reject the record whole: ")+name).c_str());
  }
