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

else()
  message(STATUS "Tests: ESP-IDF tests are built as separate projects")
endif()

add_library(hq_tests INTERFACE)
