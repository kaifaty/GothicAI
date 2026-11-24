#include "oumessages.h"

#include "basestore.h"
#include "game/definitions/runtimebindings.h"
#include "game/definitions/stockstoryprograms.h"

#include <algorithm>
#include <cstdio>
#include <rapidjson/document.h>
#include <stdexcept>

namespace {

int exportMessages(const std::filesystem::path& input) {
  // The server passes the selected base. Selecting it directly takes a base lease
  // without consulting, preparing, or mounting any original installation.
  const auto base = std::filesystem::canonical(input);
  BaseStore::Profile profile;
  if(BaseStore::isNativeProjection(base))
    profile = {.stockProgram=StockStoryPrograms::stockProgram,.projectedOnly=true,
               .validateProgramMetadata=RuntimeBindings::validate};
  const auto selection = BaseStore::resolve(base,{},[](std::string_view) {},{},profile,nullptr,base);
  const auto identity = BaseImport::readIdentity(selection.path);
  rapidjson::Document metadata;
  metadata.Parse(identity.json.data(),identity.json.size());
  BaseImport::Files archives;
  for(const auto& archive:metadata["archives"].GetArray())
    archives.emplace_back(archive["name"].GetString(),archive["size"].GetUint64());
  std::sort(archives.begin(),archives.end());
  return storygraph::printOuMessages(storygraph::readOuIndex(selection.path,archives));
  }

}

#ifdef _MSC_VER
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if(argc!=3) {
    std::fputs("usage: storygraph-ou <prepared base> --ou-messages\n",stderr);
    return 2;
    }
  try {
    if(std::filesystem::path(argv[2])=="--ou-messages")
      return exportMessages(argv[1]);
    if(std::filesystem::path(argv[1])=="--ou-messages")
      return exportMessages(argv[2]);
    std::fputs("usage: storygraph-ou <prepared base> --ou-messages\n",stderr);
    return 2;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"storygraph: %s\n",error.what());
    return 1;
    }
  }
