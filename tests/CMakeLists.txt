if(NOT ESP_PLATFORM)
  include(${CMAKE_CURRENT_SOURCE_DIR}/common_tests.cmake)

  # TASK-143: optional focused sanitizer build for the race regression
  # executable.  Accepted values: ""/none (default), "address" (ASan + UBSan),
  # "thread" (clang TSan data-race detector where supported).
  set(HQ_SANITIZE "" CACHE STRING "Focused sanitizer for TASK-143 race tests: none|address|thread")

  # Optional directory containing the sanitizer runtime shared libraries that
  # belongs with the active toolchain (libasan.so, libubsan.so, libtsan.so and
  # their versioned SONAME links).  Some toolchains split the sanitizer runtime
  # out of the base compiler into a separate package (Fedora: libasan, libubsan,
  # libtsan); when that package is missing the compiler's own linker scripts
  # point at absent system files and the focused build cannot link.  Pointing
  # this at a user-local copy of the matching runtime lets the focused sanitizer
  # build link and run on such hosts.  The raced executables must then be run
  # with LD_LIBRARY_PATH set to this directory so the loader finds the SONAME
  # (e.g. libasan.so.8) at run time.
  set(HQ_SANITIZE_RUNTIME_DIR "" CACHE STRING
      "Optional dir with sanitizer runtime libs (libasan.so, libubsan.so, libtsan.so + SONAME links)")

  # ---- HAL public API contract tests (TASK-004) ----
  # Header-only contract test.  The public HAL API is the normative contract
  # for the GPIO/PWM backends (TASK-005..TASK-010), which do not exist yet,
  # so this target compiles the public headers standalone on the host
  # (proving they are self-contained, valid C and free of target-specific
  # includes) and validates the documented portable types and constants.
  add_executable(hal_api_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/hal/hal_api_test.c
  )

  target_include_directories(hal_api_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/hal/include
  )

  target_compile_options(hal_api_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)

  target_link_libraries(hal_api_tests
    unity
  )

  add_test(NAME hal_api_tests COMMAND hal_api_tests)
  set_tests_properties(hal_api_tests PROPERTIES LABELS "unit;hal")

  message(STATUS "Tests: Building HAL API contract test (hal_api_tests)")

  # ---- HAL POSIX backend tests (TASK-005) ----
  # Exercises the real POSIX GPIO/PWM backends (src/hal/posix) against the
  # portable contract: argument/pin validation, the non-idempotent
  # init/write/read/deinit lifecycle, logical-value round trips for both
  # active polarities, and PWM duty-cycle range checks (including NaN/inf).
  # TASK-005 keeps the hq_hal component a source-free INTERFACE scaffold
  # (the POSIX backend is wired back into hq_hal in TASK-006), so the backend
  # sources are compiled directly into this test executable.  Linking hq_hal
  # still verifies that the host library is discovered by the POSIX build and
  # exports src/hal/include as its public include directory.
  add_executable(hal_posix_backend_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/hal/hal_posix_backend_test.c
  )

  target_include_directories(hal_posix_backend_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/hal/include
    PRIVATE ${HQ_CONFIG_DIR}
  )

  target_link_libraries(hal_posix_backend_tests
    hq_hal
    unity
  )

  add_test(NAME hal_posix_backend_tests COMMAND hal_posix_backend_tests)
  set_tests_properties(hal_posix_backend_tests PROPERTIES LABELS "unit;hal")

  message(STATUS "Tests: Building HAL POSIX backend test (hal_posix_backend_tests)")

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
    ${WIFI_TEST_DIR}/wifi_mgmt_deinit_test.c
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

  # WIFI_MGMT_TEST_OBSERVABILITY enables the test-only lifecycle snapshot API
  # (TASK-135C) inside wifi_managment.c/h.  It is defined only for this test
  # target; production builds and the other Wi-Fi test targets never define it.
  target_compile_definitions(wifi_tests PRIVATE
    OSAL_TESTS_AGGREGATE
    WIFI_MGMT_TEST_OBSERVABILITY
  )

  target_link_libraries(wifi_tests
    hq_osal
    hq_cjson
    unity
    pthread
  )

  add_test(NAME wifi_tests COMMAND wifi_tests)
  set_tests_properties(wifi_tests PROPERTIES LABELS "unit;wifi")

  message(STATUS "Tests: Building Wi-Fi test application (wifi_tests)")

  # ---- POSIX Wi-Fi HAL lifecycle regression tests ----
  # Links the REAL POSIX HAL (not the mock) to verify the callback-quiescent,
  # session-operation-barrier, repeat-safe deinit contract: deinit-before-init,
  # init/deinit/deinit, partial primitive-init unwind (clean and leaving
  # CLEANUP_REQUIRED), pthread_create/join failures with a successful deinit
  # retry, admitted operations racing deinit, and events (timer and in-flight
  # callback) racing deinit.  WIFI_HAL_POSIX_TESTING turns on the HAL's
  # test-only failure-injection hooks; production builds never define it and
  # always call the real pthread APIs.
  if(CONFIG_HQ_PLATFORM_POSIX)
    add_executable(wifi_hal_posix_tests
      ${WIFI_TEST_DIR}/wifi_hal_posix_test.c
      ${WIFI_SRC_DIR}/platforms/posix/wifi_hal_driver.c
    )

    target_include_directories(wifi_hal_posix_tests
      PRIVATE ${WIFI_SRC_DIR}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
      PRIVATE ${HQ_CONFIG_DIR}
    )

    target_compile_definitions(wifi_hal_posix_tests PRIVATE WIFI_HAL_POSIX_TESTING)

    target_link_libraries(wifi_hal_posix_tests
      hq_osal
      unity
      pthread
    )

    add_test(NAME wifi_hal_posix_tests COMMAND wifi_hal_posix_tests)
    set_tests_properties(wifi_hal_posix_tests PROPERTIES LABELS "unit;wifi")

    message(STATUS "Tests: Building POSIX Wi-Fi HAL test application (wifi_hal_posix_tests)")
  endif()

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

  # Shared paths for the Wi-Fi HTTP provisioning test targets.
  set(WIFI_PROVISIONING_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wifi_provisioning)
  set(WIFI_PROVISIONING_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../src/wifi_provisioning)

  # ---- Wi-Fi HTTP provisioning application lifecycle tests ----
  # Only built when the optional provisioning component is enabled.
  # TASK-142: each integration-style target compiles the shared test fixture
  # so it installs its own unique temporary littlefs image and tears down in
  # reverse ownership order (provisioning -> Wi-Fi -> Mongoose -> filesystem).
  if(CONFIG_WIFI_HTTP_PROVISIONING)
    add_executable(wifi_http_provisioning_tests
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_test.c
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_test_fixture.c
    )

    target_include_directories(wifi_http_provisioning_tests
      PRIVATE ${WIFI_PROVISIONING_TEST_DIR}
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
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_lifecycle_test.c
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_test_fixture.c
    )

    target_include_directories(wifi_http_provisioning_lifecycle_tests
      PRIVATE ${WIFI_PROVISIONING_TEST_DIR}
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

    add_executable(wifi_http_provisioning_status_tests
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_http_provisioning_status_test.c
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_test_fixture.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_status_tests
      PRIVATE ${WIFI_PROVISIONING_TEST_DIR}
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

    # Mock-based targets reseed the mock HAL through the shared fixture.
    target_compile_definitions(wifi_http_provisioning_status_tests PRIVATE
      WIFI_PROVISIONING_TEST_FIXTURE_MOCK_HAL
    )

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
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_test_fixture.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_scan_tests
      PRIVATE ${WIFI_PROVISIONING_TEST_DIR}
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

    # Mock-based targets reseed the mock HAL through the shared fixture.
    target_compile_definitions(wifi_http_provisioning_scan_tests PRIVATE
      WIFI_PROVISIONING_TEST_FIXTURE_MOCK_HAL
    )

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
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_test_fixture.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_credentials_tests
      PRIVATE ${WIFI_PROVISIONING_TEST_DIR}
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

    # Mock-based targets reseed the mock HAL through the shared fixture.
    target_compile_definitions(wifi_http_provisioning_credentials_tests PRIVATE
      WIFI_PROVISIONING_TEST_FIXTURE_MOCK_HAL
    )

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
      ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_test_fixture.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
      ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
      ${WIFI_SRC_DIR}/wifi_managment.c
      ${WIFI_SRC_DIR}/wifi_config.c
      ${WIFI_SRC_DIR}/wifi_utils.c
      ${WIFI_TEST_DIR}/wifi_hal_mock.c
    )

    target_include_directories(wifi_http_provisioning_captive_tests
      PRIVATE ${WIFI_PROVISIONING_TEST_DIR}
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

    # Mock-based targets reseed the mock HAL through the shared fixture.
    target_compile_definitions(wifi_http_provisioning_captive_tests PRIVATE
      WIFI_PROVISIONING_TEST_FIXTURE_MOCK_HAL
    )

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

      target_compile_definitions(wifi_provisioning_fallback_flow_tests
        PRIVATE WIFI_PROVISIONING_TEST_OBSERVABILITY=1
      )

      add_test(NAME wifi_provisioning_fallback_flow_tests COMMAND wifi_provisioning_fallback_flow_tests)
      set_tests_properties(wifi_provisioning_fallback_flow_tests PROPERTIES LABELS "unit;wifi_provisioning")

      message(STATUS "Tests: Building Wi-Fi provisioning fallback flow test application (wifi_provisioning_fallback_flow_tests)")
# ---- Wi-Fi provisioning deterministic race regression tests (TASK-143) ----
      # Barrier-driven reproduction of the former pre-fix ordering races: delayed
      # HAL init (early GOT_IP must be dropped until readiness), concurrent
      # Mongoose invocations, grace expiry racing a disconnect + controller
      # deinit, and teardown while a provisioning listener operation is pending.
      add_executable(wifi_provisioning_race_tests
        ${WIFI_PROVISIONING_TEST_DIR}/wifi_provisioning_race_test.c
        ${WIFI_PROVISIONING_SRC_DIR}/wifi_http_provisioning.c
        ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning.c
        ${WIFI_PROVISIONING_SRC_DIR}/wifi_provisioning_controller.c
        ${WIFI_SRC_DIR}/wifi_managment.c
        ${WIFI_SRC_DIR}/wifi_config.c
        ${WIFI_SRC_DIR}/wifi_utils.c
        ${WIFI_TEST_DIR}/wifi_hal_mock.c
      )

      target_include_directories(wifi_provisioning_race_tests
        PRIVATE ${WIFI_PROVISIONING_SRC_DIR}
        PRIVATE ${WIFI_SRC_DIR}
        PRIVATE ${WIFI_TEST_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/mongoose
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
        PRIVATE ${HQ_CONFIG_DIR}
        PRIVATE ${HQ_CJSON_INCLUDE_DIRS}
      )

      if(CONFIG_HQ_PLATFORM_POSIX)
        target_include_directories(wifi_provisioning_race_tests
          PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
        )
      endif()

      target_link_libraries(wifi_provisioning_race_tests
        hq_mongoose
        hq_osal
        hq_cjson
        unity
        pthread
      )

      target_compile_definitions(wifi_provisioning_race_tests
        PRIVATE WIFI_PROVISIONING_TEST_OBSERVABILITY=1
      )

      # Optional focused sanitizer build (TASK-143).  HQ_SANITIZE=none is the
      # default and adds no flags.  "address" enables AddressSanitizer +
      # UndefinedBehaviorSanitizer (GCC/clang); "thread" enables the clang-only
      # ThreadSanitizer data-race detector where the compiler supports it.
      if(HQ_SANITIZE MATCHES "address|undefined")
        target_compile_options(wifi_provisioning_race_tests PRIVATE
          "-fsanitize=address,undefined" "-fno-omit-frame-pointer"
        )
        target_link_options(wifi_provisioning_race_tests PRIVATE
          "-fsanitize=address,undefined"
        )
      elseif(HQ_SANITIZE STREQUAL "thread")
        target_compile_options(wifi_provisioning_race_tests PRIVATE "-fsanitize=thread")
        target_link_options(wifi_provisioning_race_tests PRIVATE "-fsanitize=thread")
      endif()
      # When the toolchain keeps the sanitizer runtime in a separate package/
      # location (e.g. Fedora libasan/libubsan/libtsan are not installed), point
      # the link at a user-local copy so `-lasan`/`-lubsan`/`-ltsan` resolve to a
      # real shared object instead of the compiler's absent default.
      if(HQ_SANITIZE_RUNTIME_DIR)
        target_link_options(wifi_provisioning_race_tests PRIVATE
          "-L${HQ_SANITIZE_RUNTIME_DIR}"
        )
      endif()

      add_test(NAME wifi_provisioning_race_tests COMMAND wifi_provisioning_race_tests)
      set_tests_properties(wifi_provisioning_race_tests PROPERTIES LABELS "unit;wifi_provisioning")

      message(STATUS "Tests: Building Wi-Fi provisioning race regression test application (wifi_provisioning_race_tests)")
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
