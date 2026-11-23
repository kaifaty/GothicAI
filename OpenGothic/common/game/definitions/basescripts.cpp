#include "basescripts.h"

#include "gamebasecontent.h"
#include "programdata.h"
#include "outputunits.h"
#include "resources.h"
#include "resources_internal.h"
#include "contentcensus.h"

#include <Tempest/Log>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Tempest;
using ResourcesInternal::censusSourcePath;
using ResourcesInternal::scriptDirectory;

namespace {
// Registered on construction: a program that links Gothic's loaders of these records also reads and checks
// their indexes on the step where Resources selects the base.
class BaseScripts final : public GameBaseContent {
  public:
    void index(const std::filesystem::path& root) override {
      basePrograms = ProgramData::readIndex(root/"programs");
      baseOutputUnits = OutputUnits::readIndex(root/"outputunits");
      }

    void validate(const Files& validationArchives, const LooseCheck& validateLoose) const override {
      if(basePrograms.archives!=validationArchives)
        throw std::runtime_error("selected base archives differ from program base index: "+basePrograms.dir.string());
      validateLoose(basePrograms.looseFiles,"program");
      Log::i("base: ",basePrograms.programs.size()," programs from \"",basePrograms.dir.string(),"\"");
      if(baseOutputUnits.archives!=validationArchives)
        throw std::runtime_error("selected base archives differ from output units base index: "+baseOutputUnits.dir.string());
      validateLoose(baseOutputUnits.looseFiles,"output units");
      Log::i("base: ",baseOutputUnits.records.size()," output units records from \"",baseOutputUnits.dir.string(),"\"");
      }

    int language() const override {
      for(int language=-1;language<8;++language) {
        const bool program=std::any_of(basePrograms.selections.begin(),basePrograms.selections.end(),
                                      [&](const auto& row) {
          return row.language==language && row.request=="GOTHIC.DAT" && row.source &&
                 basePrograms.sources.at(*row.source).skipped.empty();
          });
        const bool outputUnits=std::any_of(baseOutputUnits.selections.begin(),baseOutputUnits.selections.end(),
                                          [&](const auto& row) {
          return row.language==language && row.source &&
                 baseOutputUnits.sources.at(*row.source).skipped.empty();
          });
        if(program && outputUnits) return language;
        }
      return -1;
      }

    ProgramData::Index basePrograms;
    OutputUnits::Index baseOutputUnits;
  };

BaseScripts scripts;
}

const ProgramData::Index& baseProgramIndex() {
  return scripts.basePrograms;
  }

const OutputUnits::Index& baseOutputUnitsIndex() {
  return scripts.baseOutputUnits;
  }

bool loadBaseOutputUnits(std::string_view request, int language,
                         std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks) {
  try {
    std::string_view branch;
    if(scripts.baseOutputUnits.findVfs(request,&branch)!=nullptr)
      return loadBaseOutputUnits(request,language,branch,blocks);
    const std::string name(request.substr(0,255));
    const OutputUnits::Source* source=scripts.baseOutputUnits.findHost("_work/Data/Scripts/content/CUTSCENE/"+name);
    branch="host_ordinary";
    if(source==nullptr && language>=0) {
      source=scripts.baseOutputUnits.findHost("_work/Data/"+std::string(scriptDirectory(language))+"/"+name);
      branch="host_localized";
      }
    if(source==nullptr) return false;
    return loadBaseOutputUnits(request,language,branch,blocks,std::filesystem::path(source->path).u16string());
    }
  catch(const BaseStore::Error&) { throw; }
  catch(const std::exception& error) { throw BaseStore::Error("output units "+std::string(request)+": "+error.what()); }
  }

bool loadBaseOutputUnits(std::string_view request, int language, std::string_view branch,
                         std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks,
                         std::u16string_view hostPath) {
  try {
    const auto& index = scripts.baseOutputUnits;
    const OutputUnits::Source* source = nullptr;
    if(hostPath.empty()) {
      std::string_view selectedBranch;
      source = index.findVfs(request,&selectedBranch);
      if(source==nullptr || selectedBranch!=branch)
        throw std::runtime_error("selected VFS source missing from output units base index");
      }
    else {
      source = index.findHost(censusSourcePath(std::filesystem::path(hostPath)));
      if(source==nullptr)
        throw std::runtime_error("selected host source missing from output units base index");
      if(!source->skipped.empty())
        return false;
      }
    if(!source->skipped.empty())
      throw std::runtime_error("selected VFS output units source skipped: "+source->skipped);
    const auto file = index.path(*source);
    std::string actualSha256;
    auto library = OutputUnits::readLibrary(file,&actualSha256);
    if(actualSha256!=source->recordSha256)
      throw std::runtime_error("output units record identity differs from base index");
    auto loaded = OutputUnits::sortedBlocks(library);
    blocks = std::move(loaded);
    if(auto* census = Resources::contentCensus()) {
      const auto record = "base/outputunits/"+file.filename().generic_string();
      census->replaced(request,record);
      census->outputUnits("GameScript::loadDialogOU",request,language,branch,record,actualSha256,
                          {source->origin,source->path,source->archive,source->sha256,source->size});
      }
    return true;
    }
  catch(const std::exception& error) {
    if(auto* census = Resources::contentCensus()) census->missing(request,error.what());
    throw BaseStore::Error("output units "+std::string(request)+": "+error.what());
    }
  }
