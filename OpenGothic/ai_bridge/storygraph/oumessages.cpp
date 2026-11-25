#include "oumessages.h"

#include "basestore.h"
#include "definitionjson.h"
#include "gothicglb.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

OutputUnits::Index storygraph::readOuIndex(const std::filesystem::path& base, const BaseImport::Files& archives) {
  auto index = OutputUnits::readIndex(base/"outputunits");
  if(index.archives!=archives)
    throw std::runtime_error("output units index archives differ from the selected base");
  return index;
  }

WorldData::ZenFile storygraph::readOuLibrary(const OutputUnits::Index& index, const OutputUnits::Source& source,
                                     std::string& actualSha) {
  auto library = OutputUnits::readLibrary(index.path(source),&actualSha);
  if(actualSha!=source.recordSha256)
    throw std::runtime_error("selected OU record identity differs from its index");
  return library;
  }

int storygraph::printOuMessages(const OutputUnits::Index& index) {
  std::string text = "{\"format\":\"gothic-ou-messages\",\"version\":1,\"source\":";
  const auto* source = index.serverOuPy();
  if(source==nullptr) {
    text += "null,\"native_sha256\":null,\"messages\":null}";
    } else {
    std::string actualSha;
    const auto library = readOuLibrary(index,*source,actualSha);
    text += "{\"path\":"+gothicglb::jsonString(source->path)+",\"sha256\":"+
            gothicglb::jsonString(source->sha256)+"},\"native_sha256\":"+
            gothicglb::jsonString(actualSha)+",\"messages\":[";
    bool first = true;
    for(const auto& message:OutputUnits::messagesInArchiveOrder(library)) {
      if(!first)
        text += ',';
      first = false;
      text += '['+gothicglb::jsonString(message->name)+','+gothicglb::jsonString(message->text)+']';
      }
    text += "]}";
    }
  text += '\n';
  if(std::fwrite(text.data(),1,text.size(),stdout)!=text.size() || std::fflush(stdout)!=0)
    throw std::runtime_error("cannot write OU messages to stdout");
  return 0;
  }

int storygraph::printPreparedOuMessages(const std::filesystem::path& base) {
  BaseImport::Identity identity;
  const auto selection = BaseStore::openPreparedBase(base,identity);
  const auto manifest = DefinitionJson::read(selection.path/"base-files.json");
  const auto requireBound = [&](const std::string& name) {
    const auto& files = manifest["files"];
    if(!std::ranges::any_of(files.GetArray(),[&](const auto& entry) { return entry["path"]==name.c_str(); }))
      throw std::runtime_error("prepared base manifest omits "+name);
    };
  requireBound("outputunits/outputunits.json");
  rapidjson::Document metadata;
  metadata.Parse(identity.json.data(),identity.json.size());
  BaseImport::Files archives;
  for(const auto& archive:metadata["archives"].GetArray())
    archives.emplace_back(archive["name"].GetString(),archive["size"].GetUint64());
  std::sort(archives.begin(),archives.end());
  const auto index = readOuIndex(selection.path,archives);
  if(const auto* source = index.serverOuPy())
    requireBound(index.path(*source).lexically_relative(selection.path).generic_string());
  return printOuMessages(index);
  }
