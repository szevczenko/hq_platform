if(NOT ESP_PLATFORM)
  include(${CMAKE_CURRENT_SOURCE_DIR}/common_tests.cmake)

  # POSIX platform: unified test application
  add_executable(osal_tests
    ${HQ_COMMON_TEST_RUNNER}
    ${HQ_COMMON_OSAL_TEST_SOURCES}
  )

  target_include_directories(osal_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(osal_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  elseif(CONFIG_HQ_PLATFORM_ESP)
    target_include_directories(osal_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/esp
    )
  endif()

  target_compile_definitions(osal_tests PRIVATE OSAL_TESTS_AGGREGATE)

  target_link_libraries(osal_tests
    hq_osal
    unity
    pthread
  )

  add_test(NAME osal_tests COMMAND osal_tests)
  set_tests_properties(osal_tests PROPERTIES LABELS "unit;osal")

  message(STATUS "Tests: Building unified test application (osal_tests)")

  # ---- Wi-Fi tests (uses mock HAL instead of platform HAL) ----
  set(WIFI_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wifi)
  set(WIFI_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi)

  add_executable(wifi_tests
    ${WIFI_TEST_DIR}/wifi_tests_runner.c
    ${WIFI_TEST_DIR}/wifi_hal_mock.c
    ${WIFI_TEST_DIR}/wifi_config_test.c
    ${WIFI_TEST_DIR}/wifi_utils_test.c
    ${WIFI_TEST_DIR}/wifi_mgmt_test.c
    # Wi-Fi module sources (without platform HAL — mock replaces it)
    ${WIFI_SRC_DIR}/wifi_managment.c
    ${WIFI_SRC_DIR}/wifi_config.c
    ${WIFI_SRC_DIR}/wifi_utils.c
  )

  target_include_directories(wifi_tests
    PRIVATE ${WIFI_SRC_DIR}
    PRIVATE ${WIFI_TEST_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
    PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(wifi_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  endif()

  target_compile_definitions(wifi_tests PRIVATE OSAL_TESTS_AGGREGATE)

  target_link_libraries(wifi_tests
    hq_osal
    hq_cjson
    unity
    pthread
  )

  add_test(NAME wifi_tests COMMAND wifi_tests)
  set_tests_properties(wifi_tests PROPERTIES LABELS "unit;wifi")

  message(STATUS "Tests: Building Wi-Fi test application (wifi_tests)")

  add_executable(mqtt_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/mqtt/mqtt_config_test.c
  )

  target_include_directories(mqtt_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  target_link_libraries(mqtt_tests
    hq_mongoose
    unity
    pthread
  )
  
  add_test(NAME mqtt_tests COMMAND mqtt_tests)
  set_tests_properties(mqtt_tests PROPERTIES LABELS "unit;mqtt")

  message(STATUS "Tests: Building MQTT test application (mqtt_tests)")

  add_executable(mqtt_functional_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/mqtt/mqtt_functional_test.c
  )

  target_include_directories(mqtt_functional_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  target_link_libraries(mqtt_functional_tests
    hq_mongoose
    unity
    pthread
  )

  message(STATUS "Tests: Building MQTT functional test application (mqtt_functional_tests)")

  # ---- Mongoose process poll-thread invocation tests ----
  add_executable(mongoose_process_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/mongoose/mongoose_process_test.c
  )

  target_include_directories(mongoose_process_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(mongoose_process_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  endif()

  target_link_libraries(mongoose_process_tests
    hq_mongoose
    unity
    pthread
  )

  add_test(NAME mongoose_process_tests COMMAND mongoose_process_tests)
  set_tests_properties(mongoose_process_tests PROPERTIES LABELS "unit;mongoose")

  message(STATUS "Tests: Building Mongoose process test application (mongoose_process_tests)")

  # ---- Captive DNS server packet codec tests ----
  add_executable(captive_dns_server_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/mongoose/captive_dns_server_test.c
  )

  target_include_directories(captive_dns_server_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  target_link_libraries(captive_dns_server_tests
    hq_mongoose
    unity
    pthread
  )

  add_test(NAME captive_dns_server_tests COMMAND captive_dns_server_tests)
  set_tests_properties(captive_dns_server_tests PROPERTIES LABELS "unit;mongoose")

  message(STATUS "Tests: Building captive DNS server codec test application (captive_dns_server_tests)")

  # ---- Captive DNS UDP service lifecycle tests ----
  add_executable(captive_dns_server_lifecycle_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/mongoose/captive_dns_server_lifecycle_test.c
  )

  target_include_directories(captive_dns_server_lifecycle_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(captive_dns_server_lifecycle_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  endif()

  target_link_libraries(captive_dns_server_lifecycle_tests
    hq_mongoose
    unity
    pthread
  )

  add_test(NAME captive_dns_server_lifecycle_tests COMMAND captive_dns_server_lifecycle_tests)
  set_tests_properties(captive_dns_server_lifecycle_tests PROPERTIES LABELS "unit;mongoose")

  message(STATUS "Tests: Building captive DNS server lifecycle test application (captive_dns_server_lifecycle_tests)")

  # ---- Captive DNS UDP loopback test (POSIX only) ----
  add_executable(captive_dns_server_loopback_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/mongoose/captive_dns_server_loopback_test.c
  )

  target_include_directories(captive_dns_server_loopback_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(captive_dns_server_loopback_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  endif()

  target_link_libraries(captive_dns_server_loopback_tests
    hq_mongoose
    unity
    pthread
  )

  add_test(NAME captive_dns_server_loopback_tests COMMAND captive_dns_server_loopback_tests)
  set_tests_properties(captive_dns_server_loopback_tests PROPERTIES LABELS "unit;mongoose")

  message(STATUS "Tests: Building captive DNS server loopback test application (captive_dns_server_loopback_tests)")

  # ---- ThingsBoard client tests (uses mock MQTT) ----
  set(TB_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thingsboard)
  set(TB_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../src/thingsboard)

  add_executable(tb_tests
    ${TB_TEST_DIR}/tb_tests.c
    ${TB_TEST_DIR}/tb_test_common.c
    ${TB_TEST_DIR}/tb_tests_client.c
    ${TB_TEST_DIR}/tb_tests_telemetry.c
    ${TB_TEST_DIR}/tb_tests_attributes_rpc.c
    ${TB_TEST_DIR}/tb_tests_provision_claim_fwu.c
    ${TB_TEST_DIR}/mqtt_app_mock.c
    # ThingsBoard module sources (linked against mock, not real mqtt_app)
    ${TB_SRC_DIR}/tb_client.c
    ${TB_SRC_DIR}/tb_telemetry.c
    ${TB_SRC_DIR}/tb_attributes.c
    ${TB_SRC_DIR}/tb_rpc.c
    ${TB_SRC_DIR}/tb_provision.c
    ${TB_SRC_DIR}/tb_claim.c
    ${TB_SRC_DIR}/tb_firmware_update.c
  )

  target_include_directories(tb_tests
    PRIVATE ${TB_SRC_DIR}/include
    PRIVATE ${TB_TEST_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    PRIVATE ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(tb_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  endif()

  target_link_libraries(tb_tests
    hq_osal
    hq_cjson
    unity
    pthread
  )

  add_test(NAME tb_tests COMMAND tb_tests)
  set_tests_properties(tb_tests PROPERTIES LABELS "unit;thingsboard")

  message(STATUS "Tests: Building ThingsBoard test application (tb_tests)")

  # ---- ThingsBoard broker integration reconnect test (real mqtt_app) ----
  add_executable(tb_reconnect_integration_tests
    ${TB_TEST_DIR}/tb_reconnect_integration_test.c
    ${TB_SRC_DIR}/tb_client.c
    ${TB_SRC_DIR}/tb_telemetry.c
    ${TB_SRC_DIR}/tb_attributes.c
    ${TB_SRC_DIR}/tb_rpc.c
    ${TB_SRC_DIR}/tb_provision.c
    ${TB_SRC_DIR}/tb_claim.c
    ${TB_SRC_DIR}/tb_firmware_update.c
  )

  target_include_directories(tb_reconnect_integration_tests
    PRIVATE ${TB_SRC_DIR}/include
    PRIVATE ${TB_TEST_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    PRIVATE ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(tb_reconnect_integration_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  endif()

  target_link_libraries(tb_reconnect_integration_tests
    hq_osal
    hq_mongoose
    hq_cjson
    unity
    pthread
  )

  message(STATUS "Tests: Building ThingsBoard reconnect integration test (tb_reconnect_integration_tests)")

  # ---- ThingsBoard TLS broker integration test (real mqtt_app) ----
  add_executable(tb_tls_integration_tests
    ${TB_TEST_DIR}/tb_tls_integration_test.c
    ${TB_SRC_DIR}/tb_client.c
    ${TB_SRC_DIR}/tb_telemetry.c
    ${TB_SRC_DIR}/tb_attributes.c
    ${TB_SRC_DIR}/tb_rpc.c
    ${TB_SRC_DIR}/tb_provision.c
    ${TB_SRC_DIR}/tb_claim.c
    ${TB_SRC_DIR}/tb_firmware_update.c
  )

  target_include_directories(tb_tls_integration_tests
    PRIVATE ${TB_SRC_DIR}/include
    PRIVATE ${TB_TEST_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    PRIVATE ${HQ_CONFIG_DIR}
  )

  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(tb_tls_integration_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  endif()

  target_link_libraries(tb_tls_integration_tests
    hq_osal
    hq_mongoose
    hq_cjson
    unity
    pthread
  )

  message(STATUS "Tests: Building ThingsBoard TLS integration test (tb_tls_integration_tests)")

else()
  message(STATUS "Tests: ESP-IDF tests are built as separate projects")
endif()

add_library(hq_tests INTERFACE)
