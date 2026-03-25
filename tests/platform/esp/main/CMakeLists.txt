include(${CMAKE_CURRENT_LIST_DIR}/../../../common_tests.cmake)

idf_component_register(
  SRCS
    ${HQ_COMMON_TEST_RUNNER}
    ${HQ_COMMON_OSAL_TEST_SOURCES}
  INCLUDE_DIRS "."
  REQUIRES osal
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE OSAL_TESTS_AGGREGATE)
