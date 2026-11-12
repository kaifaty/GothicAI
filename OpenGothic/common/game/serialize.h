#pragma once

#include "savearchive.h"

#include "gametime.h"
#include "constants.h"

#include <ctime>
#include <memory>

namespace daedalus {
  enum class DaedalusDataType : std::uint32_t;
  class DaedalusVm;
  struct INpc;
  }

class WayPoint;
class Npc;
class Interactive;
class World;
class FpLock;
class ScriptFn;
class SaveGameHeader;

class Serialize : public SaveArchive {
  public:
    Serialize(Tempest::ODevice& fout);
    Serialize(Tempest::IDevice&  fin);
    Serialize(Serialize&&)=delete;
    ~Serialize() noexcept(false);

    void setContext(World* ctx);

    template<class ... Arg>
    void write(const Arg& ... a){
      (writeValue(*this,a),... );
      }

    template<class ... Arg>
    void read(Arg& ... a){
      (readValue(*this,a),... );
      }

    void readNpc(daedalus::DaedalusVm& vm, std::shared_ptr<daedalus::INpc>& npc);

  private:
    friend class SaveArchive;
    using SaveArchive::implWrite;
    using SaveArchive::implRead;

    void implWrite(daedalus::DaedalusDataType  i)    { write((uint32_t) i); }
    void implRead (daedalus::DaedalusDataType& i)    { read((uint32_t&) i); }

    // typedef
    void implWrite(gtime  i)    { writeBytes(&i,sizeof(i)); }
    void implRead (gtime& i)    { readBytes (&i,sizeof(i)); }

    void implWrite(WalkBit   i) { writeBytes(&i,sizeof(i)); }
    void implRead (WalkBit&  i) { readBytes (&i,sizeof(i)); }

    void implWrite(BodyState i) { writeBytes(&i,sizeof(i)); }
    void implRead (BodyState&i) { readBytes (&i,sizeof(i)); }

    void implWrite(Attitude  i) { writeBytes(&i,sizeof(i));}
    void implRead (Attitude& i) { readBytes (&i,sizeof(i)); }

    void implWrite(WeaponState  w);
    void implRead (WeaponState &w);

    void implWrite(const SaveGameHeader& p);
    void implRead (SaveGameHeader&       p);

    void implWrite(const daedalus::INpc& h);

    void implWrite(const FpLock& fp);
    void implRead (FpLock& fp);

    // pointers
    void implWrite(const WayPoint*  wptr);
    void implRead (const WayPoint*& wptr);

    void implWrite(const ScriptFn& fn);
    void implRead (ScriptFn&       fn);

    void implWrite(const Npc*  npc);
    void implRead (const Npc*& npc);

    void implWrite(Npc*        npc);
    void implRead (Npc*&       npc);

    void implWrite(Interactive*  mobsi);
    void implRead (Interactive*& mobsi);

    std::string tmpStr;
    World*      ctx = nullptr;
  };
