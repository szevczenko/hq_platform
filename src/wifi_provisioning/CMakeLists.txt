# Wi-Fi provisioning component.
#
# POSIX:   compiled only when Wi-Fi HTTP provisioning is enabled
#          (CONFIG_WIFI_HTTP_PROVISIONING). The POSIX tree adds this
#          subdirectory conditionally from src/CMakeLists.txt; the config
#          guard below keeps a disabled POSIX build empty.
# ESP-IDF: ESP-IDF component discovery runs a component's CMakeLists in an
#          early scripting pass before any configuration is generated, so no
#          hq_platform CONFIG_* gate can guard idf_component_register().
#          Registration is therefore unconditional; the C sources gate their
#          own behavior (portal/controller) on the generated hq_config.h
#          macros, and only projects that opt this directory into
#          EXTRA_COMPONENT_DIRS (the root ESP project, this example) build it.
#
# HTTP portal, captive DNS responder, and automatic fallback controller.

set(WIFI_PROVISIONING_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${HQ_CONFIG_DIR}
)

set(WIFI_PROVISIONING_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning.c
  ${CMAKE_CURRENT_SOURCE_DIR}/wifi_http_provisioning.c
)

# The automatic fallback controller is an optional policy layer: on POSIX it
# is compiled only when CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK=y; on ESP
# it is always compiled and the C source itself is guarded by the
# corresponding hq_config.h macro.
if(CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK OR ESP_PLATFORM)
  list(APPEND WIFI_PROVISIONING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning_controller.c
  )
endif()

if(ESP_PLATFORM)
  idf_component_register(SRCS ${WIFI_PROVISIONING_SOURCES}
                         INCLUDE_DIRS ${WIFI_PROVISIONING_PUBLIC_INCLUDES}
                         REQUIRES osal mongoose wifi)
elseif(CONFIG_WIFI_HTTP_PROVISIONING)
  add_library(hq_wifi_provisioning STATIC ${WIFI_PROVISIONING_SOURCES})
  target_include_directories(hq_wifi_provisioning
    PUBLIC ${WIFI_PROVISIONING_PUBLIC_INCLUDES}
  )
  target_link_libraries(hq_wifi_provisioning
    PUBLIC hq_osal hq_mongoose hq_wifi hq_cjson
  )
endif()