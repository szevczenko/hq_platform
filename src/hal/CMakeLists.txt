# src/hal/CMakeLists.txt — shared HAL build description and host entry point
# (TASK-005, extended by TASK-006).
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
# Platform branches:
#   - host/POSIX branch: `hq_hal` is a STATIC library that compiles exactly
#     the POSIX GPIO and PWM backend sources (src/hal/posix/*.c) and exports
#     src/hal/include publicly.  The POSIX backend directory is exposed only
#     privately (it contains the host/test-only inspection header
#     hal_posix_inspect.h, which must never become part of the public
#     interface); tests reach it directly through their own include path.
#   - ESP-IDF branch: `hq_hal` component compiling the ESP-IDF GPIO backend
#     (src/hal/esp/hal_gpio_esp.c; TASK-008) and exporting src/hal/include
#     publicly.  It intentionally does not reference any POSIX source or
#     header; the ESP-IDF GPIO driver is an implementation-private
#     dependency.  TASK-009 adds the PWM backend source.
# Only the public include directory is ever exposed publicly, so host builds
# never parse ESP-IDF headers and the ESP-IDF public interface never exposes
# POSIX implementation headers.

set(HAL_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_LIST_DIR}/include
)

if(ESP_PLATFORM)
  # ESP-IDF branch.  This file is `include()`d by
  # src/hal/hq_hal/CMakeLists.txt, so the component name comes from the
  # hq_hal/ directory basename.  The component compiles exactly the ESP-IDF
  # GPIO backend (src/hal/esp/hal_gpio_esp.c) and exports only the public
  # include directory; it never compiles any POSIX source.  The dependency
  # on the ESP-IDF GPIO driver (`driver`) is implementation-private
  # (PRIV_REQUIRES): it is required to compile the backend source but must
  # never leak into the public interface, which stays pure C99.  TASK-009
  # wires the PWM backend source into this branch.
  idf_component_register(
    SRCS ${CMAKE_CURRENT_LIST_DIR}/esp/hal_gpio_esp.c
    INCLUDE_DIRS ${HAL_PUBLIC_INCLUDES}
    PRIV_REQUIRES driver
  )
else()
  # Host/POSIX branch.  `hq_hal` is a STATIC library that compiles exactly the
  # POSIX GPIO and PWM backend sources (no other HAL source is included) and
  # exports only the public include directory.  The POSIX backend directory is
  # kept PRIVATE: it provides the host/test-only inspection header
  # (hal_posix_inspect.h) used by the backend sources and by unit tests, and
  # must not leak into the public interface.
  add_library(hq_hal STATIC
    ${CMAKE_CURRENT_LIST_DIR}/posix/hal_gpio_posix.c
    ${CMAKE_CURRENT_LIST_DIR}/posix/hal_pwm_posix.c
  )
  target_include_directories(hq_hal
    PUBLIC ${HAL_PUBLIC_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/posix
  )
endif()
