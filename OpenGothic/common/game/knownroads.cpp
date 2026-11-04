#include "knownroads.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>

using Tempest::Vec3;

namespace {
struct Projection final {
  Vec3 point = {};
  size_t segment = 0;
  double alongCm = 0;
  double distanceCm = 0;
  };

// Closest point of the polyline restricted to [fromCm, toCm] of its length.
// The window keeps the projection on the stretch the actor is following: a road
// that bends back behind a wall can be closer in space but is far ahead in route.
Projection project(const std::vector<Vec3>& line, const Vec3& position,
                   double fromCm = 0, double toCm = std::numeric_limits<double>::infinity()) {
  Projection result;
  result.point = line.front();
  double best = std::numeric_limits<double>::infinity();
  if(fromCm<=0)
    best = double((position-line.front()).quadLength());
  double along = 0;
  for(size_t i=0; i+1<line.size(); ++i) {
    const auto edge = line[i+1]-line[i];
    const double squared = double(edge.quadLength());
    const double length = std::sqrt(squared);
    if(along>toCm)
      break;
    if(along+length<fromCm) {
      along += length;
      continue;
      }
    const double low = length>0 ? std::max(0.,(fromCm-along)/length) : 0.;
    const double high = length>0 ? std::min(1.,(toCm-along)/length) : 0.;
    const auto delta = position-line[i];
    const double dot = double(delta.x)*edge.x+double(delta.y)*edge.y+double(delta.z)*edge.z;
    const double t = squared>0 ? std::clamp(dot/squared,low,high) : 0.;
    const auto point = line[i]+edge*float(t);
    const double distance = double((point-position).quadLength());
    // Strict comparison preserves the earliest projection at crossings.
    if(distance<best) {
      best = distance;
      result.point = point;
      result.segment = i;
      result.alongCm = along+t*length;
      }
    along += length;
    }
  result.distanceCm = std::sqrt(best);
  return result;
  }

Vec3 pointAt(const std::vector<Vec3>& line, double distance) {
  for(size_t i=0; i+1<line.size(); ++i) {
    const auto edge = line[i+1]-line[i];
    const double length = double(edge.length());
    if(length>0 && distance<length)
      return line[i]+edge*float(distance/length);
    distance -= length;
    }
  return line.back();
  }
}

KnownRoads::KnownRoads(std::vector<Node> nodes, std::vector<Edge> edges)
  : graphNodes(std::move(nodes)), graphEdges(std::move(edges)),
    adjacency(graphNodes.size()), majorNodes(graphNodes.size(),0) {
  for(size_t i=0; i<graphNodes.size(); ++i) {
    const auto& p = graphNodes[i].position;
    if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      throw std::invalid_argument("KnownRoads: non-finite node position");
    }
  for(size_t i=0; i<graphEdges.size(); ++i) {
    const auto& edge = graphEdges[i];
    if(edge.a>=graphNodes.size() || edge.b>=graphNodes.size() ||
       !std::isfinite(edge.lengthCm) || edge.lengthCm<0)
      throw std::invalid_argument("KnownRoads: invalid graph edge");
    adjacency[edge.a].push_back(i);
    if(edge.b!=edge.a)
      adjacency[edge.b].push_back(i);
    if(edge.major)
      majorNodes[edge.a] = majorNodes[edge.b] = 1;
    }
  }

size_t KnownRoads::other(size_t edge, size_t node) const {
  const auto& e = graphEdges[edge];
  return e.a==node ? e.b : e.a;
  }

void KnownRoads::validate(const Knowledge& knowledge) const {
  if(knowledge.nodes.size()!=graphNodes.size() || knowledge.edges.size()!=graphEdges.size())
    throw std::invalid_argument("KnownRoads: knowledge belongs to another graph");
  }

size_t KnownRoads::nearest(const Vec3& position) const {
  size_t result = invalid;
  double best = std::numeric_limits<double>::infinity();
  for(size_t i=0; i<graphNodes.size(); ++i) {
    const double distance = double((graphNodes[i].position-position).quadLength());
    if(distance<best) {
      result = i;
      best = distance;
      }
    }
  return result;
  }

