#include "gamemusic.h"
#include "gothic.h"
#include "settings.h"
#include "world/benchhooks.h"

#include <Tempest/Sound>
#include <Tempest/Log>

#include "game/definitions/musicdefinitions.h"
#include "dmusic/mixer.h"
#include "dmusic/dxmusicsource.h"
#include "resources.h"
#include "dmusic.h"

#include <condition_variable>
#include <thread>

using namespace Tempest;

static constexpr uint16_t SAMPLE_RATE = 44100;

struct GameMusic::MusicProvider : Tempest::SoundProducer {
  using Tempest::SoundProducer::SoundProducer;

  public:
    ~MusicProvider() override {
      stopLoader();
      }

    void stopTheme() {
      std::lock_guard<std::mutex> guard(pendingSync);
      cancelTheme();
      }

    void setEnabled(bool b) {
      std::lock_guard<std::mutex> guard(pendingSync);
      if(enable.load() == b)
        return;

      enable.store(b);
      if(b) {
        hasPending = pendingMusic.has_value();
        ++requestVersion;
        pendingChanged.notify_one();
        } else {
        cancelTheme();
        }
      }

    bool isEnabled() const {
      return enable.load();
      }

    void playTheme(const Def::MusicTheme &theme, GameMusic::Tags tags) {
      std::lock_guard<std::mutex> guard(pendingSync);
      if(pendingMusic && pendingMusic->file == theme.file && appliedVersion == requestVersion)
        return;
      pendingMusic = theme;
      pendingTags  = tags;
      hasPending   = true;
      ++requestVersion;
      pendingChanged.notify_one();
      }

  protected:
    struct ThemeRequest {
      Def::MusicTheme theme;
      Tags            tags;
      Tags            previousTags;
      uint64_t        version;
      };

    void startLoader() {
      loader = std::thread([this] { loaderMain(); });
      }

    void stopLoader() {
      {
      std::lock_guard<std::mutex> guard(pendingSync);
      quitting = true;
      ++requestVersion;
      }
      pendingChanged.notify_one();
      if(loader.joinable())
        loader.join();
      }

    // Called with pendingSync held after the mixer accepts a benchmark theme.
    void noteBenchmarkTheme(std::string& name) {
      benchmarkTheme.swap(name);
      pendingChanged.notify_one();
      }

    virtual void prepareTheme(const ThemeRequest& request) = 0;

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
    void cancelTheme() {
      pendingMusic.reset();
      hasPending = false;
      stopPending = true;
      themeStopped.store(true);
      ++requestVersion;
      }

    void loaderMain() {
      for(;;) {
        std::unique_lock<std::mutex> guard(pendingSync);
        pendingChanged.wait(guard, [this] {
          return quitting || !benchmarkTheme.empty() || (hasPending && enable.load());
          });
        if(!benchmarkTheme.empty()) {
          std::string name;
          name.swap(benchmarkTheme);
          guard.unlock();
          Log::i("Dx8 music: ", name);
          continue;
          }
        if(quitting)
          return;

        ThemeRequest request{*pendingMusic, pendingTags, currentTags, requestVersion};
        hasPending = false;
        guard.unlock();
        try {
          if(request.theme.file.empty()) {
            guard.lock();
            if(request.version == requestVersion)
              cancelTheme();
            continue;
            }
          prepareTheme(request);
          }
        catch(const std::runtime_error&) {
          Log::e("unable to load sound: \"", request.theme.file, "\"");
          guard.lock();
          if(request.version == requestVersion)
            cancelTheme();
          }
        catch(const std::bad_alloc&) {
          Log::e("out of memory for sound: \"", request.theme.file, "\"");
          guard.lock();
          if(request.version == requestVersion)
            cancelTheme();
          }
        }
      }

    std::atomic_bool     enable{true};

    std::condition_variable pendingChanged;
    std::thread          loader;
    bool                 quitting = false;
    bool                 hasPending  = false;
    Tags                 pendingTags = Tags::Day;
    std::optional<Def::MusicTheme> pendingMusic;
    std::string          benchmarkTheme;

  protected:
    std::mutex           pendingSync;
    uint64_t             requestVersion = 0;
    uint64_t             appliedVersion = 0;
    bool                 stopPending = false;
    std::atomic_bool     themeStopped{false};
    Tags                 currentTags = Tags::Day;
  };

struct GameMusic::OpenGothicMusicProvider : GameMusic::MusicProvider {
  OpenGothicMusicProvider(uint16_t rate, uint16_t channels) : MusicProvider(rate, channels) {
    startLoader();
    }

  ~OpenGothicMusicProvider() override {
    stopLoader();
    }

