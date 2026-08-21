# Wi-Fi provisioning component.
#
# This component is compiled only when Wi-Fi HTTP provisioning is enabled
# (CONFIG_WIFI_HTTP_PROVISIONING). The POSIX tree adds this subdirectory
# conditionally from src/CMakeLists.txt, and the root CMakeLists.txt lists
# this directory in EXTRA_COMPONENT_DIRS for ESP-IDF discovery; the same
# config guard keeps a disabled ESP-IDF build empty.
#
# HTTP portal, captive DNS responder, and Wi-Fi behavior are added by later
# tasks; this file only provides the build scaffold.

if(CONFIG_WIFI_HTTP_PROVISIONING)
  set(WIFI_PROVISIONING_PUBLIC_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HQ_CONFIG_DIR}
  )

  set(WIFI_PROVISIONING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning.c
  )

  if(ESP_PLATFORM)
    idf_component_register(SRCS ${WIFI_PROVISIONING_SOURCES}
                           INCLUDE_DIRS ${WIFI_PROVISIONING_PUBLIC_INCLUDES}
                           REQUIRES osal mongoose wifi)
  else()
    add_library(hq_wifi_provisioning STATIC ${WIFI_PROVISIONING_SOURCES})
    target_include_directories(hq_wifi_provisioning
      PUBLIC ${WIFI_PROVISIONING_PUBLIC_INCLUDES}
    )
    target_link_libraries(hq_wifi_provisioning
      PUBLIC hq_osal hq_mongoose hq_wifi hq_cjson
    )
  endif()
endif()