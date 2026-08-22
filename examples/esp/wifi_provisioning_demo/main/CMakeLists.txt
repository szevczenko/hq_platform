idf_component_register(
  SRCS ${CMAKE_CURRENT_LIST_DIR}/app_main.c
  INCLUDE_DIRS .
  REQUIRES wifi_provisioning osal mongoose wifi
)