#pragma once

#include <string>
#include <memory>
#include <thread>
#include <exception>
#include <functional>
#include <utility>

#include <Tempest/Signal>
#include <Tempest/Dir>

#include "daedalus/vm.h"
#include "settings.h"
#include "gameservices.h"

#include "game/chronicle.h"
#include "game/gamesession.h"
#include "world/world.h"
#include "ui/documentmenu.h"
#include "ui/chapterscreen.h"
#include "sound/soundfx.h"

class CameraDefinitions;
class SoundDefinitions;
class VisualFxDefinitions;
class ParticlesDefinitions;
class MusicDefinitions;
class FightAi;
class IniFile;
class Serialize;
struct PhysicsNoise;
struct HeroDeed;

class Gothic final : public GameServices {
  public:
    Gothic();
    ~Gothic();

    static Gothic& inst();

    enum class LoadState:int {
      Idle       = 0,
      Loading    = 1,
      Saving     = 2,
      Finalize   = 3,
      FailedLoad = 4,
      FailedSave = 5
      };

    using PhysicsFlags = Settings::PhysicsFlags;
    using Options = Settings::Options;

    bool         isInGame() const;
    bool         isInGameAndAlive() const;

    std::string_view defaultWorld()       const;
    std::string_view defaultPlayer()      const;
    std::string_view defaultSave()        const;
    std::string_view defaultGameDatFile() const;
    std::string_view defaultOutputUnits() const;

    std::string_view menuMain() const;

    void         setGame(std::unique_ptr<GameSession> &&w);
    auto         clearGame() -> std::unique_ptr<GameSession>;

    auto         gameSession() const -> const GameSession*;
    auto         gameSession() -> GameSession*;
    const World* world() const;
    World*       world() override;
    WorldView*   worldView() const override;
    const BloodDecalSource* bloodDecalSource() const override;
    Npc*         player() override;
    const CameraTarget* cameraTarget() override;
    Camera*      camera() override;
    void         cameraProjection(Tempest::Matrix4x4& proj) override;
    bool         cameraShake(Tempest::Vec3& shake) override;
    auto         questLog() const -> const QuestLog*;
    // the hero's chronicle for the diary (ai_bridge keeps it fresh; empty without the server)
    auto         chronicle() const -> const ChronicleData& { return chronicleBook; }
    void         setChronicle(ChronicleData data) { chronicleBook = std::move(data); }

    void         setupGlobalScripts();

    auto         loadingBanner() const -> const Tempest::Texture2d*;
    int          loadingProgress() const;
    void         setLoadingProgress(int v);

    SoundFx*     loadSoundFx   (std::string_view name);
    SoundFx*     loadSoundWavFx(std::string_view name);

    auto         loadParticleFx(std::string_view name, bool relaxed=false) -> const ParticleFx* override;
    auto         loadParticleFx(const ParticleFx* base, const VisualFx::Key* key) -> const ParticleFx* override;
    auto         loadVisualFx  (std::string_view name) -> const VisualFx* override;

    void         emitGlobalSound(std::string_view   sfx);
    void         emitGlobalSound(const SoundFx*     sfx);
    void         emitGlobalSound(const Tempest::Sound& sfx);

    void         emitGlobalSoundWav(std::string_view wav);

    static auto  invCatOrder()  -> const std::vector<ItmFlags>&;

    void         pushPause();
    void         popPause();
    bool         isPause() const override;

    bool         isMarvinEnabled() const;
    void         setMarvinEnabled(bool m);

    static auto  options() -> const Options& { return Settings::options(); }
    static float interfaceScale(const Tempest::Widget* w);

    bool         isGodMode() const { return godMode; }
    void         setGodMode(bool g) { godMode = g; }
    // a scene of its own (the AI bridge's endings): no bars, no focus, no messages over it
    bool         isHudHidden() const { return hudHidden; }
    void         setHudHidden(bool h) { hudHidden = h; }
    // the game's script ends the game (the finale's exitsession): a taker (ai_bridge's final card,
    // docs/achievements.md) shows something first and ends the session itself; false: it ends at once
    std::function<bool(std::string_view kind)> takeGameEnd;

    void         toggleDesktop() { desktop = !desktop; }
    bool         isDesktop() { return desktop; }

    bool         doFrate() const { return showFpsCounter; }
    void         setFRate(bool f) { showFpsCounter = f; }

