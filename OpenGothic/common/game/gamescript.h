#pragma once

#include "daedalus/vm.h"
#include "daedalus/classes.h"

#include <array>
#include <memory>
#include <optional>
#include <set>
#include <random>
#include <unordered_map>

#include <Tempest/Matrix4x4>
#include <Tempest/Painter>

#include "game/definitions/svmdefinitions.h"
#include "game/definitions/spelldefinitions.h"
#include "game/aiouputpipe.h"
#include "game/constants.h"
#include "game/scripttypes.h"
#include "game/gamerules.h"
#include "game/gothicguildtraits.h"
#include "game/questlog.h"
#include "game/ownsymbols.h"
#include "game/gamescript_startupfloor.h"
#include "game/itemdefinitionruntime.h"
#include "game/routinedefinitionruntime.h"
#include "game/npcdefinitionruntime.h"
#include "game/placementdefinitionruntime.h"
#include "game/activitydefinitionruntime.h"
#include "world/npcactivityprobe.h"
#include "asset/worldtypes.h"

class GameSession;
class World;
class Npc;
class Item;
class VisualFx;
class ParticleFx;
class Serialize;
class Gothic;

class GameScript final : public GameRules {
  public:
    GameScript(GameSession &owner);
    ~GameScript() override;

    using DlgChoice = ::DlgChoice;
    using PerDist = ::PerDist;
    using PhysicalSpell = ::PhysicalSpell;

    std::string_view symbolName(size_t id) override;
    ScriptFn         findFunction(std::string_view name) override;
    int32_t          intSymbol(std::string_view name, int32_t fallback) override;
    std::string_view stringSymbol(std::string_view name) override;
    bool             isNpcInstance(size_t id) override;
    auto             npcRecord(size_t id) -> std::shared_ptr<NpcRecord> override;
    size_t           goldItemId() const override;
    int              whirlwindDuration() override;
    void             startupFloorForget(Npc& npc) override;
    void             startupFloorBeforeResetMove(Npc& npc, const Tempest::Vec3& destination) override;
    void             startupFloorBeforeKeep(Npc& npc) override;
    std::array<int32_t,3> storyNpcTypes() override;
    size_t           scriptVmMemorySize() const override;

    DamageValue damageValue(Npc& src, Npc& other, const Bullet* bullet, bool isSpell, const Damage& spellDamage, CollideMask mask) override;
    float meleeMultiplier(float speedCmS, float typicalCmS, float bladeFraction, float surfaceAngleDeg, size_t targetIndex) override;
    DamageValue damageFall(Npc& npc, float speed) override;
    int32_t vanillaChance(int32_t hitChance, float distance) override;
    Damage rangeDamageValue(Npc& npc) override;
    int32_t damageTypeMask(Npc& npc) override;
    const FightTactic& fightAi(size_t tactic) override;
    float attackFinishDistance() override;

    bool         hasSymbolName(std::string_view fn);
    void         runStartup(std::string_view fn);
    void         runWorldInit(std::string_view fn);

    auto         createNpc(Npc& owner, size_t instance, std::string_view waypoint, uint32_t& kind) -> std::shared_ptr<NpcRecord> override;
    uint32_t     initializeInstanceNpc(const std::shared_ptr<NpcRecord>& npc, size_t instance) override;
    auto         loadNpc(Serialize& fin, Npc* owner, uint32_t& kind) -> std::shared_ptr<NpcRecord> override;
    void         bindNpc(const std::shared_ptr<NpcRecord>& npc) override;
    auto         copyNpc(const std::shared_ptr<NpcRecord>& npc) -> std::shared_ptr<NpcRecord> override;
    void         restoreNpc(const std::shared_ptr<NpcRecord>& dst, const std::shared_ptr<NpcRecord>& src, Npc& owner) override;
    void         detachNpc(const std::shared_ptr<NpcRecord>& npc, Npc& owner) override;
    Npc*         findNpc(NpcRecord* npc) override;
    auto         npcRecord(daedalus::DaedalusSymbol* symbol) -> std::shared_ptr<NpcRecord>;
    auto         createItem(Item& owner, size_t instance, uint32_t& kind) -> std::shared_ptr<ItemRecord> override;
    auto         createItem(size_t instance) -> std::shared_ptr<ItemRecord> override;
    auto         createItemForQuery(size_t instance) -> std::shared_ptr<ItemRecord>;
    auto         loadItem(Serialize& fin, Item& owner, uint32_t& kind) -> std::shared_ptr<ItemRecord> override;
    void         attachItem(const std::shared_ptr<ItemRecord>& item, Item& owner) override;
    void         initializeInstanceItem(const std::shared_ptr<daedalus::IItem>& item, size_t instance);
    void         resetVarPointers();

    void         initDialogs();
    void         saveQuests(Serialize& fout);
    void         loadQuests(Serialize& fin);
    void         saveVar(Serialize& fout);
    void         loadVar(Serialize& fin);
    void         initPerceptions();
    void         savePerc(Serialize& fout);
    void         loadPerc(Serialize& fin);

    inline auto& getVm() { return vm; }
    // symbols of ours: the AI director's story variables and npc's
    inline auto& ownSymbols() { return own; }
    auto         questLog() const -> const QuestLog&;

    const World& world() const;
    World&       world();
    uint64_t     tickCount() const;
    void         tick(uint64_t dt);

    // Draws for `actor` come from its own stream when the test bench isolated it; everything
    // else (and every NPC by default) shares randGen in tick order.
    uint32_t     rand(uint32_t max, const Npc* actor = nullptr) override;
    // the random numbers start over (the test bench)
    void         setSeed(uint32_t s) { randGen.seed(s); }
    // Body arena opt-in: the actor's native choices, damage rolls and own scripts no longer
    // depend on how many draws the rest of the world made before it.
    void         isolateRandom(const Npc& actor, uint32_t seed, uint32_t stream);
    void         clearIsolatedRandom() { isolatedRandom.clear(); }
    // Drops one actor's private stream; only the pointer value is used, the NPC may be gone.
    void         releaseIsolatedRandom(const Npc* actor) { isolatedRandom.erase(actor); }
    std::string  randomState(const Npc* actor) const override;
    void         removeItem(Item& it);

