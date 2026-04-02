cmake_minimum_required(VERSION 3.16)

get_filename_component(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")
set(EXTRA_COMPONENT_DIRS ${HQ_REPO_ROOT}/src/osal ${HQ_REPO_ROOT}/src/cmd)
include(${HQ_REPO_ROOT}/cmake/configure.cmake)
include(${HQ_REPO_ROOT}/cmake/esp.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cmd_demo)