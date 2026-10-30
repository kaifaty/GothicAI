#include "movablevobs.h"
#include "voblayer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "movablevobsdata.h"

namespace {

// '*' and '?' cover asset families without turning unrelated architecture,
// grouped props or broken furniture into single convex bodies.
bool matches(std::string_view pattern, std::string_view text) {
  size_t p = 0, t = 0, star = std::string_view::npos, retry = 0;
  while(t<text.size()) {
    if(p<pattern.size() && (pattern[p]=='?' || pattern[p]==text[t])) {
      ++p;
      ++t;
      } else if(p<pattern.size() && pattern[p]=='*') {
      star = p++;
      retry = t;
      } else if(star!=std::string_view::npos) {
      p = star+1;
      t = ++retry;
      } else {
      return false;
      }
    }
  while(p<pattern.size() && pattern[p]=='*')
    ++p;
  return p==pattern.size();
  }

const std::vector<MovableVobs::Rule>& rules() {
  static const auto value = [] {
    std::vector<MovableVobs::Rule> result;
    std::istringstream csv(movableVobsCsv);
    for(std::string line; std::getline(csv,line);) {
      if(line.empty() || line[0]=='#')
        continue;
      std::istringstream row(line);
      MovableVobs::Rule rule;
      std::string multiplier, walking;
      if(!std::getline(row,rule.visualPattern,',') || !std::getline(row,rule.className,',') ||
         !std::getline(row,multiplier,',') || !std::getline(row,walking))
        continue;
      rule.massMultiplier = std::strtof(multiplier.c_str(),nullptr);
      rule.blocksWalking = walking=="1";
      if(rule.massMultiplier>0.f)
        result.emplace_back(std::move(rule));
      }
    return result;
    }();
  return value;
  }

}

const MovableVobs::Rule* MovableVobs::classify(const asset::VirtualObject& vob) {
  // Layer meshes request either static mesh collision or no collision.
  if(VobLayer::isAdded(vob))
    return nullptr;
  if(vob.type!=asset::VirtualObjectType::zCVob || !vob.show_visual || vob.visual==nullptr)
    return nullptr;
  if(vob.visual->type!=asset::VisualType::MESH && vob.visual->type!=asset::VisualType::MULTI_RESOLUTION_MESH)
    return nullptr;
  if(vob.anim_mode!=asset::AnimationType::NONE && vob.anim_strength>0.f)
    return nullptr;
  return classifyVisual(vob.visual->name);
  }

const MovableVobs::Rule* MovableVobs::classifyVisual(std::string_view visual) {
  const auto slash = visual.find_last_of("/\\");
  if(slash!=std::string_view::npos)
    visual.remove_prefix(slash+1);
  visual = visual.substr(0,visual.find_last_of('.'));
  std::string name(visual);
  std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  for(const auto& rule:rules())
    if(matches(rule.visualPattern,name))
      return &rule;
  return nullptr;
  }
