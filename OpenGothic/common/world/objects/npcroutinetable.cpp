#include "npcroutinetable.h"
#include "world/waypoint.h"

#include <algorithm>

std::string_view NpcRoutineTable::Row::wayPointName() const {
  return point!=nullptr ? point->name : fallbackName;
  }

void NpcRoutineTable::add(std::vector<Row>& routines, gtime start, gtime end, uint32_t callback,
                          const WayPoint* point, std::string_view name) {
  Row row;
  row.start=start;
  row.end=end;
  row.callback=callback;
  row.point=point;
  if(point==nullptr)
    row.fallbackName=name;
  routines.push_back(std::move(row));
  std::stable_sort(routines.begin(),routines.end(),[](const Row& left,const Row& right) {
    return left.start<right.start;
    });
  }
