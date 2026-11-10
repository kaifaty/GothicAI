#include "graphics/instancestorage.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// Inspect only CPU state. No device is constructed: any production GPU access
// fails through Resources::device instead of being hidden by a graphics stub.
struct InstanceStorageContractAccess {
  static const std::vector<uint8_t>& cpu(const InstanceStorage& storage) { return storage.dataCpu; }
  static const std::vector<uint32_t>& dirty(const InstanceStorage& storage) { return storage.durty; }
  static void uploaded(InstanceStorage& storage) { std::fill(storage.durty.begin(),storage.durty.end(),0); }

  // History: the production planner and what it put into this frame's patch buffer.
  using Plan = InstanceStorage::Plan;
  static Plan plan(InstanceStorage& storage, size_t gpuCapacity) { return storage.planCommit(gpuCapacity); }
  static size_t gpuBytes(const InstanceStorage& storage) { return storage.gpuBytes(); }
  static const std::vector<uint8_t>& patch(const InstanceStorage& storage) { return storage.patchCpu; }
  static bool keepsHistoryState(const InstanceStorage& storage) {
    return storage.durtyPrev.size()>0 || storage.fresh.size()>0 || storage.saveBlock.size()>0 ||
           storage.freshBlock.size()>0 || storage.historyValid || !storage.dataGpuPrev.isEmpty();
    }
  static size_t reserved(const InstanceStorage::Id& id) { return id.rgn.size; }
  };

namespace {
  using Matrix = Tempest::Matrix4x4;
  constexpr size_t blockBytes = 64;

