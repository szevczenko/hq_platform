#
# packed_fs_generate.cmake — Generate a Mongoose packed filesystem source.
#
# This script produces `packed_fs.c` from the web assets stored under WEB_DIR,
# using the vendored Mongoose pack utility (third_party/mongoose/test/pack.c).
#
# Required -D variables:
#   WEB_DIR   — directory containing the web assets to pack
#   OUTPUT    — full path where packed_fs.c is written (under the build dir)
#
# The pack tool may be supplied in one of two ways:
#   PACK_TOOL   — full path to an already-built Mongoose pack executable
#   PACK_SOURCE — full path to pack.c; the script builds it with a host C
#                 compiler (used by builds that generate at configure time,
#                 e.g. ESP-IDF)
#
# No generated output is written into the source tree.

if(NOT DEFINED WEB_DIR OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR
    "packed_fs_generate.cmake requires WEB_DIR and OUTPUT (-D arguments).")
endif()

get_filename_component(OUT_DIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUT_DIR}")

# Resolve the pack executable: prefer a prebuilt tool, otherwise compile the
# vendored pack.c on the host so no binary artifact is ever committed.
if(NOT DEFINED PACK_TOOL OR PACK_TOOL STREQUAL "")
  if(NOT DEFINED PACK_SOURCE OR PACK_SOURCE STREQUAL "")
    message(FATAL_ERROR
      "packed_fs_generate.cmake needs PACK_TOOL or PACK_SOURCE.")
  endif()
  find_program(PACK_HOST_COMPILER NAMES gcc cc)
  if(NOT PACK_HOST_COMPILER)
    message(FATAL_ERROR
      "No host C compiler (gcc/cc) found to build the Mongoose pack tool.")
  endif()
  set(PACK_TOOL "${OUT_DIR}/mongoose_pack_host")
  execute_process(
    COMMAND "${PACK_HOST_COMPILER}" "${PACK_SOURCE}" -o "${PACK_TOOL}"
    RESULT_VARIABLE PACK_BUILD_RESULT
  )
  if(NOT PACK_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Failed to build the Mongoose pack tool from ${PACK_SOURCE}")
  endif()
endif()

# Sort assets for deterministic output (pack.c names entries "v1", "v2", ...).
file(GLOB WEB_ASSETS "${WEB_DIR}/*")
list(SORT WEB_ASSETS)

# `pack` maps each file to "/<relative-path>" by stripping the web directory
# prefix from the full path.
set(PACK_ARGS "-s" "${WEB_DIR}/")
foreach(asset IN LISTS WEB_ASSETS)
  list(APPEND PACK_ARGS "${asset}")
endforeach()

execute_process(
  COMMAND "${PACK_TOOL}" ${PACK_ARGS}
  WORKING_DIRECTORY "${WEB_DIR}"
  OUTPUT_VARIABLE PACKED_SOURCE
  RESULT_VARIABLE PACKED_RESULT
)
if(NOT PACKED_RESULT EQUAL 0)
  message(FATAL_ERROR
    "The Mongoose pack utility failed (status ${PACKED_RESULT}) for ${PACK_TOOL}")
endif()

file(WRITE "${OUTPUT}" "${PACKED_SOURCE}")
message(STATUS "Generated Mongoose packed filesystem: ${OUTPUT}")