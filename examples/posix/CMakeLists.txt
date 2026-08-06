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

# -- thingboard_firmware_update_demo example --
# Configure via cmake -D flags:
#   -DFW_DEMO_CLIENT_ID="my_fw_device"
#   -DFW_DEMO_USERNAME="token"
#   -DFW_DEMO_PASSWORD="pass"
#   -DFW_DEMO_DEVICE_NAME="FW Device"
#   -DFW_DEMO_MQTT_URL="mqtt://broker:1883"
#   -DFW_DEMO_CHECK_PERIOD_MS=10000
#   -DFW_DEMO_RECONNECT_DELAY_MS=3000
#   -DFW_DEMO_CHUNK_SIZE=1024

add_executable(thingboard_firmware_update_demo
  ${CMAKE_CURRENT_LIST_DIR}/../common/thingboard_firmware_update_demo.c
)

target_include_directories(thingboard_firmware_update_demo
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/thingsboard/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/mongoose
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/posix
  PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
  PRIVATE ${HQ_CONFIG_DIR}
)

if(DEFINED FW_DEMO_CLIENT_ID)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_CLIENT_ID="${FW_DEMO_CLIENT_ID}")
endif()
if(DEFINED FW_DEMO_USERNAME)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_USERNAME="${FW_DEMO_USERNAME}")
endif()
if(DEFINED FW_DEMO_PASSWORD)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_PASSWORD="${FW_DEMO_PASSWORD}")
endif()
if(DEFINED FW_DEMO_DEVICE_NAME)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_DEVICE_NAME="${FW_DEMO_DEVICE_NAME}")
endif()
if(DEFINED FW_DEMO_MQTT_URL)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_MQTT_URL="${FW_DEMO_MQTT_URL}")
endif()
if(DEFINED FW_DEMO_CHECK_PERIOD_MS)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_CHECK_PERIOD_MS=${FW_DEMO_CHECK_PERIOD_MS})
endif()
if(DEFINED FW_DEMO_RECONNECT_DELAY_MS)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_RECONNECT_DELAY_MS=${FW_DEMO_RECONNECT_DELAY_MS})
endif()
if(DEFINED FW_DEMO_CHUNK_SIZE)
  target_compile_definitions(thingboard_firmware_update_demo PRIVATE CONFIG_FW_DEMO_CHUNK_SIZE=${FW_DEMO_CHUNK_SIZE})
endif()

target_link_libraries(thingboard_firmware_update_demo
  hq_thingsboard
  hq_mongoose
  hq_cjson
  pthread
)

set_target_properties(thingboard_firmware_update_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

# -- thingboard_telemetry_attributes_demo example --
# Configure via cmake -D flags:
#   -DTA_DEMO_CLIENT_ID="tb_telem_attr_0001"
#   -DTA_DEMO_USERNAME="token"
#   -DTA_DEMO_PASSWORD="pass"
#   -DTA_DEMO_DEVICE_NAME="Telemetry Attributes Demo"
#   -DTA_DEMO_MQTT_URL="mqtt://broker:1883"
#   -DTA_DEMO_SEND_PERIOD_MS=7000
#   -DTA_DEMO_RECONNECT_DELAY_MS=3000
#   -DTA_DEMO_ENABLE_QOS_DEMO=1

add_executable(thingboard_telemetry_attributes_demo
  ${CMAKE_CURRENT_LIST_DIR}/../common/thingboard_telemetry_attributes_demo.c
)

target_include_directories(thingboard_telemetry_attributes_demo
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/thingsboard/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/mongoose
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/posix
  PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
  PRIVATE ${HQ_CONFIG_DIR}
)

if(DEFINED TA_DEMO_CLIENT_ID)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_CLIENT_ID="${TA_DEMO_CLIENT_ID}")
endif()
if(DEFINED TA_DEMO_USERNAME)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_USERNAME="${TA_DEMO_USERNAME}")
endif()
if(DEFINED TA_DEMO_PASSWORD)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_PASSWORD="${TA_DEMO_PASSWORD}")
endif()
if(DEFINED TA_DEMO_DEVICE_NAME)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_DEVICE_NAME="${TA_DEMO_DEVICE_NAME}")
endif()
if(DEFINED TA_DEMO_MQTT_URL)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_MQTT_URL="${TA_DEMO_MQTT_URL}")
endif()
if(DEFINED TA_DEMO_SEND_PERIOD_MS)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_SEND_PERIOD_MS=${TA_DEMO_SEND_PERIOD_MS})
endif()
if(DEFINED TA_DEMO_RECONNECT_DELAY_MS)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_RECONNECT_DELAY_MS=${TA_DEMO_RECONNECT_DELAY_MS})
endif()
if(DEFINED TA_DEMO_ENABLE_QOS_DEMO)
  target_compile_definitions(thingboard_telemetry_attributes_demo PRIVATE CONFIG_TA_DEMO_ENABLE_QOS_DEMO=${TA_DEMO_ENABLE_QOS_DEMO})
endif()

target_link_libraries(thingboard_telemetry_attributes_demo
  hq_thingsboard
  hq_mongoose
  hq_cjson
  pthread
)