    bool         doClock() const { return showTime; }
    void         setClock(bool t) { showTime = t; }

    bool         doVobBox() const { return vobBox; }
    void         setVobBox(bool v) { vobBox = v; }

    bool         doVobRays() const { return vobRays; }
    void         setVobRays(bool v) { vobRays = v; }

    bool         doMeleeBladeDraw() const { return meleeBladeDraw; }
    void         setMeleeBladeDraw(bool v) { meleeBladeDraw = v; }
    bool         doMeleeBladeLabels() const { return meleeBladeLabels; }
    void         setMeleeBladeLabels(bool v) { meleeBladeLabels = v; }

    bool         isBenchmarkMode() const;
    bool         isBenchmarkModeCi() const;
    void         setBenchmarkMode(Benchmark b);

    LoadState    checkLoading() const;
    bool         finishLoading();
    void         startLoad(std::string_view banner, const std::function<std::unique_ptr<GameSession>(std::unique_ptr<GameSession>&&)> f);
    void         startSave(Tempest::Texture2d&& tex, const std::function<std::unique_ptr<GameSession>(std::unique_ptr<GameSession>&&)> f);
    void         cancelLoading();

    void         tick(uint64_t dt);

    void         updateAnimation(uint64_t dt);
    void         quickSave();
    void         quickLoad();
    void         save(std::string_view slot, std::string_view usrName);
    void         load(std::string_view slot);

    auto         updateDialog(const GameScript::DlgChoice& dlg, Npc& player, Npc& npc) -> std::vector<GameScript::DlgChoice>;
    void         dialogExec  (const GameScript::DlgChoice& dlg, Npc& player, Npc& npc);

    void         openDialogPipe (Npc& player, Npc& npc, AiOuputPipe*& pipe);
    bool         isNpcInDialog(const Npc& npc) const;
    bool         isInDialog() const;

    Tempest::Signal<void(std::string_view)>                             onStartGame;
    Tempest::Signal<void(std::string_view)>                             onLoadGame;
    Tempest::Signal<void(std::string_view,std::string_view)>            onSaveGame;

    Tempest::Signal<void(Npc&,Npc&,AiOuputPipe*&)>                      onDialogPipe;
    std::function<bool(const Npc*)>                                     isNpcInDialogFn;
    // the dialog window: what it offers now and picking the i-th as the player does (debug and test tools)
    std::function<std::vector<std::string>()>                           dialogEntriesFn;
    std::function<bool(size_t)>                                         dialogPickFn;

    Tempest::Signal<void(std::string_view,int,int,int,const GthFont&)>  onPrintScreen;
    Tempest::Signal<void(std::string_view)>                             onPrint;
    Tempest::Signal<void(std::string_view, bool)>                       onVideo;

    Tempest::Signal<void(const ChapterScreen::Show&)>                   onIntroChapter;
    Tempest::Signal<void(const DocumentMenu::Show&)>                    onShowDocument;
    Tempest::Signal<void()>                                             onWorldLoaded;
    Tempest::Signal<void()>                                             onStartLoading;
    Tempest::Signal<void()>                                             onSessionExit;
    Tempest::Signal<void()>                                             onSessionUnload;  // the session and its sound device are about to go

    Tempest::Signal<void()>                                             onBenchmarkFinished;

