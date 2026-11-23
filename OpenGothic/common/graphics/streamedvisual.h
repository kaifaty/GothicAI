#pragma once

#include "worldchunksruntime.h"

#include <cstddef>

class WorldView;

class StreamedVisual {
  public:
    virtual ~StreamedVisual() = default;
    virtual void createView() = 0;
    virtual void releaseView() = 0;
    virtual bool isViewPublished() const = 0;
    virtual bool isViewMissing() const = 0;
    virtual size_t viewNodesCount() const = 0;
    virtual size_t probePhysicsBytes() const = 0;

  protected:
    WorldView* streamView = nullptr;
    ChunkId streamCell;

  friend class WorldView;
  };
