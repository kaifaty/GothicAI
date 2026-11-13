#pragma once

// The base's Daedalus programs (P12) and dialog line library (OU, P14): Gothic's data. Their indexes are read and
// checked when Resources selects the base, through GameBaseContent; their loaders read the selected records.

#include <memory>
#include <string_view>
#include <vector>

namespace ProgramData { struct Index; }
namespace OutputUnits { struct Index; }
namespace asset { struct CutsceneBlock; }

const ProgramData::Index& baseProgramIndex();
const OutputUnits::Index& baseOutputUnitsIndex();
bool loadBaseOutputUnits(std::string_view request, int language,
                         std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks);
bool loadBaseOutputUnits(std::string_view request, int language, std::string_view branch,
                         std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks,
                         std::u16string_view hostPath = {});
