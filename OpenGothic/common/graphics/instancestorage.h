#pragma once

#include <Tempest/Device>
#include <Tempest/Matrix4x4>
#include <Tempest/UniformBuffer>

#include <condition_variable>
#include <vector>
#include <thread>

#include "resources.h"

class InstanceStorage {
  private:
    struct Range {
      size_t begin = 0;
      size_t size  = 0;
      size_t asize = 0;
      };

    static constexpr size_t blockSz   = 64;
    static constexpr size_t alignment = 64;

  public:
    class Id {
      public:
        Id() = default;
        Id(InstanceStorage& owner, Range rgn):owner(&owner), rgn(rgn){}
        Id(Id&& other) noexcept;
        Id& operator = (Id&& other) noexcept;
        ~Id();

        const size_t   size() const { return rgn.asize; }
        void           set(const Tempest::Matrix4x4* anim);
        void           set(const Tempest::Matrix4x4& obj, size_t offset);
        void           set(const void* data, size_t offset, size_t size);

        template<class T>
        const uint32_t offsetId() const { return uint32_t(rgn.begin/sizeof(T)); }

        bool           isEmpty() const { return rgn.asize==0; }

      private:
        InstanceStorage* owner = nullptr;
        Range            rgn;
      friend class InstanceStorage;
      friend struct InstanceStorageContractAccess;
      };

    InstanceStorage();
    ~InstanceStorage();

    Id   alloc(const size_t size);
    bool realloc(Id& id, const size_t size);
    auto ssbo () const -> const Tempest::StorageBuffer&;
    bool commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    void join();

    // A GPU copy of the buffer as it was after the previous commit (motion vectors).
    // Off by default; main thread, between frames. Turning it on makes the next
    // commit copy the whole buffer; turning it off releases the copy.
    void setHistory(bool enabled);
    bool hasHistory() const { return history; }
    // The previous frame's buffer when history is on and filled, otherwise ssbo():
    // a reader of "prev" then sees prev==cur, i.e. no motion.
    auto ssboPrev() const -> const Tempest::StorageBuffer&;

  private:
    void free(const Range& r);
    void uploadMain();

    struct Path {
      uint32_t dst;
      uint32_t src;
      uint32_t size;
      };

    // cur -> prev at the same offset; bytes while planning, uint's in patchCpu
    struct Copy {
      uint32_t at;
      uint32_t size;
      };

    // What one commit does, decided on the CPU without a device. patchCpu holds
    // the frame's patch buffer: [Path headers][payload][save Copy's][fresh Copy's].
    struct Plan {
      bool     grow       = false; // recreate the GPU buffer(s) from dataCpu, nothing else
      bool     upload     = false; // patchCpu must reach patchGpu[fId]
      uint32_t patchCount = 0;     // Path headers at uint 0
      uint32_t saveBase   = 0;     // uint offset of the save list: cur->prev before the patch
      uint32_t saveCount  = 0;
      uint32_t freshBase  = 0;     // uint offset of the fresh list: cur->prev after the patch
      uint32_t freshCount = 0;
      };

    size_t gpuBytes() const;
    Plan   planCommit(size_t gpuCapacity);
    void   markFresh(const Range& r);
    void   pushCopies(std::vector<Copy>& out, size_t firstBlock, size_t endBlock);

    std::vector<Range>      rgn;
    std::vector<uint32_t>   durty;
    size_t                  blockCnt = 0;

    Tempest::StorageBuffer  patchGpu[Resources::MaxFramesInFlight];
    std::vector<uint8_t>    patchCpu;
    std::vector<Path>       patchBlock;

    Tempest::StorageBuffer  dataGpu;
    std::vector<uint8_t>    dataCpu;

    // history: kept only while it is on
    bool                    history      = false;
    bool                    historyValid = false; // prev holds a full copy, kept by the 3-step rule
    std::vector<uint32_t>   durtyPrev;            // blocks dirty in the previous commit
    std::vector<uint32_t>   fresh;                // blocks allocated since the previous commit
    std::vector<Copy>       saveBlock;
    std::vector<Copy>       freshBlock;
    Tempest::StorageBuffer  dataGpuPrev;

    std::thread             uploadTh;
    std::mutex              sync;
    std::condition_variable uploadCnd;
    int32_t                 uploadFId = -1;

    friend struct InstanceStorageContractAccess;
  };
