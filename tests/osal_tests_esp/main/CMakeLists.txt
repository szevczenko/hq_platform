idf_component_register(
  SRCS
    "../../tests.c"
    "../../osal/osal_task_test.c"
    "../../osal/osal_sync_test.c"
    "../../osal/osal_queue_test.c"
    "../../osal/osal_timer_test.c"
  INCLUDE_DIRS "."
  REQUIRES osal
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE OSAL_TESTS_AGGREGATE)