    void         setInstanceNPC (std::string_view name, Npc& npc) override;
    void         setInstanceItem(Npc& holder, size_t itemId) override;

    AiOuputPipe* openAiOuput() override;
    AiOuputPipe* openDlgOuput(Npc &player, Npc &npc) override;

    auto*            goldId() const { return itMi_Gold; }
    ScriptFn         playerPercAssessMagic() override;
    std::string_view currencyName() const { return goldTxt; }
    int              npcDamDiveTime() override;
    // cm: G2's dice for arrows - the talent's chance at the first, none at the second (AI_Constants.d)
    float            rangedChanceMinDist() const { return rangedChanceMin; }
    float            rangedChanceMaxDist() const { return rangedChanceMax; }
    auto*            lockPickSymbol() const { return ItKE_lockpick; }
    uint32_t         lockPickId() const override;

    const FocusRecord&           focusNorm()  const override { return cFocusNorm;  }
    const FocusRecord&           focusMelee() const override { return cFocusMelee; }
    const FocusRecord&           focusRange() const override { return cFocusRange; }
    const FocusRecord&           focusMage()  const override { return cFocusMage;  }
    const GuildRecord&           guildVal()   const override { return *cGuildVal;  }
    const GuildTraits&           guildTraits() const override { return gothicGuildTraits(); }

    std::string_view            menuMain() const;

    daedalus::DaedalusSymbol*     findSymbol(std::string_view s);
    daedalus::DaedalusSymbol*     findSymbol(const size_t s);
    // the hero has heard this dialog of the npc's, as when it is played (Npc_KnowsInfo)
    void                        setNpcInfoKnown(const NpcRecord& npc, const daedalus::IInfo& info);
    bool                        doesNpcKnowInfo(const NpcRecord& npc, size_t infoInstance) const;
    size_t                      findSymbolIndex(std::string_view s) override;
    size_t                      symbolsCount() const;

    const AiState&              aiState  (ScriptFn id) override;
    const SpellRecord&          spellDesc(int32_t splId) override;
    const VisualFx*             spellVfx (int32_t splId) override;
    PhysicalSpell              physicalSpell(int32_t splId) const override;

    auto dialogChoices(std::shared_ptr<NpcRecord> self, std::shared_ptr<NpcRecord> npc, const std::vector<uint32_t> &except, bool includeImp) -> std::vector<DlgChoice> override;
    auto updateDialog (const GameScript::DlgChoice &dlg, Npc &player, Npc &npc) -> std::vector<GameScript::DlgChoice>;
    void exec(const DlgChoice &dlg, Npc &player, Npc &npc);

    void  printCannotUseError         (Npc &npc, int32_t atr, int32_t nValue) override;
    void  printCannotCastError        (Npc &npc, int32_t plM, int32_t itM) override;
    void  printCannotBuyError         (Npc &npc) override;
    void  printMobMissingItem         (Npc &npc) override;
    void  printMobAnotherIsUsing      (Npc &npc) override;
    void  printMobMissingKey          (Npc &npc) override;
    void  printMobMissingKeyOrLockpick(Npc &npc) override;
    void  printMobMissingLockpick     (Npc &npc) override;
    void  printMobTooFar              (Npc &npc) override;

    void invokeState(const std::shared_ptr<NpcRecord>& hnpc, const std::shared_ptr<NpcRecord>& hother, const char* name) override;
    int  invokeState(Npc* npc, Npc* other, Npc *victim, ScriptFn fn) override;
    void invokeItem (Npc* npc, ScriptFn fn) override;
    int  invokeMana (Npc& npc, Npc* target, int mana) override;
    int  invokeManaRelease (Npc& npc, Npc* target, int mana) override;
    void invokeSpell(Npc& npc, Npc *target, Item&  fn) override;
    int  invokeCond (Npc& npc, std::string_view func) override;
    void invokePickLock(Npc& npc, int bSuccess, int bBrokenOpen) override;
    void invokeRefreshAtInsert(Npc& npc) override;
    auto canNpcCollideWithSpell(Npc& npc, Npc* shooter, int32_t spellId) -> CollideMask override;

    int  playerHotKeyScreenMap(Npc& pl) override;
    void playerHotLamePotion(Npc& pl) override;
    void playerHotLameHeal(Npc& pl) override;

    const PerDist& percRanges() const override { return perceptionRanges; }
    bool usesNativeQuietSound(ScriptFn callback) const override;

    bool isDead       (const Npc &pl) override;
    bool isUnconscious(const Npc &pl) override;
    bool isTalk       (const Npc &pl) override;
    bool isAttack     (const Npc &pl) const override;
    // the npc's important dialogs count as told to the player: it will not stop him to talk (test helper)
    void skipImportantInfos(Npc& npc, Npc& pl);

    std::string_view spellCastAnim(Npc& npc, Item&  fn) override;
    std::string_view messageFromSvm(std::string_view id,int voice) const override;
    std::string_view messageByName (std::string_view id) const;
    uint32_t         messageTime   (std::string_view id) const override;

    void     printNothingToGet() override;
    float    tradeValueMultiplier() const override { return tradeValMult; }
    void     useInteractive(const std::shared_ptr<NpcRecord>& hnpc, std::string_view func) override;
    Attitude guildAttitude(const Npc& p0,const Npc& p1) const;
    Attitude personAttitude(const Npc& p0,const Npc& p1) const override;
    bool     isFriendlyFire(const Npc& src, const Npc& dst) const override;

    BodyState schemeToBodystate(std::string_view sc) override;

    void      onWldItemRemoved(const Item& itm) override;
    void      fixNpcPosition(Npc& npc, float angle0, float distBias) override;
    StartupFloor& startupFloor() { return floorDefer; }

    const ActivityEvents::Record* eventActivity(ScriptFn init) override;
    std::vector<std::string_view> eventActivityNames();
    void         runActivityGesture(Npc& npc, const ActivityEvents::Record& activity) override;
    bool         routineTimerDue(const Npc& npc, ScriptFn loop) const override;

