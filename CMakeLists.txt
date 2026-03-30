cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{IDF_PATH} AND ESP_PLATFORM)
  # Tell ESP-IDF where to find components
  set(EXTRA_COMPONENT_DIRS src/osal src/mongoose src/cmd main)
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  project(hq_platform)
else()
  project(hq_platform C)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

# Generate hq_config.h using kconfiglib genconfig
if(DEFINED PYTHON AND NOT PYTHON STREQUAL "")
  set(HQ_PYTHON_EXECUTABLE "${PYTHON}")
else()
  find_program(HQ_PYTHON_EXECUTABLE NAMES python3 python REQUIRED)
endif()

if(ESP_PLATFORM)
  set(HQ_CONFIG_DIR "${CMAKE_BINARY_DIR}/config")
else()
  set(HQ_CONFIG_DIR "${CMAKE_BINARY_DIR}")
endif()

file(MAKE_DIRECTORY "${HQ_CONFIG_DIR}")
set(HQ_CONFIG_HEADER "${HQ_CONFIG_DIR}/hq_config.h")

if(ESP_PLATFORM)
  set(_hq_kconfig_config "${CMAKE_SOURCE_DIR}/sdkconfig")
elseif(HQ_DEFCONFIG)
  set(_hq_kconfig_config "${HQ_DEFCONFIG}")
else()
  message(FATAL_ERROR "HQ_DEFCONFIG not set. Pass -DHQ_DEFCONFIG=path/to/defconfig")
endif()

if(EXISTS "${_hq_kconfig_config}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
      KCONFIG_CONFIG=${_hq_kconfig_config}
      ${HQ_PYTHON_EXECUTABLE} -m genconfig
        --header-path ${HQ_CONFIG_HEADER}
        ${CMAKE_SOURCE_DIR}/Kconfig
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE _genconfig_result
    OUTPUT_VARIABLE _genconfig_stdout
    ERROR_VARIABLE _genconfig_stderr
  )
  if(NOT _genconfig_result EQUAL 0)
    message(FATAL_ERROR
      "genconfig failed for ${_hq_kconfig_config}\n"
      "stdout:\n${_genconfig_stdout}\n"
      "stderr:\n${_genconfig_stderr}\n"
      "Install kconfiglib with:\n  pip3 install --break-system-packages kconfiglib")
  endif()
else()
  message(WARNING "Config not found: ${_hq_kconfig_config} - hq_config.h will not be generated")
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
endif()
