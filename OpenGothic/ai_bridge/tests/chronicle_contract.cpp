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

  const auto emptyCard = parseFateCard(R"({"type":"fate_card","req":"empty"})");
  require(!emptyCard.off && emptyCard.title.empty() && emptyCard.text.empty() && emptyCard.feats.empty(),
          "empty card fields and the protocol's off default are preserved");
  require(emptyCard.chapter==1 && emptyCard.day==0 && emptyCard.count==0,
          "empty card dates and count use defaults");
  require(parseFateCard(R"({"type":"fate_card","off":true})").off,
          "disabled card is parsed");

  const auto card = parseFateCard(R"({
    "type":"fate_card","req":"fate_card:1","title":"Твое слово","text":"Герой прошёл свой путь.",
    "chapter":6,"day":42,"count":7,"feats":[
      {"id":"first","title":"Твоё слово","text":"Герой сдержал обещание.","category":"word",
       "chapter":2,"day":3,"engraving":"/tmp/летопись/engraving.jpg","icon":"/tmp/летопись/icon.png",
       "epilogue":"Позже его узнали в городе."},
      {"id":"second","title":"Второй шаг","text":"Он продолжил путь.","category":"survival",
       "chapter":3,"day":15,"engraving":"","icon":"","epilogue":""},
      {"id":"third","title":"Последний шаг","text":"Он покинул эти земли.","category":"oath",
       "chapter":6,"day":42,"engraving":"","icon":"","epilogue":""}]})");
  require(!card.off && card.title==std::string("\xD2\xE2\xEE\xE5 \xF1\xEB\xEE\xE2\xEE"),
          "card title uses Windows-1251");
  require(TextCodec::toUtf8(card.text)=="Герой прошел свой путь.","card text uses the game codepage");
  require(card.chapter==6 && card.day==42 && card.count==7,"card dates and full feat count are parsed");
  require(card.feats.size()==3,"three best feats are parsed");
  require(card.feats[0].id=="first" && card.feats[1].id=="second" && card.feats[2].id=="third",
          "best feats retain chronicle order");
  require(card.feats[0].title==std::string("\xD2\xE2\xEE\xE5 \xF1\xEB\xEE\xE2\xEE") &&
          TextCodec::toUtf8(card.feats[0].text)=="Герой сдержал обещание." &&
          TextCodec::toUtf8(card.feats[0].epilogue)=="Позже его узнали в городе.",
          "best feat display text uses the game codepage");
  require(card.feats[0].engraving=="/tmp/летопись/engraving.jpg" &&
          card.feats[0].icon=="/tmp/летопись/icon.png","best feat paths retain UTF-8");
  require(card.feats[1].engraving.empty() && card.feats[1].icon.empty() && card.feats[1].epilogue.empty(),
          "empty best feat fields remain empty");
  std::printf("Chronicle contract: %d checks passed\n",checks);
  }
