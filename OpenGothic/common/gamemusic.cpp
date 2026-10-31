#include "gamemusic.h"
#include "gothic.h"
#include "testbench.h"

#include <Tempest/Sound>
#include <Tempest/Log>

#include "game/definitions/musicdefinitions.h"
#include "dmusic/mixer.h"
#include "resources.h"
#include "dmusic.h"

using namespace Tempest;

static constexpr uint16_t SAMPLE_RATE = 44100;

struct GameMusic::MusicProvider : Tempest::SoundProducer {
  using Tempest::SoundProducer::SoundProducer;

  public:
    virtual void stopTheme() {
      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      pendingMusic.reset();
      }

    void setEnabled(bool b) {
      if(enable == b)
        return;

      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      if(b) {
        hasPending = true;
        reloadTheme = true;
        enable.store(true);
        } else {
        enable.store(false);
        stopTheme();
        }
      }

    bool isEnabled() const {
      return enable.load();
      }

    void playTheme(const Def::MusicTheme &theme, GameMusic::Tags tags) {
      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      reloadTheme  = !pendingMusic || pendingMusic->file != theme.file;
      pendingMusic = theme;
      pendingTags  = tags;
      hasPending   = true;
      }

  protected:
    bool updateTheme(Def::MusicTheme& theme, Tags& tags) {
      bool reloadTheme = false;

      std::lock_guard<std::recursive_mutex> guard(pendingSync);
      if(hasPending && pendingMusic && enable.load()) {
        hasPending  = false;
        reloadTheme = this->reloadTheme;
        theme       = this->pendingMusic.value();
        tags        = this->pendingTags;
        }

      this->reloadTheme = false;
      return reloadTheme;
      }

    static Def::MusicTransitionEffect computeTransitionEffect(Tags nextTags, Tags currTags, Def::MusicTransitionEffect transtype) {
      const int cur  = currTags & (Tags::Std | Tags::Fgt | Tags::Thr);
      const int next = nextTags & (Tags::Std | Tags::Fgt | Tags::Thr);

      //Def::MusicTransitionEffect embellishment = transtype;
      Def::MusicTransitionEffect embellishment = Def::MusicTransitionEffect::NONE;
      if(next == Tags::Std) {
        if(cur != Tags::Std)
          embellishment = Def::MusicTransitionEffect::BREAK;
        }
      else if(next == Tags::Fgt) {
        if(cur == Tags::Thr)
          embellishment = Def::MusicTransitionEffect::FILL;
        }
      else if(next == Tags::Thr) {
        if(cur == Tags::Fgt)
          embellishment = Def::MusicTransitionEffect::NONE;
        }

      return embellishment;
      }

  private:
    std::atomic_bool     enable{true};

    std::recursive_mutex pendingSync;
    bool                 hasPending  = false;
    bool                 reloadTheme = false;
    Tags                 pendingTags = Tags::Day;
    std::optional<Def::MusicTheme> pendingMusic;

  protected:
    Tags                 currentTags = Tags::Day;
  };

struct GameMusic::OpenGothicMusicProvider : GameMusic::MusicProvider {
  using GameMusic::MusicProvider::MusicProvider;

  void renderSound(int16_t *out, size_t n) override {
    if(!isEnabled()) {
      std::memset(out, 0, n * sizeof(out[0]) * 2);
      return;
      }
    updateTheme();
    mix.mix(out, n);
    }

  void updateTheme() {
    Def::MusicTheme theme;
    Tags                tags;
    if(!GameMusic::MusicProvider::updateTheme(theme, tags))
      return;

    if(theme.file.empty()) {
      stopTheme();
      return;
      }

    try {
      if(/*reloadTheme*/true) {
        Dx8::PatternList p = Resources::loadDxMusic(theme.file);

        Dx8::Music m;
        m.addPattern(p);

        auto effect = computeTransitionEffect(tags, currentTags, theme.transtype);
        Dx8::DMUS_EMBELLISHT_TYPES em = computeEmbellishment(effect);
        mix.setMusic(m, em);
        if(TestBench::isActive())
          Log::i("Dx8 music: ",theme.file);
        currentTags = tags;
        }
      mix.setMusicVolume(theme.vol);
      }
    catch (std::runtime_error &) {
      Log::e("unable to load sound: \"", theme.file, "\"");
      stopTheme();
      }
    catch (std::bad_alloc &) {
      Log::e("out of memory for sound: \"", theme.file, "\"");
      stopTheme();
      }
    }

  void stopTheme() override {
    GameMusic::MusicProvider::stopTheme();
    }

