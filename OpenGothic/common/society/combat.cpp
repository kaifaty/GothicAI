#include "combat.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Society::Combat {

namespace {
constexpr size_t MaxActors = 64;
constexpr size_t MaxProfiles = 256;
constexpr Millis MaxDuration = 60*60*1000;
constexpr uint32_t MaxEvents = 65536;
constexpr size_t MaxPending = 8192;
constexpr int32_t MaxCoordinate = 100'000'000;
constexpr uint32_t SnapshotMagic = 0x54424d43; // CMBT, little endian
constexpr uint16_t SnapshotVersion = 1;

bool positive(Millis n) { return n>0 && n<=MaxDuration; }
bool validDuration(Millis n) { return n>=0 && n<=MaxDuration; }
bool validPoint(Point p) {
  return p.x>=-MaxCoordinate && p.x<=MaxCoordinate && p.z>=-MaxCoordinate && p.z<=MaxCoordinate;
}

int64_t distanceSquared(Point a, Point b) {
  const auto dx = int64_t(a.x)-b.x, dz = int64_t(a.z)-b.z;
  return dx*dx + dz*dz;
}

bool inRange(Point a, Point b, int32_t cm) {
  return distanceSquared(a,b)<=int64_t(cm)*cm;
}

bool active(const Combatant& actor) { return actor.state==ActorState::Active; }
bool committed(EventKind kind) { return kind==EventKind::Impact || kind==EventKind::EffectTick; }

bool validClock(Millis now, Millis deadline) {
  return now>=0 && deadline>=now && deadline-now<=MaxDuration &&
         deadline<=std::numeric_limits<Millis>::max()-MaxDuration;
}

bool validActor(const Combatant& actor, size_t profiles) {
  return actor.id!=NoActor && actor.profile<profiles && validPoint(actor.position) &&
         actor.ammunition>=0 && actor.mana>=0 && actor.strength>=0 && actor.moveCmPerSecond>=0 &&
         actor.readyAt>=0 && actor.stunnedUntil>=0;
}

int32_t damageOnHit(const AttackProfile& profile, const Combatant& attacker, const Combatant& defender, bool critical) {
  int64_t total = 0;
  bool vulnerable = false;
  const bool melee = profile.kind==AttackKind::HumanMelee || profile.kind==AttackKind::BeastMelee;
  for(size_t i=0;i<profile.damage.size();++i) {
    if((profile.damageMask & (1u<<i))==0 || defender.protection[i]<0)
      continue;
    vulnerable = true;
    const auto strength = melee ? attacker.strength : 0;
    const auto part = std::max<int64_t>(0,int64_t(profile.damage[i])+strength-defender.protection[i]);
    total += critical || profile.normalDamageDivisor==1 ? part : std::max<int64_t>(0,part-1)/profile.normalDamageDivisor;
    }
  if(!vulnerable)
    return 0;
  return int32_t(std::clamp<int64_t>(total,profile.minDamage,std::numeric_limits<int32_t>::max()));
}

bool validProfile(const AttackProfile& profile) {
  if(profile.rangeCm<=0 || profile.rangeCm>2*MaxCoordinate ||
     profile.areaRadiusCm<0 || profile.areaRadiusCm>MaxCoordinate ||
     !validDuration(profile.windup) || !positive(profile.recovery) || !validDuration(profile.flight) ||
     profile.hitPermille>1000 || profile.criticalPermille>1000 || profile.normalDamageDivisor==0 ||
     profile.minDamage<0 || profile.resourceCost<0 || !validDuration(profile.stunDuration) ||
     profile.tickDamage<0 || !validDuration(profile.tickPeriod) || !validDuration(profile.effectDuration))
    return false;
  if(std::any_of(profile.damage.begin(),profile.damage.end(),[](int32_t amount) { return amount<0; }))
    return false;
  if(profile.tickDamage==0 ? profile.effectDuration!=0 || profile.tickPeriod!=0
                          : profile.tickPeriod==0 || profile.tickPeriod>profile.effectDuration)
    return false;
  if(profile.damageMask==0 && profile.stunDuration==0 && profile.tickDamage==0)
    return false;
  switch(profile.kind) {
    case AttackKind::HumanMelee:
    case AttackKind::BeastMelee: return profile.resourceCost==0 && profile.flight==0;
    case AttackKind::Ranged:     return profile.resourceCost>0 && profile.flight>0;
    case AttackKind::Magic:      return true;
    }
  return false;
}

[[noreturn]] void invalidSnapshot() {
  throw std::invalid_argument("Invalid combat encounter snapshot");
}

struct SnapshotWriter {
  std::vector<uint8_t> bytes;

