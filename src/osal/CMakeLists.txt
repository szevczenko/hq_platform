set(OSAL_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${HQ_CONFIG_DIR}
)

set(OSAL_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/common/osal_error.c
  ${CMAKE_CURRENT_SOURCE_DIR}/common/osal_log.c
)

if(CONFIG_HQ_PLATFORM_POSIX)
  set(OSAL_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_task_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_bin_sem_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_count_sem_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_mutex_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_queue_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_timer_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_log_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix/osal_assert.c
  )
  set(OSAL_PLATFORM_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/posix)
elseif(CONFIG_HQ_PLATFORM_ESP)
  set(OSAL_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_task_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_bin_sem_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_count_sem_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_mutex_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_queue_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_timer_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_log_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/esp/osal_assert.c
  )
  set(OSAL_PLATFORM_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/esp)
endif()

set(OSAL_ALL_SOURCES ${OSAL_COMMON_SOURCES} ${OSAL_PLATFORM_SOURCES})

# Set default log level if not configured
# 0=DEBUG (all), 1=INFO, 2=WARNING, 3=ERROR (only errors)
if(NOT DEFINED CONFIG_OSAL_LOG_LEVEL OR CONFIG_OSAL_LOG_LEVEL STREQUAL "")
  set(CONFIG_OSAL_LOG_LEVEL 0)  # Enable all log levels by default
endif()

if(ESP_PLATFORM)
  idf_component_register(SRCS ${OSAL_ALL_SOURCES}
                         INCLUDE_DIRS ${OSAL_PUBLIC_INCLUDES} ${OSAL_PLATFORM_INCLUDES})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL})
else()
  add_library(hq_osal STATIC ${OSAL_ALL_SOURCES})
  target_include_directories(hq_osal
    PUBLIC ${OSAL_PUBLIC_INCLUDES}
    PRIVATE ${OSAL_PLATFORM_INCLUDES}
  )
  target_compile_definitions(hq_osal PRIVATE OSAL_LOG_LEVEL=${CONFIG_OSAL_LOG_LEVEL})
endif()
