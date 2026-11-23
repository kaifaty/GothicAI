# Writes OUTPUT, a header with the git commit of SOURCE ("unknown" without git, as in a copy without .git).
# Runs on every build of its target and rewrites the header only when the commit changed, so only the
# files that include it are rebuilt. Used by the society export (game/societyexport.cpp).
execute_process(COMMAND git -C "${SOURCE}" rev-parse HEAD
                OUTPUT_VARIABLE commit RESULT_VARIABLE status
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT status EQUAL 0 OR NOT commit MATCHES "^[0-9a-f]+$")
  set(commit "unknown")
endif()
set(text "#pragma once\n#define OPENGOTHIC_ENGINE_COMMIT \"${commit}\"\n")
set(old "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUTPUT}" "${text}")
endif()