  template<class T> void put(T value) {
    if constexpr(std::is_enum_v<T>) {
      put(static_cast<std::underlying_type_t<T>>(value));
      } else if constexpr(std::is_same_v<T,bool>) {
      put(uint8_t(value));
      } else {
      using U = std::make_unsigned_t<T>;
      const auto bits = std::bit_cast<U>(value);
      for(size_t i=0;i<sizeof(T);++i)
        bytes.push_back(uint8_t(bits>>(8*i)));
      }
    }

  template<class... T> void operator()(T... value) { (put(value),...); }
};

struct SnapshotReader {
  std::span<const uint8_t> bytes;

  template<class T> void get(T& value) {
    if constexpr(std::is_enum_v<T>) {
      std::underlying_type_t<T> bits;
      get(bits);
      value = static_cast<T>(bits);
      } else if constexpr(std::is_same_v<T,bool>) {
      uint8_t bits;
      get(bits);
      if(bits>1)
        invalidSnapshot();
      value = bits!=0;
      } else {
      if(bytes.size()<sizeof(T))
        invalidSnapshot();
      uint64_t bits = 0;
      for(size_t i=0;i<sizeof(T);++i)
        bits |= uint64_t(bytes[i])<<(8*i);
      value = std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
      bytes = bytes.subspan(sizeof(T));
      }
    }

  template<class... T> void operator()(T&... value) { (get(value),...); }

  template<class T> void resize(std::vector<T>& entries, size_t limit) {
    uint32_t count;
    get(count);
    if(count>limit)
      invalidSnapshot();
    entries.resize(count);
    }
};

template<class Archive, class Profile>
void profileFields(Archive& archive, Profile& p) {
  archive(p.kind,p.calibrated,p.rangeCm,p.areaRadiusCm,p.windup,p.recovery,p.flight,p.hitPermille,
          p.criticalPermille,p.damageMask,p.normalDamageDivisor,p.minDamage,p.resourceCost,p.stunDuration,
          p.tickPeriod,p.effectDuration,p.tickDamage);
  for(auto& damage:p.damage)
    archive(damage);
}

template<class Archive, class Actor>
void actorFields(Archive& archive, Actor& a) {
  archive(a.id,a.side,a.profile,a.target,a.position.x,a.position.z,a.hp,a.ammunition,a.mana,a.strength,
          a.moveCmPerSecond,a.readyAt,a.stunnedUntil,a.state);
  for(auto& protection:a.protection)
    archive(protection);
}

template<class Archive, class Pending>
void pendingFields(Archive& archive, Pending& p) {
  archive(p.at,p.seq,p.actor,p.target,p.kind,p.point.x,p.point.z,p.expiry);
}
}

bool Encounter::Pending::operator<(const Pending& other) const {
  return at!=other.at ? at>other.at : seq>other.seq;
}

struct Access {
  static Combatant* find(Encounter& enc, uint32_t id) {
    auto it = std::lower_bound(enc.actors_.begin(),enc.actors_.end(),id,
                                [](const Combatant& actor, uint32_t key) { return actor.id<key; });
    return it!=enc.actors_.end() && it->id==id ? &*it : nullptr;
  }

