cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{IDF_PATH} AND ESP_PLATFORM)
  # Tell ESP-IDF where to find components.  Each entry points directly to the
  # component directory so ESP-IDF derives the required component name.
  # src/hal/hq_hal (not src/hal) is registered: ESP-IDF derives the component
  # name from the directory basename, so registering src/hal directly would
  # override the framework `hal` component shipped with ESP-IDF.
  set(EXTRA_COMPONENT_DIRS main src/osal src/hal/hq_hal src/mongoose src/cmd src/wifi src/thingsboard src/wifi_provisioning)
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  project(hq_platform)
else()
  project(hq_platform C)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(HQ_BUILD_TESTS "Build tests" OFF)
option(HQ_BUILD_EXAMPLES "Build examples" OFF)
option(HQ_BUILD_TOOLS "Build host tools" OFF)
set(HQ_DEFCONFIG "" CACHE FILEPATH "Path to defconfig file")
set(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}")

if(NOT DEFINED HQ_DEFCONFIG OR HQ_DEFCONFIG STREQUAL "")
  if(ESP_PLATFORM)
    message(WARNING "HQ_DEFCONFIG is not defined. Using default defconfig for ESP platform.")
    set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")
  endif()
endif()

# Generate and load project configuration before configuration-dependent logic.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/configure.cmake)
include(${HQ_CONFIG_CMAKE})

if(NOT ESP_PLATFORM)
  if(CONFIG_HQ_PLATFORM_POSIX AND CONFIG_HQ_PLATFORM_ESP)
    message(FATAL_ERROR "Both CONFIG_HQ_PLATFORM_POSIX and CONFIG_HQ_PLATFORM_ESP are enabled")
  endif()

  if(NOT CONFIG_HQ_PLATFORM_POSIX AND NOT CONFIG_HQ_PLATFORM_ESP)
    message(FATAL_ERROR "No platform selected: enable CONFIG_HQ_PLATFORM_POSIX or CONFIG_HQ_PLATFORM_ESP")
  endif()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules.cmake)

if(HQ_BUILD_TESTS AND NOT ESP_PLATFORM)
  enable_testing()

  set(HQ_UNITY_SUBMODULE_DIR "${CMAKE_CURRENT_LIST_DIR}/tests/unity")
  if(NOT EXISTS "${HQ_UNITY_SUBMODULE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "Unity submodule not found at tests/unity. Run: git submodule update --init --recursive"
    )
  endif()

  add_subdirectory(${HQ_UNITY_SUBMODULE_DIR} ${CMAKE_BINARY_DIR}/unity)
endif()

if(NOT ESP_PLATFORM)
  if(CONFIG_HQ_PLATFORM_POSIX)
    include(cmake/posix.cmake)
  else()
    include(cmake/esp.cmake)
  endif()
endif()

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

  if(HQ_BUILD_TOOLS)
    add_subdirectory(tools)
  endif()
endif()
