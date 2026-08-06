idf_component_register(
  SRCS ${CMAKE_CURRENT_LIST_DIR}/../../../common/thingboard_attributes_demo.c ${CMAKE_CURRENT_LIST_DIR}/app_main.c
  INCLUDE_DIRS .
  REQUIRES thingsboard osal json wifi
)
