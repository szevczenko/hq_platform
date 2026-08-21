cmake_minimum_required(VERSION 3.16)

get_filename_component(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp.defconfig")
set(EXTRA_COMPONENT_DIRS ${HQ_REPO_ROOT}/src/osal ${HQ_REPO_ROOT}/src/cmd ${HQ_REPO_ROOT}/src/mongoose ${HQ_REPO_ROOT}/src/wifi ${HQ_REPO_ROOT}/src/thingsboard)
include(${HQ_REPO_ROOT}/cmake/configure.cmake)
include(${HQ_REPO_ROOT}/cmake/esp.cmake)
include(${HQ_REPO_ROOT}/cmake/modules.cmake)

if(COMMAND littlefs_create_partition_image)
	littlefs_create_partition_image(storage ${HQ_REPO_ROOT}/cert FLASH_IN_PROJECT)
else()
	message(STATUS "littlefs_create_partition_image not available; use prebuilt storage.littlefs")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cmd_demo)