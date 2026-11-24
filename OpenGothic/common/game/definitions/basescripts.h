#pragma once

// Resources loads the game's catalog selection and dialog line library through GameBaseContent.

#include <memory>
#include <string_view>
#include <vector>

namespace ProgramData { struct Index; }
namespace OutputUnits { struct Index; }
namespace asset { struct CutsceneBlock; }
namespace NativeBaseCatalog { struct Selection; }

// Selected identifier of the prepared fight tactics on the loaded base.
std::string_view baseFightCatalog();
// The prepared runtime catalog on a v3 base, owned until the next base selection.
const NativeBaseCatalog::Selection* baseNativeCatalog();
const ProgramData::Index& baseProgramIndex();
const OutputUnits::Index& baseOutputUnitsIndex();
bool loadBaseOutputUnits(std::string_view request, int language,
                         std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks);
bool loadBaseOutputUnits(std::string_view request, int language, std::string_view branch,
                         std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks,
                         std::u16string_view hostPath = {});
