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
                         REQUIRES osal esp_partition vfs app_update esp_timer mbedtls json)
else()
  add_library(hq_mongoose STATIC ${MONGOOSE_COMMON_SOURCES})

  find_library(MBEDTLS_LIB mbedtls REQUIRED)
  find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED)
  find_library(MBEDX509_LIB mbedx509 REQUIRED)

  target_include_directories(hq_mongoose
    PUBLIC ${MONGOOSE_PUBLIC_INCLUDES}
    PRIVATE ${MONGOOSE_PLATFORM_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/../osal/include
            ${MONGOOSE_OSAL_PLATFORM_INCLUDE}
            ${HQ_CJSON_INCLUDE_DIRS}
  )
  target_compile_definitions(hq_mongoose PRIVATE
    MG_ARCH=MG_ARCH_CUSTOM
    MG_TLS=MG_TLS_MBED
  )
  target_link_libraries(hq_mongoose
    PUBLIC hq_osal hq_cjson
    PRIVATE ${MBEDTLS_LIB} ${MBEDCRYPTO_LIB} ${MBEDX509_LIB}
  )
endif()