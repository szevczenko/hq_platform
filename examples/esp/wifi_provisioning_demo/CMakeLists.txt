cmake_minimum_required(VERSION 3.16)

get_filename_component(HQ_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# TASK-131: this example builds with Wi-Fi HTTP provisioning enabled by
# default. The shared defconfig/esp.defconfig keeps provisioning disabled so
# existing ESP examples are unaffected; this example selects the dedicated
# provisioning-enabled ESP defconfig instead.
set(HQ_DEFCONFIG "${HQ_REPO_ROOT}/defconfig/esp_provisioning.defconfig")
set(EXTRA_COMPONENT_DIRS
  ${HQ_REPO_ROOT}/src/osal
  ${HQ_REPO_ROOT}/src/mongoose
  ${HQ_REPO_ROOT}/src/wifi
  ${HQ_REPO_ROOT}/src/wifi_provisioning
)
include(${HQ_REPO_ROOT}/cmake/configure.cmake)
# Load the generated CMake configuration (CONFIG_* variables) so the
# provisioning-enabled component tree and the mongoose packed filesystem are
# selected exactly like the root build system does.
include(${HQ_CONFIG_CMAKE})
include(${HQ_REPO_ROOT}/cmake/esp.cmake)
include(${HQ_REPO_ROOT}/cmake/modules.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_provisioning_demo)