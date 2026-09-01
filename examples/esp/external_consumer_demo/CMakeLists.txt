cmake_minimum_required(VERSION 3.16)

# external_consumer_demo — standalone ESP-IDF consumer of hq_platform.
#
# hq_platform is consumed as an external collection of ESP-IDF components
# (see README.md "Integrating into a downstream ESP-IDF application").  The
# repository root must NOT be listed in EXTRA_COMPONENT_DIRS — only the
# src/* component directories the application needs, including transitive
# ones, are registered here.  No hq_platform source file is copied into this
# example.

get_filename_component(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")

# ESP-IDF only scans the directories listed in EXTRA_COMPONENT_DIRS, so every
# hq_platform component the application needs — including ones pulled in
# transitively (osal via wifi/thingsboard, mongoose via osal/thingsboard) —
# must be listed explicitly.  This fixture does not consume any HAL public
# API, so src/hal/hq_hal is intentionally not registered here.
set(EXTRA_COMPONENT_DIRS
  ${HQ_REPO_ROOT}/src/osal
  ${HQ_REPO_ROOT}/src/wifi
  ${HQ_REPO_ROOT}/src/thingsboard
  ${HQ_REPO_ROOT}/src/mongoose
)

include(${HQ_REPO_ROOT}/cmake/configure.cmake)
# Load the generated CMake configuration (CONFIG_* variables) immediately
# after configure.cmake and before component discovery, so configuration-
# dependent component logic (e.g. CONFIG_WIFI_HTTP_PROVISIONING and
# CONFIG_CMD_ESP_OUTPUT_UART) is evaluated exactly like the root build system
# does.
include(${HQ_CONFIG_CMAKE})
include(${HQ_REPO_ROOT}/cmake/esp.cmake)
include(${HQ_REPO_ROOT}/cmake/modules.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(external_consumer_demo)