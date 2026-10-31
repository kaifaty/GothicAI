#pragma once

#include "game/definitions/definitiontypes.h"

#include <Tempest/SoundDevice>
#include <Tempest/SoundEffect>
#include <memory>

class GameMusic final {
  public:
    GameMusic();
    GameMusic(const GameMusic&)=delete;
    ~GameMusic();

    static GameMusic& inst();

    enum Music : uint8_t {
      SysLoading
      };

    enum Tags : uint8_t {
      Day = 0,
      Ngt = 1<<0,

      Std = 0,
      Fgt = 1<<1,
      Thr = 1<<2
      };

    static Tags mkTags(Tags daytime,Tags mode);

    void      setEnabled(bool e);
    bool      isEnabled() const;
    void      setMusic(Music m);
    void      setMusic(const Def::MusicTheme &theme, Tags t);
    void      stopMusic();

  private:
    struct MusicProvider;
    struct OpenGothicMusicProvider;
    struct GothicKitMusicProvider;

    void      setupSettings();

    static GameMusic* instance;

    struct {
      Def::MusicTheme theme = {};
      Tags                tags  = Tags::Std;
      } currentMusic;

    int                  provider = -1;
    std::unique_ptr<Tempest::SoundDevice> device;
    Tempest::SoundEffect sound;
    MusicProvider*       impl = nullptr;
    bool                 silentEnabled = false;
  };