  void renderSound(int16_t *out, size_t n) override {
    {
    std::unique_lock<std::mutex> guard(pendingSync, std::try_to_lock);
    if(guard && ready && ready->version == requestVersion && ready->version != appliedVersion && isEnabled()) {
      mix.setMusic(ready->music, ready->embellishment);
      mix.setMusicVolume(ready->volume);
      if(Bench::isActive())
        noteBenchmarkTheme(ready->name);
      currentTags = ready->tags;
      appliedVersion = ready->version;
      themeStopped.store(false);
      }
    }
    if(!isEnabled() || themeStopped.load()) {
      std::memset(out, 0, n * sizeof(out[0]) * 2);
      return;
      }
    mix.mix(out, n);
    }

  void prepareTheme(const ThemeRequest& request) override {
    auto next = std::make_unique<PreparedTheme>();
    next->music.addPattern(loadDxMusic(request.theme.file));
    next->name = request.theme.file;
    const auto effect = computeTransitionEffect(request.tags, request.previousTags, request.theme.transtype);
    next->embellishment = computeEmbellishment(effect);
    next->volume = request.theme.vol;
    next->music.setVolume(next->volume);
    next->tags = request.tags;
    next->version = request.version;
    {
    std::lock_guard<std::mutex> guard(pendingSync);
    if(request.version == requestVersion && isEnabled())
      ready.swap(next);
    }
    // Release superseded preparations on the loader, outside the audio lock.
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
    struct PreparedTheme {
      Dx8::Music              music;
      Dx8::DMUS_EMBELLISHT_TYPES embellishment = Dx8::DMUS_EMBELLISHT_NORMAL;
      float                   volume = 1.f;
      Tags                    tags = Tags::Day;
      uint64_t                version = 0;
      std::string             name;
      };

    std::unique_ptr<PreparedTheme> ready;
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
    startLoader();
    }

  ~GothicKitMusicProvider() override {
    stopLoader();
    DmPerformance_release(performance);
    }

  void renderSound(int16_t *out, size_t n) override {
    {
    std::unique_lock<std::mutex> guard(pendingSync, std::try_to_lock);
    if(guard) {
      if(stopPending) {
        DmPerformance_playTransition(performance, nullptr, DmEmbellishment_NONE, DmTiming_INSTANT);
        stopPending = false;
        }
      if(ready && ready->version == requestVersion && ready->version != appliedVersion && isEnabled()) {
        const auto rv = DmPerformance_playTransition(performance, ready->segment.get(), ready->embellishment, ready->timing);
        if(rv != DmResult_SUCCESS) {
          Log::e("Failed to play theme: ", ready->name);
          themeStopped.store(true);
          } else {
          DmPerformance_setVolume(performance, ready->volume);
          currentTags = ready->tags;
          themeStopped.store(false);
          }
        appliedVersion = ready->version;
        }
      }
    }
    if(!isEnabled() || themeStopped.load()) {
      std::memset(out, 0, n * sizeof(out[0]) * 2);
      return;
      }
    DmPerformance_renderPcm(performance, out, n * 2, DmRenderOptions(DmRender_SHORT | DmRender_STEREO));
    }

  void prepareTheme(const ThemeRequest& request) override {
    auto next = std::make_unique<PreparedTheme>();
    next->segment.reset(Resources::loadMusicSegment(request.theme.file.c_str()));
    if(!next->segment)
      throw std::runtime_error("music segment not found");
    const auto effect = computeTransitionEffect(request.tags, request.previousTags, request.theme.transtype);
    const auto transition = computeEmbellishmentAndTiming(effect, request.theme.transsubtype);
    next->timing = transition.first;
    next->embellishment = transition.second;
    next->volume = request.theme.vol;
    next->tags = request.tags;
    next->version = request.version;
    next->name = request.theme.file;
    {
    std::lock_guard<std::mutex> guard(pendingSync);
    if(request.version == requestVersion && isEnabled())
      ready.swap(next);
    }
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
    struct PreparedTheme {
      std::unique_ptr<DmSegment, decltype(&DmSegment_release)> segment{nullptr, &DmSegment_release};
      DmTiming            timing = DmTiming_MEASURE;
      DmEmbellishmentType embellishment = DmEmbellishment_NONE;
      float               volume = 1.f;
      Tags                tags = Tags::Day;
      uint64_t            version = 0;
      std::string         name;
      };

    std::unique_ptr<PreparedTheme> ready;
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

  Settings::inst().onSettingsChanged.bind(this, &GameMusic::setupSettings);
  setupSettings();
  }

GameMusic::~GameMusic() {
  sound = SoundEffect();
  instance = nullptr;
  Settings::inst().onSettingsChanged.ubind(this, &GameMusic::setupSettings);
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
  const int   musicEnabled  = Settings::getI("SOUND",    "musicEnabled");
  if(!device) {
    setEnabled(musicEnabled!=0);
    return;
    }
  const float musicVolume   = Settings::getF("SOUND",    "musicVolume");
  const int   providerIndex = Settings::getI("INTERNAL", "soundProviderIndex");

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
