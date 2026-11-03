#include "game/archery.h"
#include "world/objects/archer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  void near(float actual, float expected, float tolerance, const char* message) {
    if(std::isfinite(actual) && std::abs(actual-expected)<=tolerance)
      return;
    std::fprintf(stderr,"FAIL: %s: %.9g versus %.9g (+/- %.9g)\n",
                 message,double(actual),double(expected),double(tolerance));
    std::exit(EXIT_FAILURE);
    }

  Archer::Input startDrawing(Archer& archer, const Archery::Stats& stats, bool burst) {
    archer.reset(1);
    Archer::Input in;
    in.raised = true;
    in.ready  = true;
    in.burst  = burst;
    in.dt     = 300;
    archer.tick(in,stats);
    archer.tick(in,stats);
    require(archer.stage()==Archer::Nocked,"arrow nocked before drawing");
    in.draw = true;
    in.dt   = 16;
    archer.tick(in,stats);
    require(archer.stage()==Archer::Drawing,"draw starts on request");
    return in;
    }

  void drawToLimit(Archer& archer, const Archery::Stats& stats, Archer::Input& in) {
    for(int i=0; i<1000 && archer.stage()==Archer::Drawing; ++i)
      archer.tick(in,stats);
    require(archer.stage()==Archer::Held,"drawing reaches the held stage");
    }
  }

