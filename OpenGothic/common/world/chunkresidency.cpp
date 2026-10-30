#include "chunkresidency.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {

int64_t cellDistance(ChunkId a, ChunkId b) {
  return std::max(std::abs(int64_t(a.x)-b.x),std::abs(int64_t(a.z)-b.z));
  }

float distanceSquared(const ChunkTable::Entry& chunk, const Tempest::Vec3& point) {
  const float x=std::max({chunk.min.x-point.x,0.f,point.x-chunk.max.x});
  const float y=std::max({chunk.min.y-point.y,0.f,point.y-chunk.max.y});
  const float z=std::max({chunk.min.z-point.z,0.f,point.z-chunk.max.z});
  return x*x+y*y+z*z;
  }

}

ChunkResidency::ChunkResidency(const ChunkTable& table):chunks(table),states(table.entries.size(),State::Outside) {
  counters.outside=states.size();
  }

std::unique_ptr<ChunkResidency> ChunkResidency::create(const ChunkTable& table) {
  const auto* enabled=std::getenv("OPENGOTHIC_STREAMING");
  if(enabled!=nullptr && std::strcmp(enabled,"0")==0)
    return nullptr;
  return std::make_unique<ChunkResidency>(table);
  }

void ChunkResidency::update(const Tempest::Vec3& hero, const Tempest::Vec3& camera, float zFar) {
  const auto started=Clock::now();
  const auto heroCell=ChunkId::fromPosition(hero.x,hero.z);
  const auto cameraCell=ChunkId::fromPosition(camera.x,camera.z);
  counters.near=0;
  counters.far=0;
  counters.outside=0;
  size_t transitions=0;
  for(size_t i=0;i<states.size();++i) {
    const auto& chunk=chunks.entries[i];
    const auto previous=states[i];
    const auto radius=previous==State::Near ? NearExitRadius : NearRadius;
    State next=State::Outside;
    if(cellDistance(chunk.id,heroCell)<=radius || cellDistance(chunk.id,cameraCell)<=radius) {
      next=State::Near;
      } else {
      const float far=zFar+(previous==State::Far ? FarExitMargin : 0.f);
      // An empty override still has a cell identity, but no Far geometry bounds.
      if(chunk.triangleCount!=0 && distanceSquared(chunk,camera)<=far*far)
        next=State::Far;
      }
    switch(next) {
      case State::Near:    ++counters.near; break;
      case State::Far:     ++counters.far; break;
      case State::Outside: ++counters.outside; break;
      }
    if(next==previous)
      continue;
    states[i]=next;
    ++transitions;
    ++counters.totalTransitions;
    if(next==State::Near || previous==State::Near)
      ++counters.transitionsNear;
    if(next==State::Far || previous==State::Far)
      ++counters.transitionsFar;
    // Snapshot ownership without copying callable state. A listener can
    // unsubscribe during dispatch while its active call remains alive.
    if(!listeners.empty()) {
      const auto callbacks=listeners;
      for(const auto& listener:callbacks)
        if(listeners.find(listener.first)!=listeners.end())
          (*listener.second)(chunk.id,previous,next);
      }
    }
  counters.peakNear=std::max(counters.peakNear,uint64_t(counters.near));
  if(transitions!=0) {
    recentTransitions.emplace_back(started,transitions);
    counters.transitionsLastSecond+=transitions;
    }
  while(!recentTransitions.empty() && started-recentTransitions.front().first>=std::chrono::seconds(1)) {
    counters.transitionsLastSecond-=recentTransitions.front().second;
    recentTransitions.pop_front();
    }

  const auto workStarted=Clock::now();
  const auto deadline=workStarted+WorkBudget;
  while(!work.empty() && Clock::now()<deadline) {
    if(work.front()(deadline))
      work.pop_front();
    }
  const auto finished=Clock::now();
  counters.workMs=std::chrono::duration<double,std::milli>(finished-workStarted).count();
  counters.updateMs=std::chrono::duration<double,std::milli>(finished-started).count();
  }

ChunkResidency::State ChunkResidency::desired(ChunkId id) const {
  for(size_t i=0;i<chunks.entries.size();++i)
    if(chunks.entries[i].id==id)
      return states[i];
  throw std::out_of_range("chunk is not in the world table");
  }

uint64_t ChunkResidency::subscribe(Listener listener) {
  const auto subscription=++nextSubscription;
  listeners.emplace(subscription,std::make_shared<Listener>(std::move(listener)));
  return subscription;
  }

void ChunkResidency::unsubscribe(uint64_t subscription) {
  listeners.erase(subscription);
  }

void ChunkResidency::enqueue(Work task) {
  work.push_back(std::move(task));
  }