set_target_properties(thingboard_telemetry_attributes_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

# -- thingboard_attributes_demo example --
# Configure via cmake -D flags:
#   -DATTR_DEMO_CLIENT_ID="tb_attr_demo_0001"
#   -DATTR_DEMO_USERNAME="token"
#   -DATTR_DEMO_PASSWORD="pass"
#   -DATTR_DEMO_DEVICE_NAME="Attributes Demo"
#   -DATTR_DEMO_MQTT_URL="mqtt://broker:1883"
#   -DATTR_DEMO_REQUEST_PERIOD_MS=10000
#   -DATTR_DEMO_RECONNECT_DELAY_MS=3000
#   -DATTR_DEMO_ENABLE_KEY_SUBS=1

add_executable(thingboard_attributes_demo
  ${CMAKE_CURRENT_LIST_DIR}/../common/thingboard_attributes_demo.c
)

target_include_directories(thingboard_attributes_demo
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/thingsboard/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/mongoose
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/posix
  PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
  PRIVATE ${HQ_CONFIG_DIR}
)

if(DEFINED ATTR_DEMO_CLIENT_ID)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_CLIENT_ID="${ATTR_DEMO_CLIENT_ID}")
endif()
if(DEFINED ATTR_DEMO_USERNAME)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_USERNAME="${ATTR_DEMO_USERNAME}")
endif()
if(DEFINED ATTR_DEMO_PASSWORD)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_PASSWORD="${ATTR_DEMO_PASSWORD}")
endif()
if(DEFINED ATTR_DEMO_DEVICE_NAME)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_DEVICE_NAME="${ATTR_DEMO_DEVICE_NAME}")
endif()
if(DEFINED ATTR_DEMO_MQTT_URL)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_MQTT_URL="${ATTR_DEMO_MQTT_URL}")
endif()
if(DEFINED ATTR_DEMO_REQUEST_PERIOD_MS)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_REQUEST_PERIOD_MS=${ATTR_DEMO_REQUEST_PERIOD_MS})
endif()
if(DEFINED ATTR_DEMO_RECONNECT_DELAY_MS)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_RECONNECT_DELAY_MS=${ATTR_DEMO_RECONNECT_DELAY_MS})
endif()
if(DEFINED ATTR_DEMO_ENABLE_KEY_SUBS)
  target_compile_definitions(thingboard_attributes_demo PRIVATE CONFIG_ATTR_DEMO_ENABLE_KEY_SUBS=${ATTR_DEMO_ENABLE_KEY_SUBS})
endif()

target_link_libraries(thingboard_attributes_demo
  hq_thingsboard
  hq_mongoose
  hq_cjson
  pthread
)

set_target_properties(thingboard_attributes_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

# -- thingboard_rpc_demo example --
# Configure via cmake -D flags:
#   -DRPC_DEMO_CLIENT_ID="tb_rpc_demo_0001"
#   -DRPC_DEMO_USERNAME="token"
#   -DRPC_DEMO_PASSWORD="pass"
#   -DRPC_DEMO_DEVICE_NAME="RPC Demo"
#   -DRPC_DEMO_MQTT_URL="mqtt://broker:1883"
#   -DRPC_DEMO_REQUEST_PERIOD_MS=15000
#   -DRPC_DEMO_REQUEST_TIMEOUT_MS=5000
#   -DRPC_DEMO_RECONNECT_DELAY_MS=3000

add_executable(thingboard_rpc_demo
  ${CMAKE_CURRENT_LIST_DIR}/../common/thingboard_rpc_demo.c
)

target_include_directories(thingboard_rpc_demo
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/thingsboard/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/mongoose
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src/osal/posix
  PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
  PRIVATE ${HQ_CONFIG_DIR}
)

if(DEFINED RPC_DEMO_CLIENT_ID)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_CLIENT_ID="${RPC_DEMO_CLIENT_ID}")
endif()
if(DEFINED RPC_DEMO_USERNAME)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_USERNAME="${RPC_DEMO_USERNAME}")
endif()
if(DEFINED RPC_DEMO_PASSWORD)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_PASSWORD="${RPC_DEMO_PASSWORD}")
endif()
if(DEFINED RPC_DEMO_DEVICE_NAME)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_DEVICE_NAME="${RPC_DEMO_DEVICE_NAME}")
endif()
if(DEFINED RPC_DEMO_MQTT_URL)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_MQTT_URL="${RPC_DEMO_MQTT_URL}")
endif()
if(DEFINED RPC_DEMO_REQUEST_PERIOD_MS)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_REQUEST_PERIOD_MS=${RPC_DEMO_REQUEST_PERIOD_MS})
endif()
if(DEFINED RPC_DEMO_REQUEST_TIMEOUT_MS)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_REQUEST_TIMEOUT_MS=${RPC_DEMO_REQUEST_TIMEOUT_MS})
endif()
if(DEFINED RPC_DEMO_RECONNECT_DELAY_MS)
  target_compile_definitions(thingboard_rpc_demo PRIVATE CONFIG_RPC_DEMO_RECONNECT_DELAY_MS=${RPC_DEMO_RECONNECT_DELAY_MS})
endif()

target_link_libraries(thingboard_rpc_demo
  hq_thingsboard
  hq_mongoose
  hq_cjson
  pthread
)

set_target_properties(thingboard_rpc_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)