KnownRoads::Search KnownRoads::search(const Seeds& seeds, const std::vector<uint8_t>* allowed,
                                    bool majorOnly, double limit) const {
  Search result;
  result.distance.assign(graphNodes.size(),std::numeric_limits<double>::infinity());
  result.previous.assign(graphNodes.size(),invalid);
  using Entry = std::pair<double,size_t>;
  std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
  for(const auto& seed:seeds) {
    if(seed.first>=graphNodes.size() || seed.second>limit || seed.second>=result.distance[seed.first])
      continue;
    result.distance[seed.first] = seed.second;
    queue.push({seed.second,seed.first});
    }
  while(!queue.empty()) {
    const auto current = queue.top();
    queue.pop();
    if(current.first!=result.distance[current.second])
      continue;
    for(size_t id:adjacency[current.second]) {
      const auto& edge = graphEdges[id];
      if((majorOnly && !edge.major) || (allowed!=nullptr && !(*allowed)[id]))
        continue;
      const size_t next = other(id,current.second);
      const double distance = current.first+double(edge.lengthCm);
      if(distance>limit || distance>=result.distance[next])
        continue;
      result.distance[next] = distance;
      result.previous[next] = id;
      queue.push({distance,next});
      }
    }
  return result;
  }

void KnownRoads::learnNeighborhood(Knowledge& knowledge, const std::vector<size_t>& centers) const {
  Seeds seeds;
  for(size_t node:centers)
    if(node<graphNodes.size())
      seeds.push_back({node,0});
  const auto found = search(seeds,nullptr,false,neighborhoodCm);
  for(size_t i=0; i<graphNodes.size(); ++i)
    if(std::isfinite(found.distance[i]))
      knowledge.nodes[i] = 1;
  for(size_t i=0; i<graphEdges.size(); ++i) {
    const auto& edge = graphEdges[i];
    if(std::isfinite(found.distance[edge.a]) && std::isfinite(found.distance[edge.b]))
      knowledge.edges[i] = 1;
    }
  }

void KnownRoads::learnPath(Knowledge& knowledge, const Search& found, size_t end) const {
  if(end>=graphNodes.size() || !std::isfinite(found.distance[end]))
    return;
  knowledge.nodes[end] = 1;
  while(found.previous[end]!=invalid) {
    const size_t id = found.previous[end];
    knowledge.edges[id] = 1;
    end = other(id,end);
    knowledge.nodes[end] = 1;
    }
  }

void KnownRoads::learnMajor(Knowledge& knowledge, const std::vector<uint8_t>& neighborhood,
                           size_t spawn) const {
  Seeds seeds;
  for(size_t i=0; i<graphNodes.size(); ++i)
    if(neighborhood[i])
      seeds.push_back({i,0});
  if(seeds.empty() && spawn<graphNodes.size())
    seeds.push_back({spawn,0});
  // The radius is distance along WayMatrix, even where a short non-major
  // segment separates road classifications. Only major edges are learned;
  // nearby disconnected road fragments do not imply knowledge of the seam.
  const auto found = search(seeds,nullptr,false,majorRangeCm);
  for(size_t i=0; i<graphNodes.size(); ++i)
    if(majorNodes[i] && std::isfinite(found.distance[i]))
      knowledge.nodes[i] = 1;
  for(size_t i=0; i<graphEdges.size(); ++i) {
    const auto& edge = graphEdges[i];
    if(edge.major && std::isfinite(found.distance[edge.a]) && std::isfinite(found.distance[edge.b]))
      knowledge.edges[i] = 1;
    }
  }

KnownRoads::Knowledge KnownRoads::build(const std::vector<size_t>& routineNodes,
                                      const Vec3& spawn, bool stranger) const {
  Knowledge result;
  result.nodes.assign(graphNodes.size(),0);
  result.edges.assign(graphEdges.size(),0);
  std::vector<size_t> routine;
  for(size_t node:routineNodes)
    if(node<graphNodes.size())
      routine.push_back(node);
  result.stranger = stranger || routine.empty();
  const size_t arrival = nearest(spawn);
  if(result.stranger) {
    learnMajor(result,result.nodes,arrival);
    return result;
    }

  learnNeighborhood(result,routine);
  const auto neighborhood = result.nodes;
  for(size_t i=0; i<routine.size(); ++i) {
    const size_t next = routine[(i+1)%routine.size()];
    if(routine[i]==next)
      continue;
    const auto found = search({{routine[i],0}},nullptr,false);
    learnPath(result,found,next);
    }
  learnMajor(result,neighborhood,arrival);
  return result;
  }

bool KnownRoads::add(Knowledge& knowledge, const std::vector<size_t>& destinationNodes) const {
  validate(knowledge);
  Knowledge destination;
  destination.nodes.assign(graphNodes.size(),0);
  destination.edges.assign(graphEdges.size(),0);
  learnNeighborhood(destination,destinationNodes);

  Seeds sources;
  for(size_t i=0; i<graphNodes.size(); ++i)
    if(knowledge.nodes[i] && majorNodes[i])
      sources.push_back({i,0});
  const auto connector = search(sources,nullptr,true);
  size_t end = invalid;
  double best = std::numeric_limits<double>::infinity();
  for(size_t i=0; i<graphNodes.size(); ++i)
    if(destination.nodes[i] && majorNodes[i] && connector.distance[i]<best) {
      best = connector.distance[i];
      end = i;
      }
  learnPath(knowledge,connector,end);
  for(size_t i=0; i<graphNodes.size(); ++i)
    knowledge.nodes[i] |= destination.nodes[i];
  for(size_t i=0; i<graphEdges.size(); ++i)
    knowledge.edges[i] |= destination.edges[i];
  return end!=invalid;
  }

