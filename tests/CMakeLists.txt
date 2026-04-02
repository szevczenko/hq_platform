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
else()
  message(STATUS "Tests: ESP-IDF tests are built as separate projects")
endif()

add_library(hq_tests INTERFACE)
