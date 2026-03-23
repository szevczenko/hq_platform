if(NOT ESP_PLATFORM)
  # POSIX platform: unified test application
  add_executable(osal_tests tests.c
    osal/osal_task_test.c
    osal/osal_sync_test.c
    osal/osal_queue_test.c
    osal/osal_timer_test.c
    osal/osal_file_test.c
    osal/osal_mount_test.c
  )

  target_include_directories(osal_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${CMAKE_BINARY_DIR}
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

  target_compile_definitions(osal_tests
    PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL}
    PRIVATE OSAL_TESTS_AGGREGATE
  )

  target_link_libraries(osal_tests
    hq_osal
    pthread
  )

  message(STATUS "Tests: Building unified test application (osal_tests)")
else()
  message(STATUS "Tests: ESP-IDF tests are built as separate projects")
endif()

add_library(hq_tests INTERFACE)