  static void finish(Encounter& enc, Outcome result, std::optional<uint32_t> winner={}) {
    enc.outcome_ = result;
    enc.winner_ = winner;
    enc.pending_.clear();
    enc.committed_ = 0;
  }

  static void push(Encounter& enc, EventKind kind, Millis at, uint32_t actor=NoActor,
                   uint32_t target=NoActor, Point point={}, Millis expiry=0) {
    if(enc.outcome_!=Outcome::Active || (at>=enc.deadline_ && kind!=EventKind::End))
      return;
    if(enc.pending_.size()>=MaxPending) {
      finish(enc,Outcome::EventLimit);
      return;
      }
    enc.pending_.push_back({at,enc.nextSeq_++,actor,target,kind,point,expiry});
    std::push_heap(enc.pending_.begin(),enc.pending_.end());
    enc.committed_ += committed(kind);
  }

  static void ready(Encounter& enc, Combatant& actor, Millis at) {
    actor.readyAt = at;
    push(enc,EventKind::Ready,at,actor.id);
  }

  static void changed(Encounter& enc, Combatant& actor, int32_t hp=0, int32_t ammo=0, int32_t mana=0) {
    enc.event_.changes.push_back({actor.id,hp,ammo,mana,actor.position,actor.state,actor.stunnedUntil});
  }

  static std::optional<uint32_t> oneSide(const Encounter& enc) {
    std::optional<uint32_t> side;
    for(const auto& actor:enc.actors_) {
      if(!active(actor))
        continue;
      if(side && *side!=actor.side)
        return {};
      side = actor.side;
      }
    // No active actors is a draw. The caller distinguishes it from multiple active sides.
    return side;
  }

  static void settle(Encounter& enc) {
    if(enc.outcome_!=Outcome::Active)
      return;
    const auto side = oneSide(enc);
    if(!side) {
      if(std::none_of(enc.actors_.begin(),enc.actors_.end(),active))
        finish(enc,Outcome::Draw);
      return;
      }

    // Attacks already released and applied spells can still hit the surviving side.
    // Stop admitting new attacks as soon as the other side is taken out.
    const auto survives = [&](const Encounter::Pending& event) {
      if(event.kind==EventKind::End || event.kind==EventKind::Withdraw)
        return true;
      if(!committed(event.kind))
        return false;
      const auto* attacker = find(enc,event.actor);
      if(attacker==nullptr || attacker->side==*side)
        return false;
      const auto* target = find(enc,event.target);
      return (target!=nullptr && active(*target)) ||
             (event.kind==EventKind::Impact && enc.profiles_[attacker->profile].areaRadiusCm>0);
      };
    std::erase_if(enc.pending_,[&](const Encounter::Pending& event) {
      if(survives(event))
        return false;
      enc.committed_ -= committed(event.kind);
      return true;
      });
    if(enc.committed_==0) {
      finish(enc,Outcome::Won,side);
      return;
      }
    enc.settling_ = true;
    std::make_heap(enc.pending_.begin(),enc.pending_.end());
  }

  static bool hasResources(const Combatant& actor, const AttackProfile& profile) {
    if(profile.kind==AttackKind::Ranged)
      return actor.ammunition>=profile.resourceCost;
    if(profile.kind==AttackKind::Magic)
      return actor.mana>=profile.resourceCost;
    return true;
  }

  static Combatant* targetFor(Encounter& enc, const Combatant& attacker, const AttackProfile& profile) {
    const auto reachable = [&](const Combatant& actor) {
      return active(actor) && actor.side!=attacker.side &&
             (attacker.moveCmPerSecond>0 || inRange(attacker.position,actor.position,profile.rangeCm));
      };
    if(auto* preferred=find(enc,attacker.target); preferred!=nullptr && reachable(*preferred))
      return preferred;
    auto it = std::find_if(enc.actors_.begin(),enc.actors_.end(),reachable);
    return it!=enc.actors_.end() ? &*it : nullptr;
  }

