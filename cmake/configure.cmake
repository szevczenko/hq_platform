#
# configure.cmake — Generate C and CMake configuration from Kconfig + a defconfig file
#
# Required input (must be set before including this file):
#   HQ_DEFCONFIG    — absolute path to the defconfig file
#   HQ_REPO_ROOT    — absolute path to the hq_platform repo root
#
# Outputs:
#   HQ_CONFIG_DIR   — directory containing the generated header
#   HQ_CONFIG_HEADER — full path to hq_config.h
#   HQ_CONFIG_CMAKE — full path to the generated CMake configuration fragment
#

if(NOT DEFINED HQ_DEFCONFIG OR HQ_DEFCONFIG STREQUAL "")
  message(FATAL_ERROR
    "HQ_DEFCONFIG is not set. Set it to the path of your defconfig file before "
    "including configure.cmake.\n"
    "  Example: set(HQ_DEFCONFIG \${HQ_REPO_ROOT}/defconfig/esp.defconfig)")
endif()

if(NOT DEFINED HQ_REPO_ROOT OR HQ_REPO_ROOT STREQUAL "")
  message(FATAL_ERROR
    "HQ_REPO_ROOT is not set. Set it to the hq_platform repo root before "
    "including configure.cmake.")
endif()

# Find Python
if(DEFINED PYTHON AND NOT PYTHON STREQUAL "")
  set(_hq_python "${PYTHON}")
else()
  find_program(_hq_python NAMES python3 python REQUIRED)
endif()

# Output directory
if(NOT DEFINED HQ_CONFIG_DIR)
  if(ESP_PLATFORM)
    set(HQ_CONFIG_DIR "${CMAKE_BINARY_DIR}/config" CACHE PATH "Generated config header directory")
  else()
    set(HQ_CONFIG_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Generated config header directory")
  endif()
endif()

file(MAKE_DIRECTORY "${HQ_CONFIG_DIR}")
set(HQ_CONFIG_HEADER "${HQ_CONFIG_DIR}/hq_config.h")
set(HQ_CONFIG_CMAKE "${HQ_CONFIG_DIR}/hq_config.cmake")

# Run genconfig
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
    KCONFIG_CONFIG=${HQ_DEFCONFIG}
    ${_hq_python} -m genconfig
      --header-path ${HQ_CONFIG_HEADER}
      ${HQ_REPO_ROOT}/Kconfig
  WORKING_DIRECTORY ${HQ_REPO_ROOT}
  RESULT_VARIABLE _genconfig_result
  OUTPUT_VARIABLE _genconfig_stdout
  ERROR_VARIABLE  _genconfig_stderr
)
if(NOT _genconfig_result EQUAL 0)
  message(FATAL_ERROR
    "genconfig failed for ${HQ_DEFCONFIG}\n"
    "stdout:\n${_genconfig_stdout}\n"
    "stderr:\n${_genconfig_stderr}\n"
      "Install kconfiglib with:\n  python3 -m pip install kconfiglib\n"
      "Consider using a virtual environment or scripts/menuconfig.sh if needed.")
endif()

execute_process(
  COMMAND ${_hq_python}
    ${HQ_REPO_ROOT}/scripts/genconfig_cmake.py
    ${HQ_REPO_ROOT}/Kconfig
    ${HQ_DEFCONFIG}
    ${HQ_CONFIG_CMAKE}
  WORKING_DIRECTORY ${HQ_REPO_ROOT}
  RESULT_VARIABLE _hq_cmake_config_result
  OUTPUT_VARIABLE _hq_cmake_config_stdout
  ERROR_VARIABLE _hq_cmake_config_stderr
)
if(NOT _hq_cmake_config_result EQUAL 0)
  message(FATAL_ERROR
    "CMake configuration generation failed for ${HQ_DEFCONFIG}\n"
    "stdout:\n${_hq_cmake_config_stdout}\n"
    "stderr:\n${_hq_cmake_config_stderr}\n")
endif()

# Clean up local variables
unset(_hq_python)
unset(_genconfig_result)
unset(_genconfig_stdout)
unset(_genconfig_stderr)
unset(_hq_cmake_config_result)
unset(_hq_cmake_config_stdout)
unset(_hq_cmake_config_stderr)
