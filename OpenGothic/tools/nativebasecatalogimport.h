#pragma once

#include "game/definitions/nativebasecatalog.h"

namespace daedalus { class DaedalusScript; }
namespace OutputUnits { struct Index; }
namespace ProgramData { struct Index; }

namespace NativeBaseCatalog {

// Prepare only stock catalog choices whose old source selection can be verified.
Selection prepare(const ProgramData::Index& programs, const OutputUnits::Index& outputUnits);
uint32_t prepareDirectorFirstId(const daedalus::DaedalusScript& program);
std::vector<State> prepareStates(const daedalus::DaedalusScript& program);
std::vector<InitialRow> prepareInitialState(const daedalus::DaedalusScript& program);
Constants prepareConstants(const daedalus::DaedalusScript& program);
Identities prepareIdentities(daedalus::DaedalusScript& program, const std::filesystem::path& base);
std::vector<Identity> prepareRoutines(const daedalus::DaedalusScript& program, const std::filesystem::path& base);
std::vector<Identity> prepareItemCallbacks(const daedalus::DaedalusScript& program, const std::filesystem::path& base);
void verify(const std::filesystem::path& base, const Selection& expected);

}
