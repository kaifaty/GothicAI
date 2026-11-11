#include "texturedump.h"

#include <Tempest/Log>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "graphics/worldview.h"
#include "resources.h"
#include "world/world.h"

void TextureDump::write(std::string_view stage, World& world) {
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
  if(auto* view=world.view())
    view->dumpTextureOwners(root/(name+"-owners.csv"));
  Tempest::Log::i("texture dump: ",name," in \"",dir,"\"");
  }