KnownRoads::Path KnownRoads::path(const Knowledge& knowledge, const Vec3& start, const Vec3& goal) const {
  validate(knowledge);
  Path result;
  size_t begin = invalid;
  double closestStart = std::numeric_limits<double>::infinity();
  for(size_t i=0; i<graphNodes.size(); ++i) {
    if(!knowledge.nodes[i])
      continue;
    const double distance = double((graphNodes[i].position-start).quadLength());
    if(distance<closestStart) {
      closestStart = distance;
      begin = i;
      }
    }
  if(begin==invalid) {
    const auto offset = goal-start;
    result.remainingCm = double(offset.length());
    if(result.remainingCm>0)
      result.direction = offset/float(result.remainingCm);
    return result;
    }
  result.startOffsetCm = std::sqrt(closestStart);
  const auto found = search({{begin,0}},&knowledge.edges,false);
  size_t end = begin;
  double closestGoal = std::numeric_limits<double>::infinity();
  for(size_t i=0; i<graphNodes.size(); ++i) {
    if(!knowledge.nodes[i])
      continue;
    const double distance = double((graphNodes[i].position-goal).quadLength());
    if(distance<=1.)
      result.goalInKnown = true;
    if(!std::isfinite(found.distance[i]))
      continue;
    if(distance<closestGoal) {
      closestGoal = distance;
      end = i;
      }
    }
  result.lengthCm = found.distance[end];
  result.remainingCm = std::sqrt(closestGoal);
  if(result.remainingCm>0)
    result.direction = (goal-graphNodes[end].position)/float(result.remainingCm);
  result.nodes.push_back(end);
  while(found.previous[end]!=invalid) {
    const size_t id = found.previous[end];
    result.edges.push_back(id);
    end = other(id,end);
    result.nodes.push_back(end);
    }
  std::reverse(result.nodes.begin(),result.nodes.end());
  std::reverse(result.edges.begin(),result.edges.end());
  return result;
  }

void KnownRoads::updateGuidance(const Knowledge& knowledge, const Vec3& position,
                               const Vec3& goal, Guidance& guidance) const {
  validate(knowledge);
  guidance.rebuilt = false;
  bool rebuild = !guidance.initialized || (goal-guidance.goal).quadLength()>.0001f;
  if(!rebuild && !guidance.polyline.empty()) {
    // Deviation is measured from the whole familiar route, as before.
    rebuild = !guidance.beyondKnown && project(guidance.polyline,position).distanceCm>guidanceReplanCm;
    // Pure-pursuit window: progress moves continuously along the route and
    // cannot skip to a later stretch that merely passes nearby.
    if(!rebuild)
      guidance.progressCm = std::max(guidance.progressCm,
          project(guidance.polyline,position,guidance.progressCm-guidanceWindowCm,
                  guidance.progressCm+guidanceWindowCm).alongCm);
    }
  if(rebuild) {
    guidance = Guidance{};
    guidance.initialized = guidance.rebuilt = true;
    guidance.goal = goal;
    guidance.route = path(knowledge,position,goal);
    for(const auto node:guidance.route.nodes)
      guidance.polyline.push_back(graphNodes[node].position);
    for(size_t i=0; i+1<guidance.polyline.size(); ++i)
      guidance.knownLengthCm += double((guidance.polyline[i+1]-guidance.polyline[i]).length());
    // The actor stands at startOffsetCm from the first node: its projection
    // can be at most that far along the route, plus one window.
    if(!guidance.polyline.empty())
      guidance.progressCm = project(guidance.polyline,position,0,
                                    guidance.route.startOffsetCm+guidanceWindowCm).alongCm;
    }
  guidance.remainingKnownCm = std::max(0.,guidance.knownLengthCm-guidance.progressCm);
  // The terminal handoff is permanent for this episode: the subsequent
  // approach to the goal must not replan back to the familiar endpoint.
  guidance.beyondKnown = guidance.beyondKnown || guidance.remainingKnownCm<=guidanceLookaheadCm;
  guidance.carrot = guidance.beyondKnown ? goal :
      pointAt(guidance.polyline,guidance.progressCm+guidanceLookaheadCm);
  }
