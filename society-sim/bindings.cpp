#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "society/journal.h"
#include "society/society.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using Society::Choice;
using Society::DecisionBatch;
using Society::Time;

class Workers {
  public:
    explicit Workers(size_t count) {
      try {
        for(size_t i=1; i<count; ++i)
          threads.emplace_back([this] { listen(); });
        } catch(...) {
        stop();
        throw;
        }
      }

    ~Workers() { stop(); }

    void run(size_t count, std::function<void(size_t)> task) {
      if(threads.empty()) {
        for(size_t i=0; i<count; ++i)
          task(i);
        return;
        }
      {
        std::lock_guard lock(mutex);
        job = std::move(task);
        size = count;
        next.store(0,std::memory_order_relaxed);
        remaining = threads.size();
        failure = {};
        ++generation;
      }
      ready.notify_all();
      execute();
      std::unique_lock lock(mutex);
      done.wait(lock,[&] { return remaining==0; });
      job = {};
      if(failure)
        std::rethrow_exception(failure);
      }

  private:
    void stop() {
      {
        std::lock_guard lock(mutex);
        stopping = true;
      }
      ready.notify_all();
      for(auto& thread : threads)
        thread.join();
      }

    void execute() {
      for(size_t i = next.fetch_add(1,std::memory_order_relaxed); i<size;
                 i = next.fetch_add(1,std::memory_order_relaxed)) {
        try {
          job(i);
          } catch(...) {
          std::lock_guard lock(mutex);
          if(!failure)
            failure = std::current_exception();
          }
        }
      }

    void listen() {
      size_t seen = 0;
      std::unique_lock lock(mutex);
      for(;;) {
        ready.wait(lock,[&] { return stopping || seen!=generation; });
        if(stopping)
          return;
        seen = generation;
        lock.unlock();
        execute();
        lock.lock();
        if(--remaining==0)
          done.notify_one();
        }
      }

    std::mutex                  mutex;
    std::condition_variable     ready, done;
    std::vector<std::thread>    threads;
    bool                        stopping = false;
    size_t                      generation = 0, remaining = 0, size = 0;
    std::atomic<size_t>         next{0};
    std::function<void(size_t)> job;
    std::exception_ptr         failure;
  };

struct PackedBatch {
  std::vector<uint32_t> world, npc;
  std::vector<uint8_t>  mask;
  std::vector<float>    own, options;
  };

struct Checkpoint {
  std::shared_ptr<const Society::World> world;
  std::vector<Society::Snapshot>        simulations;
  std::vector<std::string>              journal;
  };

template<class T>
nb::ndarray<nb::numpy,const T> view(const std::vector<T>& values, std::initializer_list<size_t> shape,
                                  const nb::object& owner) {
  // NumPy may ask for a data pointer even when one of the dimensions is zero.
  static const T empty{};
  return nb::ndarray<nb::numpy,const T>(values.empty() ? &empty : values.data(),shape,owner);
  }

template<class T>
auto vectorOwner(std::vector<T> values) {
  auto data = std::make_unique<std::vector<T>>(std::move(values));
  nb::capsule owner(data.get(),[](void* ptr) noexcept { delete static_cast<std::vector<T>*>(ptr); });
  return std::pair(data.release(),std::move(owner));
  }

class VecSociety {
  public:
    VecSociety(const std::filesystem::path& worldPath, const std::filesystem::path& configPath,
               const std::vector<uint64_t>& seeds, size_t threads, bool recordEvents)
      : world(std::make_shared<const Society::World>(Society::loadWorld(worldPath,configPath))),
        layout(Society::featureLayout(*world)), journal(recordEvents ? seeds.size() : 0),
        batches(seeds.size()), answers(seeds.size()), offsets(seeds.size()+1),
        workers(workerCount(seeds.size(),threads)) {
      simulations.reserve(seeds.size());
      for(uint64_t seed : seeds)
        simulations.push_back(Society::makeSim(*world,{seed,true}));
      for(size_t i=0; i<seeds.size(); ++i)
        startJournal(i);
      }

