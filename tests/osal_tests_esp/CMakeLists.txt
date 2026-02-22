cmake_minimum_required(VERSION 3.16)

# Tell ESP-IDF where to find the OSAL component
set(EXTRA_COMPONENT_DIRS ../../src/osal)

# Ensure OSAL uses the ESP32 backend in this project
set(CONFIG_HQ_PLATFORM_ESP ON CACHE BOOL "Build ESP backend" FORCE)
set(CONFIG_HQ_PLATFORM_POSIX OFF CACHE BOOL "Build POSIX backend" FORCE)
set(CONFIG_OSAL_LOG_LEVEL 0 CACHE STRING "OSAL log level" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(osal_tests)