    // external AI control (ai_bridge)
    Tempest::Signal<void(uint64_t)>                                     onTick;
    Tempest::Signal<void(Npc&,Npc&,Npc*,PercType)>                      onExternalPerception;
    // perceptions of npc's that follow their scripts (seen, not consumed): ai_bridge watches them
    Tempest::Signal<void(Npc&,Npc&,Npc*,PercType)>                      onObservedPerception;
    Tempest::Signal<void(Npc&,Npc&,int32_t,std::string_view)>           onCombatHit;
    // A completed hero action and the actual recipients of its passive perception.
    Tempest::Signal<void(const HeroDeed&)>                              onHeroDeed;
    // A physical impact actually heard, before the listener turns towards it.
    Tempest::Signal<void(Npc&,const PhysicsNoise&)>                     onPhysicsNoiseHeard;
    // a perception the scripts would handle may be taken over (true: the scripts do not get it now);
    // the taker decides and hands it back with Npc::passPerception if it does not act on it
    std::function<bool(Npc&,Npc&,Npc*,PercType,const HeroDeed*)>         interceptPerception;
    // A native SVM line is ready to start: true replaces its sound and subtitle, retaining its wait.
    std::function<bool(Npc&,std::string_view,std::string_view,Npc*,bool)> interceptSvm;
    std::function<bool(std::string_view,std::string&)>                  consoleCommandFn;
    // runtime dialog lines: id -> subtitle text and duration (ms); false if the id is not external
    std::function<bool(std::string_view,std::string_view&,uint32_t&)>   externalMessageFn;
    // custom save-game data, called on the loading thread: write own entries into a save;
    // a GameSession was created from a save (or nullptr for a new game)
    std::function<void(Serialize&)>                                     saveExtFn;
    std::function<void(Serialize*)>                                     sessionStartFn;
    // story graph: which dialogs an npc offers. Called while the dialog menu is built and by Npc_CheckInfo,
    // with self and other set as for the conditions.
    // a script dialog whose condition was checked: `visible` comes in as the script's answer
    std::function<void(const daedalus::IInfo&,bool&)>                     dialogFilterFn;
    // a choice of a script dialog (Info_AddChoice) about to be shown: false hides it
    std::function<bool(const daedalus::IInfo&,int32_t)>                   dialogChoiceFilterFn;
    // dialogs made at runtime that the npc offers now; `important` ones start on their own
    std::function<void(const daedalus::INpc&,bool,std::vector<GameScript::DlgChoice>&)> dialogExtraFn;
    // one of those was chosen: play it
    std::function<void(const GameScript::DlgChoice&,Npc&,Npc&)>         dialogExtraExecFn;
    // the choices one of those offers now, after its lines (as Info_AddChoice's after a script dialog)
    std::function<void(const GameScript::DlgChoice&,std::vector<GameScript::DlgChoice>&)> dialogExtraChoicesFn;
    // a dialog or a choice was chosen and has run
    Tempest::Signal<void(const GameScript::DlgChoice&,Npc&,Npc&)>       onDialogChosen;
    // a dialog's script function (its condition, its information or a choice) is about to run, self and other
    // set: true if the story graph ran it in place of the VM, a condition's answer in `result`
    std::function<bool(daedalus::DaedalusSymbol&,int32_t&)>               dialogScriptFn;

    std::string_view                      messageFromSvm(std::string_view id, int voice) const;
    std::string_view                      messageByName (std::string_view id) const;
    uint32_t                              messageTime   (std::string_view id) const;

    static std::u16string                 nestedPath(const std::initializer_list<const char16_t*> &name, Tempest::Dir::FileType type);
    std::unique_ptr<daedalus::DaedalusVm>   createPhoenixVm(std::string_view datFile, const ScriptLang lang = ScriptLang::NONE);
    daedalus::DaedalusScript                loadScript(std::string_view datFile, const ScriptLang lang);
    void                                  setupCommonScriptClasses(daedalus::DaedalusScript &sc);
    void                                  setupVmCommonApi(daedalus::DaedalusVm &vm);

    static const FightAi&                 fai();
    static const SoundDefinitions&        sfx();
    static const MusicDefinitions&        musicDef();
    static const CameraDefinitions&       cameraDef();

    static const PhysicsFlags& physicsFlags() { return Settings::physicsFlags(); }
    static bool settingsHasSection(std::string_view sec) { return Settings::has(sec); }
    static int settingsGetI(std::string_view sec, std::string_view name) { return Settings::getI(sec,name); }
    static void settingsSetI(std::string_view sec, std::string_view name, int val) { Settings::setI(sec,name,val); }
    static std::string_view settingsGetS(std::string_view sec, std::string_view name) { return Settings::getS(sec,name); }
    static void settingsSetS(std::string_view sec, std::string_view name, std::string_view val) { Settings::setS(sec,name,val); }
    static float settingsGetF(std::string_view sec, std::string_view name) { return Settings::getF(sec,name); }
    static void settingsSetF(std::string_view sec, std::string_view name, float val) { Settings::setF(sec,name,val); }
    static float settingsSoundVolume() { return Settings::soundVolume(); }
    static void flushSettings() { Settings::flush(); }

    const CameraPresets& cameraDefinitions() const override;
    const Def::SoundEffect& soundEffect(std::string_view name) const override;
    int64_t worldTime() const override;
    uint64_t worldTickCount() const override;
    DynamicWorld* physicsWorld() const override;

