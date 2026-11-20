#pragma once

#include <Tempest/CommandBuffer>
#include <Tempest/Encoder>
#include <Tempest/IndexBuffer>
#include <Tempest/StorageBuffer>
#include <Tempest/Texture2d>
#include <Tempest/Vec>

#include <algorithm>
#include <array>
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
#include "graphics/grass/grassborder.h"
#include "graphics/grass/grassdiag.h"
#include "graphics/grass/grasspack.h"
#include "graphics/grass/grassplace.h"
#include "graphics/grass/grassplots.h"
#include "graphics/grass/grasswaste.h"
#include "worldchunkmanifest.h"

class SceneGlobals;

namespace Grass {

template<class Draw>
void orderDraws(std::vector<Draw>& draws) {
  if(orderOff())
    return;
  std::stable_sort(draws.begin(), draws.end(), [](const Draw& a, const Draw& b) {
    return a.cell->dist < b.cell->dist;
    });
  }

}

// The grass round the camera (reports/engine/2026-11-14-grass-system). The streaming of the landscape gives each
// chunk's mesh as it reads it (addChunk); the field keeps the summary of its ground under materials with `grass`,
// builds the blades of the 8 m cells of the ring on its own thread (Grass::buildCell), keeps one buffer per cell
// and draws them into the G-buffer after the landscape (Shaders::grass, shader/grass/grass.vert).
//
// A cell is built when its chunk and the eight round it are in (or are not in the world). A cell far from the
// camera keeps only the blades of the lower ranks, as many as the ring shows there (Grass::Ring); it is built anew
// when the camera comes 2 m short of needing more. The blades of a batch lie by rank, so a frame draws the start
// of it: the blades the ring keeps at the cell's distance.
//
// The meadow's own ground (reports/engine/2026-11-14-meadow-ground; with bindless textures alone): the field also
// keeps the ground with `verge` of each chunk, finds on its thread the pieces of the grass's open edges along it
// (Grass::chunkBorder) whenever a chunk comes or goes, and gives them to the ground's shader as one buffer
// (SceneGlobals::grassBorder). That it does without the blades too (OPENGOTHIC_GRASS=ground).
class GrassField final {
  public:
    // the things to pick up in a rectangle of the world, cm: the grass round them is low
    using Items = std::function<void(float x0, float z0, float x1, float z1, std::vector<Grass::Disc>& out)>;

    // worldContentDir: of the content's world, for the ground it gives to the grass (grassplots.h)
    GrassField(SceneGlobals& scene, std::vector<ChunkId> chunks, const std::filesystem::path& worldContentDir);
    ~GrassField();

    void setItems(Items fn) { items = std::move(fn); }

    // any thread: the landscape's packing
    void addChunk (ChunkId id, const asset::WorldMesh& mesh);
    void dropChunk(ChunkId id);

    // the main thread, once a frame: what the ring needs, the worker's results into buffers
    void update();
    void draw(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, bool historyOwner = false);

    struct Stats final {
      size_t cells     = 0; // with blades
      size_t stored    = 0; // blades in the buffers of the ring
      size_t drawn     = 0; // of them the last frame drew: in view, of a rank the ring keeps at the cell's distance
      size_t fine      = 0; // of those the fine blades of the short sward
      size_t triangles = 0; // of those
      uint32_t pack    = 1; // blades per instance
      size_t instances = 0; // submitted instances, including each partially filled pack
      size_t draws     = 0; // draw calls of the last frame
      size_t drawUs    = 0; // with OPENGOTHIC_GRASS_WASTE=1: the mean time of draw() on the CPU, microseconds
      size_t bytes     = 0; // of the buffers
      size_t waiting   = 0; // cells not built yet
      Grass::Waste waste;      // with OPENGOTHIC_GRASS_WASTE=1: of the drawn those submitted for nothing
      };
    Stats stats() const { return last; }
    // nothing is being built for the camera's place of the last frame, and no chunk came since; without the blades
    // (the ground's look alone) no cell is ever built. The borders are those of the chunks that are in
    bool  ready() const {
      return (!blades || (last.waiting==0 && !dirty.load())) && (!layers || borderHave.load()==borderWant.load());
      }

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