    // The scheduler checks these windows in order; the society export preserves that order.
    static constexpr const char* monsterScheduler = "ZS_MM_ALLSCHEDULER";
    struct MonsterRoutineWindow {
      const char* state;
      const char* start;
      const char* end;
      size_t      stockStart;
      size_t      stockEnd;
      };
    // Native phases use verified stock slots. Society export resolves the field names from the loaded program.
    static constexpr MonsterRoutineWindow monsterRoutineWindows[] = {
      {"ZS_MM_RTN_SLEEP",     "AIV_MM_SLEEPSTART",     "AIV_MM_SLEEPEND",     30, 31},
      {"ZS_MM_RTN_REST",      "AIV_MM_RESTSTART",      "AIV_MM_RESTEND",      32, 33},
      {"ZS_MM_RTN_ROAM",      "AIV_MM_ROAMSTART",      "AIV_MM_ROAMEND",      34, 35},
      {"ZS_MM_RTN_EATGROUND", "AIV_MM_EATGROUNDSTART", "AIV_MM_EATGROUNDEND", 36, 37},
      {"ZS_MM_RTN_WUSEL",     "AIV_MM_WUSELSTART",     "AIV_MM_WUSELEND",     38, 39},
      {"ZS_MM_RTN_ORCSIT",    "AIV_MM_ORCSITSTART",    "AIV_MM_ORCSITEND",    40, 41},
      };