  void require(bool condition, const char* message) {
    if(condition) return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  size_t begin(const InstanceStorage::Id& id) { return size_t(id.offsetId<Matrix>())*sizeof(Matrix); }

  // An independent page image makes uninitialized GPU tail bytes observable.
  // The production CPU dirty state decides which blocks the image receives.
  struct MockPage {
    std::vector<uint8_t> bytes;

    void commit(InstanceStorage& storage) {
      const auto& cpu=InstanceStorageContractAccess::cpu(storage);
      const auto& dirty=InstanceStorageContractAccess::dirty(storage);
      const size_t capacity=((cpu.size()+4095)/4096)*4096;
      if(bytes.size()!=capacity) {
        bytes.assign(capacity,0xcd);
        std::copy(cpu.begin(),cpu.end(),bytes.begin());
        } else {
        // The constructor identity is uploaded by the full-copy path, but no
        // dirty metadata exists until the first nonempty allocation.
        for(size_t block=0;block<cpu.size()/blockBytes;++block)
          if(block/32<dirty.size() && (dirty[block/32] & (uint32_t(1)<<(block%32)))!=0)
            std::copy_n(cpu.data()+block*blockBytes,blockBytes,bytes.data()+block*blockBytes);
        }
      InstanceStorageContractAccess::uploaded(storage);
      }

    bool equal(size_t at, const void* source, size_t size) const {
      return at<=bytes.size() && size<=bytes.size()-at && std::memcmp(bytes.data()+at,source,size)==0;
      }
    };

  void expectDirty(const InstanceStorage& storage, const std::vector<size_t>& expected) {
    const auto& dirty=InstanceStorageContractAccess::dirty(storage);
    for(const auto block:expected)
      require(block/32<dirty.size(),"every expected dirty block must have metadata");
    for(size_t block=0;block<dirty.size()*32;++block) {
      const bool marked=(dirty[block/32] & (uint32_t(1)<<(block%32)))!=0;
      require(marked==(std::find(expected.begin(),expected.end(),block)!=expected.end()),
              "dirty blocks must match exactly, including old pending bits");
      }
    }

  void testZeroPaddingReuse() {
    InstanceStorage storage;
    MockPage gpu;
    gpu.commit(storage);
    const auto initial=gpu.bytes;
    auto empty=storage.alloc(0);
    empty.set(static_cast<const Matrix*>(nullptr));
    expectDirty(storage,{});
    gpu.commit(storage);
    require(gpu.bytes==initial,"empty palette must not read a null pointer or change storage");

    // 65 used bytes reserve 128 bytes, all within an existing GPU page.
    auto id=storage.alloc(65);
    require(begin(id)==64,"first allocation must follow the initial identity matrix");
    std::vector<Matrix> zero(2);
    id.set(zero.data());
    expectDirty(storage,{1,2});
    gpu.commit(storage);
    require(gpu.equal(begin(id),zero.data(),128),"cold zero palette and padding must replace unknown GPU tail");
    require(gpu.bytes[192]==0xcd,"initialization must not overwrite unrelated GPU capacity");

    std::vector<Matrix> partial(2,Matrix::mkIdentity());
    id.set(partial.data());
    expectDirty(storage,{1,2});
    gpu.commit(storage);
    require(gpu.equal(begin(id),partial.data(),65),"changed partial-block palette must upload its exact used bytes");
    id.set(zero.data());
    gpu.commit(storage);

    require(!storage.realloc(id,128),"growth within reserved padding must retain the region");
    id.set(zero.data());
    expectDirty(storage,{});
    gpu.commit(storage);
    require(gpu.equal(begin(id),zero.data(),128),"in-place growth must expose initialized padding");

    std::vector<Matrix> pose(2,Matrix::mkIdentity());
    pose[1].set(3,0,42.f);
    id.set(pose.data());
    expectDirty(storage,{1,2});
    id.set(pose.data());
    expectDirty(storage,{1,2});
    gpu.commit(storage);
    require(gpu.equal(begin(id),pose.data(),128),"identical second write must retain an earlier pending upload");

    const auto oldBegin=begin(id);
    id=InstanceStorage::Id();
    auto reused=storage.alloc(128);
    require(begin(reused)==oldBegin,"fixture must reuse the released range");
    reused.set(pose.data());
    expectDirty(storage,{});
    gpu.commit(storage);
    require(gpu.equal(begin(reused),pose.data(),128),"identical reused range must preserve the existing GPU palette");
    reused.set(zero.data());
    expectDirty(storage,{1,2});
    gpu.commit(storage);
    require(gpu.equal(begin(reused),zero.data(),128),"changed reused range must still upload exact bytes");
    }

  void testMovedGrowthAndPageResize() {
    InstanceStorage storage;
    MockPage gpu;
    gpu.commit(storage);
    auto id=storage.alloc(64);
    Matrix pose=Matrix::mkIdentity();
    pose.set(3,1,-17.f);
    id.set(&pose);
    gpu.commit(storage);
    auto blocker=storage.alloc(64);
    Matrix other=Matrix::mkIdentity();
    other.set(3,2,91.f);
    blocker.set(&other);
    gpu.commit(storage);

    const auto oldBegin=begin(id);
    require(storage.realloc(id,129),"growth beyond reserved capacity must move the region");
    require(begin(id)!=oldBegin && begin(id)==192,"moved-growth fixture must occupy the fresh range");
    std::vector<Matrix> expanded(4);
    expanded[0]=pose;
    // Identical used bytes must not discard initialization of all 256 reserved bytes.
    id.set(expanded.data());
    expectDirty(storage,{3,4,5,6});
    gpu.commit(storage);
    require(gpu.equal(begin(id),expanded.data(),256),"moved growth must copy old data and initialize new padding");
    require(gpu.equal(begin(blocker),&other,64),"moved growth must preserve another live region");

    require(!storage.realloc(id,256),"later in-capacity growth must not relocate");
    id.set(expanded.data());
    expectDirty(storage,{});
    gpu.commit(storage);
    require(gpu.equal(begin(id),expanded.data(),256),"exposed moved padding must already have exact GPU bytes");

    auto large=storage.alloc(8192);
    std::vector<Matrix> zeros(128);
    large.set(zeros.data());
    const auto oldCapacity=gpu.bytes.size();
    gpu.commit(storage);
    require(gpu.bytes.size()>oldCapacity,"fixture must force a new GPU page capacity");
    require(gpu.equal(begin(large),zeros.data(),8192),"GPU resize must initialize a byte-identical zero range");
    require(gpu.equal(begin(id),expanded.data(),256) && gpu.equal(begin(blocker),&other,64),
            "GPU resize must preserve every existing palette");
    expectDirty(storage,{});
    }

  void testMovedReusedRange() {
    InstanceStorage storage;
    MockPage gpu;
    auto id=storage.alloc(64);
    auto spare=storage.alloc(256);
    Matrix old=Matrix::mkIdentity();
    old.set(3,0,777.f);
    std::vector<Matrix> expected(4,Matrix::mkIdentity());
    for(size_t i=0;i<expected.size();++i) expected[i].set(3,0,float(10+i));
    id.set(&old);
    spare.set(expected.data());
    gpu.commit(storage);
    const auto destination=begin(spare);
    spare=InstanceStorage::Id();
    require(storage.realloc(id,129) && begin(id)==destination,"moved growth must reuse the initialized spare range");
    expected[0]=old;
    id.set(expected.data());
    expectDirty(storage,{destination/blockBytes});
    gpu.commit(storage);
    require(gpu.equal(destination,expected.data(),256),"copied prefix must upload even when the moved palette is otherwise identical");
    require(!storage.realloc(id,256),"reused reserved padding must support in-place growth");
    id.set(expected.data());
    expectDirty(storage,{});
    gpu.commit(storage);
    require(gpu.equal(destination,expected.data(),256),"reused padding must retain its initialized GPU bytes");
    }

  void testWordBoundaries() {
    for(const size_t prefix:{size_t(30),size_t(31)}) {
      InstanceStorage storage;
      MockPage gpu;
      std::vector<InstanceStorage::Id> earlier;
      for(size_t i=0;i<prefix;++i) earlier.push_back(storage.alloc(64));
      const size_t count=prefix==30 ? 37 : 32;
      auto id=storage.alloc(count*sizeof(Matrix));
      require(begin(id)/blockBytes==prefix+1,"boundary fixture must start at its declared block");
      gpu.commit(storage);
      const auto pending=Matrix::mkIdentity();
      earlier[0].set(&pending);
      std::vector<Matrix> pose(count,Matrix::mkIdentity());
      id.set(pose.data());
      std::vector<size_t> expected{1};
      for(size_t i=0;i<count;++i) expected.push_back(prefix+1+i);
      expectDirty(storage,expected);
      gpu.commit(storage);
      require(gpu.equal(begin(id),pose.data(),count*sizeof(Matrix)),"cross-word/full-word marking must upload the entire palette");
      }
    }

  void testConcurrentSharedWords() {
    InstanceStorage storage;
    MockPage gpu;
    constexpr size_t actors=96,workers=16;
    std::vector<InstanceStorage::Id> ids;
    std::vector<Matrix> pose(actors,Matrix::mkIdentity());
    for(size_t i=0;i<actors;++i) ids.push_back(storage.alloc(64));
    auto pending=storage.alloc(64);
    gpu.commit(storage);
    std::vector<size_t> expected;
    for(size_t i=1;i<=actors+1;++i) expected.push_back(i);

    for(size_t round=0;round<16;++round) {
      Matrix marker=Matrix::mkIdentity();
      marker.set(3,0,float(round+1));
      pending.set(&marker);
      for(size_t i=0;i<actors;++i) pose[i].set(3,0,float(1+round*actors+i));
      std::atomic<size_t> ready{0};
      std::atomic<bool> start{false};
      std::vector<std::thread> threads;
      for(size_t worker=0;worker<workers;++worker)
        threads.emplace_back([&,worker] {
          ready.fetch_add(1);
          while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
          for(size_t i=worker;i<actors;i+=workers) ids[i].set(&pose[i]);
          });
      while(ready.load()!=workers) std::this_thread::yield();
      start.store(true,std::memory_order_release);
      for(auto& thread:threads) thread.join();
      expectDirty(storage,expected);
      gpu.commit(storage);
      for(size_t i=0;i<actors;++i)
        require(gpu.equal(begin(ids[i]),&pose[i],64),"shared-word atomic OR must retain every disjoint worker palette");
      require(gpu.equal(begin(pending),&marker,64),"concurrent marking must retain unrelated pending work");
      for(size_t i=0;i<actors;++i) ids[i].set(&pose[i]);
      expectDirty(storage,{});
      }
    }

  // ---- previous-frame copy (history) ------------------------------------------------
  // Two page images, cur and prev, driven only by the production plan: the save list,
  // the patch and the fresh list are read back from this frame's patch buffer the way
  // patch_copy.comp and patch.comp read them.
  struct HistoryPages {
    std::vector<uint8_t> cur, prev, before;
    InstanceStorageContractAccess::Plan last;

    uint32_t word(const std::vector<uint8_t>& patch, size_t i) const {
      require((i+1)*4<=patch.size(),"every list and header must lie inside this frame's patch buffer");
      uint32_t v=0;
      std::memcpy(&v,patch.data()+i*4,4);
      return v;
      }

    void copies(const std::vector<uint8_t>& patch, uint32_t base, uint32_t count) {
      for(uint32_t wg=0;wg<count;++wg) {
        const size_t at=word(patch,base+wg*2)*size_t(4), size=word(patch,base+wg*2+1)*size_t(4);
        require(at%blockBytes==0 && size>0 && size%blockBytes==0,"copies must be whole blocks");
        require(at+size<=cur.size() && at+size<=prev.size(),"copies must stay inside the buffers");
        std::copy_n(cur.data()+at,size,prev.data()+at);
        }
      }

    void commit(InstanceStorage& storage) {
      before=cur;
      const auto& cpu=InstanceStorageContractAccess::cpu(storage);
      last=InstanceStorageContractAccess::plan(storage,cur.size());
      if(last.grow) {
        const size_t capacity=InstanceStorageContractAccess::gpuBytes(storage);
        require(capacity!=cur.size(),"growth is planned only for a new capacity");
        cur.assign(capacity,0xcd);
        std::copy(cpu.begin(),cpu.end(),cur.begin());
        prev.clear();
        if(storage.hasHistory()) {
          prev.assign(capacity,0xab);
          std::copy(cpu.begin(),cpu.end(),prev.begin());
          }
        return;
        }
      if(!storage.hasHistory())
        prev.clear(); // setHistory(false) recycles it
      require(last.patchCount>0 || last.saveCount>0 || last.freshCount>0 || !last.upload,
              "an upload must carry work");
      if(!last.upload)
        return;
      const auto& patch=InstanceStorageContractAccess::patch(storage);
      require(patch.size()%4==0,"patch buffer is read as uint's");
      if(!storage.hasHistory()) {
        require(last.saveCount==0 && last.freshCount==0,"history off: no copies");
        size_t payload=0;
        for(uint32_t wg=0;wg<last.patchCount;++wg) payload+=word(patch,wg*3+2)*size_t(4);
        require(patch.size()==last.patchCount*12+payload,"history off: the patch buffer is headers and payload only");
        }
      if(storage.hasHistory() && prev.size()!=cur.size())
        prev.assign(cur.size(),0xab); // first commit with history: uninitialized device memory
      copies(patch,last.saveBase,last.saveCount);
      for(uint32_t wg=0;wg<last.patchCount;++wg) {
        const size_t dst=word(patch,wg*3)*size_t(4), src=word(patch,wg*3+1)*size_t(4), size=word(patch,wg*3+2)*size_t(4);
        require(dst+size<=cur.size() && src+size<=patch.size(),"patch must stay inside its buffers");
        std::copy_n(patch.data()+src,size,cur.data()+dst);
        }
      copies(patch,last.freshBase,last.freshCount);
      }
    };

  // Fresh ranges of this frame, recorded by the scenario when it allocates.
  struct FreshSet {
    std::vector<std::pair<size_t,size_t>> ranges;
    void add(const InstanceStorage::Id& id) {
      if(!id.isEmpty()) ranges.emplace_back(begin(id),InstanceStorageContractAccess::reserved(id));
      }
    bool has(size_t block) const {
      for(auto& r:ranges)
        if(block*blockBytes>=r.first && block*blockBytes<r.first+r.second) return true;
      return false;
      }
    };

  // After every commit: cur equals the CPU copy; prev equals what cur held after the
  // previous commit, except fresh ranges (and frames without history before) where prev==cur.
  void expectHistory(const InstanceStorage& storage, const HistoryPages& gpu, const FreshSet& freshNow,
                     bool noMotion, const char* what) {
    const auto& cpu=InstanceStorageContractAccess::cpu(storage);
    require(gpu.cur.size()>=cpu.size(),what);
    require(std::equal(cpu.begin(),cpu.end(),gpu.cur.begin()),"cur must equal the CPU copy after every commit");
    require(storage.hasHistory(),what);
    require(gpu.prev.size()==gpu.cur.size(),"prev must exist with history on");
    const size_t blocks=(cpu.size()+blockBytes-1)/blockBytes;
    for(size_t b=0;b<blocks;++b) {
      const size_t at=b*blockBytes, size=std::min(blockBytes,cpu.size()-at);
      const bool same=noMotion || freshNow.has(b);
      const auto& expected=same ? gpu.cur : gpu.before;
      if(!same && at+size>expected.size()) {
        std::fprintf(stderr,"block %zu beyond the previous capacity (%s)\n",b,what);
        require(false,"a stable block must have existed in the previous frame");
        }
      if(std::memcmp(gpu.prev.data()+at,expected.data()+at,size)!=0) {
        std::fprintf(stderr,"block %zu: prev != %s (%s)\n",b,same ? "cur" : "cur of the previous commit",what);
        require(false,"prev must follow the three-step rule");
        }
      }
    }

  Matrix marker(float v) {
    Matrix m=Matrix::mkIdentity();
    m.set(3,0,v);
    return m;
    }

  void commitAndCheck(InstanceStorage& storage, HistoryPages& gpu, FreshSet& freshNow, const char* what) {
    gpu.commit(storage);
    expectHistory(storage,gpu,freshNow,gpu.last.grow,what);
    freshNow.ranges.clear();
    }

  void testHistoryStillAfterChange() {
    // (a) changed in N-1, still in N; (f) nothing dirty right after a dirty frame; (b) every frame
    InstanceStorage storage;
    storage.setHistory(true);
    HistoryPages gpu;
    FreshSet fresh;
    auto a=storage.alloc(64);
    auto b=storage.alloc(64);
    fresh.add(a); fresh.add(b);
    auto m=marker(1);
    a.set(&m);
    commitAndCheck(storage,gpu,fresh,"first commit");
    require(gpu.last.grow,"fixture: the first commit creates the buffers");
    commitAndCheck(storage,gpu,fresh,"idle frame");
    require(!gpu.last.upload,"an idle frame after an idle frame uploads nothing");

    m=marker(2);
    a.set(&m);
    commitAndCheck(storage,gpu,fresh,"(a) frame N-1: changed");
    require(gpu.last.saveCount>0 && gpu.last.patchCount>0,"a changed block is saved and patched");
    commitAndCheck(storage,gpu,fresh,"(a,f) frame N: still");
    require(gpu.last.upload && gpu.last.patchCount==0 && gpu.last.saveCount>0,
            "(f) a frame with nothing dirty must still save the blocks dirty in the previous one");
    require(std::memcmp(gpu.prev.data()+begin(a),&m,sizeof(m))==0,"(a) prev catches up with the still block");
    commitAndCheck(storage,gpu,fresh,"(a) frame N+1");
    require(!gpu.last.upload,"two idle frames: nothing to save");

    for(int frame=0;frame<6;++frame) {
      const auto before=marker(float(10+frame));
      const auto now=marker(float(11+frame));
      m=now;
      a.set(&m);
      auto other=marker(float(100+frame));
      if(frame%2==0) b.set(&other);
      commitAndCheck(storage,gpu,fresh,"(b) changed every frame");
      if(frame>0)
        require(std::memcmp(gpu.prev.data()+begin(a),&before,sizeof(before))==0,"(b) prev is the previous pose");
      require(std::memcmp(gpu.cur.data()+begin(a),&now,sizeof(now))==0,"(b) cur is this pose");
      }
    }

  void testHistoryReuseAndMove() {
    // (c) freed and reused by another Id in the same frame; (d) realloc that moves
    InstanceStorage storage;
    storage.setHistory(true);
    HistoryPages gpu;
    FreshSet fresh;
    auto a=storage.alloc(128);
    auto blocker=storage.alloc(64);
    auto spare=storage.alloc(256);
    auto mover=storage.alloc(64);
    auto tail=storage.alloc(64);
    std::vector<Matrix> pa(2,marker(1)), ps(4,marker(3));
    a.set(pa.data());
    spare.set(ps.data());
    auto mm=marker(5);
    mover.set(&mm);
    commitAndCheck(storage,gpu,fresh,"setup");
    commitAndCheck(storage,gpu,fresh,"setup idle");

    // (c) stable for a frame, then freed and its range reused with other data
    const auto oldBegin=begin(a);
    a=InstanceStorage::Id();
    auto reused=storage.alloc(128);
    fresh.add(reused);
    require(begin(reused)==oldBegin,"fixture: the freed range is reused");
    std::vector<Matrix> pr(2,marker(42));
    reused.set(pr.data());
    commitAndCheck(storage,gpu,fresh,"(c) reuse with new data");
    require(std::memcmp(gpu.prev.data()+oldBegin,pr.data(),128)==0,"(c) a new Id's past is its present, not the old owner's");
    commitAndCheck(storage,gpu,fresh,"(c) next frame");

    // (c') reused without writing: the range keeps the old owner's bytes, prev==cur anyway
    pr.assign(2,marker(43));
    reused.set(pr.data());
    commitAndCheck(storage,gpu,fresh,"(c') change before free");
    reused=InstanceStorage::Id();
    auto silent=storage.alloc(100);
    fresh.add(silent);
    require(begin(silent)==oldBegin,"fixture: the freed range is reused again");
    commitAndCheck(storage,gpu,fresh,"(c') reuse, nothing written");

    // (d) realloc that moves into a fresh tail range and into a reused spare range
    require(storage.realloc(mover,129),"fixture: growth beyond the reserve moves");
    fresh.add(mover);
    std::vector<Matrix> pm(4,marker(7));
    pm[0]=mm;
    mover.set(pm.data());
    commitAndCheck(storage,gpu,fresh,"(d) moved to the tail");
    require(std::memcmp(gpu.prev.data()+begin(mover),pm.data(),mover.size())==0,"(d) a moved range has no motion");
    pm[1]=marker(8);
    mover.set(pm.data());
    commitAndCheck(storage,gpu,fresh,"(d) moved then changed");

    const auto spareBegin=begin(spare);
    spare=InstanceStorage::Id();
    require(storage.realloc(blocker,129) && begin(blocker)==spareBegin,"fixture: moved growth reuses the spare range");
    fresh.add(blocker);
    commitAndCheck(storage,gpu,fresh,"(d) moved into a reused range, prefix only");

    // allocated and freed in the same frame: copied fresh, nothing else may move
    {
      auto brief=storage.alloc(64);
      fresh.add(brief);
      auto bm=marker(9);
      brief.set(&bm);
    }
    commitAndCheck(storage,gpu,fresh,"alloc and free in one frame");
    }

  void testHistoryGrowth() {
    // (e) growth recreates both from the CPU copy, then the rule resumes
    InstanceStorage storage;
    storage.setHistory(true);
    HistoryPages gpu;
    FreshSet fresh;
    auto a=storage.alloc(64);
    fresh.add(a);
    auto m=marker(1);
    a.set(&m);
    commitAndCheck(storage,gpu,fresh,"setup");
    m=marker(2);
    a.set(&m);
    auto large=storage.alloc(8192);
    fresh.add(large);
    std::vector<Matrix> zeros(128);
    large.set(zeros.data());
    const auto capacity=gpu.cur.size();
    commitAndCheck(storage,gpu,fresh,"(e) growth");
    require(gpu.last.grow && gpu.cur.size()>capacity,"fixture: a new capacity");
    m=marker(3);
    a.set(&m);
    commitAndCheck(storage,gpu,fresh,"(e) after growth");
    const auto grown=marker(2);
    require(std::memcmp(gpu.prev.data()+begin(a),&grown,sizeof(grown))==0,"(e) prev is the state of the growth frame");
    commitAndCheck(storage,gpu,fresh,"(e) still");
    }

  void testHistoryOnMidRun() {
    // (g) switched on mid-run: the first commit makes prev a full copy of cur
    InstanceStorage storage;
    HistoryPages gpu;
    FreshSet fresh;
    std::vector<InstanceStorage::Id> ids;
    for(int i=0;i<40;++i) ids.push_back(storage.alloc(64*size_t(1+i%3)));
    for(int frame=0;frame<3;++frame) {
      for(size_t i=0;i<ids.size();i+=3) {
        auto m=marker(float(frame*100+int(i)));
        ids[i].set(&m);
        }
      gpu.commit(storage);
      require(gpu.prev.empty(),"history off: no prev page");
      }
    auto m=marker(-1);
    ids[1].set(&m);
    storage.setHistory(true);
    require(&storage.ssboPrev()==&storage.ssbo(),"(g) before the first commit with history prev is cur");
    auto late=storage.alloc(64);
    gpu.commit(storage);
    require(gpu.last.saveCount==0 && gpu.last.freshCount>0,"(g) the first commit copies, it does not save");
    expectHistory(storage,gpu,fresh,true,"(g) first commit with history");
    for(int frame=0;frame<4;++frame) {
      m=marker(float(500+frame));
      ids[size_t(frame)].set(&m);
      commitAndCheck(storage,gpu,fresh,"(g) after switching on");
      }
    }

  void testHistoryOff() {
    // (h) off by default and after switching off: no prev state, ssboPrev is ssbo
    InstanceStorage storage;
    require(!storage.hasHistory() && !InstanceStorageContractAccess::keepsHistoryState(storage),"off by default");
    require(&storage.ssboPrev()==&storage.ssbo(),"(h) ssboPrev is ssbo with history off");
    HistoryPages gpu;
    auto a=storage.alloc(64);
    auto m=marker(1);
    a.set(&m);
    gpu.commit(storage);
    gpu.commit(storage);
    require(!gpu.last.upload,"(h) history off: an idle frame uploads nothing");
    m=marker(2);
    a.set(&m);
    gpu.commit(storage);
    require(gpu.last.upload && gpu.last.patchCount>0,"(h) history off: a change is patched");
    gpu.commit(storage);
    require(!gpu.last.upload,"(h) history off: the frame after a change uploads nothing");
    require(!InstanceStorageContractAccess::keepsHistoryState(storage),"(h) no history state kept while off");

    storage.setHistory(true);
    FreshSet fresh;
    gpu.commit(storage);
    expectHistory(storage,gpu,fresh,true,"(h) on");
    m=marker(3);
    a.set(&m);
    commitAndCheck(storage,gpu,fresh,"(h) on, changed");
    storage.setHistory(false);
    require(!InstanceStorageContractAccess::keepsHistoryState(storage),"(h) switching off releases every history state");
    require(&storage.ssboPrev()==&storage.ssbo(),"(h) ssboPrev is ssbo after switching off");
    gpu.commit(storage);
    require(gpu.prev.empty() && !gpu.last.upload,"(h) off again: the dirty-of-previous frame is forgotten");
    auto b=storage.alloc(64);
    gpu.commit(storage);
    require(!InstanceStorageContractAccess::keepsHistoryState(storage),"(h) alloc while off keeps no fresh state");
    }

  void testHistoryRandom() {
    // a long mixed run: alloc, free, realloc, sets from frame to frame, history toggled
    InstanceStorage storage;
    storage.setHistory(true);
    HistoryPages gpu;
    FreshSet fresh;
    std::vector<InstanceStorage::Id> ids(48);
    uint32_t seed=12345;
    auto rnd=[&seed](uint32_t n){ seed=seed*1664525u+1013904223u; return (seed>>8)%n; };
    bool fullCopy=false;
    for(int frame=0;frame<400;++frame) {
      if(frame==150) storage.setHistory(false);
      if(frame==170) { storage.setHistory(true); fullCopy=true; }
      const auto ops=rnd(8);
      for(uint32_t op=0;op<ops;++op) {
        auto& id=ids[rnd(uint32_t(ids.size()))];
        switch(rnd(6)) {
          case 0:
            id=storage.alloc(16*size_t(1+rnd(40)));
            fresh.add(id);
            break;
          case 1:
            id=InstanceStorage::Id();
            break;
          case 2: {
            const auto before=id.isEmpty() ? size_t(-1) : begin(id);
            storage.realloc(id,16*size_t(rnd(64)));
            if(!id.isEmpty() && begin(id)!=before) fresh.add(id);
            break;
            }
          default:
            if(!id.isEmpty()) {
              std::vector<uint8_t> bytes(id.size());
              for(auto& v:bytes) v=uint8_t(rnd(4));
              const size_t at=rnd(uint32_t(bytes.size()));
              id.set(bytes.data()+at,at,bytes.size()-at);
              }
          }
        }
      gpu.commit(storage);
      if(storage.hasHistory()) {
        expectHistory(storage,gpu,fresh,gpu.last.grow || fullCopy,"random run");
        fullCopy=false;
        } else {
        require(gpu.prev.empty() && !InstanceStorageContractAccess::keepsHistoryState(storage),"random run, off");
        }
      fresh.ranges.clear();
      }
    }
  }

int main() {
  require(!Resources::hasGraphicsDevice(),"CPU storage contract must not construct a graphics device");
  testZeroPaddingReuse();
  testMovedGrowthAndPageResize();
  testMovedReusedRange();
  testWordBoundaries();
  testConcurrentSharedWords();
  testHistoryStillAfterChange();
  testHistoryReuseAndMove();
  testHistoryGrowth();
  testHistoryOnMidRun();
  testHistoryOff();
  testHistoryRandom();
  std::puts("instance storage contracts passed");
  return EXIT_SUCCESS;
  }
