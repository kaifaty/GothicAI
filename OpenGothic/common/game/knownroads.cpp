#include "knownroads.h"

#include <algorithm>
#include <cmath>
#include <functional>
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

Projection project(const std::vector<Vec3>& line, const Vec3& position) {
  Projection result;
  result.point = line.front();
  double best = double((position-line.front()).quadLength());
  double along = 0;
  for(size_t i=0; i+1<line.size(); ++i) {
    const auto edge = line[i+1]-line[i];
    const double squared = double(edge.quadLength());
    const double length = std::sqrt(squared);
    const auto delta = position-line[i];
    const double dot = double(delta.x)*edge.x+double(delta.y)*edge.y+double(delta.z)*edge.z;
    const double t = squared>0 ? std::clamp(dot/squared,0.,1.) : 0.;
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

struct Sample final {
  double alongCm = 0;
  double distanceCm = 0;
  };

// Distance from the actor to the route over [fromCm, toCm] of its length,
// sampled at segment ends and feet: between two samples it is monotone.
std::vector<Sample> samples(const std::vector<Vec3>& line, const Vec3& position,
                            double fromCm, double toCm) {
  std::vector<Sample> result;
  const auto add = [&](double along, const Vec3& point) {
    if(result.empty() || along>result.back().alongCm)
      result.push_back({along,std::sqrt(double((point-position).quadLength()))});
    };
  if(line.size()==1) {
    add(0,line.front());
    return result;
    }
  double along = 0;
  for(size_t i=0; i+1<line.size() && along<=toCm; ++i) {
    const auto edge = line[i+1]-line[i];
    const double squared = double(edge.quadLength());
    const double length = std::sqrt(squared);
    const double from = std::max(along,fromCm), to = std::min(along+length,toCm);
    const auto at = [&](double distance) {
      return length>0 ? line[i]+edge*float(std::clamp((distance-along)/length,0.,1.)) : line[i];
      };
    if(from<=to) {
      add(from,at(from));
      if(squared>0) {
        const auto delta = position-line[i];
        const double foot = along+(double(delta.x)*edge.x+double(delta.y)*edge.y+double(delta.z)*edge.z)/squared*length;
        if(foot>from && foot<to)
          add(foot,at(foot));
        }
      add(to,at(to));
      }
    along += length;
    }
  return result;
  }

// Progress on the route: the closest local minimum of the distance within
// [fromCm, toCm], never below floorCm. A minimum pinned at the forward edge is
// a later stretch beyond the window and does not count: progress cannot creep
// there window by window, even while the actor stands still.
double windowProgress(const std::vector<Vec3>& line, const Vec3& position,
                      double fromCm, double toCm, double floorCm) {
  double length = 0;
  for(size_t i=0; i+1<line.size(); ++i)
    length += std::sqrt(double((line[i+1]-line[i]).quadLength()));
  const auto sampled = samples(line,position,fromCm,toCm);
  const Sample* best = nullptr;
  for(size_t k=0; k<sampled.size(); ++k) {
    const auto& sample = sampled[k];
    const bool left = k==0 || sample.distanceCm<=sampled[k-1].distanceCm;
    const bool right = k+1<sampled.size() ? sample.distanceCm<=sampled[k+1].distanceCm
                                          : sample.alongCm>=length-.01;
    // Strict comparison preserves the earliest minimum at crossings.
    if(left && right && (best==nullptr || sample.distanceCm<best->distanceCm))
      best = &sample;
    }
  return best==nullptr ? floorCm : std::max(floorCm,best->alongCm);
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

// The actor has cut a loop of the road: the closest point of the whole route
// lies later than the window's projection and closer to the actor. Progress
// takes it only through a straight passage the caller's memory has seen free;
// a stretch behind a seen wall or out of sight stays outside the window.
bool catchUp(const std::vector<Vec3>& line, const Vec3& position, double& progressCm,
             const KnownRoads::PassageCheck& passage) {
  if(!passage || line.size()<2)
    return false;
  const auto whole = project(line,position);
  const double current = double((pointAt(line,progressCm)-position).length());
  // One centimetre keeps rounding of the same point from counting as a later stretch.
  if(whole.alongCm<=progressCm+1. || whole.distanceCm+1.>=current)
    return false;
  if(passage(position,whole.point)!=KnownRoads::Passage::Free)
    return false;
  progressCm = whole.alongCm;
  return true;
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

KnownRoads::Path KnownRoads::path(const Knowledge& knowledge, const Vec3& start, const Vec3& goal,
                                  const PassageCheck& passage, size_t departure) const {
  validate(knowledge);
  Path result;
  size_t begin = invalid;
  double closestStart = std::numeric_limits<double>::infinity();
  std::vector<std::pair<double,size_t>> candidates;
  for(size_t i=0; i<graphNodes.size(); ++i) {
    if(!knowledge.nodes[i])
      continue;
    const double distance = double((graphNodes[i].position-start).quadLength());
    if(passage)
      candidates.push_back({distance,i});
    if(distance<closestStart) {
      closestStart = distance;
      begin = i;
      }
    }
  if(passage && begin!=invalid) {
    // The closest node memory does not close off; ties keep the lower ID, as above.
    const size_t count = std::min(candidates.size(),startCandidates);
    std::partial_sort(candidates.begin(),candidates.begin()+std::ptrdiff_t(count),candidates.end());
    result.startFallback = true;
    for(size_t k=0; k<count; ++k) {
      if(passage(start,graphNodes[candidates[k].second].position)==Passage::Closed) {
        ++result.startSkipped;
        continue;
        }
      begin = candidates[k].second;
      closestStart = candidates[k].first;
      result.startFallback = false;
      break;
      }
    if(result.startFallback) {
      result.startSkipped = 0;
      // Every candidate is behind a seen hazard: go back to where the walker
      // left its previous route; it has walked from there itself.
      if(departure<graphNodes.size() && knowledge.nodes[departure]) {
        begin = departure;
        closestStart = double((graphNodes[departure].position-start).quadLength());
        result.startDeparture = true;
        }
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
                               const Vec3& goal, Guidance& guidance, const PassageCheck& passage) const {
  validate(knowledge);
  guidance.rebuilt = false;
  guidance.caughtUp = false;
  bool rebuild = !guidance.initialized || (goal-guidance.goal).quadLength()>.0001f;
  if(!rebuild && !guidance.polyline.empty()) {
    // Deviation is measured from the whole familiar route, as before.
    rebuild = !guidance.beyondKnown && project(guidance.polyline,position).distanceCm>guidanceReplanCm;
    // Pure-pursuit window: progress moves continuously along the route and
    // cannot skip to a later stretch that merely passes nearby.
    if(!rebuild) {
      guidance.progressCm = windowProgress(guidance.polyline,position,guidance.progressCm-guidanceWindowCm,
                                           guidance.progressCm+guidanceWindowCm,guidance.progressCm);
      if(!guidance.beyondKnown)
        guidance.caughtUp = catchUp(guidance.polyline,position,guidance.progressCm,passage);
      }
    }
  if(rebuild) {
    // The last node of the previous route that progress has passed, for the
    // same goal: where the walker left that route.
    size_t departure = invalid;
    if(guidance.initialized && (goal-guidance.goal).quadLength()<=.0001f && !guidance.route.nodes.empty()) {
      size_t at = 0;
      double along = 0;
      for(size_t i=0; i+1<guidance.polyline.size(); ++i) {
        along += double((guidance.polyline[i+1]-guidance.polyline[i]).length());
        if(along>guidance.progressCm)
          break;
        at = i+1;
        }
      departure = guidance.route.nodes[std::min(at,guidance.route.nodes.size()-1)];
      }
    guidance = Guidance{};
    guidance.initialized = guidance.rebuilt = true;
    guidance.goal = goal;
    guidance.route = path(knowledge,position,goal,passage,departure);
    for(const auto node:guidance.route.nodes)
      guidance.polyline.push_back(graphNodes[node].position);
    for(size_t i=0; i+1<guidance.polyline.size(); ++i)
      guidance.knownLengthCm += double((guidance.polyline[i+1]-guidance.polyline[i]).length());
    // The actor stands at startOffsetCm from the first node: its projection
    // can be at most that far along the route, plus one window.
    if(!guidance.polyline.empty()) {
      guidance.progressCm = windowProgress(guidance.polyline,position,0,
                                           guidance.route.startOffsetCm+guidanceWindowCm,0);
      guidance.caughtUp = catchUp(guidance.polyline,position,guidance.progressCm,passage);
      }
    }
  guidance.remainingKnownCm = std::max(0.,guidance.knownLengthCm-guidance.progressCm);
  // The terminal handoff is permanent for this episode: the subsequent
  // approach to the goal must not replan back to the familiar endpoint.
  guidance.beyondKnown = guidance.beyondKnown || guidance.remainingKnownCm<=guidanceLookaheadCm;
  guidance.carrot = guidance.beyondKnown ? goal :
      pointAt(guidance.polyline,guidance.progressCm+guidanceLookaheadCm);
  }
