set(WIFI_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${HQ_CONFIG_DIR}
)

set(WIFI_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/wifi_managment.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wifi_config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wifi_utils.c
)

if(CONFIG_HQ_PLATFORM_POSIX)
  set(WIFI_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/posix/wifi_hal_driver.c
  )
  set(WIFI_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/posix
  )
elseif(CONFIG_HQ_PLATFORM_ESP OR ESP_PLATFORM)
  set(WIFI_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/esp/wifi_hal_driver.c
  )
  set(WIFI_PLATFORM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/platforms/esp
  )
endif()

set(WIFI_ALL_SOURCES
  ${WIFI_COMMON_SOURCES}
  ${WIFI_PLATFORM_SOURCES}
)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${WIFI_ALL_SOURCES} ${HQ_CJSON_SOURCES}
                         INCLUDE_DIRS ${WIFI_PUBLIC_INCLUDES} ${HQ_CJSON_INCLUDE_DIRS}
                         PRIV_INCLUDE_DIRS ${WIFI_PLATFORM_INCLUDES}
                         REQUIRES osal)
else()
  add_library(hq_wifi STATIC ${WIFI_ALL_SOURCES})
  target_include_directories(hq_wifi
    PUBLIC ${WIFI_PUBLIC_INCLUDES}
    PRIVATE ${WIFI_PLATFORM_INCLUDES} ${HQ_CJSON_INCLUDE_DIRS}
  )
  target_link_libraries(hq_wifi PUBLIC hq_osal hq_cjson)
endif()
