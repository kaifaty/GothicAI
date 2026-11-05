#include "saveworld.h"

#include "daedalus/vm.h"
#include <zenkit/SaveGame.hh>
#include <zenkit/World.hh>
#include "daedalus/classes.h"
#include <zenkit/vobs/Misc.hh>
#include <zenkit/world/WayNet.hh>

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

using namespace storygraph;
namespace fs = std::filesystem;
using daedalus::DaedalusDataType;

static constexpr uint32_t ItemActive = 1u << 30;  // ITEM_ACTIVE: equipped
static constexpr int32_t  Far        = std::numeric_limits<int32_t>::max();

static std::string upper(std::string s) {
  for(auto& c : s)
    c = char(std::toupper(uint8_t(c)));
  return s;
  }

SaveWorld::SaveWorld(daedalus::DaedalusVm& vm)
  :vm(vm) {
  if(auto* s = vm.find_symbol_by_name("ZS_TALK"))
    zsTalk = int32_t(s->index());
  addHandlers();
  }

int32_t SaveWorld::constant(const char* name, int32_t def) const {
  auto* s = vm.find_symbol_by_name(name);
  return s!=nullptr && s->type()==DaedalusDataType::INT && s->is_const() ? s->get_int(0) : def;
  }

const SaveWorld::Npc* SaveWorld::npc(uint32_t instanceSymbol) const {
  auto it = bySymbol.find(instanceSymbol);
  return it==bySymbol.end() ? nullptr : &npcs[it->second];
  }

const SaveWorld::Npc* SaveWorld::npc(const daedalus::DaedalusInstance* script) const {
  auto it = byScript.find(script);
  return it==byScript.end() ? nullptr : &npcs[it->second];
  }

const SaveWorld::Npc* SaveWorld::arg(const std::vector<Value>& a, size_t i) const {
  return i<a.size() && a[i].type==Value::Inst ? npc(a[i].inst.get()) : nullptr;
  }

// the function of the AI state, as a symbol index; the npc in a dialog is in ZS_TALK
int32_t SaveWorld::state(const Npc& n) const {
  if(n.script.get()==partner)
    return zsTalk;
  auto& name = n.save->current_state_name.empty() ? n.save->next_state_name : n.save->current_state_name;
  auto* s    = name.empty() ? nullptr : vm.find_symbol_by_name(name);
  return s ? int32_t(s->index()) : -1;
  }

std::shared_ptr<daedalus::IItem> SaveWorld::item(uint32_t symbol) {
  auto it = items.find(symbol);
  if(it!=items.end())
    return it->second;
  std::shared_ptr<daedalus::IItem> r;
  auto* sym = vm.find_symbol_by_index(symbol);
  if(sym!=nullptr && sym->type()==DaedalusDataType::INSTANCE) {
    try {
      r = vm.init_instance<daedalus::IItem>(sym);
      }
    catch(const std::exception&) {
      r = nullptr;
      }
    }
  return items[symbol] = r;
  }

// an equipped item of the category (ITEM_KAT_ARMOR, ITEM_KAT_NF...)
std::shared_ptr<daedalus::IItem> SaveWorld::equipped(const Npc* n, uint32_t category) {
  if(n==nullptr)
    return nullptr;
  for(auto& i : n->save->items) {
    auto flags = uint32_t(i->s_flags);
    if((flags & ItemActive)==0 || (flags & category)==0)
      continue;
    if(auto* s = vm.find_symbol_by_name(i->instance))
      return item(s->index());
    }
  return nullptr;
  }

