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
    void commit();
    bool ready() const { return pubEntryCount!=0 && !data.isEmpty(); }
    const Tempest::ComputePipeline& cull(Tempest::Encoder<Tempest::CommandBuffer>& enc, bool hiZ, bool probe,
                                         bool history, bool ordered, uint32_t& token);
    const Tempest::ComputePipeline& historyClassify();
    void expand(Tempest::Encoder<Tempest::CommandBuffer>& enc, uint32_t token, bool lateHiZ);
    void draw(Tempest::Encoder<Tempest::CommandBuffer>& enc, DrawCommands::HistoryDraw history);
    void readStats(Tempest::Encoder<Tempest::CommandBuffer>& enc, uint8_t frame, bool expanded);
    bool diagnose(Tempest::Encoder<Tempest::CommandBuffer>& enc, uint8_t frame, const Tempest::Texture2d& depth,
                  const Tempest::Matrix4x4& viewProject, bool verify, uint64_t stamp);

  private:
    static constexpr uint32_t StatsCount = 8;
    static constexpr uint32_t DiagnosticCount = 7;

    struct DiagnosticRead {
      Tempest::StorageBuffer counts, before, after;
      uint64_t stamp = 0;
      uint32_t commandWords = 0;
      bool stockCounts = false;
      };

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
      uint32_t referenceOffset = 0;
      };

    struct Command {
      DrawCommands::DrawCmd draw;
      uint32_t unit = 0; // the vertex path: indices of a node's class; the mesh path: nodes of a workgroup
      };

    uint32_t command(const Material& material, uint16_t bucket, uint32_t unit);
    void registerAsset(const StaticMesh& mesh, Asset& asset);
    void uploadData();
    void uploadReference();
    void copy(Tempest::Encoder<Tempest::CommandBuffer>& enc, const Tempest::StorageBuffer& source,
              Tempest::StorageBuffer& destination, uint32_t offset, uint32_t count, uint32_t sourceCount);
    uint32_t copyCommands(Tempest::Encoder<Tempest::CommandBuffer>& enc, Tempest::StorageBuffer& destination);
    void readDiagnostic(DiagnosticRead& slot);

    DrawCommands& owner;
    std::map<uint32_t,Entry> entries;
    std::unordered_map<uint32_t,Tempest::Matrix4x4> references;
    std::map<const StaticMesh*,Asset> assets;
    std::vector<Command> commands;
    // Published snapshot of the frame being recorded: commit() copies the commands and the entry count after upload,
    // so draw/cull/expand/diagnose read neither the live commands vector nor the entries map that tick/streaming
    // mutate through addBough/removeBough. The counts match the entry table and indirect draws commit() uploaded.
    std::vector<Command> pubCommands;
    size_t               pubEntryCount = 0;
    uint32_t referenceSize = 0;
    uint32_t epoch = 0;
    bool dirty = true;

    Tempest::StorageBuffer data, state, referenceData, indirect, payload;
    std::array<Tempest::StorageBuffer,Resources::MaxFramesInFlight> stats;
    std::array<bool,Resources::MaxFramesInFlight> readPending = {};
    Tempest::ComputePipeline cullPipelines[4][2];
    Tempest::ComputePipeline expandPipeline, copyPipeline, historyPipeline;
    Tempest::ComputePipeline diagnosticPipeline;
    Tempest::StorageBuffer diagnosticCounts;
    std::array<DiagnosticRead,Resources::MaxFramesInFlight> diagnosticRead;
    uint32_t expandedToken = 0, expandedFlags = 0;
    const bool cpu;
  };
