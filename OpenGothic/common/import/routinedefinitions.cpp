#include "baseimport.h"
#include "npcsnapshot.h"
#include "programdata.h"
#include "daedalus/script.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int BaseImport::convertRoutineDefinitions(const fs::path& base) {
  const auto start=std::chrono::steady_clock::now();
  const auto index=ProgramData::readIndex(base/"programs");
  size_t total=0,captured=0,programs=0;
  for(const auto& [name,sha]:index.programs) {
    NpcSnapshot::Program program;
    program.name=name;
    program.data=ProgramData::readProgram(index.dir/(name+".json"),&program.sha256);
    if(program.sha256!=sha) throw std::runtime_error("routine definitions: program SHA differs: "+name);
    const auto script=daedalus::DaedalusScript::from_program(program.data);
    bool hasRoutines=false;
    for(const auto& symbol:script.symbols())
      if(symbol.type()==daedalus::DaedalusDataType::FUNCTION && !symbol.is_external() && symbol.name().starts_with("RTN_"))
        hasRoutines=true;
    if(!hasRoutines) continue;
    ++programs;
    const auto file=base/"definitions"/sha/"routines.json";
    const auto counts=NpcSnapshot::writeRoutines(program,file);
    total+=counts.total;
    captured+=counts.captured;
    if(NpcSnapshot::verifyRoutines(program,file)!=0)
      throw std::runtime_error("routine definitions: function comparison failed: "+name);
    }
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"written and verified "<<programs<<" routine definition programs; "<<total<<" routines, "<<captured
           <<" captured, "<<(total-captured)<<" uncaptured; 0 failed ("<<ms<<" ms)\n";
  return 0;
  }