void SaveWorld::addHandlers() {
  auto& h = handlers;
  auto  integer = [](auto v) { return Value::integer(int32_t(v)); };
  auto  dist = [](const Npc* a, float x, float y, float z) {
    float d = std::sqrt((a->x-x)*(a->x-x) + (a->y-y)*(a->y-y) + (a->z-z)*(a->z-z));
    return d<float(Far) ? int32_t(d) : Far;
    };
  const int32_t attNeutral = constant("ATT_NEUTRAL", 2);
  const int32_t attHostile = constant("ATT_HOSTILE", 0);
  const auto    katArmor   = uint32_t(constant("ITEM_KAT_ARMOR", 1<<4));
  const auto    katNf      = uint32_t(constant("ITEM_KAT_NF",    1<<1));
  const auto    katFf      = uint32_t(constant("ITEM_KAT_FF",    1<<2));

  h["NPC_KNOWSINFO"] = [=,this](auto& a) {
    auto* n = arg(a,0);
    return integer(n!=nullptr && n->script==hero && told.count(uint32_t(a[1].i))>0);
    };
  h["NPC_HASITEMS"] = [=,this](auto& a) {
    auto* n = arg(a,0);
    if(n==nullptr)
      return integer(0);
    auto it = n->items.find(uint32_t(a[1].i));
    return integer(it==n->items.end() ? 0 : it->second);
    };
  h["NPC_ISDEAD"]     = [=,this](auto& a) { auto* n = arg(a,0); return integer(n==nullptr || n->script->attribute[0]<=0); };
  h["HLP_ISVALIDNPC"] = [=,this](auto& a) { return integer(arg(a,0)!=nullptr); };
  h["NPC_ISPLAYER"]   = [=,this](auto& a) { auto* n = arg(a,0); return integer(n!=nullptr && n->script==hero); };
  h["HLP_GETNPC"]     = [=,this](auto& a) {
    auto* n = npc(uint32_t(a[0].i));
    return Value::instance(n ? n->script : nullptr);
    };
  h["NPC_ISINSTATE"]  = [=,this](auto& a) { auto* n = arg(a,0); return integer(n!=nullptr && state(*n)==a[1].i); };
  h["NPC_WASINSTATE"] = [=,this](auto& a) { auto* n = arg(a,0); return integer(n!=nullptr && n->save->last_ai_state==a[1].i); };
  h["NPC_ISINROUTINE"] = [=,this](auto& a) {
    auto* n = arg(a,0);
    if(n==nullptr || state(*n)!=a[1].i)
      return integer(0);
    bool current = !n->save->current_state_name.empty();
    return integer(current ? n->save->current_state_is_routine : n->save->next_state_is_routine);
    };
  h["NPC_REFUSETALK"]    = [=](auto&) { return integer(0); };  // a timer the save does not keep
  h["NPC_GETTRUEGUILD"]  = [=,this](auto& a) { auto* n = arg(a,0); return integer(n ? n->save->guild_true : 0); };
  h["NPC_GETTALENTSKILL"] = [=,this](auto& a) {
    if(auto* n = arg(a,0))
      for(auto& t : n->save->talents)
        if(t && t->talent==a[1].i)
          return integer(t->skill);
    return integer(0);
    };
  h["NPC_GETTALENTVALUE"] = [=,this](auto& a) {
    if(auto* n = arg(a,0))
      for(auto& t : n->save->talents)
        if(t && t->talent==a[1].i)
          return integer(t->value);
    return integer(0);
    };
  h["NPC_HASEQUIPPEDARMOR"]         = [=,this](auto& a) { return integer(equipped(arg(a,0), katArmor)!=nullptr); };
  h["NPC_HASEQUIPPEDMELEEWEAPON"]   = [=,this](auto& a) { return integer(equipped(arg(a,0), katNf)!=nullptr); };
  h["NPC_HASEQUIPPEDRANGEDWEAPON"]  = [=,this](auto& a) { return integer(equipped(arg(a,0), katFf)!=nullptr); };
  h["NPC_HASEQUIPPEDWEAPON"]        = [=,this](auto& a) { return integer(equipped(arg(a,0), katNf|katFf)!=nullptr); };
  h["NPC_GETEQUIPPEDARMOR"]         = [=,this](auto& a) { return Value::instance(equipped(arg(a,0), katArmor)); };
  h["NPC_GETEQUIPPEDMELEEWEAPON"]   = [=,this](auto& a) { return Value::instance(equipped(arg(a,0), katNf)); };
  h["NPC_GETEQUIPPEDRANGEDWEAPON"]  = [=,this](auto& a) { return Value::instance(equipped(arg(a,0), katFf)); };
  h["NPC_ISINFIGHTMODE"] = [=,this](auto& a) { auto* n = arg(a,0); return integer(n!=nullptr && n->save->fight_mode==a[1].i); };

  h["WLD_GETDAY"] = [=,this](auto&) { return integer(day); };
  h["WLD_ISTIME"] = [=,this](auto& a) {
    // like OpenGothic: [begin, end), wrapping over midnight
    int32_t begin = a[0].i*60 + a[1].i, end = a[2].i*60 + a[3].i, now = hour*60 + minute;
    if(begin<=end)
      return integer(begin<=now && now<end);
    return integer(now<end || begin<=now);
    };

  h["NPC_GETDISTTONPC"] = [=,this](auto& a) {
    auto* x = arg(a,0);
    auto* y = arg(a,1);
    if(x==nullptr || y==nullptr || !x->placed || !y->placed || x->world!=y->world)
      return integer(Far);
    return integer(dist(x, y->x, y->y, y->z));
    };
  h["NPC_GETDISTTOPLAYER"] = [=,this](auto& a) {
    auto* x = arg(a,0);
    auto* y = hero ? npc(hero.get()) : nullptr;
    if(x==nullptr || y==nullptr || !x->placed || !y->placed || x->world!=y->world)
      return integer(Far);
    return integer(dist(x, y->x, y->y, y->z));
    };
  h["NPC_GETDISTTOWP"] = [=,this](auto& a) {
    auto* n = arg(a,0);
    if(n==nullptr || !n->placed)
      return integer(Far);
    auto& wps = waypoints[n->world];
    auto  it  = wps.find(upper(a[1].s));
    if(it==wps.end())
      return integer(Far);
    return integer(dist(n, it->second[0], it->second[1], it->second[2]));
    };
  h["NPC_GETNEARESTWP"] = [=,this](auto& a) {
    auto* n = arg(a,0);
    std::string best;
    if(n!=nullptr && n->placed) {
      int32_t d = Far;
      for(auto& [name,p] : waypoints[n->world]) {
        auto x = dist(n, p[0], p[1], p[2]);
        if(x<d) {
          d    = x;
          best = name;
          }
        }
      }
    return Value::str(best);
    };

  // attitudes like OpenGothic: towards the hero the npc's own attitude, otherwise the guilds'
  auto guildAtt = [this,attHostile](int32_t g1, int32_t g2) {
    if(g1<0 || g2<0 || g1>=42 || g2>=42)
      return attHostile;
    return int32_t(guildAttitudes[g1][g2]);
    };
  auto personAtt = [=,this](const std::vector<Value>& a, bool temp) {
    auto* x = arg(a,0);
    auto* y = arg(a,1);
    if(x==nullptr || y==nullptr)
      return integer(attNeutral);
    if(x->script!=hero && y->script!=hero)
      return integer(guildAtt(x->script->guild, y->script->guild));
    auto* n = x->script==hero ? y : x;
    return integer(temp ? n->save->attitude_temp : n->save->attitude);
    };
  h["NPC_GETPERMATTITUDE"]  = [=](auto& a) { return personAtt(a, false); };
  h["NPC_GETATTITUDE"]      = [=](auto& a) { return personAtt(a, true); };
  h["NPC_GETGUILDATTITUDE"] = [=,this](auto& a) {
    auto* x = arg(a,0);
    auto* y = arg(a,1);
    return integer(x && y ? guildAtt(x->script->guild, y->script->guild) : attNeutral);
    };
  h["WLD_GETGUILDATTITUDE"] = [=](auto& a) { return integer(guildAtt(a[0].i, a[1].i)); };
  h["NPC_HASNEWS"] = [=,this](auto& a) {
    auto* n = arg(a,0);
    if(n==nullptr)
      return integer(0);
    int32_t c = 0;
    for(auto& x : n->save->news)
      if(x && (a[1].i==0 || int32_t(x->news_id)==a[1].i))
        c++;
    return integer(c);
    };

  h["HLP_STRCMP"]        = [=](auto& a) { return integer(a[0].s==a[1].s); };
  h["HLP_GETINSTANCEID"] = [=](auto& a) { return integer(a[0].inst ? int32_t(a[0].inst->symbol_index()) : -1); };
  h["HLP_ISVALIDITEM"]   = [=](auto& a) { return integer(a[0].inst!=nullptr); };
  h["HLP_ISITEM"]        = [=](auto& a) { return integer(a[0].inst && int32_t(a[0].inst->symbol_index())==a[1].i); };
  h["HLP_RANDOM"]        = [=](auto&) { return integer(0); };  // the same answer for the VM and the model
  h["INTTOSTRING"]       = [=](auto& a) { return Value::str(std::to_string(a[0].i)); };
  h["CONCATSTRINGS"]     = [=](auto& a) { return Value::str(a[0].s + a[1].s); };
  h["INTTOFLOAT"]        = [=](auto& a) { return Value::real(float(a[0].i)); };
  h["FLOATTOINT"]        = [=](auto& a) { return integer(int32_t(a[0].f)); };
  }

