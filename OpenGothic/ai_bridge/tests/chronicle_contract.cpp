#include "chronicledata.h"
#include "textcodec.h"

#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }
}

int main() {
  const auto empty = parseChronicle(R"({"type":"chronicle","req":"empty","fate":[],"others":[]})");
  require(empty.fate.empty() && empty.others.empty(),"empty chronicle has no entries or forks");

  const auto data = parseChronicle(R"({
    "type":"chronicle", "req":"chronicle:1",
    "fate":[{"id":"hero-1","title":"Твоё слово","text":"Герой сдержал обещание.",
             "category":"mercy","chapter":3,"day":12,"engraving":"/tmp/летопись/engraving.jpg",
             "icon":"/tmp/летопись/icon.png","epilogue":"Позже его узнали в городе."}],
    "others":[
      {"chapter":2,"day":7,"feats":[
        {"id":"other-1","title":"Другая судьба","text":"Он выбрал иную дорогу.",
         "category":"choice","chapter":2,"day":9,"engraving":"","icon":"","epilogue":""},
        {"id":"other-2","title":"Тайна","text":"Свидетель запомнил героя.",
         "category":"mercy","chapter":3,"day":10,"engraving":"","icon":"","epilogue":"О нём заговорили."}]},
      {"chapter":1,"day":2,"feats":[
        {"id":"early","title":"Ранняя дорога","text":"Начало пути.","category":"choice",
         "chapter":1,"day":3,"engraving":"","icon":"","epilogue":""}]}
    ]})");
  require(data.fate.size()==1,"current fate is parsed");
  const auto& feat = data.fate[0];
  require(feat.id=="hero-1" && feat.category=="mercy","feat identity and category are preserved");
  require(feat.title==std::string("\xD2\xE2\xEE\xE5 \xF1\xEB\xEE\xE2\xEE"),
          "Cyrillic title uses Windows-1251 and the game's yo fallback");
  require(TextCodec::toUtf8(feat.text)=="Герой сдержал обещание.","Cyrillic text uses the game codepage");
  require(feat.chapter==3 && feat.day==12,"feat chapter and day are parsed");
  require(feat.engraving=="/tmp/летопись/engraving.jpg" && feat.icon=="/tmp/летопись/icon.png",
          "asset paths retain their UTF-8 bytes");
  require(TextCodec::toUtf8(feat.epilogue)=="Позже его узнали в городе.",
          "shown epilogue is converted to the game codepage");
  require(data.others.size()==2,"both forks are parsed");
  require(data.others[0].chapter==2 && data.others[0].day==7 &&
          data.others[1].chapter==1 && data.others[1].day==2,"fork dates retain server order");
  require(data.others[0].feats.size()==2 && data.others[1].feats.size()==1,"entries stay in their forks");
  require(data.others[0].feats[0].id=="other-1" && data.others[0].feats[1].id=="other-2" &&
          data.others[1].feats[0].id=="early","entry order is preserved");
  require(data.others[0].feats[0].epilogue.empty(),"an unshown epilogue remains empty");
  require(TextCodec::toUtf8(data.others[0].feats[1].epilogue)=="О нем заговорили.",
          "other fates receive converted epilogues");
  require(data.others[0].feats[0].engraving.empty() && data.others[0].feats[0].icon.empty(),
          "missing pictures remain empty");
  std::printf("Chronicle contract: %d checks passed\n",checks);
  }
