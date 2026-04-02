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