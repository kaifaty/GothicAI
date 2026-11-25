#pragma once

#include <cstdint>
#include <typeinfo>

namespace daedalus {
class DaedalusScript;
class DaedalusSymbol;
class DaedalusTransientInstance;
class DaedalusVm;
}

// Identity of a live engine record. Several records can use the same authored ID.
class RecordInstance {
  public:
    virtual ~RecordInstance() = default;

    uint32_t symbol_index() const { return _m_symbol_index; }
    const std::type_info* instance_type() const { return _m_type; }

    void setRecordIdentity(uint32_t id) {
      _m_symbol_index = id;
      _m_type = &typeid(*this);
      }

    void* user_ptr = nullptr;

  protected:
    // The offline adapter's registered member layout is relative to this record.
    virtual uint8_t* data() { return reinterpret_cast<uint8_t*>(this); }
    virtual const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(this); }

  private:
    friend class daedalus::DaedalusScript;
    friend class daedalus::DaedalusSymbol;
    friend class daedalus::DaedalusTransientInstance;
    friend class daedalus::DaedalusVm;

    uint32_t _m_symbol_index = uint32_t(-1);
    const std::type_info* _m_type = nullptr;
  };

struct NpcInstance;
struct ItemInstance;
struct DialogInstance;
struct DialogChoiceRecord;

namespace daedalus {
using DaedalusInstance = ::RecordInstance;
using INpc = ::NpcInstance;
using IItem = ::ItemInstance;
using IInfo = ::DialogInstance;
using IInfoChoice = ::DialogChoiceRecord;
}
