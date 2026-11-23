#include "game/healing.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  struct Buffer final {
    std::vector<char> bytes;
    size_t position = 0;

    void writeBytes(const void* data, size_t size) {
      const auto* first = static_cast<const char*>(data);
      bytes.insert(bytes.end(),first,first+size);
      }

    void readBytes(void* data, size_t size) {
      require(position+size<=bytes.size(),"serialized pool is complete");
      std::memcpy(data,bytes.data()+position,size);
      position += size;
      }

    template<class... Values>
    void write(const Values&... values) {
      (writeBytes(&values,sizeof(values)),...);
      }

    template<class... Values>
    void read(Values&... values) {
      (readBytes(&values,sizeof(values)),...);
      }
    };
  }

int main() {
  using namespace Healing;

  require(effectMoment(Category::Potion,true,true,true)==EffectMoment::Destroy,
          "enabled potion waits for ITEM_DESTROY");
  require(effectMoment(Category::Food,true,true,true)==EffectMoment::Destroy,
          "enabled food waits for ITEM_DESTROY");
  require(effectMoment(Category::Potion,true,true,false)==EffectMoment::Immediate,
          "item without animation executes immediately");
  require(effectMoment(Category::Potion,false,true,true)==EffectMoment::Immediate &&
          effectMoment(Category::Food,true,false,true)==EffectMoment::Immediate &&
          effectMoment(Category::Other,true,true,true)==EffectMoment::Immediate,
          "disabled flags and other items preserve immediate effects");

  std::array<Potion,3> potions{{{1,50,true},{2,70,true},{3,100,true}}};
  require(choosePotion(potions,99)==2,"hotkey chooses strongest available potion within missing HP");
  require(choosePotion(potions,30)==1,"hotkey chooses weakest available when every potion would overflow");
  require(choosePotion(potions,200)==3,"hotkey chooses strongest available for a large deficit");
  potions[1].available = false;
  require(choosePotion(potions,99)==1,"hotkey skips unavailable potions");
  require(choosePotion(potions,0)==NoPotion,"hotkey does nothing at full health");
  potions[0].available = false;
  potions[2].available = false;
  require(choosePotion(potions,100)==NoPotion,"hotkey without a matching potion does nothing");

  Pool pool;
  add(pool,3,0);
  int total = 0;
  for(int ms=1;ms<10000;++ms)
    total += advance(pool,1,100,100).hp;
  require(total==2 && pool.remaining==1,"three HP retain fractions until the last millisecond");
  total += advance(pool,1,100,100).hp;
  require(total==3 && pool.remaining==0,"three HP finish exactly after 10000 ms");

  for(int portion=0;portion<5;++portion)
    add(pool,3,0);
  total = 0;
  for(int second=0;second<10;++second)
    total += advance(pool,1000,100,100).hp;
  require(total==15 && pool.remaining==0,"five consecutive apples deliver exactly fifteen HP");

  add(pool,3,1);
  total = advance(pool,5000,100,100).hp;
  require(total==1 && pool.hpFraction>0.49,"half an apple leaves fractional HP in the pool");
  add(pool,3,1);
  require(pool.remaining==10000,"a new portion resets the remaining duration to ten seconds");
  total += advance(pool,5000,100,100).hp;
  require(pool.remaining==5000,"old deadline does not finish an extended pool");
  total += advance(pool,4999,100,100).hp;
  require(pool.remaining==1,"extended pool remains active until its own last millisecond");
  total += advance(pool,1,100,100).hp;
  require(total==6 && pool.remaining==0,"fractional HP survive extension and the exact total is delivered");

  add(pool,10,4);
  auto dose = advance(pool,10000,2,1);
  require(dose.hp==2 && dose.mana==1 && pool.hp==0 && pool.mana==0,
          "maximums cap both attributes and discard excess");
  add(pool,10,4);
  clear(pool);
  dose = advance(pool,10000,100,100);
  require(dose.hp==0 && dose.mana==0 && pool.remaining==0,"death clears HP and mana healing");

  add(pool,3,1);
  const auto first = advance(pool,4321,100,100);
  Buffer serialized;
  save(serialized,pool);
  Pool loaded;
  load(serialized,loaded);
  require(serialized.position==serialized.bytes.size() && loaded.hp==pool.hp && loaded.mana==pool.mana &&
          loaded.remaining==5679 && loaded.hpFraction==pool.hpFraction && loaded.manaFraction==pool.manaFraction,
          "pool HP, mana, time and fractions roundtrip without loss");
  const auto tail = advance(loaded,5678,100,100);
  const auto last = advance(loaded,1,100,100);
  require(first.hp+tail.hp+last.hp==3 && first.mana+tail.mana+last.mana==1 && loaded.remaining==0,
          "loaded pool finishes at the original deadline with exact sums");

  std::puts("healing contract passed");
  }
