# src/hal/hq_hal/CMakeLists.txt — thin ESP-IDF component entry point (TASK-005)
#
# ESP-IDF derives an external component's name from the basename of its
# component directory.  This directory is intentionally named `hq_hal`: the
# parent `src/hal` directory must not be registered directly through
# EXTRA_COMPONENT_DIRS because that would override the framework `hal`
# component shipped with ESP-IDF and break its internals.
#
# All build logic lives in the shared parent build description
# (../CMakeLists.txt); this file only delegates to it, so source lists,
# include lists and the single idf_component_register() call are never
# duplicated between the two files.
include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)