  private:
    enum MonsterRoutine { MonsterSleep, MonsterRest, MonsterRoam, MonsterEatGround, MonsterWusel, MonsterOrcSit };
    friend struct NpcDefinitionContract;
    friend class StoryRuntime;
    bool invokeActivity(Npc& npc, uint32_t function, NpcActivityProbe::Phase& probe);
    bool invokeNativeRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& probe);
    bool runMovementRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runFearOrSleepSpell(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runFreezeOrWhirlwindSpell(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    void stopInterruptedSpell(Npc& npc, bool resumeRoutine = false);
    bool invokeNativeStatusPerception(Npc& npc, const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& phase);
    void stopMagicFear(Npc& npc);
    void stopMagicSleep(Npc& npc);
    void sayMagicReaction(Npc& npc, std::string_view line, bool overlay);
    void raisePaladinZombie(Npc& npc);
    bool runItemRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    int32_t giveInventoryItems(const std::shared_ptr<daedalus::INpc>& giver,
                               const std::shared_ptr<daedalus::INpc>& taker, int32_t item, int32_t amount);
    bool runPointRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runBanditCamp(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runDormantGolem(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool invokeNativeRoutineReaction(Npc& npc, const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& phase);
    void wakeDormantGolem(Npc& npc);
    void assessSleepTalk(Npc& npc);
    void runPortalEarthquake(Npc& npc);
    void resetRoutineNpc(const std::shared_ptr<daedalus::INpc>& self);
    bool npcBodyStateContains(const std::shared_ptr<daedalus::INpc>& self, int32_t state);
    bool runSocialRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runTalk(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runObservePlayer(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runClearRoom(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runHumanReaction(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runHumanAttack(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    void sayAttackReason();
    void sayAttackEnd();
    bool runHumanFlee(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runWatchFight(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runFakeGuildComment(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runHumanPlunder(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runControlSpell(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    void stopControlSpell(const std::shared_ptr<daedalus::INpc>& self, bool continueRoutine);
    void validateCombatTarget(const std::shared_ptr<daedalus::INpc>& self);
    bool runDeadState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runUnconsciousState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runHealSelf(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runSpellVictimState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterScheduler(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterAttack(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterThreat(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterFlee(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterHunt(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterEatBody(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterDragonRest(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterSheepFollow(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMonsterSummoned(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool nativeMonsterPerceptionsEnabled();
    bool invokeNativeMonsterPerception(const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& probe);
    void runMonsterAssessBody();
    void runMonsterAssessDamage();
    void runMonsterAssessEnemy();
    void runMonsterAssessOthersDamage();
    void runMonsterAssessPlayer();
    void runMonsterAssessWarn();
    void runMonsterAssessSurprise();
    void runMonsterAssessQuietSoundSleep();
    void runMonsterSummonedAssessTalk();
    bool monsterWantsToEat(const std::shared_ptr<daedalus::INpc>& monster, const std::shared_ptr<daedalus::INpc>& body);
    bool monsterPredatorFoundPrey(const std::shared_ptr<daedalus::INpc>& predator, const std::shared_ptr<daedalus::INpc>& prey);
    bool runRoamingRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runShrinkState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    bool runMagicHurtState(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result);
    void magicHurtNpc(const std::shared_ptr<daedalus::INpc>& attacker, const std::shared_ptr<daedalus::INpc>& target,
                      int32_t damage);
    bool usesNativeShrinkSpell(const Item* item) const;
    int runShrinkMana(Npc& caster, int mana, bool release);
    void castShrink(Npc& caster, Npc* target);
    std::optional<int> runStockSpellMana(Npc& caster, int mana);
    bool runStockSpellCast(Npc& caster, std::string_view function, int32_t level);
    void assessMagic(Npc& npc);
    void givePlayerXp(int32_t amount);
    void loadNativeQuestLog(const std::function<bool()>& enabled);
    void closeQuestTopic(std::string_view topic, int32_t mission, int32_t common, int32_t endChapter);
    bool monsterRoutineWindowActive(const Npc& npc, const MonsterRoutineWindow& window);
    void enableMonsterRoutinePerceptions(Npc& npc);
    bool isMonsterMage(const std::shared_ptr<daedalus::INpc>& npc);
    void beginMonsterRoutine(Npc& npc, bool desynchronize = true, WalkBit mode = WalkBit::WM_Walk);
    bool endMonsterRoutine(Npc& npc, MonsterRoutine routine);
    void removeRavensStoneGuardians(Npc& npc);
    void loadNativeRoutines();
    void loadNativeRoutineExchange();

    using NativeRoutineCallback = bool (GameScript::*)(Npc&, const daedalus::DaedalusSymbol&, int&);
    enum class RoutineWake : uint8_t { Timer, Sleep, Smalltalk, Herald, Concert };
    struct NativeRoutinePhase {
      std::string_view state;
      NativeRoutineCallback callback;
      uint32_t minStateTime = 0;
      RoutineWake wake = RoutineWake::Timer;
      bool enters = false;
      };
    void refreshNpcAtInsert(Npc& npc);
    void loadNativeKillNpc();
    void loadNativeNpcGlobals();
    void loadNativeMonsterAttitudes();
    void loadNativeWorldEntry();
    enum class ChapterWorld : uint8_t { New, Old, Addon };
    void enterChapterWorld(ChapterWorld world);
    void changeChapter(int32_t chapter, int32_t level);
    void invokeKillNpc(int32_t symbol);
    void killNpc(int32_t symbol);
    void initializeInstanceNpc(const std::shared_ptr<daedalus::INpc>& npc, size_t instance, bool useRecords=true);
    void initializeNpcDefinition(const std::shared_ptr<daedalus::INpc>& npc, daedalus::DaedalusSymbol* symbol, bool useRecords);
    bool startHotkeyPotion(Npc& pl, bool mana);

    template<typename T>
    struct DetermineSignature {
      using signature = void();
      };

    template <typename C, typename R, typename ... P>
    struct DetermineSignature<R(C::*)(P...)> {
      using signature = R(P...);
      };

    struct GlobalOutput : AiOuputPipe {
      explicit GlobalOutput(GameScript& owner):owner(owner){}

      bool output   (Npc &npc, std::string_view text) override;
      bool outputSvm(Npc& npc, std::string_view text, std::string_view svm, Npc* target) override;
      bool outputOv (Npc& npc, std::string_view text, std::string_view svm, Npc* target) override;
      bool printScr (Npc& npc, int time, std::string_view msg, int x,int y, std::string_view font) override;
      bool isFinished() override;

      bool close() override { return true; }

      GameScript& owner;
      };

    template <class F>
    void bindExternal(const std::string& name, F function) {
      const size_t floorId = floorDefer.registerExternal(name,StartupFloor::classify(name));
      vm.register_external(name, std::function<typename DetermineSignature<F>::signature> (
                                   [this, function, floorId](auto ... v) {
                                     if(floorDefer.active())
                                       floorBarrier(floorId,v...);
                                     return (this->*function)(v...);
                                     }));
      }

    // StartupFloor: the NPC arguments of an external about to run.
    template <class ... A>
    void floorBarrier(size_t id, const A& ... v) {
      Npc*   args[sizeof...(A)+1] = {};
      size_t count = 0;
      (floorArg(args,count,v), ...);
      floorDefer.external(id,args,count);
      }
    template <class T>
    void floorArg(Npc**, size_t&, const T&) {}
    void floorArg(Npc** args, size_t& count, const std::shared_ptr<daedalus::INpc>& v);
    void floorArg(Npc** args, size_t& count, const std::shared_ptr<daedalus::DaedalusInstance>& v);

    void  initCommon();
    void  initSettings();
    void  loadDialogOU();
    auto  readDialogInfo(daedalus::DaedalusSymbol& sym) -> std::shared_ptr<daedalus::IInfo>;

    Item* findItem(daedalus::IItem* handle);
    Item* findItemById(size_t id);
    Item* findItemById(const std::shared_ptr<daedalus::DaedalusInstance>& handle);
    Npc*  findNpc(daedalus::DaedalusSymbol* s);
    Npc*  findNpc(daedalus::INpc*           handle);
    Npc*  findNpc(const std::shared_ptr<daedalus::INpc>& handle);
    Npc*  findNpcById (const std::shared_ptr<daedalus::DaedalusInstance>& handle);
    Npc*  findNpcById (size_t id);
    auto  findInfo    (size_t id) -> daedalus::IInfo*;
    auto  findFocus(std::string_view name) -> daedalus::IFocus;

    void storeItem(Item* it);
    bool invokeNativeMobState(const daedalus::DaedalusSymbol& symbol);

    bool aiOutput   (Npc &from, std::string_view name, bool overlay);
    bool aiOutputSvm(Npc &from, std::string_view name, std::string_view svm, Npc* target, bool overlay);

    bool searchScheme(std::string_view sc, std::string_view listName);
    struct QuietSoundReaction {
      ScriptFn callback;
      int32_t heightLimit = 0;
      int32_t indoorHeightLimit = 0;
      };
    bool nativeRoomReactionsEnabled() const;
    bool invokeNativeRoomPerception(const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& phase);
    void loadNativeRoomReaction();
    void loadNativeHumanPerceptions();
    void bindCrimeGlobals();
    bool nativeHumanPerceptionsEnabled() const;
    bool invokeNativeHumanPerception(ScriptFn fn, NpcActivityProbe::Phase& phase);
    int32_t runDrawWeaponReaction();
    void runThreatReaction();
    void runWarnReaction();
    void runFightSoundReaction();
    void runFightDefeat();
    void runCheerFight();
    bool hasAttackReasonToKill(const std::shared_ptr<daedalus::INpc>& npc);
    bool wantsToAttackSheepKiller(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    bool wantsToAttackThief(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    bool wantsToAttackMurderer(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    bool belongsToBanditCamp(const std::shared_ptr<daedalus::INpc>& npc);
    bool isNpcDownInScript(const std::shared_ptr<daedalus::INpc>& npc);
    void runDamageReaction();
    int32_t runEnemyReaction();
    void runPlayerAssessment();
    void setCombatAttitude(const std::shared_ptr<daedalus::INpc>& npc, int32_t attitude);
    void sayGuildGreeting(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    void sayAmbientGreeting(std::string_view line);
    void runFollowerPlayerAssessment();
    void runGuidePlayerAssessment();
    void runCompanionFightSound(bool follower);
    int32_t diegoRouteBoundary(int32_t tolerance);
    void runPropertyCrime(bool usingMob);
    void runMoveMobReaction();
    void runSurpriseReaction();
    void runMurderReaction();
    void startHumanWarning(const std::shared_ptr<daedalus::INpc>& self, ScriptFn state);
    int32_t botheredByWeapon(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    void loadNativeEquipment();
    bool applyArmorSetEffect(Npc& npc, uint32_t callback);
    bool applyFightSkillItemEffect(Npc& npc, uint32_t callback);
    void createAmmo(const std::shared_ptr<daedalus::INpc>& self);
    void selectWeapon(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    bool chooseCombatSpell(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    bool isUndeadNpc(const std::shared_ptr<daedalus::INpc>& npc);
    bool isEvilNpc(const std::shared_ptr<daedalus::INpc>& npc);
    void loadNativeTalkReaction();
    bool canRunNativeTalkReaction();
    bool invokeNativeTalkPerception(const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& phase);
    void runAssessTalk();
    bool refusesTalk(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    void readySpell(const std::shared_ptr<daedalus::INpc>& self, int32_t spell, int32_t mana);
    int32_t runEnterRoomReaction();
    int32_t runExitIfRoomLeft();
    void runPortalCollisionReaction();
    void runClearRoomTalk();
    bool wantsToAttackRoomIntruder(const std::shared_ptr<daedalus::INpc>& self);
    int32_t playerCrime(const std::shared_ptr<daedalus::INpc>& self);
    void memorizePlayerCrime(const std::shared_ptr<daedalus::INpc>& witness, const std::shared_ptr<daedalus::INpc>& offender, int32_t crime);
    int32_t currentAbsolutionLevel(const std::shared_ptr<daedalus::INpc>& self);
    void adjustPetzCrime(const std::shared_ptr<daedalus::INpc>& witness, std::optional<int32_t> newCrime);
    bool isToughGuy(const std::shared_ptr<daedalus::INpc>& self);
    void startNpcAttack(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other,
                        int32_t reason, int32_t wait);
    void runQuietSoundReaction(Npc& listener);

    void setPerceptionPreset(bool normal);
    void clearPerceptions(const std::shared_ptr<daedalus::INpc>& self);
    bool isGateGuard(const std::shared_ptr<daedalus::INpc>& self);
    bool isBotheredByPlayerRoomGuild(const std::shared_ptr<daedalus::INpc>& self);
    bool isPlayerFakeBandit(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    bool isPlayerFakePirate(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    bool hasFakeGuild(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    void desynchronizeNpc();
    void sayToNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other,
                  std::string_view line, bool overlay = false);
    void turnToNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    void lookAtNpc(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other);
    void stopLookAtNpc(const std::shared_ptr<daedalus::INpc>& self);
    static daedalus::DaedalusVm createVm(Gothic& gothic, ItemDefinitionRuntime::Definitions& items,
                                       RoutineDefinitionRuntime::Definitions& routines,
                                       NpcDefinitionRuntime::Definitions& npcs, PlacementDefinitionRuntime::Definitions& placements,
                                       ActivityDefinitionRuntime::Definitions& activities, QuietSoundReaction& quietSound,
                                       bool& nativeRoutineProgram);
    int initializeRoutine(const std::shared_ptr<daedalus::INpc>& npc, daedalus::DaedalusSymbol* function);
    bool game_initgerman     ();
    bool game_initenglish    ();
    int  hlp_random          (int bound);
    uint32_t rollRandom(uint32_t bound, const Npc* actor, bool script);
    bool hlp_isvalidnpc      (std::shared_ptr<daedalus::INpc> npcRef);
    bool hlp_isitem          (std::shared_ptr<daedalus::IItem> itemRef, int instanceSymbol);
    bool hlp_isvaliditem     (std::shared_ptr<daedalus::IItem> itemRef);
    int  hlp_getinstanceid   (std::shared_ptr<daedalus::DaedalusInstance> instance);
    auto hlp_getnpc          (int instanceSymbol) -> std::shared_ptr<daedalus::INpc>;

    void wld_insertitem      (int itemInstance, std::string_view spawnpoint);
    void wld_insertnpc       (int npcInstance, std::string_view spawnpoint);
    void wld_removenpc       (int npcInstance);
    void wld_settime         (int hour, int minute);
    int  wld_getday          ();
    void wld_playeffect      (std::string_view visual, std::shared_ptr<daedalus::DaedalusInstance> sourceId, std::shared_ptr<daedalus::DaedalusInstance> targetId, int effectLevel, int damage, int damageType, int isProjectile);
    void wld_stopeffect      (std::string_view visual);
    int  wld_getplayerportalguild();
    int  wld_getformerplayerportalguild();
    void wld_setguildattitude (int gil1, int att, int gil2);
    int  wld_getguildattitude (int g1, int g0);
    void wld_exchangeguildattitudes (std::string_view name);
    bool wld_istime           (int hour0, int min0, int hour1, int min1);
    bool wld_isfpavailable    (std::shared_ptr<daedalus::INpc> self, std::string_view name);
    bool wld_isnextfpavailable(std::shared_ptr<daedalus::INpc> self, std::string_view name);
    bool wld_ismobavailable   (std::shared_ptr<daedalus::INpc> self, std::string_view name);
    void wld_setmobroutine    (int h, int m, std::string_view name, int st);
    int  wld_getmobstate      (std::shared_ptr<daedalus::INpc> npc, std::string_view scheme);
    void wld_assignroomtoguild(std::string_view name, int g);
    bool wld_detectnpc        (std::shared_ptr<daedalus::INpc> npcRef, int inst, int state, int guild);
    bool wld_detectnpcex      (std::shared_ptr<daedalus::INpc> npcRef, int inst, int state, int guild, int player);
    bool wld_detectitem       (std::shared_ptr<daedalus::INpc> npcRef, int flags);
    void wld_spawnnpcrange    (std::shared_ptr<daedalus::INpc> npcRef, int clsId, int count, float lifeTime);
    void wld_sendtrigger      (std::string_view triggerTarget);
    void wld_senduntrigger    (std::string_view triggerTarget);
    bool wld_israining        ();

    void mdl_setvisual       (std::shared_ptr<daedalus::INpc> npcRef, std::string_view visual);
    void mdl_setvisualbody   (std::shared_ptr<daedalus::INpc> npcRef, std::string_view body, int bodyTexNr, int bodyTexColor, std::string_view head, int headTexNr, int teethTexNr, int armor);
    void mdl_setmodelfatness (std::shared_ptr<daedalus::INpc> npcRef, float fat);
    void mdl_applyoverlaymds (std::shared_ptr<daedalus::INpc> npcRef, std::string_view overlayname);
    void mdl_applyoverlaymdstimed(std::shared_ptr<daedalus::INpc> npcRef, std::string_view overlayname, int ticks);
    void mdl_removeoverlaymds(std::shared_ptr<daedalus::INpc> npcRef, std::string_view overlayname);
    void mdl_setmodelscale   (std::shared_ptr<daedalus::INpc> npcRef, float x, float y, float z);
    void mdl_startfaceani    (std::shared_ptr<daedalus::INpc> npcRef, std::string_view ani, float intensity, float time);
    void mdl_applyrandomani  (std::shared_ptr<daedalus::INpc> npcRef, std::string_view s1, std::string_view s0);
    void mdl_applyrandomanifreq(std::shared_ptr<daedalus::INpc> npcRef, std::string_view s1, float f0);
    void mdl_applyrandomfaceani(std::shared_ptr<daedalus::INpc> npcRef, std::string_view name, float timeMin, float timeMinVar, float timeMax, float timeMaxVar, float probMin);

    void npc_settofightmode  (std::shared_ptr<daedalus::INpc> npcRef, int weaponSymbol);
    void npc_settofistmode   (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_isinstate       (std::shared_ptr<daedalus::INpc> npcRef, int stateFn);
    bool npc_isinroutine     (std::shared_ptr<daedalus::INpc> npcRef, int stateFn);
    bool npc_wasinstate      (std::shared_ptr<daedalus::INpc> npcRef, int stateFn);
    int  npc_getdisttowp     (std::shared_ptr<daedalus::INpc> npcRef, std::string_view wpname);
    void npc_exchangeroutine (std::shared_ptr<daedalus::INpc> npcRef, std::string_view rname);
    bool npc_isdead          (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_knowsinfo       (std::shared_ptr<daedalus::INpc> npcRef, int infoinstance);
    void npc_settalentskill  (std::shared_ptr<daedalus::INpc> npcRef, int t, int lvl);
    int  npc_gettalentskill  (std::shared_ptr<daedalus::INpc> npcRef, int skillId);
    void npc_settalentvalue  (std::shared_ptr<daedalus::INpc> npcRef, int t, int lvl);
    int  npc_gettalentvalue  (std::shared_ptr<daedalus::INpc> npcRef, int skillId);
    void npc_setrefusetalk   (std::shared_ptr<daedalus::INpc> npcRef, int timeSec);
    bool npc_refusetalk      (std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_hasitems        (std::shared_ptr<daedalus::INpc> npcRef, int itemId);
    bool npc_hasspell        (std::shared_ptr<daedalus::INpc> npcRef, int splId);
    int  npc_getinvitem      (std::shared_ptr<daedalus::INpc> npcRef, int itemId);
    int  npc_getinvitembyslot(std::shared_ptr<daedalus::INpc> npcRef, int cat, int slotnr);
    int  npc_removeinvitem   (std::shared_ptr<daedalus::INpc> npcRef, int itemId);
    int  npc_removeinvitems  (std::shared_ptr<daedalus::INpc> npcRef, int itemId, int amount);
    int  npc_getbodystate    (std::shared_ptr<daedalus::INpc> npcRef);
    std::shared_ptr<daedalus::INpc> npc_getlookattarget (std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_getdisttonpc    (std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef);
    bool npc_hasequippedarmor(std::shared_ptr<daedalus::INpc> npcRef);
    void npc_setperctime     (std::shared_ptr<daedalus::INpc> npcRef, float sec);
    void npc_percenable      (std::shared_ptr<daedalus::INpc> npcRef, int pr, int fn);
    void npc_percdisable     (std::shared_ptr<daedalus::INpc> npcRef, int pr);
    std::string npc_getnearestwp    (std::shared_ptr<daedalus::INpc> npcRef);
    std::string npc_getnextwp(std::shared_ptr<daedalus::INpc> npcRef);
    void npc_clearaiqueue    (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_isplayer        (std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_getstatetime    (std::shared_ptr<daedalus::INpc> npcRef);
    void npc_setstatetime    (std::shared_ptr<daedalus::INpc> npcRef, int val);
    void npc_changeattribute (std::shared_ptr<daedalus::INpc> npcRef, int atr, int val);
    bool npc_isonfp          (std::shared_ptr<daedalus::INpc> npcRef, std::string_view val);
    int  npc_getheighttonpc  (std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef);
    std::shared_ptr<daedalus::IItem> npc_getequippedmeleeweapon (std::shared_ptr<daedalus::INpc> npcRef);
    std::shared_ptr<daedalus::IItem> npc_getequippedrangedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    std::shared_ptr<daedalus::IItem> npc_getequippedarmor(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_canseenpc       (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef);
    bool npc_canseenpcfreelos(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef);
    bool npc_canseeitem      (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itemRef);
    bool npc_hasequippedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_hasequippedmeleeweapon(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_hasequippedrangedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_getactivespell  (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_getactivespellisscroll(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_isinfightmode   (std::shared_ptr<daedalus::INpc> npcRef, int modeI);
    int  npc_getactivespellcat(std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_setactivespellinfo(std::shared_ptr<daedalus::INpc> npcRef, int v);
    int  npc_getactivespelllevel(std::shared_ptr<daedalus::INpc> npcRef);
    void npc_settarget       (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef);
    bool npc_gettarget       (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_getnexttarget   (std::shared_ptr<daedalus::INpc> npcRef);
    void npc_sendpassiveperc (std::shared_ptr<daedalus::INpc> npcRef, int id, std::shared_ptr<daedalus::INpc> victimRef, std::shared_ptr<daedalus::INpc> otherRef);
    void npc_sendsingleperc  (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef, int id);
    bool npc_checkinfo       (std::shared_ptr<daedalus::INpc> npcRef, int imp);
    int  npc_getportalguild  (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_isinplayersroom (std::shared_ptr<daedalus::INpc> npcRef);
    std::shared_ptr<daedalus::IItem> npc_getreadiedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_hasreadiedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_hasreadiedmeleeweapon(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_hasreadiedrangedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_hasrangedweaponwithammo(std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_isdrawingspell  (std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_isdrawingweapon (std::shared_ptr<daedalus::INpc> npcRef);
    void npc_perceiveall     (std::shared_ptr<daedalus::INpc> npcRef);
    void npc_stopani         (std::shared_ptr<daedalus::INpc> npcRef, std::string_view name);
    int  npc_settrueguild    (std::shared_ptr<daedalus::INpc> npcRef, int gil);
    int  npc_gettrueguild    (std::shared_ptr<daedalus::INpc> npcRef);
    void npc_clearinventory  (std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_getattitude     (std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef);
    int  npc_getpermattitude (std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef);
    void npc_setattitude     (std::shared_ptr<daedalus::INpc> npcRef, int att);
    void npc_settempattitude (std::shared_ptr<daedalus::INpc> npcRef, int att);
    bool npc_hasbodyflag     (std::shared_ptr<daedalus::INpc> npcRef, int bodyflag);
    int  npc_getlasthitspellid(std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_getlasthitspellcat(std::shared_ptr<daedalus::INpc> npcRef);
    void npc_playani         (std::shared_ptr<daedalus::INpc> npcRef, std::string_view name);
    bool npc_isdetectedmobownedbynpc(std::shared_ptr<daedalus::INpc> usrRef, std::shared_ptr<daedalus::INpc> npcRef);
    std::string npc_getdetectedmob  (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_ownedbynpc      (std::shared_ptr<daedalus::IItem> itmRef, std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_canseesource    (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_isincutscene    (std::shared_ptr<daedalus::INpc> npcRef);
    int  npc_getdisttoitem   (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef);
    int  npc_getheighttoitem (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef);
    int  npc_getdisttoplayer (std::shared_ptr<daedalus::INpc> npcRef);
    bool npc_isdetectedmobownedbyguild(std::shared_ptr<daedalus::INpc> npcRef, int guild);

    void ai_output           (std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> targetRef, std::string_view outputname);
    void ai_stopprocessinfos (std::shared_ptr<daedalus::INpc> selfRef);
    void ai_processinfos     (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_standup          (std::shared_ptr<daedalus::INpc> selfRef);
    void ai_standupquick     (std::shared_ptr<daedalus::INpc> selfRef);
    void ai_continueroutine  (std::shared_ptr<daedalus::INpc> selfRef);
    void ai_stoplookat       (std::shared_ptr<daedalus::INpc> selfRef);
    void ai_lookat           (std::shared_ptr<daedalus::INpc> selfRef, std::string_view waypoint);
    void ai_lookatnpc        (std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef);
    void ai_removeweapon     (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_unreadyspell     (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_turnaway         (std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef);
    void ai_turntonpc        (std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef);
    void ai_whirlaround      (std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef);
    void ai_outputsvm        (std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> targetRef, std::string_view name);
    void ai_outputsvm_overlay(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> targetRef, std::string_view name);
    void ai_startstate       (std::shared_ptr<daedalus::INpc> selfRef, int func, int state, std::string_view wp);
    void ai_playani          (std::shared_ptr<daedalus::INpc> npcRef, std::string_view name);
    void ai_setwalkmode      (std::shared_ptr<daedalus::INpc> npcRef, int modeBits);
    void ai_wait             (std::shared_ptr<daedalus::INpc> npcRef, float ms);
    void ai_waitms           (std::shared_ptr<daedalus::INpc> npcRef, int ms);
    void ai_aligntowp        (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_gotowp           (std::shared_ptr<daedalus::INpc> npcRef, std::string_view waypoint);
    void ai_gotofp           (std::shared_ptr<daedalus::INpc> npcRef, std::string_view waypoint);
    void ai_playanibs        (std::shared_ptr<daedalus::INpc> npcRef, std::string_view ani, int bs);
    void ai_equiparmor       (std::shared_ptr<daedalus::INpc> npcRef, int id);
    void ai_equipbestarmor   (std::shared_ptr<daedalus::INpc> npcRef);
    int  ai_equipbestmeleeweapon (std::shared_ptr<daedalus::INpc> npcRef);
    int  ai_equipbestrangedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    bool ai_usemob           (std::shared_ptr<daedalus::INpc> npcRef, std::string_view tg, int state);
    void ai_teleport         (std::shared_ptr<daedalus::INpc> npcRef, std::string_view tg);
    void ai_stoppointat      (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_drawweapon       (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_readymeleeweapon (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_readyrangedweapon(std::shared_ptr<daedalus::INpc> npcRef);
    void ai_readyspell       (std::shared_ptr<daedalus::INpc> npcRef, int spell, int mana);
    void ai_attack            (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_flee             (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_dodge            (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_unequipweapons   (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_unequiparmor     (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_gotonpc          (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> toRef);
    void ai_gotonextfp       (std::shared_ptr<daedalus::INpc> npcRef, std::string_view to);
    void ai_aligntofp        (std::shared_ptr<daedalus::INpc> npcRef);
    void ai_useitem          (std::shared_ptr<daedalus::INpc> npcRef, int item);
    void ai_useitemtostate   (std::shared_ptr<daedalus::INpc> npcRef, int item, int state);
    void ai_setnpcstostate   (std::shared_ptr<daedalus::INpc> npcRef, int state, int radius);
    void ai_finishingmove    (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> othRef);
    void ai_takeitem         (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef);
    void ai_gotoitem         (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef);
    void ai_pointat          (std::shared_ptr<daedalus::INpc> npcRef, std::string_view waypoint);
    void ai_pointatnpc       (std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef);
    int  ai_printscreen      (std::string_view msg, int posx, int posy, std::string_view font, int timesec);

    int  mob_hasitems        (std::string_view tag, int item);

    void ta_min              (std::shared_ptr<daedalus::INpc> npcRef, int start_h, int start_m, int stop_h, int stop_m, int action, std::string_view waypoint);

    void log_createtopic     (std::string_view topicName, int section);
    void log_settopicstatus  (std::string_view topicName, int status);
    void log_addentry        (std::string_view topicName, std::string_view entry);

    void equipitem           (std::shared_ptr<daedalus::INpc> npcRef, int cls);
    void createinvitem       (std::shared_ptr<daedalus::INpc> npcRef, int itemInstance);
    void createinvitems      (std::shared_ptr<daedalus::INpc> npcRef, int itemInstance, int amount);

    void perc_setrange       (int perc, int dist);

    void info_addchoice      (int infoInstance, std::string_view text, int func);
    void info_clearchoices   (int infoInstance);
    bool infomanager_hasfinished();

    void snd_play            (std::string_view fileS);
    void snd_play3d          (std::shared_ptr<daedalus::INpc> npcRef, std::string_view fileS);

    void exitsession         ();

    void sort(std::vector<DlgChoice>& dlg);
    // a dialog's condition, information or choice: the story graph runs it if it takes it, else the VM
    int32_t dialogFunction(daedalus::DaedalusSymbol* fn, bool returns);

    void saveSym(Serialize& fout, daedalus::DaedalusSymbol& s);

    void onWldInstanceRemoved(const daedalus::DaedalusInstance* obj);
    void makeCurrent(Item* w);

    GameSession&                                                owner;
    ItemDefinitionRuntime::Definitions                          itemDefinitions;
    RoutineDefinitionRuntime::Definitions                       routineDefinitions;
    NpcDefinitionRuntime::Definitions                           npcDefinitions;
    PlacementDefinitionRuntime::Definitions                     placementDefinitions;
    ActivityDefinitionRuntime::Definitions                      activityDefinitions;
    bool                                                        activityEvents = ActivityDefinitionRuntime::events();
    QuietSoundReaction                                          quietSound;
    bool                                                        nativeRoutineProgram = false;
    ScriptFn                                                    drawWeaponCallback;
    ScriptFn                                                    threatCallback;
    ScriptFn                                                    warnCallback;
    ScriptFn                                                    fightSoundCallback;
    ScriptFn                                                    fightDefeatCallback;
    ScriptFn                                                    cheerFightCallback;
    ScriptFn                                                    damageCallback;
    ScriptFn                                                    enemyCallback;
    ScriptFn                                                    playerCallback;
    ScriptFn                                                    followPlayerCallback, guidePlayerCallback;
    ScriptFn                                                    followFightSoundCallback, guideFightSoundCallback;
    ScriptFn                                                    theftCallback;
    ScriptFn                                                    useMobCallback;
    ScriptFn                                                    moveMobCallback;
    ScriptFn                                                    surpriseCallback;
    ScriptFn                                                    murderCallback;
    ScriptFn                                                    stopShortZapCallback;
    ScriptFn                                                    restartTentacleCallback;
    std::unordered_map<uint32_t,NativeRoutinePhase>              nativeRoutinePhases;
    // The stock VM aliases this storage and is destroyed before it.
    int32_t                                                     storyChapter = 0;
    std::array<int32_t,5>                                       crimeAbsolutionLevels{};
    std::array<int32_t,19>                                      petzCrimeCounters{};
    daedalus::DaedalusVm                                          vm;
    StartupFloor                                                floorDefer{*this};
    OwnSymbols                                                  own{vm};
    int32_t                                                     vmLang = -1;
    std::mt19937                                                randGen;
    std::unordered_map<const Npc*,std::mt19937>                 isolatedRandom;
    NpcProcessPolicy                                            aiProcessPolicy = NpcProcessPolicy::AiNormal;

    std::unique_ptr<SpellDefinitions>                           spells;
    std::unique_ptr<SvmDefinitions>                             svm;
    uint64_t                                                    svmBarrier=0;

    std::set<std::pair<size_t,size_t>>                          dlgKnownInfos;
    std::vector<std::shared_ptr<daedalus::IInfo>>                 dialogsInfo;
    std::vector<std::shared_ptr<asset::CutsceneBlock>>           dialogs;
    std::unordered_map<size_t,AiState>                          aiStates;
    std::unique_ptr<AiOuputPipe>                                aiDefaultPipe;

    QuestLog                                                    quests;
    daedalus::DaedalusSymbol*                                     itMi_Gold = nullptr;
    daedalus::DaedalusSymbol*                                     ItKE_lockpick = nullptr;
    daedalus::DaedalusSymbol*                                     B_RefreshAtInsert = nullptr;
    float                                                       tradeValMult = 0.3f;
    daedalus::DaedalusSymbol*                                     spellFxInstanceNames = nullptr;
    daedalus::DaedalusSymbol*                                     spellFxAniLetters = nullptr;
    std::string                                                 goldTxt;
    float                                                       viewTimePerChar = 0.5;
    float                                                       rangedChanceMin = 1500;
    float                                                       rangedChanceMax = 4500;
    mutable std::unordered_map<std::string,uint32_t>            msgTimings;
    size_t                                                      gilTblSize=0;
    size_t                                                      gilCount=0;
    std::vector<int32_t>                                        gilAttitudes;
    int                                                         aiOutOrderId=0;

    PerDist                                                     perceptionRanges;

    size_t                                                      ZS_Dead=0;
    size_t                                                      ZS_Unconscious=0;
    size_t                                                      ZS_Talk=0;
    size_t                                                      ZS_Attack=0;
    size_t                                                      ZS_MM_Attack=0;

    daedalus::IFocus                                              cFocusNorm,cFocusMelee,cFocusRange,cFocusMage;
    std::shared_ptr<daedalus::IGuildValues>                       cGuildVal;

  friend struct ScopeCtx;
  friend class BodyController;
  };
