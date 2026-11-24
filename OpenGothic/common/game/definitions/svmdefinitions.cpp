#include "svmdefinitions.h"

#include "daedalus/vm.h"
#include "gameplaydefinitions.h"
#include "utils/string_frm.h"

#include <cctype>

SvmDefinitions::SvmDefinitions(daedalus::DaedalusVm& vm) : vm(&vm) {
  }

SvmDefinitions::SvmDefinitions(std::shared_ptr<const GameplayDefinitions> records) : records(std::move(records)) {
  }

SvmDefinitions::~SvmDefinitions() {
  }

std::string_view SvmDefinitions::find(std::string_view speech, int intId) {
  if(speech.empty() || speech[0]!='$' || intId<0)
    return "";

  string_frm member("C_SVM.",speech.substr(1));
  if(records) {
    for(auto& ch : member)
      ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return records->speech(intId,member);
    }

  const auto id = size_t(intId);
  if(svm.size()<=id)
    svm.resize(id+1);
  if(svm[id]==nullptr) {
    const string_frm name("SVM_",intId);
    auto* symbol = vm->find_symbol_by_name(name);
    if(symbol==nullptr)
      return "";
    svm[id] = vm->init_opaque_instance(symbol);
    }

  auto* field = vm->find_symbol_by_name(member);
  if(field==nullptr)
    return "";
  return field->get_string(0,svm[id].get());
  }
