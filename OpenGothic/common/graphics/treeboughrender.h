#pragma once

#include <Tempest/StorageBuffer>

#include <array>
#include <map>
#include <unordered_map>

#include "graphics/drawbuckets.h"
#include "graphics/drawcommands.h"

class TreeBoughRender {
  public:
    explicit TreeBoughRender(DrawCommands& owner);
    ~TreeBoughRender();

    void add(const StaticMesh& mesh, uint32_t cluster, uint16_t main, uint16_t impostor, uint32_t first, uint32_t count);
    void remove(uint32_t cluster);
    void setTransform(uint32_t cluster, const Tempest::Matrix4x4& transform);
    void commit(Tempest::Encoder<Tempest::CommandBuffer>& enc);
    bool ready() const { return !entries.empty() && !data.isEmpty(); }
    const Tempest::ComputePipeline& cull(Tempest::Encoder<Tempest::CommandBuffer>& enc, bool hiZ, bool probe,
                                         bool history, bool ordered, uint32_t& token);
    void expand(Tempest::Encoder<Tempest::CommandBuffer>& enc, uint32_t token, bool lateHiZ);
    void draw(Tempest::Encoder<Tempest::CommandBuffer>& enc, DrawCommands::HistoryDraw history);
    void readStats(Tempest::Encoder<Tempest::CommandBuffer>& enc, uint8_t frame, bool expanded);

  private:
    struct Asset {
      std::vector<DrawBuckets::Id> buckets;
      std::vector<uint32_t>         rangeCommands;
      std::vector<std::pair<uint32_t,uint32_t>> capacity;
      uint32_t                    refs = 0;
      uint32_t                    offset = 0;
      };

    struct Entry {
      const StaticMesh* mesh = nullptr;
      uint32_t commands = 0;
      uint32_t first = 0, count = 0;
      uint32_t stateOffset = 0, referenceOffset = 0;
      };

    struct Reference {
      Tempest::Matrix4x4 transform;
      std::vector<uint32_t> history;
      };

    struct Command {
      DrawCommands::DrawCmd draw;
      uint32_t vertexCount = 0;
      };

    uint32_t command(const Material& material, uint16_t bucket, uint32_t indices);
    void registerAsset(const StaticMesh& mesh, Asset& asset);
    void uploadData(Tempest::Encoder<Tempest::CommandBuffer>& enc);
    void uploadReference();
    void copy(Tempest::Encoder<Tempest::CommandBuffer>& enc, const Tempest::StorageBuffer& source,
              Tempest::StorageBuffer& destination, uint32_t offset, uint32_t count, uint32_t sourceCount);

    DrawCommands& owner;
    std::map<uint32_t,Entry> entries;
    std::unordered_map<uint32_t,Reference> references;
    std::map<const StaticMesh*,Asset> assets;
    std::vector<Command> commands;
    std::map<uint32_t,uint32_t> freeState;
    std::vector<std::pair<uint32_t,uint32_t>> clearState;
    uint32_t stateSize = 5;
    uint32_t referenceSize = 0;
    uint32_t epoch = 0;
    bool dirty = true;

    Tempest::StorageBuffer data, state, referenceData, indirect, payload;
    std::array<Tempest::StorageBuffer,Resources::MaxFramesInFlight> stats;
    std::array<bool,Resources::MaxFramesInFlight> readPending = {};
    Tempest::ComputePipeline cullPipelines[4][2];
    Tempest::ComputePipeline expandPipeline, copyPipeline;
    const bool cpu;
  };
