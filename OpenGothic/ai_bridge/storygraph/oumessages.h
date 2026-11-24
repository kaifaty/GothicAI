#pragma once

#include "outputunits.h"

namespace storygraph {

OutputUnits::Index readOuIndex(const std::filesystem::path& base, const BaseImport::Files& archives);
WorldData::ZenFile readOuLibrary(const OutputUnits::Index& index, const OutputUnits::Source& source,
                                std::string& actualSha);
int printOuMessages(const OutputUnits::Index& index);

}
