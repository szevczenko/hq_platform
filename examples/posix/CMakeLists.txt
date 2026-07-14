add_executable(osal_demo
  ${CMAKE_CURRENT_LIST_DIR}/../common/osal_demo.c
)

target_include_directories(osal_demo
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/posix
  PRIVATE ${HQ_CONFIG_DIR}
)

target_link_libraries(osal_demo
  hq_osal
  pthread
)

set_target_properties(osal_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

add_executable(cmd_demo
  ${CMAKE_CURRENT_LIST_DIR}/../common/cmd_demo.c
)

target_include_directories(cmd_demo
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/cmd/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${HQ_CONFIG_DIR}
)

target_link_libraries(cmd_demo
  hq_cmd
  pthread
)

set_target_properties(cmd_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

add_executable(mqtt_demo
  ${CMAKE_CURRENT_LIST_DIR}/../common/mqtt_demo.c
)

target_include_directories(mqtt_demo
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/mongoose
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${HQ_CONFIG_DIR}
)

target_link_libraries(mqtt_demo
  hq_mongoose
  pthread
)

set_target_properties(mqtt_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

# -- thingboard_rgb_lamp example --
# Configure via cmake -D flags:
#   -DLAMP_CLIENT_ID="my_lamp"
#   -DLAMP_USERNAME="token"
#   -DLAMP_PASSWORD="pass"
#   -DLAMP_DEVICE_NAME="My Lamp"
#   -DLAMP_MQTT_URL="mqtt://broker:1883"
#   -DLAMP_TELEMETRY_PERIOD_MS=5000
#   -DLAMP_RECONNECT_DELAY_MS=3000

add_executable(thingboard_rgb_lamp
  ${CMAKE_CURRENT_LIST_DIR}/../common/thingboard_rgb_lamp.c
)

target_include_directories(thingboard_rgb_lamp
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/thingsboard/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/mongoose
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/posix
  PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
  PRIVATE ${HQ_CONFIG_DIR}
)

# Pass cmake -D variables as compile definitions (only if set)
if(DEFINED LAMP_CLIENT_ID)
  target_compile_definitions(thingboard_rgb_lamp PRIVATE CONFIG_LAMP_CLIENT_ID="${LAMP_CLIENT_ID}")
endif()
if(DEFINED LAMP_USERNAME)
  target_compile_definitions(thingboard_rgb_lamp PRIVATE CONFIG_LAMP_USERNAME="${LAMP_USERNAME}")
endif()
if(DEFINED LAMP_PASSWORD)
  target_compile_definitions(thingboard_rgb_lamp PRIVATE CONFIG_LAMP_PASSWORD="${LAMP_PASSWORD}")
endif()
if(DEFINED LAMP_DEVICE_NAME)
  target_compile_definitions(thingboard_rgb_lamp PRIVATE CONFIG_LAMP_DEVICE_NAME="${LAMP_DEVICE_NAME}")
endif()
if(DEFINED LAMP_MQTT_URL)
  target_compile_definitions(thingboard_rgb_lamp PRIVATE CONFIG_LAMP_MQTT_URL="${LAMP_MQTT_URL}")
endif()
if(DEFINED LAMP_TELEMETRY_PERIOD_MS)
  target_compile_definitions(thingboard_rgb_lamp PRIVATE CONFIG_LAMP_TELEMETRY_PERIOD_MS=${LAMP_TELEMETRY_PERIOD_MS})
endif()
if(DEFINED LAMP_RECONNECT_DELAY_MS)
  target_compile_definitions(thingboard_rgb_lamp PRIVATE CONFIG_LAMP_RECONNECT_DELAY_MS=${LAMP_RECONNECT_DELAY_MS})
endif()

target_link_libraries(thingboard_rgb_lamp
  hq_thingsboard
  hq_mongoose
  hq_cjson
  pthread
)

set_target_properties(thingboard_rgb_lamp PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)