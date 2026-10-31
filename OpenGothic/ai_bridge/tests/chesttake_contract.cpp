#include "chesttake.h"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if(condition) return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }
}

int main() {
  using ChestTake::Request;
  using ChestTake::Stock;
  std::map<std::pair<std::string,std::string>,Stock> chests{
    {{"CHEST_A","GOLD"},{3,false}},
    {{"CHEST_A","LETTER"},{1,true}},
    {{"CHEST_B","GOLD"},{2,false}},
    };
  const auto inspect = [&](const std::string& chest, const std::string& item) -> std::optional<Stock> {
    if(chest!="CHEST_A" && chest!="CHEST_B")
      return std::nullopt;
    const auto found = chests.find({chest,item});
    return found==chests.end() ? Stock{} : found->second;
    };
  require(ChestTake::check({Request{"CHEST_A","GOLD",3}},inspect).empty(),
          "the exact available stack may pass");
  require(ChestTake::check({Request{"CHEST_A","GOLD",4}},inspect)==
          "missing: chest CHEST_A GOLD 4 (has 3)","a shortage rejects the whole requested amount");
  require(ChestTake::check({Request{"CHEST_A","LETTER",1}},inspect)=="a mission item: LETTER",
          "a mission stack cannot be taken even when sufficient");
  require(ChestTake::check({Request{"CHEST_A","NONE",1}},inspect)==
          "missing: chest CHEST_A NONE 1 (has 0)","a missing stack has zero available");
  require(ChestTake::check({Request{"NONE","GOLD",1}},inspect)=="no chest NONE",
          "an unknown chest rejects preflight");
  require(ChestTake::check({Request{"CHEST_A","GOLD",2},Request{"CHEST_A","GOLD",2}},inspect)==
          "missing: chest CHEST_A GOLD 4 (has 3)","repeated requests cannot overdraw the same stack");
  require(ChestTake::check({Request{"CHEST_A","GOLD",2},Request{"CHEST_B","GOLD",2}},inspect).empty(),
          "stacks in different chests are counted separately");
  require(ChestTake::check({Request{"CHEST_A","GOLD",1},Request{"CHEST_B","GOLD",3}},inspect)==
          "missing: chest CHEST_B GOLD 3 (has 2)","a later source can reject an otherwise valid batch");
  require(chests.at({"CHEST_A","GOLD"}).count==3 && chests.at({"CHEST_B","GOLD"}).count==2,
          "preflight has no transfer or perception side effects, including on failure");
  require(ChestTake::check({},inspect).empty(),"an effect batch without chest_take remains unaffected");
  std::printf("Chest take contract: %d checks passed\n",checks);
  }
