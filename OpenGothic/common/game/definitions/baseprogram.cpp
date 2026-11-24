#include "baseprogram.h"
#include "basestore.h"
#include "programdata.h"
#include "resources_internal.h"

#include <stdexcept>
#include <string>

using ResourcesInternal::scriptDirectory;

BaseProgramSelection selectBaseProgram(const ProgramData::Index& index, std::string_view request, int language) {
  try {
    std::string_view branch;
    if(const auto* source=index.findVfs(request,&branch))
      return {*source,branch};
    const std::string name(request.substr(0,255));
    std::string path="_work/Data/Scripts/_compiled/"+name;
    const ProgramData::Source* source=index.findHost(path);
    branch="host_ordinary";
    if(source==nullptr && language>=0) {
      path="_work/Data/"+std::string(scriptDirectory(language))+"/_compiled/"+name;
      source=index.findHost(path);
      branch="host_localized_full";
      }
    if(source==nullptr && language>=0) {
      const auto separator=request.find_last_of("/\\");
      const auto basename=separator==std::string_view::npos ? request : request.substr(separator+1);
      path="_work/Data/"+std::string(scriptDirectory(language))+"/_compiled/"+std::string(basename.substr(0,255));
      source=index.findHost(path);
      branch="host_localized_basename";
      }
    if(source==nullptr)
      throw std::runtime_error("no source in program base index");
    return {*source,branch};
    }
  catch(const BaseStore::Error&) { throw; }
  catch(const std::exception& error) { throw BaseStore::Error("program "+std::string(request)+": "+error.what()); }
  }
