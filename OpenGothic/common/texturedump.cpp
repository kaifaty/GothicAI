#include "texturedump.h"

#include <Tempest/Log>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "graphics/worldview.h"
#include "resources.h"

void TextureDump::write(std::string_view stage, WorldView* view) {
  const char* dir = std::getenv("OPENGOTHIC_TEXTURE_DUMP");
  if(dir==nullptr || dir[0]=='\0')
    return;
  std::string name(stage);
  for(auto& c:name)
    if(c=='/' || c=='\\' || c==' ')
      c = '-';
  std::error_code error;
  std::filesystem::create_directories(dir,error);
  const std::filesystem::path root(dir);
  Resources::dumpTextures(root/(name+"-textures.csv"));
  if(view!=nullptr)
    view->dumpTextureOwners(root/(name+"-owners.csv"));
  Tempest::Log::i("texture dump: ",name," in \"",dir,"\"");
  }
