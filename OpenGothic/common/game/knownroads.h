#pragma once

#include <Tempest/Vec>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Opt-in road knowledge, independent of world/physics and of actual movement.
// The stand copies WayMatrix into this graph; all distances are centimetres.
class KnownRoads final {
  public:
    static constexpr size_t invalid = size_t(-1);
    static constexpr float neighborhoodCm = 4000.f;
    static constexpr float majorRangeCm = 30000.f;
    static constexpr float guidanceLookaheadCm = 600.f;
    static constexpr float guidanceReplanCm = 800.f;
    // Progress is the closest local minimum of the actor's distance to the
    // route within this distance along it around the current progress: the 6 m
    // lookahead plus the largest recorded XZ step of one 100 ms decision
    // (50.4 cm). A minimum at the window's forward edge does not count. A
    // rebuilt route projects on its first startOffsetCm plus this window.
    static constexpr float guidanceWindowCm = 650.f;
    // Closest familiar nodes tried for the start of a rebuilt route; if memory
    // closes all of them, the route starts where the walker left its previous
    // route, and the first route at the closest node, as before.
    static constexpr size_t startCandidates = 32;

    // The caller's verdict on a straight passage, from the walker's own memory:
    // Free, seen free all along; Closed, a seen static hazard crosses it;
    // Unknown otherwise. KnownRoads itself has no geometry.
    enum class Passage : uint8_t { Unknown, Free, Closed };
    using PassageCheck = std::function<Passage(const Tempest::Vec3& from, const Tempest::Vec3& to)>;

    struct Node final {
      std::string name;
      Tempest::Vec3 position = {};
      };
    struct Edge final {
      size_t a = 0, b = 0;
      float lengthCm = 0;
      bool major = false;
      };
    struct Knowledge final {
      std::vector<uint8_t> nodes, edges;
      bool stranger = false;
      };
    struct Path final {
      std::vector<size_t> nodes, edges;
      double lengthCm = 0;
      double remainingCm = 0;
      double startOffsetCm = 0;
      Tempest::Vec3 direction = {}; // unit vector from road endpoint to goal
      // A known node matches the goal position (within 1 cm), even if it is in
      // another familiar component. Never consults unknown nodes or edges.
      bool goalInKnown = false;
      // Diagnosis of the start: closer familiar nodes skipped because memory
      // closed the passage to them; fallback, every candidate was closed;
      // departure, then the route starts at the departure node, otherwise at
      // the closest node.
      uint32_t startSkipped = 0;
      bool startFallback = false;
      bool startDeparture = false;
      };
    // One episode's coarse familiar route. The polyline contains only graph
    // nodes: lookahead is measured on the road from the actor's projection,
    // never along an off-road connector. route keeps the graph IDs.
    struct Guidance final {
      Path route;
      std::vector<Tempest::Vec3> polyline;
      Tempest::Vec3 goal = {}, carrot = {};
      double progressCm = 0;
      double knownLengthCm = 0;
      double remainingKnownCm = 0;
      bool initialized = false;
      bool beyondKnown = false;
      bool rebuilt = false;
      // Diagnosis of this update: progress took a later stretch beyond its
      // window through a passage memory has seen free.
      bool caughtUp = false;
      };

    KnownRoads(std::vector<Node> nodes, std::vector<Edge> edges);
    const std::vector<Node>& nodes() const { return graphNodes; }
    const std::vector<Edge>& edges() const { return graphEdges; }
    size_t nearest(const Tempest::Vec3& position) const;

    // The caller orders all daily routine nodes by time. The last-to-first
    // trip is included. An empty routine is a stranger, as is an explicit mark.
    Knowledge build(const std::vector<size_t>& routineNodes, const Tempest::Vec3& spawn,
                    bool stranger = false) const;
    // Learn the destination neighbourhood and connect it to existing knowledge
    // through major roads. False means there is no such major-road connection;
    // the neighbourhood itself is still learned (it can be a separate island).
    bool add(Knowledge& knowledge, const std::vector<size_t>& destinationNodes) const;
    // Start at the closest known node, then approach the goal as closely as the
    // reachable familiar component permits. Search never crosses unknown edges.
    // With a passage check, the start is the closest known node that memory
    // does not close off from `start` (of the startCandidates closest); if
    // memory closes them all, the known `departure` node (where the walker
    // left its previous route), else the closest node.
    Path path(const Knowledge& knowledge, const Tempest::Vec3& start,
              const Tempest::Vec3& goal, const PassageCheck& passage = {},
              size_t departure = invalid) const;
    // No mesh or hidden geometry. Reset Guidance at an episode boundary.
    // Progress cannot decrease and never leaves its window, also not window by
    // window while the actor stands; only a >8 m deviation from the whole route
    // rebuilds it. With a passage check, progress may take the closest point of
    // the whole route when it lies later than the window's projection, is
    // closer to the actor and memory has seen the straight passage to it free:
    // the actor has cut a loop of the road. Without a check the rules are the
    // window alone and the closest start node, as before.
    // If the 6 m lookahead passes the familiar endpoint, eyes take over:
    // carrot becomes the goal and beyondKnown remains true until reset.
    void updateGuidance(const Knowledge& knowledge, const Tempest::Vec3& position,
                        const Tempest::Vec3& goal, Guidance& guidance,
                        const PassageCheck& passage = {}) const;

  private:
    struct Search final {
      std::vector<double> distance;
      std::vector<size_t> previous;
      };
    using Seeds = std::vector<std::pair<size_t,double>>;
    Search search(const Seeds& seeds, const std::vector<uint8_t>* allowed,
                  bool majorOnly, double limit = std::numeric_limits<double>::infinity()) const;
    void learnNeighborhood(Knowledge& knowledge, const std::vector<size_t>& centers) const;
    void learnPath(Knowledge& knowledge, const Search& found, size_t end) const;
    void learnMajor(Knowledge& knowledge, const std::vector<uint8_t>& neighborhood,
                    size_t spawn) const;
    void validate(const Knowledge& knowledge) const;
    size_t other(size_t edge, size_t node) const;

    std::vector<Node> graphNodes;
    std::vector<Edge> graphEdges;
    std::vector<std::vector<size_t>> adjacency;
    std::vector<uint8_t> majorNodes;
  };
