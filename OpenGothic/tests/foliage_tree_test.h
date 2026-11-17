#pragma once

// A part of material_texture_test.cpp, included inside its unnamed namespace after `require`, `textFile`, `source`
// and `Fixture`.
// graphics/foliagetree.h: the processor's copy of the rule of shader/materials/foliage_tree.glsl, the tree's wind
// (reports/engine/2026-11-16-modern-trees/tree-wind.md). A spruce 12 m high stands in its mesh's axes, the root at
// the origin.
void foliageTreeMaths() {
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

  float topMin = 1e9f, topMax = -1e9f, sideMax = 0.f, nodMin = 1e9f, nodMax = -1e9f, tremble = 0.f;
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
            std::abs((onTrunk.z-fork.z) - top.z*(640.f/H)*(640.f/H))<0.5f && onTrunk!=move(fork,color(255,0,0,255),leafBits,H,upright,place,gust) &&
            onTrunk!=move(fork,color(0,0,255,255),leafBits,H,upright,place,gust),
            "with R = 0 and B = 0 the vertex goes with the trunk alone");

    // the branch: the end nods and gives way, the base stands on the trunk
    const Vec3 twig(180,600,0);
    const Vec3 held = move(twig,color(0,90,0,255),barkBits,H,upright,place,gust);
    const Vec3 free = move(twig,color(255,90,0,255),barkBits,H,upright,place,gust);
    nodMin = std::min(nodMin,free.y-held.y);
    nodMax = std::max(nodMax,free.y-held.y);
    require((free-held).length()<=H*(BranchLean+BranchSway+BranchNod)+1e-3f,"a branch's end goes no farther than its share of the height");
    // a branch that points down the wind is not pushed along itself
    const Vec3 lee(0,600,180);
    const Vec3 leeMove = move(lee,color(255,90,0,255),barkBits,H,upright,place,gust) - move(lee,color(0,90,0,255),barkBits,H,upright,place,gust);
    require(std::abs(leeMove.z)<0.05f*H*BranchLean && std::abs(leeMove.x)<1e-3f,"a branch keeps its length: the wind does not push it along itself");

    // the leaf: its free edge trembles by a centimetre or two, the stalk stays on the branch
    const Vec3 card(150,700,40);
    const Vec3 stalk = move(card,color(200,90,0,255),leafBits,H,upright,place,gust);
    const Vec3 edge  = move(card,color(200,90,255,255),leafBits,H,upright,place,gust);
    tremble = std::max(tremble,(edge-stalk).length());
    require((edge-stalk).length()<=0.5f*(LeafCalm+LeafWind*LeafMaxQ)*(LeafUp+LeafSide)+0.02f,"a leaf trembles within its bound");
    }
  require(topMin>0.02f*H && topMax<0.04f*H && topMax-topMin>0.005f*H && sideMax>0.001f*H && sideMax<0.004f*H,
          "in a gust the top of a tree of 12 m leans 3 % of its height down the wind and sways about that");
  require(nodMin<-0.004f*H && nodMax>0.004f*H,"a branch's end nods up and down");
  require(tremble>0.5f && tremble<1.1f,"a leaf's edge trembles by about a centimetre");

  // the breeze: the lean is the pressure's share of the gust's, the branches nearly stand
  {
  float lean = 0.f, nod = 0.f;
  for(int i=0; i<240; ++i) {
    const Scene calm = wind(0.15f,float(i)/240.f);
    lean = std::max(lean,move(Vec3(0,H,0),color(0,0,0,255),barkBits,H,upright,place,calm).z);
    nod  = std::max(nod,std::abs(move(Vec3(180,600,0),color(255,90,0,255),barkBits,H,upright,place,calm).y -
                                 move(Vec3(180,600,0),color(0,90,0,255),barkBits,H,upright,place,calm).y));
    }
  require(lean>0.003f*H && lean<0.008f*H,"between gusts the top leans a fraction of a percent of the height");
  require(nod<0.003f*H,"between gusts the branches nearly stand");
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
      const Vec3 p(boxMin.x+(boxMax.x-boxMin.x)*rnd(), H*rnd(), boxMin.z+(boxMax.z-boxMin.z)*rnd());
      const uint32_t c = color(uint32_t(rnd()*255.99f),uint32_t(rnd()*255.99f),uint32_t(rnd()*255.99f),uint32_t(rnd()*255.99f));
      const Vec3 r = move(p,c,bits,H,(i%2)==0 ? upright : turned,place+Vec3(float(i)*37.f,0,float(i)*11.f),sc);
      require((r-p).length()<=grow,"the bounds' travel covers the vertex in any wind");
      }
    }
  require(travel(barkBits,H)>0.09f*H && travel(barkBits,H)<0.12f*H,"a tree's bounds grow by a tenth of its height");

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
          Material(source("SIDES_SAME.TGA"),false).twoSided==Sides::Off,
          "a material takes the sides of its record, and none with the material maps off");
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
