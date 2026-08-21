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

# ---------------------------------------------------------------------------
# Mongoose packed filesystem generation.
#
# When Wi-Fi HTTP provisioning is enabled, the provisioning web assets under
# src/wifi_provisioning/web/ are packed by the vendored Mongoose pack utility
# (third_party/mongoose/test/pack.c) into a Mongoose packed_fs.c source.  The
# generated source is placed exclusively under the build directory and enabled
# through the MG_ENABLE_PACKED_FS macro so every target that pulls in the
# Mongoose library also links the packed asset table.
#
# POSIX:  independently built host tool + build-time regeneration custom step.
# ESP:    ESP-IDF components register their sources at configure time, so the
#         assets are packed during configuration on the build host.
# ---------------------------------------------------------------------------
if(CONFIG_WIFI_HTTP_PROVISIONING)
  set(MONGOOSE_PACK_SOURCE "${CMAKE_SOURCE_DIR}/third_party/mongoose/test/pack.c")
  set(MONGOOSE_WEB_DIR "${CMAKE_SOURCE_DIR}/src/wifi_provisioning/web")
  set(MONGOOSE_PACKED_FS_DIR "${CMAKE_BINARY_DIR}/generated/mongoose_web")
  set(MONGOOSE_PACKED_FS "${MONGOOSE_PACKED_FS_DIR}/packed_fs.c")
  set(MONGOOSE_PACK_GENERATOR "${CMAKE_SOURCE_DIR}/cmake/packed_fs_generate.cmake")
endif()

if(ESP_PLATFORM)
  if(CONFIG_WIFI_HTTP_PROVISIONING)
    # ESP-IDF components are registered during CMake configure, so pack the
    # assets up-front and register the generated source with the component.
    file(MAKE_DIRECTORY "${MONGOOSE_PACKED_FS_DIR}")
    execute_process(
      COMMAND ${CMAKE_COMMAND}
        "-DWEB_DIR=${MONGOOSE_WEB_DIR}"
        "-DOUTPUT=${MONGOOSE_PACKED_FS}"
        "-DPACK_SOURCE=${MONGOOSE_PACK_SOURCE}"
        -P "${MONGOOSE_PACK_GENERATOR}"
      RESULT_VARIABLE _MONGOOSE_PACK_ESP_RESULT
    )
    if(NOT _MONGOOSE_PACK_ESP_RESULT EQUAL 0)
      message(FATAL_ERROR "Mongoose packed filesystem generation failed")
    endif()

    # A mongoose_config override that also enables the packed filesystem is
    # generated under the build directory and searched before the platform
    # config so a provisioning-enabled ESP build compiles packed assets too.
    set(MONGOOSE_ESP_CONFIG "${CMAKE_BINARY_DIR}/config/mongoose_config.h")
    file(WRITE "${MONGOOSE_ESP_CONFIG}"
      "#define MG_ARCH MG_ARCH_ESP32\n"
      "#define MG_TLS MG_TLS_MBED\n"
      "#define MG_ENABLE_PACKED_FS 1\n")

    idf_component_register(SRCS "${MONGOOSE_PACKED_FS}" ${MONGOOSE_COMMON_SOURCES}
                           INCLUDE_DIRS "${CMAKE_BINARY_DIR}/config"
                                       ${MONGOOSE_PUBLIC_INCLUDES}
                                       ${MONGOOSE_PLATFORM_INCLUDES}
                           REQUIRES osal esp_partition vfs app_update esp_timer mbedtls json)
    unset(_MONGOOSE_PACK_ESP_RESULT)
  else()
    idf_component_register(SRCS ${MONGOOSE_COMMON_SOURCES}
                           INCLUDE_DIRS ${MONGOOSE_PUBLIC_INCLUDES} ${MONGOOSE_PLATFORM_INCLUDES}
                           REQUIRES osal esp_partition vfs app_update esp_timer mbedtls json)
  endif()
else()
  add_library(hq_mongoose STATIC ${MONGOOSE_COMMON_SOURCES})

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
    PRIVATE MbedTLS::mbedtls
  )

  if(CONFIG_WIFI_HTTP_PROVISIONING)
    # Build the vendored Mongoose pack utility as a host tool and wire up a
    # build-time custom command that regenerates packed_fs.c whenever a web
    # asset under src/wifi_provisioning/web/ changes.
    set(MONGOOSE_PACK_TOOL "${CMAKE_CURRENT_BINARY_DIR}/mongoose_pack")

    add_executable(mongoose_pack "${MONGOOSE_PACK_SOURCE}")

    file(MAKE_DIRECTORY "${MONGOOSE_PACKED_FS_DIR}")
    file(GLOB MONGOOSE_WEB_FILES CONFIGURE_DEPENDS "${MONGOOSE_WEB_DIR}/*")

    add_custom_command(
      OUTPUT "${MONGOOSE_PACKED_FS}"
      COMMAND ${CMAKE_COMMAND}
        "-DPACK_TOOL=${MONGOOSE_PACK_TOOL}"
        "-DWEB_DIR=${MONGOOSE_WEB_DIR}"
        "-DOUTPUT=${MONGOOSE_PACKED_FS}"
        -P "${MONGOOSE_PACK_GENERATOR}"
      WORKING_DIRECTORY "${MONGOOSE_WEB_DIR}"
      DEPENDS ${MONGOOSE_WEB_FILES}
      COMMENT "Generating Mongoose packed filesystem (packed_fs.c)"
    )
    add_custom_target(hq_mongoose_packed_fs_generate DEPENDS "${MONGOOSE_PACKED_FS}")
    add_dependencies(hq_mongoose_packed_fs_generate mongoose_pack)
    add_dependencies(hq_mongoose hq_mongoose_packed_fs_generate)

    # Compile the generated assets into the Mongoose library and enable the
    # packed filesystem consistently for every consumer of hq_mongoose.
    target_sources(hq_mongoose PRIVATE "${MONGOOSE_PACKED_FS}")
    target_compile_definitions(hq_mongoose PRIVATE MG_ENABLE_PACKED_FS=1)
  endif()
endif()