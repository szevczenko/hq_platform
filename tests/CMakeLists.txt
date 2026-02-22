if(NOT ESP_PLATFORM)
  # POSIX platform tests
  add_executable(osal_task_test osal/osal_task_test.c)
  add_executable(osal_sync_test osal/osal_sync_test.c)
  add_executable(osal_queue_test osal/osal_queue_test.c)
  add_executable(osal_timer_test osal/osal_timer_test.c)
  add_executable(osal_tests tests.c
    osal/osal_task_test.c
    osal/osal_sync_test.c
    osal/osal_queue_test.c
    osal/osal_timer_test.c
  )
  
  target_include_directories(osal_task_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${CMAKE_BINARY_DIR}
  )

  target_include_directories(osal_sync_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${CMAKE_BINARY_DIR}
  )

  target_include_directories(osal_queue_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${CMAKE_BINARY_DIR}
  )

  target_include_directories(osal_timer_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${CMAKE_BINARY_DIR}
  )

  target_include_directories(osal_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${CMAKE_BINARY_DIR}
  )
  
  if(CONFIG_HQ_PLATFORM_POSIX)
    target_include_directories(osal_task_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
    target_include_directories(osal_sync_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
    target_include_directories(osal_queue_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
    target_include_directories(osal_timer_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
    target_include_directories(osal_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix
    )
  elseif(CONFIG_HQ_PLATFORM_ESP)
    target_include_directories(osal_task_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/esp
    )
    target_include_directories(osal_sync_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/esp
    )
    target_include_directories(osal_queue_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/esp
    )
    target_include_directories(osal_timer_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/esp
    )
    target_include_directories(osal_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/esp
    )
  endif()
  
  target_compile_definitions(osal_task_test
    PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL}
  )

  target_compile_definitions(osal_sync_test
    PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL}
  )

  target_compile_definitions(osal_queue_test
    PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL}
  )

  target_compile_definitions(osal_timer_test
    PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL}
  )

  target_compile_definitions(osal_tests
    PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL}
    PRIVATE OSAL_TESTS_AGGREGATE
  )
  
  target_link_libraries(osal_task_test
    hq_osal
    pthread
  )

  target_link_libraries(osal_sync_test
    hq_osal
    pthread
  )

  target_link_libraries(osal_queue_test
    hq_osal
    pthread
  )

  target_link_libraries(osal_timer_test
    hq_osal
    pthread
  )

  target_link_libraries(osal_tests
    hq_osal
    pthread
  )
  
  message(STATUS "Tests: OSAL task test will be built (osal_task_test)")
  message(STATUS "Tests: OSAL sync test will be built (osal_sync_test)")
  message(STATUS "Tests: OSAL queue test will be built (osal_queue_test)")
  message(STATUS "Tests: OSAL timer test will be built (osal_timer_test)")
  message(STATUS "Tests: OSAL aggregated tests will be built (osal_tests)")
else()
  message(STATUS "Tests: ESP-IDF tests are built as separate projects")
endif()

add_library(hq_tests INTERFACE)
