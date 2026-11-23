#include "zenkitlog.h"

#include <Tempest/Log>
#include <zenkit/Logger.hh>

namespace {
thread_local bool preparingVideo = false;
}

void ZenKitLog::write(zenkit::LogLevel level, const char*, const char* message) {
  if(preparingVideo) return;
  switch(level) {
    case zenkit::LogLevel::ERROR:
    case zenkit::LogLevel::WARNING:
      Tempest::Log::e("[zenkit] ", message);
      break;
    case zenkit::LogLevel::INFO:
      Tempest::Log::i("[zenkit] ", message);
      break;
    case zenkit::LogLevel::DEBUG:
    case zenkit::LogLevel::TRACE:
      Tempest::Log::d("[zenkit] ", message);
      break;
    }
  }

void ZenKitLog::install(Level level) {
  zenkit::Logger::set(level==Level::Info ? zenkit::LogLevel::INFO : zenkit::LogLevel::WARNING,write);
  }

void ZenKitLog::silenceForVideo() {
  preparingVideo = true;
  }
