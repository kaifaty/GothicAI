#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The game's own families of the selected base, which the platform does not read: for Gothic the
// Daedalus programs and the dialog line library (game/definitions/basescripts.cpp). As with GameServices,
// the game's object registers itself on construction. Resources::loadBase calls it on the step of base
// selection, in the place of those families. With none registered (a program without the game) they are
// neither read nor checked.
class GameBaseContent {
  public:
    // (name, size), sorted: identities of archives, or loose files relative to the installation.
    using Files = std::vector<std::pair<std::string,uintmax_t>>;
    // Throws when a file is not a loose file of the base identity; `family` names the index in the message.
    using LooseCheck = std::function<void(const Files& files, std::string_view family)>;

    virtual ~GameBaseContent();

    // The registered object; nullptr when the program has none.
    static GameBaseContent* inst();

    // Reads the game's indexes of the base at `root`; throws on missing or corrupt ones.
    virtual void index(const std::filesystem::path& root) = 0;
    // Checks them against the archives of the base identity and its loose files, then writes them to the log;
    // throws on a difference.
    virtual void validate(const Files& archives, const LooseCheck& validateLoose) const = 0;
    // GAME.language of the selected base; -1 when there is none.
    virtual int language() const = 0;

  protected:
    GameBaseContent();

  private:
    static GameBaseContent* instance;
  };
