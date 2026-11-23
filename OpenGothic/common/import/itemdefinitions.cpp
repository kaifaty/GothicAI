#include "baseimport.h"
#include "definitionjson.h"
#include "npcsnapshot.h"
#include "programdata.h"
#include "asset/itemdocuments.h"
#include "asset/npcdefinitions.h"
#include "daedalus/vm.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
using namespace daedalus;
using Op = DaedalusOpcode;
using Type = DaedalusDataType;
using DefinitionJson::put;
using DefinitionJson::text;
using DefinitionJson::require;
using DefinitionJson::Json;

struct StaticDocument {
  const DaedalusSymbol* handle;
  bool map;
  };

// A static document allocates one handle, writes only its own ID variable, calls
// document setters with literals, and shows the document before returning.
std::optional<StaticDocument> staticDocument(const DaedalusVm& vm,const DaedalusSymbol& fn) {
  if(fn.type()!=Type::FUNCTION || fn.count()!=0 || fn.has_return() || fn.is_external() || !fn.is_const() ||
     fn.address()>=vm.size())
    return {};
  auto pc=fn.address();
  const auto next=[&] {
    const auto op=vm.instruction_at(pc);
    pc+=op.size;
    return op;
    };
  const auto first=next();
  const auto* create=first.op==Op::BE ? vm.find_symbol_by_index(first.symbol) : nullptr;
  if(create==nullptr || (create->name()!="DOC_CREATE" && create->name()!="DOC_CREATEMAP"))
    return {};
  const auto target=next();
  const auto* handle=target.op==Op::PUSHV ? vm.find_symbol_by_index(target.symbol) : nullptr;
  if(handle==nullptr || handle->name()!=fn.name()+".NDOCID" || handle->type()!=Type::INT ||
     handle->count()!=1 || handle->is_member() || handle->is_const() || next().op!=Op::MOVI)
    return {};
  while(pc<vm.size()) {
    const auto op=next();
    if(op.op==Op::PUSHI || op.op==Op::NEGATE)
      continue;
    if(op.op==Op::PUSHV) {
      const auto* sym=vm.find_symbol_by_index(op.symbol);
      if(sym==handle || (sym!=nullptr && sym->is_const() && !sym->is_member() &&
                         (sym->type()==Type::INT || sym->type()==Type::STRING)))
        continue;
      }
    if(op.op==Op::BE) {
      const auto* target=vm.find_symbol_by_index(op.symbol);
      if(target!=nullptr && target->name()=="DOC_SHOW" && next().op==Op::RSR)
        return StaticDocument{handle,create->name()=="DOC_CREATEMAP"};
      if(target!=nullptr && !ItemDefinitions::documentArguments(target->name()).empty())
        continue;
      }
    return {};
    }
  return {};
  }

Json readDocumentCalls(DaedalusVm& vm,const DaedalusSymbol& fn,const StaticDocument& doc,
                       int32_t documentId,DefinitionJson::Allocator& allocator) {
  Json calls(rapidjson::kArrayType);
  vm.register_default_external_custom([&](DaedalusVm& vm,DaedalusSymbol& symbol) {
    const auto& name=symbol.name();
    if(name=="DOC_CREATE" || name=="DOC_CREATEMAP") {
      vm.push_int(documentId);
      return;
      }
    const auto types=ItemDefinitions::documentArguments(name);
    require(name=="DOC_SHOW" || !types.empty(),"unexpected document action "+name);
    std::vector<Json> values;
    for(auto it=types.rbegin();it!=types.rend();++it) {
      if(*it=='s') values.push_back(text(DefinitionJson::encode(vm.pop_string()),allocator));
      else values.emplace_back(vm.pop_int());
      }
    require(vm.pop_int()==documentId,"document action used a different handle in "+fn.name());
    if(name=="DOC_SHOW")
      return;
    Json call(rapidjson::kObjectType),arguments(rapidjson::kArrayType);
    for(auto it=values.rbegin();it!=values.rend();++it)
      arguments.PushBack(*it,allocator);
    put(call,"function",text(name,allocator),allocator);
    put(call,"arguments",std::move(arguments),allocator);
    calls.PushBack(call,allocator);
    });
  vm.unsafe_call(&fn);
  vm.register_default_external_custom({});
  require(doc.handle->get_int()==documentId,"document did not store its own handle in "+fn.name());
  return calls;
  }

size_t writeItemDocuments(const fs::path& items,const NpcSnapshot::Program& program) {
  const auto library=DefinitionJson::read(items);
  auto vm=DaedalusVm(DaedalusScript::from_program(program.data));
  std::set<std::string> roots;
  for(const auto& record:library["records"].GetArray()) {
    if(std::string_view(record["status"].GetString())!="captured")
      continue;
    const auto& callback=record["fields"]["on_state"][rapidjson::SizeType(0)];
    if(callback.IsString())
      roots.insert(DefinitionJson::string(callback));
    }
  rapidjson::Document out;
  auto& allocator=out.GetAllocator();
  out.SetObject();
  Json entries(rapidjson::kObjectType);
  for(const auto& name:roots) {
    const auto* fn=vm.find_symbol_by_name(name);
    if(fn==nullptr)
      continue;
    const auto doc=staticDocument(vm,*fn);
    if(!doc)
      continue;
    auto calls=readDocumentCalls(vm,*fn,*doc,17,allocator);
    const auto retry=readDocumentCalls(vm,*fn,*doc,931,allocator);
    require(calls==retry,"document values depend on the allocated handle in "+name);
    Json entry(rapidjson::kObjectType);
    put(entry,"handle",text(doc->handle->name(),allocator),allocator);
    put(entry,"map",Json(doc->map),allocator);
    put(entry,"calls",std::move(calls),allocator);
    entries.AddMember(text(name,allocator),entry,allocator);
    }
  const auto count=entries.MemberCount();
  put(out,"format",text(ItemDefinitions::DocumentFormat,allocator),allocator);
  put(out,"version",Json(ItemDefinitions::DocumentVersion),allocator);
  put(out,"program_sha256",text(program.sha256,allocator),allocator);
  put(out,"records",std::move(entries),allocator);
  DefinitionJson::write(items.parent_path()/"item-documents.json",out);
  return count;
  }
}

int BaseImport::convertItemDefinitions(const fs::path& base) {
  const auto start=std::chrono::steady_clock::now();
  const auto index=ProgramData::readIndex(base/"programs");
  size_t total=0,captured=0,programs=0,documents=0;
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
    const auto counts=NpcSnapshot::writeItems(program,file);
    total+=counts.total;
    captured+=counts.captured;
    if(NpcSnapshot::verifyItems(program,file)!=0)
      throw std::runtime_error("item definitions: constructor comparison failed: "+name);
    if(program.sha256==NpcDefinitions::TalentWriteProgram)
      documents+=writeItemDocuments(file,program);
    }
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"written and verified "<<programs<<" item definition programs";
  std::cout<<"; "<<total<<" items, "<<captured<<" captured, "<<(total-captured)<<" uncaptured";
  std::cout<<"; "<<documents<<" static item documents";
  std::cout<<"; 0 failed ("<<ms<<" ms)\n";
  return 0;
  }
