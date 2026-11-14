#pragma once

#include <Tempest/CommandBuffer>
#include <Tempest/Encoder>
#include <Tempest/IndexBuffer>
#include <Tempest/StorageBuffer>
#include <Tempest/Texture2d>
#include <Tempest/Vec>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "asset/material.h"
#include "asset/worldmesh.h"
#include "graphics/grass/grassplace.h"
#include "worldchunkmanifest.h"

class SceneGlobals;

// The grass round the camera (reports/engine/2026-11-14-grass-system). The streaming of the landscape gives each
// chunk's mesh as it reads it (addChunk); the field keeps the summary of its ground under materials with `grass`,
// builds the blades of the 8 m cells of the ring on its own thread (Grass::buildCell), keeps one buffer per cell
// and draws them into the G-buffer after the landscape (Shaders::grass, shader/grass/grass.vert).
//
// A cell is built when its chunk and the eight round it are in (or are not in the world). A cell far from the
// camera keeps only the blades of the lower ranks, as many as the ring shows there (Grass::Ring); it is built anew
// when the camera comes 2 m short of needing more. The blades of a batch lie by rank, so a frame draws the start
// of it: the blades the ring keeps at the cell's distance.
class GrassField final {
  public:
    // the things to pick up in a rectangle of the world, cm: the grass round them is low
    using Items = std::function<void(float x0, float z0, float x1, float z1, std::vector<Grass::Disc>& out)>;

    GrassField(SceneGlobals& scene, std::vector<ChunkId> chunks);
    ~GrassField();

    void setItems(Items fn) { items = std::move(fn); }

    // any thread: the landscape's packing
    void addChunk (ChunkId id, const asset::WorldMesh& mesh);
    void dropChunk(ChunkId id);

    // the main thread, once a frame: what the ring needs, the worker's results into buffers
    void update();
    void draw(Tempest::Encoder<Tempest::CommandBuffer>& cmd);

    struct Stats final {
      size_t cells     = 0; // with blades
      size_t stored    = 0; // blades in the buffers of the ring
      size_t drawn     = 0; // of them the last frame drew: in view, of a rank the ring keeps at the cell's distance
      size_t triangles = 0; // of those
      size_t bytes     = 0; // of the buffers
      size_t waiting   = 0; // cells not built yet
      };
    Stats stats() const { return last; }
    // nothing is being built for the camera's place of the last frame, and no chunk came since; without the blades
    // (the ground's look alone) nothing is ever built
    bool  ready() const { return !blades || (last.waiting==0 && !dirty.load()); }

  private:
    struct Slot final {
      asset::Material           material;
      Grass::Params             params;
      const Tempest::Texture2d* tex = nullptr; // the ground's own texture; loaded by the main thread
      };

    struct Chunk final {
      std::shared_ptr<const Grass::Summary> summary;
      uint64_t                              epoch = 0;
      };

    struct Cell final {
      int32_t                   cx = 0, cz = 0;
      Tempest::StorageBuffer    gpu;
      std::vector<Grass::Batch> batches;
      int32_t                   minY = 0, maxY = 0;
      uint32_t                  count = 0;
      std::vector<uint32_t>     below;           // by batch: 257 counts of its blades of a rank under i
      float                     dist = 0;        // cm: from the camera to the nearest of the cell, in x and z
      bool                      live = false;
      uint8_t                   level = 0;
      uint64_t                  epoch = 0;
      bool                      pending = false; // a job is out
      uint8_t                   reqLevel = 0;
      uint64_t                  reqEpoch = 0;
      };

    struct Job final {
      int32_t                                            cx = 0, cz = 0;
      uint8_t                                            level = 0;
      uint64_t                                           epoch = 0;
      float                                              dist  = 0;
      std::vector<std::shared_ptr<const Grass::Summary>> summaries;
      std::vector<Grass::Params>                         params;
      std::vector<Grass::Disc>                           items;
      };

    struct Done final {
      int32_t            cx = 0, cz = 0;
      uint8_t            level = 0;
      uint64_t           epoch = 0;
      Grass::CellBlades  blades;
      };

    static uint64_t key(int32_t x, int32_t z) { return (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(z)); }
    uint32_t        rankLimit(uint8_t level) const;
    uint8_t         levelAt(float dist) const;
    uint16_t        slotOf(const asset::Material& m); // with `sync` held
    void            run();
    void            upload(Cell& c, Done&& d);
    void            release(Cell& c);

    SceneGlobals&                       scene;
    Grass::Ring                         ring;
    bool                                blades = true;
    Items                               items;
    std::unordered_set<uint64_t>        world;   // the chunks the world has

    mutable std::mutex                  sync;    // chunks, slots, jobs, done
    std::condition_variable             wake;
    std::unordered_map<uint64_t,Chunk>  chunks;
    std::vector<Slot>                   slots;
    std::unordered_map<std::string,uint16_t> slotByTexture;
    uint64_t                            epoch = 0;
    std::atomic_bool                    dirty{true};
    std::deque<Job>                     jobs;
    std::vector<Done>                   done;
    bool                                stopping = false;
    std::thread                         worker;

    // the main thread's
    std::unordered_map<uint64_t,Cell>   cells;
    std::deque<Done>                    toUpload;
    std::vector<const Tempest::Texture2d*> slotTex;
    std::vector<Grass::Params>          slotParams;
    Tempest::IndexBuffer<uint16_t>      ibo;
    Stats                               last;
  };
