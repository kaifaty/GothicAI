#pragma once

#include "game/definitions/nativebasecatalog.h"

namespace daedalus { class DaedalusScript; }
namespace OutputUnits { struct Index; }
namespace ProgramData { struct Index; }

namespace NativeBaseCatalog {

// Prepare only stock catalog choices whose old source selection can be verified.
Selection prepare(const ProgramData::Index& programs, const OutputUnits::Index& outputUnits);
std::vector<State> prepareStates(const daedalus::DaedalusScript& program);
void verify(const std::filesystem::path& base, const Selection& expected);

}
