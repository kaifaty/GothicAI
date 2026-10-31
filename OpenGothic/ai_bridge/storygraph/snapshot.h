#pragma once

#include <zenkit/DaedalusVm.hh>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace storygraph {

using zenkit::DaedalusDataType;

// Values of all script variables, to undo what checking a condition wrote (C_Beklauen stores the theft
// values, conditions keep static locals): every dialog is checked against the state of the save.
class Snapshot final {
  public:
    explicit Snapshot(zenkit::DaedalusVm& vm) {
      for(uint32_t i=0; i<vm.symbols().size(); ++i) {
        auto* s = vm.find_symbol_by_index(i);
        // the VM's own scratch for pushed strings is not a script variable, and it grows during a call:
        // restored by the count taken before, it would read past the values taken
        if(s==nullptr || s->is_const() || s->is_member() || s->is_external() || s->name()=="$PHOENIX_FAKE_STRINGS")
          continue;
        auto t = s->type();
        if(t==DaedalusDataType::INT || t==DaedalusDataType::FLOAT || t==DaedalusDataType::STRING ||
           t==DaedalusDataType::INSTANCE)
          syms.push_back(s);
        }
      }

    void take() {
      ints.clear();
      floats.clear();
      strs.clear();
      insts.clear();
      for(auto* s : syms)
        for(uint16_t i=0; i<count(*s); ++i)
          switch(s->type()) {
            case DaedalusDataType::INT:      ints.push_back(s->get_int(i));      break;
            case DaedalusDataType::FLOAT:    floats.push_back(s->get_float(i));  break;
            case DaedalusDataType::STRING:   strs.push_back(s->get_string(i));   break;
            default:                         insts.push_back(s->get_instance()); break;
            }
      }

    void restore() {
      size_t ni = 0, nf = 0, ns = 0, nn = 0;
      for(auto* s : syms)
        for(uint16_t i=0; i<count(*s); ++i)
          switch(s->type()) {
            case DaedalusDataType::INT:      s->set_int(ints[ni++], i);      break;
            case DaedalusDataType::FLOAT:    s->set_float(floats[nf++], i);  break;
            case DaedalusDataType::STRING:   s->set_string(strs[ns++], i);   break;
            default:                         s->set_instance(insts[nn++]);   break;
            }
      }

    // the first variable whose value differs from `o`'s ("MIS_X: 1, 2"), empty if none; both taken over one VM
    std::string differs(const Snapshot& o) const {
      size_t ni = 0, nf = 0, ns = 0, nn = 0;
      for(auto* s : syms)
        for(uint16_t i=0; i<count(*s); ++i) {
          auto name = [&]() { return s->name() + (s->count()>1 ? "[" + std::to_string(i) + "]" : ""); };
          switch(s->type()) {
            case DaedalusDataType::INT:
              if(ints[ni]!=o.ints[ni])
                return name() + ": " + std::to_string(ints[ni]) + ", " + std::to_string(o.ints[ni]);
              ni++;
              break;
            case DaedalusDataType::FLOAT:
              if(std::memcmp(&floats[nf], &o.floats[nf], sizeof(float))!=0)
                return name() + ": " + std::to_string(floats[nf]) + ", " + std::to_string(o.floats[nf]);
              nf++;
              break;
            case DaedalusDataType::STRING:
              if(strs[ns]!=o.strs[ns])
                return name() + ": \"" + strs[ns] + "\", \"" + o.strs[ns] + "\"";
              ns++;
              break;
            default:
              if(insts[nn]!=o.insts[nn])
                return name() + ": another instance";
              nn++;
              break;
            }
          }
      return {};
      }

  private:
    static uint16_t count(const zenkit::DaedalusSymbol& s) {
      return s.type()==DaedalusDataType::INSTANCE ? 1 : uint16_t(s.count());
      }

    std::vector<zenkit::DaedalusSymbol*>                    syms;
    std::vector<int32_t>                                    ints;
    std::vector<float>                                      floats;
    std::vector<std::string>                                strs;
    std::vector<std::shared_ptr<zenkit::DaedalusInstance>>  insts;
  };

}
