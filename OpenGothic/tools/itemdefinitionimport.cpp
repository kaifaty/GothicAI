#include "itemdefinitionimport.h"

#include "game/itemdefinitionresolve.h"
#include "itemloadprobe.h"

#include <cstdlib>
#include <cstring>

ItemDefinitionRuntime::Definitions ItemDefinitionRuntime::resolve(const ItemDefinitions::Library& library,
                                                                  const daedalus::DaedalusScript& script) {
  const auto lookup=[&](std::string_view name) -> std::optional<uint32_t> {
    const auto* symbol=script.find_symbol_by_name(name);
    return symbol!=nullptr ? std::optional(symbol->index()) : std::nullopt;
    };
  auto definitions=resolveRecords(library,lookup,[&](std::string_view,std::string_view name) { return lookup(name); });
  for(const auto& [name,document]:library.documents) {
    const auto* root=script.find_symbol_by_name(name);
    const auto* handle=script.find_symbol_by_name(document.handle);
    using Type=daedalus::DaedalusDataType;
    if(root==nullptr || !definitions.itemUsePrograms.contains(root->index()) || handle==nullptr ||
       handle->name()!=root->name()+".NDOCID" || handle->type()!=Type::INT || handle->count()!=1 ||
       handle->is_const() || handle->is_member())
      throw std::runtime_error("invalid item document "+name);
    Document resolved{handle->index(),document.map,{}};
    for(const auto& call:document.calls) {
      const auto* external=script.find_symbol_by_name(call.function);
      if(external==nullptr || !external->is_external() || external->count()!=call.arguments.size()+1 || external->has_return())
        throw std::runtime_error("invalid document action "+call.function);
      resolved.calls.push_back({call.function,call.arguments});
      }
    definitions.documents.emplace(root->index(),std::move(resolved));
    }
  return definitions;
  }

ItemDefinitionRuntime::Definitions ItemDefinitionRuntime::load(const std::filesystem::path& base,
                                                               const daedalus::DaedalusScript& program,
                                                               std::string_view sha256) {
  const auto* enabled=std::getenv("OPENGOTHIC_ITEM_RECORDS");
  if(enabled==nullptr || std::strcmp(enabled,"0")!=0) {
    return resolve(ItemDefinitions::read(base,sha256),program);
    }
  return {};
  }

bool ItemDefinitionRuntime::initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::IItem>& item,
                                       daedalus::DaedalusSymbol* symbol, const Definitions& definitions) {
  if(symbol!=nullptr) {
    const auto found=definitions.items.find(symbol->index());
    if(found!=definitions.items.end()) {
      // allocate_instance binds the symbol and type. Constructor calls restore SELF,
      // context, PC and stack; pure item constructors leave no other VM state behind.
      vm.allocate_instance(item,symbol);
      static_cast<ItemRecord&>(*item)=found->second;
      return true;
      }
    if(definitions.brokenEye==symbol->index()) {
      // This constructor samples the current language/story setting, including on each recreation.
      // Its two generated literals are bound by the verified stock program fingerprint.
      const auto get=[&](std::string_view name) { return vm.find_symbol_by_name(name); };
      vm.allocate_instance(item,symbol);
      item->name=get("$15687")->get_string();
      item->main_flag=get("ITEM_KAT_MAGIC")->get_int();
      item->flags=ItemFlag(get("ITEM_AMULET")->get_int() | get("ITEM_MISSION")->get_int());
      item->value=0;
      item->visual=get("$15688")->get_string();
      item->material=get("MAT_STONE")->get_int();
      item->description=item->name;
      item->on_equip=int32_t(get("EQUIP_ITMI_INNOSEYE_BROKEN_MIS")->index());
      item->text[0]=get("TEXT_INNOSEYE_SETTING")->get_string();
      item->text[1]=get("TEXT_INNOSEYE_GEM")->get_string();
      item->inv_zbias=get("INVCAM_ENTF_AMULETTE_STANDARD")->get_int();
      return true;
      }
    }
  LoadProbe::Items::Constructor probe;
  vm.init_instance(item,symbol);
  return false;
  }