  static void approach(Encounter& enc, Combatant& actor, const Combatant& target, const AttackProfile& profile) {
    const auto squared = distanceSquared(actor.position,target.position);
    auto distance = int64_t(std::sqrt(double(squared)));
    // Correct rounding near a whole centimetre before using integer motion arithmetic.
    if(distance*distance<squared)
      ++distance;
    const auto travel = distance-(profile.rangeCm-1);
    const auto delay = (travel*1000+actor.moveCmPerSecond-1)/actor.moveCmPerSecond;
    if(delay>=enc.deadline_-enc.now_)
      return;
    const auto along = [&](int32_t from, int32_t to) {
      const auto delta = (int64_t(to)-from)*travel;
      const auto rounded = delta>=0 ? delta+distance/2 : delta-distance/2;
      return from+int32_t(rounded/distance);
      };
    const Point destination{along(actor.position.x,target.position.x),along(actor.position.z,target.position.z)};
    actor.readyAt = enc.now_+delay;
    push(enc,EventKind::Arrive,actor.readyAt,actor.id,target.id,destination);
  }

  static void onReady(Encounter& enc, Combatant& actor, const AttackProfile& profile) {
    if(!active(actor))
      return;
    if(actor.stunnedUntil>enc.now_) {
      ready(enc,actor,actor.stunnedUntil);
      return;
      }
    if(!hasResources(actor,profile))
      return;
    auto* target = targetFor(enc,actor,profile);
    if(target==nullptr) {
      enc.waitingForRange_.push_back(actor.id);
      return;
      }
    actor.target = target->id;
    enc.event_.target = target->id;
    if(!inRange(actor.position,target->position,profile.rangeCm)) {
      approach(enc,actor,*target,profile);
      return;
      }
    actor.readyAt = enc.now_+profile.windup;
    push(enc,EventKind::Release,actor.readyAt,actor.id,target->id);
  }

  static void onArrive(Encounter& enc, Combatant& actor, Point point) {
    if(!active(actor))
      return;
    if(actor.stunnedUntil>enc.now_) {
      ready(enc,actor,actor.stunnedUntil);
      return;
      }
    actor.position = point;
    changed(enc,actor);
    ready(enc,actor,enc.now_);
    std::erase_if(enc.waitingForRange_,[&](uint32_t id) {
      auto& idle = *find(enc,id);
      if(!active(idle))
        return true;
      if(idle.side==actor.side || !inRange(idle.position,point,enc.profiles_[idle.profile].rangeCm))
        return false;
      ready(enc,idle,enc.now_);
      return true;
      });
  }

  static void onRelease(Encounter& enc, Combatant& actor, const AttackProfile& profile, const Combatant* target) {
    if(!active(actor))
      return;
    if(actor.stunnedUntil>enc.now_) {
      ready(enc,actor,actor.stunnedUntil);
      return;
      }
    if(target==nullptr || !active(*target) || !inRange(actor.position,target->position,profile.rangeCm)) {
      ready(enc,actor,enc.now_);
      return;
      }
    if(!hasResources(actor,profile))
      return;
    // The attack is committed at release. A projectile may land after its attacker falls.
    if(profile.kind==AttackKind::Ranged) {
      actor.ammunition -= profile.resourceCost;
      changed(enc,actor,0,-profile.resourceCost);
      } else if(profile.kind==AttackKind::Magic && profile.resourceCost>0) {
      actor.mana -= profile.resourceCost;
      changed(enc,actor,0,0,-profile.resourceCost);
      }
    push(enc,EventKind::Impact,enc.now_+profile.flight,actor.id,target->id,target->position);
    ready(enc,actor,enc.now_+profile.recovery);
  }

