cmake_minimum_required(VERSION 3.16)

# Tell ESP-IDF where to find the OSAL component
set(EXTRA_COMPONENT_DIRS ../../src/osal)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(osal_demo)
