add_library(hq_examples INTERFACE)
if(BUILD_EXAMPLES OR HQ_BUILD_EXAMPLES)
  add_executable(osal_demo
    osal_demo.c
  )
  
  if(CONFIG_HQ_PLATFORM_POSIX)
    set(_PLATFORM_IMPL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/posix)
  elseif(CONFIG_HQ_PLATFORM_ESP)
    set(_PLATFORM_IMPL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/esp)
  endif()
  
  target_include_directories(osal_demo
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/osal/include
    PRIVATE ${_PLATFORM_IMPL_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}
  )
  
  target_compile_definitions(osal_demo
    PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL}
  )
  
  target_link_libraries(osal_demo
    hq_osal
    pthread
  )
  
  message(STATUS "Examples: OSAL demo application will be built")
else()
  message(STATUS "Examples: Skipped (enable with -DHQ_BUILD_EXAMPLES=ON)")
endif()