int main() {
  using namespace Archery;
  near(bowDrawWeight(30),10.f,0.0001f,"light bow draw weight");
  near(bowDrawWeight(65),27.5f,0.0001f,"medium bow draw weight");
  near(bowDrawWeight(150),70.f,0.0001f,"heavy bow draw weight");
  near(bowDrawWeight(30,25),25.f,0.0001f,"item strength requirement wins");
  const std::array<float,5> ratios = {0.286f,0.571f,0.857f,1.f,2.f};
  const std::array<float,5> steady = {0.286f,0.571f,0.857f,1.f,1.f};
  const std::array<float,5> bursts = {0.4004f,0.7994f,1.f,1.f,1.f};
  for(size_t i=0; i<ratios.size(); ++i) {
    near(steadyDraw(ratios[i]),steady[i],0.0001f,"steady draw limit");
    near(burstDraw(ratios[i]),bursts[i],0.0001f,"burst draw limit");
    if(ratios[i]<1.f)
      near(drawTime(ratios[i],steady[i],baseDrawSpeed()),ratios[i]*DrawT1,0.02f*DrawT1,"steady draw time scales with strength");
    }

  const std::array<float,7> fullRatios = {0.8f,1.f,1.25f,1.5f,2.f,3.f,4.f};
  const std::array<float,7> times      = {1.57f,1.f,0.72f,0.58f,0.46f,0.36f,0.32f};
  for(size_t i=0; i<fullRatios.size(); ++i) {
    const float time = drawTime(fullRatios[i],1.f,baseDrawSpeed());
    near(time/DrawT1,times[i],0.02f,"full draw time");
    near(advanceDraw(fullRatios[i],0.f,time,baseDrawSpeed()),1.f,0.0001f,"integrated draw reaches full draw on time");
    std::printf("full draw r=%.3f: %.4f T1\n",double(fullRatios[i]),double(time/DrawT1));
    }
  near(drawTime(0.571f,burstDraw(0.571f),baseDrawSpeed())/DrawT1,1.48f,0.03f,"finite time to burst limit");
  near(drawSpeed(0.571f,burstDraw(0.571f),baseDrawSpeed()),baseDrawSpeed()*DrawEpsilon,0.0001f,"epsilon speed at burst limit");
  float frameDraw = 0;
  for(int i=0; i<25; ++i)
    frameDraw = advanceDraw(0.571f,frameDraw,0.016f,baseDrawSpeed());
  near(frameDraw,advanceDraw(0.571f,0.f,0.4f,baseDrawSpeed()),0.0001f,"draw integration is independent of tick duration");

  near(holdLimit(1.f,1.f),3.f,0.001f,"full draw holding at equal strength");
  near(holdLimit(0.8f,1.f),1.3f,0.05f,"full draw holding in a burst");
  near(holdLimit(4.f,1.f),8.f,0.0001f,"holding ceiling");
  near(rohmert(0.4f-0.00001f),rohmert(0.4f+0.00001f),0.001f,"Rohmert branches meet continuously");

  // Snapshots of main's stats, with the same strengths/dexterities and talents 10/60/100.
  struct Legacy { float strength, dex, skill, damage, draw, hold, speed, sway; };
  const std::array<Legacy,3> legacy = {{
    {10,10,0.1f,30,1.54735904f,1.66666667f,5.f,0.0210506100f},
    {60,60,0.6f,65,0.741727713f,4.61538462f,5.72916667f,0.00890117919f},
    {150,150,1.f,150,0.4f,5.f,7.5f,0.00436332313f}
    }};
  for(const auto& row:legacy) {
    const auto old = stats(row.strength,row.dex,row.skill,row.damage,0,false,false);
    near(old.drawTime,row.draw,0.000001f,"flag off preserves main draw time");
    near(old.holdLimit,row.hold,0.000001f,"flag off preserves main holding");
    near(old.speed,row.speed,0.000001f,"flag off preserves main arrow speed");
    near(old.sway,row.sway,0.000001f,"flag off preserves main sway");
    near(speedAt(old,0.5f),row.speed*0.7f,0.000001f,"flag off preserves partial-draw speed");
    }

  const auto bow = stats(40,150,1,150,0,false,true);
  const auto dex = stats(40,10,1,150,0,false,true);
  near(bow.drawTime,dex.drawTime,0.000001f,"dexterity does not change bow draw time");
  near(bow.sway,0.25f*float(M_PI)/180.f,0.000001f,"weak strength does not amplify bow sway");
  near(speedAt(bow,0.4f),bow.speed*0.4f,0.000001f,"arrow speed is proportional to draw");
  near(damageAt(0.4f),0.4f,0.000001f,"draw damage multiplier stays unchanged");
  const auto cant = canted(bow,1.f);
  near(cant.drawSpeed,bow.drawSpeed/0.6f,0.000001f,"cant speeds up the strength draw");
  near(cant.drawTime,bow.drawTime*0.6f,0.000001f,"cant changes the AI draw-time estimate");
  const auto cbow = stats(20,10,0.5f,65,0,true,true);
  const auto oldCbow = stats(20,10,0.5f,65,0,true,false);
  require(!cbow.bowStrength,"strength draw never applies to crossbows");
  near(cbow.need,39.f,0.000001f,"crossbow draw weight stays unchanged");
  near(cbow.drawTime,oldCbow.drawTime,0.000001f,"crossbow draw time stays unchanged");
  near(cbow.holdLimit,oldCbow.holdLimit,0.000001f,"crossbow holding stays unchanged");
  near(cbow.sway,oldCbow.sway,0.000001f,"crossbow sway stays unchanged");
  near(speedAt(cbow,0.4f),speedAt(oldCbow,0.4f),0.000001f,"crossbow speed stays unchanged");

  // the release as it is seen: the kick by the energy the bow stored (reports/archery/2026-11-03-release-follow-through)
  near(releaseKick(0.5f,10.f),0.15f,0.000001f,"a light bow drawn half kicks at the floor");
  near(releaseKick(1.f,27.5f),27.5f/70.f,0.000001f,"a middle bow drawn fully kicks 0.39");
  near(releaseKick(1.f,70.f),1.f,0.000001f,"a heavy bow drawn fully kicks fully");
  near(releaseKick(1.f,150.f),1.f,0.000001f,"the kick is not over 1");
  near(releaseSettle(1.f),1.f,0.000001f,"an archer as strong as the bow settles it in the base time");
  near(releaseSettle(4.f),0.7f,0.000001f,"a strong archer settles the bow faster, to the bound");

  Archer archer;
  auto input = startDrawing(archer,bow,false);
  drawToLimit(archer,bow,input);
  near(archer.draw(),bow.steadyDraw,0.000001f,"NPC holds at its steady partial draw");
  input.draw = false;
  require(archer.tick(input,bow),"held partial draw releases an arrow");
  input = startDrawing(archer,bow,true);
  drawToLimit(archer,bow,input);
  near(archer.draw(),bow.burstDraw,0.000001f,"hero holds at the burst limit");
  // the burst is seen: the arms shake across the arrow, the drawing hand creeps along it
  float shakeMax = 0, creepMax = 0;
  for(int i=0; i<60; ++i) {
    archer.tick(input,bow);
    shakeMax = std::max(shakeMax,std::hypot(archer.shake().x,archer.shake().y));
    creepMax = std::max(creepMax,archer.shake().z);
    }
  require(shakeMax>0.5f && shakeMax<4.f,"burst shakes the arms by centimetres");
  require(creepMax>0.5f && creepMax<=2.5f,"burst creeps the drawing hand along the arrow");
  const auto strong = stats(140,150,1,150,0,false,true);
  input = startDrawing(archer,strong,true);
  drawToLimit(archer,strong,input);
  for(int i=0; i<60; ++i) {
    archer.tick(input,strong);
    require(archer.shake().x==0.f && archer.shake().y==0.f && archer.shake().z==0.f,"strong enough: no shake seen");
    }
  const auto weak = stats(10,150,1,150,0,false,true);
  input = startDrawing(archer,weak,true);
  drawToLimit(archer,weak,input);
  require(archer.draw()<MinDraw,"very weak bow draw stays below the release threshold");
  input.draw = false;
  require(!archer.tick(input,weak),"held below MinDraw lets down the string without an arrow");
  near(archer.draw(),0.f,0.000001f,"letting down clears partial draw");
  std::puts("archery contract passed");
  }
