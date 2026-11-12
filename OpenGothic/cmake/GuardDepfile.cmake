# Shared by the include guards (ZenKitGuard.cmake, EngineLayerGuard.cmake) in `cmake -P` mode.

# Write `<stamp>.d` so the stamp reruns when any of the checked files changes, then touch the stamp.
# A newly included header enters the depfile on the next source/header edit.
# Escape paths for Make/Ninja depfiles, including worktrees whose paths contain spaces.
function(guard_write_depfile stamp)
  set(dependencies "${stamp}" ${ARGN})
  set(escaped)
  foreach(path IN LISTS dependencies)
    string(REPLACE "$" "$$" path "${path}")
    string(REPLACE "#" "\\#" path "${path}")
    string(REPLACE " " "\\ " path "${path}")
    list(APPEND escaped "${path}")
  endforeach()
  list(POP_FRONT escaped output)
  list(JOIN escaped " " depfiles)
  file(WRITE "${stamp}.d" "${output}: ${depfiles}\n")
  file(TOUCH "${stamp}")
endfunction()