  static bool roll(Encounter& enc, uint16_t permille) {
    return permille==1000 || (permille>0 && enc.rng_.unit()*1000.0<permille);
  }

  static int32_t damage(Encounter& enc, Combatant& target, int32_t amount) {
    if(amount<=0)
      return 0;
    const auto hpBefore = target.hp;
    const auto floor = enc.lethal_ ? 0 : 1;
    target.hp = std::max(floor,target.hp-amount);
    if(target.hp==floor)
      target.state = enc.lethal_ ? ActorState::Dead : ActorState::KnockedOut;
    return target.hp-hpBefore;
  }

  static void hit(Encounter& enc, Combatant& actor, Combatant& target, const AttackProfile& profile) {
    if(!active(target) || actor.side==target.side || !roll(enc,profile.hitPermille))
      return;
    const auto oldState = target.state;
    const auto oldStun = target.stunnedUntil;
    const auto hp = damage(enc,target,damageOnHit(profile,actor,target,roll(enc,profile.criticalPermille)));
    if(active(target)) {
      if(profile.stunDuration>0)
        target.stunnedUntil = std::max(target.stunnedUntil,enc.now_+profile.stunDuration);
      if(profile.tickPeriod>0)
        push(enc,EventKind::EffectTick,enc.now_+profile.tickPeriod,actor.id,target.id,{},enc.now_+profile.effectDuration);
      }
    if(hp!=0 || oldState!=target.state || oldStun!=target.stunnedUntil)
      changed(enc,target,hp);
  }

  static void onImpact(Encounter& enc, Combatant& actor, Combatant* target, const AttackProfile& profile, Point point) {
    if(profile.areaRadiusCm==0) {
      if(target!=nullptr)
        hit(enc,actor,*target,profile);
      return;
      }
    for(auto& candidate:enc.actors_)
      if(inRange(candidate.position,point,profile.areaRadiusCm))
        hit(enc,actor,candidate,profile);
  }

  static void onEffectTick(Encounter& enc, Combatant& actor, Combatant& target, const AttackProfile& profile, Millis expiry) {
    if(!active(target))
      return;
    const auto oldState = target.state;
    const auto hp = damage(enc,target,profile.tickDamage);
    if(hp!=0 || oldState!=target.state)
      changed(enc,target,hp);
    if(active(target) && profile.tickPeriod<=expiry-enc.now_)
      push(enc,EventKind::EffectTick,enc.now_+profile.tickPeriod,actor.id,target.id,{},expiry);
  }

  static Encounter start(Millis now, Millis deadline, bool lethal, uint64_t seed,
                         std::span<const Combatant> actors, std::span<const AttackProfile> profiles, bool gameplay) {
    if(!validClock(now,deadline) || deadline==now ||
       actors.size()<2 || actors.size()>MaxActors || profiles.empty() || profiles.size()>MaxProfiles)
      throw std::invalid_argument("Invalid combat encounter limits");
    for(const auto& profile:profiles)
      if(!validProfile(profile) || (gameplay && !profile.calibrated))
        throw std::invalid_argument("Invalid or uncalibrated combat profile");

    Encounter enc;
    enc.now_ = now;
    enc.deadline_ = deadline;
    enc.lethal_ = lethal;
    enc.rng_.seed(seed);
    enc.profiles_.assign(profiles.begin(),profiles.end());
    enc.actors_.assign(actors.begin(),actors.end());
    std::sort(enc.actors_.begin(),enc.actors_.end(),[](const Combatant& a,const Combatant& b) { return a.id<b.id; });

    uint32_t previous = NoActor;
    for(const auto& actor:enc.actors_) {
      if(!validActor(actor,profiles.size()) || actor.id==previous || actor.hp<=0 || !active(actor) ||
         (actor.readyAt!=0 && actor.readyAt<now))
        throw std::invalid_argument("Invalid combat participant");
      if(actor.target!=NoActor) {
        const auto* target = find(enc,actor.target);
        if(target==nullptr || target->side==actor.side)
          throw std::invalid_argument("Invalid combat target");
        }
      previous = actor.id;
      }
    if(oneSide(enc))
      throw std::invalid_argument("Combat requires at least two sides");

    enc.pending_.reserve(2*actors.size()+1);
    enc.event_.changes.reserve(actors.size());
    push(enc,EventKind::End,deadline);
    for(auto& actor:enc.actors_)
      ready(enc,actor,std::max(now,actor.readyAt));
    return enc;
  }

