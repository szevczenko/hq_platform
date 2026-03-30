set(OSAL_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${HQ_CONFIG_DIR}
)

set(OSAL_LITTLEFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../third_party/littlefs)

file(GLOB OSAL_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/common/*.c
)

if(CONFIG_HQ_PLATFORM_POSIX)
  file(GLOB OSAL_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/*.c
  )
  set(OSAL_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/posix
    ${OSAL_LITTLEFS_DIR}
    ${OSAL_LITTLEFS_DIR}/bd
  )
  set(OSAL_LITTLEFS_SOURCES
    ${OSAL_LITTLEFS_DIR}/lfs.c
    ${OSAL_LITTLEFS_DIR}/lfs_util.c
    ${OSAL_LITTLEFS_DIR}/bd/lfs_filebd.c
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
    ${OSAL_LITTLEFS_DIR}
  )
  set(OSAL_LITTLEFS_SOURCES
    ${OSAL_LITTLEFS_DIR}/lfs.c
    ${OSAL_LITTLEFS_DIR}/lfs_util.c
    ${OSAL_ESP_LITTLEFS_SOURCES}
  )
endif()

set(OSAL_ALL_SOURCES ${OSAL_COMMON_SOURCES} ${OSAL_PLATFORM_SOURCES} ${OSAL_LITTLEFS_SOURCES})

if(ESP_PLATFORM)
  idf_component_register(SRCS ${OSAL_ALL_SOURCES}
                         INCLUDE_DIRS ${OSAL_PUBLIC_INCLUDES} ${OSAL_PLATFORM_INCLUDES}
                         REQUIRES esp_partition vfs spi_flash
                         KCONFIG ${COMPONENT_DIR}/esp/littlefs_impl/Kconfig)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE LFS_CONFIG=lfs_config.h)
else()
  add_library(hq_osal STATIC ${OSAL_ALL_SOURCES})
  target_include_directories(hq_osal
    PUBLIC ${OSAL_PUBLIC_INCLUDES}
    PRIVATE ${OSAL_PLATFORM_INCLUDES}
  )
endif()
