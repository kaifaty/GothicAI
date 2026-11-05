set(ZENKIT_ALLOWED_HEADERS
  # The game executes Daedalus programs in the existing VM.
  zenkit/DaedalusVm.hh
  # Compiled programs and symbols remain part of the VM.
  zenkit/DaedalusScript.hh
  # Script classes describe the VM's instance layout; replacing the VM is outside P20.
  zenkit/addon/daedalus.hh)
# Only the log adapter owns ZenKit logging.
set(ZENKIT_LOG_HEADER zenkit/Logger.hh)
set(ZENKIT_LOG_FILES common/zenkitlog.cpp)
