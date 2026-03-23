set(OSAL_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${HQ_CONFIG_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/external
  ${CMAKE_CURRENT_SOURCE_DIR}/external/littlefs
)

file(GLOB OSAL_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/common/*.c
)

if(CONFIG_HQ_PLATFORM_POSIX)
  file(GLOB OSAL_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/*.c
  )
  set(OSAL_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/posix
    ${CMAKE_CURRENT_SOURCE_DIR}/external/littlefs/bd
  )
  set(OSAL_LITTLEFS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/external/littlefs/lfs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/external/littlefs/lfs_util.c
    ${CMAKE_CURRENT_SOURCE_DIR}/external/littlefs/bd/lfs_filebd.c
  )
elseif(CONFIG_HQ_PLATFORM_ESP OR ESP_PLATFORM)
  file(GLOB OSAL_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/*.c
  )
  file(GLOB OSAL_ESP_LITTLEFS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/littlefs_impl/*.c
  )
  set(OSAL_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/esp
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/littlefs_impl
  )
  set(OSAL_LITTLEFS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/external/littlefs/lfs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/external/littlefs/lfs_util.c
    ${OSAL_ESP_LITTLEFS_SOURCES}
  )
endif()

set(OSAL_ALL_SOURCES ${OSAL_COMMON_SOURCES} ${OSAL_PLATFORM_SOURCES} ${OSAL_LITTLEFS_SOURCES})

# Set default log level if not configured
# 0=DEBUG (all), 1=INFO, 2=WARNING, 3=ERROR (only errors)
if(NOT DEFINED CONFIG_OSAL_LOG_LEVEL OR CONFIG_OSAL_LOG_LEVEL STREQUAL "")
  set(CONFIG_OSAL_LOG_LEVEL 0)  # Enable all log levels by default
endif()

if(ESP_PLATFORM)
  idf_component_register(SRCS ${OSAL_ALL_SOURCES}
                         INCLUDE_DIRS ${OSAL_PUBLIC_INCLUDES} ${OSAL_PLATFORM_INCLUDES}
                         REQUIRES esp_partition vfs spi_flash)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE LFS_CONFIG=lfs_config.h)
else()
  add_library(hq_osal STATIC ${OSAL_ALL_SOURCES})
  target_include_directories(hq_osal
    PUBLIC ${OSAL_PUBLIC_INCLUDES}
    PRIVATE ${OSAL_PLATFORM_INCLUDES}
  )
  target_compile_definitions(hq_osal PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL})
endif()