Value SaveWorld::call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) {
  auto  it = handlers.find(fn.name());
  Value r;
  if(it!=handlers.end()) {
    r = it->second(args);
    answered[fn.name()]++;
    } else {
    switch(fn.rtype()) {
      case DaedalusDataType::STRING:   r = Value::str({});         break;
      case DaedalusDataType::INSTANCE: r = Value::instance(nullptr); break;
      case DaedalusDataType::FLOAT:    r = Value::real(0);         break;
      default:                         break;
      }
    defaulted[fn.name()]++;
    }
  if(trace) {
    auto        params = vm.find_parameters_for_function(&fn);
    std::string s      = ident(fn.name()) + "(";
    for(size_t i=0; i<args.size(); ++i) {
      auto& a   = args[i];
      bool  sym = a.type==Value::Int && i<params.size() && params[i].type()==DaedalusDataType::FUNCTION;
      std::string text = a.text(vm);
      if(a.type==Value::Int && (sym || fn.name()=="NPC_KNOWSINFO" || fn.name()=="NPC_HASITEMS" || fn.name()=="HLP_GETNPC")) {
        if(auto* x = vm.find_symbol_by_index(uint32_t(a.i)))
          text = ident(x->name());
        }
      s += (i ? ", " : "") + text;
      }
    s += ")";
    if(fn.has_return())
      s += " = " + r.text(vm);
    if(it==handlers.end())
      s += "   [not modelled]";
    trace(s);
    }
  return r;
  }