    // a ground with `verge`: the slot of the grass it is the verge of, none without the record
    struct VergeOf final {
      uint16_t slot   = uint16_t(-1);
      float    reach  = 0.f, margin = 0.f;
      };

    // the worker's: a chunk's pieces of the border and what they were found from
    struct Border final {
      uint64_t                      from = 0;
      std::vector<Grass::BorderSeg> segs;
      };

    struct Cell final {
      int32_t                   cx = 0, cz = 0;
      Tempest::StorageBuffer    gpu;
      std::vector<Grass::Batch> batches;
      std::vector<Grass::Blade> cpu;             // with OPENGOTHIC_GRASS_WASTE=1 alone (grasswaste.h)
      int32_t                   minY = 0, maxY = 0;
      float                     reach = 0;       // cm: no blade's tip is further from its root
      uint32_t                  count = 0;
      std::vector<uint8_t>      ranks;           // of each blade: within a batch they rise
      float                     dist = 0;        // cm: from the camera to the nearest of the cell, in x and z
      bool                      live = false;
      uint8_t                   level = 0;
      uint64_t                  epoch = 0;
      bool                      pending = false; // a job is out
      uint8_t                   reqLevel = 0;
      uint64_t                  reqEpoch = 0;
      };

    struct Draw final {
      const Cell* cell      = nullptr;
      uint16_t    slot      = 0;
      bool        fine      = false;
      uint8_t     links     = 0;
      uint32_t    count     = 0;
      uint32_t    table     = 0;
      uint32_t    instances = 0;
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
    VergeOf         vergeOf(const asset::Material& m); // with `sync` held
    void            buildBorder();
    void            takeBorder();
    void            run();
    void            upload(Cell& c, Done&& d);
    void            release(Cell& c);
    const Tempest::StorageBuffer* uploadIndirect(const std::vector<Draw>& draws, uint8_t fId, uint32_t pack);

    SceneGlobals&                       scene;
    Grass::Ring                         ring;
    bool                                blades = true;
    bool                                layers = false; // the grounds' own layers and their borders
    Items                               items;
    std::unordered_set<uint64_t>        world;   // the chunks the world has
    Grass::Plots                        plots;   // read once, before the worker

    mutable std::mutex                  sync;    // chunks, slots, jobs, done
    std::condition_variable             wake;
    std::unordered_map<uint64_t,Chunk>  chunks;
    std::vector<Slot>                   slots;
    std::unordered_map<std::string,uint16_t> slotByTexture;
    std::unordered_map<std::string,VergeOf>  vergeByTexture;
    uint64_t                            borderBuilt = 0; // the epoch of the chunks the worker last made the borders of
    std::vector<uint32_t>               borderGrid;      // Grass::packBorder, for the main thread to take
    bool                                borderNew = false;
    std::atomic<uint64_t>               borderWant{0};
    std::atomic<uint64_t>               borderHave{0};
    std::unordered_map<uint64_t,Border> borderOf;        // the worker's alone
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
    std::vector<const Tempest::Texture2d*> slotSward; // of a ground with its own layers the sward's map
    std::vector<Grass::Params>          slotParams;
    std::array<Tempest::IndexBuffer<uint16_t>, Grass::PackSizes.size()> packBuffers;
    Tempest::StorageBuffer              borderGpu;
    std::vector<uint32_t>               borderCpu;
    Stats                               last;
    // a frame's draws and the table of their spans (draw)
    std::vector<uint32_t>               spanCpu;
    Tempest::StorageBuffer              spanGpu[2];      // by the frame in flight (Resources::MaxFramesInFlight)
    Tempest::StorageBuffer              indirectGpu[2];
    uint32_t                            wasteFrame = 0;
    uint64_t                            drawNs     = 0;
    uint32_t                            drawFrames = 0;
  };
