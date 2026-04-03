cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{IDF_PATH} AND ESP_PLATFORM)
  # Tell ESP-IDF where to find components
  set(EXTRA_COMPONENT_DIRS src/osal src/mongoose src/cmd src/wifi main)
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  project(hq_platform)
else()
  project(hq_platform C)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules.cmake)

option(HQ_BUILD_TESTS "Build tests" OFF)
option(HQ_BUILD_EXAMPLES "Build examples" OFF)
set(HQ_DEFCONFIG "" CACHE FILEPATH "Path to defconfig file")

if(NOT ESP_PLATFORM)
  if(CONFIG_HQ_PLATFORM_POSIX AND CONFIG_HQ_PLATFORM_ESP)
    message(FATAL_ERROR "Both CONFIG_HQ_PLATFORM_POSIX and CONFIG_HQ_PLATFORM_ESP are enabled")
  endif()

  if(NOT CONFIG_HQ_PLATFORM_POSIX AND NOT CONFIG_HQ_PLATFORM_ESP)
    message(FATAL_ERROR "No platform selected: enable CONFIG_HQ_PLATFORM_POSIX or CONFIG_HQ_PLATFORM_ESP")
  endif()
endif()

if(NOT ESP_PLATFORM)
  if(CONFIG_HQ_PLATFORM_POSIX)
    include(cmake/posix.cmake)
  else()
    include(cmake/esp.cmake)
  endif()
endif()

# Generate hq_config.h from defconfig
set(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}")
if(NOT DEFINED HQ_DEFCONFIG OR HQ_DEFCONFIG STREQUAL "")
  if(ESP_PLATFORM)
    set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")
  endif()
endif()
include(${CMAKE_CURRENT_LIST_DIR}/cmake/configure.cmake)

if(ESP_PLATFORM)
  # For ESP-IDF, components are registered in their own CMakeLists.txt
  # ESP-IDF will automatically find and build them
else()
  add_subdirectory(src)

  if(HQ_BUILD_TESTS)
    add_subdirectory(tests)
  endif()

  if(HQ_BUILD_EXAMPLES)
    add_subdirectory(examples)
  endif()
endif()