void SaveWorld::addNpc(const std::shared_ptr<zenkit::VNpc>& v, const std::string& w, const zenkit::WayNet* wn,
                       bool placed) {
  if(v==nullptr)
    return;
  auto* sym = vm.find_symbol_by_name(v->npc_instance);
  if(sym==nullptr || sym->type()!=DaedalusDataType::INSTANCE) {
    npcsSkipped++;
    return;
    }
  if(bySymbol.count(sym->index()))
    return;  // an npc of the current world wins; of a kind of monster, one is enough

  // the instance function sets what the save does not keep (name, voice, routine); its engine calls go nowhere
  std::shared_ptr<daedalus::INpc> s;
  try {
    s = vm.init_instance<daedalus::INpc>(sym);
    }
  catch(const std::exception&) {
    npcsSkipped++;
    return;
    }
  for(uint32_t i=0; i<zenkit::VNpc::attribute_count; ++i)
    s->attribute[i] = v->attributes[i];
  for(uint32_t i=0; i<zenkit::VNpc::protection_count; ++i)
    s->protection[i] = v->protection[i];
  for(uint32_t i=0; i<zenkit::VNpc::aivar_count; ++i)
    s->aivar[i] = v->aivar[i];
  // the save keeps four hit chances, for NPC_TALENT_1H..NPC_TALENT_CROSSBOW
  for(uint32_t i=0; i<zenkit::VNpc::hcs_count; ++i)
    s->hitchance[i+1] = v->hit_chance[i];
  s->guild        = v->guild;
  s->level        = v->level;
  s->exp          = v->xp;
  s->exp_next     = v->xp_next_level;
  s->lp           = v->lp;
  s->fight_tactic = v->fight_tactic;
  s->flags        = daedalus::NpcFlag(uint32_t(v->flags));
  if(!v->script_waypoint.empty())
    s->wp = v->script_waypoint;

  Npc n;
  n.save   = v;
  n.script = s;
  n.world  = w;
  n.placed = placed;
  n.x = v->position.x;
  n.y = v->position.y;
  n.z = v->position.z;
  if(!placed && wn!=nullptr) {
    // npcs away from the hero are kept without a position: take their routine's waypoint
    auto& wps = waypoints[w];
    auto  it  = wps.find(upper(v->script_waypoint));
    if(it!=wps.end()) {
      n.x = it->second[0];
      n.y = it->second[1];
      n.z = it->second[2];
      n.placed = true;
      }
    }
  // the inventory is packed by category as "ITFO_APPLE:3,ITMI_GOLD:120"; npcs without it list their items
  bool packed = false;
  for(auto& p : v->packed) {
    size_t at = 0;
    while(at<p.size()) {
      auto end   = p.find(',', at);
      auto entry = p.substr(at, end==std::string::npos ? std::string::npos : end-at);
      at = end==std::string::npos ? p.size() : end+1;
      auto colon = entry.rfind(':');
      if(colon==std::string::npos)
        continue;
      if(auto* is = vm.find_symbol_by_name(entry.substr(0, colon))) {
        n.items[is->index()] += std::atoi(entry.c_str()+colon+1);
        packed = true;
        }
      }
    }
  if(!packed)
    for(auto& i : v->items)
      if(i)
        if(auto* is = vm.find_symbol_by_name(i->instance))
          n.items[is->index()] += std::max(i->s_amount, 1);

  if(v->player && hero==nullptr)
    hero = s;
  bySymbol[sym->index()] = npcs.size();
  byScript[s.get()]      = npcs.size();
  npcs.push_back(std::move(n));
  }

