// Offline replay of familiar-road guidance (KnownRoads::updateGuidance) on
// recorded positions; no world, physics or policy. scripts/nav_roads_carrot.py
// writes the text input and compiles this file together with one revision of
// OpenGothic/common/game/knownroads.cpp, so the same trajectories are replayed
// by the old and the new rule.
//
// Input (whitespace separated):
//   graph N E, then N lines "x y z", then E lines "a b length_cm major"
//   episode ID stranger R node... spawn_x spawn_y spawn_z goal_x goal_y goal_z T,
//   then T lines "x y z"
// Output: per episode "episode ID known_nodes known_edges", per decision
//   "d progress_cm rebuilt beyond carrot_x carrot_y carrot_z remaining_known_cm",
//   and after each rebuild "polyline K x y z ...".
#include "game/knownroads.h"

#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
  std::ios::sync_with_stdio(false);
  std::unique_ptr<KnownRoads> roads;
  std::string word;
  while(std::cin >> word) {
    if(word=="graph") {
      size_t nodeCount=0, edgeCount=0;
      std::cin >> nodeCount >> edgeCount;
      std::vector<KnownRoads::Node> nodes(nodeCount);
      for(auto& node:nodes)
        std::cin >> node.position.x >> node.position.y >> node.position.z;
      std::vector<KnownRoads::Edge> edges(edgeCount);
      for(auto& edge:edges) {
        int major=0;
        std::cin >> edge.a >> edge.b >> edge.lengthCm >> major;
        edge.major = major!=0;
        }
      if(!std::cin)
        throw std::runtime_error("truncated graph");
      roads = std::make_unique<KnownRoads>(std::move(nodes),std::move(edges));
      continue;
      }
    if(word!="episode" || !roads)
      throw std::runtime_error("expected graph before episode, got "+word);
    std::string id;
    int stranger=0;
    size_t routineCount=0;
    std::cin >> id >> stranger >> routineCount;
    std::vector<size_t> routine(routineCount);
    for(auto& node:routine)
      std::cin >> node;
    Tempest::Vec3 spawn, goal;
    size_t decisions=0;
    std::cin >> spawn.x >> spawn.y >> spawn.z >> goal.x >> goal.y >> goal.z >> decisions;
    if(!std::cin)
      throw std::runtime_error("truncated episode header "+id);
    const auto knowledge = roads->build(routine,spawn,stranger!=0);
    size_t knownNodes=0, knownEdges=0;
    for(auto bit:knowledge.nodes) knownNodes += bit!=0;
    for(auto bit:knowledge.edges) knownEdges += bit!=0;
    std::printf("episode %s %zu %zu\n",id.c_str(),knownNodes,knownEdges);
    KnownRoads::Guidance guidance;
    for(size_t i=0; i<decisions; ++i) {
      Tempest::Vec3 position;
      std::cin >> position.x >> position.y >> position.z;
      if(!std::cin)
        throw std::runtime_error("truncated trajectory "+id);
      roads->updateGuidance(knowledge,position,goal,guidance);
      std::printf("d %.4f %d %d %.4f %.4f %.4f %.4f\n",guidance.progressCm,int(guidance.rebuilt),
                  int(guidance.beyondKnown),guidance.carrot.x,guidance.carrot.y,guidance.carrot.z,
                  guidance.remainingKnownCm);
      if(guidance.rebuilt) {
        std::printf("polyline %zu",guidance.polyline.size());
        for(const auto& point:guidance.polyline)
          std::printf(" %.4f %.4f %.4f",point.x,point.y,point.z);
        std::printf("\n");
        }
      }
    }
  return 0;
  }