  static std::optional<Millis> nextEventAt(const Encounter& enc) {
    if(enc.pending_.empty())
      return {};
    return enc.pending_.front().at;
  }

  static void sealAttacks(Encounter& enc) {
    enc.waitingForRange_.clear();
    std::erase_if(enc.pending_,[](const Encounter::Pending& event) {
      return event.kind==EventKind::Ready || event.kind==EventKind::Arrive || event.kind==EventKind::Release;
      });
    std::make_heap(enc.pending_.begin(),enc.pending_.end());
  }

  static bool hasCommitted(const Encounter& enc) { return enc.committed_>0; }

  static const Event& step(Encounter& enc) {
    if(enc.pending_.empty())
      throw std::logic_error("Combat has no scheduled event");
    std::pop_heap(enc.pending_.begin(),enc.pending_.end());
    const auto pending = enc.pending_.back();
    enc.pending_.pop_back();
    enc.committed_ -= committed(pending.kind);
    enc.now_ = pending.at;
    enc.event_.at = pending.at;
    enc.event_.kind = pending.kind;
    enc.event_.actor = pending.actor;
    enc.event_.target = pending.target;
    enc.event_.changes.clear();

    if(enc.processed_++>=MaxEvents) {
      enc.event_.kind = EventKind::End;
      finish(enc,Outcome::EventLimit);
      } else if(pending.kind==EventKind::End) {
      const auto side = oneSide(enc);
      finish(enc,side ? Outcome::Won : Outcome::Timeout,side);
      } else {
      // The validated roster lives for the whole encounter, including actors who were hit or left.
      auto& actor = *find(enc,pending.actor);
      auto* target = find(enc,pending.target);
      const auto& profile = enc.profiles_[actor.profile];
      switch(pending.kind) {
        case EventKind::Ready:      onReady(enc,actor,profile); break;
        case EventKind::Arrive:     onArrive(enc,actor,pending.point); break;
        case EventKind::Release:    onRelease(enc,actor,profile,target); break;
        case EventKind::Impact:     onImpact(enc,actor,target,profile,pending.point); break;
        case EventKind::EffectTick: onEffectTick(enc,actor,*target,profile,pending.expiry); break;
        case EventKind::Withdraw:
          if(active(actor)) {
            actor.state = ActorState::Withdrawn;
            changed(enc,actor);
            }
          break;
        case EventKind::End:        break;
        }
      const bool left = std::any_of(enc.event_.changes.begin(),enc.event_.changes.end(),
                                    [](const Change& change) { return change.state!=ActorState::Active; });
      if(left || (enc.settling_ && enc.committed_==0))
        settle(enc);
      }
    enc.event_.outcome = enc.outcome_;
    enc.event_.winner = enc.winner_;
    return enc.event_;
  }

  static void withdraw(Encounter& enc, uint32_t actor, Millis at) {
    auto* current = find(enc,actor);
    if(enc.outcome_!=Outcome::Active || at<enc.now_ || at>=enc.deadline_ || current==nullptr || !active(*current) ||
       enc.pending_.size()>=MaxPending)
      throw std::invalid_argument("Invalid combat withdrawal");
    push(enc,EventKind::Withdraw,at,actor);
  }