    nb::dict advance(Time horizon) {
      const Operation operation(*this);
      requireHealthy();
      if(pending())
        throw std::logic_error("answer the pending batch before advancing");
      for(const auto& sim : simulations)
        if(horizon<sim.s.now)
          throw std::invalid_argument("the advance horizon cannot precede a world's time");
      if(!journal.empty() && horizon>std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("the advance horizon is outside the journal time range");

      run([&](size_t i) {
        Society::advance(simulations[i],batches[i],horizon);
        record(i);
      });
      indexBatches();
      return batch();
      }

    void act(nb::ndarray<nb::numpy,const int64_t,nb::ndim<1>,nb::c_contig> choices) {
      const Operation operation(*this);
      requirePending();
      if(choices.size()!=offsets.back())
        throw std::invalid_argument("provide one choice for every pending row");
      // Check all worlds before applying any choice.
      for(size_t i=0; i<batches.size(); ++i) {
        const auto& b = batches[i];
        auto& out = answers[i];
        out.resize(b.size());
        for(size_t row=0; row<b.size(); ++row) {
          const int64_t option = choices(offsets[i]+row);
          if(option<0 || static_cast<uint64_t>(option)>=b.stride || b.mask[row*b.stride+size_t(option)]==0)
            throw std::invalid_argument("a choice is outside its row's mask");
          out[row] = {b.npc[row],uint16_t(option),Society::Chooser::External};
          }
        }
      applyAnswers();
      }

    auto actTeacher() {
      const Operation operation(*this);
      requirePending();
      std::vector<uint16_t> selected(offsets.back());
      run([&](size_t i) {
        if(batches[i].size()==0)
          return;
        Society::teacher(simulations[i],batches[i],answers[i]);
        for(size_t row=0; row<answers[i].size(); ++row)
          selected[offsets[i]+row] = answers[i][row].option;
      });
      applyAnswers();
      const auto [values,owner] = vectorOwner(std::move(selected));
      return view(*values,{values->size()},owner);
      }

    auto teacherScores() {
      const Operation operation(*this);
      requirePending();
      std::vector<float> scores(offsets.back()*layout.stride);
      run([&](size_t i) {
        const auto& b = batches[i];
        if(b.size()!=0)
          Society::teacherScores(simulations[i],b,std::span(scores).subspan(offsets[i]*layout.stride,b.size()*layout.stride));
      });
      const auto [values,owner] = vectorOwner(std::move(scores));
      return view(*values,{offsets.back(),layout.stride},owner);
      }

    nb::list events() {
      const Operation operation(*this);
      requireHealthy();
      if(journal.empty())
        throw std::logic_error("journaling is disabled");
      nb::list out;
      for(auto& bytes : journal) {
        out.append(nb::bytes(bytes.data(),bytes.size()));
        bytes.clear();
        }
      return out;
      }

    Checkpoint save() {
      const Operation operation(*this);
      requireHealthy();
      Checkpoint out{world,{},journal};
      out.simulations.reserve(simulations.size());
      for(const auto& sim : simulations)
        out.simulations.push_back(Society::save(sim));
      return out;
      }

    nb::dict restore(const Checkpoint& checkpoint) {
      const Operation operation(*this);
      if(checkpoint.world!=world)
        throw std::invalid_argument("the checkpoint belongs to another VecSociety");
      journal = checkpoint.journal;
      run([&](size_t i) {
        auto& sim = simulations[i];
        Society::restore(sim,checkpoint.simulations[i]);
        batches[i] = sim.s.batch;
        if(batches[i].size()!=0)
          Society::encode(sim,batches[i]);
      });
      healthy = true;
      indexBatches();
      return batch();
      }

    void reset(const std::vector<uint64_t>& seeds) {
      const Operation operation(*this);
      if(seeds.size()!=simulations.size())
        throw std::invalid_argument("provide one seed for each world");
      run([&](size_t i) {
        Society::reset(simulations[i],seeds[i]);
        startJournal(i);
      });
      healthy = true;
      clearBatches();
      }

    auto times() const {
      requireIdle();
      requireHealthy();
      std::vector<Time> out;
      out.reserve(simulations.size());
      for(const auto& sim : simulations)
        out.push_back(sim.s.now);
      return out;
      }

    Time                       startTime() const { return world->startTime(); }
    uint64_t                   worldHash() const { return world->exportHash; }
    uint64_t                   configHash() const { return world->config.resolvedHash; }
    size_t                     stride() const { return layout.stride; }
    size_t                     worlds() const { return simulations.size(); }
    auto                       featureVersion() const { return Society::FeatureVersion; }
    const auto&                ownNames() const { return layout.own; }
    const auto&                optionNames() const { return layout.options; }

  private:
    struct Operation {
      explicit Operation(VecSociety& owner) : self(owner) {
        self.requireIdle();
        self.busy = true;
        }
      ~Operation() { self.busy = false; }
      VecSociety& self;
      };

    static size_t workerCount(size_t worlds, size_t threads) {
      if(worlds==0 || worlds>std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("provide seeds for at least one world");
      if(threads==0)
        throw std::invalid_argument("threads must be positive");
      return std::min(worlds,threads);
      }

    void requireIdle() const {
      if(busy)
        throw std::logic_error("another operation is running on this VecSociety");
      }

    void requireHealthy() const {
      if(!healthy)
        throw std::logic_error("a world operation failed; reset or restore before continuing");
      }

    bool pending() const { return offsets.back()!=0; }

    void requirePending() const {
      requireHealthy();
      if(!pending())
        throw std::logic_error("there is no pending decision batch");
      }

    void indexBatches() {
      for(size_t i=0; i<batches.size(); ++i)
        offsets[i+1] = offsets[i]+batches[i].size();
      }

    void clearBatches() {
      for(auto& b : batches)
        b.clear();
      std::fill(offsets.begin(),offsets.end(),0);
      }

    nb::dict batch() const {
      const size_t rows = offsets.back();
      const size_t ownWidth = layout.own.size();
      const size_t optionWidth = layout.options.size();
      auto holder = std::make_unique<PackedBatch>();
      auto* data = holder.get();
      data->world.reserve(rows);
      data->npc.reserve(rows);
      data->mask.reserve(rows*layout.stride);
      data->own.reserve(rows*ownWidth);
      data->options.reserve(rows*layout.stride*optionWidth);
      for(size_t i=0; i<batches.size(); ++i) {
        const auto& b = batches[i];
        if(b.size()==0)
          continue;
        if(b.stride!=layout.stride || b.own.size()!=b.size()*ownWidth ||
           b.options.size()!=b.size()*b.stride*optionWidth || b.mask.size()!=b.size()*b.stride)
          throw std::logic_error("the decision batch disagrees with the feature layout");
        data->world.insert(data->world.end(),b.size(),uint32_t(i));
        data->npc.insert(data->npc.end(),b.npc.begin(),b.npc.end());
        data->mask.insert(data->mask.end(),b.mask.begin(),b.mask.end());
        data->own.insert(data->own.end(),b.own.begin(),b.own.end());
        data->options.insert(data->options.end(),b.options.begin(),b.options.end());
        }
      nb::capsule owner(data,[](void* ptr) noexcept { delete static_cast<PackedBatch*>(ptr); });
      holder.release();
      nb::dict out;
      out["world"] = view(data->world,{rows},owner);
      out["npc"] = view(data->npc,{rows},owner);
      out["mask"] = view(data->mask,{rows,layout.stride},owner);
      out["own"] = view(data->own,{rows,ownWidth},owner);
      out["options"] = view(data->options,{rows,layout.stride,optionWidth},owner);
      return out;
      }

    void run(std::function<void(size_t)> task) {
      try {
        nb::gil_scoped_release release;
        workers.run(simulations.size(),std::move(task));
        } catch(...) {
        healthy = false;
        throw;
        }
      }

    void startJournal(size_t i) {
      if(!journal.empty())
        journal[i] = Society::Journal::header(*world,simulations[i].seed,"batch");
      record(i);
      }

    void record(size_t i) {
      const auto emitted = Society::drainEvents(simulations[i]);
      if(!journal.empty())
        Society::Journal::append(journal[i],emitted);
      }

    void applyAnswers() {
      run([&](size_t i) {
        if(batches[i].size()==0)
          return;
        Society::act(simulations[i],answers[i]);
        record(i);
      });
      clearBatches();
      }

    const std::shared_ptr<const Society::World> world;
    const Society::FeatureLayout               layout;
    std::vector<Society::Sim>                   simulations;
    std::vector<std::string>                    journal;
    std::vector<DecisionBatch>                  batches;
    std::vector<std::vector<Choice>>           answers;
    std::vector<size_t>                         offsets;
    Workers                                     workers;
    bool                                        busy = false, healthy = true;
  };

}

NB_MODULE(_native, m) {
  nb::class_<Checkpoint>(m,"Checkpoint");
  nb::class_<VecSociety>(m,"VecSociety")
    .def(nb::init<const std::filesystem::path&,const std::filesystem::path&,const std::vector<uint64_t>&,size_t,bool>(),
         "world_path"_a,"config_path"_a,"seeds"_a,"threads"_a=1,"record_events"_a=false)
    .def("advance",&VecSociety::advance,"horizon"_a)
    .def("act",&VecSociety::act,"choices"_a.noconvert())
    .def("act_teacher",&VecSociety::actTeacher)
    .def("teacher_scores",&VecSociety::teacherScores)
    .def("events",&VecSociety::events)
    .def("save",&VecSociety::save)
    .def("restore",&VecSociety::restore,"checkpoint"_a)
    .def("reset",&VecSociety::reset,"seeds"_a)
    .def_prop_ro("times",&VecSociety::times)
    .def_prop_ro("start_time",&VecSociety::startTime)
    .def_prop_ro("world_hash",&VecSociety::worldHash)
    .def_prop_ro("config_hash",&VecSociety::configHash)
    .def_prop_ro("stride",&VecSociety::stride)
    .def_prop_ro("worlds",&VecSociety::worlds)
    .def_prop_ro("feature_version",&VecSociety::featureVersion)
    .def_prop_ro("own_feature_names",&VecSociety::ownNames)
    .def_prop_ro("option_feature_names",&VecSociety::optionNames);
  }
