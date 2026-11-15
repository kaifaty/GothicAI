#include "baseimport.h"
#include "npcsnapshot.h"
#include "programdata.h"
#include "daedalus/script.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

int BaseImport::convertItemDefinitions(const fs::path& base, bool verifyOnly) {
  const auto start=std::chrono::steady_clock::now();
  const auto index=ProgramData::readIndex(base/"programs");
  size_t total=0,captured=0,programs=0;
  for(const auto& [name,sha]:index.programs) {
    NpcSnapshot::Program program;
    program.name=name;
    program.data=ProgramData::readProgram(index.dir/(name+".json"),&program.sha256);
    if(program.sha256!=sha) throw std::runtime_error("item definitions: program SHA differs: "+name);
    auto script=daedalus::DaedalusScript::from_program(program.data);
    bool hasItems=false;
    if(script.find_symbol_by_name("C_ITEM"))
      script.enumerate_instances_by_class_name("C_ITEM",[&](daedalus::DaedalusSymbol& symbol) {
        hasItems=hasItems || (symbol.is_const() && symbol.address()>0);
        });
    if(!hasItems) continue;
    ++programs;
    const auto file=base/"definitions"/sha/"items.json";
    if(!verifyOnly) {
      const auto counts=NpcSnapshot::writeItems(program,file);
      total+=counts.total;
      captured+=counts.captured;
      }
    if(NpcSnapshot::verifyItems(program,file)!=0)
      throw std::runtime_error("item definitions: constructor comparison failed: "+name);
    }
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<(verifyOnly ? "verified " : "written and verified ")<<programs<<" item definition programs";
  if(!verifyOnly) std::cout<<"; "<<total<<" items, "<<captured<<" captured, "<<(total-captured)<<" uncaptured";
  std::cout<<"; 0 failed ("<<ms<<" ms)\n";
  return 0;
  }