  static Dx8::DMUS_EMBELLISHT_TYPES computeEmbellishment(Def::MusicTransitionEffect ef) {
    switch (ef) {
      case Def::MusicTransitionEffect::UNKNOWN:
      case Def::MusicTransitionEffect::NONE:
        return Dx8::DMUS_EMBELLISHT_NORMAL;
      case Def::MusicTransitionEffect::GROOVE:
        return Dx8::DMUS_EMBELLISHT_NORMAL;
      case Def::MusicTransitionEffect::FILL:
        return Dx8::DMUS_EMBELLISHT_FILL;
      case Def::MusicTransitionEffect::BREAK:
        return Dx8::DMUS_EMBELLISHT_BREAK;
      case Def::MusicTransitionEffect::INTRO:
        return Dx8::DMUS_EMBELLISHT_INTRO;
      case Def::MusicTransitionEffect::END:
      case Def::MusicTransitionEffect::END_AND_INTO:
        return Dx8::DMUS_EMBELLISHT_END;
      }
    return Dx8::DMUS_EMBELLISHT_NORMAL;
    }

  private:
    Dx8::Mixer mix;
  };

static std::pair<DmTiming, DmEmbellishmentType> computeEmbellishmentAndTiming(const Def::MusicTransitionEffect effect, const Def::MusicTransitionType type) {
  DmEmbellishmentType embellishment = DmEmbellishment_NONE;
  switch (effect) {
    case Def::MusicTransitionEffect::UNKNOWN:
    case Def::MusicTransitionEffect::NONE:
      embellishment = DmEmbellishment_NONE;
      break;
    case Def::MusicTransitionEffect::GROOVE:
      embellishment = DmEmbellishment_GROOVE;
      break;
    case Def::MusicTransitionEffect::FILL:
      embellishment = DmEmbellishment_FILL;
      break;
    case Def::MusicTransitionEffect::BREAK:
      embellishment = DmEmbellishment_BREAK;
      break;
    case Def::MusicTransitionEffect::INTRO:
      embellishment = DmEmbellishment_INTRO;
      break;
    case Def::MusicTransitionEffect::END:
      embellishment = DmEmbellishment_END;
      break;
    case Def::MusicTransitionEffect::END_AND_INTO:
      embellishment = DmEmbellishment_END_AND_INTRO;
      break;
    }

  DmTiming timing = DmTiming_MEASURE;
  switch (type) {
    case Def::MusicTransitionType::UNKNOWN:
    case Def::MusicTransitionType::MEASURE:
      timing = DmTiming_MEASURE;
      break;
    case Def::MusicTransitionType::IMMEDIATE:
      timing = DmTiming_INSTANT;
      break;
    case Def::MusicTransitionType::BEAT:
      timing = DmTiming_BEAT;
      break;
    }

  return std::make_pair(timing, embellishment);
  }

struct GameMusic::GothicKitMusicProvider : GameMusic::MusicProvider {
  GothicKitMusicProvider(uint16_t rate, uint16_t channels) : GameMusic::MusicProvider(rate, channels) {
    /*
    Dm_setRandomNumberGenerator([](void*) -> uint32_t {
      return 0;
      }, nullptr);
    */

    DmResult rv = DmPerformance_create(&performance, rate);
    if(rv != DmResult_SUCCESS) {
      Log::e("Unable to create DmPerformance object. Out of memory?");
      }
    }

  ~GothicKitMusicProvider() override {
    DmPerformance_release(performance);
    }

  void renderSound(int16_t *out, size_t n) override {
    if(!isEnabled()) {
      std::memset(out, 0, n * sizeof(out[0]) * 2);
      return;
      }
    updateTheme();
    DmPerformance_renderPcm(performance, out, n * 2, DmRenderOptions(DmRender_SHORT | DmRender_STEREO));
    }

  void updateTheme() {
    Def::MusicTheme theme;
    Tags                tags;
    if(!GameMusic::MusicProvider::updateTheme(theme, tags))
      return;

    if(theme.file.empty()) {
      stopTheme();
      return;
      }

    auto effect = computeTransitionEffect(tags, currentTags, theme.transtype);
    auto [timing, embellishment] = computeEmbellishmentAndTiming(effect, theme.transsubtype);

    DmSegment* sgt = Resources::loadMusicSegment(theme.file.c_str());
    DmResult rv = DmPerformance_playTransition(performance, sgt, embellishment, timing);
    if(rv != DmResult_SUCCESS) {
      Log::e("Failed to play theme: ", theme.file);
      stopTheme();
      }

    DmPerformance_setVolume(performance, theme.vol);
    DmSegment_release(sgt);
    currentTags = tags;
    }

  void stopTheme() override {
    GameMusic::MusicProvider::stopTheme();
    DmPerformance_playTransition(performance, nullptr, DmEmbellishment_NONE, DmTiming_INSTANT);
    }

