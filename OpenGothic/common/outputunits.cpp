#include "outputunits.h"
#include "gothicglb.h"
#include "sha256.h"

#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <set>
#include <stdexcept>

namespace OutputUnits {
namespace {
namespace fs = std::filesystem;
using Json = rapidjson::Value;

void require(bool ok, const std::string& reason) {
  if(!ok)
    throw std::runtime_error(reason);
  }

std::string upperAscii(std::string_view s) {
  std::string out(s);
  for(auto& c:out)
    if(c>='a' && c<='z')
      c = char(c-'a'+'A');
  return out;
  }

// zenkit::VfsNode::child trims the requested name with std::isspace.
std::string_view trimTrailing(std::string_view s) {
  while(!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
  }

const Json& member(const Json& obj, const char* name) {
  require(obj.HasMember(name),std::string("missing field ")+name);
  return obj[name];
  }

uint64_t unsignedValue(const Json& value, const char* name) {
  require(value.IsUint64(),std::string(name)+" must be an unsigned integer");
  return value.GetUint64();
  }

void exactMembers(const Json& obj, const char* what, std::initializer_list<const char*> names) {
  require(obj.IsObject(),std::string(what)+" must be an object");
  for(const char* name:names)
    member(obj,name);
  require(obj.MemberCount()==names.size(),std::string(what)+" has unknown members");
  }

std::string optionalString(const Json& v, const char* name) {
  return v.IsNull() ? std::string() : gothicglb::jsonToString(v,name);
  }

std::optional<size_t> sourceRef(const Json& v, size_t sources, const char* name) {
  if(v.IsNull())
    return std::nullopt;
  const auto i = unsignedValue(v,name);
  require(i<sources,std::string(name)+" out of range");
  return size_t(i);
  }

// Host paths stay inside the installation, as Resources::BaseIndex::load requires of loose files.
std::string relativePath(const Json& v, const char* name) {
  auto path = gothicglb::jsonToString(v,name);
  const fs::path p(path);
  require(!path.empty() && !p.is_absolute() && path.find('\0')==std::string::npos &&
          std::none_of(p.begin(),p.end(),[](const fs::path& part) { return part==".."; }),
          std::string("invalid relative ")+name);
  return path;
  }

bool isHex64(const std::string& s) {
  return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;
  }

void visitMessages(const asset::CutsceneBlock& block, std::set<const asset::Object*>& seen,
                   std::vector<std::shared_ptr<asset::ConversationMessageEvent>>& out) {
  if(!seen.insert(&block).second)
    return;
  if(std::holds_alternative<std::shared_ptr<asset::CutsceneBlock>>(block.block)) {
    if(const auto& nested = std::get<std::shared_ptr<asset::CutsceneBlock>>(block.block))
      visitMessages(*nested,seen,out);
    return;
    }
  const auto& atomic = std::get<std::shared_ptr<asset::CutsceneAtomicBlock>>(block.block);
  if(atomic==nullptr || !seen.insert(atomic.get()).second)
    return;
  if(atomic->message!=nullptr && seen.insert(atomic->message.get()).second)
    out.push_back(atomic->message);
  }
}

const Source* Index::findVfs(std::string_view request, std::string_view* step) const {
  if(auto it=vfs.find(upperAscii(trimTrailing(request))); it!=vfs.end()) {
    if(step!=nullptr) *step = "vfs_find_request";
    return &sources[it->second];
    }
  const auto segment = request.find_last_of("\\/");
  if(segment==std::string_view::npos)
    return nullptr;
  if(auto it=vfs.find(upperAscii(trimTrailing(request.substr(segment+1)))); it!=vfs.end()) {
    if(step!=nullptr) *step = "vfs_find_basename";
    return &sources[it->second];
    }
  return nullptr;
  }

const Source* Index::findHost(std::string_view relativePath) const {
  const auto key = fs::path(std::string(relativePath)).lexically_normal().generic_string();
  if(auto it=host.find(key); it!=host.end())
    return &sources[it->second];
  const auto folded = upperAscii(key);
  const Source* found = nullptr;
  for(const auto& [path,source]:host)
    if(upperAscii(path)==folded) {
      if(found!=nullptr)
        throw std::runtime_error("output units: "+key+": ambiguous host path (differs only in case)");
      found = &sources[source];
      }
  return found;
  }

const Source* Index::serverOuPy() const {
  return serverOuPySource ? &sources[*serverOuPySource] : nullptr;
  }

fs::path Index::path(const Source& source) const {
  if(source.record.empty())
    throw std::runtime_error("output units: "+source.path+": not an importable cutscene library ("+source.skipped+")");
  return dir/(source.record+".json");
  }

Index readIndex(const fs::path& dir) {
  const auto file = dir/"outputunits.json";
  try {
    const auto bytes = gothicglb::readFile(file);
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(!doc.HasParseError(),rapidjson::GetParseError_En(doc.GetParseError()));
    exactMembers(doc,"index",{"format","version","archives","loose_files","records","sources","vfs","host",
                              "selections","server"});
    require(gothicglb::jsonToString(doc["format"],"format")=="gothic-outputunits","unsupported format");
    require(doc["version"].IsInt() && doc["version"].GetInt()==1,"unsupported version");

    Index index;
    index.dir = dir;
    require(doc["archives"].IsArray(),"archives must be an array");
    for(const auto& a:doc["archives"].GetArray()) {
      exactMembers(a,"archive",{"name","size"});
      index.archives.emplace_back(gothicglb::jsonToString(a["name"],"name"),unsignedValue(a["size"],"size"));
      }
    require(doc["loose_files"].IsArray(),"loose_files must be an array");
    for(const auto& l:doc["loose_files"].GetArray()) {
      exactMembers(l,"loose file",{"path","size"});
      index.looseFiles.emplace_back(relativePath(l["path"],"path"),unsignedValue(l["size"],"size"));
      }
    require(doc["records"].IsObject(),"records must be an object");
    for(const auto& r:doc["records"].GetObject()) {
      const auto name = gothicglb::jsonToString(r.name,"record name");
      const auto identity = gothicglb::jsonToString(r.value,"record identity");
      require(isHex64(name) && identity==name,"invalid record name or identity");
      index.records[name] = identity;
      }
    require(doc["sources"].IsArray(),"sources must be an array");
    for(const auto& s:doc["sources"].GetArray()) {
      exactMembers(s,"source",{"origin","path","archive","size","sha256","record","skipped"});
      Source src;
      src.origin  = gothicglb::jsonToString(s["origin"],"origin");
      src.path    = gothicglb::jsonToString(s["path"],"path");
      src.archive = optionalString(s["archive"],"archive");
      src.size    = unsignedValue(s["size"],"size");
      src.sha256  = gothicglb::jsonToString(s["sha256"],"sha256");
      src.record  = optionalString(s["record"],"record");
      src.skipped = optionalString(s["skipped"],"skipped");
      require(src.origin=="host" || src.origin=="archive","invalid origin");
      if(src.origin=="host")
        src.path = relativePath(s["path"],"path");
      require((src.origin=="archive")==!src.archive.empty(),"archive name must match the origin");
      require(isHex64(src.sha256),"invalid source sha256");
      require(src.record.empty()!=src.skipped.empty(),"a source is either a library or skipped");
      if(!src.record.empty()) {
        const auto it = index.records.find(src.record);
        require(it!=index.records.end(),"source references an unknown record");
        src.recordSha256 = it->second;
        }
      index.sources.push_back(std::move(src));
      }
    for(const char* table:{"vfs","host"}) {
      require(doc[table].IsObject(),std::string(table)+" must be an object");
      auto& out = std::string_view(table)=="vfs" ? index.vfs : index.host;
      for(const auto& e:doc[table].GetObject()) {
        const auto source = sourceRef(e.value,index.sources.size(),table);
        require(source.has_value(),std::string(table)+" entry without a source");
        if(&out==&index.host) {
          require(index.sources[*source].origin=="host","host entry must name a host source");
          out[relativePath(e.name,table)] = *source;
          }
        else {
          out[gothicglb::jsonToString(e.name,table)] = *source;
          }
        }
      }
    require(doc["selections"].IsArray(),"selections must be an array");
    for(const auto& s:doc["selections"].GetArray()) {
      exactMembers(s,"selection",{"language","candidate","step","path","source"});
      Selection sel;
      require(s["language"].IsInt(),"language must be an integer");
      sel.language  = s["language"].GetInt();
      sel.candidate = gothicglb::jsonToString(s["candidate"],"candidate");
      sel.step      = gothicglb::jsonToString(s["step"],"step");
      sel.path      = optionalString(s["path"],"path");
      sel.source    = sourceRef(s["source"],index.sources.size(),"source");
      index.selections.push_back(std::move(sel));
      }
    exactMembers(doc["server"],"server",{"ou_py"});
    index.serverOuPySource = sourceRef(doc["server"]["ou_py"],index.sources.size(),"ou_py");
    if(index.serverOuPySource)
      require(index.sources[*index.serverOuPySource].origin=="host","ou_py must name a host source");
    return index;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("output units: "+file.string()+": "+e.what());
    }
  }

WorldData::ZenFile readLibrary(const fs::path& file, std::string* actualSha256) {
  try {
    const auto bytes = gothicglb::readFile(file);
    auto library = WorldData::parseZen(bytes,file.string());
    require(library.kind=="cutscene_library","record kind is "+library.kind+", not cutscene_library");
    if(actualSha256!=nullptr)
      *actualSha256 = Sha256::hash(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    return library;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("output units: "+file.string()+": "+e.what());
    }
  }

std::string writeLibrary(const WorldData::ZenFile& library, const fs::path& file) {
  require(library.kind=="cutscene_library","output units: writing a "+library.kind+" record as a cutscene library");
  WorldData::writeZen(library,file);
  const auto bytes = gothicglb::readFile(file);
  return Sha256::hash(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
  }

std::vector<std::shared_ptr<asset::CutsceneBlock>> sortedBlocks(const WorldData::ZenFile& library) {
  auto sorted = library.blocks;
  // zenkit::CutsceneLibrary::load: the same comparator over the same archive order.
  std::sort(sorted.begin(),sorted.end(),[](auto const& a, auto const& b) {
    return a->name < b->name;
    });
  return sorted;
  }

std::shared_ptr<asset::CutsceneBlock> blockByName(const std::vector<std::shared_ptr<asset::CutsceneBlock>>& sorted,
                                                  std::string_view name) {
  // zenkit::CutsceneLibrary::block_by_name, including its missing equality check.
  auto result = std::lower_bound(sorted.begin(),sorted.end(),name,[](auto const& it, auto n) {
    return it->name < n;
    });
  if(result==sorted.end())
    return nullptr;
  return *result;
  }

std::shared_ptr<asset::ConversationMessageEvent> message(const asset::CutsceneBlock& block) {
  // zenkit::CutsceneBlock::get_message.
  const auto* cur = &block;
  while(std::holds_alternative<std::shared_ptr<asset::CutsceneBlock>>(cur->block)) {
    cur = std::get<std::shared_ptr<asset::CutsceneBlock>>(cur->block).get();
    if(cur==nullptr)
      return nullptr;
    }
  return std::get<std::shared_ptr<asset::CutsceneAtomicBlock>>(cur->block)->message;
  }

std::vector<std::shared_ptr<asset::ConversationMessageEvent>> messagesInArchiveOrder(const WorldData::ZenFile& library) {
  std::vector<std::shared_ptr<asset::ConversationMessageEvent>> out;
  std::set<const asset::Object*> seen;
  for(const auto& root:library.blocks)
    if(root!=nullptr)
      visitMessages(*root,seen,out);
  return out;
  }

}
