#include "baseimport.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "definitionjson.h"
#include "game/definitions/stockstoryprograms.h"
#include "gothicglb.h"
#include "outputunits.h"
#include "programdata.h"
#include "storygraph/bytecode.h"
#include "storygraph/graphjson.h"
#include "storygraph/lifter.h"

void BaseImport::convertStoryGraph(const std::filesystem::path& base) {
  using DefinitionJson::require;
  using namespace storygraph;
  const auto index = ProgramData::readIndex(base/"programs");
  const auto* source = index.server();
  if(source==nullptr || source->origin!="host" || source->recordSha256!=StockStoryPrograms::stockProgram)
    return;

  std::string programSha;
  auto program = ProgramData::readProgram(index.path(*source),&programSha);
  require(programSha==source->recordSha256,"story graph: program record differs from index");

  const auto ouIndex = OutputUnits::readIndex(base/"outputunits");
  const auto relativeOu = std::filesystem::path(source->path).parent_path().parent_path()/"CONTENT"/"CUTSCENE"/"OU.BIN";
  const auto* ou = ouIndex.findHost(relativeOu.generic_string());
  if(ou==nullptr)
    return;
  std::string ouSha;
  const auto library = OutputUnits::readLibrary(ouIndex.path(*ou),&ouSha);
  require(ouSha==ou->recordSha256,"story graph: output units record differs from index");
  const auto lines = loadLines(library);
  if(lines.empty())
    return;

  auto script = daedalus::DaedalusScript::from_program(std::move(program));
  daedalus::register_all_script_classes(script);
  daedalus::DaedalusVm vm(std::move(script),daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  vm.register_default_external([](const daedalus::DaedalusSymbol&) {});
  const Bytecode code(vm);
  const Lifter lifter(vm,code);
  const auto model = buildModel(vm,code,lifter,lines);
  const auto text = toJson(model,{programSha,source->sha256,ouSha,ou->sha256,{}},false);
  const auto path = base/"storygraph"/"story.json";
  std::filesystem::create_directories(path.parent_path());
  gothicglb::writeFile(path,text.data(),text.size());
  }
