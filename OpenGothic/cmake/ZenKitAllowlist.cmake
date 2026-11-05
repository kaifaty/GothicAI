set(ZENKIT_ALLOWED_HEADERS
  # The game executes Daedalus programs in the existing VM.
  zenkit/DaedalusVm.hh
  # Compiled programs and symbols remain part of the VM.
  zenkit/DaedalusScript.hh
  # Script classes describe the VM's instance layout; replacing the VM is outside P20.
  zenkit/addon/daedalus.hh
  # Temporary until P20b replaces the asset vectors, matrices and colors.
  zenkit/Misc.hh
  # Temporary until P20b replaces the asset bounding boxes.
  zenkit/Boxes.hh)
# Only the log adapter owns ZenKit logging; baseimport.h is temporary until P20b.
set(ZENKIT_LOG_HEADER zenkit/Logger.hh)
set(ZENKIT_LOG_FILES common/zenkitlog.cpp common/baseimport.h)
