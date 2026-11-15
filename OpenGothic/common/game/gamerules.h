#pragma once

#include "game/aistate.h"
#include "game/damagetypes.h"
#include "game/fighttactic.h"
#include "game/aiouputpipe.h"
#include "game/scripttypes.h"
#include "world/records/focusrecord.h"
#include "world/records/guildrecord.h"
#include "world/records/itemrecord.h"
#include "world/records/npcrecord.h"
#include "world/records/spellrecord.h"

#include <Tempest/Vec>
#include <array>
#include <memory>
#include <string_view>
#include <vector>

class Bullet;
class Item;
class Npc;
class Serialize;
class VisualFx;

class GameRules {
  public:
    virtual ~GameRules() = default;

    // Symbols and kinds.
    virtual size_t           findSymbolIndex(std::string_view name) = 0;
    virtual std::string_view symbolName(size_t id) = 0;
    virtual ScriptFn         findFunction(std::string_view name) = 0;
    virtual int32_t          intSymbol(std::string_view name, int32_t fallback) = 0;
    virtual std::string_view stringSymbol(std::string_view name) = 0;
    virtual bool             isNpcInstance(size_t id) = 0;
    virtual auto             npcRecord(size_t id) -> std::shared_ptr<NpcRecord> = 0;
    virtual size_t           goldItemId() const = 0;
    virtual uint32_t         lockPickId() const = 0;

    // Creating, loading and binding NPC and item instances.
    virtual auto     createNpc(Npc& owner, size_t instance, std::string_view waypoint, uint32_t& kind) -> std::shared_ptr<NpcRecord> = 0;
    virtual uint32_t initializeInstanceNpc(const std::shared_ptr<NpcRecord>& npc, size_t instance) = 0;
    virtual auto     loadNpc(Serialize& fin, Npc* owner, uint32_t& kind) -> std::shared_ptr<NpcRecord> = 0;
    virtual void     bindNpc(const std::shared_ptr<NpcRecord>& npc) = 0;
    virtual auto     copyNpc(const std::shared_ptr<NpcRecord>& npc) -> std::shared_ptr<NpcRecord> = 0;
    virtual void     restoreNpc(const std::shared_ptr<NpcRecord>& dst, const std::shared_ptr<NpcRecord>& src, Npc& owner) = 0;
    virtual void     detachNpc(const std::shared_ptr<NpcRecord>& npc, Npc& owner) = 0;
    virtual Npc*     findNpc(NpcRecord* npc) = 0;
    virtual auto     createItem(Item& owner, size_t instance, uint32_t& kind) -> std::shared_ptr<ItemRecord> = 0;
    virtual auto     createItem(size_t instance) -> std::shared_ptr<ItemRecord> = 0;
    virtual auto     loadItem(Serialize& fin, Item& owner, uint32_t& kind) -> std::shared_ptr<ItemRecord> = 0;
    virtual void     attachItem(const std::shared_ptr<ItemRecord>& item, Item& owner) = 0;
    virtual void     setInstanceNPC(std::string_view name, Npc& npc) = 0;
    virtual void     setInstanceItem(Npc& holder, size_t itemId) = 0;

    // AI states and script calls.
    virtual const AiState& aiState(ScriptFn id) = 0;
    virtual void invokeState(const std::shared_ptr<NpcRecord>& npc, const std::shared_ptr<NpcRecord>& other, const char* name) = 0;
    virtual int  invokeState(Npc* npc, Npc* other, Npc* victim, ScriptFn fn) = 0;
    virtual void invokeItem(Npc* npc, ScriptFn fn) = 0;
    virtual int  invokeCond(Npc& npc, std::string_view func) = 0;
    virtual void invokePickLock(Npc& npc, int success, int brokenOpen) = 0;
    virtual void invokeRefreshAtInsert(Npc& npc) = 0;
    virtual void useInteractive(const std::shared_ptr<NpcRecord>& npc, std::string_view func) = 0;
    virtual bool isDead(const Npc& npc) = 0;
    virtual bool isUnconscious(const Npc& npc) = 0;
    virtual bool isTalk(const Npc& npc) = 0;
    virtual bool isAttack(const Npc& npc) const = 0;
    virtual ScriptFn playerPercAssessMagic() = 0;
    virtual const PerDist& percRanges() const = 0;

