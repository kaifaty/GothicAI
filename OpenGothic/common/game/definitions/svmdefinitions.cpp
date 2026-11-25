#include "svmdefinitions.h"

#include "gameplaydefinitions.h"
#include "utils/string_frm.h"

#include <cctype>

SvmDefinitions::SvmDefinitions(std::shared_ptr<const GameplayDefinitions> records) : records(std::move(records)) {
  }

SvmDefinitions::~SvmDefinitions() {
  }

std::string_view SvmDefinitions::find(std::string_view speech, int intId) {
  if(speech.empty() || speech[0]!='$' || intId<0)
    return "";

  string_frm member("C_SVM.",speech.substr(1));
  for(auto& ch : member)
    ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return records->speech(intId,member);
  }