  private:
    Settings                                settings;
    std::mt19937                            randGen;
    uint16_t                                pauseSum=0;
    bool                                    isMarvin       = false;
    bool                                    godMode        = false;
    bool                                    hudHidden      = false;
    bool                                    desktop        = false;
    bool                                    showFpsCounter = false;
    bool                                    showTime       = false;
    bool                                    vobBox         = false;
    bool                                    vobRays        = false;
    bool                                    meleeBladeDraw = false;
    bool                                    meleeBladeLabels = true;
    Benchmark                               isBenchmark    = Benchmark::None;

    std::string                             wrldDef, plDef, gameDatDef, ouDef;

    std::unique_ptr<IniFile>                modFile;
    std::unique_ptr<IniFile>                systemPackIniFile;

    Tempest::Texture2d                      loadTex;
    Tempest::Texture2d                      saveTex;
    std::atomic_int                         loadProgress{0};
    std::thread                             loaderTh;
    std::exception_ptr                      baseLoadError;
    std::atomic<LoadState>                  loadingFlag{LoadState::Idle};
#if defined(__GLIBC__)
    bool                                    loadingWorld = false;
#endif

    std::unique_ptr<GameSession>            game, pendingGame;
    ChronicleData                           chronicleBook;
    std::unique_ptr<FightAi>                fight;
    std::unique_ptr<CameraDefinitions>      camDef;
    std::unique_ptr<SoundDefinitions>       soundDef;
    std::unique_ptr<VisualFxDefinitions>    vfxDef;
    std::unique_ptr<ParticlesDefinitions>   particleDef;
    std::unique_ptr<MusicDefinitions>       music;

    std::mutex                              syncSnd;
    std::unique_ptr<Tempest::SoundDevice>    sndDev;
    std::unordered_map<std::string,SoundFx> sndFxCache;
    std::unordered_map<std::string,SoundFx> sndWavCache;
    std::vector<Tempest::SoundEffect>       sndStorage;

    std::vector<std::unique_ptr<DocumentMenu::Show>> documents;
    ChapterScreen::Show                     chapter;
    bool                                    pendingChapter=false;

    std::vector<ItmFlags>                   inventoryOrder;

    static Gothic*                          instance;

    void                                    implStartLoadSave(std::string_view banner,
                                                              bool load,
                                                              const std::function<std::unique_ptr<GameSession>(std::unique_ptr<GameSession>&&)> f);

    void                                    setupSettings();

    auto                                    getDocument(int id) -> std::unique_ptr<DocumentMenu::Show>&;

    static void                             notImplementedRoutine(std::string_view fn);

    static std::string                      concatstrings     (std::string_view a, std::string_view b);
    static std::string                      inttostring       (int i);
    static std::string                      floattostring     (float f);
    static int                              floattoint        (float f);
    static float                            inttofloat        (int i);

    static bool                             hlp_strcmp        (std::string_view a, std::string_view b);
    int                                     hlp_random        (int max);

    void                                    introducechapter  (std::string_view title, std::string_view subtitle, std::string_view img, std::string_view sound, int time);
    bool                                    playvideo         (std::string_view name);
    bool                                    playvideoex       (std::string_view name, bool screenBlend, bool exitSession);
    bool                                    printscreen       (std::string_view msg, int posx, int posy, std::string_view font, int timesec);
    bool                                    printdialog       (int dialognr, std::string_view msg, int posx, int posy, std::string_view font, int timesec);
    void                                    print             (std::string_view msg);

    int                                     doc_create        ();
    int                                     doc_createmap     ();
    void                                    doc_setpage       (int handle, int page, std::string_view img, int scale);
    void                                    doc_setpages      (int handle, int count);
    void                                    doc_printline     (int handle, int page, std::string_view text);
    void                                    doc_printlines    (int handle, int page, std::string_view text);
    void                                    doc_setmargins    (int handle, int page, int left, int top, int right, int bottom, int mul);
    void                                    doc_setfont       (int handle, int page, std::string_view font);
    void                                    doc_setlevel      (int handle, std::string_view level);
    void                                    doc_setlevelcoords(int handle, int left, int top, int right, int bottom);
    void                                    doc_show          (int handle);

    void                                    exitgame          ();

    void                                    printdebug        (std::string_view msg);
    void                                    printdebugch      (int ch, std::string_view msg);
    void                                    printdebuginst    (std::string_view msg);
    void                                    printdebuginstch  (int ch, std::string_view msg);
  };
