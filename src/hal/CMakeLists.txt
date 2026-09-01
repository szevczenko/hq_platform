# src/hal/CMakeLists.txt — shared HAL build description and host entry point
# (TASK-005).
#
# Collision-free naming: ESP-IDF derives an external component's name from the
# basename of its component directory.  Registering this `hal` parent directory
# directly through EXTRA_COMPONENT_DIRS would override the framework `hal`
# component shipped with ESP-IDF and break its internals, so the ESP-IDF entry
# point lives in src/hal/hq_hal/, whose basename supplies the collision-free
# component name `hq_hal`.  That entry point only `include()`s this file, so
# this description is the single source of truth: one source list, one include
# list and exactly one registration call; nothing is duplicated in
# src/hal/hq_hal/CMakeLists.txt.
#
# Structure-only scaffold; backend sources are wired by later tasks
# (TASK-006/TASK-008/TASK-009):
#   - host/POSIX branch: source-free INTERFACE `hq_hal` target exporting
#     src/hal/include publicly,
#   - ESP-IDF branch: source-free, config-only `hq_hal` component exporting
#     src/hal/include publicly.
# Only the public include directory is exposed.  Platform-specific backend
# directories stay private (and are not compiled yet), so host builds never
# parse ESP-IDF headers and the ESP-IDF public interface never exposes POSIX
# implementation headers.

set(HAL_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_LIST_DIR}/include
)

if(ESP_PLATFORM)
  # ESP-IDF branch.  This file is `include()`d by
  # src/hal/hq_hal/CMakeLists.txt, so the component name comes from the
  # hq_hal/ directory basename.  The scaffold is source-free/config-only: it
  # only propagates the public include directory.  The ESP backend and its
  # driver dependency are wired in TASK-008/TASK-009.
  idf_component_register(
    INCLUDE_DIRS ${HAL_PUBLIC_INCLUDES}
  )
else()
  # Host/POSIX branch.  Structure-only scaffold: `hq_hal` is an INTERFACE
  # library that exports only the public include directory.  It is source-free
  # so the scaffold configures and builds independently of the backend
  # implementation; the POSIX backend sources are wired into this target in
  # TASK-006.
  add_library(hq_hal INTERFACE)
  target_include_directories(hq_hal
    INTERFACE ${HAL_PUBLIC_INCLUDES}
  )
endif()
