set(MONGOOSE_PUBLIC_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${HQ_CONFIG_DIR}
)

file(GLOB MONGOOSE_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.c
)

if(CONFIG_HQ_PLATFORM_POSIX)
  set(MONGOOSE_PLATFORM_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/posix)
  set(MONGOOSE_OSAL_PLATFORM_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../osal/posix)
elseif(CONFIG_HQ_PLATFORM_ESP OR ESP_PLATFORM)
  set(MONGOOSE_PLATFORM_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/esp)
  set(MONGOOSE_OSAL_PLATFORM_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../osal/esp)
endif()

if(ESP_PLATFORM)
  idf_component_register(SRCS ${MONGOOSE_COMMON_SOURCES}
                         INCLUDE_DIRS ${MONGOOSE_PUBLIC_INCLUDES} ${MONGOOSE_PLATFORM_INCLUDES}
                         REQUIRES osal esp_partition vfs app_update esp_timer mbedtls)
else()
  add_library(hq_mongoose STATIC ${MONGOOSE_COMMON_SOURCES})
  target_include_directories(hq_mongoose
    PUBLIC ${MONGOOSE_PUBLIC_INCLUDES}
    PRIVATE ${MONGOOSE_PLATFORM_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/../osal/include
            ${MONGOOSE_OSAL_PLATFORM_INCLUDE}
  )
  target_compile_definitions(hq_mongoose PRIVATE MG_ARCH=MG_ARCH_CUSTOM)
  target_link_libraries(hq_mongoose PUBLIC hq_osal)
endif()