    // Guilds and attitudes.
    virtual const GuildRecord& guildVal() const = 0;
    virtual Attitude personAttitude(const Npc& a, const Npc& b) const = 0;
    virtual bool isFriendlyFire(const Npc& src, const Npc& dst) const = 0;

    // Damage and native combat tactics.
    virtual DamageValue damageValue(Npc& src, Npc& other, const Bullet* bullet, bool isSpell, const Damage& spellDamage, CollideMask mask) = 0;
    virtual float meleeMultiplier(float speedCmS, float typicalCmS, float bladeFraction, float surfaceAngleDeg, size_t targetIndex) = 0;
    virtual DamageValue damageFall(Npc& npc, float speed) = 0;
    virtual int32_t vanillaChance(int32_t hitChance, float distance) = 0;
    virtual Damage rangeDamageValue(Npc& npc) = 0;
    virtual int32_t damageTypeMask(Npc& npc) = 0;
    virtual const FightTactic& fightAi(size_t tactic) = 0;
    virtual float attackFinishDistance() = 0;

    // Spells.
    virtual const SpellRecord& spellDesc(int32_t spellId) = 0;
    virtual const VisualFx* spellVfx(int32_t spellId) = 0;
    virtual PhysicalSpell physicalSpell(int32_t spellId) const = 0;
    virtual int  invokeMana(Npc& npc, Npc* target, int mana) = 0;
    virtual int  invokeManaRelease(Npc& npc, Npc* target, int mana) = 0;
    virtual void invokeSpell(Npc& npc, Npc* target, Item& item) = 0;
    virtual CollideMask canNpcCollideWithSpell(Npc& npc, Npc* shooter, int32_t spellId) = 0;
    virtual std::string_view spellCastAnim(Npc& npc, Item& item) = 0;
    virtual int whirlwindDuration() = 0;

    // Messages and dialogue.
    virtual AiOuputPipe* openAiOuput() = 0;
    virtual AiOuputPipe* openDlgOuput(Npc& player, Npc& npc) = 0;
    virtual auto dialogChoices(std::shared_ptr<NpcRecord> self, std::shared_ptr<NpcRecord> npc, const std::vector<uint32_t>& except, bool includeImp) -> std::vector<DlgChoice> = 0;
    virtual std::string_view messageFromSvm(std::string_view id, int voice) const = 0;
    virtual uint32_t messageTime(std::string_view id) const = 0;
    virtual void printCannotUseError(Npc& npc, int32_t attribute, int32_t value) = 0;
    virtual void printCannotCastError(Npc& npc, int32_t playerMagic, int32_t itemMagic) = 0;
    virtual void printCannotBuyError(Npc& npc) = 0;
    virtual void printMobMissingItem(Npc& npc) = 0;
    virtual void printMobAnotherIsUsing(Npc& npc) = 0;
    virtual void printMobMissingKey(Npc& npc) = 0;
    virtual void printMobMissingKeyOrLockpick(Npc& npc) = 0;
    virtual void printMobMissingLockpick(Npc& npc) = 0;
    virtual void printMobTooFar(Npc& npc) = 0;
    virtual void printNothingToGet() = 0;

    // Random numbers.
    virtual uint32_t rand(uint32_t max, const Npc* actor = nullptr) = 0;
    virtual std::string randomState(const Npc* actor) const = 0;

    // Focus.
    virtual const FocusRecord& focusNorm() const = 0;
    virtual const FocusRecord& focusMelee() const = 0;
    virtual const FocusRecord& focusRange() const = 0;
    virtual const FocusRecord& focusMage() const = 0;

    // Other game rules and startup placement.
    virtual int  npcDamDiveTime() = 0;
    virtual float tradeValueMultiplier() const = 0;
    virtual BodyState schemeToBodystate(std::string_view scheme) = 0;
    virtual int  playerHotKeyScreenMap(Npc& player) = 0;
    virtual void playerHotLamePotion(Npc& player) = 0;
    virtual void playerHotLameHeal(Npc& player) = 0;
    virtual void onWldItemRemoved(const Item& item) = 0;
    virtual void fixNpcPosition(Npc& npc, float angle, float distanceBias) = 0;
    virtual void startupFloorForget(Npc& npc) = 0;
    virtual void startupFloorBeforeResetMove(Npc& npc, const Tempest::Vec3& destination) = 0;
    virtual void startupFloorBeforeKeep(Npc& npc) = 0;
    virtual std::array<int32_t,3> storyNpcTypes() = 0;
    virtual size_t scriptVmMemorySize() const = 0;
  };
