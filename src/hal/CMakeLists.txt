# src/hal/CMakeLists.txt — HAL component (TASK-005)
#
# This is the single build description for the portable HAL.  The same file is
# consumed directly by ESP-IDF and by the repository's host CMake build.
# Public headers are target-independent; only one target-specific backend is
# selected, so host builds never parse ESP-IDF headers and the ESP-IDF public
# interface never exposes POSIX implementation headers.

set(HAL_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_LIST_DIR}/include
)

if(ESP_PLATFORM)
  file(GLOB HAL_PLATFORM_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/esp/*.c
  )
  set(HAL_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/esp
  )
elseif(CONFIG_HQ_PLATFORM_POSIX)
  file(GLOB HAL_PLATFORM_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/posix/*.c
  )
  set(HAL_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/posix
  )
else()
  message(FATAL_ERROR
    "HAL: unsupported build configuration. Select CONFIG_HQ_PLATFORM_POSIX "
    "for host/POSIX builds or build through ESP-IDF (ESP_PLATFORM).")
endif()

if(ESP_PLATFORM)
  # The directory name intentionally defines the ESP-IDF component name as
  # `hal`.  This is the only idf_component_register() call for this component.
  idf_component_register(
    SRCS ${HAL_PLATFORM_SOURCES}
    INCLUDE_DIRS ${HAL_PUBLIC_INCLUDES}
    PRIV_INCLUDE_DIRS ${HAL_PLATFORM_INCLUDES}
    PRIV_REQUIRES driver
  )
else()
  # Keep the repository's established host target name and architecture.
  add_library(hq_hal STATIC ${HAL_PLATFORM_SOURCES})
  target_include_directories(hq_hal
    PUBLIC ${HAL_PUBLIC_INCLUDES}
    PRIVATE ${HAL_PLATFORM_INCLUDES}
  )
endif()