  static std::vector<uint8_t> save(const Encounter& enc) {
    SnapshotWriter out;
    out(SnapshotMagic,SnapshotVersion,enc.now_,enc.deadline_,enc.lethal_,enc.settling_,enc.nextSeq_,enc.processed_,
        enc.outcome_,enc.winner_.has_value(),enc.winner_.value_or(0));
    for(auto word:enc.rng_.s)
      out(word);
    out(uint32_t(enc.profiles_.size()));
    for(const auto& profile:enc.profiles_)
      profileFields(out,profile);
    out(uint32_t(enc.actors_.size()));
    for(const auto& actor:enc.actors_)
      actorFields(out,actor);
    out(uint32_t(enc.waitingForRange_.size()));
    for(auto id:enc.waitingForRange_)
      out(id);
    out(uint32_t(enc.pending_.size()));
    for(const auto& pending:enc.pending_)
      pendingFields(out,pending);
    return std::move(out.bytes);
  }

  static bool validActorState(const Combatant& actor, bool lethal) {
    switch(actor.state) {
      case ActorState::Active:
      case ActorState::Withdrawn:  return actor.hp>0;
      case ActorState::KnockedOut: return !lethal && actor.hp==1;
      case ActorState::Dead:       return lethal && actor.hp==0;
      }
    return false;
  }

  static bool validPending(Encounter& enc, const Encounter::Pending& p) {
    if(!validPoint(p.point) || p.seq>=enc.nextSeq_ || p.at<enc.now_ || p.at>enc.deadline_)
      return false;
    if(p.kind==EventKind::End)
      return p.seq==0 && p.at==enc.deadline_ && p.actor==NoActor && p.target==NoActor && p.expiry==0;
    const auto* actor = find(enc,p.actor);
    if(actor==nullptr || p.seq==0 || p.at==enc.deadline_)
      return false;
    const auto* target = find(enc,p.target);
    const bool opponent = target!=nullptr && target->side!=actor->side;
    switch(p.kind) {
      case EventKind::Ready:
      case EventKind::Withdraw:   return p.target==NoActor && p.expiry==0;
      case EventKind::Arrive:     return opponent && p.expiry==0 && actor->moveCmPerSecond>0;
      case EventKind::Release:
      case EventKind::Impact:     return opponent && p.expiry==0;
      case EventKind::EffectTick: return opponent && enc.profiles_[actor->profile].tickPeriod>0 &&
                                         p.expiry>=p.at && p.expiry<=enc.deadline_+MaxDuration;
      case EventKind::End:        return false;
      }
    return false;
  }

