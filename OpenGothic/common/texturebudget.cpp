#include "texturebudget.h"

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

uint64_t TextureBudget::target(const Device& device, uint64_t liveTextures) {
  if(!device.known || device.budget==0)
    return Floor;
  const uint64_t other    = device.usage>liveTextures ? device.usage-liveTextures : 0;
  const uint64_t headroom = std::max(MinHeadroom,device.budget/HeadroomDivisor);
  const uint64_t share    = device.budget/(device.shared ? ShareShared : ShareDiscrete);
  const uint64_t taken    = other+headroom;
  const uint64_t room     = device.budget>taken ? device.budget-taken : 0;
  return std::max(Floor,std::min(share,room));
  }

uint64_t TextureBudget::follow(uint64_t working, uint64_t next) {
  if(working==0)
    return next;
  const double change = std::abs(double(next)-double(working))/double(working);
  return change>Step ? next : working;
  }

bool TextureBudget::logWorthy(uint64_t logged, uint64_t now) {
  if(logged==0)
    return now!=0;
  return std::abs(double(now)-double(logged))/double(logged)>LogStep;
  }

const char* TextureBudget::name(Source source) {
  switch(source) {
    case Source::Default:     return "default, the device budget is not readable";
    case Source::Rule:        return "rule";
    case Source::Setting:     return "setting";
    case Source::Environment: return "environment";
    }
  return "?";
  }

TextureBudget::Choice TextureBudget::choose(const char* environment, std::string_view setting, const char* field) {
  const bool fromEnvironment = environment!=nullptr;
  const std::string text(fromEnvironment ? std::string_view(environment) : setting);
  Choice ret;
  if(text.empty() || text=="auto")
    return ret;
  char* end = nullptr;
  const auto value = std::strtoll(text.c_str(),&end,10);
  if(end==text.c_str() || *end!='\0' || value<0 || uint64_t(value)>std::numeric_limits<uint64_t>::max()/MiB)
    throw std::runtime_error(std::string(field)+" must be a nonnegative integer or auto");
  ret.automatic = false;
  ret.bytes     = uint64_t(value)*MiB;
  ret.source    = fromEnvironment ? Source::Environment : Source::Setting;
  return ret;
  }

uint32_t TextureBudget::reserve(uint32_t stage, uint32_t reserve) {
  return stage==0 ? reserve : 0;
  }

void TextureBudget::Sums::clear() {
  d0 = 0;
  d1 = 0;
  deltas.clear();
  }

void TextureBudget::Sums::fixed(uint64_t bytes) {
  d0 += bytes;
  d1 += bytes;
  }

void TextureBudget::Sums::add(uint32_t stage, int64_t delta) {
  const size_t at = stage-2;
  if(deltas.size()<=at)
    deltas.resize(at+1,0);
  deltas[at] += delta;
  }

std::vector<uint64_t> TextureBudget::Sums::finish() const {
  std::vector<uint64_t> d = {d0,d1};
  int64_t sum = int64_t(d1);
  for(const auto delta:deltas) {
    sum += delta;
    d.push_back(uint64_t(std::max<int64_t>(sum,0)));
    }
  // the last stage that changes anything
  while(d.size()>1 && d.back()==d[d.size()-2])
    d.pop_back();
  return d;
  }

uint32_t TextureBudget::step(Ladder& ladder, const std::vector<uint64_t>& d, uint64_t target, Clock::time_point now, bool force) {
  if(d.empty()) {
    ladder = Ladder();
    return 0;
    }
  const uint32_t deepest = uint32_t(d.size()-1);
  const auto shallowest = [&](double limit) {
    for(uint32_t s=0; s<deepest; ++s)
      if(double(d[s])<=limit)
        return s;
    return deepest;
    };
  const uint32_t fit = shallowest(double(target));
  ladder.stage = std::min(ladder.stage,deepest);
  if(force || fit>=ladder.stage) {
    ladder.stage   = fit;
    ladder.backing = false;
    return ladder.stage;
    }
  const double back = BackFraction*double(target);
  if(double(d[ladder.stage-1])>back) {
    ladder.backing = false;
    return ladder.stage;
    }
  if(!ladder.backing) {
    ladder.backing   = true;
    ladder.backSince = now;
    return ladder.stage;
    }
  if(now-ladder.backSince<BackDelay)
    return ladder.stage;
  ladder.stage   = shallowest(back);
  ladder.backing = false;
  return ladder.stage;
  }
