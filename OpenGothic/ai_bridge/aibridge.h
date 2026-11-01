#pragma once

#include "chronicledata.h"

#include <functional>
#include <memory>

class StuckLogger;

// Connects the game to the external AI server (WebSocket + JSON, see docs/protocol.md in gothic-ai).
// Server address: env GOTHIC_AI_URL (default ws://127.0.0.1:47817); GOTHIC_AI=0 disables the bridge.
class AiBridge final {
  public:
    AiBridge();
    ~AiBridge();

    AiBridge(const AiBridge&) = delete;
    AiBridge& operator=(const AiBridge&) = delete;

    // Call on the game thread. The callback runs there, immediately when disconnected.
    static void requestChronicle(std::function<void(const ChronicleData&)> callback);
    static void requestFateCard(std::string kind, std::string killer, std::string name,
                                std::function<void(const FateCard&)> callback);

  private:
    struct Impl;
    std::unique_ptr<StuckLogger> stuck;
    std::unique_ptr<Impl> impl;
  };
