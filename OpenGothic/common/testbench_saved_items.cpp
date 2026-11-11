#include "testbench_internal.h"
#include "testbench.h"

#include <Tempest/MemReader>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "game/gamescript.h"
#include "game/serialize.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/world.h"

using namespace Tempest;
using namespace testbench_detail;

namespace {

float savedItemRotationDelta(const Matrix4x4& a,const Matrix4x4& b) {
  double trace=0;
  for(int column=0;column<3;++column) {
    double dot=0,lengthA=0,lengthB=0;
    for(int row=0;row<3;++row) {
      const double x=a.at(column,row),y=b.at(column,row);
      dot+=x*y;lengthA+=x*x;lengthB+=y*y;
    }
    trace+=dot/std::sqrt(lengthA*lengthB);
  }
  return float(std::acos(std::clamp((trace-1.)*.5,-1.,1.))*180./M_PI);
}

}

namespace testbench_detail {

void savedItemSample(World& world) {
  if(!bench.savedItemTracing || bench.savedItemSample==world.tickCount())return;
  bench.savedItemSample=world.tickCount();
  for(auto& probe:bench.savedItemProbes) {
    const auto expected=Vec3(probe.saved.at(3,0),probe.saved.at(3,1),probe.saved.at(3,2));
    probe.maximumPositionDelta=std::max(probe.maximumPositionDelta,(probe.item->position()-expected).length());
    probe.maximumRotationDelta=std::max(probe.maximumRotationDelta,savedItemRotationDelta(probe.saved,probe.item->transform()));
    probe.alwaysSleeping&=probe.item->physicsSleeping();
  }
}

void savedItemInitial(World& world,Npc& player,uint64_t initialDt) {
  if(initialDt!=0)throw std::runtime_error("item save observation must precede the first native step");
  const auto* path=envStr("SAVE");
  if(path==nullptr)throw std::runtime_error("item save observation requires the loaded SAVE archive");
  std::ifstream file(path,std::ios::binary);
  if(!file)throw std::runtime_error("cannot open loaded item save archive");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
  MemReader reader(bytes);Serialize in(reader);in.setContext(&world);
  if(!in.setEntry("worlds/",world.name(),"/item-physics"))
    throw std::runtime_error("loaded archive has no native item physics entry");
  uint32_t magic=0,total=0,count=0;
  uint16_t schema=0;
  std::string worldName;
  in.read(magic,schema,worldName,total,count);
  if(magic!=0x31505449 || schema!=1 || worldName!=world.name() || count>total || count>100000)
    throw std::runtime_error("loaded item physics entry has unexpected identity");
  const auto symbol=world.script().findSymbolIndex("ITFO_BEER");
  bench.savedItemProbes.clear();
  std::string items="[";
  for(uint32_t i=0;i<count;++i) {
    uint32_t index=0,instance=0;
    std::string visual;
    Matrix4x4 matrix;
    Vec3 velocity,angular;
    bool sleeping=false;
    in.read(index,instance,visual,matrix,velocity,angular,sleeping);
    const Vec3 point(matrix.at(3,0),matrix.at(3,1),matrix.at(3,2));
    if(instance!=symbol || (point-player.position()).length()>400.f)continue;
    if(!sleeping)throw std::runtime_error("item save fixture contains an awake dropped Item");
    Item* found=nullptr;
    world.detectItem(point,1.f,[&](Item& item) {
      if(item.clsId()!=instance || item.handle().visual!=visual)return;
      if(found!=nullptr)throw std::runtime_error("item save fixture has ambiguous loaded Item positions");
      found=&item;
    });
    if(found==nullptr || !found->isDynamic() || !found->physicsSleeping())
      throw std::runtime_error("saved sleeping dropped Item was not restored with its native body");
    if(std::any_of(bench.savedItemProbes.begin(),bench.savedItemProbes.end(),[&](const auto& probe){return probe.item==found;}))
      throw std::runtime_error("saved item records resolved to the same loaded Item");
    Bench::SavedItemProbe probe;probe.item=found;probe.saved=matrix;
    bench.savedItemProbes.push_back(probe);
    if(items.size()>1)items+=',';
    items+="{\"index\":"+std::to_string(index)+",\"symbol\":"+std::to_string(instance)+
      ",\"visual\":"+jsonStr(visual)+",\"saved_transform\":"+ragdollMatrix(matrix)+
      ",\"loaded_transform\":"+ragdollMatrix(found->transform())+
      ",\"saved_sleeping\":true,\"loaded_sleeping\":true}";
  }
  if(bench.savedItemProbes.size()!=10)
    throw std::runtime_error("item save fixture requires exactly ten saved sleeping dropped beer Items");
  bench.savedItemStart=world.tickCount();bench.savedItemSample=uint64_t(-1);bench.savedItemTracing=true;
  savedItemSample(world);
  float position=0,rotation=0;
  for(const auto& probe:bench.savedItemProbes) {
    position=std::max(position,probe.maximumPositionDelta);rotation=std::max(rotation,probe.maximumRotationDelta);
  }
  event("item_save_initial","\"items\":"+items+"],\"count\":10,\"saved_sleepers\":10,\"loaded_sleepers\":10"+
    std::string(",\"initial_dt\":0,\"maximum_position_delta_cm\":")+ragdollNumber(position)+
    ",\"maximum_rotation_delta_degrees\":"+ragdollNumber(rotation)+
    ",\"pass\":"+(position<=.1f && rotation<=.1f?"true":"false"));
}

void savedItemReport(World& world) {
  if(!bench.savedItemTracing)throw std::runtime_error("item save observation was not initialized");
  savedItemSample(world);bench.savedItemTracing=false;
  const auto elapsed=world.tickCount()-bench.savedItemStart;
  float position=0,rotation=0;size_t sleeping=0;
  for(const auto& probe:bench.savedItemProbes) {
    position=std::max(position,probe.maximumPositionDelta);rotation=std::max(rotation,probe.maximumRotationDelta);
    sleeping+=probe.alwaysSleeping;
  }
  event("item_save_stability","\"count\":"+std::to_string(bench.savedItemProbes.size())+
    ",\"elapsed_ms\":"+std::to_string(elapsed)+",\"always_sleeping\":"+std::to_string(sleeping)+
    ",\"maximum_position_delta_cm\":"+ragdollNumber(position)+
    ",\"maximum_rotation_delta_degrees\":"+ragdollNumber(rotation)+
    ",\"pass\":"+(elapsed>=3000 && position<=.1f && rotation<=.1f && sleeping==10?"true":"false"));
}

}
