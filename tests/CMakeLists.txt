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
    ${WIFI_TEST_DIR}/wifi_hal_mock_test.c
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

  # ---- Captive DNS server tests (codec + UDP service) ----
  # The captive DNS responder is part of the Wi-Fi HTTP provisioning feature
  # and is compiled into the Mongoose library only when provisioning is
  # enabled, so its tests follow the same gate.
  if(CONFIG_WIFI_HTTP_PROVISIONING)
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
  endif() # CONFIG_WIFI_HTTP_PROVISIONING (captive DNS tests)

  # ---- Wi-Fi HTTP provisioning application lifecycle tests ----
  # Only built when the optional provisioning component is enabled.
  if(CONFIG_WIFI_HTTP_PROVISIONING)
    add_executable(wifi_http_provisioning_tests
      ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning/wifi_http_provisioning_test.c
    )

    target_include_directories(wifi_http_provisioning_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi_provisioning
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_tests
      hq_wifi_provisioning
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_tests COMMAND wifi_http_provisioning_tests)
    set_tests_properties(wifi_http_provisioning_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning lifecycle test application (wifi_http_provisioning_tests)")

    # ---- Wi-Fi HTTP provisioning lifecycle robustness tests ----
    # Exercises repeated start/stop cycles, HTTP and DNS bind-failure rollback,
    # recovery after a failed start, and the guarantee that stopping
    # provisioning never deinitializes the shared Mongoose process (so MQTT
    # and ThingsBoard, which ride the same process, keep running).  Listener
    # failures are injected with real socket conflicts.  Registered as a
    # separate target so a lifecycle regression is isolated from the route
    # tests.
    add_executable(wifi_http_provisioning_lifecycle_tests
      ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning/wifi_http_provisioning_lifecycle_test.c
    )

    target_include_directories(wifi_http_provisioning_lifecycle_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi_provisioning
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_lifecycle_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_lifecycle_tests
      hq_wifi_provisioning
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_lifecycle_tests COMMAND wifi_http_provisioning_lifecycle_tests)
    set_tests_properties(wifi_http_provisioning_lifecycle_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning lifecycle robustness test application (wifi_http_provisioning_lifecycle_tests)")

    # ---- Wi-Fi HTTP provisioning callback subscription tests ----
    # Verifies the status route's typed Wi-Fi event subscriptions are taken on
    # a clean start and released on every stop, across repeated cycles and
    # bind-failure rollbacks. The test compiles the provisioning module against
    # a mock Wi-Fi management API (which counts live subscriptions) while using
    # the real Mongoose process and captive DNS service, so no WiFiMgmt source
    # is linked here (that would pull in the real driver).
    add_executable(wifi_http_provisioning_subscription_tests
      ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning/wifi_http_provisioning_subscription_test.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi_provisioning/wifi_http_provisioning.c
      ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi_provisioning/wifi_provisioning.c
    )

    target_include_directories(wifi_http_provisioning_subscription_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi_provisioning
      PRIVATE ${WIFI_SRC_DIR}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
      PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_subscription_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_subscription_tests
      hq_mongoose
      hq_osal
      hq_cjson
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_subscription_tests COMMAND wifi_http_provisioning_subscription_tests)
    set_tests_properties(wifi_http_provisioning_subscription_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning callback subscription test application (wifi_http_provisioning_subscription_tests)")

    # ---- Wi-Fi HTTP provisioning status endpoint tests (mock Wi-Fi HAL) ----
    # Built only when the optional provisioning component is enabled.  The mock
    # HAL gives deterministic control over the Wi-Fi link state (connect
    # results and injected events), so this target compiles the Wi-Fi
    # management sources directly instead of linking the platform driver.
    set(WIFI_PROVISIONING_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning)
    set(WIFI_PROVISIONING_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi_provisioning)

    add_executable(wifi_http_provisioning_status_tests
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_status_test.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_status_tests
      PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
      PRIVATE ${WIFI_SRC_DIR}
      PRIVATE ${WIFI_TEST_DIR}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
      PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_status_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_status_tests
      hq_mongoose
      hq_osal
      hq_cjson
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_status_tests COMMAND wifi_http_provisioning_status_tests)
    set_tests_properties(wifi_http_provisioning_status_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning status test application (wifi_http_provisioning_status_tests)")

    # ---- Wi-Fi HTTP provisioning scan endpoint tests (mock Wi-Fi HAL) ----
    # Exercises POST /api/v1/wifi/scans and GET /api/v1/wifi/networks against
    # the real HTTP listener, the shared Mongoose process, and the mock HAL so
    # scan start, in-flight window, completion, and failure can be driven
    # deterministically.
    add_executable(wifi_http_provisioning_scan_tests
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_scan_test.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_scan_tests
      PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
      PRIVATE ${WIFI_SRC_DIR}
      PRIVATE ${WIFI_TEST_DIR}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
      PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_scan_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_scan_tests
      hq_mongoose
      hq_osal
      hq_cjson
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_scan_tests COMMAND wifi_http_provisioning_scan_tests)
    set_tests_properties(wifi_http_provisioning_scan_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning scan test application (wifi_http_provisioning_scan_tests)")

    # ---- Wi-Fi HTTP provisioning credential endpoint tests (mock Wi-Fi HAL) ----
    # Exercises POST /api/v1/wifi/credentials over the real HTTP listener
    # against the shared Mongoose process and the mock HAL so valid and invalid
    # submissions (byte-length, body-size, Content-Type, and JSON validation)
    # can be driven deterministically over HTTP.
    add_executable(wifi_http_provisioning_credentials_tests
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_credentials_test.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_credentials_tests
      PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
      PRIVATE ${WIFI_SRC_DIR}
      PRIVATE ${WIFI_TEST_DIR}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
      PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_credentials_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_credentials_tests
      hq_mongoose
      hq_osal
      hq_cjson
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_credentials_tests COMMAND wifi_http_provisioning_credentials_tests)
    set_tests_properties(wifi_http_provisioning_credentials_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning credential test application (wifi_http_provisioning_credentials_tests)")

    # ---- Wi-Fi HTTP provisioning disconnect endpoint tests (mock Wi-Fi HAL) ----
    # Exercises DELETE /api/v1/wifi/connection over the real HTTP listener
    # against the shared Mongoose process and the mock HAL so an accepted
    # asynchronous disconnect (connected and already-disconnected), an
    # un-requestable disconnect (Wi-Fi stopped -> stable service error), and
    # method rejection are driven deterministically over HTTP. Also proves the
    # provisioning HTTP listener and lifecycle stay reachable after a station
    # disconnect and that no response leaks credential data.
    add_executable(wifi_http_provisioning_disconnect_tests
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_disconnect_test.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_disconnect_tests
      PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
      PRIVATE ${WIFI_SRC_DIR}
      PRIVATE ${WIFI_TEST_DIR}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
      PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_disconnect_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_disconnect_tests
      hq_mongoose
      hq_osal
      hq_cjson
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_disconnect_tests COMMAND wifi_http_provisioning_disconnect_tests)
    set_tests_properties(wifi_http_provisioning_disconnect_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning disconnect test application (wifi_http_provisioning_disconnect_tests)")

    # ---- Wi-Fi HTTP provisioning captive portal routing tests (mock Wi-Fi HAL) ----
    # Exercises the captive-check handling and fallback routing over the real
    # HTTP listener against the shared Mongoose process and the mock HAL:
    # Android /generate_204, Apple /hotspot-detect.html, and Windows /ncsi.txt
    # and /connecttest.txt probes all resolve to the portal; the portal root is
    # served directly; unknown browser GET routes redirect to the portal root;
    # API routes are never redirected; unsupported methods return 405 with
    # Allow: GET; and HTTPS is not intercepted (a raw TLS handshake receives no
    # portal response).
    add_executable(wifi_http_provisioning_captive_tests
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_captive_test.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_captive_tests
      PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
      PRIVATE ${WIFI_SRC_DIR}
      PRIVATE ${WIFI_TEST_DIR}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
      PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
    )

    if(CONFIG_HQ_PLATFORM_POSIX)
      target_include_directories(wifi_http_provisioning_captive_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
      )
    endif()

    target_link_libraries(wifi_http_provisioning_captive_tests
      hq_mongoose
      hq_osal
      hq_cjson
      unity
      pthread
    )

    add_test(NAME wifi_http_provisioning_captive_tests COMMAND wifi_http_provisioning_captive_tests)
    set_tests_properties(wifi_http_provisioning_captive_tests PROPERTIES LABELS "unit;wifi_provisioning")

    message(STATUS "Tests: Building Wi-Fi HTTP provisioning captive routing test application (wifi_http_provisioning_captive_tests)")

    # ---- Wi-Fi provisioning automatic fallback controller tests ----
    # Built only when CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK is enabled
    # (the controller source is compiled exclusively under that guard).
    # The controller is linked against lightweight mocks of the Wi-Fi
    # management and provisioning lifecycle APIs so the policy can be driven
    # deterministically by injecting typed events.
    if(CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK)
      add_executable(wifi_provisioning_controller_tests
        ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_controller_test.c
        ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning_controller.c
      )

      target_include_directories(wifi_provisioning_controller_tests
        PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
        PRIVATE ${WIFI_SRC_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
        PRIVATE ${HQ_CONFIG_DIR}
      )

      if(CONFIG_HQ_PLATFORM_POSIX)
        target_include_directories(wifi_provisioning_controller_tests
          PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
        )
      endif()

      target_link_libraries(wifi_provisioning_controller_tests
        unity
        pthread
      )

      add_test(NAME wifi_provisioning_controller_tests COMMAND wifi_provisioning_controller_tests)
      set_tests_properties(wifi_provisioning_controller_tests PROPERTIES LABELS "unit;wifi_provisioning")

      message(STATUS "Tests: Building Wi-Fi provisioning controller test application (wifi_provisioning_controller_tests)")

      # ---- Wi-Fi provisioning automatic fallback end-to-end flow tests ----
      # TASK-131: drives the complete provisioning flow in the documented
      # ownership order (OSAL -> Mongoose -> Wi-Fi management -> provisioning)
      # against the real provisioning application, the real Wi-Fi management
      # layer and the mock Wi-Fi HAL. Covers the host-side behaviors required
      # for the ESP provisioning example: DNS responses, credential
      # persistence (wifi_ap.json), and the STA-only transition after the
      # success grace period.
      add_executable(wifi_provisioning_fallback_flow_tests
        ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_fallback_flow_test.c
        ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
        ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
        ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning_controller.c
        ${WIFI_SRC_DIR}/wifi_managment.c
        ${WIFI_SRC_DIR}/wifi_config.c
        ${WIFI_SRC_DIR}/wifi_utils.c
        ${WIFI_TEST_DIR}/wifi_hal_mock.c
      )

      target_include_directories(wifi_provisioning_fallback_flow_tests
        PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
        PRIVATE ${WIFI_SRC_DIR}
        PRIVATE ${WIFI_TEST_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
        PRIVATE ${HQ_CONFIG_DIR}
        PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
      )

      if(CONFIG_HQ_PLATFORM_POSIX)
        target_include_directories(wifi_provisioning_fallback_flow_tests
          PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
        )
      endif()

      target_link_libraries(wifi_provisioning_fallback_flow_tests
        hq_mongoose
        hq_osal
        hq_cjson
        unity
        pthread
      )

      add_test(NAME wifi_provisioning_fallback_flow_tests COMMAND wifi_provisioning_fallback_flow_tests)
      set_tests_properties(wifi_provisioning_fallback_flow_tests PROPERTIES LABELS "unit;wifi_provisioning")

      message(STATUS "Tests: Building Wi-Fi provisioning fallback flow test application (wifi_provisioning_fallback_flow_tests)")
    endif()
  endif()

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
