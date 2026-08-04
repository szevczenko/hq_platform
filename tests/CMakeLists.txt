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
    pthread
  )

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
    pthread
  )

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
    pthread
  )

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
    pthread
  )

  message(STATUS "Tests: Building MQTT functional test application (mqtt_functional_tests)")

  # ---- ThingsBoard client tests (uses mock MQTT) ----
  set(TB_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thingsboard)
  set(TB_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../src/thingsboard)

  add_executable(tb_tests
    ${TB_TEST_DIR}/tb_tests.c
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
    pthread
  )

  message(STATUS "Tests: Building ThingsBoard test application (tb_tests)")

else()
  message(STATUS "Tests: ESP-IDF tests are built as separate projects")
endif()

add_library(hq_tests INTERFACE)
