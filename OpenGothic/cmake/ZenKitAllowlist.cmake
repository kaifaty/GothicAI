set(ZENKIT_ALLOWED_HEADERS
  # P21a: only the program record remains in ZenKit, until P21b moves the base type.
  zenkit/DaedalusScript.hh)
# Only the log adapter owns ZenKit logging.
set(ZENKIT_LOG_HEADER zenkit/Logger.hh)
set(ZENKIT_LOG_FILES common/zenkitlog.cpp)