bool SaveWorld::load(const fs::path& dir, std::string& error) {
  zenkit::SaveGame sg(zenkit::GameVersion::GOTHIC_2);
  try {
    sg.load(dir);
    }
  catch(const std::exception& e) {
    error = e.what();
    return false;
    }
  title  = sg.metadata.title;
  world  = upper(fs::path(sg.metadata.world).stem().string());
  day    = sg.state.day;
  hour   = sg.state.hour;
  minute = sg.state.minute;
  std::memcpy(guildAttitudes, sg.state.guild_attitudes, sizeof(guildAttitudes));

  // the current world first: its npcs win over the copies left in the others
  std::vector<std::string> names = {world};
  for(auto& e : fs::directory_iterator(dir)) {
    auto stem = upper(e.path().stem().string());
    if(upper(e.path().extension().string())!=".SAV" || stem==world)
      continue;
    if(stem=="SAVEDAT" || stem=="SAVEHDR" || stem=="SAVEINFO" || stem=="THUMB")
      continue;
    names.push_back(stem);
    }
  for(auto& w : names) {
    std::shared_ptr<zenkit::World> wld;
    try {
      wld = sg.load_world(w + ".ZEN");
      }
    catch(const std::exception& e) {
      worldErrors.push_back(w + ": " + e.what());
      continue;
      }
    if(wld==nullptr) {
      worldErrors.push_back(w + ": not in the save");
      continue;
      }
    worlds.push_back(w);
    if(wld->way_net)
      for(auto& p : wld->way_net->points)
        if(p)
          waypoints[w][upper(p->name)] = {p->position.x, p->position.y, p->position.z};
    // npcs near the hero are vobs with a position, the others wait in a list
    std::function<void(const std::vector<std::shared_ptr<zenkit::VirtualObject>>&)> walk = [&](auto& vobs) {
      for(auto& o : vobs) {
        if(o==nullptr)
          continue;
        if(auto n = std::dynamic_pointer_cast<zenkit::VNpc>(o))
          addNpc(n, w, wld->way_net.get(), true);
        walk(o->children);
        }
      };
    walk(wld->world_vobs);
    for(auto& n : wld->npcs)
      addNpc(n, w, wld->way_net.get(), false);
    }

  // story variables after the npcs: instance functions may write globals
  for(auto& s : sg.state.symbols) {
    auto* sym = vm.find_symbol_by_name(s.name);
    if(sym==nullptr) {
      symbolsMissing++;  // Ikarus, LeGo...
      continue;
      }
    if(sym->is_const() || sym->is_member() || sym->type()!=DaedalusDataType::INT)
      continue;
    for(size_t i=0; i<s.values.size() && i<sym->count(); ++i)
      sym->set_int(s.values[i], uint16_t(i));
    symbolsSet++;
    }
  if(auto* k = vm.find_symbol_by_name("KAPITEL"))
    kapitel = k->get_int(0);
  for(auto& i : sg.state.infos)
    if(i.told)
      if(auto* s = vm.find_symbol_by_name(i.name))
        told.insert(s->index());

  if(hero==nullptr) {
    auto v = std::make_shared<zenkit::VNpc>();
    v->npc_instance  = "PC_HERO";
    v->player        = true;
    v->attributes[0] = v->attributes[1] = 1;
    addNpc(v, world, nullptr, false);
    heroStandIn = hero!=nullptr;
    }
  if(hero!=nullptr) {
    if(auto* h = vm.global_hero())
      h->set_instance(hero);
    if(auto* o = vm.global_other())
      o->set_instance(hero);
    }
  // HILDA = Hlp_GetNpc(BAU_951_HILDA)...: the game sets these up on every load
  if(auto* init = vm.find_symbol_by_name("B_INITNPCGLOBALS")) {
    try {
      vm.call_function(init);
      }
    catch(const std::exception& e) {
      error = std::string("B_InitNpcGlobals: ") + e.what();
      return false;
      }
    }
  resetCounts();
  return true;
  }