  DmEmbellishmentType computeEmbellishment(Tags nextTags, Tags currTags, Def::MusicTransitionEffect transtype) const {
    const int cur  = currTags & (Tags::Std | Tags::Fgt | Tags::Thr);
    const int next = nextTags & (Tags::Std | Tags::Fgt | Tags::Thr);

    DmEmbellishmentType embellishment = DmEmbellishment_NONE;
    switch (transtype) {
      case Def::MusicTransitionEffect::UNKNOWN:
      case Def::MusicTransitionEffect::NONE:
        embellishment = DmEmbellishment_NONE;
        break;
      case Def::MusicTransitionEffect::GROOVE:
        embellishment = DmEmbellishment_GROOVE;
        break;
      case Def::MusicTransitionEffect::FILL:
        embellishment = DmEmbellishment_FILL;
        break;
      case Def::MusicTransitionEffect::BREAK:
        embellishment = DmEmbellishment_BREAK;
        break;
      case Def::MusicTransitionEffect::INTRO:
        embellishment = DmEmbellishment_INTRO;
        break;
      case Def::MusicTransitionEffect::END:
        embellishment = DmEmbellishment_END;
        break;
      case Def::MusicTransitionEffect::END_AND_INTO:
        embellishment = DmEmbellishment_END_AND_INTRO;
        break;
      }

    if(next == Tags::Std) {
      if(cur != Tags::Std)
        embellishment = DmEmbellishmentType::DmEmbellishment_BREAK;
      }
    else if(next == Tags::Fgt) {
      if(cur == Tags::Thr)
        embellishment = DmEmbellishmentType::DmEmbellishment_FILL;
      }
    else if(next == Tags::Thr) {
      if(cur == Tags::Fgt)
        embellishment = DmEmbellishmentType::DmEmbellishment_NONE;
      }

    return embellishment;
    }

  private:
    DmPerformance *performance = nullptr;
  };

static constexpr int PROVIDER_UNINITIALIZED = -1;
static constexpr int PROVIDER_OPENGOTHIC = 0;
// static constexpr int PROVIDER_GOTHICKIT = 1;

GameMusic *GameMusic::instance = nullptr;

GameMusic::GameMusic() {
  instance = this;
  provider = PROVIDER_UNINITIALIZED;
  if(!Resources::isHeadless())
    device = std::make_unique<Tempest::SoundDevice>();

  Gothic::inst().onSettingsChanged.bind(this, &GameMusic::setupSettings);
  setupSettings();
  }

GameMusic::~GameMusic() {
  sound = SoundEffect();
  instance = nullptr;
  Gothic::inst().onSettingsChanged.ubind(this, &GameMusic::setupSettings);
  }

GameMusic &GameMusic::inst() {
  return *instance;
  }

GameMusic::Tags GameMusic::mkTags(GameMusic::Tags daytime, GameMusic::Tags mode) {
  return Tags(daytime | mode);
  }

void GameMusic::setEnabled(bool e) {
  if(impl!=nullptr)
    impl->setEnabled(e);
  silentEnabled = e;
  }

bool GameMusic::isEnabled() const {
  return impl!=nullptr ? impl->isEnabled() : silentEnabled;
  }

void GameMusic::setMusic(GameMusic::Music m) {
  const char *clsTheme = "";
  switch (m) {
    case GameMusic::SysLoading:
      clsTheme = "SYS_Loading";
      break;
    }
  if(auto theme = Gothic::musicDef()[clsTheme])
    setMusic(*theme, GameMusic::mkTags(GameMusic::Std, GameMusic::Day));
  }

void GameMusic::setMusic(const Def::MusicTheme &theme, Tags tags) {
  currentMusic.theme = theme;
  currentMusic.tags  = tags;
  if(impl!=nullptr)
    impl->playTheme(currentMusic.theme, currentMusic.tags);
  }

void GameMusic::stopMusic() {
  setEnabled(false);
  }

void GameMusic::setupSettings() {
  const int   musicEnabled  = Gothic::settingsGetI("SOUND",    "musicEnabled");
  if(!device) {
    setEnabled(musicEnabled!=0);
    return;
    }
  const float musicVolume   = Gothic::settingsGetF("SOUND",    "musicVolume");
  const int   providerIndex = Gothic::settingsGetI("INTERNAL", "soundProviderIndex");

  if(providerIndex != provider || musicEnabled!=isEnabled()) {
    Log::i("Switching music provider to ", providerIndex == PROVIDER_OPENGOTHIC ? "'OpenGothic'" : "'GothicKit'");
    sound = SoundEffect();

    std::unique_ptr<MusicProvider> p;
    if(providerIndex == PROVIDER_OPENGOTHIC) {
      p = std::make_unique<OpenGothicMusicProvider>(SAMPLE_RATE, 2);
      } else {
      p = std::make_unique<GothicKitMusicProvider>(SAMPLE_RATE, 2);
      }

    provider = providerIndex;
    impl = p.get();
    impl->playTheme(currentMusic.theme, currentMusic.tags);

    sound = device->load(std::move(p));
    sound.play();
    }

  setEnabled(musicEnabled != 0);
  sound.setVolume(musicVolume);
  }