  static void validateSnapshot(Encounter& enc, bool gameplay) {
    if(!validClock(enc.now_,enc.deadline_) || enc.outcome_>Outcome::EventLimit ||
       enc.processed_>MaxEvents+1 || enc.processed_>enc.nextSeq_ ||
       enc.nextSeq_==0 || enc.nextSeq_>(MaxEvents+2)*(MaxPending+2) ||
       enc.actors_.size()<2 || enc.profiles_.empty() ||
       std::none_of(enc.rng_.s.begin(),enc.rng_.s.end(),[](uint64_t word) { return word!=0; }))
      invalidSnapshot();
    for(const auto& profile:enc.profiles_)
      if(!validProfile(profile) || (gameplay && !profile.calibrated))
        invalidSnapshot();
    for(size_t i=0;i<enc.actors_.size();++i) {
      const auto& actor = enc.actors_[i];
      if(!validActor(actor,enc.profiles_.size()) || !validActorState(actor,enc.lethal_) ||
         (i>0 && enc.actors_[i-1].id>=actor.id))
        invalidSnapshot();
      }
    for(const auto& actor:enc.actors_) {
      const auto* target = find(enc,actor.target);
      if(actor.target!=NoActor && (target==nullptr || target->side==actor.side))
        invalidSnapshot();
      }
    for(size_t i=0;i<enc.waitingForRange_.size();++i) {
      const auto id = enc.waitingForRange_[i];
      const auto earlier = enc.waitingForRange_.begin()+std::ptrdiff_t(i);
      if(find(enc,id)==nullptr || std::find(enc.waitingForRange_.begin(),earlier,id)!=earlier)
        invalidSnapshot();
      }
    const bool live = enc.outcome_==Outcome::Active;
    const auto survivingSide = oneSide(enc);
    const bool anyoneActive = std::any_of(enc.actors_.begin(),enc.actors_.end(),active);
    if(live==enc.pending_.empty() || (live && enc.now_==enc.deadline_) ||
       (live && (!anyoneActive || enc.settling_!=survivingSide.has_value() || (enc.settling_ && enc.committed_==0))) ||
       (enc.outcome_==Outcome::Draw && anyoneActive) ||
       (enc.outcome_==Outcome::Timeout && (enc.now_!=enc.deadline_ || !anyoneActive || survivingSide)) ||
       enc.winner_.has_value()!=(enc.outcome_==Outcome::Won) || (enc.winner_ && enc.winner_!=survivingSide))
      invalidSnapshot();
    std::vector<uint32_t> sequences;
    sequences.reserve(enc.pending_.size());
    for(const auto& p:enc.pending_) {
      if(!validPending(enc,p))
        invalidSnapshot();
      sequences.push_back(p.seq);
      }
    std::sort(sequences.begin(),sequences.end());
    if(live && (sequences.front()!=0 || std::adjacent_find(sequences.begin(),sequences.end())!=sequences.end() ||
                !std::is_heap(enc.pending_.begin(),enc.pending_.end())))
      invalidSnapshot();
  }

  static Encounter restore(std::span<const uint8_t> snapshot, bool gameplay) {
    SnapshotReader in{snapshot};
    uint32_t magic, winner;
    uint16_t version;
    bool won;
    in(magic,version);
    if(magic!=SnapshotMagic || version!=SnapshotVersion)
      invalidSnapshot();
    Encounter enc;
    in(enc.now_,enc.deadline_,enc.lethal_,enc.settling_,enc.nextSeq_,enc.processed_,enc.outcome_,won,winner);
    if(won)
      enc.winner_ = winner;
    for(auto& word:enc.rng_.s)
      in(word);
    in.resize(enc.profiles_,MaxProfiles);
    for(auto& profile:enc.profiles_)
      profileFields(in,profile);
    in.resize(enc.actors_,MaxActors);
    for(auto& actor:enc.actors_)
      actorFields(in,actor);
    in.resize(enc.waitingForRange_,MaxActors);
    for(auto& id:enc.waitingForRange_)
      in(id);
    in.resize(enc.pending_,MaxPending);
    for(auto& pending:enc.pending_) {
      pendingFields(in,pending);
      enc.committed_ += committed(pending.kind);
      }
    if(!in.bytes.empty())
      invalidSnapshot();
    validateSnapshot(enc,gameplay);
    enc.event_.changes.reserve(enc.actors_.size());
    return enc;
  }
};

Encounter start(Millis now, Millis deadline, bool lethal, uint64_t seed,
                std::span<const Combatant> actors, std::span<const AttackProfile> profiles, bool gameplay) {
  return Access::start(now,deadline,lethal,seed,actors,profiles,gameplay);
}

std::optional<Millis> nextEventAt(const Encounter& enc) { return Access::nextEventAt(enc); }
const Event& step(Encounter& enc) { return Access::step(enc); }
void sealAttacks(Encounter& enc) { Access::sealAttacks(enc); }
bool hasCommitted(const Encounter& enc) { return Access::hasCommitted(enc); }
void withdraw(Encounter& enc, uint32_t actor, Millis at) { Access::withdraw(enc,actor,at); }
std::vector<uint8_t> save(const Encounter& enc) { return Access::save(enc); }
Encounter restore(std::span<const uint8_t> snapshot, bool gameplay) { return Access::restore(snapshot,gameplay); }

}
