cmake_minimum_required(VERSION 3.16)

# hal_demo — standalone ESP-IDF example demonstrating the portable HAL API.
#
# hq_platform is consumed as an external collection of ESP-IDF components (see
# README.md "Integrating into a downstream ESP-IDF application").  This example
# registers exactly one component, the portable HAL, and drives GPIO/PWM
# exclusively through the public HAL API (hal_types.h, hal_gpio.h, hal_pwm.h).
# No HAL source file is copied into this example: main/CMakeLists.txt declares
# `REQUIRES hq_hal` and the component's sources are compiled in place under
# src/hal.
#
# Component naming: ESP-IDF derives an external component's name from the
# basename of its component directory, so src/hal/hq_hal registers as `hq_hal`.
# Only src/hal/hq_hal is added to EXTRA_COMPONENT_DIRS here — src/hal must
# never be registered directly, because that would override the framework
# `hal` component shipped with ESP-IDF and break its internals.  For the same
# reason the application declares `REQUIRES hq_hal`, never `REQUIRES hal`.

get_filename_component(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")

# Register the portable HAL as an external ESP-IDF component.
set(EXTRA_COMPONENT_DIRS
  ${HQ_REPO_ROOT}/src/hal/hq_hal
  ${HQ_REPO_ROOT}/src/osal
  ${HQ_REPO_ROOT}/src/mongoose
)

include(${HQ_REPO_ROOT}/cmake/configure.cmake)
# Load the generated CMake configuration (CONFIG_* variables) immediately
# after configure.cmake and before component discovery.
include(${HQ_CONFIG_CMAKE})
include(${HQ_REPO_ROOT}/cmake/esp.cmake)
include(${HQ_REPO_ROOT}/cmake/modules.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hal_demo)
