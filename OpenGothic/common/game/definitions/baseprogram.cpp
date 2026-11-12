#include "baseprogram.h"

#include "resources.h"
#include "resources_internal.h"
#include "contentcensus.h"
#include "daedalus/script.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

using ResourcesInternal::censusSourcePath;
using ResourcesInternal::scriptDirectory;

daedalus::DaedalusScript loadBaseProgram(std::string_view request, int language) {
  try {
    std::string_view branch;
    if(Resources::baseProgramIndex().findVfs(request,&branch)!=nullptr)
      return loadBaseProgram(request,language,branch);
    const std::string name(request.substr(0,255));
    std::string path="_work/Data/Scripts/_compiled/"+name;
    const ProgramData::Source* source=Resources::baseProgramIndex().findHost(path);
    branch="host_ordinary";
    if(source==nullptr && language>=0) {
      path="_work/Data/"+std::string(scriptDirectory(language))+"/_compiled/"+name;
      source=Resources::baseProgramIndex().findHost(path);
      branch="host_localized_full";
      }
    if(source==nullptr && language>=0) {
      const auto separator=request.find_last_of("/\\");
      const auto basename=separator==std::string_view::npos ? request : request.substr(separator+1);
      path="_work/Data/"+std::string(scriptDirectory(language))+"/_compiled/"+std::string(basename.substr(0,255));
      source=Resources::baseProgramIndex().findHost(path);
      branch="host_localized_basename";
      }
    if(source==nullptr)
      throw std::runtime_error("no source in program base index");
    return loadBaseProgram(request,language,branch,std::filesystem::path(source->path).u16string());
    }
  catch(const BaseStore::Error&) { throw; }
  catch(const std::exception& error) { throw BaseStore::Error("program "+std::string(request)+": "+error.what()); }
  }

daedalus::DaedalusScript loadBaseProgram(std::string_view request, int language,
                                       std::string_view branch, std::u16string_view hostPath) {
  try {
    const auto& index = Resources::baseProgramIndex();
    const ProgramData::Source* source = nullptr;
    if(hostPath.empty()) {
      std::string_view selectedBranch;
      source = index.findVfs(request,&selectedBranch);
      if(source==nullptr || selectedBranch!=branch)
        throw std::runtime_error("selected VFS source missing from program base index");
      }
    else {
      source = index.findHost(censusSourcePath(std::filesystem::path(hostPath)));
      if(source==nullptr)
        throw std::runtime_error("selected host source missing from program base index");
      }
    const auto file = index.path(*source);
    std::string actualSha256;
    auto program = ProgramData::readProgram(file,&actualSha256);
    if(actualSha256!=source->recordSha256)
      throw std::runtime_error("program record identity differs from base index");
    auto script = daedalus::DaedalusScript::from_program(std::move(program));
    if(Resources::contentCensus()) {
      const auto record = "base/programs/"+file.filename().generic_string();
      Resources::contentCensus()->replaced(request,record);
      Resources::contentCensus()->program("Gothic::loadScript",request,language,branch,record,actualSha256,*source);
      }
    return script;
    }
  catch(const std::exception& error) {
    if(Resources::contentCensus()) Resources::contentCensus()->missing(request,error.what());
    throw BaseStore::Error("program "+std::string(request)+": "+error.what());
    }
  }
